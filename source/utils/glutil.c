#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/dialog.h"
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <gpu_es4/psp2_pvr_hint.h>

static EGLDisplay display;
static EGLSurface surface;
static EGLContext context;

static int s_frame_counter = 0;
static unsigned int s_draw_calls_since_diag = 0;

// "Enemigos invisibles" investigation: the periodic single-point-in-time
// snapshot in gl_log_render_diag only ever caught 2 texture IDs (910013,
// 560008) across the whole of log_082's combat section, even long after the
// troll's assets loaded successfully -- but a single sample per ~60 frames
// can easily always land between an enemy's draw calls and miss its texture
// entirely. This tracks every DISTINCT texture bound across an entire diag
// window instead of one instant, so a third ID either shows up somewhere in
// that window or genuinely never does.
//
// log_084 hit exactly 16/16 in every sampled window during real troll combat
// -- meaning player/UI/environment textures alone already fill the old cap,
// and track_seen_texture() silently stops recording once full (see below).
// That makes the earlier "16 distinct textures, consistent with the enemy
// being drawn" conclusion UNVERIFIED: we can't tell whether a 17th+ ID (the
// troll's own) was ever reached or was silently dropped. Raised the cap so a
// real answer is possible, and the cap-hit path below now says so explicitly
// instead of just under-reporting a stale-looking number.
#define MAX_TRACKED_TEXTURES 64
static GLuint s_seen_textures[MAX_TRACKED_TEXTURES];
// Program bound at the moment each s_seen_textures[i] was first seen this
// window -- log_088's [gl_shader_variant] lines identified specific programs
// using the shared shader template's AL/AT (separate AlphaSampler) variant,
// but gl_diag3's once-per-60-frames program=%d sample never once landed on
// one of them across the whole log, so it couldn't say whether an enemy's
// own texture is ever drawn by an alpha_map=1 program. Recording the program
// alongside each distinct texture (same per-draw hook, no extra sampling
// cadence) answers that directly instead of hoping a periodic snapshot gets
// lucky.
static GLint s_seen_texture_programs[MAX_TRACKED_TEXTURES];
static int s_seen_textures_count = 0;
// True at least once this diag window if a bound texture couldn't be
// recorded because s_seen_textures was already full -- distinguishes "we
// counted every distinct texture and it was exactly N" from "there were more
// than N and we stopped counting", which the old code couldn't tell apart.
static int s_seen_textures_overflowed = 0;

#ifdef DEBUG_SOLOADER
// "Enemigos invisibles" investigation: GL_Diffuse_L1_iPhone_FS/VS.glsl (the
// shared lit-character shader template, see shaders.pak) hardcodes vertex
// alpha to 1.0 and derives final alpha from the diffuse texture's own alpha
// channel UNLESS the engine compiles this template with AL/AT defined, in
// which case a SEPARATE AlphaSampler texture overwrites it instead. If
// monster materials use the AL/AT variant while the player's don't (or vice
// versa), and that second sampler isn't bound to a valid texture for
// whichever entities go through this path, GLES2's "sampling an unbound unit
// returns (0,0,0,0)" behavior would silently zero every affected fragment's
// alpha -- exactly the observed symptom. This tracks, per linked program,
// whether any of its attached shaders' source enabled AL/AT, purely to
// correlate against gl_diag3's periodic program=%d sample and the
// distinct-texture list during real monster combat -- log-only, changes
// nothing about what's actually compiled/linked/bound.
#define MAX_TRACKED_SHADERS 64
#define MAX_TRACKED_PROGRAMS 32
typedef struct { GLuint shader; int alpha_map; } TrackedShader;
static TrackedShader s_tracked_shaders[MAX_TRACKED_SHADERS];
static int s_tracked_shader_count = 0;
typedef struct { GLuint program; int alpha_map; int logged; } TrackedProgram;
static TrackedProgram s_tracked_programs[MAX_TRACKED_PROGRAMS];
static int s_tracked_program_count = 0;

// True if "#define AL" or "#define AT" appears active (not behind a "//"
// comment) anywhere in the source -- the shared template itself keeps these
// same tokens permanently commented out (e.g. "//#define AL") as
// documentation of its own #ifdef surface, so a naive substring search would
// false-positive on every single shader compiled from it. The engine injects
// the REAL, active define (as its own uncommented line, typically combined
// like "AL_LM") ahead of the template body when compiling a specific
// material permutation.
static int source_enables_alpha_map(const char *s) {
    static const char *needles[] = { "#define AL", "#define AT" };
    for (int n = 0; n < 2; n++) {
        const char *p = s;
        while ((p = strstr(p, needles[n])) != NULL) {
            int commented = (p - s >= 2) && p[-1] == '/' && p[-2] == '/';
            if (!commented) return 1;
            p += 1;
        }
    }
    return 0;
}

