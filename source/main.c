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
#include <psp2/touch.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

int _newlib_heap_size_user = 256 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
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
static void (* nativeSetPhone)(int w, int h);
// GameRenderer.java: public native void nativeGameRenderer(); (instance, ctor)
static void (* nativeGameRenderer)(void);
// GameRenderer.java: public static native void nativeConfig(); (ctor, after nativeGameRenderer)
static void (* nativeConfig)(void);
// GameRenderer.java: public native void nativeGetJNIEnv(); (instance, onSurfaceCreated)
static void (* nativeGetJNIEnv)(void);
// DungeonHunter2.java: public static native void nativeInit(int isDemo);
static void (* nativeInit)(int is_demo);
// GameRenderer.java: public static native void nativeInit(int always1); -- SAME
// Java method name as DungeonHunter2.nativeInit but a DIFFERENT JNI symbol
// (different class), always called with a hardcoded 1 in GameRenderer.onSurfaceCreated().
static void (* nativeRendererInit)(int always1);
// GameRenderer.java: public native void nativeOnSurfaceChanged(int w, int h);
static void (* nativeOnSurfaceChanged)(int w, int h);
// GameRenderer.java: public static native void nativeRender();
// NOTE: nativeOnDrawFrame is also exported by the .so but GameRenderer.onDrawFrame()
// never calls it -- only nativeRender(). Do not implement/call nativeOnDrawFrame.
static void (* nativeRender)(void);
// DungeonHunter2.java: public static native void nativeKeyDown(int keyCode);
static void (* nativeKeyDown)(int keycode);
// DungeonHunter2.java: public static native void nativeKeyUp(int keyCode);
static void (* nativeKeyUp)(int keycode);
// GameGLSurfaceView.java: public static native void nativeOnTouch(int type, int x, int y, long pointerId, int, int);
// type: 1=down, 2=move, 0=up (see GameGLSurfaceView.onTouchEvent). The last
// two int params are always passed as literal 0 by the original Java in every
// call site -- unused by this build, kept for ABI compatibility.
static void (* nativeOnTouch)(int type, int x, int y, long long pointer_id, int unused1, int unused2);
// DungeonHunter2.java: public static native void nativePause(int always1);
static void (* nativePause)(int always1);
// DungeonHunter2.java: public static native void nativeResume(int always1);
static void (* nativeResume)(int always1);
// DungeonHunter2.java: public static native int nativeCanInterrupt();
static int (* nativeCanInterrupt)(void);

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
    soloader_init_all();

    l_success("Resolving Dungeon Hunter 2 native entry points...");
    nativeSetPhone         = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_DungeonHunter2_nativeSetPhone");
    nativeGameRenderer     = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_GameRenderer_nativeGameRenderer");
    nativeConfig           = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_GameRenderer_nativeConfig");
    nativeGetJNIEnv        = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_GameRenderer_nativeGetJNIEnv");
    nativeInit             = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_DungeonHunter2_nativeInit");
    nativeRendererInit     = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_GameRenderer_nativeInit");
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

    gl_init();
    l_success("vitaGL initialized.");

    // Real Android sequence (DungeonHunter2.onCreate): nativeSetPhone is
    // called with the ACTUAL screen pixel dimensions before the
    // GLSurfaceView/GameRenderer are created -- there is no separate fixed
    // internal framebuffer resolution like the Gamevil Nexus2 engine used by
    // the sibling Zenonia ports. Passing the Vita's native resolution here
    // means touch coordinates fed to nativeOnTouch below must also be in
    // this same 960x544 space (Phase 3 TODO: confirm on first real run --
    // if the UI/hit-testing looks wrong, this is the first assumption to
    // revisit).
    if (nativeSetPhone) nativeSetPhone(SCREEN_W, SCREEN_H);

    // GameRenderer(Context) ctor: nativeGameRenderer() then nativeConfig(), in that order.
    if (nativeGameRenderer) nativeGameRenderer();
    if (nativeConfig) nativeConfig();

    // GameRenderer.onSurfaceCreated(): nativeGetJNIEnv(), then
    // DungeonHunter2.nativeInit(isDemo), then GameRenderer.nativeInit(1).
    // isDemo() reads a SharedPreferences flag that gates the purchased/full
    // version -- this port always requests the full game (argument 0), since
    // there is no purchase flow to unlock it via.
    if (nativeGetJNIEnv) nativeGetJNIEnv();
    if (nativeInit) nativeInit(0);
    if (nativeRendererInit) nativeRendererInit(1);

    // GameRenderer.onSurfaceChanged(): called once after onSurfaceCreated
    // with the real surface size, and again on any resize (never, for us).
    if (nativeOnSurfaceChanged) nativeOnSurfaceChanged(SCREEN_W, SCREEN_H);

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
                if (nativeOnTouch) nativeOnTouch(1, x, y, 0, 0, 0);
                last_touch = 1;
            } else if (x != last_tx || y != last_ty) {
                if (nativeOnTouch) nativeOnTouch(2, x, y, 0, 0, 0);
            }
            last_tx = x; last_ty = y;
        } else if (last_touch) {
            if (nativeOnTouch) nativeOnTouch(0, last_tx, last_ty, 0, 0, 0);
            last_touch = 0;
        }

        if (pending_key_down != -1 && nativeKeyDown) { nativeKeyDown(pending_key_down); pending_key_down = -1; }
        if (pending_key_up != -1 && nativeKeyUp) { nativeKeyUp(pending_key_up); pending_key_up = -1; }

        if (nativeRender) nativeRender();

        gl_swap();
    }

    if (nativePause && nativeCanInterrupt) {
        while (nativeCanInterrupt() == 0) {
            sceKernelDelayThread(10 * 1000);
        }
        nativePause(1);
    }

    sceKernelExitDeleteThread(0);
    return 0;
}
