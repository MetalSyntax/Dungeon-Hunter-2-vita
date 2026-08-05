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
#include "video.h"

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
#include <stdint.h>
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

// RULED OUT, do not re-attempt without new evidence: standard Android
// gamepad KeyEvent codes (BUTTON_A/X/Y/L1/R1) sent via nativeKeyDown/Up,
// modeled after the sibling Prince-of-Persia port where this genuinely works
// for its cocos2d-x build. Traced DH2's own nativeKeyDown all the way
// through in out_ghidra.c: Java_..._DungeonHunter2_nativeKeyDown ->
// appKeyPressed(void) -- and appKeyPressed is a complete no-op stub in this
// compiled .so (just `_DEBUG_OUT("keypresseddddddddddddddddddddddddd %d",
// lastOpenMenuID); return;`, doesn't even reference its keycode argument).
// This means NO keycode -- D-Pad, BACK, MENU, or any BUTTON_* value -- can
// ever drive gameplay through this entry point, regardless of which int we
// send. The D-Pad/menu keycodes in btn_map above are kept only because they
// still reach real menu-navigation code through a DIFFERENT native function
// (nativegetState, called directly for MENU/BACK -- see
// DungeonHunter2.onKeyUp in the decompiled Java), not through appKeyPressed.
// The only real gameplay input path in this engine is appOnTouch (fully
// implemented, dispatches to real vtable methods) -- i.e. nativeOnTouch,
// which action_btn_map below already uses. If Cross/Square still do nothing
// through touch, the bug is in the touch coordinates/protocol, not in
// choosing touch over keycodes.

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

// DH2's actual combat actions (attack/dodge/block + equipped skills) are
// 100% touch-driven HUD buttons drawn by dqhud.swf -- confirmed via
// GameGLSurfaceView.java, which forwards raw MotionEvent coordinates
// straight into nativeOnTouch with no scaling and no engine-side gamepad
// path reachable on this Android build (InputManager::GetFirstConnectedGamepad()
// is real native code, but nothing in the decompiled Java ever registers a
// gamepad -- no MOGA/InputDevice/KEYCODE_BUTTON_* anywhere -- so it always
// returns null here; not worth hooking). So physical buttons are wired as
// synthetic taps at each HUD button's on-screen coordinate instead of a
// key-code path.
//
// Coordinates below are measured directly off a real Vita screenshot
// (2026-07-23-225855.jpg, our actual 960x544 output, letterboxed bars and
// all) by cropping each icon and computing its center -- NOT estimated from
// the earlier iPhone reference anymore, so these should be exact for
// whatever we're currently rendering.
//
// That real capture only shows THREE distinct combat-style buttons (the
// plain red sphere bottom-left, a sword+flame icon, and a bigger sword+
// helmet icon in the bottom-right cluster) -- no second/third skill icon
// like the iPhone reference had. Most likely this savegame just hasn't
// unlocked/equipped any active skills yet, so those slots aren't drawn.
// L1/R1 below are wired to the two other real icons on screen (the gold
// rune-wheel and the health-potion quick-use) as a stopgap so the buttons
// aren't left idle -- neither is confirmed to be a "power" slot. Re-screenshot
// once skills are equipped so L1/R1 can be pointed at the real skill icons.
//
// Each button gets its own fixed pointer_id (1-5, real touch panel uses 0)
// so a physical-button tap never collides with a real finger on the touch
// panel or with another physical button held at the same time.
static const struct { unsigned int btn; int x; int y; long long pointer_id; const char *name; } action_btn_map[] = {
    { SCE_CTRL_CROSS,    180, 445, 1, "plain red sphere, bottom-left (likely: primary attack)" },
    { SCE_CTRL_SQUARE,   683, 373, 2, "sword+flame icon (likely: block/heavy attack)" },
    { SCE_CTRL_TRIANGLE, 760, 453, 3, "sword+helmet icon, bottom-right corner, biggest (likely: dodge/special)" },
    { SCE_CTRL_L1,       810, 273, 4, "gold rune-wheel icon, mid-right (UNCONFIRMED -- not clearly a skill slot)" },
    { SCE_CTRL_R1,       820,  50, 5, "health potion quick-use icon, top-right (stopgap, not a combat 'power')" },
};
#define ACTION_BTN_MAP_COUNT (sizeof(action_btn_map) / sizeof(action_btn_map[0]))