static void record_shader_variant(GLuint shader, GLsizei count, const GLchar *const *string) {
    if (s_tracked_shader_count >= MAX_TRACKED_SHADERS) return;
    int alpha_map = 0;
    for (GLsizei i = 0; i < count; i++) {
        if (string[i] && source_enables_alpha_map(string[i])) {
            alpha_map = 1;
            break;
        }
    }
    s_tracked_shaders[s_tracked_shader_count].shader = shader;
    s_tracked_shaders[s_tracked_shader_count].alpha_map = alpha_map;
    s_tracked_shader_count++;
}

static int shader_has_alpha_map(GLuint shader) {
    for (int i = 0; i < s_tracked_shader_count; i++) {
        if (s_tracked_shaders[i].shader == shader) {
            return s_tracked_shaders[i].alpha_map;
        }
    }
    return -1; // unknown -- not a shader we saw glShaderSource for
}

static TrackedProgram *find_or_track_program(GLuint program) {
    for (int i = 0; i < s_tracked_program_count; i++) {
        if (s_tracked_programs[i].program == program) {
            return &s_tracked_programs[i];
        }
    }
    if (s_tracked_program_count >= MAX_TRACKED_PROGRAMS) return NULL;
    TrackedProgram *p = &s_tracked_programs[s_tracked_program_count++];
    p->program = program;
    p->alpha_map = 0;
    p->logged = 0;
    return p;
}
#endif // DEBUG_SOLOADER

// Real physical Vita screen -- shared by glViewport_soloader's letterbox
// math and (when DOWNSAMPLE_RENDER is on) the final upscale blit's target
// rect, so both always agree on where the letterboxed content actually is.
#define REAL_SCREEN_W 960
#define REAL_SCREEN_H 544

// Letterboxed sub-rect of the TRUE physical screen (960x544) that the game's
// assumed 960x640 logical canvas maps into, aspect-correct, centered --
// same math as glViewport_soloader, factored out so DOWNSAMPLE_RENDER's
// final blit targets exactly the same rect instead of a second hardcoded
// copy that could drift out of sync.
#define LOGICAL_W 960
#define LOGICAL_H 640

static void compute_letterbox_rect(int *out_x, int *out_y, int *out_w, int *out_h) {
    const float scale = (float) REAL_SCREEN_H / (float) LOGICAL_H;
    const int scaled_w = (int) (LOGICAL_W * scale + 0.5f);
    *out_x = (REAL_SCREEN_W - scaled_w) / 2;
    *out_y = 0;
    *out_w = scaled_w;
    *out_h = REAL_SCREEN_H;
}

int glutil_screen_touch_to_logical(int screen_x, int screen_y, int *out_x, int *out_y) {
    int lb_x, lb_y, lb_w, lb_h;
    compute_letterbox_rect(&lb_x, &lb_y, &lb_w, &lb_h);
    if (screen_x < lb_x || screen_x >= lb_x + lb_w || screen_y < lb_y || screen_y >= lb_y + lb_h) {
        return 0;
    }
    *out_x = (screen_x - lb_x) * LOGICAL_W / lb_w;
    *out_y = (screen_y - lb_y) * LOGICAL_H / lb_h;
    return 1;
}

#ifdef DOWNSAMPLE_RENDER
// Offscreen FBO the whole scene renders into instead of the real default
// framebuffer (see glBindFramebuffer_soloader), sized DS_NUM/DS_DEN of
// native -- gl_swap() upscales it onto the real screen with one GL_LINEAR
// blit before the real eglSwapBuffers. s_ds_fbo == 0 means either not yet
// initialized or initialization failed (FBO incomplete) -- both cases fall
// back to rendering straight to the default framebuffer, same as
// DOWNSAMPLE_RENDER being off.
static GLuint s_ds_fbo = 0;
static GLuint s_ds_color_tex = 0;
static GLuint s_ds_depth_rb = 0;
static GLuint s_blit_program = 0;

// Fullscreen triangle strip, interleaved (x, y, u, v) per vertex -- covers
// the full [-1,1] clip-space quad with matching [0,1] texcoords (no Y-flip
// needed: the FBO's color texture and this blit live in the same GL
// texture-coordinate convention, origin bottom-left, as the render itself).
static const GLfloat kBlitQuad[16] = {
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
};

static GLuint compile_blit_shader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE) {
        GLchar log[512];
        GLsizei len = 0;
        glGetShaderInfoLog(shader, sizeof(log), &len, log);
        l_error("DOWNSAMPLE_RENDER: blit shader compile FAILED: %s", log);
    }
    return shader;
}

