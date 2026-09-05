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
#include <math.h>
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
// Stick analogico izquierdo -> joystick virtual del HUD.
//
// Para retomar la ruta NATIVA (sin touch) mas adelante, los simbolos ya estan
// identificados y verificados en .dynsym:
//
//   Character* NativeGetPlayerChar(int idx, bool remote)      0x43c388
//   void Character::Ctrl_HeadTowards(const Point3D<float>&)   0x3adb60
//   void Character::Ctrl_Stop()                               0x3ad890
//   bool v2Controller::s_blocked                              0x9a318b
//   void v2Controller::Cmd_HeadTowards(const Point3D<float>&) 0x405374
//
// Ctrl_HeadTowards recibe una DIRECCION, no un destino (internamente compara
// x²+y²+z² contra ~1e-4 y normaliza), y respeta el estado del personaje via
// SM_IsUsingSkill/SM_IsCasting -- o sea que la magnitud del stick no da
// velocidad variable. Ver el comentario de stick_update() para por que esa ruta
// no funciono en el primer intento y que probar despues.

// Centro y radio del joystick virtual de Flash, en coordenadas FISICAS de
// pantalla (960x544), medidos sobre la captura 2026-08-14-175233.jpg: el pad
// esta abajo a la izquierda. Se convierten al espacio logico del motor con
// glutil_screen_touch_to_logical(), igual que hace action_btn_map.
// Si el personaje se mueve pero el pad no responde bien, ajustar estos tres.
#define VJOY_CX 115
#define VJOY_CY 450
#define VJOY_R  62

// pointer_id propio: 0 lo usa el touch real y 1..5 los botones de accion.
#define VJOY_POINTER_ID 6

static int s_vjoy_active = 0;
static int s_vjoy_last_x = 0, s_vjoy_last_y = 0;

// El stick analogico se mapea sintetizando touch SOBRE el joystick virtual de
// Flash, que es el mismo camino que ya usan los botones de accion.
//
// Por que no la API nativa del motor: se intento primero llamar directo a
// Character::Ctrl_HeadTowards() (ver git log, commit 54f1b1a). Los 4 simbolos
// resolvieron bien (log_011.log no tiene ni un "Symbol not found") y se
// probaron los 4 mapeos de ejes posibles, pero el personaje NO se movio nunca.
// Hipotesis para retomarlo: el v2HudController del motor corre su propio
// Update() dentro de nativeRender() leyendo el joystick de Flash (que esta en
// cero) y PISA nuestra direccion con un Ctrl_Stop. Nuestra llamada pasa antes
// de nativeRender(), asi que siempre pierde. Para que la ruta nativa funcione
// probablemente haya que alimentar al v2Controller (Cmd_HeadTowards, que si
// respeta los gates) en vez de al Character, y para eso falta conseguir el
// v2Controller* vivo -- esta en Character+884 y no tiene getter publico.
//
// CONTRA de esta solucion: depende de que el clip de Flash del joystick exista.
// Cuando se oculte el HUD hay que volver a la ruta nativa.
static void stick_update(const SceCtrlData *pad) {
    if (!nativeOnTouch) return;

    // Stick de Vita: 0..255 con centro en ~128. sy positivo = ABAJO en pantalla,
    // que coincide con el eje Y de las coordenadas de pantalla, asi que no hay
    // que invertir nada.
    float sx = ((float) pad->lx - 128.0f) / 128.0f;
    float sy = ((float) pad->ly - 128.0f) / 128.0f;

    // Deadzone radial, no por eje: los sticks de Vita derivan y una deadzone por
    // eje deja pasar diagonales fantasma.
    const float DEADZONE = 0.28f;
    float mag = sqrtf(sx * sx + sy * sy);
    if (mag < DEADZONE) {
        if (s_vjoy_active) {
            nativeOnTouch(&jni, NULL, 0, s_vjoy_last_x, s_vjoy_last_y, VJOY_POINTER_ID, 0, 0);
            s_vjoy_active = 0;
        }
        return;
    }

    // Reescalar de [DEADZONE..1] a [0..1] para no perder recorrido util, y topear
    // en 1 (las esquinas del cuadrado dan magnitud > 1).
    float scaled = (mag - DEADZONE) / (1.0f - DEADZONE);
    if (scaled > 1.0f) scaled = 1.0f;
    float nx = (sx / mag) * scaled;
    float ny = (sy / mag) * scaled;

    int px = VJOY_CX + (int) (nx * VJOY_R);
    int py = VJOY_CY + (int) (ny * VJOY_R);

    int lx, ly;
    if (!glutil_screen_touch_to_logical(px, py, &lx, &ly)) {
        lx = px; ly = py;
    }

    if (!s_vjoy_active) {
        // Un joystick analogico necesita DOWN y despues MOVEs continuos: el clip
        // de Flash arranca a seguir el dedo en el DOWN y calcula la direccion
        // como el offset respecto de donde se apoyo. Por eso el DOWN va en el
        // CENTRO del pad (no en la posicion desplazada): si no, el clip tomaria
        // ese punto como origen y la primera direccion saldria nula.
        int clx, cly;
        if (!glutil_screen_touch_to_logical(VJOY_CX, VJOY_CY, &clx, &cly)) {
            clx = VJOY_CX; cly = VJOY_CY;
        }
        nativeOnTouch(&jni, NULL, 1, clx, cly, VJOY_POINTER_ID, 0, 0);
        s_vjoy_active = 1;
    }

    // MOVE en cada frame mientras haya deflexion, incluso si la posicion no
    // cambio: algunos clips de GameSWF solo actualizan su estado al recibir el
    // evento, no lo mantienen entre frames.
    nativeOnTouch(&jni, NULL, 2, lx, ly, VJOY_POINTER_ID, 0, 0);
    s_vjoy_last_x = lx;
    s_vjoy_last_y = ly;
}

