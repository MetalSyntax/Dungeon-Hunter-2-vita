// Cutscene playback via the Vita's native SceAvPlayer, wired into
// GLMediaPlayer_loadMovie (java.c). Ported from the Prince of Persia Classic
// Vita port's source/video.cpp (same soloader lineage, hardware-confirmed
// there across several rounds) -- see that project's Fixes_Log.md #16-#19 for
// the original debugging history. Two things differ here because this
// project's graphics stack is different:
//
//  * No vitaGL: this project renders through the real PVR_PSP2 GLES2 driver
//    (source/utils/glutil.c), a GLES2-only context with no fixed-function
//    matrix stack/immediate-mode arrays. draw_video_frame() below is a small
//    dedicated GLSL ES program (compiled with the same glCompileShader_/
//    glLinkProgram_soloader status-checking wrappers glutil.c already proves
//    work against the real on-device GLSL compiler) instead of the
//    glOrthof/glVertexPointer calls the vitaGL/GLES1 build used. Buffer
//    presentation goes through this project's own gl_swap() (glutil.h)
//    instead of vglSwapBuffers().
//  * SceAvPlayer's memory-allocator/event/file-replacement setup and the NV12
//    (Y + interleaved UV) -> RGB565 NEON conversion are UNCHANGED: none of
//    that touches vitaGL/GLES at all (memalign/free, a dedicated
//    sceKernelAllocMemBlock CDRAM/PHYCONT block per texture allocation +
//    sceGxmMapMemory, and plain NEON intrinsics), so it carries over as-is.
//
// Same never-hangs contract as the original: video_play() always returns
// (natural end, user skip, or any open/init failure), so the caller can
// unconditionally poke videoDone afterwards.

#include "video.h"
#include "utils/logger.h"
#include "utils/glutil.h"

#include <psp2/avplayer.h>
#include <psp2/sysmodule.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/ctrl.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/gxm.h>

#include <GLES2/gl2.h>

#include <arm_neon.h>
#include <malloc.h>
#include <pthread.h>
#include <string.h>
#include <string>

static bool gModuleLoaded = false;
static unsigned short *gRgbBuf = NULL;
static unsigned gRgbBufCap = 0;
static unsigned char *gYuvScratch = NULL;
static unsigned gYuvScratchCap = 0;

// --- SceAvPlayer file I/O: plain sceIo, with visibility into how far the
// player actually got into the file before giving up (see the original
// project's file-level comment for why this exists instead of leaving
// fileReplacement unset -- it's the difference between "silent black
// screen" and a log that shows open/size/read results). ---
struct AvFileCtx {
    SceUID fd;
    uint64_t total_read;
    unsigned read_calls;
};
static AvFileCtx gAvFileCtx = { -1, 0, 0 };

static int av_file_open(void *p, const char *filename) {
    AvFileCtx *ctx = (AvFileCtx *) p;
    ctx->fd = sceIoOpen(filename, SCE_O_RDONLY, 0);
    ctx->total_read = 0;
    ctx->read_calls = 0;
    l_info("video: file open(%s) -> fd=0x%08X", filename, (unsigned) ctx->fd);
    return ctx->fd < 0 ? -1 : 0;
}

static int av_file_close(void *p) {
    AvFileCtx *ctx = (AvFileCtx *) p;
    l_info("video: file close (reads=%u, total_bytes=%llu)", ctx->read_calls,
           (unsigned long long) ctx->total_read);
    if (ctx->fd >= 0) sceIoClose(ctx->fd);
    ctx->fd = -1;
    return 0;
}

static int av_file_read(void *p, uint8_t *buffer, uint64_t position, uint32_t length) {
    AvFileCtx *ctx = (AvFileCtx *) p;
    int n = sceIoPread(ctx->fd, buffer, length, (SceOff) position);
    ctx->read_calls++;
    if (ctx->read_calls <= 5 || n < 0)
        l_info("video: file read #%u pos=%llu len=%u -> %d", ctx->read_calls,
               (unsigned long long) position, length, n);
    if (n > 0) ctx->total_read += (uint64_t) n;
    return n;
}

static uint64_t av_file_size(void *p) {
    AvFileCtx *ctx = (AvFileCtx *) p;
    SceOff end = sceIoLseek(ctx->fd, 0, SCE_SEEK_END);
    l_info("video: file size -> %llu", (unsigned long long) end);
    return (uint64_t) end;
}

// --- SceAvPlayer event callback: the player's own diagnostic channel --
// every state transition, and (WARNING_ID) the actual error code on a silent
// abort, arrives here instead of just IsActive flipping to false. ---
static const char *av_event_name(int32_t id) {
    switch (id) {
        case 0x01: return "STATE_STOP";
        case 0x02: return "STATE_READY";
        case 0x03: return "STATE_PLAY";
        case 0x04: return "STATE_PAUSE";
        case 0x05: return "STATE_BUFFERING";
        case 0x10: return "TIMED_TEXT_DELIVERY";
        case 0x20: return "WARNING_ID";
        default:   return "?";
    }
}

static void av_event_cb(void *p, int32_t eventId, int32_t sourceId, void *eventData) {
    (void) p;
    if (eventId == 0x20 && eventData) {
        l_error("video: event WARNING_ID source=%d code=0x%08X", sourceId,
                (unsigned) *(int32_t *) eventData);
    } else {
        l_info("video: event %s (0x%02X) source=%d data=%p", av_event_name(eventId),
               (unsigned) eventId, sourceId, eventData);
    }
}

// --- SceAvPlayer memory: general allocations to the newlib heap (memalign/
// free -- AvPlayer makes many small internal allocations at startup;
// backing each with its own kernel memblock exhausts the process's memblock
// limit before the player finishes activating), texture/frame-buffer
// allocations to a DEDICATED kernel memblock per allocation (CDRAM, falling
// back to PHYCONT), sceGxmMapMemory'd -- the pattern proven on real hardware
// by OpenFMV and carried over unchanged from the Prince of Persia port. This
// is independent of vitaGL/PVR_PSP2: sceGxm is the shared low-level graphics
// API underneath either driver, and mapping memory into it doesn't touch
// whichever GL driver happens to be using it for rendering. ---
#define AV_FB_ALIGNMENT 0x40000
#define AV_ALIGN_MEM(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

static void *av_alloc(void *arg, uint32_t alignment, uint32_t size) {
    (void) arg;
    void *p = memalign(alignment, size);
    if (!p)
        l_error("video: general alloc FAILED (align=%u size=%u)", alignment, size);
    return p;
}

static void av_free(void *arg, void *ptr) {
    (void) arg;
    free(ptr);
}

#define AV_TEX_MAX_BLOCKS 8
static struct { void *base; SceUID uid; } gAvTexBlocks[AV_TEX_MAX_BLOCKS];

static void *av_alloc_texture(void *arg, uint32_t alignment, uint32_t size) {
    (void) arg;
    uint32_t req_align = alignment, req_size = size;
    if (alignment < AV_FB_ALIGNMENT)
        alignment = AV_FB_ALIGNMENT;
    size = AV_ALIGN_MEM(size, alignment);

    SceKernelAllocMemBlockOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = 0x00000004U; // SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT
    opt.alignment = alignment;
    SceUID blk = sceKernelAllocMemBlock("av_tex", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, &opt);
    SceUID usedType = SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
    if (blk < 0) {
        // Retry once from the PHYCONT partition (a separate physical pool
        // from CDRAM) before failing for real -- same fallback the source
        // project needed on real hardware when CDRAM was tight.
        SceUID blk2 = sceKernelAllocMemBlock("av_tex_phycont", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, size, &opt);
        if (blk2 < 0) {
            l_error("video: texture memblock alloc FAILED on both CDRAM (0x%08X) and PHYCONT (0x%08X) (req align=%u size=%u -> size=%u)",
                    (unsigned) blk, (unsigned) blk2, req_align, req_size, size);
            return NULL;
        }
        l_warn("video: CDRAM alloc failed (0x%08X) -- fell back to PHYCONT for this frame buffer", (unsigned) blk);
        blk = blk2;
        usedType = SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW;
    }
    void *base = NULL;
    sceKernelGetMemBlockBase(blk, &base);
    int map = sceGxmMapMemory(base, size, (SceGxmMemoryAttribFlags)(SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE));

    int slot = -1;
    for (int i = 0; i < AV_TEX_MAX_BLOCKS; i++) {
        if (!gAvTexBlocks[i].base) { slot = i; break; }
    }
    if (slot >= 0) {
        gAvTexBlocks[slot].base = base;
        gAvTexBlocks[slot].uid = blk;
    }
    l_info("video: texture memblock ok (%s) (req align=%u size=%u -> size=%u) base=%p uid=0x%08X gxm_map=0x%08X",
           usedType == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? "CDRAM" : "PHYCONT",
           req_align, req_size, size, base, (unsigned) blk, (unsigned) map);
    return base;
}