static void init_downsample_render(void) {
    int ds_w = REAL_SCREEN_W * DS_NUM / DS_DEN;
    int ds_h = REAL_SCREEN_H * DS_NUM / DS_DEN;

    glGenTextures(1, &s_ds_color_tex);
    glBindTexture(GL_TEXTURE_2D, s_ds_color_tex);
    // GL_RGBA (not GL_RGB): the 3-channel format isn't a universally
    // guaranteed color-renderable format on embedded GLES2 drivers -- RGBA8
    // is the one format every GLES2 implementation is required to support
    // as an FBO color attachment. Using GL_RGB here was a real candidate for
    // why the downsample build "didn't even open" on hardware: an
    // unsupported attachment format can misbehave a lot worse than the
    // graceful glCheckFramebufferStatus-incomplete path below is built to
    // handle, on a driver that isn't fully spec-conformant for the edge case.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ds_w, ds_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &s_ds_depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, s_ds_depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, ds_w, ds_h);

    glGenFramebuffers(1, &s_ds_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_ds_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_ds_color_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_ds_depth_rb);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        l_error("DOWNSAMPLE_RENDER: FBO incomplete (status=0x%04x) -- falling back to native resolution", status);
        glDeleteFramebuffers(1, &s_ds_fbo);
        s_ds_fbo = 0;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    l_success("DOWNSAMPLE_RENDER: FBO %dx%d ready (native %dx%d, ratio %d/%d)",
              ds_w, ds_h, REAL_SCREEN_W, REAL_SCREEN_H, DS_NUM, DS_DEN);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    static const char *kBlitVS =
        "attribute vec2 aPos;\n"
        "attribute vec2 aUV;\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "    vUV = aUV;\n"
        "}\n";
    static const char *kBlitFS =
        "precision mediump float;\n"
        "varying vec2 vUV;\n"
        "uniform sampler2D uTex;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(uTex, vUV);\n"
        "}\n";

    GLuint vs = compile_blit_shader(GL_VERTEX_SHADER, kBlitVS);
    GLuint fs = compile_blit_shader(GL_FRAGMENT_SHADER, kBlitFS);
    s_blit_program = glCreateProgram();
    glAttachShader(s_blit_program, vs);
    glAttachShader(s_blit_program, fs);
    glBindAttribLocation(s_blit_program, 0, "aPos");
    glBindAttribLocation(s_blit_program, 1, "aUV");
    glLinkProgram(s_blit_program);
    GLint link_status = GL_FALSE;
    glGetProgramiv(s_blit_program, GL_LINK_STATUS, &link_status);
    if (link_status == GL_FALSE) {
        GLchar log[512];
        GLsizei len = 0;
        glGetProgramInfoLog(s_blit_program, sizeof(log), &len, log);
        l_error("DOWNSAMPLE_RENDER: blit program link FAILED: %s", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
}
#endif // DOWNSAMPLE_RENDER

void gl_preload() {
    // Nothing required for PVR_PSP2
}

void gl_init() {
    PVRSRV_PSP2_APPHINT hint;
    memset(&hint, 0, sizeof(hint));
    unsigned int rc = PVRSRVInitializeAppHint(&hint);
    l_success("PVRSRVInitializeAppHint -> %u", rc);
    if (!rc) {
        l_error("PVRSRVInitializeAppHint failed");
        sceKernelExitProcess(0);
    }

    // PVRSRVInitializeAppHint() only fills the numeric/boolean defaults. The
    // window-system and GLES driver paths are app-specific and are left empty,
    // so we MUST set them here: libIMGEGL dlopen()s these three modules to back
    // the EGL display. If szWindowSystem is empty, eglGetDisplay() returns
    // EGL_NO_DISPLAY while eglGetError() still reports EGL_SUCCESS (0x3000) --
    // the exact failure seen in logs 047-050 after this block was reduced to a
    // bare memset. Paths mirror the sceKernelLoadStartModule() calls in main.c.
    strncpy(hint.szWindowSystem, "app0:module/libpvrPSP2_WSEGL.suprx", sizeof(hint.szWindowSystem) - 1);
    strncpy(hint.szGLES1, "app0:module/libGLESv1_CM.suprx", sizeof(hint.szGLES1) - 1);
    strncpy(hint.szGLES2, "app0:module/libGLESv2.suprx", sizeof(hint.szGLES2) - 1);

    rc = PVRSRVCreateVirtualAppHint(&hint);
    l_success("PVRSRVCreateVirtualAppHint -> %u (WS=%s)", rc, hint.szWindowSystem);
    if (!rc) {
        l_error("PVRSRVCreateVirtualAppHint failed");
        sceKernelExitProcess(0);
    }

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    l_success("eglGetDisplay -> %p (err 0x%x)", (void *)display, eglGetError());
    if (display == EGL_NO_DISPLAY) {
        l_error("eglGetDisplay failed with error 0x%x", eglGetError());
        sceKernelExitProcess(0);
    }

    if (eglInitialize(display, NULL, NULL) != EGL_TRUE) { l_error("eglInitialize failed"); sceKernelExitProcess(0); }

    EGLConfig config;
    EGLint num_config;
    EGLint attrib_list[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_STENCIL_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    if (eglChooseConfig(display, attrib_list, &config, 1, &num_config) != EGL_TRUE || num_config == 0) {
        l_error("eglChooseConfig failed with error 0x%x", eglGetError());
        sceKernelExitProcess(0);
    }

    {
        EGLint depth_bits = -1, stencil_bits = -1;
        eglGetConfigAttrib(display, config, EGL_DEPTH_SIZE, &depth_bits);
        eglGetConfigAttrib(display, config, EGL_STENCIL_SIZE, &stencil_bits);
        l_success("EGL config: depth=%d stencil=%d (requested depth=16)", depth_bits, stencil_bits);
    }

    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (context == EGL_NO_CONTEXT) { l_error("eglCreateContext failed with error 0x%x", eglGetError()); sceKernelExitProcess(0); }

    surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)0, NULL);
    if (surface == EGL_NO_SURFACE) { l_error("eglCreateWindowSurface failed with error 0x%x", eglGetError()); sceKernelExitProcess(0); }

    if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
        l_error("eglMakeCurrent failed with error 0x%x", eglGetError());
        sceKernelExitProcess(0);
    }

    l_success("PVR_PSP2 EGL context created.");

#ifdef DISABLE_VSYNC
    // Diagnostic only (causes tearing): answers "is our measured FPS
    // artificially capped by vsync, or genuinely below the refresh rate
    // already" before investing in DOWNSAMPLE_RENDER below.
    if (eglSwapInterval(display, 0) == EGL_TRUE) {
        l_success("DISABLE_VSYNC: eglSwapInterval(0) applied");
    } else {
        l_error("DISABLE_VSYNC: eglSwapInterval(0) failed with error 0x%x", eglGetError());
    }
#endif

#ifdef DOWNSAMPLE_RENDER
    l_success("DOWNSAMPLE_RENDER: about to create the offscreen FBO...");
    init_downsample_render();
    l_success("DOWNSAMPLE_RENDER: init_downsample_render() returned (fbo=%u)", s_ds_fbo);
#endif
}

