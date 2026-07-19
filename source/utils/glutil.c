#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/dialog.h"
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <stddef.h>
#include <string.h>
#include <gpu_es4/psp2_pvr_hint.h>

static EGLDisplay display;
static EGLSurface surface;
static EGLContext context;

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
}

void gl_swap() {
    eglSwapBuffers(display, surface);
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
