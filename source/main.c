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
// OJO: a diferencia de appKeyPressed (confirmado vacio, solo un _DEBUG_OUT:
// decompiled/.../ghidra/out_ghidra.c linea 409243), appKeyReleased (nativeKeyUp)
// SI tiene logica real -- ver el comentario junto a action_btn_map mas abajo.
// AKEYCODE_BACK se saco de aca a proposito: el motor le da manejo propio a BACK
// en appKeyReleased (confirmacion de salida / minimizar en cutscene) que no
// queremos disparar por accidente al usar CIRCLE como boton de skill.
static const struct { unsigned int btn; int keycode; } btn_map[] = {
    { SCE_CTRL_UP,       AKEYCODE_DPAD_UP },
    { SCE_CTRL_DOWN,     AKEYCODE_DPAD_DOWN },
    { SCE_CTRL_LEFT,     AKEYCODE_DPAD_LEFT },
    { SCE_CTRL_RIGHT,    AKEYCODE_DPAD_RIGHT },
    { SCE_CTRL_CROSS,    AKEYCODE_DPAD_CENTER },
    { SCE_CTRL_L1,       AKEYCODE_MENU },
};
#define BTN_MAP_COUNT (sizeof(btn_map) / sizeof(btn_map[0]))

/**
 * @brief Mapping physical action buttons to synthetic touch presses on HUD.
 */
static const struct { unsigned int btn; int x; int y; long long pointer_id; const char *name; } action_btn_map[] = {
    { SCE_CTRL_CROSS,    850, 450, 1, "Primary attack (sword icon, bottom-right)" },
    { SCE_CTRL_SQUARE,   686, 467, 2, "Skill 1 (cyan box, bottom-left of cluster)" },
    { SCE_CTRL_TRIANGLE, 905, 240, 3, "Skill 2 (golden lightning wheel icon, upper-right)" },
    // Cuadro azul oscuro en 2026-09-18-191203.jpg: icono de espada en llamas (centro 716, 372)
    { SCE_CTRL_CIRCLE,   716, 372, 7, "Skill 3 (dark blue box, fiery sword icon)" },
    { SCE_CTRL_R1,       905,  60, 4, "Health potion (red flask icon, top-right)" },
    // Cuadro verde en 2026-09-18-191203.jpg: icono de pausa (centro 71, 174)
    { SCE_CTRL_START,     71, 174, 8, "Pause button (green box, pause icon)" },
    // Cuadro rosado en 2026-09-18-191203.jpg: retrato del personaje (centro 103, 76)
    { SCE_CTRL_SELECT,   103,  76, 9, "Character screen (pink box, portrait icon)" },
};
#define ACTION_BTN_MAP_COUNT (sizeof(action_btn_map) / sizeof(action_btn_map[0]))
// Rastrea, por boton, si su DOWN sintetico realmente se mando (ver el loop en
// main() para por que un UP sin DOWN previo desincroniza el touch del motor).
static int s_action_down_sent[ACTION_BTN_MAP_COUNT];