void gl_swap() {
#ifdef DOWNSAMPLE_RENDER
    if (s_ds_fbo != 0) {
        // Blit the reduced-resolution FBO up onto the REAL default
        // framebuffer -- glBindFramebuffer(0) here calls the real GL entry
        // point directly (this is glutil.c, not the _soloader wrapper), so
        // it genuinely targets the window's framebuffer, bypassing the
        // FBO-0 redirect that glBindFramebuffer_soloader applies to the
        // engine's own calls.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);

        int lb_x, lb_y, lb_w, lb_h;
        compute_letterbox_rect(&lb_x, &lb_y, &lb_w, &lb_h);
        glViewport(lb_x, lb_y, lb_w, lb_h);

        glUseProgram(s_blit_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_ds_color_tex);
        glUniform1i(glGetUniformLocation(s_blit_program, "uTex"), 0);

        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), kBlitQuad);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), kBlitQuad + 2);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
    }
#endif
    eglSwapBuffers(display, surface);
    s_frame_counter++;
}

void glCompileShader_soloader(GLuint shader) {
    glCompileShader(shader);

    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE) {
        GLchar log[1024];
        GLsizei len = 0;
        glGetShaderInfoLog(shader, sizeof(log), &len, log);
        l_error("glCompileShader(%u) FAILED: %s", shader, log);
    }
}

void glLinkProgram_soloader(GLuint program) {
    glLinkProgram(program);

    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status == GL_FALSE) {
        GLchar log[1024];
        GLsizei len = 0;
        glGetProgramInfoLog(program, sizeof(log), &len, log);
        l_error("glLinkProgram(%u) FAILED: %s", program, log);
    }
}

void glShaderSource_soloader(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length) {
#ifdef DEBUG_SOLOADER
    record_shader_variant(shader, count, string);
#endif
    glShaderSource(shader, count, (const GLchar **) string, length);
}

void glAttachShader_soloader(GLuint program, GLuint shader) {
#ifdef DEBUG_SOLOADER
    int alpha_map = shader_has_alpha_map(shader);
    if (alpha_map == 1) {
        TrackedProgram *p = find_or_track_program(program);
        if (p) p->alpha_map = 1;
    }
#endif
    glAttachShader(program, shader);
}

void glUseProgram_soloader(GLuint program) {
#ifdef DEBUG_SOLOADER
    TrackedProgram *p = find_or_track_program(program);
    if (p && !p->logged) {
        p->logged = 1;
        l_info("[gl_shader_variant] program=%u alpha_map(AL/AT)=%d", program, p->alpha_map);
    }
#endif
    glUseProgram(program);
}

void glClearColor_soloader(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
    l_debug("glClearColor requested (%.2f, %.2f, %.2f, %.2f), forcing black",
            red, green, blue, alpha);
    glClearColor(0.0f, 0.0f, 0.0f, alpha);
}

