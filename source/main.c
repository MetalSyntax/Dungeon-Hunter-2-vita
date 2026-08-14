/*
 * main.c
 *
 * Dungeon Hunter 2 (Gameloft) -- ARMv7 shared library loader.
 */

/**
 * @file  main.c
 * @brief Main entry point and loader initialization sequence for PS Vita.
 * @details Refer to technical documentation in Docs/main_comments.md for details on
 *          overclocking, JNI sequence, touch/physical controls mapping, and render loop.
 */

#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/dialog.h"
#include "video.h"
#include "audio.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <psp2/rtc.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/touch.h>
#include <psp2/power.h>

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

/**
 * @brief Function pointers to native methods exported by libDungeonHunter2.so.
 */
static void (* nativeSetPhone)(JNIEnv *env, jobject clazz, int w, int h);
static void (* nativeGetInfo)(JNIEnv *env, jobject clazz, jstring s1, jstring s2, jstring s3, jstring s4);
static void (* nativeGameRenderer)(JNIEnv *env, jobject clazz);
static void (* nativeConfig)(JNIEnv *env, jobject clazz);
static void (* nativeGetJNIEnv)(JNIEnv *env, jobject clazz);
static void (* nativeInit)(JNIEnv *env, jobject clazz, int is_demo);
static void (* nativeRendererInit)(JNIEnv *env, jobject clazz, int always1);
static void (* nativeGLMediaPlayerInit)(JNIEnv *env, jobject clazz);
static void (* nativeGLResLoaderInit)(JNIEnv *env, jobject clazz, jint i);
static void (* nativeMusicplayerInit)(JNIEnv *env, jobject clazz);
static void (* nativeGLUtilsDeviceInit)(JNIEnv *env, jobject clazz);
static void (* nativeOnSurfaceChanged)(JNIEnv *env, jobject clazz, int w, int h);
static void (* nativeRender)(JNIEnv *env, jobject clazz);
static void (* nativeKeyDown)(JNIEnv *env, jobject clazz, int keycode);
static void (* nativeKeyUp)(JNIEnv *env, jobject clazz, int keycode);
static void (* nativeOnTouch)(JNIEnv *env, jobject clazz, int type, int x, int y, long long pointer_id, int unused1, int unused2);
static void (* nativePause)(JNIEnv *env, jobject clazz, int always1);
static void (* nativeResume)(JNIEnv *env, jobject clazz, int always1);
static int (* nativeCanInterrupt)(JNIEnv *env, jobject clazz);

#define AKEYCODE_BACK         4
#define AKEYCODE_DPAD_UP      19
#define AKEYCODE_DPAD_DOWN    20
#define AKEYCODE_DPAD_LEFT    21
#define AKEYCODE_DPAD_RIGHT   22
#define AKEYCODE_DPAD_CENTER  23
#define AKEYCODE_MENU         82

/**
 * @brief Mapping physical D-Pad and menu buttons to Android KeyEvents.
 */
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

/**
 * @brief Mapping physical action buttons to synthetic touch presses on HUD.
 */
static const struct { unsigned int btn; int x; int y; long long pointer_id; const char *name; } action_btn_map[] = {
    { SCE_CTRL_CROSS,    180, 445, 1, "plain red sphere, bottom-left (likely: primary attack)" },
    { SCE_CTRL_SQUARE,   683, 373, 2, "sword+flame icon (likely: block/heavy attack)" },
    { SCE_CTRL_TRIANGLE, 760, 453, 3, "sword+helmet icon, bottom-right corner, biggest (likely: dodge/special)" },
    { SCE_CTRL_L1,       810, 273, 4, "gold rune-wheel icon, mid-right (UNCONFIRMED -- not clearly a skill slot)" },
    { SCE_CTRL_R1,       820,  50, 5, "health potion quick-use icon, top-right (stopgap, not a combat 'power')" },
};
#define ACTION_BTN_MAP_COUNT (sizeof(action_btn_map) / sizeof(action_btn_map[0]))

static void *app_singleton_inst;
static void (* SavegameManager_setLanguage)(void *this_, int lang);
#define LANGUAGE_FORCE_FRAMES 180
#define LANGUAGE_ENGLISH 0

static void *so_sym_or_warn(const char *name) {
    void *addr = (void *) so_symbol(&so_mod, name);
    if (!addr) {
        l_warn("Symbol not found (may be genuinely unused by this build): %s", name);
    }
    return addr;
}