static void av_free_texture(void *arg, void *ptr) {
    (void) arg;
    if (!ptr) return;
    glFinish();
    for (int i = 0; i < AV_TEX_MAX_BLOCKS; i++) {
        if (gAvTexBlocks[i].base == ptr) {
            l_info("video: texture memblock free %p uid=0x%08X", ptr, (unsigned) gAvTexBlocks[i].uid);
            sceGxmUnmapMemory(ptr);
            sceKernelFreeMemBlock(gAvTexBlocks[i].uid);
            gAvTexBlocks[i].base = NULL;
            gAvTexBlocks[i].uid = -1;
            return;
        }
    }
    l_warn("video: texture free for unknown ptr %p (leaking it)", ptr);
}

// --- NV12 (Y plane + interleaved U/V, each subsampled 2x2) -> RGB565, BT.601,
// NEON-vectorized -- unchanged from the source project (pure C/NEON, no GL
// dependency at all). See that project's video.cpp for the derivation notes;
// kept verbatim here since it's already proven correct on real hardware. ---

static int CV_R[256];
static int CV_G[256];
static int CU_G[256];
static int CU_B[256];
static unsigned char clip_table[768];
static bool tables_init = false;

static void init_yuv_tables() {
    if (tables_init) return;
    for (int i = 0; i < 256; i++) {
        int V = i - 128;
        int U = i - 128;
        CV_R[i] = (91881 * V) >> 16;
        CV_G[i] = (46802 * V) >> 16;
        CU_G[i] = (22554 * U) >> 16;
        CU_B[i] = (116130 * U) >> 16;
    }
    for (int i = 0; i < 768; i++) {
        int v = i - 256;
        clip_table[i] = (v < 0) ? 0 : ((v > 255) ? 255 : v);
    }
    tables_init = true;
}

#define CLIP(X) (clip_table[(X) + 256])

static inline void store_rgb565_8(unsigned short *dst, uint8x8_t r, uint8x8_t g, uint8x8_t b) {
    uint16x8_t rw = vmovl_u8(r);
    uint16x8_t gw = vmovl_u8(g);
    uint16x8_t bw = vmovl_u8(b);
    uint16x8_t rr = vshlq_n_u16(vandq_u16(rw, vdupq_n_u16(0xF8)), 8);
    uint16x8_t gg = vshlq_n_u16(vandq_u16(gw, vdupq_n_u16(0xFC)), 3);
    uint16x8_t bb = vshrq_n_u16(bw, 3);
    vst1q_u16((uint16_t *) dst, vorrq_u16(vorrq_u16(rr, gg), bb));
}

static void yuv420p_to_rgb565(const unsigned char *src, unsigned w, unsigned h, unsigned short *dst) {
    init_yuv_tables();
    const unsigned char *yp = src;
    const unsigned char *uvp = src + (size_t) w * h;
    for (unsigned y = 0; y < h; y += 2) {
        const unsigned char *yrow0 = yp + (size_t) y * w;
        const unsigned char *yrow1 = yrow0 + w;
        const unsigned char *uvrow = uvp + (size_t) (y / 2) * w;
        unsigned short *drow0 = dst + (size_t) y * w;
        unsigned short *drow1 = drow0 + w;

        unsigned x = 0;
        for (; x + 16 <= w; x += 16) {
            uint8x8x2_t uv = vld2_u8(uvrow + x);
            int16x8_t Uc = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(uv.val[0])), vdupq_n_s16(128));
            int16x8_t Vc = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(uv.val[1])), vdupq_n_s16(128));

            int32x4_t Uc_lo = vmovl_s16(vget_low_s16(Uc));
            int32x4_t Uc_hi = vmovl_s16(vget_high_s16(Uc));
            int32x4_t Vc_lo = vmovl_s16(vget_low_s16(Vc));
            int32x4_t Vc_hi = vmovl_s16(vget_high_s16(Vc));

            int16x4_t r_add_lo = vmovn_s32(vshrq_n_s32(vmulq_n_s32(Vc_lo, 91881), 16));
            int16x4_t r_add_hi = vmovn_s32(vshrq_n_s32(vmulq_n_s32(Vc_hi, 91881), 16));

            int32x4_t cu_g_lo = vshrq_n_s32(vmulq_n_s32(Uc_lo, 22554), 16);
            int32x4_t cu_g_hi = vshrq_n_s32(vmulq_n_s32(Uc_hi, 22554), 16);
            int32x4_t cv_g_lo = vshrq_n_s32(vmulq_n_s32(Vc_lo, 46802), 16);
            int32x4_t cv_g_hi = vshrq_n_s32(vmulq_n_s32(Vc_hi, 46802), 16);
            int16x4_t g_add_lo = vneg_s16(vmovn_s32(vaddq_s32(cu_g_lo, cv_g_lo)));
            int16x4_t g_add_hi = vneg_s16(vmovn_s32(vaddq_s32(cu_g_hi, cv_g_hi)));

            int16x4_t b_add_lo = vmovn_s32(vshrq_n_s32(vmulq_n_s32(Uc_lo, 116130), 16));
            int16x4_t b_add_hi = vmovn_s32(vshrq_n_s32(vmulq_n_s32(Uc_hi, 116130), 16));

            int16x8_t r_add8 = vcombine_s16(r_add_lo, r_add_hi);
            int16x8_t g_add8 = vcombine_s16(g_add_lo, g_add_hi);
            int16x8_t b_add8 = vcombine_s16(b_add_lo, b_add_hi);
            int16x8x2_t r_dup = vzipq_s16(r_add8, r_add8);
            int16x8x2_t g_dup = vzipq_s16(g_add8, g_add8);
            int16x8x2_t b_dup = vzipq_s16(b_add8, b_add8);

            for (int half = 0; half < 2; half++) {
                const unsigned char *yr0 = yrow0 + x + half * 8;
                const unsigned char *yr1 = yrow1 + x + half * 8;
                int16x8_t r_add = half == 0 ? r_dup.val[0] : r_dup.val[1];
                int16x8_t g_add = half == 0 ? g_dup.val[0] : g_dup.val[1];
                int16x8_t b_add = half == 0 ? b_dup.val[0] : b_dup.val[1];

                int16x8_t Y0 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(yr0)));
                int16x8_t Y1 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(yr1)));

                uint8x8_t r0 = vqmovun_s16(vaddq_s16(Y0, r_add));
                uint8x8_t g0 = vqmovun_s16(vaddq_s16(Y0, g_add));
                uint8x8_t b0 = vqmovun_s16(vaddq_s16(Y0, b_add));
                uint8x8_t r1 = vqmovun_s16(vaddq_s16(Y1, r_add));
                uint8x8_t g1 = vqmovun_s16(vaddq_s16(Y1, g_add));
                uint8x8_t b1 = vqmovun_s16(vaddq_s16(Y1, b_add));

                store_rgb565_8(drow0 + x + half * 8, r0, g0, b0);
                store_rgb565_8(drow1 + x + half * 8, r1, g1, b1);
            }
        }

        for (; x < w; x += 2) {
            unsigned char U = uvrow[x + 0];
            unsigned char V = uvrow[x + 1];

            int r_add = CV_R[V];
            int g_add = -(CU_G[U] + CV_G[V]);
            int b_add = CU_B[U];

            int Y00 = yrow0[x];
            unsigned char r00 = CLIP(Y00 + r_add), g00 = CLIP(Y00 + g_add), b00 = CLIP(Y00 + b_add);
            drow0[x] = (unsigned short) (((r00 & 0xF8) << 8) | ((g00 & 0xFC) << 3) | (b00 >> 3));

            int Y01 = yrow0[x+1];
            unsigned char r01 = CLIP(Y01 + r_add), g01 = CLIP(Y01 + g_add), b01 = CLIP(Y01 + b_add);
            drow0[x+1] = (unsigned short) (((r01 & 0xF8) << 8) | ((g01 & 0xFC) << 3) | (b01 >> 3));

            int Y10 = yrow1[x];
            unsigned char r10 = CLIP(Y10 + r_add), g10 = CLIP(Y10 + g_add), b10 = CLIP(Y10 + b_add);
            drow1[x] = (unsigned short) (((r10 & 0xF8) << 8) | ((g10 & 0xFC) << 3) | (b10 >> 3));

            int Y11 = yrow1[x+1];
            unsigned char r11 = CLIP(Y11 + r_add), g11 = CLIP(Y11 + g_add), b11 = CLIP(Y11 + b_add);
            drow1[x+1] = (unsigned short) (((r11 & 0xF8) << 8) | ((g11 & 0xFC) << 3) | (b11 >> 3));
        }
    }
}

