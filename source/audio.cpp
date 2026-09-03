/**
 * @file audio.cpp
 * @brief Implementation of sceAudioOut audio mixing subsystem and AudioTrack shim.
 * @details Refer to technical documentation in Docs/audio_comments.md for details on
 *          WAV/IMA-ADPCM decoding, VoxN container parser, and Vita audio architecture.
 */

#include "audio.h"
#include "utils/logger.h"
#include "sounddefs.h"

#include <falso_jni/FalsoJNI_Impl.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MIX_RATE   44100
#define MIX_GRAIN  2048
#define MAX_VOICES 12

/**
 * @struct SfxSample
 * @brief Internal structure for decoded PCM audio sample storage.
 */
struct SfxSample {
    short *pcm;        // interleaved, native channel count, malloc'd
    unsigned frames;
    int channels;       // 1 or 2
    int rate;
};

/**
 * @brief ITU/IMA-ADPCM standard lookup tables for step sizes and index shifts.
 */
static const short kImaStepTable[89] = {
        7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
        19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
        50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
        130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
        337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
        876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
        2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
        5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const signed char kImaIndexTable[16] = {
        -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

struct ImaState {
    int predictor;
    int stepIndex;
};

static inline short ima_decode_nibble(ImaState *st, int nibble) {
    int step = kImaStepTable[st->stepIndex];
    int diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    if (nibble & 8) st->predictor -= diff; else st->predictor += diff;
    if (st->predictor > 32767) st->predictor = 32767;
    else if (st->predictor < -32768) st->predictor = -32768;
    st->stepIndex += kImaIndexTable[nibble];
    if (st->stepIndex < 0) st->stepIndex = 0;
    else if (st->stepIndex > 88) st->stepIndex = 88;
    return (short) st->predictor;
}

/**
 * @brief Mono IMA-ADPCM block decoder.
 */
static void decode_ima_block_mono(const unsigned char *block, int blockAlign, short *out, int samplesPerBlock) {
    ImaState st;
    st.predictor = (short) (block[0] | (block[1] << 8));
    st.stepIndex = block[2];
    if (st.stepIndex > 88) st.stepIndex = 88;
    out[0] = (short) st.predictor;
    int outIdx = 1;
    for (int i = 4; i < blockAlign && outIdx < samplesPerBlock; i++) {
        int b = block[i];
        out[outIdx++] = ima_decode_nibble(&st, b & 0xF);
        if (outIdx < samplesPerBlock) out[outIdx++] = ima_decode_nibble(&st, (b >> 4) & 0xF);
    }
}

/**
 * @brief Stereo IMA-ADPCM block decoder.
 */
static void decode_ima_block_stereo(const unsigned char *block, int blockAlign, short *out, int samplesPerBlock) {
    ImaState stL, stR;
    stL.predictor = (short) (block[0] | (block[1] << 8));
    stL.stepIndex = block[2];
    if (stL.stepIndex > 88) stL.stepIndex = 88;
    stR.predictor = (short) (block[4] | (block[5] << 8));
    stR.stepIndex = block[6];
    if (stR.stepIndex > 88) stR.stepIndex = 88;
    out[0] = (short) stL.predictor;
    out[1] = (short) stR.predictor;
    int frameIdx = 1;
    int pos = 8;
    while (pos + 8 <= blockAlign && frameIdx < samplesPerBlock) {
        short lSamples[8], rSamples[8];
        for (int n = 0; n < 4; n++) {
            int b = block[pos + n];
            lSamples[n * 2] = ima_decode_nibble(&stL, b & 0xF);
            lSamples[n * 2 + 1] = ima_decode_nibble(&stL, (b >> 4) & 0xF);
        }
        for (int n = 0; n < 4; n++) {
            int b = block[pos + 4 + n];
            rSamples[n * 2] = ima_decode_nibble(&stR, b & 0xF);
            rSamples[n * 2 + 1] = ima_decode_nibble(&stR, (b >> 4) & 0xF);
        }
        for (int k = 0; k < 8 && frameIdx < samplesPerBlock; k++) {
            out[frameIdx * 2] = lSamples[k];
            out[frameIdx * 2 + 1] = rSamples[k];
            frameIdx++;
        }
        pos += 8;
    }
}

static bool decode_ima_adpcm(const unsigned char *raw, unsigned dataSize, int channels, int blockAlign,
                              int samplesPerBlock, short **outPcm, unsigned *outFrames) {
    if (blockAlign <= 0 || samplesPerBlock <= 0 || (channels != 1 && channels != 2)) return false;
    unsigned numBlocks = dataSize / (unsigned) blockAlign;
    if (numBlocks == 0) return false;
    unsigned totalFrames = numBlocks * (unsigned) samplesPerBlock;
    short *pcm = (short *) malloc((size_t) totalFrames * channels * sizeof(short));
    if (!pcm) return false;
    for (unsigned b = 0; b < numBlocks; b++) {
        const unsigned char *block = raw + (size_t) b * blockAlign;
        short *out = pcm + (size_t) b * samplesPerBlock * channels;
        if (channels == 2) decode_ima_block_stereo(block, blockAlign, out, samplesPerBlock);
        else decode_ima_block_mono(block, blockAlign, out, samplesPerBlock);
    }
    *outPcm = pcm;
    *outFrames = totalFrames;
    return true;
}

/**
 * @brief Generic RIFF/WAVE chunk parser for .wav files (PCM and IMA-ADPCM).
 */
static bool decode_wav_file(const char *path, SfxSample *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    unsigned char riff[12];
    if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        fclose(f);
        return false;
    }

    int channels = 0, rate = 0, bits = 0, formatTag = 0, blockAlign = 0, samplesPerBlock = 0;
    bool haveFmt = false, haveData = false;
    long dataOffset = 0;
    unsigned dataSize = 0;

    while (!haveData) {
        unsigned char hdr[8];
        if (fread(hdr, 1, 8, f) != 8) break;
        unsigned chunkSize = (unsigned) hdr[4] | ((unsigned) hdr[5] << 8) | ((unsigned) hdr[6] << 16) |
                             ((unsigned) hdr[7] << 24);

        if (memcmp(hdr, "fmt ", 4) == 0) {
            unsigned char fb[40];
            unsigned toRead = chunkSize < sizeof(fb) ? chunkSize : (unsigned) sizeof(fb);
            if (fread(fb, 1, toRead, f) != toRead) break;
            if (chunkSize > toRead) fseek(f, (long) (chunkSize - toRead), SEEK_CUR);
            formatTag = fb[0] | (fb[1] << 8);
            channels = fb[2] | (fb[3] << 8);
            rate = fb[4] | (fb[5] << 8) | (fb[6] << 16) | (fb[7] << 24);
            blockAlign = fb[12] | (fb[13] << 8);
            bits = fb[14] | (fb[15] << 8);
            if (formatTag == 17 /* IMA ADPCM */ && toRead >= 20) {
                samplesPerBlock = fb[18] | (fb[19] << 8);
            }
            haveFmt = true;
        } else if (memcmp(hdr, "data", 4) == 0) {
            dataOffset = ftell(f);
            dataSize = chunkSize;
            haveData = true;
        } else {
            fseek(f, (long) (chunkSize + (chunkSize & 1)), SEEK_CUR);
        }
    }

    if (!haveFmt || !haveData || channels < 1 || channels > 2 || rate <= 0 || dataSize == 0) {
        fclose(f);
        return false;
    }

    bool ok = false;
    if (formatTag == 1 && bits == 16) {
        short *pcm = (short *) malloc(dataSize);
        if (pcm) {
            fseek(f, dataOffset, SEEK_SET);
            if (fread(pcm, 1, dataSize, f) == dataSize) {
                out->pcm = pcm;
                out->frames = dataSize / ((unsigned) channels * 2);
                out->channels = channels;
                out->rate = rate;
                ok = true;
            } else {
                free(pcm);
            }
        }
    } else if (formatTag == 17) {
        unsigned char *raw = (unsigned char *) malloc(dataSize);
        if (raw) {
            fseek(f, dataOffset, SEEK_SET);
            if (fread(raw, 1, dataSize, f) == dataSize) {
                short *pcm = NULL;
                unsigned frames = 0;
                if (decode_ima_adpcm(raw, dataSize, channels, blockAlign, samplesPerBlock, &pcm, &frames)) {
                    out->pcm = pcm;
                    out->frames = frames;
                    out->channels = channels;
                    out->rate = rate;
                    ok = true;
                }
            }
            free(raw);
        }
    } else {
        l_warn("[audio] %s: unsupported WAV format tag %d (bits=%d) -- silent", path, formatTag, bits);
    }

    fclose(f);
    return ok;
}

/**
 * @brief Decoder for VoxN (.vxn) audio container files.
 */
static bool decode_vxn_file(const char *path, SfxSample *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    unsigned char hdr[8];
    if (fread(hdr, 1, 8, f) != 8 || memcmp(hdr, "VoxN", 4) != 0) {
        fclose(f);
        return false;
    }
    unsigned topLen = (unsigned) hdr[4] | ((unsigned) hdr[5] << 8) | ((unsigned) hdr[6] << 16) |
                      ((unsigned) hdr[7] << 24);
    fseek(f, (long) topLen, SEEK_CUR);

    int channels = 0, rate = 0, formatTag = 0;
    bool haveFmt = false, haveData = false;
    long dataOffset = 0;
    unsigned dataSize = 0;

    for (int guard = 0; guard < 64 && !haveData; guard++) {
        unsigned char chdr[8];
        if (fread(chdr, 1, 8, f) != 8) break;
        for (int i = 0; i < 4; i++) {
            if (chdr[i] < 0x20 || chdr[i] > 0x7e) { haveData = false; goto done; }
        }
        unsigned len = (unsigned) chdr[4] | ((unsigned) chdr[5] << 8) | ((unsigned) chdr[6] << 16) |
                       ((unsigned) chdr[7] << 24);
        if (memcmp(chdr, "Afmt", 4) == 0 && len >= 8) {
            unsigned char fb[12];
            unsigned toRead = len < sizeof(fb) ? len : (unsigned) sizeof(fb);
            if (fread(fb, 1, toRead, f) != toRead) break;
            if (len > toRead) fseek(f, (long) (len - toRead), SEEK_CUR);
            formatTag = fb[0] | (fb[1] << 8);
            channels = fb[2] | (fb[3] << 8);
            rate = fb[4] | (fb[5] << 8) | (fb[6] << 16) | (fb[7] << 24);
            haveFmt = true;
        } else if (memcmp(chdr, "Data", 4) == 0) {
            dataOffset = ftell(f);
            dataSize = len;
            haveData = true;
        } else {
            fseek(f, (long) len, SEEK_CUR);
        }
    }
done:
    if (!haveFmt || !haveData || channels < 1 || channels > 2 || rate <= 0 || dataSize == 0 || formatTag != 17) {
        fclose(f);
        return false;
    }

    const int blockAlign = 1024;
    const int samplesPerBlock = (channels == 2) ? (((blockAlign - 8) / 8) * 8 + 1) : (((blockAlign - 4) * 2) + 1);

    bool ok = false;
    unsigned char *raw = (unsigned char *) malloc(dataSize);
    if (raw) {
        fseek(f, dataOffset, SEEK_SET);
        if (fread(raw, 1, dataSize, f) == dataSize) {
            short *pcm = NULL;
            unsigned frames = 0;
            if (decode_ima_adpcm(raw, dataSize, channels, blockAlign, samplesPerBlock, &pcm, &frames)) {
                out->pcm = pcm;
                out->frames = frames;
                out->channels = channels;
                out->rate = rate;
                ok = true;
            }
        }
        free(raw);
    }

    fclose(f);
    return ok;
}

/**
 * @brief Shared sound sample cache indexed by sound ID.
 */
static SfxSample *gCache[SOUNDDEFS_COUNT];

static void sound_path_for(int sndId, const char *ext, char *out, size_t outSize) {
    const char *name = Sounddefs[sndId];
    const char *dot = strrchr(name, '.');
    size_t baseLen = dot ? (size_t) (dot - name) : strlen(name);
    snprintf(out, outSize, DATA_PATH "data/sounds/%.*s.%s", (int) baseLen, name, ext);
}

static SfxSample *sfx_get(int sndId) {
    if (sndId < 0 || sndId >= SOUNDDEFS_COUNT) return NULL;
    if (gCache[sndId]) return gCache[sndId];

    char path[512];
    SfxSample *s = (SfxSample *) calloc(1, sizeof(SfxSample));
    if (!s) return NULL;

    sound_path_for(sndId, "wav", path, sizeof(path));
    bool ok = decode_wav_file(path, s);
    if (!ok) {
        sound_path_for(sndId, "vxn", path, sizeof(path));
        ok = decode_vxn_file(path, s);
    }
    if (!ok) {
        l_warn("[audio] could not load sound %d (%s) from %s -- staying silent for this id",
               sndId, Sounddefs[sndId], path);
        free(s);
        gCache[sndId] = (SfxSample *) -1;
        return NULL;
    }
    gCache[sndId] = s;
    return s;
}

static inline SfxSample *sfx_cached_or_null(int sndId) {
    if (sndId < 0 || sndId >= SOUNDDEFS_COUNT) return NULL;
    SfxSample *s = gCache[sndId];
    return (s == (SfxSample *) -1) ? NULL : s;
}

/**
 * @struct Voice
 * @brief Polyphonic voice instance for sceAudioOut mixer.
 */
struct Voice {
    SfxSample *smp;
    double pos;
    double step;
    float gain;
    float targetGain;
    float gainStep;
    int fadeFramesLeft;
    bool loop;
    bool paused;
    int sndId;
    int instance;
};

static void voice_fade_to(Voice *v, float target, int fadeMs) {
    if (target < 0.0f) target = 0.0f;
    if (target > 1.0f) target = 1.0f;
    if (fadeMs <= 0) {
        v->gain = target;
        v->targetGain = target;
        v->fadeFramesLeft = 0;
        v->gainStep = 0.0f;
        return;
    }
    int frames = (int) ((double) fadeMs * MIX_RATE / 1000.0);
    if (frames < 1) frames = 1;
    v->targetGain = target;
    v->fadeFramesLeft = frames;
    v->gainStep = (target - v->gain) / (float) frames;
}

static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static bool gAudioReady = false;
static volatile int gQuit = 0;
static int gPort = -1;
static SceUID gThread = -1;

static Voice gVoices[MAX_VOICES];
static Voice gBig;

#define SOFT_CLIP_THRESHOLD 0.92f

/**
 * @brief Soft-clipping compression using tanhf to prevent distortion when mixing voices.
 */
static inline short soft_clip16(int v) {
    const float full = 32768.0f;
    float x = (float) v / full;
    float ax = fabsf(x);
    if (ax <= SOFT_CLIP_THRESHOLD) {
        if (v > 32767) return 32767;
        if (v < -32768) return -32768;
        return (short) v;
    }
    float sign = (x < 0.0f) ? -1.0f : 1.0f;
    float over = (ax - SOFT_CLIP_THRESHOLD) / (1.0f - SOFT_CLIP_THRESHOLD);
    float compressed = SOFT_CLIP_THRESHOLD + (1.0f - SOFT_CLIP_THRESHOLD) * tanhf(over);
    float outv = sign * compressed * full;
    if (outv > 32767.0f) outv = 32767.0f;
    if (outv < -32768.0f) outv = -32768.0f;
    return (short) outv;
}

static void mix_voice(Voice *v, int *acc, int frames) {
    SfxSample *s = v->smp;
    for (int i = 0; i < frames; i++) {
        unsigned idx = (unsigned) v->pos;
        if (idx + 1 >= s->frames) {
            if (v->loop && s->frames > 1) {
                v->pos -= (double) (s->frames - 1);
                if (v->pos < 0.0) v->pos = 0.0;
                idx = (unsigned) v->pos;
            } else {
                v->smp = NULL;
                return;
            }
        }
        float frac = (float) (v->pos - idx);
        float l, r;
        if (s->channels == 2) {
            const short *a = &s->pcm[idx * 2];
            const short *b = &s->pcm[(idx + 1) * 2];
            l = a[0] + (b[0] - a[0]) * frac;
            r = a[1] + (b[1] - a[1]) * frac;
        } else {
            float m = s->pcm[idx] + (s->pcm[idx + 1] - s->pcm[idx]) * frac;
            l = r = m;
        }
        if (v->fadeFramesLeft > 0) {
            v->gain += v->gainStep;
            if (--v->fadeFramesLeft == 0) v->gain = v->targetGain;
        }
        acc[i * 2] += (int) (l * v->gain);
        acc[i * 2 + 1] += (int) (r * v->gain);
        v->pos += v->step;
    }
}

static void drain_audiotrack_into(int *acc, int frames);

/**
 * @brief Main mixer thread processing active voices and draining AudioTrack FIFO buffer.
 */
static int mixer_thread(SceSize args, void *argp) {
    (void) args;
    (void) argp;
    static short out[2][MIX_GRAIN * 2];
    int acc[MIX_GRAIN * 2];
    int bufId = 0;

    while (!gQuit) {
        memset(acc, 0, sizeof(acc));

        pthread_mutex_lock(&gLock);
        if (gBig.smp && !gBig.paused) mix_voice(&gBig, acc, MIX_GRAIN);
        for (int i = 0; i < MAX_VOICES; i++) {
            if (gVoices[i].smp && !gVoices[i].paused) mix_voice(&gVoices[i], acc, MIX_GRAIN);
        }
        pthread_mutex_unlock(&gLock);

        drain_audiotrack_into(acc, MIX_GRAIN);

        for (int i = 0; i < MIX_GRAIN * 2; i++) {
            out[bufId][i] = soft_clip16(acc[i]);
        }
        sceAudioOutOutput(gPort, out[bufId]);
        bufId ^= 1;
    }
    return 0;
}

void audio_init(void) {
    memset(gCache, 0, sizeof(gCache));
    memset(gVoices, 0, sizeof(gVoices));
    memset(&gBig, 0, sizeof(gBig));

    gPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, MIX_GRAIN, MIX_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (gPort < 0) {
        l_error("[audio] sceAudioOutOpenPort failed (0x%08X) -- audio disabled, game continues silent",
                (unsigned) gPort);
        return;
    }

    gQuit = 0;
    // Pinned to USER_1, off the main thread's core (USER_0, see main.c) -- this
    // thread runs continuously for the whole game session (blocks on
    // sceAudioOutOutput between mix passes, not a one-shot), so leaving it
    // unpinned means the scheduler can put it on the same core as the main
    // render/logic thread at any time, unlike a transient worker.
    gThread = sceKernelCreateThread("audio_mixer", mixer_thread, 0x10000100, 0x10000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (gThread < 0) {
        l_error("[audio] mixer thread creation failed (0x%08X) -- audio disabled", (unsigned) gThread);
        sceAudioOutReleasePort(gPort);
        gPort = -1;
        return;
    }
    sceKernelStartThread(gThread, 0, NULL);
    gAudioReady = true;
    l_success("[audio] initialized (sceAudioOut mixer, %d voices + 1 BGM/big slot)", MAX_VOICES);
}

void audio_shutdown(void) {
    if (!gAudioReady) return;
    gAudioReady = false;
    gQuit = 1;
    sceKernelWaitThreadEnd(gThread, NULL, NULL);
    sceKernelDeleteThread(gThread);
    gThread = -1;
    sceAudioOutReleasePort(gPort);
    gPort = -1;

    for (int i = 0; i < SOUNDDEFS_COUNT; i++) {
        if (gCache[i] && gCache[i] != (SfxSample *) -1) {
            free(gCache[i]->pcm);
            free(gCache[i]);
        }
    }
    memset(gCache, 0, sizeof(gCache));
}

/**
 * @brief Implementation of GLMediaPlayer JNI functions.
 */
jint GLMediaPlayer_isSoundLoaded(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    (void) va_arg(args, jint);
    return sfx_cached_or_null(sndId) ? 1 : 0;
}

jint GLMediaPlayer_isSoundLoadedBig(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    return sfx_cached_or_null(sndId) ? 1 : 0;
}

void GLMediaPlayer_loadSound(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    (void) va_arg(args, jint);
    if (!gAudioReady) return;
    if (sndId >= 0 && sndId < SOUNDDEFS_COUNT) sfx_get(sndId);
}

void GLMediaPlayer_loadSoundBig(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    if (!gAudioReady) return;
    if (sndId >= 0 && sndId < SOUNDDEFS_COUNT) sfx_get(sndId);
}

void GLMediaPlayer_playSound(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    int instance = va_arg(args, jint);
    float vol = (float) va_arg(args, double);
    if (!gAudioReady || sndId < 0 || sndId >= SOUNDDEFS_COUNT) return;

    SfxSample *s = sfx_get(sndId);
    if (!s) return;

    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;

    pthread_mutex_lock(&gLock);
    Voice *v = NULL;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!gVoices[i].smp) { v = &gVoices[i]; break; }
    }
    if (v) {
        v->pos = 0.0;
        v->step = (double) s->rate / (double) MIX_RATE;
        v->gain = v->targetGain = vol;
        v->fadeFramesLeft = 0;
        v->gainStep = 0.0f;
        v->loop = false;
        v->paused = false;
        v->sndId = sndId;
        v->instance = instance;
        v->smp = s;
    }
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_playSoundBig(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    float vol = (float) va_arg(args, double);
    int loop = va_arg(args, jint);
    int fadeInMs = va_arg(args, jint);
    if (!gAudioReady || sndId < 0 || sndId >= SOUNDDEFS_COUNT) return;

    SfxSample *s = sfx_get(sndId);
    if (!s) return;

    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;

    pthread_mutex_lock(&gLock);
    gBig.pos = 0.0;
    gBig.step = (double) s->rate / (double) MIX_RATE;
    gBig.loop = loop ? true : false;
    gBig.paused = false;
    gBig.sndId = sndId;
    gBig.instance = -1;
    if (fadeInMs > 0) {
        gBig.gain = 0.0f;
        voice_fade_to(&gBig, vol, fadeInMs);
    } else {
        voice_fade_to(&gBig, vol, 0);
    }
    gBig.smp = s;
    pthread_mutex_unlock(&gLock);
    l_info("[audio] playSoundBig: %s (loop=%d, vol=%.2f, fadeIn=%dms)", Sounddefs[sndId], loop, (double) vol,
           fadeInMs);
}

static Voice *find_voice_locked(int sndId, int instance) {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (gVoices[i].smp && gVoices[i].sndId == sndId && gVoices[i].instance == instance) return &gVoices[i];
    }
    return NULL;
}