void gl_log_render_diag(int frame) {
#ifndef DEBUG_SOLOADER
    // All of this was diagnostic instrumentation for the "3D world invisible"
    // investigation (now resolved, log_076 onward renders correctly) -- every
    // call here was a real glGet*() driver round-trip that ran unconditionally
    // even in Release, since only the l_info/l_debug macros themselves were
    // ever compiled out, not the work computed for them. Skip all of it
    // outside debug builds now that it's done its job.
    (void) frame;
    return;
#else
    GLint depth_bits = -1;
    glGetIntegerv(GL_DEPTH_BITS, &depth_bits);
    GLboolean depth_test = glIsEnabled(GL_DEPTH_TEST);

    // log_075 caught GL_DEPTH_RANGE collapsed to (1.0, 1.0) from frame 121
    // onward, right when the 3D character preview starts drawing -- with
    // near==far, every fragment's window-space depth becomes a constant 1.0
    // regardless of its actual clip-space Z. Combined with GL_DEPTH_FUNC ==
    // GL_LESS (the GLES2 default) and a depth buffer cleared to the default
    // 1.0, that constant-1.0 fragment depth can never be "less than" the
    // cleared 1.0 already sitting in the buffer -- the depth test would fail
    // for every single fragment of that draw, discarding all of its color
    // output with zero GL error. GL_DEPTH_FUNC/GL_DEPTH_CLEAR_VALUE weren't
    // captured before, so this confirms or kills that theory directly.
    GLint depth_func = -1;
    glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
    GLfloat depth_clear_value = -1.0f;
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &depth_clear_value);

    GLint bound_fbo = -1;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound_fbo);

    // Draw-call count accumulated since the previous call to this function --
    // settles whether the engine is even issuing 3D draw calls (vs. depth_test
    // staying disabled just because nothing is being drawn at all) without
    // logging every single glDrawArrays/glDrawElements call, which would be
    // way too noisy across a full scene.
    unsigned int draw_calls = s_draw_calls_since_diag;
    s_draw_calls_since_diag = 0;

    // Culling/winding state: draw_calls being non-zero with depth_test toggling
    // correctly around them still leaves "every 3D triangle gets backface-culled
    // due to inverted winding" on the table as an explanation for "draws happen,
    // nothing appears" -- a real risk given the Y-axis handling differences
    // between what the engine assumes and how PVR_PSP2/EGL present the surface.
    GLboolean cull_enabled = glIsEnabled(GL_CULL_FACE);
    GLint cull_mode = -1, front_face = -1;
    glGetIntegerv(GL_CULL_FACE_MODE, &cull_mode);
    glGetIntegerv(GL_FRONT_FACE, &front_face);

    if (bound_fbo != 0) {
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        l_info("[gl_diag] frame %d: depth_bits=%d depth_test=%d depth_func=0x%04x depth_clear=%.2f bound_fbo=%d status=0x%04x draw_calls=%u cull=%d cull_mode=0x%04x front_face=0x%04x",
               frame, depth_bits, depth_test, depth_func, depth_clear_value, bound_fbo, status, draw_calls, cull_enabled, cull_mode, front_face);
    } else {
        l_info("[gl_diag] frame %d: depth_bits=%d depth_test=%d depth_func=0x%04x depth_clear=%.2f bound_fbo=default draw_calls=%u cull=%d cull_mode=0x%04x front_face=0x%04x",
               frame, depth_bits, depth_test, depth_func, depth_clear_value, draw_calls, cull_enabled, cull_mode, front_face);
    }

    // Scissor/blend/writemask/stencil: each of these can independently produce
    // "draws happen, GL reports no error, nothing appears on screen" --
    // a degenerate scissor box clips every fragment, a blend factor that
    // resolves to zero alpha/color makes the draw fully transparent, a
    // disabled color writemask draws to depth only, and a stencil test with
    // a mismatching ref discards every fragment. None of these are covered by
    // the depth/cull/matrix checks above, so covering them here closes off
    // the other common "invisible with zero errors" causes at once.
    GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
    GLint scissor_box[4] = {-1, -1, -1, -1};
    glGetIntegerv(GL_SCISSOR_BOX, scissor_box);

    GLboolean blend_enabled = glIsEnabled(GL_BLEND);
    GLint blend_src_rgb = -1, blend_dst_rgb = -1, blend_src_a = -1, blend_dst_a = -1, blend_eq_rgb = -1;
    glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &blend_eq_rgb);

    GLboolean color_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    GLboolean depth_mask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);

    GLboolean stencil_enabled = glIsEnabled(GL_STENCIL_TEST);
    GLint stencil_func = -1, stencil_ref = -1, stencil_mask = -1;
    glGetIntegerv(GL_STENCIL_FUNC, &stencil_func);
    glGetIntegerv(GL_STENCIL_REF, &stencil_ref);
    glGetIntegerv(GL_STENCIL_VALUE_MASK, &stencil_mask);

    l_info("[gl_diag2] frame %d: scissor=%d box=(%d,%d,%d,%d) blend=%d src_rgb=0x%04x dst_rgb=0x%04x src_a=0x%04x dst_a=0x%04x eq_rgb=0x%04x color_mask=%d%d%d%d depth_mask=%d stencil=%d func=0x%04x ref=%d mask=0x%x",
           frame, scissor_enabled, scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3],
           blend_enabled, blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a, blend_eq_rgb,
           color_mask[0], color_mask[1], color_mask[2], color_mask[3], depth_mask,
           stencil_enabled, stencil_func, stencil_ref, stencil_mask);

    // Viewport/depth-range/program/texture snapshot: catches a degenerate
    // (zero-area or off-screen) viewport specifically for whichever pass ran
    // most recently, a depth range collapsed to a single value, no shader
    // program bound at all (glUseProgram(0) left active), or -- the single
    // most likely silent culprit for "draws, but solid black" -- no texture
    // bound on the active unit while the fragment shader still samples one
    // (implementation-defined, but returning black is extremely common).
    GLint viewport[4] = {-1, -1, -1, -1};
    glGetIntegerv(GL_VIEWPORT, viewport);
    GLfloat depth_range[2] = {-1.0f, -1.0f};
    glGetFloatv(GL_DEPTH_RANGE, depth_range);
    GLint current_program = -1;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
    GLint active_texture = -1;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
    GLint tex_binding_2d = -1;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex_binding_2d);

    l_info("[gl_diag3] frame %d: viewport=(%d,%d,%d,%d) depth_range=(%.2f,%.2f) program=%d active_texture=0x%04x tex_binding_2d=%d",
           frame, viewport[0], viewport[1], viewport[2], viewport[3],
           depth_range[0], depth_range[1], current_program, active_texture, tex_binding_2d);

    // "Enemigos invisibles": every DISTINCT texture bound across draw calls
    // since the last diag window (see track_seen_texture()), not just this
    // one instant -- if a third/fourth ID never shows up here across many
    // consecutive windows during actual combat, enemy draws are either not
    // happening or are reusing the player/UI textures, which would explain
    // the reported invisibility far better than a one-point-in-time sample
    // that could just be unlucky.
    {
        char buf[512];
        int off = 0;
        for (int i = 0; i < s_seen_textures_count && off < (int) sizeof(buf) - 16; i++) {
            off += snprintf(buf + off, sizeof(buf) - off, "%s%u:p%d", i ? "," : "",
                             s_seen_textures[i], s_seen_texture_programs[i]);
        }
        l_info("[gl_diag_textures] frame %d: distinct_textures_used=%d%s [%s]",
               frame, s_seen_textures_count,
               s_seen_textures_overflowed ? "+ (cap hit, some IDs dropped)" : "",
               s_seen_textures_count ? buf : "");
        s_seen_textures_count = 0;
        s_seen_textures_overflowed = 0;
    }
