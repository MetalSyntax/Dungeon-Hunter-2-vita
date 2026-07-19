#ifndef SOLOADER_GLUTIL_H
#define SOLOADER_GLUTIL_H
#include <psp2/types.h>
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES2/gl2.h>

#ifdef __cplusplus
extern "C" {
#endif

void gl_init();
void gl_preload();
void gl_swap();

// Thin wrappers around the real glCompileShader/glLinkProgram that also check
// GL_COMPILE_STATUS/GL_LINK_STATUS and log the info log on failure -- wired
// into dynlib.c's import table in place of the raw GL entry points so we get
// a definitive yes/no on whether the engine's real GLSL (see shaders.pak)
// actually compiles against the real PVR_PSP2 GLSL ES compiler, instead of
// guessing from a flat/wrong-colored screen.
void glCompileShader_soloader(GLuint shader);
void glLinkProgram_soloader(GLuint program);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GLUTIL_H