// Force English regardless of whatever language got persisted into
// dh2_settings.savegame on an earlier run (this build's copy of that file
// on the test device was stuck loading the ".german"-suffixed text pack
// rather than English, even though our own Get_PhoneLanguage stub already
// correctly returns 0/English -- see java.c -- because the engine only
// re-derives language from Get_PhoneLanguage on a genuinely fresh/first-ever
// settings file, not on every boot). Confirmed via real exported symbols
// (not Ghidra placeholders): every SavegameManager::setLanguage call site in
// the real .so operates on the SavegameManager* stored at byte offset 76
// inside the Singleton<Application> instance (a real global object, not a
// pointer-to-pointer):
//   Singleton<Application>::s_inst -- _ZN9SingletonI11ApplicationE6s_instE
//   SavegameManager::setLanguage(int) -- _ZN15SavegameManager11setLanguageEi
// Applied every frame for a short window after boot (rather than once at a
// guessed frame number) because GSInit's own settings-load and
// StringManager::switchPack steps are both driven by its internal multi-frame
// step machine -- calling this after every nativeRender() for the first
// LANGUAGE_FORCE_FRAMES frames guarantees our override always lands after
// whatever GSInit did that frame, regardless of exactly which frame each
// step happens to run on.
static void *app_singleton_inst;
static void (* SavegameManager_setLanguage)(void *this_, int lang);
#define LANGUAGE_FORCE_FRAMES 180
#define LANGUAGE_ENGLISH 0

