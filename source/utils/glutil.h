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

// Diagnostic-only (DEBUG_SOLOADER-gated internally, real GL calls always run
// unmodified either way) trio for the "invisible enemies" investigation:
// tags each shader as using the shared GL_Diffuse_L1_iPhone template's AL/AT
// (separate AlphaSampler) variant or not, propagates that tag through
// whichever program a tagged shader gets attached to, and logs each newly-
// used program's tag once. Lets a hardware log answer "do monster draws use
// a different alpha path than the player's" without touching any shader
// source, attach, link, or use call's actual behavior. Wired into dynlib.c in
// place of the raw glShaderSource/glAttachShader/glUseProgram entries.
void glShaderSource_soloader(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length);
void glAttachShader_soloader(GLuint program, GLuint shader);
void glUseProgram_soloader(GLuint program);

// The engine's own GSInit loading screen clears to a flat aquamarine color
// (its default "nothing loaded yet" background) before any real menu/game
// content draws over it -- forces black instead, a less jarring stand-in for
// the same "nothing here yet" background. Wired into dynlib.c in place of
// the raw glClearColor entry point.
void glClearColor_soloader(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void glClearColorx_soloader(GLclampx red, GLclampx green, GLclampx blue, GLclampx alpha);

// DH2's appInit() (out_ghidra.c) picks a hardcoded logical rendering-canvas
// size purely from the screen width it's told about -- for our real width
// (960, matching its "960-wide device" bucket) that logical canvas is
// 960x640, and the ENTIRE engine (2D UI and 3D world alike) issues all its
// glViewport calls in that 960x640 space via glitch::createDevice(). The
// Vita's real surface is 960x544 -- 96px short of that assumed height,
// which is why 2D/3D content looks squashed/stretched. This wrapper
// letterboxes (pillarboxes) every glViewport call from that assumed 960x640
// logical space into a centered, aspect-correct sub-rectangle of the real
// 960x544 physical screen, instead of patching the engine's internal
// dimension table (not reachable via so_symbol() -- it's local/static, not
// in .dynsym). Wired into dynlib.c in place of the raw glViewport entry
// point.
void glViewport_soloader(GLint x, GLint y, GLsizei width, GLsizei height);

// Same letterbox remap as glViewport_soloader, applied to glScissor -- the
// engine issues glScissor calls in the same assumed 960x640 logical canvas as
// glViewport (e.g. to clip a HUD sub-panel to its visible row count), but
// unlike glViewport this call was still wired straight to the real driver
// function (source/dynlib.c), so its clip rect never got remapped into the
// real letterboxed 960x544 screen. Found investigating a HUD bug: a repeating
// icon-list panel bled extra rows onto screen that should have been clipped
// off, because the scissor box no longer matched where the content actually
// is post-letterbox. Wired into dynlib.c in place of the raw glScissor entry.
void glScissor_soloader(GLint x, GLint y, GLsizei width, GLsizei height);

// Inverts the glViewport_soloader letterbox: converts a touch coordinate in
// real physical-screen space (0..959, 0..543) into the engine's logical
// 960x640 space that nativeOnTouch's hit-testing actually operates in. Needed
// because the touch path (main.c) fed raw physical-screen coordinates
// straight into nativeOnTouch with no knowledge of the pillarbox, so on-screen
// buttons only registered when tapped off-position (found investigating a
// "touch hitbox doesn't match the visible button" bug). Returns 0 (leaving
// *out_x/*out_y untouched) if the point falls in the pillarbox bars, where
// there's no real content to hit; 1 with *out_x/*out_y filled otherwise.
int glutil_screen_touch_to_logical(int screen_x, int screen_y, int *out_x, int *out_y);

// Tamano real del render target al que se esta dibujando ahora mismo: la
// pantalla fisica (960x544) normalmente, o el FBO reducido cuando
// DOWNSAMPLE_RENDER esta activo. Cualquier codigo NUESTRO que setee un
// glViewport de "pantalla completa" a mano tiene que usar esto en vez de
// hardcodear 960x544 -- si no, dibuja fuera del FBO reducido y solo se ve el
// pedazo que entra (fue exactamente el "video con zoom" de log_001.log).
void glutil_get_render_target_size(int *out_w, int *out_h);

// Diagnostic for the "2D UI draws fine, 3D world is invisible/replaced by a
// flat color" symptom: logs whether depth testing is actually enabled,
// whether the currently bound framebuffer (if not the default one) is
// complete, and how many glDrawArrays/glDrawElements calls happened since
// the previous call to this function -- so a broken depth test or an
// incomplete render-target used for the 3D pass can be told apart from a
// genuine "engine issues zero 3D draw calls" case. Called periodically from
// main.c's existing per-frame diagnostic.
void gl_log_render_diag(int frame);

// Thin wrappers that log a frame-numbered trace specifically when
// GL_DEPTH_TEST is toggled -- log_072 showed depth_test flipping to disabled
// around the main-menu 3D character preview and staying disabled through
// level entry, but couldn't say whether the engine ever re-enables it (and
// gets overridden) or simply never asks. Only GL_DEPTH_TEST is logged (any
// other cap is a silent passthrough) to avoid drowning the log in unrelated
// glEnable/glDisable traffic. Wired into dynlib.c in place of the raw
// glEnable/glDisable entry points.
void glEnable_soloader(GLenum cap);
void glDisable_soloader(GLenum cap);

// Thin passthrough wrappers around glDrawArrays/glDrawElements that only
// increment a per-diagnostic-interval counter (read and reset by
// gl_log_render_diag) -- settles whether the engine is issuing any 3D draw
// calls at all during the "3D world invisible" window, instead of inferring
// it indirectly from depth-test state alone. Wired into dynlib.c in place of
// the raw glDrawArrays/glDrawElements entry points.
void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count);
void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type, const void *indices);