// --- GLES2 fullscreen-quad draw (replaces the vitaGL/GLES1 immediate-mode
// path: no glMatrixMode/glOrthof/glVertexPointer in a GLES2-only context).
// Same shader-compile/link idiom as source/utils/glutil.c's DOWNSAMPLE_RENDER
// blit program (glCompileShader_soloader/glLinkProgram_soloader, already
// proven against the real on-device GLSL ES compiler by this project's own
// shaders), just with a video-appropriate quad recomputed per (w,h). ---

// Perf fix (log_106/107/108.txt): yuv420p_to_rgb565's CPU NEON conversion
// measured at 228ms/frame for a 1280x720 cutscene (84% of total elapsed
// time) -- confirmed NOT a Debug-vs-Release artifact (CMakeLists.txt applies
// -O3 unconditionally to both build types; CMAKE_BUILD_TYPE only toggles the
// DEBUG_SOLOADER log macro), so the CPU conversion itself is the genuine
// bottleneck on real hardware. Standard fix for exactly this: skip the CPU
// color conversion entirely and let the GPU's texture sampler + a few ALU
// ops in the fragment shader do it -- upload the Y plane as a GL_LUMINANCE
// texture and the interleaved NV12 UV plane as a GL_LUMINANCE_ALPHA texture
// at half resolution (each (L,A) texel IS one (U,V) byte pair, no repacking
// needed), then compute BT.601 RGB per-pixel in the fragment shader using
// the exact same coefficients (91881/116130/22554/46802, all /65536) the CPU
// path already used -- same math, same visual output, just running on the
// GPU's fixed-function texture units instead of NEON. Bonus: sampling the
// half-res UV texture with GL_LINEAR gives smooth bilinear chroma upsampling
// for free, an improvement over the CPU path's nearest-block replication.
// Gated behind a compile-time flag (not a runtime check) so a hardware
// regression (wrong colors, black screen) has a one-line revert instead of
// needing another round trip to re-add the proven CPU path.
#define VIDEO_GPU_YUV_CONVERT 1

static GLuint gVideoProgram = 0;
static GLint gVideoPosLoc = -1;
static GLint gVideoUvLoc = -1;
#if VIDEO_GPU_YUV_CONVERT
static GLint gVideoYTexLoc = -1;
static GLint gVideoUVTexLoc = -1;
static GLuint gVideoYTex = 0;
static GLuint gVideoUVTex = 0;
#else
static GLint gVideoTexLoc = -1;
static GLuint gVideoTex = 0;
#endif
static unsigned gVideoTexW = 0;
static unsigned gVideoTexH = 0;

static void ensure_video_program() {
    if (gVideoProgram) return;

    static const char *kVideoVS =
        "attribute vec2 aPos;\n"
        "attribute vec2 aUV;\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "    vUV = aUV;\n"
        "}\n";
#if VIDEO_GPU_YUV_CONVERT
    static const char *kVideoFS =
        "precision mediump float;\n"
        "varying vec2 vUV;\n"
        "uniform sampler2D uYTex;\n"
        "uniform sampler2D uUVTex;\n"
        "void main() {\n"
        "    float y = texture2D(uYTex, vUV).r;\n"
        "    vec2 uv = texture2D(uUVTex, vUV).ra - vec2(0.5, 0.5);\n"
        "    float r = y + 1.401993 * uv.y;\n"
        "    float g = y - 0.344136 * uv.x - 0.714136 * uv.y;\n"
        "    float b = y + 1.772000 * uv.x;\n"
        "    gl_FragColor = vec4(r, g, b, 1.0);\n"
        "}\n";
#else
    static const char *kVideoFS =
        "precision mediump float;\n"
        "varying vec2 vUV;\n"
        "uniform sampler2D uTex;\n"
        "void main() {\n"
        "    gl_FragColor = vec4(texture2D(uTex, vUV).rgb, 1.0);\n"
        "}\n";
#endif

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &kVideoVS, NULL);
    glCompileShader_soloader(vs);

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &kVideoFS, NULL);
    glCompileShader_soloader(fs);

    gVideoProgram = glCreateProgram();
    glAttachShader(gVideoProgram, vs);
    glAttachShader(gVideoProgram, fs);
    glBindAttribLocation(gVideoProgram, 0, "aPos");
    glBindAttribLocation(gVideoProgram, 1, "aUV");
    glLinkProgram_soloader(gVideoProgram);

    glDeleteShader(vs);
    glDeleteShader(fs);

    gVideoPosLoc = 0;
    gVideoUvLoc = 1;
#if VIDEO_GPU_YUV_CONVERT
    gVideoYTexLoc = glGetUniformLocation(gVideoProgram, "uYTex");
    gVideoUVTexLoc = glGetUniformLocation(gVideoProgram, "uUVTex");
    l_info("video: GLES2 YUV program ready (program=%u posLoc=%d uvLoc=%d yTexLoc=%d uvTexLoc=%d)",
           gVideoProgram, gVideoPosLoc, gVideoUvLoc, gVideoYTexLoc, gVideoUVTexLoc);
#else
    gVideoTexLoc = glGetUniformLocation(gVideoProgram, "uTex");
    l_info("video: GLES2 program ready (program=%u posLoc=%d uvLoc=%d texLoc=%d)",
           gVideoProgram, gVideoPosLoc, gVideoUvLoc, gVideoTexLoc);
#endif
}

#define REAL_SCREEN_W 960
#define REAL_SCREEN_H 544

static bool gFirstDrawLogged = false;
#define FIRST_DRAW_LOG(...) do { if (!gFirstDrawLogged) l_info(__VA_ARGS__); } while (0)

#if VIDEO_GPU_YUV_CONVERT
// Perf follow-up (log_109.txt): the GPU YUV conversion already took the
// intro from 2.6 to 13.9 avg fps (45.6ms/frame total, down from 318ms), and
// the user asked to push toward 25fps (needs ~40ms/frame). Rather than
// guess at which of texture-upload/glDrawArrays/gl_swap (vsync wait) is the
// remaining cost, split all three explicitly -- accumulated here and read
// by video_play()'s per-loop summary line, same pattern as
// convert_us_total/draw_us_total already use.
uint64_t gVideoUploadUsTotal = 0;
uint64_t gVideoGlDrawUsTotal = 0;
uint64_t gVideoSwapUsTotal = 0;
// log_125.txt: tex_upload stayed the dominant cost again (39.2ms/frame, ~75%
// of the whole convert+draw budget) with no visibility into whether the
// 4x-larger Y plane or the UV plane is actually responsible -- split the two
// glTexSubImage2D calls' timing separately so the next log can confirm
// whether this is proportional to bytes uploaded (bandwidth/twiddle-bound,
// making a downsampled-texture experiment worthwhile) or dominated by fixed
// per-call overhead instead (which downsampling wouldn't fix).
uint64_t gVideoUploadYUsTotal = 0;
uint64_t gVideoUploadUVUsTotal = 0;