// RULED OUT, reverted (see the appKeyPressed finding below in
// action_btn_map's comment area): forcing DH2's "DPad" saved option (via
// SavegameManager::setOption, same mechanism as setLanguage above) once
// looked like a legitimate way to get digital movement through the engine's
// own control path -- Application::IsUsingDPad()'s readback DID confirm the
// write took (log_077: "readback after forcing DPad=1: 1"), but D-Pad
// movement still never worked (log_083), and tracing WHY revealed
// nativeKeyDown's real handler (appKeyPressed) is a no-op stub in this
// compiled .so -- no keycode can ever drive movement here, D-Pad or
// otherwise. Worse, forcing "DPad"=1 is actively counterproductive now:
// Character::Ctrl_Click (out_ghidra.c) skips its click-to-move/auto-target
// object-iteration loop specifically WHEN IsUsingDPad() is true, on the
// assumption that a real D-Pad is handling movement instead -- since ours
// never can, forcing this option risked disabling whatever touch-based
// movement/targeting DOES work, for zero benefit. Kept only as a comment so
// a future session doesn't reintroduce it without the appKeyPressed context.

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
    app_singleton_inst     = so_sym_or_warn("_ZN9SingletonI11ApplicationE6s_instE");
    SavegameManager_setLanguage = so_sym_or_warn("_ZN15SavegameManager11setLanguageEi");

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

    video_init();

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

        // Combat action buttons: synthesize a touch-down at the mapped HUD
        // button's coordinate on press, touch-up on release -- mirrors a
        // real finger holding that button (matters for anything that reacts
        // to hold-vs-tap). See action_btn_map's comment for the coordinate
        // caveats.
        for (int i = 0; i < ACTION_BTN_MAP_COUNT; i++) {
            // action_btn_map's coordinates are measured off a real physical-
            // screen screenshot, but nativeOnTouch hit-tests against the
            // engine's logical 960x640 canvas -- convert through the same
            // letterbox inverse as real touches (see glutil_screen_touch_to_logical).
            int lx, ly;
            if (!glutil_screen_touch_to_logical(action_btn_map[i].x, action_btn_map[i].y, &lx, &ly)) {
                lx = action_btn_map[i].x; ly = action_btn_map[i].y;
            }
            if (pressed & action_btn_map[i].btn) {
                l_debug("action_btn: synthetic touch DOWN (%d,%d)->(%d,%d) ptr=%lld [%s]",
                        action_btn_map[i].x, action_btn_map[i].y, lx, ly, action_btn_map[i].pointer_id, action_btn_map[i].name);
                if (nativeOnTouch) nativeOnTouch(&jni, NULL, 1, lx, ly,
                                                  action_btn_map[i].pointer_id, 0, 0);
            }
            if (released & action_btn_map[i].btn) {
                l_debug("action_btn: synthetic touch UP (%d,%d)->(%d,%d) ptr=%lld [%s]",
                        action_btn_map[i].x, action_btn_map[i].y, lx, ly, action_btn_map[i].pointer_id, action_btn_map[i].name);
                if (nativeOnTouch) nativeOnTouch(&jni, NULL, 0, lx, ly,
                                                  action_btn_map[i].pointer_id, 0, 0);
            }
        }

        // Touch panel is 1920x1088 (2x the 960x544 native resolution) --
        // scale down to physical-screen space, then invert the letterbox
        // (glutil_screen_touch_to_logical) to land in the engine's logical
        // 960x640 space that nativeOnTouch hit-tests against -- without this,
        // on-screen buttons only registered when tapped off-position (below
        // and to the right of where they're actually drawn).
        if (touch.reportNum > 0) {
            int phys_x = touch.report[0].x * SCREEN_W / 1920;
            int phys_y = touch.report[0].y * SCREEN_H / 1088;
            int x, y;
            if (glutil_screen_touch_to_logical(phys_x, phys_y, &x, &y)) {
                if (!last_touch) {
                    if (nativeOnTouch) nativeOnTouch(&jni, NULL, 1, x, y, 0, 0, 0);
                    last_touch = 1;
                } else if (x != last_tx || y != last_ty) {
                    if (nativeOnTouch) nativeOnTouch(&jni, NULL, 2, x, y, 0, 0, 0);
                }
                last_tx = x; last_ty = y;
            } else if (last_touch) {
                // Finger slid into the pillarbox bars -- treat as a release.
                if (nativeOnTouch) nativeOnTouch(&jni, NULL, 0, last_tx, last_ty, 0, 0, 0);
                last_touch = 0;
            }
        } else if (last_touch) {
            if (nativeOnTouch) nativeOnTouch(&jni, NULL, 0, last_tx, last_ty, 0, 0, 0);
            last_touch = 0;
        }

        if (pending_key_down != -1 && nativeKeyDown) { nativeKeyDown(&jni, NULL, pending_key_down); pending_key_down = -1; }
        if (pending_key_up != -1 && nativeKeyUp) { nativeKeyUp(&jni, NULL, pending_key_up); pending_key_up = -1; }

        if (nativeRender) nativeRender(&jni, NULL);

        // Force English for the first LANGUAGE_FORCE_FRAMES frames -- see the
        // declarations above for why this can't just be done once at a fixed
        // frame number. Cheap (one function call) and self-limiting.
        {
            static int lang_force_frame = 0;
            if (lang_force_frame < LANGUAGE_FORCE_FRAMES) {
                lang_force_frame++;
                if (app_singleton_inst && SavegameManager_setLanguage) {
                    void *sgm = *(void **)((char *) app_singleton_inst + 76);
                    if (sgm) {
                        SavegameManager_setLanguage(sgm, LANGUAGE_ENGLISH);
                    }
                }
            }
        }

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
                gl_log_render_diag(diag_frame);
            }
        }

        // Always-on FPS counter (l_error, never compiled out by DEBUG_SOLOADER)
        // so real frame-rate numbers are visible in both Debug and Release
        // logs -- needed as an actual measurement to drive the performance
        // work, instead of an eyeballed "~12 FPS".
        {
            static SceRtcTick fps_last_tick;
            static int fps_frame_count = 0;
            static int fps_initialized = 0;
            SceRtcTick now;
            sceRtcGetCurrentTick(&now);
            if (!fps_initialized) {
                fps_last_tick = now;
                fps_initialized = 1;
            }
            fps_frame_count++;
            uint64_t elapsed_us = now.tick - fps_last_tick.tick;
            if (elapsed_us >= 3000000) {
                float fps = (float) fps_frame_count * 1000000.0f / (float) elapsed_us;
                l_error("[fps] %.1f frames/sec (%d frames in %.2fs)", fps, fps_frame_count, (double) elapsed_us / 1000000.0);
                fps_frame_count = 0;
                fps_last_tick = now;
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