int main() {
    /**
     * @brief Hardware clock configuration at maximum nominal Vita limits.
     */
    {
        int armRet = scePowerSetArmClockFrequency(444);
        int busRet = scePowerSetBusClockFrequency(222);
        int gpuRet = scePowerSetGpuClockFrequency(222);
        int gpuXbarRet = scePowerSetGpuXbarClockFrequency(166);
        l_error("[clock] ARM=444MHz(0x%08X) BUS=222MHz(0x%08X) GPU=222MHz(0x%08X) GPU_XBAR=166MHz(0x%08X)",
                armRet, busRet, gpuRet, gpuXbarRet);
    }

    extern void pthread_init(void);
    pthread_init();

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

    /**
     * @note Pre-loading GPU modules (libgpu_es4_ext.suprx and libIMGEGL.suprx).
     */
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
    audio_init();

    if (nativeGetInfo) {
        nativeGetInfo(&jni, NULL, (jstring)DATA_PATH, (jstring)"EN", (jstring)"PSVita", (jstring)"Sony");
    }

    if (nativeSetPhone) nativeSetPhone(&jni, NULL, SCREEN_W, SCREEN_H);

    if (nativeGameRenderer) nativeGameRenderer(&jni, NULL);
    if (nativeConfig) nativeConfig(&jni, NULL);

    if (nativeGetJNIEnv) nativeGetJNIEnv(&jni, NULL);
    if (nativeGLMediaPlayerInit) nativeGLMediaPlayerInit(&jni, NULL);
    if (nativeGLResLoaderInit) nativeGLResLoaderInit(&jni, NULL, 0);
    if (nativeMusicplayerInit) nativeMusicplayerInit(&jni, NULL);
    if (nativeGLUtilsDeviceInit) nativeGLUtilsDeviceInit(&jni, NULL);

    if (nativeInit) nativeInit(&jni, NULL, 0);
    if (nativeRendererInit) nativeRendererInit(&jni, NULL, 1);

    if (nativeOnSurfaceChanged) nativeOnSurfaceChanged(&jni, NULL, SCREEN_W, SCREEN_H);

    l_success("Starting main loop...");
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    SceCtrlData pad;
    SceTouchData touch;
    unsigned int old_buttons = 0;
    int last_touch = 0;
    int last_tx = 0, last_ty = 0;
    int pending_key_down = -1, pending_key_up = -1;

    uint64_t last_touch_down_us = 0;

    /**
     * @brief Main event handling and rendering loop.
     */
    while (1) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        sceCtrlPeekBufferPositive(0, &pad, 1);
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

        if ((pad.buttons & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT)) break;

        unsigned int pressed = pad.buttons & ~old_buttons;
        unsigned int released = old_buttons & ~pad.buttons;
        old_buttons = pad.buttons;

        for (int i = 0; i < BTN_MAP_COUNT; i++) {
            if (pressed & btn_map[i].btn) pending_key_down = btn_map[i].keycode;
            if (released & btn_map[i].btn) pending_key_up = btn_map[i].keycode;
        }

        SceRtcTick now_tick;
        sceRtcGetCurrentTick(&now_tick);

        for (int i = 0; i < ACTION_BTN_MAP_COUNT; i++) {
            int lx, ly;
            if (!glutil_screen_touch_to_logical(action_btn_map[i].x, action_btn_map[i].y, &lx, &ly)) {
                lx = action_btn_map[i].x; ly = action_btn_map[i].y;
            }
            if (pressed & action_btn_map[i].btn) {
                if (now_tick.tick - last_touch_down_us >= 250000) { // 250ms debounce
                    last_touch_down_us = now_tick.tick;
                    l_debug("action_btn: synthetic touch DOWN (%d,%d)->(%d,%d) ptr=%lld [%s]",
                            action_btn_map[i].x, action_btn_map[i].y, lx, ly, action_btn_map[i].pointer_id, action_btn_map[i].name);
                    if (nativeOnTouch) nativeOnTouch(&jni, NULL, 1, lx, ly,
                                                      action_btn_map[i].pointer_id, 0, 0);
                }
            }
            if (released & action_btn_map[i].btn) {
                l_debug("action_btn: synthetic touch UP (%d,%d)->(%d,%d) ptr=%lld [%s]",
                        action_btn_map[i].x, action_btn_map[i].y, lx, ly, action_btn_map[i].pointer_id, action_btn_map[i].name);
                if (nativeOnTouch) nativeOnTouch(&jni, NULL, 0, lx, ly,
                                                  action_btn_map[i].pointer_id, 0, 0);
            }
        }

        if (touch.reportNum > 0) {
            int phys_x = touch.report[0].x * SCREEN_W / 1920;
            int phys_y = touch.report[0].y * SCREEN_H / 1088;
            int x, y;
            if (glutil_screen_touch_to_logical(phys_x, phys_y, &x, &y)) {
                if (!last_touch) {
                    if (now_tick.tick - last_touch_down_us >= 250000) { // 250ms debounce
                        last_touch_down_us = now_tick.tick;
                        if (nativeOnTouch) nativeOnTouch(&jni, NULL, 1, x, y, 0, 0, 0);
                        last_touch = 1;
                    }
                } else if (x != last_tx || y != last_ty) {
                    if (nativeOnTouch) nativeOnTouch(&jni, NULL, 2, x, y, 0, 0, 0);
                }
                last_tx = x; last_ty = y;
            } else if (last_touch) {
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