// Diagnostic for the invisible-enemy bug (DEBUG_SOLOADER-gated, real GL calls
// always run unmodified either way): patch.c's SkinnedMeshSceneNode/
// CModularSkinnedMeshSceneNode/XrayModularSkinnedMeshSceneNode render() hooks
// bracket their SO_CONTINUE call with reset/get so each render() invocation
// can be attributed a "draw_calls issued, last texture bound, last vertex/
// index count" summary -- settling whether a specific scene-node instance's
// render() draws NOTHING at all (a mesh/submesh-list problem upstream of any
// texture/shader concern) vs. draws with a suspicious texture/geometry
// (0-vertex draws, or a texture ID that never appears for known-visible
// characters). Every real glDrawArrays_soloader/glDrawElements_soloader call
// updates this regardless of whether a render() hook is currently on the
// stack, so callers must reset immediately before SO_CONTINUE and read
// immediately after, not across any other GL activity.
//
// 2026-08-06 (log_104.txt) extension: draw_calls/last_texture alone showed
// most enemies DO issue a real draw with a valid texture during actual
// gameplay (only 1/8 new instances post-load had draw_calls==0, the other 76
// zero-draw hits were all during the loading screen, before any mesh is
// ready -- expected, not the bug). User also reports the SAME player
// character renders visibly transparent in cutscenes/character-select. A
// draw that happens but is see-through is a blend/depth-state question, not
// a "nothing drew" one -- so this now also captures the exact blend/depth
// state active at the moment of the last draw call for the tracked node,
// settles whether an invisible-but-drawing character is due to e.g. blend
// left enabled with a src/dst factor pair that composites to fully
// transparent, or depth-write disabled letting something else immediately
// paint over it.
typedef struct {
    unsigned draw_calls;
    GLint last_texture;
    GLsizei last_vertex_count;
    GLboolean last_blend_enabled;
    GLint last_blend_src_rgb;
    GLint last_blend_dst_rgb;
    GLint last_blend_src_alpha;
    GLint last_blend_dst_alpha;
    GLboolean last_depth_test_enabled;
    GLboolean last_depth_write_mask;
} GLNodeDrawState;

void gl_diag_reset_render_track(void);
void gl_diag_get_render_track(GLNodeDrawState *out);

// Sanity-checks every matrix uploaded via glUniformMatrix4fv (camera/model/
// projection, whichever the engine is setting) for being all-zero or
// containing NaN/Inf -- draw_calls and depth_test look correct per log_073,
// so if geometry is still invisible with zero GL errors, a degenerate
// transform is the next most likely silent cause. Wired into dynlib.c in
// place of the raw glUniformMatrix4fv entry point.
void glUniformMatrix4fv_soloader(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);

// Sibling check for plain vec4 uniforms (tint/color multiplies, common
// mobile-shader idiom) -- glUniformMatrix4fv_soloader only ever sees 4x4
// matrices, so a vec4 alpha-multiply uniform sitting at near-zero would
// slip past it entirely. Flags NaN/Inf same as the matrix check, plus a
// vec4-that-looks-like-a-color-with-near-zero-alpha heuristic. Wired into
// dynlib.c in place of the raw glUniform4fv entry point.
void glUniform4fv_soloader(GLint location, GLsizei count, const GLfloat *value);