// log_126.txt confirmed the hypothesis above with real numbers: Y=24.2ms/frame
// vs UV=11.5ms/frame is a 2.10x ratio, matching the 2.00x ratio of raw bytes
// uploaded (1280x720 Y vs 640x360 UV) almost exactly -- this cost is
// bandwidth/twiddle-bound, not fixed per-call driver overhead, so uploading
// fewer bytes should give a near-linear win. The display already downscales
// this 1280x720 source into a <=960x544 letterboxed quad (see REAL_SCREEN_W/H
// below), so real detail is being thrown away at draw time regardless --
// halving the uploaded resolution loses comparatively little of what's
// actually visible. Downsamples via simple nearest-neighbor decimation (pick
// every other sample, no averaging) rather than a box filter: this is a
// cutscene on a CPU-bound port (Phase 14), and a strided copy is the cheapest
// possible way to shrink the upload without adding meaningful CPU cost back.
// One-line revert: set to 0.
#define VIDEO_DOWNSAMPLE_UPLOAD 1

#if VIDEO_DOWNSAMPLE_UPLOAD
static unsigned char *gVideoDsY = NULL;
static unsigned char *gVideoDsUV = NULL;
static unsigned gVideoDsYCap = 0, gVideoDsUVCap = 0;

// src is a plain 8-bit luminance plane (srcW*srcH bytes) -- picks one sample
// per 2x2 block (top-left) into a (srcW/2)x(srcH/2) destination.
static void downsample2x_luminance(const unsigned char *src, unsigned srcW,
                                    unsigned char *dst, unsigned dstW, unsigned dstH) {
    for (unsigned y = 0; y < dstH; y++) {
        const unsigned char *srow = src + (size_t) (y * 2) * srcW;
        unsigned char *drow = dst + (size_t) y * dstW;
        for (unsigned x = 0; x < dstW; x++) {
            drow[x] = srow[x * 2];
        }
    }
}

// src is interleaved (L,A) byte pairs (NV12's (U,V) read as GL_LUMINANCE_ALPHA,
// srcW*srcH pairs) -- same 2x2-block top-left pick, applied per-pair so both
// U and V come from the same source texel.
static void downsample2x_luminance_alpha(const unsigned char *src, unsigned srcW,
                                          unsigned char *dst, unsigned dstW, unsigned dstH) {
    for (unsigned y = 0; y < dstH; y++) {
        const unsigned char *srow = src + (size_t) (y * 2) * srcW * 2;
        unsigned char *drow = dst + (size_t) y * dstW * 2;
        for (unsigned x = 0; x < dstW; x++) {
            drow[x * 2] = srow[x * 4];
            drow[x * 2 + 1] = srow[x * 4 + 1];
        }
    }
}
#endif
#endif

#if VIDEO_GPU_YUV_CONVERT
// yuvData layout matches gYuvScratch: w*h Y-plane bytes followed by the NV12
// interleaved UV plane (w*h/2 bytes, (w/2)x(h/2) U/V byte pairs) -- exactly
// what av_alloc_texture/the AVC decoder already produces, so this uploads it
// straight through with no CPU-side repacking at all.
static void draw_video_frame(const unsigned char *yuvData, unsigned w, unsigned h) {
    FIRST_DRAW_LOG("video: draw_video_frame ENTER (%ux%u, GPU YUV convert)", w, h);
    ensure_video_program();
    FIRST_DRAW_LOG("video: ensure_video_program() returned");

    const unsigned char *yPlane = yuvData;
    const unsigned char *uvPlane = yuvData + (size_t) w * h;
    unsigned uvW = w / 2, uvH = h / 2;

#if VIDEO_DOWNSAMPLE_UPLOAD
    // Only downsample when the source is big enough to halve cleanly -- a
    // cutscene's resolution is fixed for its whole playback, so this either
    // never triggers (tiny/odd source) or always does, no per-frame flapping.
    int canDownsample = (w >= 4 && h >= 4 && uvW >= 2 && uvH >= 2);
    unsigned texW = canDownsample ? w / 2 : w;
    unsigned texH = canDownsample ? h / 2 : h;
    unsigned uvTexW = canDownsample ? uvW / 2 : uvW;
    unsigned uvTexH = canDownsample ? uvH / 2 : uvH;
#else
    unsigned texW = w, texH = h, uvTexW = uvW, uvTexH = uvH;
#endif

    if (!gVideoYTex || gVideoTexW != texW || gVideoTexH != texH) {
        FIRST_DRAW_LOG("video: creating Y/UV texture storage...");
        if (!gVideoYTex) glGenTextures(1, &gVideoYTex);
        if (!gVideoUVTex) glGenTextures(1, &gVideoUVTex);

        glBindTexture(GL_TEXTURE_2D, gVideoYTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, (GLsizei) texW, (GLsizei) texH, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
        FIRST_DRAW_LOG("video: Y glTexImage2D returned (err=0x%04x)", glGetError());

        glBindTexture(GL_TEXTURE_2D, gVideoUVTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, (GLsizei) uvTexW, (GLsizei) uvTexH, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, NULL);
        FIRST_DRAW_LOG("video: UV glTexImage2D returned (err=0x%04x)", glGetError());

        gVideoTexW = texW;
        gVideoTexH = texH;

#if VIDEO_DOWNSAMPLE_UPLOAD
        unsigned needY = texW * texH;
        unsigned needUV = uvTexW * uvTexH * 2;
        if (needY > gVideoDsYCap) {
            free(gVideoDsY);
            gVideoDsY = (unsigned char *) malloc(needY);
            gVideoDsYCap = gVideoDsY ? needY : 0;
        }
        if (needUV > gVideoDsUVCap) {
            free(gVideoDsUV);
            gVideoDsUV = (unsigned char *) malloc(needUV);
            gVideoDsUVCap = gVideoDsUV ? needUV : 0;
        }
#endif
    }

    uint64_t uploadStart = sceKernelGetProcessTimeWide();
#if VIDEO_DOWNSAMPLE_UPLOAD
    const unsigned char *yUpload = yPlane;
    const unsigned char *uvUpload = uvPlane;
    if (canDownsample && gVideoDsY && gVideoDsUV) {
        downsample2x_luminance(yPlane, w, gVideoDsY, texW, texH);
        downsample2x_luminance_alpha(uvPlane, uvW, gVideoDsUV, uvTexW, uvTexH);
        yUpload = gVideoDsY;
        uvUpload = gVideoDsUV;
    }
    glBindTexture(GL_TEXTURE_2D, gVideoYTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei) texW, (GLsizei) texH, GL_LUMINANCE, GL_UNSIGNED_BYTE, yUpload);
    FIRST_DRAW_LOG("video: Y glTexSubImage2D returned (err=0x%04x)", glGetError());
    uint64_t yDoneTime = sceKernelGetProcessTimeWide();
    gVideoUploadYUsTotal += yDoneTime - uploadStart;
    glBindTexture(GL_TEXTURE_2D, gVideoUVTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei) uvTexW, (GLsizei) uvTexH, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uvUpload);
    FIRST_DRAW_LOG("video: UV glTexSubImage2D returned (err=0x%04x)", glGetError());
#else
    glBindTexture(GL_TEXTURE_2D, gVideoYTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei) w, (GLsizei) h, GL_LUMINANCE, GL_UNSIGNED_BYTE, yPlane);
    FIRST_DRAW_LOG("video: Y glTexSubImage2D returned (err=0x%04x)", glGetError());
    uint64_t yDoneTime = sceKernelGetProcessTimeWide();
    gVideoUploadYUsTotal += yDoneTime - uploadStart;
    glBindTexture(GL_TEXTURE_2D, gVideoUVTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei) uvW, (GLsizei) uvH, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uvPlane);
    FIRST_DRAW_LOG("video: UV glTexSubImage2D returned (err=0x%04x)", glGetError());