// ---------------------------------------------------------------------------
// Stick analogico izquierdo + cruceta -> movimiento del personaje.
//
// Tres intentos previos (ver git log) fallaron por razones distintas, ambas
// confirmadas por fin leyendo el desensamblado REAL con objdump sobre
// dungeon-hunter-2_extract/lib/armeabi-v7a/libDungeonHunter2.so (NO el
// pseudo-C de Ghidra, que en estas funciones viene garbled en los floats):
//  1) nativeKeyDown/nativeKeyUp son no-ops verificados (appKeyPressed/
//     appKeyReleased vacias) -- la cruceta nunca pudo pasar por ahi.
//  2) Sintetizar touch sobre el joystick de Flash (intentos previos) nunca
//     hizo que GameSWF aceptara el hit-test (logs: el flag "enganchado" y el
//     vector de HUDControls se quedaban en 0 pese al touch sintetico
//     llegando al motor).
//
// La solucion: en vez de fingir un touch y depender del hit-test de GameSWF,
// escribimos DIRECTAMENTE los campos que HUDControls::Update() (0x41a780)
// lee cada frame para decidir el movimiento -- los mismos que llena su propio
// OnEvent (0x418d28) en la rama DRAG (0x419764) cuando un dedo real arrastra
// el stick:
//
//   HUDControls (GetInstance()/hasInstance(), .dynsym) + offset:
//     +0xa    int8  "enganchado": Update() solo llama a Cmd_HeadTowards si
//             esto es != 0 (y ademas Character::CTRLIsAllowed() en ese frame).
//     +0x65c  float x  \
//     +0x660  float y   > vector de direccion UNITARIO en el espacio que
//     +0x664  float z  /  espera Cmd_HeadTowards (z siempre 0, es 2D).
//     +0x668  float     escala/magnitud (0..~1); Update() hace
//             final = vector * escala antes de llamar Cmd_HeadTowards.
//
//   Character* (NativeGetPlayerChar(0,false), 0x43c388) + 0x378: v2Controller*
//     -- confirmado en la rama RELEASE de OnEvent (0x4191e0) y en Update()
//     (0x41a990): ambas hacen `ldr r_ctrl,[r_char,#0x378]` antes de llamar
//     Cmd_Stop()/usar el vector. Solo se usa aca para poder llamar
//     Character::Ctrl_Stop() al soltar (Update() nunca llama Cmd_Stop por si
//     sola con +0xa=0, solo deja de mover).
//
// El (x,y) que OnEvent guarda ahi NO es el delta de pantalla tal cual: es un
// vector semilla (1,-1,0) normalizado y rotado (90 - angulo) grados alrededor
// del origen (Point3D::rotateXYBy, 0x418c48 -- rotacion CCW estandar,
// verificada leyendo esa funcion). Repitiendo esa rotacion en forma cerrada
// para un angulo de pantalla theta=atan2f(dy,dx) con (dx,dy) ya unitario
// (nuestro nx,ny):
//   x' = (nx+ny)/sqrt(2)
//   y' = (nx-ny)/sqrt(2)
// (cambio de base de 45 grados -- control isometrico tipico: "arriba" en
// pantalla no es +X en el mundo). Si en consola la direccion sale rotada o
// espejada, es este signo el que hay que ajustar; lo que importa es que
// ahora el personaje se mueve, que es lo que fallaba en los 3 intentos
// anteriores.
static void *(* HUDControls_GetInstance)(void);
static int (* HUDControls_hasInstance)(void);
static void *(* NativeGetPlayerChar)(int idx, int remote);
static void (* Character_Ctrl_Stop)(void *character);

typedef struct {
    float m_[4][2]; // [R, G, B, A] x [mult, add]
} gameswf_cxform;

static const gameswf_cxform s_cxform_1pct = {
    {
        { 1.0f, 0.0f },
        { 1.0f, 0.0f },
        { 1.0f, 0.0f },
        { 0.01f, 0.0f } // 1% opacity
    }
};

static void (* gameswf_character_set_cxform)(void *this_, const void *cx);
static void *(* gameswf_character_get_parent)(void *this_);
static void *(* DebugCachedCharacter_GetChar)(void *this_);

static void hud_apply_bottom_controls_opacity(void *h) {
    if (!h || !DebugCachedCharacter_GetChar || !gameswf_character_set_cxform) return;

    // Offsets de DebugCachedCharacter para los controles inferiores:
    // +0x4c: Joystick (contiene stick como hijo)
    // +0x88: btn_interact (ataque primario)
    // +0x3b8: btn_spell (rueda dorada)
    // +0x3e8: btn_skill1
    // +0x418: btn_skill2
    // +0x448: btn_skill3
    // +0x118, +0x148, +0x178: variantes de skill para HUDStyle < 2
    static const uintptr_t dcc_offsets[] = {
        0x4c, 0x88, 0x3b8, 0x3e8, 0x418, 0x448, 0x118, 0x148, 0x178
    };

    void *parents[10];
    int parent_count = 0;

    for (size_t i = 0; i < sizeof(dcc_offsets) / sizeof(dcc_offsets[0]); i++) {
        void *dcc = (void *)((char *)h + dcc_offsets[i]);
        void *ch = DebugCachedCharacter_GetChar(dcc);
        if (!ch) continue;

        void *parent = gameswf_character_get_parent ? gameswf_character_get_parent(ch) : NULL;
        if (parent) {
            int found = 0;
            for (int j = 0; j < parent_count; j++) {
                if (parents[j] == parent) {
                    found = 1;
                    break;
                }
            }
            if (!found && parent_count < 10) {
                parents[parent_count++] = parent;
            }
        } else {
            gameswf_character_set_cxform(ch, &s_cxform_1pct);
        }
    }

    for (int j = 0; j < parent_count; j++) {
        gameswf_character_set_cxform(parents[j], &s_cxform_1pct);
    }

    static int s_logged_opacity = 0;
    if (!s_logged_opacity && parent_count > 0) {
        l_info("[hud_opacity] 1%% opacity applied to %d bottom controls container(s)", parent_count);
        s_logged_opacity = 1;
    }
}

