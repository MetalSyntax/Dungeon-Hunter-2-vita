/*
 * main.c
 *
 * Dungeon Hunter 2 (Gameloft) -- ARMv7 shared library loader.
 *
 * Only libDungeonHunter2.so is loaded (see CMakeLists.txt SO_PATH and
 * PORTING_PLAN.md Phase 1/2): libStormGLOFT.so is Gameloft's ARM/THUMB
 * inline-hooking anti-tamper layer (no JNI exports, not a renderer despite
 * the name) and libnativeinterface.so is unrelated Samsung Zirconia DRM --
 * neither is needed for gameplay.
 *
 * Init sequence and per-frame call order below are transcribed directly from
 * the real Java (DungeonHunter2.java, GameGLSurfaceView.java, GameRenderer.java
 * in decompiled/apk_jadx/sources/com/gameloft/android/GAND/GloftD2SS/), not
 * assumed from the cocos2d-x/Gamevil Nexus2 patterns used by the sibling
 * ports in this workspace -- this is a different, proprietary Gameloft engine.
 */

#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/dialog.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <psp2/rtc.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/touch.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <unistd.h>

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include "utils/dialog.h"
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>

int _newlib_heap_size_user = 256 * 1024 * 1024; // 256 MB

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 32 * 1024 * 1024;
#endif



so_module so_mod;

// lib/falso_jni/FalsoJNI_Logger.c (vendored from the Zenonia ports, MIT,
// unmodified) logs through game_log() rather than this project's own
// utils/logger.h macros -- forward to l_info so JNI call traces still show
// up without having to fork FalsoJNI_Logger.c just for this.
void game_log(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    l_info("%s", buf);
}

#define SCREEN_W 960
#define SCREEN_H 544

// DungeonHunter2.java: static native void nativeSetPhone(int width, int height);
static void (* nativeSetPhone)(JNIEnv *env, jobject clazz, int w, int h);
static void (* nativeGetInfo)(JNIEnv *env, jobject clazz, jstring s1, jstring s2, jstring s3, jstring s4);
// GameRenderer.java: public native void nativeGameRenderer(); (instance, ctor)
static void (* nativeGameRenderer)(JNIEnv *env, jobject clazz);
// GameRenderer.java: public static native void nativeConfig(); (ctor, after nativeGameRenderer)
static void (* nativeConfig)(JNIEnv *env, jobject clazz);
// GameRenderer.java: public native void nativeGetJNIEnv(); (instance, onSurfaceCreated)
static void (* nativeGetJNIEnv)(JNIEnv *env, jobject clazz);
// DungeonHunter2.java: public static native void nativeInit(int isDemo);
static void (* nativeInit)(JNIEnv *env, jobject clazz, int is_demo);
// GameRenderer.java: public static native void nativeInit(int always1); -- SAME
// Java method name as DungeonHunter2.nativeInit but a DIFFERENT JNI symbol
// (different class), always called with a hardcoded 1 in GameRenderer.onSurfaceCreated().
static void (* nativeRendererInit)(JNIEnv *env, jobject clazz, int always1);
static void (* nativeGLMediaPlayerInit)(JNIEnv *env, jobject clazz);
static void (* nativeGLResLoaderInit)(JNIEnv *env, jobject clazz, jint i);
static void (* nativeMusicplayerInit)(JNIEnv *env, jobject clazz);
static void (* nativeGLUtilsDeviceInit)(JNIEnv *env, jobject clazz);
// GameRenderer.java: public native void nativeOnSurfaceChanged(int w, int h);
static void (* nativeOnSurfaceChanged)(JNIEnv *env, jobject clazz, int w, int h);
// GameRenderer.java: public static native void nativeRender();
// NOTE: nativeOnDrawFrame is also exported by the .so but GameRenderer.onDrawFrame()
// never calls it -- only nativeRender(). Do not implement/call nativeOnDrawFrame.
static void (* nativeRender)(JNIEnv *env, jobject clazz);
// DungeonHunter2.java: public static native void nativeKeyDown(int keyCode);
static void (* nativeKeyDown)(JNIEnv *env, jobject clazz, int keycode);
// DungeonHunter2.java: public static native void nativeKeyUp(int keyCode);
static void (* nativeKeyUp)(JNIEnv *env, jobject clazz, int keycode);
// GameGLSurfaceView.java: public static native void nativeOnTouch(int type, int x, int y, long pointerId, int, int);
// type: 1=down, 2=move, 0=up (see GameGLSurfaceView.onTouchEvent). The last
// two int params are always passed as literal 0 by the original Java in every
// call site -- unused by this build, kept for ABI compatibility.
static void (* nativeOnTouch)(JNIEnv *env, jobject clazz, int type, int x, int y, long long pointer_id, int unused1, int unused2);
// DungeonHunter2.java: public static native void nativePause(int always1);
static void (* nativePause)(JNIEnv *env, jobject clazz, int always1);
// DungeonHunter2.java: public static native void nativeResume(int always1);
static void (* nativeResume)(JNIEnv *env, jobject clazz, int always1);
// DungeonHunter2.java: public static native int nativeCanInterrupt();
static int (* nativeCanInterrupt)(JNIEnv *env, jobject clazz);