#endif
}

// One-shot state dump taken on the first glDrawArrays/glDrawElements call
// after each diagnostic window resets (i.e. roughly once per ~60 frames,
// same cadence as gl_log_render_diag, but captured at the exact moment a 3D
// draw call actually executes rather than at an arbitrary point in the
// frame) -- catches the case where the periodic snapshot above lands between
// draws and misses a program/texture-binding state that only holds during
// the draw itself.
static void log_first_draw_state(const char *call) {
#ifndef DEBUG_SOLOADER
    (void) call;
    return;
#else
    GLint current_program = -1;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
    GLint active_texture = -1;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
    GLint tex_binding_2d = -1;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex_binding_2d);
    GLint viewport[4] = {-1, -1, -1, -1};
    glGetIntegerv(GL_VIEWPORT, viewport);

    l_info("[gl_diag_firstdraw] frame %d: %s program=%d active_texture=0x%04x tex_binding_2d=%d viewport=(%d,%d,%d,%d)",
           s_frame_counter, call, current_program, active_texture, tex_binding_2d,
           viewport[0], viewport[1], viewport[2], viewport[3]);
#endif
}

void glEnable_soloader(GLenum cap) {
    if (cap == GL_DEPTH_TEST) {
        l_debug("glEnable(GL_DEPTH_TEST) at frame %d", s_frame_counter);
    }
    if (cap == GL_SCISSOR_TEST) {
        // Diagnostic for the repeating-icon-column HUD bug: gl_diag2's
        // periodic sampling never once caught scissor test enabled all of
        // log_090.txt, but that's a sampling-timing gap, not proof the
        // engine never enables it -- this logs every real toggle instead.
        l_debug("glEnable(GL_SCISSOR_TEST) at frame %d", s_frame_counter);
    }
    glEnable(cap);
}

void glDisable_soloader(GLenum cap) {
    if (cap == GL_DEPTH_TEST) {
        l_debug("glDisable(GL_DEPTH_TEST) at frame %d", s_frame_counter);
    }
    if (cap == GL_SCISSOR_TEST) {
        l_debug("glDisable(GL_SCISSOR_TEST) at frame %d", s_frame_counter);
    }
    glDisable(cap);
}