#endif
    gVideoUploadUVUsTotal += sceKernelGetProcessTimeWide() - yDoneTime;
    gVideoUploadUsTotal += sceKernelGetProcessTimeWide() - uploadStart;
#else
static void draw_video_frame(const unsigned short *rgb565, unsigned w, unsigned h) {
    // Fine-grained, first-frame-only tracing: this is new code (no vitaGL
    // equivalent proven on this driver yet), so if it ever hangs again the
    // next log needs to show exactly which GL call never returned instead of
    // just going silent after "GLES2 program ready".
    FIRST_DRAW_LOG("video: draw_video_frame ENTER (%ux%u)", w, h);
    ensure_video_program();
    FIRST_DRAW_LOG("video: ensure_video_program() returned");

    // Allocate the texture's storage once per (w,h) -- a cutscene's
    // resolution never changes mid-playback, and reallocating storage every
    // frame (glTexImage2D) is a known stall source on embedded GL drivers.
    if (!gVideoTex || gVideoTexW != w || gVideoTexH != h) {
        FIRST_DRAW_LOG("video: creating texture storage...");
        if (!gVideoTex) glGenTextures(1, &gVideoTex);
        glBindTexture(GL_TEXTURE_2D, gVideoTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        FIRST_DRAW_LOG("video: about to glTexImage2D (%ux%u, RGB565)...", w, h);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (GLsizei) w, (GLsizei) h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
        FIRST_DRAW_LOG("video: glTexImage2D returned (err=0x%04x)", glGetError());
        gVideoTexW = w;
        gVideoTexH = h;
    }
    glBindTexture(GL_TEXTURE_2D, gVideoTex);
    FIRST_DRAW_LOG("video: about to glTexSubImage2D...");
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei) w, (GLsizei) h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, rgb565);
    FIRST_DRAW_LOG("video: glTexSubImage2D returned (err=0x%04x)", glGetError());
#endif

    // Letterbox against the REAL physical 960x544 screen -- on real Android
    // the cutscene plays in its own separate fullscreen Activity/View, with
    // no relationship to this engine's internal 960x640 logical canvas (see
    // glutil.c's compute_letterbox_rect), so this deliberately does NOT use
    // that same letterbox.
    float srcAspect = (float) w / (float) h;
    float dstAspect = (float) REAL_SCREEN_W / (float) REAL_SCREEN_H;
    float qx0 = 0, qy0 = 0, qx1 = REAL_SCREEN_W, qy1 = REAL_SCREEN_H;
    if (srcAspect > dstAspect) {
        float qh = REAL_SCREEN_W / srcAspect;
        qy0 = (REAL_SCREEN_H - qh) / 2.0f;
        qy1 = qy0 + qh;
    } else {
        float qw = REAL_SCREEN_H * srcAspect;
        qx0 = (REAL_SCREEN_W - qw) / 2.0f;
        qx1 = qx0 + qw;
    }

    // Pixel space (y=0 top) -> NDC (y=+1 top): x' = x/W*2-1, y' = 1 - y/H*2.
    float nx0 = qx0 / REAL_SCREEN_W * 2.0f - 1.0f;
    float nx1 = qx1 / REAL_SCREEN_W * 2.0f - 1.0f;
    float ny0 = 1.0f - qy0 / REAL_SCREEN_H * 2.0f; // top edge
    float ny1 = 1.0f - qy1 / REAL_SCREEN_H * 2.0f; // bottom edge

    // Triangle strip: top-left, top-right, bottom-left, bottom-right.
    // UV (0,0) at top-left matches row 0 of the CPU buffer being the
    // frame's top scanline (yuv420p_to_rgb565 writes top-to-bottom).
    const GLfloat verts[8] = {
        nx0, ny0,  nx1, ny0,  nx0, ny1,  nx1, ny1,
    };
    const GLfloat uvs[8] = {
        0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f,  1.0f, 1.0f,
    };

    FIRST_DRAW_LOG("video: about to save GL state...");
    GLboolean savedBlend = glIsEnabled(GL_BLEND);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean savedScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean savedCull = glIsEnabled(GL_CULL_FACE);
    GLint savedViewport[4];
    glGetIntegerv(GL_VIEWPORT, savedViewport);
    GLint savedProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);
#if VIDEO_GPU_YUV_CONVERT
    // Two sampler units now (Y on unit 0, UV on unit 1) -- DH2's own
    // alpha_map(AL/AT) shader variant already uses BOTH texture units for
    // its own diffuse+alpha-mask sampling (see glutil.c's
    // [gl_diag_alphamap] diagnostic), so a LATER cutscene mid-game can very
    // plausibly find unit 1 already bound to something real. Save/restore
    // both units' bindings (and the active-unit pointer itself) explicitly,
    // same rigor as glutil.c's own alphamap probe uses for this exact
    // situation -- anything less risks the same class of "silently wrong
    // texture bound" bug the GL_ARRAY_BUFFER fix above already closed once.
    GLint savedActiveTexture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    GLint savedTex0 = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex0);
    glActiveTexture(GL_TEXTURE1);
    GLint savedTex1 = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex1);
    glActiveTexture((GLenum) savedActiveTexture);
#else
    GLint savedTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex);
#endif
    // DH2's own 3D rendering is real VBO-based GLES2 (unlike the GLES1
    // fixed-function immediate-mode path this file was ported from), so by
    // the time a LATER cutscene plays mid-game, GL_ARRAY_BUFFER is very
    // likely left bound to one of the engine's own vertex buffers. If it is,
    // glVertexAttribPointer's last argument stops meaning "a client-side
    // pointer to `verts`/`uvs`" and instead means "a byte offset into
    // whatever buffer is bound" -- a classic GLES2 footgun that produces
    // garbage/degenerate geometry (or nothing visible) with zero GL error,
    // exactly the "everything reports success, still nothing draws" shape of
    // this bug. Unbinding it here costs nothing on the intro video (nothing
    // has bound a buffer yet at boot) and closes off this failure mode for
    // every subsequent cutscene.
    GLint savedArrayBuffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &savedArrayBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
#if VIDEO_GPU_YUV_CONVERT
    FIRST_DRAW_LOG("video: GL state saved (blend=%d depth=%d scissor=%d cull=%d program=%d tex0=%d tex1=%d array_buffer=%d)",
                   savedBlend, savedDepthTest, savedScissor, savedCull, savedProgram, savedTex0, savedTex1, savedArrayBuffer);
#else
    FIRST_DRAW_LOG("video: GL state saved (blend=%d depth=%d scissor=%d cull=%d program=%d tex=%d array_buffer=%d)",
                   savedBlend, savedDepthTest, savedScissor, savedCull, savedProgram, savedTex, savedArrayBuffer);
#endif

    glViewport(0, 0, REAL_SCREEN_W, REAL_SCREEN_H);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    FIRST_DRAW_LOG("video: viewport/disables done");

    glUseProgram(gVideoProgram);
#if VIDEO_GPU_YUV_CONVERT
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gVideoYTex);
    glUniform1i(gVideoYTexLoc, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, gVideoUVTex);
    glUniform1i(gVideoUVTexLoc, 1);
#else
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gVideoTex);
    glUniform1i(gVideoTexLoc, 0);
#endif
    FIRST_DRAW_LOG("video: program/texture bound");

    glVertexAttribPointer(gVideoPosLoc, 2, GL_FLOAT, GL_FALSE, 0, verts);
    glVertexAttribPointer(gVideoUvLoc, 2, GL_FLOAT, GL_FALSE, 0, uvs);
    glEnableVertexAttribArray(gVideoPosLoc);
    glEnableVertexAttribArray(gVideoUvLoc);
    FIRST_DRAW_LOG("video: about to glDrawArrays...");