// Android KeyEvent codes -- confirmed from DungeonHunter2.onKeyDown/onKeyUp:
// the Java forwards the RAW keyCode int to nativeKeyDown/Up (no translation),
// so the .so expects real android.view.KeyEvent.KEYCODE_* values.
#define AKEYCODE_BACK         4
#define AKEYCODE_DPAD_UP      19
#define AKEYCODE_DPAD_DOWN    20
#define AKEYCODE_DPAD_LEFT    21
#define AKEYCODE_DPAD_RIGHT   22
#define AKEYCODE_DPAD_CENTER  23
#define AKEYCODE_MENU         82

// Best-effort physical button mapping (Phase 6 TODO in PORTING_PLAN.md: this
// engine's action-button semantics -- attack/skill/inventory -- are not yet
// confirmed from the UI Java classes, only the D-pad/back/menu keycodes
// DungeonHunter2.java itself reads are confirmed here).
static const struct { unsigned int btn; int keycode; } btn_map[] = {
    { SCE_CTRL_UP,       AKEYCODE_DPAD_UP },
    { SCE_CTRL_DOWN,     AKEYCODE_DPAD_DOWN },
    { SCE_CTRL_LEFT,     AKEYCODE_DPAD_LEFT },
    { SCE_CTRL_RIGHT,    AKEYCODE_DPAD_RIGHT },
    { SCE_CTRL_CROSS,    AKEYCODE_DPAD_CENTER },
    { SCE_CTRL_CIRCLE,   AKEYCODE_BACK },
    { SCE_CTRL_START,    AKEYCODE_MENU },
};
#define BTN_MAP_COUNT (sizeof(btn_map) / sizeof(btn_map[0]))

static void *so_sym_or_warn(const char *name) {
    void *addr = (void *) so_symbol(&so_mod, name);
    if (!addr) {
        l_warn("Symbol not found (may be genuinely unused by this build): %s", name);
    }
    return addr;
}