#ifdef DEBUG_SOLOADER
static void track_seen_texture(void) {
    GLint tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    for (int i = 0; i < s_seen_textures_count; i++) {
        if (s_seen_textures[i] == (GLuint) tex) {
            return;
        }
    }
    if (s_seen_textures_count >= MAX_TRACKED_TEXTURES) {
        // Don't silently drop this ID -- flag the window as truncated so
        // gl_log_render_diag reports "N+, some dropped" instead of a flat
        // number indistinguishable from "this really is every texture".
        s_seen_textures_overflowed = 1;
        return;
    }
    GLint program = -1;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    s_seen_texture_programs[s_seen_textures_count] = program;
    s_seen_textures[s_seen_textures_count++] = (GLuint) tex;

    // log_089 confirmed some combat-only (enemy) textures ARE drawn through
    // an alpha_map==1 (AL/AT) program at least some of the time -- if that
    // program's separate AlphaSampler unit (conventionally unit 1, DiffuseSampler
    // being unit 0) isn't actually bound to a real texture for this draw,
    // GLES2's defined behavior for sampling an unbound unit (texture object 0)
    // is to return (0,0,0,0) -- exactly the "draws happen, alpha is silently
    // zero" symptom this whole investigation is chasing. One-shot probe per
    // newly-seen (texture, alpha-map program) pair, not every draw -- reads
    // GL_ACTIVE_TEXTURE/GL_TEXTURE_BINDING_2D on units 0 and 1 and restores
    // the active unit exactly afterward, so this is read-only and changes
    // nothing the engine would ever observe.
    TrackedProgram *tp = find_or_track_program((GLuint) program);
    if (tp && tp->alpha_map == 1) {
        GLint prev_active = -1;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
        GLint unit0_tex = -1, unit1_tex = -1;
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &unit0_tex);
        glActiveTexture(GL_TEXTURE1);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &unit1_tex);
        glActiveTexture((GLenum) prev_active);
        l_info("[gl_diag_alphamap] texture=%u program=%u unit0_tex=%d unit1_tex=%d",
               (GLuint) tex, (GLuint) program, unit0_tex, unit1_tex);
    }
}
#endif

void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count) {
#ifdef DEBUG_SOLOADER
    if (s_draw_calls_since_diag == 0) {
        log_first_draw_state("glDrawArrays");
    }
    s_draw_calls_since_diag++;
    track_seen_texture();
#endif
    glDrawArrays(mode, first, count);
}

void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type, const void *indices) {
#ifdef DEBUG_SOLOADER
    if (s_draw_calls_since_diag == 0) {
        log_first_draw_state("glDrawElements");
    }
    s_draw_calls_since_diag++;
    track_seen_texture();
#endif
    glDrawElements(mode, count, type, indices);
}

void glDepthRangef_soloader(GLclampf n, GLclampf f) {
    // log_075 caught GL_DEPTH_RANGE stuck at (1.0, 1.0) from the moment the 3D
    // character preview starts drawing onward -- logging every real call
    // (args + frame) pins down whether the engine itself asks for this
    // degenerate range (and how often/from where in its own render loop) vs.
    // it being some leftover/uninitialized state we're misreading.
    l_debug("glDepthRangef(%.4f, %.4f) at frame %d", n, f, s_frame_counter);
    glDepthRangef(n, f);
}

void glUniformMatrix4fv_soloader(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
#ifdef DEBUG_SOLOADER
    // Sanity check for "draws happen, nothing appears" with no GL error: an
    // all-zero or NaN/Inf matrix uploaded as a camera/projection/model uniform
    // would collapse or fling geometry out of the clip volume silently. Only
    // logs when actually degenerate, so this stays quiet on every normal frame.
    // Gated behind DEBUG_SOLOADER, not just the log macro -- this scans 16
    // floats on EVERY uniform upload (potentially several per draw call,
    // thousands of draws/sec once the game is running), real CPU work that a
    // no-op l_warn alone wouldn't have removed in Release.
    for (GLsizei i = 0; i < count; i++) {
        const GLfloat *m = value + (size_t) i * 16;
        int all_zero = 1;
        int has_nan_or_inf = 0;
        for (int j = 0; j < 16; j++) {
            if (m[j] != 0.0f) all_zero = 0;
            if (isnan(m[j]) || isinf(m[j])) has_nan_or_inf = 1;
        }
        if (all_zero || has_nan_or_inf) {
            l_warn("glUniformMatrix4fv(location=%d, matrix %d/%d) DEGENERATE at frame %d: all_zero=%d has_nan_or_inf=%d",
                   location, i + 1, count, s_frame_counter, all_zero, has_nan_or_inf);
        }
    }
#endif
    glUniformMatrix4fv(location, count, transpose, value);
}