#if VIDEO_GPU_YUV_CONVERT
    uint64_t glDrawStart = sceKernelGetProcessTimeWide();
#endif
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    FIRST_DRAW_LOG("video: glDrawArrays returned (err=0x%04x)", glGetError());
    glDisableVertexAttribArray(gVideoPosLoc);
    glDisableVertexAttribArray(gVideoUvLoc);
#if VIDEO_GPU_YUV_CONVERT
    gVideoGlDrawUsTotal += sceKernelGetProcessTimeWide() - glDrawStart;
#endif

    // Diagnostic for the "audio plays, screen stays black" report: reads
    // back the actual framebuffer content right after our draw, before
    // anything else touches it or the swap happens. If this is real,
    // varying color data, the draw genuinely put a visible image in the
    // color buffer and the black screen has to come from something AFTER
    // this point (the swap/present path, or a later redraw clobbering it
    // before the display scans it out). If it reads back solid black, the
    // draw/upload itself is the dead end -- combined with the [video_diag]
    // Y-plane scan above (which tells the DECODE side), between the two
    // this settles which half of the pipeline is actually at fault instead
    // of guessing.
    if (!gFirstDrawLogged) {
        unsigned char pixel[4] = {0, 0, 0, 0};
        glReadPixels(REAL_SCREEN_W / 2, REAL_SCREEN_H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        l_info("[video_diag] framebuffer readback at center, right after glDrawArrays: rgba=%u,%u,%u,%u (err=0x%04x)",
               pixel[0], pixel[1], pixel[2], pixel[3], glGetError());
    }

    glBindBuffer(GL_ARRAY_BUFFER, (GLuint) savedArrayBuffer);
#if VIDEO_GPU_YUV_CONVERT
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, (GLuint) savedTex1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint) savedTex0);
    glActiveTexture((GLenum) savedActiveTexture);
#else
    glBindTexture(GL_TEXTURE_2D, (GLuint) savedTex);