int main() {
    // Explicitly initialize pthread to avoid EAGAIN (error 11) on pthread_create.
    // The linker sometimes strips the constructor in pthr.c.
    extern void pthread_init(void);
    pthread_init();

    // Set the current working directory so relative fopens (e.g., effects.bdae) succeed
    chdir(DATA_PATH "assets/");

    soloader_init_all();

    l_success("Resolving Dungeon Hunter 2 native entry points...");
    nativeSetPhone         = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_DungeonHunter2_nativeSetPhone");
    nativeGameRenderer     = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_GameRenderer_nativeGameRenderer");
    nativeGetInfo          = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_DungeonHunter2_nativeGetInfo");
    nativeConfig           = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_GameRenderer_nativeConfig");
    nativeGetJNIEnv        = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_GameRenderer_nativeGetJNIEnv");
    nativeInit             = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_DungeonHunter2_nativeInit");
    nativeRendererInit     = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_GameRenderer_nativeInit");
    nativeGLMediaPlayerInit = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_GLMediaPlayer_nativeInit");
    nativeGLResLoaderInit   = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_GLResLoader_nativeInit");
    nativeMusicplayerInit   = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_Musicplayer_nativeInitplayer");
    nativeGLUtilsDeviceInit = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_GLUtils_Device_nativeInit");
    nativeOnSurfaceChanged = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_GameRenderer_nativeOnSurfaceChanged");
    nativeRender           = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_GameRenderer_nativeRender");
    nativeKeyDown          = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_DungeonHunter2_nativeKeyDown");
    nativeKeyUp            = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_DungeonHunter2_nativeKeyUp");
    nativeOnTouch          = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_GameGLSurfaceView_nativeOnTouch");
    nativePause            = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_DungeonHunter2_nativePause");
    nativeResume           = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_DungeonHunter2_nativeResume");
    nativeCanInterrupt     = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_DungeonHunter2_nativeCanInterrupt");

    int (* JNI_OnLoad)(void *jvm) = (void *) so_symbol(&so_mod, "JNI_OnLoad");
    if (!JNI_OnLoad) {
        fatal_error("JNI_OnLoad not found in libDungeonHunter2.so -- wrong/corrupt file?");
    }
    l_success("Calling JNI_OnLoad...");
    JNI_OnLoad(&jvm);

    // sceKernelLoadStartModule returns a SceUID (>=0) on success or a negative
    // SCE error (esp. 0x8002D082 "already loaded" is OK, 0x8002D080 "not found"
    // means the .suprx is missing on-device -- the fast-deploy paths push only
    // eboot.bin, so the modules must come from a full VPK install).
    //
    // Only preload libgpu_es4_ext.suprx and libIMGEGL.suprx here, from app0:
    // root -- this matches GrapheneCt/PVR_PSP2's own reference test
    // (unittests/gles1test1/gles1test1.c), which declares exactly these two
    // as its SCE_USER_MODULE_LIST and nothing else. libIMGEGL loads
    // libpvrPSP2_WSEGL/libGLESv1_CM/libGLESv2 ITSELF, lazily, the first time
    // eglGetDisplay/eglInitialize needs them, using the paths already baked
    // into PVRSRVInitializeAppHint's defaults ("app0:module/lib*.suprx" --
    // see pvr_apphint.c in that repo) which glutil.c's AppHint setup mirrors.
    // Preloading those 3 ourselves (as an earlier version of this code did)
    // made libIMGEGL's own internal (re-)load of the same named module fail
    // -- its loader (PVRSRVLoadLibrary/LoadNamedWSModule) has no "already
    // loaded is fine" fallback, any failure there is fatal, and eglGetDisplay
    // surfaces it as EGL_NO_DISPLAY with the misleading EGL_SUCCESS (0x3000)
    // code. Confirmed on real hardware (log_054.txt, 2026-07-18): all 5
    // manual LoadStartModule calls succeeded and PVRSRVCreateVirtualAppHint
    // succeeded, yet eglGetDisplay still failed -- this double-load is the
    // remaining suspect now that the module/ vs modules/ naming is settled.
    static const char *pvr_modules[] = {
        "app0:libgpu_es4_ext.suprx",
        "app0:libIMGEGL.suprx",
    };
    for (int i = 0; i < (int)(sizeof(pvr_modules) / sizeof(pvr_modules[0])); ++i) {
        int ret = sceKernelLoadStartModule(pvr_modules[i], 0, NULL, 0, NULL, NULL);
        if (ret < 0) {
            l_error("LoadStartModule(%s) FAILED: 0x%08X", pvr_modules[i], ret);
        } else {
            l_success("LoadStartModule(%s) -> uid 0x%08X", pvr_modules[i], ret);
        }
    }

    gl_init();
    l_success("PVR_PSP2 initialized.");

    // Real Android sequence (DungeonHunter2.onCreate): nativeSetPhone is
    // called with the ACTUAL screen pixel dimensions before the
    // GLSurfaceView/GameRenderer are created -- there is no separate fixed
    // internal framebuffer resolution like the Gamevil Nexus2 engine used by
    // the sibling Zenonia ports. Passing the Vita's native resolution here
    // means touch coordinates fed to nativeOnTouch below must also be in
    // this same 960x544 space (Phase 3 TODO: confirm on first real run --
    // if the UI/hit-testing looks wrong, this is the first assumption to
    // revisit).
    if (nativeGetInfo) {
        // nativeGetInfo(String path, String lang, String device, String manufacturer)
        nativeGetInfo(&jni, NULL, (jstring)DATA_PATH, (jstring)"EN", (jstring)"PSVita", (jstring)"Sony");
    }

    if (nativeSetPhone) nativeSetPhone(&jni, NULL, SCREEN_W, SCREEN_H);

    // GameRenderer(Context) ctor: nativeGameRenderer() then nativeConfig(), in that order.
    if (nativeGameRenderer) nativeGameRenderer(&jni, NULL);
    if (nativeConfig) nativeConfig(&jni, NULL);

    // GameRenderer.onSurfaceCreated(): nativeGetJNIEnv(), then
    // DungeonHunter2.nativeInit(isDemo), then GameRenderer.nativeInit(1).
    // isDemo() reads a SharedPreferences flag that gates the purchased/full
    // version -- this port always requests the full game (argument 0), since
    // there is no purchase flow to unlock it via.
    if (nativeGetJNIEnv) nativeGetJNIEnv(&jni, NULL);
    // JNI initialization sequence for all modules to cache method IDs
    if (nativeGLMediaPlayerInit) nativeGLMediaPlayerInit(&jni, NULL);
    if (nativeGLResLoaderInit) nativeGLResLoaderInit(&jni, NULL, 0);
    if (nativeMusicplayerInit) nativeMusicplayerInit(&jni, NULL);
    if (nativeGLUtilsDeviceInit) nativeGLUtilsDeviceInit(&jni, NULL);

    if (nativeInit) nativeInit(&jni, NULL, 0); // DungeonHunter2.nativeInit
    if (nativeRendererInit) nativeRendererInit(&jni, NULL, 1);

    // GameRenderer.onSurfaceChanged(): called once after onSurfaceCreated
    // with the real surface size, and again on any resize (never, for us).
    if (nativeOnSurfaceChanged) nativeOnSurfaceChanged(&jni, NULL, SCREEN_W, SCREEN_H);

    l_success("Starting main loop...");
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    SceCtrlData pad;
    SceTouchData touch;
    unsigned int old_buttons = 0;
    int last_touch = 0;
    int last_tx = 0, last_ty = 0;
    int pending_key_down = -1, pending_key_up = -1;

    while (1) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

        // Emergency exit: START+SELECT together.
        if ((pad.buttons & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT)) break;

        unsigned int pressed = pad.buttons & ~old_buttons;
        unsigned int released = old_buttons & ~pad.buttons;
        old_buttons = pad.buttons;

        // Single most-recent-key queue, same protocol as
        // GameRenderer.onDrawFrame (DungeonHunter2.p/q consumed once per
        // frame then reset to -1) -- see GameRenderer.java.
        for (int i = 0; i < BTN_MAP_COUNT; i++) {
            if (pressed & btn_map[i].btn) pending_key_down = btn_map[i].keycode;
            if (released & btn_map[i].btn) pending_key_up = btn_map[i].keycode;
        }

        // Touch panel is 1920x1088 (2x the 960x544 native resolution) --
        // scale down to match the SCREEN_W/H space nativeSetPhone was told about.
        if (touch.reportNum > 0) {
            int x = touch.report[0].x * SCREEN_W / 1920;
            int y = touch.report[0].y * SCREEN_H / 1088;
            if (!last_touch) {
                if (nativeOnTouch) nativeOnTouch(&jni, NULL, 1, x, y, 0, 0, 0);
                last_touch = 1;
            } else if (x != last_tx || y != last_ty) {
                if (nativeOnTouch) nativeOnTouch(&jni, NULL, 2, x, y, 0, 0, 0);
            }
            last_tx = x; last_ty = y;
        } else if (last_touch) {
            if (nativeOnTouch) nativeOnTouch(&jni, NULL, 0, last_tx, last_ty, 0, 0, 0);
            last_touch = 0;
        }

        if (pending_key_down != -1 && nativeKeyDown) { nativeKeyDown(&jni, NULL, pending_key_down); pending_key_down = -1; }
        if (pending_key_up != -1 && nativeKeyUp) { nativeKeyUp(&jni, NULL, pending_key_up); pending_key_up = -1; }

        if (nativeRender) nativeRender(&jni, NULL);

        // Diagnostic: the game reaches its main loop and runs its update/menu
        // logic, but nothing draws (black screen). Log any accumulated GL error
        // roughly once per second so we can tell a broken GL pipeline apart
        // from a "runs fine but produces nothing visible" content problem,
        // without flooding the log every frame.
        {
            static int diag_frame = 0;
            if ((diag_frame++ % 60) == 0) {
                GLenum err = glGetError();
                if (err != GL_NO_ERROR) {
                    l_warn("[gl_diag] frame %d: glGetError() = 0x%04x", diag_frame, err);
                } else {
                    l_info("[gl_diag] frame %d: GL pipeline clean (no error)", diag_frame);
                }
            }
        }

        gl_swap();
    }

    if (nativePause && nativeCanInterrupt) {
        while (nativeCanInterrupt(&jni, NULL) == 0) {
            sceKernelDelayThread(10 * 1000);
        }
        nativePause(&jni, NULL, 1);
    }

    sceKernelExitDeleteThread(0);
    return 0;
}