void GLMediaPlayer_pauseSound(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    int instance = va_arg(args, jint);
    if (!gAudioReady) return;
    pthread_mutex_lock(&gLock);
    Voice *v = find_voice_locked(sndId, instance);
    if (v) v->paused = true;
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_resumeSound(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    int instance = va_arg(args, jint);
    if (!gAudioReady) return;
    pthread_mutex_lock(&gLock);
    Voice *v = find_voice_locked(sndId, instance);
    if (v) v->paused = false;
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_stopSound(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    int instance = va_arg(args, jint);
    if (!gAudioReady) return;
    pthread_mutex_lock(&gLock);
    Voice *v = find_voice_locked(sndId, instance);
    if (v) v->smp = NULL;
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_setVolume(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    int instance = va_arg(args, jint);
    float vol = (float) va_arg(args, double);
    if (!gAudioReady) return;
    pthread_mutex_lock(&gLock);
    Voice *v = find_voice_locked(sndId, instance);
    if (v) voice_fade_to(v, vol, 0);
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_setPitch(jmethodID id, va_list args) {
    (void) id;
    int sndId = va_arg(args, jint);
    int instance = va_arg(args, jint);
    float pitch = (float) va_arg(args, double);
    if (!gAudioReady) return;
    if (pitch < 0.25f) pitch = 0.25f;
    if (pitch > 4.0f) pitch = 4.0f;
    pthread_mutex_lock(&gLock);
    Voice *v = find_voice_locked(sndId, instance);
    if (v && v->smp) v->step = (double) pitch * ((double) v->smp->rate / (double) MIX_RATE);
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_pauseSoundBig(jmethodID id, va_list args) {
    (void) id;
    (void) va_arg(args, jint);
    if (!gAudioReady) return;
    pthread_mutex_lock(&gLock);
    gBig.paused = true;
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_resumeSoundBig(jmethodID id, va_list args) {
    (void) id;
    (void) va_arg(args, jint);
    if (!gAudioReady) return;
    pthread_mutex_lock(&gLock);
    gBig.paused = false;
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_stopSoundBig(jmethodID id, va_list args) {
    (void) id;
    (void) va_arg(args, jint);
    if (!gAudioReady) return;
    pthread_mutex_lock(&gLock);
    gBig.smp = NULL;
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_setVolumeBig(jmethodID id, va_list args) {
    (void) id;
    (void) va_arg(args, jint);
    float vol = (float) va_arg(args, double);
    int fadeMs = va_arg(args, jint);
    if (!gAudioReady) return;
    pthread_mutex_lock(&gLock);
    if (gBig.smp) voice_fade_to(&gBig, vol, fadeMs);
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_stopAllSounds(jmethodID id, va_list args) {
    (void) id;
    (void) args;
    if (!gAudioReady) return;
    pthread_mutex_lock(&gLock);
    for (int i = 0; i < MAX_VOICES; i++) gVoices[i].smp = NULL;
    pthread_mutex_unlock(&gLock);
}

void GLMediaPlayer_stopAllPool(jmethodID id, va_list args) {
    GLMediaPlayer_stopAllSounds(id, args);
}

void GLMediaPlayer_stopAllBig(jmethodID id, va_list args) {
    (void) id;
    (void) args;
    if (!gAudioReady) return;
    pthread_mutex_lock(&gLock);
    gBig.smp = NULL;
    pthread_mutex_unlock(&gLock);
}

/**
 * @file audio.cpp (AudioTrack Shim)
 * @brief Interceptor for android.media.AudioTrack.write() calls from Vox middleware.
 */
#define AT_FIFO_BYTES (MIX_GRAIN * 2 * 2 * 4)

static unsigned char gATFifo[AT_FIFO_BYTES];
static int gATFifoLen = 0;
static pthread_mutex_t gATLock = PTHREAD_MUTEX_INITIALIZER;

static unsigned gATCallCount = 0;
static unsigned long long gATBytesTotal = 0;
static unsigned gATTimeoutCount = 0;
static unsigned long long gATFramesDrained = 0;
#define AT_LOG_EVERY_N_CALLS 500

static void drain_audiotrack_into(int *acc, int frames) {
    pthread_mutex_lock(&gATLock);
    int wantBytes = frames * 2 * 2;
    int copyBytes = gATFifoLen < wantBytes ? gATFifoLen : wantBytes;
    copyBytes -= copyBytes % 4;
    if (copyBytes > 0) {
        const short *src = (const short *) gATFifo;
        int n = copyBytes / 2;
        for (int i = 0; i < n; i++) acc[i] += src[i];
        int remaining = gATFifoLen - copyBytes;
        if (remaining > 0) memmove(gATFifo, gATFifo + copyBytes, (size_t) remaining);
        gATFifoLen = remaining;
        gATFramesDrained += (unsigned long long) (copyBytes / 4);
    }
    pthread_mutex_unlock(&gATLock);
}

jint Misc_AudioTrackWrite(jmethodID id, va_list args) {
    (void) id;
    JavaDynArray *jda = (JavaDynArray *) va_arg(args, jobject);
    jint offset = va_arg(args, jint);
    jint sizeInBytes = va_arg(args, jint);

    if (!jda || !jda->array || sizeInBytes <= 0) return sizeInBytes;
    if (!gAudioReady) return sizeInBytes;

    gATCallCount++;
    if (gATCallCount == 1) {
        l_success("[audiotrack] first write() call: sizeInBytes=%d -- Vox's AudioTrack output path is alive",
                   (int) sizeInBytes);
    }

    const unsigned char *src = (const unsigned char *) jda->array + offset;
    int remaining = (int) sizeInBytes;

    int guard = 0;
    while (remaining > 0 && guard < 2000) {
        pthread_mutex_lock(&gATLock);
        int space = AT_FIFO_BYTES - gATFifoLen;
        int chunk = remaining < space ? remaining : space;
        if (chunk > 0) {
            memcpy(gATFifo + gATFifoLen, src, (size_t) chunk);
            gATFifoLen += chunk;
            src += chunk;
            remaining -= chunk;
        }
        pthread_mutex_unlock(&gATLock);
        if (remaining > 0) {
            sceKernelDelayThread(1000);
            guard++;
        }
    }

    gATBytesTotal += (unsigned long long) (sizeInBytes - remaining);
    if (remaining > 0) {
        gATTimeoutCount++;
        if (gATTimeoutCount <= 5) {
            l_warn("[audiotrack] write() timed out waiting for FIFO room -- dropped %d/%d bytes (timeout #%u)",
                   remaining, (int) sizeInBytes, gATTimeoutCount);
        }
    }
    if (gATCallCount % AT_LOG_EVERY_N_CALLS == 0) {
        l_info("[audiotrack] %u write() calls, %llu bytes total, %llu frames drained by mixer, %u timeouts so far",
               gATCallCount, gATBytesTotal, gATFramesDrained, gATTimeoutCount);
    }

    return sizeInBytes;
}

void Misc_AudioTrackRelease(jmethodID id, va_list args) {
    (void) id;
    (void) args;
    pthread_mutex_lock(&gATLock);
    gATFifoLen = 0;
    pthread_mutex_unlock(&gATLock);
}