static void *app_singleton_inst;
static void (* SavegameManager_setLanguage)(void *this_, int lang);
static int (* SavegameManager_getLanguage)(void *this_);
static void (* SavegameManager_saveSettings)(void *this_);
// Queued async saves are drained on a timer and at exit (see savejobs_* in
// source/patch.c); implemented there, declared here.
extern void savejobs_drain(void);
extern int savejobs_has_pending(void);

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
    nativePause            = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_DungeonHunter2_nativePause");
    nativeResume           = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_DungeonHunter2_nativeResume");
    nativeCanInterrupt     = so_sym_or_warn("Java_com_gameloft_android_GAND_GloftD2SS_DungeonHunter2_nativeCanInterrupt");
    app_singleton_inst     = so_sym_or_warn("_ZN9SingletonI11ApplicationE6s_instE");
    SavegameManager_setLanguage = so_sym_or_warn("_ZN15SavegameManager11setLanguageEi");
    SavegameManager_getLanguage = so_sym_or_warn("_ZNK15SavegameManager11getLanguageEv");
    SavegameManager_saveSettings = so_sym_or_warn("_ZN15SavegameManager12saveSettingsEv");

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
            // One-shot language sanitize (replaces the old 180-frame forced
            // English). The old force rewrote the user's saved language to
            // English on EVERY boot, so "can't change language" was
            // guaranteed even when option saves worked fine. Now: wait until
            // the SavegameManager instance exists, read the persisted value
            // once, and only touch it if it is out of range (the engine
            // itself clamps >7 in its menu_language flow; anything else is a
            // legitimate user choice that must survive reboots).
            static int lang_sanitized = 0;
            if (!lang_sanitized && app_singleton_inst && SavegameManager_getLanguage) {
                void *sgm = *(void **)((char *) app_singleton_inst + 76);
                if (sgm) {
                    int lang = SavegameManager_getLanguage(sgm);
                    if (lang < 0 || lang > 7) {
                        l_warn("[save_io] saved language %d out of range, resetting to English (0)", lang);
                        if (SavegameManager_setLanguage)
                            SavegameManager_setLanguage(sgm, 0);
                    } else {
                        l_warn("[save_io] keeping saved language %d", lang);
                    }
                    lang_sanitized = 1;
                }
            }
        }

        // Periodic async-save drain: Savegame::saveAll only QUEUES write jobs
        // (see source/patch.c). If the engine's fire-and-forget workers ever
        // fail to run them, progress would silently die in RAM -- the exact
        // "played but nothing persisted" symptom. Draining here (same
        // FlushJobs(NULL) call Application::Quit makes) guarantees the queue
        // reaches disk even then; when workers are healthy this is a cheap
        // no-op because the queue is already empty. Gated on the pending flag
        // set by the AddJob hook so idle frames pay nothing.
        {
            static int drain_tick = 0;
            if ((drain_tick++ % 600) == 0 && savejobs_has_pending()) {
                savejobs_drain();
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

    // Exit flush: START+SELECT (or any loop break) used to skip
    // Application::Quit entirely, so Savegame::FlushJobs(NULL) -- the only
    // synchronous drain of the async save queue -- never ran and every save
    // still sitting in RAM died with the process. Mirror the relevant part of
    // Quit here: persist settings synchronously, then drain all queued save
    // jobs before touching nativePause/exit.
    {
        void *sgm = NULL;
        if (app_singleton_inst)
            sgm = *(void **)((char *) app_singleton_inst + 76);
        if (sgm && SavegameManager_saveSettings) {
            l_warn("[save_io] exit: saving settings...");
            SavegameManager_saveSettings(sgm);
        }
        if (savejobs_has_pending()) {
            l_warn("[save_io] exit: draining pending save jobs...");
            savejobs_drain();
        } else {
            // Drain unconditionally anyway: cheap when empty, and covers jobs
            // queued without the hook flag (e.g. before hooks installed).
            savejobs_drain();
        }
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