#define HUD_OFF_ENGAGED 0xa
#define HUD_OFF_DIR_X   0x65c
#define HUD_OFF_DIR_Y   0x660
#define HUD_OFF_DIR_Z   0x664
#define HUD_OFF_DIR_MAG 0x668

static int s_stick_engaged = 0;

static void stick_update(const SceCtrlData *pad) {
    if (!HUDControls_hasInstance || !HUDControls_GetInstance) return;
    if (!HUDControls_hasInstance()) return;
    void *h = HUDControls_GetInstance();
    if (!h) return;

    hud_apply_bottom_controls_opacity(h);

    // Stick de Vita: 0..255 con centro en ~128. sy positivo = ABAJO en
    // pantalla, igual que el resto del motor. Requiere
    // sceCtrlSetSamplingMode(Ext)(ANALOG_WIDE) (ver main()), sin eso lx/ly
    // siempre leen 128 y esto nunca sale de la deadzone.
    float sx = ((float) pad->lx - 128.0f) / 128.0f;
    float sy = ((float) pad->ly - 128.0f) / 128.0f;

    // Deadzone radial, no por eje: los sticks de Vita derivan y una deadzone
    // por eje deja pasar diagonales fantasma. Bajado de 0.28: log_030.log
    // (lectura ya andando via PeekBufferPositiveExt2) mostro al usuario
    // deflectando el stick sin pasar de mag=0.097 -- con 0.28 eso nunca
    // salia de la deadzone y el personaje no se movia pese a que la lectura
    // ya funcionaba. 0.12 sigue filtrando el ruido de reposo visto en el
    // mismo log (~0.02-0.03).
    const float DEADZONE = 0.12f;
    float mag = sqrtf(sx * sx + sy * sy);
    // Diagnostico temporal: la cruceta (misma escritura a HUDControls, mismo
    // gate CTRLIsAllowed) ya mueve al personaje en consola real, pero el stick
    // analogico no -- esto aisla si el problema es la LECTURA de pad->lx/ly
    // (nunca sale de ~128 pese a sceCtrlSetSamplingModeExt) o el calculo de
    // aca en adelante.
    {
        static unsigned s_stick_diag = 0;
        if ((++s_stick_diag % 20) == 0) {
            l_debug("stick_diag: lx=%d ly=%d sx=%.3f sy=%.3f mag=%.3f deadzone=%.2f",
                    pad->lx, pad->ly, sx, sy, mag, DEADZONE);
        }
    }
    float nx = 0.0f, ny = 0.0f, scale = 0.0f;
    int have_vec = 0;
    if (mag >= DEADZONE) {
        // Reescalar de [DEADZONE..1] a [0..1] para no perder recorrido util, y
        // topear en 1 (las esquinas del cuadrado dan magnitud > 1).
        float rescaled = (mag - DEADZONE) / (1.0f - DEADZONE);
        if (rescaled > 1.0f) rescaled = 1.0f;
        nx = sx / mag;
        ny = sy / mag;
        scale = rescaled;
        have_vec = 1;
    } else {
        // Cruceta a deflexion completa (nativeKeyDown es no-op, ver arriba).
        float dx = ((pad->buttons & SCE_CTRL_RIGHT) ? 1.0f : 0.0f)
                 - ((pad->buttons & SCE_CTRL_LEFT)  ? 1.0f : 0.0f);
        float dy = ((pad->buttons & SCE_CTRL_DOWN)  ? 1.0f : 0.0f)
                 - ((pad->buttons & SCE_CTRL_UP)    ? 1.0f : 0.0f);
        if (dx != 0.0f || dy != 0.0f) {
            float len = sqrtf(dx * dx + dy * dy); // diagonal normalizada
            nx = dx / len;
            ny = dy / len;
            scale = 1.0f;
            have_vec = 1;
        }
    }

    if (!have_vec) {
        if (s_stick_engaged) {
            *(volatile signed char *) ((char *) h + HUD_OFF_ENGAGED) = 0;
            s_stick_engaged = 0;
            if (NativeGetPlayerChar && Character_Ctrl_Stop) {
                void *ch = NativeGetPlayerChar(0, 0);
                if (ch) Character_Ctrl_Stop(ch);
            }
        }
        return;
    }

    const float SQRT1_2 = 0.70710678f;
    float wx = (nx + ny) * SQRT1_2;
    float wy = (nx - ny) * SQRT1_2;

    *(volatile float *) ((char *) h + HUD_OFF_DIR_X) = wx;
    *(volatile float *) ((char *) h + HUD_OFF_DIR_Y) = wy;
    *(volatile float *) ((char *) h + HUD_OFF_DIR_Z) = 0.0f;
    *(volatile float *) ((char *) h + HUD_OFF_DIR_MAG) = scale;
    *(volatile signed char *) ((char *) h + HUD_OFF_ENGAGED) = 1;
    s_stick_engaged = 1;
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
    HUDControls_GetInstance = so_sym_or_warn("_ZN11HUDControls11GetInstanceEv");
    HUDControls_hasInstance = so_sym_or_warn("_ZN11HUDControls11hasInstanceEv");
    NativeGetPlayerChar     = so_sym_or_warn("_Z19NativeGetPlayerCharib");
    Character_Ctrl_Stop     = so_sym_or_warn("_ZN9Character9Ctrl_StopEv");
    SavegameManager_setLanguage = so_sym_or_warn("_ZN15SavegameManager11setLanguageEi");
    SavegameManager_getLanguage = so_sym_or_warn("_ZNK15SavegameManager11getLanguageEv");
    SavegameManager_saveSettings = so_sym_or_warn("_ZN15SavegameManager12saveSettingsEv");
    gameswf_character_set_cxform = so_sym_or_warn("_ZN7gameswf9character10set_cxformERKNS_6cxformE");
    gameswf_character_get_parent = so_sym_or_warn("_ZNK7gameswf9character10get_parentEv");
    DebugCachedCharacter_GetChar = so_sym_or_warn("_ZN20DebugCachedCharacter7GetCharEv");

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

    // Sin esto pad.lx/ly siempre leen 128 (centro) y el stick no existe para
    // stick_update(). dialog.c lo re-aplica tras el IME, que lo resetea.
    {
        int smRet = sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
        l_success("ctrl sampling ANALOG_WIDE (ret=0x%08X)", (unsigned) smRet);
    }

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
        // sceCtrlPeekBufferPositive() por si sola no traia lx/ly (siempre 128,
        // log_029.log: 133/133 muestras identicas) una vez que main() llama
        // sceCtrlSetSamplingModeExt(ANALOG_WIDE) -- ese modo lo consume el
        // buffer "Ext2", no el basico. Mismo patron que MC2BPegasus-Vita y
        // asphalt8-vita (los unicos ports hermanos que leen el stick de
        // verdad, no solo botones): PeekBufferPositiveExt2 primero, con el
        // basico como fallback si por algun motivo devuelve error.
        if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) <= 0) {
            sceCtrlPeekBufferPositive(0, &pad, 1);
        }
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
                    s_action_down_sent[i] = 1;
                }
            }
            // Solo se manda el UP si el DOWN realmente se mando: si el debounce
            // de arriba se lo comio, mandar el UP igual sueltaba un puntero que
            // el motor nunca vio bajar -- como appOnTouch es single-touch (ver
            // bloque de comentarios de stick_update()), ese UP huerfano
            // desincroniza su unico estado de touch y deja al personaje sin
            // responder a NINGUN boton hasta reiniciar (visto en consola real
            // presionando START justo despues de otro boton).
            if ((released & action_btn_map[i].btn) && s_action_down_sent[i]) {
                l_debug("action_btn: synthetic touch UP (%d,%d)->(%d,%d) ptr=%lld [%s]",
                        action_btn_map[i].x, action_btn_map[i].y, lx, ly, action_btn_map[i].pointer_id, action_btn_map[i].name);
                if (nativeOnTouch) nativeOnTouch(&jni, NULL, 0, lx, ly,
                                                  action_btn_map[i].pointer_id, 0, 0);
                s_action_down_sent[i] = 0;
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
                        l_debug("touch_real: DOWN phys=(%d,%d) logical=(%d,%d)",
                                phys_x, phys_y, x, y);
                        last_touch = 1;
                    }
                } else if (x != last_tx || y != last_ty) {
                    if (nativeOnTouch) nativeOnTouch(&jni, NULL, 2, x, y, 0, 0, 0);
                }
                last_tx = x; last_ty = y;
            } else if (last_touch) {
                if (nativeOnTouch) nativeOnTouch(&jni, NULL, 0, last_tx, last_ty, 0, 0, 0);
                l_debug("touch_real: UP logical=(%d,%d)", last_tx, last_ty);
                last_touch = 0;
            }
        } else if (last_touch) {
            if (nativeOnTouch) nativeOnTouch(&jni, NULL, 0, last_tx, last_ty, 0, 0, 0);
            l_debug("touch_real: UP logical=(%d,%d)", last_tx, last_ty);
            last_touch = 0;
        }

        // Escribe directo en HUDControls (ver bloque de arriba); no toca el
        // touch, asi que no compite por el unico slot tactil del motor con los
        // botones de accion o el touch real.
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
