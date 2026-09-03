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
    { SCE_CTRL_CROSS,    850, 450, 1, "Primary attack (sword icon, bottom-right)" },
    { SCE_CTRL_SQUARE,   740, 350, 2, "Skill 1 (frost snowflake icon, middle-right)" },
    { SCE_CTRL_TRIANGLE, 905, 240, 3, "Skill 2 (golden lightning wheel icon, upper-right)" },
    { SCE_CTRL_R1,       905,  60, 4, "Health potion (red flask icon, top-right)" },
    { SCE_CTRL_L1,        75, 160, 5, "Pause button (left below portrait)" },
};
#define ACTION_BTN_MAP_COUNT (sizeof(action_btn_map) / sizeof(action_btn_map[0]))

// ---------------------------------------------------------------------------
// Stick analogico -> movimiento del personaje, SIN sintetizar touch.
//
// El motor tiene compilada una API de comandos de personaje con movimiento
// direccional POR VECTOR, que es exactamente la forma que tiene un stick. Los
// tres simbolos estan en .dynsym, asi que se resuelven con so_symbol():
//
//   Character* NativeGetPlayerChar(int idx, bool remote)   -- funcion libre
//   void Character::Ctrl_HeadTowards(const Point3D<float>&)
//   void Character::Ctrl_Stop()
//
// Ctrl_HeadTowards recibe una DIRECCION, no un destino: internamente compara
// x²+y²+z² contra ~1e-4 (test de deadzone propio), respeta el estado del
// personaje (no camina mientras castea, via SM_IsUsingSkill/SM_IsCasting) y
// normaliza el vector. Por eso la magnitud del stick NO da velocidad variable.
//
// Esto es mejor que el touch sintetico que usan los botones de accion
// (action_btn_map) porque no depende de que el clip de Flash del joystick
// virtual exista: sigue funcionando cuando se oculte el HUD.
static void *(* NativeGetPlayerChar)(int idx, int remote);
static void (* Character_Ctrl_HeadTowards)(void *this_, const float *dir);
static void (* Character_Ctrl_Stop)(void *this_);
// v2Controller::s_blocked es el gate que el motor usa para bloquear input en
// cutscenes, menus y el IGM. Llamar Ctrl_* directo se lo saltea, asi que hay
// que respetarlo a mano o el personaje camina durante los dialogos.
static unsigned char *v2Controller_s_blocked;

// La orientacion de los ejes del mundo respecto de la pantalla no se pudo
// determinar del analisis estatico (la camara es isometrica). En vez de
// adivinar y hacer varias idas y vueltas a la consola, los modos plausibles se
// pueden ciclar en vivo con SELECT y cada cambio se loguea con l_error, asi que
// una sola sesion de prueba resuelve cual es. Una vez confirmado, fijar
// STICK_MAP_DEFAULT y (si se quiere) sacar el ciclado.
#define STICK_MAP_COUNT 4
#ifndef STICK_MAP_DEFAULT
#define STICK_MAP_DEFAULT 0
#endif
static int s_stick_map = STICK_MAP_DEFAULT;
static const char *kStickMapNames[STICK_MAP_COUNT] = {
    "0: (x, 0, y)  ejes alineados, mundo Y-up",
    "1: (x, 0, -y) igual pero Z invertido",
    "2: (x, y, 0)  plano XY, mundo Z-up",
    "3: iso 45 deg  ((x-y), 0, (x+y)) * 0.7071",
};

// Ultimo estado, para llamar Ctrl_Stop una sola vez al soltar en vez de en
// cada frame.
static int s_stick_was_active = 0;

static void stick_update(const SceCtrlData *pad) {
    if (!NativeGetPlayerChar || !Character_Ctrl_HeadTowards || !Character_Ctrl_Stop) {
        return;
    }
    // Devuelve 0 mientras no haya jugador (menus, carga), asi que sirve de
    // chequeo de "estamos en gameplay" sin ningun trabajo extra.
    void *player = NativeGetPlayerChar(0, 0);
    if (!player) {
        s_stick_was_active = 0;
        return;
    }
    if (v2Controller_s_blocked && *v2Controller_s_blocked) {
        if (s_stick_was_active) {
            Character_Ctrl_Stop(player);
            s_stick_was_active = 0;
        }
        return;
    }

    // Stick de Vita: 0..255 con centro en ~128. sy positivo = ABAJO en pantalla.
    float sx = ((float) pad->lx - 128.0f) / 128.0f;
    float sy = ((float) pad->ly - 128.0f) / 128.0f;

    // Deadzone radial (no por eje): los sticks de Vita derivan bastante, y una
    // deadzone por eje deja pasar diagonales fantasma.
    const float DEADZONE = 0.28f;
    float mag2 = sx * sx + sy * sy;
    if (mag2 < DEADZONE * DEADZONE) {
        if (s_stick_was_active) {
            Character_Ctrl_Stop(player);
            s_stick_was_active = 0;
        }
        return;
    }

    float dir[3] = {0.0f, 0.0f, 0.0f};
    switch (s_stick_map) {
        case 0: dir[0] = sx; dir[2] = sy; break;
        case 1: dir[0] = sx; dir[2] = -sy; break;
        case 2: dir[0] = sx; dir[1] = sy; break;
        case 3: dir[0] = (sx - sy) * 0.7071f; dir[2] = (sx + sy) * 0.7071f; break;
        default: dir[0] = sx; dir[2] = sy; break;
    }

    Character_Ctrl_HeadTowards(player, dir);
    s_stick_was_active = 1;
}

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

