/**
 * @file  audio.h
 * @brief Native sceAudioOut-based mixer backing GLMediaPlayer's sound bridge
 *        (see java.c's "GLMediaPlayer: sound bridge" section, previously all
 *        no-op stubs -- "Phase 7, not implemented yet").
 *
 * Architecture follows the sibling Prince-of-Persia-vita port's proven
 * audio.cpp (sceAudioOut + a small software mixer on its own thread), NOT
 * SoLoud -- same reasoning applies here: keep this file's own file I/O and
 * decoding fully self-contained, no vendored library with its own FILE*
 * assumptions to fight. Adapted for what THIS game's assets actually are
 * (confirmed by inspecting the real extracted data/sounds/ directory, not
 * assumed from Sounddefs[]'s own ".ogg" names, which turned out to be stale --
 * the real shipped files are .wav):
 *   - 228 files: plain 16-bit PCM WAV (mono or stereo).
 *   - 16 files: Microsoft IMA-ADPCM WAV, stereo, 32kHz (the "m_*" boss/theme
 *     tracks) -- about 4:1 smaller than PCM, presumably why Gameloft used it
 *     for the longer music tracks specifically.
 *   - 17 files (data/sounds/m_level_*.vxn, one per zone's ambient loop): a
 *     proprietary "VoxN" container. Decoded now (previously skipped): it
 *     turned out to be a plain chunked format, structurally identical in
 *     spirit to RIFF/WAV -- [4-byte ASCII tag][4-byte LE length][payload],
 *     repeated. Verified generically across 3 different files (different
 *     zone tracks have different chunk counts/sizes for the adaptive-music
 *     metadata chunks -- "Segm"/"Rule"/"Plst"/"Stat"/"Trsn"/"Grps"/"Grpe" --
 *     but always terminate in a "Data" chunk whose declared offset+length
 *     lands EXACTLY on EOF in all 3, byte for byte). The metadata chunks
 *     (loop segments/transition rules/groups -- presumably how Vox
 *     crossfades between a zone's "calm"/"combat" states) are walked and
 *     skipped generically by their own self-declared length, not
 *     interpreted -- only "Afmt" (format/channels/rate, same 3 fields WAV's
 *     fmt chunk has) and "Data" (the raw IMA-ADPCM payload, decoded with the
 *     exact same decoder as the .wav files above) are read. This means
 *     playback loops the WHOLE track from the start rather than honoring
 *     whatever the Segm chunk's actual loop-in/loop-out points are -- a
 *     deliberate simplification (full segment/rule semantics were not worth
 *     the added risk to reverse-engineer blind), not a bug.
 */

#ifndef DH2_AUDIO_H
#define DH2_AUDIO_H

#include <falso_jni/FalsoJNI.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_init(void);
void audio_shutdown(void);

// GLMediaPlayer: sound bridge (see java.c). Implemented for real here:
jint GLMediaPlayer_isSoundLoaded(jmethodID id, va_list args);
jint GLMediaPlayer_isSoundLoadedBig(jmethodID id, va_list args);
void GLMediaPlayer_loadSound(jmethodID id, va_list args);
void GLMediaPlayer_loadSoundBig(jmethodID id, va_list args);
void GLMediaPlayer_playSound(jmethodID id, va_list args);
void GLMediaPlayer_playSoundBig(jmethodID id, va_list args); // fadeIn now implemented as a real linear ramp
void GLMediaPlayer_pauseSound(jmethodID id, va_list args);
void GLMediaPlayer_resumeSound(jmethodID id, va_list args);
void GLMediaPlayer_stopSound(jmethodID id, va_list args);
void GLMediaPlayer_setVolume(jmethodID id, va_list args);
void GLMediaPlayer_setPitch(jmethodID id, va_list args);
void GLMediaPlayer_pauseSoundBig(jmethodID id, va_list args);
void GLMediaPlayer_resumeSoundBig(jmethodID id, va_list args);
void GLMediaPlayer_stopSoundBig(jmethodID id, va_list args);
void GLMediaPlayer_setVolumeBig(jmethodID id, va_list args); // fade argument implemented too
void GLMediaPlayer_stopAllSounds(jmethodID id, va_list args);
void GLMediaPlayer_stopAllPool(jmethodID id, va_list args);
void GLMediaPlayer_stopAllBig(jmethodID id, va_list args);

// android/media/AudioTrack.write/release bridge -- see java.c's Misc_Dummy*
// stubs and audio.cpp's own top comment for why THIS, not GLMediaPlayer
// above, turned out to be the real active audio path: the engine's own
// native audio middleware ("vox::DriverAndroid", see out_ghidra.c) decodes
// and mixes everything itself, then calls Java's AudioTrack.write() purely
// as an output sink. Misc_DummyAudioTrack/Misc_GetMinBufferSize/play/pause/
// stop stay exactly as they were in java.c (harmless as no-ops/plausible
// constants) -- only write (was a complete fake: read the args, discarded
// them, lied that it wrote everything) and release are real now.
jint Misc_AudioTrackWrite(jmethodID id, va_list args);
void Misc_AudioTrackRelease(jmethodID id, va_list args);

#ifdef __cplusplus
}
#endif

#endif // DH2_AUDIO_H