void glCompressedTexImage2D_soloader(GLenum target, GLint level, GLenum internalformat,
                                      GLsizei width, GLsizei height, GLint border,
                                      GLsizei imageSize, const void *data) {
#ifdef DEBUG_SOLOADER
    GLint tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);

    const char *fmt_name = "unknown";
    int has_alpha = -1;
    switch (internalformat) {
        case GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG:  fmt_name = "PVRTC_RGB_4BPP";  has_alpha = 0; break;
        case GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG:  fmt_name = "PVRTC_RGB_2BPP";  has_alpha = 0; break;
        case GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG: fmt_name = "PVRTC_RGBA_4BPP"; has_alpha = 1; break;
        case GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG: fmt_name = "PVRTC_RGBA_2BPP"; has_alpha = 1; break;
        default: break;
    }

    // Cheapest possible corruption signal without a full PVRTC block decoder:
    // a compressed blob where every single byte is identical (0x00 most
    // tellingly) cannot encode real image variation no matter the format --
    // flag it before anyone attempts real block-level decode work.
    int degenerate = 0;
    if (data && imageSize > 1) {
        const unsigned char *bytes = (const unsigned char *) data;
        unsigned char first = bytes[0];
        degenerate = 1;
        for (GLsizei i = 1; i < imageSize; i++) {
            if (bytes[i] != first) { degenerate = 0; break; }
        }
    }

    l_debug("[gl_diag_pvrtc] texture=%d level=%d format=0x%04x(%s alpha=%d) %dx%d imageSize=%d%s",
            tex, level, internalformat, fmt_name, has_alpha, width, height, imageSize,
            degenerate ? " <-- DEGENERATE (every byte identical)" : "");
#endif
    glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
}

void glViewport_soloader(GLint x, GLint y, GLsizei width, GLsizei height) {
    // DH2's logical canvas for a 960-wide screen (see glutil.h for how this
    // was confirmed) vs. the target this actually renders to -- REAL_W/REAL_H
    // must match SCREEN_W/SCREEN_H in main.c UNLESS DOWNSAMPLE_RENDER is on,
    // in which case every glViewport call happens while our reduced-res FBO
    // is bound (see glBindFramebuffer_soloader) and must be scaled into ITS
    // dimensions instead -- gl_swap()'s upscale blit is what maps the result
    // back onto the true 960x544 screen afterwards.
#ifdef DOWNSAMPLE_RENDER
    const int REAL_W = REAL_SCREEN_W * DS_NUM / DS_DEN;
    const int REAL_H = REAL_SCREEN_H * DS_NUM / DS_DEN;
#else
    const int REAL_W = REAL_SCREEN_W;
    const int REAL_H = REAL_SCREEN_H;
#endif

    const float scale = (float) REAL_H / (float) LOGICAL_H;
    const int scaled_w = (int) (LOGICAL_W * scale + 0.5f);
    const int x_offset = (REAL_W - scaled_w) / 2;

    glViewport(x_offset + (int) (x * scale + 0.5f),
               (int) (y * scale + 0.5f),
               (int) (width * scale + 0.5f),
               (int) (height * scale + 0.5f));
}

void glScissor_soloader(GLint x, GLint y, GLsizei width, GLsizei height) {
    // Identical remap math to glViewport_soloader -- see glutil.h for why:
    // the engine's glScissor calls live in the same assumed 960x640 logical
    // canvas as its glViewport calls, so the clip rect needs the same
    // letterbox scale+offset to line up with the real 960x544 screen.
#ifdef DOWNSAMPLE_RENDER
    const int REAL_W = REAL_SCREEN_W * DS_NUM / DS_DEN;
    const int REAL_H = REAL_SCREEN_H * DS_NUM / DS_DEN;
#else
    const int REAL_W = REAL_SCREEN_W;
    const int REAL_H = REAL_SCREEN_H;
#endif

    const float scale = (float) REAL_H / (float) LOGICAL_H;
    const int scaled_w = (int) (LOGICAL_W * scale + 0.5f);
    const int x_offset = (REAL_W - scaled_w) / 2;

    const int out_x = x_offset + (int) (x * scale + 0.5f);
    const int out_y = (int) (y * scale + 0.5f);
    const int out_w = (int) (width * scale + 0.5f);
    const int out_h = (int) (height * scale + 0.5f);

    // Diagnostic for the repeating-icon-column HUD bug: shows whether the
    // engine's logical scissor rect (and our remapped physical one) is ever
    // sane/tight around that panel, since gl_diag2's periodic sampling can't
    // be correlated to the specific frame the bug is visible on.
    l_debug("glScissor logical=(%d,%d,%d,%d) -> physical=(%d,%d,%d,%d) at frame %d",
            x, y, width, height, out_x, out_y, out_w, out_h, s_frame_counter);

    glScissor(out_x, out_y, out_w, out_h);
}

void glBindFramebuffer_soloader(GLenum target, GLuint framebuffer) {
#ifdef DOWNSAMPLE_RENDER
    if (framebuffer == 0 && s_ds_fbo != 0) {
        glBindFramebuffer(target, s_ds_fbo);
        return;
    }
#endif
    glBindFramebuffer(target, framebuffer);
}