// Diagnostico temporal (ver log_159.txt): el main loop se cuelga por completo
// (0 frames mas, nunca vuelve de nativeRender()) un par de frames despues de
// que el intro se salta por fallo de alloc CDRAM/PHYCONT -- sin ningun log
// nuestro entre medio, porque el cuelgue esta DENTRO del codigo del motor
// (compilado, cerrado) al que nativeRender() llama, no en algo que logueemos.
// Sin un debugger conectado, la unica forma de ver EN QUE FUNCION esta
// realmente colgado el hilo principal es forzar un crash controlado (con todos
// los registros/stacks de todos los hilos incluidos en el .psp2dmp resultante)
// apenas se detecta que dejo de avanzar, en vez de esperar indefinidamente a
// que el usuario cierre el juego a mano sin dejar rastro utilizable.
volatile int g_loop_iter = -1;
#if 0
static int watchdog_thread(SceSize args, void *argp) {
    int last_seen = -2;
    int stale_ticks = 0;
    while (1) {
        sceKernelDelayThread(1000 * 1000); // 1s
        int cur = g_loop_iter;
        if (cur == last_seen) {
            stale_ticks++;
            l_warn("[watchdog] main loop hasn't advanced past iter=%d in %ds", cur, stale_ticks);
            if (stale_ticks >= 8) {
                l_error("[watchdog] main loop stuck for 8s+ at iter=%d -- forcing a crash to capture "
                        "a .psp2dmp with the main thread's real call stack at the hang point", cur);
                sceKernelDelayThread(200 * 1000); // give the log a moment to flush to disk
                volatile int *null_ptr = NULL;
                *null_ptr = 0xDEAD; // deliberate fault -- see comment above
            }
        } else {
            last_seen = cur;
            stale_ticks = 0;
        }
    }
    return 0;
}
#endif

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

    // Dedicate a core to the main thread (engine tick + render submission, all
    // synchronous/single-threaded through nativeRender()/gl_swap()) so it isn't
    // preempted by/sharing a core with the audio mixer thread (audio_init(),
    // source/audio.cpp) or any transient worker the engine spawns via
    // pthread_create_soloader (source/reimpl/pthr.c). Vita exposes 3 user-mode
    // cores (SCE_KERNEL_CPU_MASK_USER_0/1/2); with no affinity set at all, the
    // scheduler is free to move any of them onto the same core as this one.
    // Matches the pattern MC2BPegasus-Vita-main uses for its own background
    // threads (audio.c). Cheap and safe -- a kernel affinity call, not a hook
    // into engine code.
    {
        int affRet = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
        l_error("[cpu_affinity] main thread -> USER_0 (0x%08X)", affRet);
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

    // Stick analogico -> movimiento (ver el comentario de stick_update()).
    // Los 4 estan en .dynsym, verificado con `nm -D --defined-only`.
    NativeGetPlayerChar        = so_sym_or_warn("_Z19NativeGetPlayerCharib");
    Character_Ctrl_HeadTowards = so_sym_or_warn("_ZN9Character16Ctrl_HeadTowardsERK7Point3DIfE");
    Character_Ctrl_Stop        = so_sym_or_warn("_ZN9Character9Ctrl_StopEv");
    v2Controller_s_blocked     = so_sym_or_warn("_ZN12v2Controller9s_blockedE");
    l_error("[stick] mapeo de ejes inicial -> %s (SELECT cicla entre los %d modos)",
            kStickMapNames[s_stick_map], STICK_MAP_COUNT);
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

    g_loop_iter = 0;
#if 0
    SceUID watchdog_uid = sceKernelCreateThread("main loop watchdog", watchdog_thread,
                                                 0x10000100, 0x4000, 0, 0, NULL);
    if (watchdog_uid >= 0) {
        sceKernelStartThread(watchdog_uid, 0, NULL);
    } else {
        l_warn("[watchdog] thread creation failed (0x%08X) -- no auto-crash-on-hang diagnostic this run",
               (unsigned) watchdog_uid);
    }
#endif

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

        // SELECT solo (sin START, que junto a START es el combo de salida) cicla
        // el mapeo de ejes del stick, para poder resolver la orientacion en una
        // sola sesion de consola. Ver stick_update().
        if ((pressed & SCE_CTRL_SELECT) && !(pad.buttons & SCE_CTRL_START)) {
            s_stick_map = (s_stick_map + 1) % STICK_MAP_COUNT;
            l_error("[stick] mapeo de ejes -> %s", kStickMapNames[s_stick_map]);
        }

        stick_update(&pad);

        if (pending_key_down != -1 && nativeKeyDown) { nativeKeyDown(&jni, NULL, pending_key_down); pending_key_down = -1; }
        if (pending_key_up != -1 && nativeKeyUp) { nativeKeyUp(&jni, NULL, pending_key_up); pending_key_up = -1; }

        // [loop_diag] (3 lineas por frame, "before nativeRender" / "after
        // nativeRender" / "after gl_swap") era el diagnostico temporal del cuelgue
        // de log_158.txt -- ese cuelgue ya no existe, el juego llega a gameplay.
        // Se saco por costo real de rendimiento, no por limpieza: usaba l_warn
        // (activo TAMBIEN en Release, nivel MINIMAL) y cada linea hace un
        // fflush() sincrono a ux0 dentro de _log_print() (utils/logger.c) --
        // 3 escrituras bloqueantes a almacenamiento por frame. En log_172.txt
        // fueron 4462 de 4770 lineas del log entero (93%), dentro del mismo
        // presupuesto de frame que estamos tratando de bajar de 180ms a 33ms.
        // Si hace falta volver a rastrear un cuelgue del loop, reactivarlo detras
        // de un #ifdef propio, nunca como l_warn incondicional.
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
        g_loop_iter++;
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