#endif
    glUseProgram((GLuint) savedProgram);
    if (savedBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (savedDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (savedScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (savedCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
    FIRST_DRAW_LOG("video: GL state restored, about to gl_swap()...");

#if VIDEO_GPU_YUV_CONVERT
    uint64_t swapStart = sceKernelGetProcessTimeWide();
#endif
    gl_swap();
#if VIDEO_GPU_YUV_CONVERT
    gVideoSwapUsTotal += sceKernelGetProcessTimeWide() - swapStart;
#endif
    FIRST_DRAW_LOG("video: gl_swap() returned -- first frame fully presented");
    gFirstDrawLogged = true;
}

// --- cutscene audio: dedicated output thread, unchanged from the source
// project -- decoupling the blocking sceAudioOutOutput() call from the same
// loop that pays for the YUV conversion above, so a slow video frame can't
// delay the next audio block (and vice versa). Uses only a mutex + short poll
// for the producer/consumer handshake (no pthread_cond_t): this project's own
// pthread port (source/reimpl/pthr.c) is the same lineage the source project
// found crashes a statically-initialized cond var on first use. ---
static pthread_mutex_t gCutAudioLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *gCutAudioBuf[2] = { NULL, NULL };
static unsigned gCutAudioBufCap = 0;
static unsigned gCutAudioLen[2] = { 0, 0 };
static int gCutAudioWriteSlot = 0;
static int gCutAudioPort = -1;
static volatile bool gCutAudioQuit = false;

static int cutscene_audio_thread(SceSize args, void *argp) {
    (void) args; (void) argp;
    int slot = 0;
    for (;;) {
        pthread_mutex_lock(&gCutAudioLock);
        unsigned len = gCutAudioLen[slot];
        bool quit = gCutAudioQuit;
        pthread_mutex_unlock(&gCutAudioLock);

        if (len == 0) {
            if (quit)
                break;
            sceKernelDelayThread(500);
            continue;
        }

        if (gCutAudioPort >= 0)
            sceAudioOutOutput(gCutAudioPort, gCutAudioBuf[slot]);
        pthread_mutex_lock(&gCutAudioLock);
        gCutAudioLen[slot] = 0;
        pthread_mutex_unlock(&gCutAudioLock);
        slot ^= 1;
    }
    return 0;
}

static void cutscene_audio_submit(const void *pData, unsigned bytes) {
    if (bytes > gCutAudioBufCap) {
        free(gCutAudioBuf[0]);
        free(gCutAudioBuf[1]);
        gCutAudioBuf[0] = (unsigned char *) malloc(bytes);
        gCutAudioBuf[1] = (unsigned char *) malloc(bytes);
        gCutAudioBufCap = (gCutAudioBuf[0] && gCutAudioBuf[1]) ? bytes : 0;
    }
    if (!gCutAudioBuf[0] || !gCutAudioBuf[1] || gCutAudioBufCap < bytes)
        return;

    for (;;) {
        pthread_mutex_lock(&gCutAudioLock);
        bool free_slot = gCutAudioLen[gCutAudioWriteSlot] == 0;
        if (free_slot) {
            memcpy(gCutAudioBuf[gCutAudioWriteSlot], pData, bytes);
            gCutAudioLen[gCutAudioWriteSlot] = bytes;
        }
        pthread_mutex_unlock(&gCutAudioLock);
        if (free_slot)
            break;
        sceKernelDelayThread(500);
    }
    gCutAudioWriteSlot ^= 1;
}

void video_init() {
    int ret = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
    if (ret < 0) {
        l_error("video: sceSysmoduleLoadModule(AVPLAYER) failed (0x%08X) -- cutscenes will be skipped", (unsigned) ret);
        gModuleLoaded = false;
        return;
    }
    gModuleLoaded = true;
    l_success("video: SceAvPlayer module loaded.");
}

void video_shutdown() {
#if VIDEO_GPU_YUV_CONVERT
    if (gVideoYTex) {
        glDeleteTextures(1, &gVideoYTex);
        gVideoYTex = 0;
    }
    if (gVideoUVTex) {
        glDeleteTextures(1, &gVideoUVTex);
        gVideoUVTex = 0;
    }
    gVideoTexW = 0;
    gVideoTexH = 0;
#else
    if (gVideoTex) {
        glDeleteTextures(1, &gVideoTex);
        gVideoTex = 0;
        gVideoTexW = 0;
        gVideoTexH = 0;
    }
#endif
    if (gVideoProgram) {
        glDeleteProgram(gVideoProgram);
        gVideoProgram = 0;
    }
    free(gRgbBuf);
    gRgbBuf = NULL;
    gRgbBufCap = 0;
    free(gYuvScratch);
    gYuvScratch = NULL;
    gYuvScratchCap = 0;
    if (gModuleLoaded) {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_AVPLAYER);
        gModuleLoaded = false;
    }
}

void video_play(const char *name) {
    if (!gModuleLoaded) {
        l_warn("video: AVPLAYER module not loaded, skipping cutscene request \"%s\"", name ? name : "(null)");
        return;
    }
    if (!name) {
        l_warn("video: video_play() called with a null name, skipping");
        return;
    }

    // Try DATA_PATH root first: log_095 showed every other asset in this
    // port (shaders.pak, *.savegame, data/...) staged flat under DATA_PATH,
    // not under a "files/" subfolder -- that subfolder only exists on real
    // Android's own app-private storage layout (getSDFolder() + name), which
    // doesn't apply to how this port's own assets got laid out on the memory
    // card. Still try "files/<name>" as a second candidate in case that
    // subfolder does get used for this file specifically.
    char path[512];
    bool found = false;
    SceIoStat st;

    snprintf(path, sizeof(path), DATA_PATH "%s", name);
    if (sceIoGetstat(path, &st) >= 0) {
        found = true;
    } else {
        snprintf(path, sizeof(path), DATA_PATH "files/%s", name);
        if (sceIoGetstat(path, &st) >= 0) {
            found = true;
        }
    }

    if (!found) {
        l_error("video: file not found for \"%s\" (tried %s%s and %sfiles/%s)",
                name, DATA_PATH, name, DATA_PATH, name);
        return;
    }

    SceAvPlayerInitData init;
    memset(&init, 0, sizeof(init));
    init.memoryReplacement.allocate = av_alloc;
    init.memoryReplacement.deallocate = av_free;
    init.memoryReplacement.allocateTexture = av_alloc_texture;
    init.memoryReplacement.deallocateTexture = av_free_texture;
    init.fileReplacement.objectPointer = &gAvFileCtx;
    init.fileReplacement.open = av_file_open;
    init.fileReplacement.close = av_file_close;
    init.fileReplacement.readOffset = av_file_read;
    init.fileReplacement.size = av_file_size;
    init.eventReplacement.objectPointer = NULL;
    init.eventReplacement.eventCallback = av_event_cb;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = 2;
    init.autoStart = SCE_TRUE;
    init.debugLevel = 0;

    SceAvPlayerHandle handle = sceAvPlayerInit(&init);
    if ((unsigned)handle == 0 || (unsigned)handle == 0xFFFFFFFF || ((unsigned)handle & 0xFF000000) == 0x80000000) {
        l_error("video: sceAvPlayerInit failed (0x%08X) for %s", (unsigned) handle, path);
        return;
    }

    if (sceAvPlayerAddSource(handle, path) < 0) {
        l_error("video: sceAvPlayerAddSource failed for %s", path);
        sceAvPlayerClose(handle);
        return;
    }

    l_success("video: playing %s", path);

    int audioPort = -1;
    int audioChannels = 0;
    unsigned audioFrameLen = 0;
    SceUID cutAudioThreadUid = -1;

    bool skipped = false;

    int wait_count = 0;
    while (!sceAvPlayerIsActive(handle) && wait_count < 500) {
        sceKernelDelayThread(10000); // 10ms
        wait_count++;
    }

    // Sample the pad AFTER the activation wait (which can take up to 5s),
    // not before it -- otherwise a button already released/re-pressed by the
    // user during that wait (e.g. finishing a tap on the previous screen)
    // reads as a fresh edge the instant this loop starts, producing an
    // instantaneous false "skip" that looks identical to a real user skip
    // in the log (see the [video_diag] investigation: log_101.txt showed
    // exactly this -- a skip 0.10s after "loop starting" that the user says
    // did not happen on their end).
    SceCtrlData pad_start;
    sceCtrlPeekBufferPositive(0, &pad_start, 1);
    uint32_t old_pad_buttons = pad_start.buttons;

    l_info("video: loop starting. active=%d, wait_count=%d, initial_pad_buttons=0x%08X",
           sceAvPlayerIsActive(handle), wait_count, (unsigned) old_pad_buttons);
    uint64_t play_start_time = sceKernelGetProcessTimeWide();

    int frame_count = 0;
    int video_frames = 0, audio_frames = 0;
    bool audioOpenAttempted = false;

    // Perf investigation (user-reported, log_104/105.txt: intro now decodes
    // and draws real frames, confirmed on hardware, but only at ~2.6-2.7 avg
    // fps for a 1280x720 cutscene). "loop exited" only ever reported the
    // OUTCOME (video_frames over elapsed_sec) with no way to tell whether our
    // own CPU work (NV12->RGB565 NEON conversion + glTexSubImage2D upload +
    // draw + gl_swap) is the bottleneck vs. the hardware AVC decoder itself
    // simply not producing frames faster than this for a 720p source --
    // those need completely different fixes (optimize our code vs. nothing
    // to optimize here at all). Timed separately per successful video frame,
    // summed, and reported as a fraction of total elapsed time in the final
    // summary line -- e.g. "convert+draw=3.1s (14% of 21.1s)" means the
    // decoder itself, not our path, owns the other 86%.
    //
    // log_106.txt answered that question: convert+draw was 84% of elapsed
    // (318.8ms/frame) -- so it's OUR code, not the hardware decoder, eating
    // the time. Split the single bucket in two (yuv420p_to_rgb565's NEON
    // CPU work vs. draw_video_frame's texture upload+GL draw+gl_swap) so the
    // next log says which half is actually slow instead of guessing at an
    // optimization blind.
    uint64_t convert_us_total = 0;
    uint64_t draw_us_total = 0;
#if VIDEO_GPU_YUV_CONVERT
    // draw_video_frame() accumulates into these file-scope globals (it has
    // no other channel back to this function's summary line) -- reset here
    // since a second video_play() call in the same process run (a later
    // cutscene) must not carry over the previous one's totals.
    gVideoUploadUsTotal = 0;
    gVideoGlDrawUsTotal = 0;
    gVideoSwapUsTotal = 0;
    gVideoUploadYUsTotal = 0;
    gVideoUploadUVUsTotal = 0;
#endif

    if (!sceAvPlayerIsActive(handle)) {
        l_warn("video: timed out waiting for video decoder to become active (%s)", path);
    }

    while (sceAvPlayerIsActive(handle)) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t pressed = pad.buttons & ~old_pad_buttons;

        if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_START)) {
            l_info("video: skipped by user button press! (pad=0x%08X old_pad=0x%08X pressed=0x%08X, %d loop iteration(s) in)",
                   (unsigned) pad.buttons, (unsigned) old_pad_buttons, (unsigned) pressed, frame_count);
            skipped = true;
            break;
        }
        old_pad_buttons = pad.buttons;

        SceAvPlayerFrameInfo video;
        if (sceAvPlayerGetVideoData(handle, &video)) {
            unsigned w = video.details.video.width;
            unsigned h = video.details.video.height;
            if (++video_frames == 1)
                l_info("video: first video frame decoded (%ux%u, pData=%p)", w, h, video.pData);
#if !VIDEO_GPU_YUV_CONVERT
            unsigned need = w * h * sizeof(unsigned short);
            if (need > gRgbBufCap) {
                free(gRgbBuf);
                gRgbBuf = (unsigned short *) malloc(need);
                gRgbBufCap = gRgbBuf ? need : 0;
            }
#endif
            unsigned yuvNeed = w * h + w * h / 2; // NV12: Y plane + half-res interleaved UV
            if (yuvNeed > gYuvScratchCap) {
                free(gYuvScratch);
                gYuvScratch = (unsigned char *) malloc(yuvNeed);
                gYuvScratchCap = gYuvScratch ? yuvNeed : 0;
            }
#if VIDEO_GPU_YUV_CONVERT
            if (gYuvScratch && gYuvScratchCap >= yuvNeed) {
#else
            if (gRgbBuf && gRgbBufCap >= need && gYuvScratch && gYuvScratchCap >= yuvNeed) {
#endif
                // Drain the CDRAM/PHYCONT source with one sequential memcpy
                // before the per-pixel conversion math (which reads it many
                // times over) -- CPU reads from that memory are far slower
                // than RAM on this hardware.
                memcpy(gYuvScratch, video.pData, yuvNeed);

                // Diagnostic for the "audio plays, screen stays black"
                // report: the NV12->RGB565 conversion and GL upload/draw
                // path below is ported verbatim from the Prince of Persia
                // Vita port (proven working on real hardware there), so if
                // the screen is still black despite that, the most likely
                // divergence is upstream of all of it -- the CDRAM/PHYCONT
                // memblock av_alloc_texture hands the hardware AVC decoder
                // (source/video.cpp above) never actually getting written
                // with real decoded pixels for this project's specific
                // driver/init sequence (PVR_PSP2, not vitaGL). An
                // all-zero Y plane converts to solid black through this
                // exact math (Y=0 with U=V=128-bias-zero clips to (0,0,0)),
                // which would look identical to a real GL/upload bug but
                // has a completely different fix. Scanning only once
                // (first video frame of the whole cutscene) is enough to
                // answer the question either way.
                if (video_frames == 1) {
                    unsigned char y_min = 255, y_max = 0;
                    bool y_all_same = true;
                    unsigned char first_y = gYuvScratch[0];
                    for (unsigned i = 0; i < (unsigned) w * h; i++) {
                        unsigned char v = gYuvScratch[i];
                        if (v < y_min) y_min = v;
                        if (v > y_max) y_max = v;
                        if (v != first_y) y_all_same = false;
                    }
                    const unsigned char *uv = gYuvScratch + (size_t) w * h;
                    l_info("[video_diag] first frame Y-plane: min=%u max=%u all_same=%d (first_byte=%u), UV[0..3]=%u,%u,%u,%u%s",
                           y_min, y_max, (int) y_all_same, first_y, uv[0], uv[1], uv[2], uv[3],
                           (y_all_same && first_y == 0) ? " <-- DECODER NEVER WROTE REAL DATA (all-zero Y plane)" : "");
                }

                uint64_t t0 = sceKernelGetProcessTimeWide();
#if VIDEO_GPU_YUV_CONVERT
                // No CPU color conversion step anymore -- draw_video_frame
                // uploads the Y/UV planes as-is and the fragment shader does
                // the YUV->RGB math. convert_us_total stays 0 so the summary
                // line makes the shift obvious at a glance.
                draw_video_frame(gYuvScratch, w, h);
                uint64_t t2 = sceKernelGetProcessTimeWide();
                draw_us_total += t2 - t0;
#else
                yuv420p_to_rgb565(gYuvScratch, w, h, gRgbBuf);
                uint64_t t1 = sceKernelGetProcessTimeWide();
                draw_video_frame(gRgbBuf, w, h);
                uint64_t t2 = sceKernelGetProcessTimeWide();
                convert_us_total += t1 - t0;
                draw_us_total += t2 - t1;
#endif
            }
        }

        SceAvPlayerFrameInfo audio;
        if (sceAvPlayerGetAudioData(handle, &audio)) {
            if (++audio_frames == 1)
                l_info("video: first audio frame decoded (ch=%u rate=%u)",
                       (unsigned) audio.details.audio.channelCount,
                       (unsigned) audio.details.audio.sampleRate);
            if (audioPort < 0 && !audioOpenAttempted) {
                audioOpenAttempted = true;
                audioChannels = audio.details.audio.channelCount;
                SceAudioOutMode mode = (audioChannels >= 2) ? SCE_AUDIO_OUT_MODE_STEREO : SCE_AUDIO_OUT_MODE_MONO;
                audioFrameLen = audio.details.audio.size / (audioChannels * sizeof(int16_t));
                l_info("video: cutscene audio port: %u frames/channel (size=%u bytes, ch=%u)",
                       audioFrameLen, (unsigned) audio.details.audio.size, audioChannels);
                // VOICE, not MAIN: MAIN requires exactly 48000Hz; VOICE has
                // no such restriction and is a distinct port type from
                // whatever this project's own (not yet implemented) BGM/SFX
                // mixer will use later, so it can't collide with it either.
                audioPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_VOICE, audioFrameLen,
                                                (int) audio.details.audio.sampleRate, mode);
                if (audioPort < 0) {
                    l_warn("video: sceAudioOutOpenPort for cutscene audio failed (0x%08X) -- cutscene audio disabled",
                           (unsigned) audioPort);
                } else {
                    gCutAudioPort = audioPort;
                    gCutAudioWriteSlot = 0;
                    gCutAudioLen[0] = 0;
                    gCutAudioLen[1] = 0;
                    gCutAudioQuit = false;
                    cutAudioThreadUid = sceKernelCreateThread("cutscene audio out", cutscene_audio_thread,
                                                               0x10000100, 0x4000, 0, 0, NULL);
                    if (cutAudioThreadUid >= 0) {
                        sceKernelStartThread(cutAudioThreadUid, 0, NULL);
                    } else {
                        l_warn("video: cutscene audio thread creation failed (0x%08X) -- cutscene audio disabled",
                               (unsigned) cutAudioThreadUid);
                        sceAudioOutReleasePort(audioPort);
                        audioPort = -1;
                        gCutAudioPort = -1;
                    }
                }
            }
            if (audioPort >= 0) {
                cutscene_audio_submit(audio.pData, (unsigned) audio.details.audio.size);
            }
        }

        frame_count++;
        if (frame_count == 1) {
            l_info("video: successfully completed first loop iteration!");
        }

        sceKernelDelayThread(1000); // avoid a tight spin when neither frame type is ready yet
    }

    uint64_t play_end_time = sceKernelGetProcessTimeWide();
    double elapsed_sec = (double) (play_end_time - play_start_time) / 1000000.0;
    double avg_fps = elapsed_sec > 0.0 ? (double) video_frames / elapsed_sec : 0.0;
    double convert_sec = (double) convert_us_total / 1000000.0;
    double draw_sec = (double) draw_us_total / 1000000.0;
    double convert_draw_sec = convert_sec + draw_sec;
    double convert_draw_pct = elapsed_sec > 0.0 ? (convert_draw_sec / elapsed_sec) * 100.0 : 0.0;
    double convert_ms_per_frame = video_frames > 0 ? (convert_sec * 1000.0) / video_frames : 0.0;
    double draw_ms_per_frame = video_frames > 0 ? (draw_sec * 1000.0) / video_frames : 0.0;