// Invisible-enemy investigation, next candidate after opacity was ruled out
// with certainty (CharProperties::RecalcProperty/PROPS_GetOpacity both read
// 1.0 for every character, see PORTING_PLAN.md) and the geometry/transform
// path was already clean (glUniformMatrix4fv_soloader above never once
// logged a degenerate matrix across three full combat sessions): the shipped
// GL_Diffuse_L1_iPhone shader derives ALL fragment alpha from the diffuse
// texture's own decoded alpha channel (vertex alpha is hardcoded to 1.0),
// so a texture whose real pixel data decodes to alpha=0 would produce
// exactly this bug with zero GL error and a fully-issued draw call. Only the
// PVRTC container's HEADER bytes have ever been compared between a working
// (player) and suspect (monster) texture -- never the actual compressed
// block data. This wrapper logs the real internalformat GL enum (confirms
// RGB-only vs RGBA-capable PVRTC at actual upload time, not just inferred
// from a static file header) and flags the cheapest possible corruption
// signal -- a compressed blob where every byte is identical, which cannot
// encode real image detail regardless of format. Wired into dynlib.c in
// place of the raw glCompressedTexImage2D entry point.
void glCompressedTexImage2D_soloader(GLenum target, GLint level, GLenum internalformat,
                                      GLsizei width, GLsizei height, GLint border,
                                      GLsizei imageSize, const void *data);

// New lead for the invisible/near-transparent-enemy investigation (2026-08-08,
// log_120.txt + user screenshot: "casi transparentes", not fully invisible --
// a stronger, more specific symptom than earlier reports). A real dumped
// fragment shader (glsl_dump/A6673032....glsl) does
// "color = texture2D(...) + DiffuseColor; color *= vColor0; color.rgb *= color.a;" --
// DiffuseColor is ADDITIVE (its usual (0,0,0,0) is the neutral no-op default,
// not a bug -- the earlier glUniform4fv_soloader NEAR_ZERO_ALPHA hits on it
// are a red herring). vColor0 is the real suspect: a per-vertex Color0
// attribute that directly gates final alpha via "color.rgb *= color.a", the
// opposite of what an older comment in this file assumed ("vertex alpha
// hardcoded to 1.0" -- true for a DIFFERENT shader variant, not this one).
// When the engine has no per-vertex color array bound for a draw, GLES
// convention is to fall back to a CONSTANT value via glVertexAttrib4f/4fv
// (glVertexAttribPointer's own per-vertex path isn't practically
// interceptable the same way). If that constant ever comes back (0,0,0,0)
// instead of the expected opaque-white (1,1,1,1) default -- e.g. the same
// class of uninitialized/corrupted read already confirmed for CharProperties
// -- this reproduces "draws, zero GL error, nearly invisible" exactly.
// Same throttled near-zero-alpha heuristic as glUniform4fv_soloader, keyed by
// attribute index. Wired into dynlib.c in place of the raw glVertexAttrib4f/
// glVertexAttrib4fv entry points.
void glVertexAttrib4f_soloader(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void glVertexAttrib4fv_soloader(GLuint index, const GLfloat *v);

// Logs every real glDepthRangef call (args + frame) -- log_075's gl_diag3
// showed GL_DEPTH_RANGE collapsed to (1.0, 1.0) from the moment the 3D
// character preview starts drawing, staying that way for the rest of the
// captured log. Combined with the GLES2-default GL_LESS depth func and a
// depth buffer cleared to 1.0, every fragment's window-space depth would
// also come out as exactly 1.0 -- never "less than" the already-1.0 cleared
// buffer, so the depth test silently discards the draw's entire color
// output. This wrapper confirms whether the engine itself requests that
// range (and from where/how often), instead of guessing from the aftermath.
// Wired into dynlib.c in place of the raw glDepthRangef entry point.
void glDepthRangef_soloader(GLclampf n, GLclampf f);

// DOWNSAMPLE_RENDER only: redirects every glBindFramebuffer(GL_FRAMEBUFFER, 0)
// (the engine asking for "the default/window framebuffer") to our reduced-
// resolution offscreen FBO instead, so the entire scene renders smaller
// without the engine knowing -- gl_swap() then blits that FBO up to the real
// default framebuffer with GL_LINEAR filtering before the real
// eglSwapBuffers. A no-op passthrough to the real glBindFramebuffer when
// DOWNSAMPLE_RENDER is off, or when the engine binds any framebuffer other
// than 0 (e.g. its own render-to-texture passes, which should stay exactly
// as they are). Wired into dynlib.c in place of the raw glBindFramebuffer
// entry point.
void glBindFramebuffer_soloader(GLenum target, GLuint framebuffer);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GLUTIL_H