#if VIDEO_GPU_YUV_CONVERT
    double upload_ms_per_frame = video_frames > 0 ? ((double) gVideoUploadUsTotal / 1000.0) / video_frames : 0.0;
    double uploadY_ms_per_frame = video_frames > 0 ? ((double) gVideoUploadYUsTotal / 1000.0) / video_frames : 0.0;
    double uploadUV_ms_per_frame = video_frames > 0 ? ((double) gVideoUploadUVUsTotal / 1000.0) / video_frames : 0.0;
    double gldraw_ms_per_frame = video_frames > 0 ? ((double) gVideoGlDrawUsTotal / 1000.0) / video_frames : 0.0;
    double swap_ms_per_frame = video_frames > 0 ? ((double) gVideoSwapUsTotal / 1000.0) / video_frames : 0.0;
    l_info("video: loop exited! active=%d, iterations=%d, video_frames=%d, audio_frames=%d, elapsed=%.2fs, avg_fps=%.1f, "
           "convert+draw=%.2fs (%.0f%% of elapsed) [yuv_convert=%.1fms/frame, tex_upload=%.1fms/frame "
           "(Y=%.1fms/frame UV=%.1fms/frame), glDrawArrays=%.1fms/frame, gl_swap(vsync)=%.1fms/frame]",
           sceAvPlayerIsActive(handle), frame_count, video_frames, audio_frames, elapsed_sec, avg_fps,
           convert_draw_sec, convert_draw_pct, convert_ms_per_frame, upload_ms_per_frame,
           uploadY_ms_per_frame, uploadUV_ms_per_frame, gldraw_ms_per_frame, swap_ms_per_frame);
#else
    l_info("video: loop exited! active=%d, iterations=%d, video_frames=%d, audio_frames=%d, elapsed=%.2fs, avg_fps=%.1f, "
           "convert+draw=%.2fs (%.0f%% of elapsed) [yuv_convert=%.1fms/frame, tex_upload+gl_draw+swap=%.1fms/frame]",
           sceAvPlayerIsActive(handle), frame_count, video_frames, audio_frames, elapsed_sec, avg_fps,
           convert_draw_sec, convert_draw_pct, convert_ms_per_frame, draw_ms_per_frame);
#endif

    if (cutAudioThreadUid >= 0) {
        pthread_mutex_lock(&gCutAudioLock);
        gCutAudioQuit = true;
        pthread_mutex_unlock(&gCutAudioLock);
        sceKernelWaitThreadEnd(cutAudioThreadUid, NULL, NULL);
        sceKernelDeleteThread(cutAudioThreadUid);
    }
    gCutAudioPort = -1;

    if (audioPort >= 0)
        sceAudioOutReleasePort(audioPort);

    sceAvPlayerStop(handle);
    sceAvPlayerClose(handle);

    l_success("video: %s (%s)", skipped ? "skipped" : "finished", path);
}
