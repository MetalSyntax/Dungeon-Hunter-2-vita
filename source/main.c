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
    { SCE_CTRL_R1,       905,  60, 4, "Health potion (red flask icon, top-right)" },
    // Circulo, START y SELECT ya NO estan aca: pasaron a llamadas directas al
    // motor (ver el bloque de hud_trigger_skill()/hud_toggle_menu_state()).
    //
    // OJO al agregar botones nuevos: pointer_id tiene que quedar en 0..7.
    // TouchScreenBase indexa sus slots SIN validar rango -- touchBegan()
    // (0x33ac24) hace `this + 48*id` y escribe en +0x20/+0x24/+0x28, y clear()
    // (0x33b4d8) recorre exactamente 8 entradas (`cmp r3,#8`). Circulo/START/
    // SELECT usaban 7/8/9: los dos ultimos escribian FUERA del array (pisando
    // el contador de touches en +0x190), y por eso en log_034 el tap sintetico
    // "llegaba al motor" pero no disparaba nada. El 0 es el dedo real.
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

// ---------------------------------------------------------------------------
// Circulo (poder), START (menu de pausa) y SELECT (pantalla de personaje):
// llamadas directas al motor, sin touch sintetico.
//
// Los tres fallaban aunque el tap sintetico SI llegaba al motor (log_034). La
// causa real no eran las coordenadas sino el pointer_id: TouchScreenBase indexa
// su array de slots sin chequear rango (touchBegan hace `this + 48*id`) y
// clear() recorre 8 entradas -> solo existen los ids 0..7. START usaba 8 y
// SELECT 9, o sea escribian fuera del array; Circulo (7) caia justo en el
// ultimo slot valido pero seguia dependiendo de acertarle al hitbox de un boton
// de Flash cuya posicion el jugador puede mover (HUDControls::SetHUDPos).
//
// En vez de seguir adivinando pixeles, replicamos lo que hace el propio
// ActionScript del HUD -- bytecode AS2 de data/menus/dqhud_i9000.swf:
//   btn_skill1.onRelease        -> NativeHUDSkill(0)
//   btn_skill2.onRelease        -> NativeHUDSkill(1)
//   btn_skill3.onRelease        -> NativeHUDSkill(2)
//   btn_mainmenu.onRelease      -> NativePushState("menu_Ingame")
//   btn_charactermenu.onRelease -> NativePushState("menu_CharacterMenu");
//                                  NativeUpdateOrientation(); NativeAwayFromHud()
//
// NativeHUDSkill (0x43e734), sacando el desempaquetado del gameswf::fn_call, es
// exactamente esto:
//     ch = NativeGetPlayerChar(0, false)
//     if (!ch || !ch->CTRLIsAllowed()) return
//     id = ch->SG_GetSkillInSlot(slot); if (id == -1) return
//     ctrl = *(void**)(ch + 0x378)
//     ctrl->Cmd_BeginSkill(id); ctrl->Cmd_EndSkill(id)
//
// NativePushState (0x43aebc) hace lo mismo que MenuBase::FS_PushState
// (0x421234), que SI esta exportada y -- leyendo su desensamblado -- nunca usa
// `this` (la primera instruccion pisa r0 con el literal de "menu_CharacterMenu"),
// asi que se puede llamar con this = NULL y nos quedamos con el guard original
// incluido: solo empuja el estado si arriba del StateMachine esta el de juego.
// Idem NativeAwayFromHud (0x43ab98): nunca toca su fn_call.
#define CHAR_OFF_CONTROLLER 0x378

// Slot de skill que dispara Circulo (el cuadro azul oscuro del screenshot).
// 0 = btn_skill1, 1 = btn_skill2, 2 = btn_skill3.
// Resuelto en consola: con slot 2 Circulo lanzaba el MISMO poder que Cuadrado
// (que toca el icono celeste), o sea celeste = btn_skill3. Eso fija la escala
// del layout del SWF (sprite463: btn_skill3 (347,186) es el de abajo-izquierda
// y btn_skill1 (370,125) el que le queda arriba-derecha, que es justo el azul
// oscuro; btn_skill2 (435,114) esta en la columna de la derecha, con el spell).
// Por eso Circulo = slot 0.
#define CIRCLE_SKILL_SLOT 0

static int  (* Character_CTRLIsAllowed)(void *character);
static int  (* Character_SG_GetSkillInSlot)(void *character, int slot);
static void (* v2Controller_Cmd_BeginSkill)(void *ctrl, unsigned int skill_id);
static void (* v2Controller_Cmd_EndSkill)(void *ctrl, unsigned int skill_id);
static int  (* MenuBase_FS_PushState)(void *this_, const char *menu, const char *arg, void *ud);
static void (* NativeAwayFromHud)(void *fn_call);
static void *(* MenuManager_GetInstance)(void);
static void *(* MenuManager_GetMenuByName)(void *this_, const char *name);
static int  (* MenuBase_IsVisible)(void *menu);
static void (* MenuFX_PopAll)(void *menufx);

#define MENUBASE_OFF_MENUFX 0x4

// Cerrar un menu ya abierto, copiado tal cual de Application::_CheckGamepad
// (0x322108-0x32212c), que es como el motor mismo maneja el boton de menu de un
// gamepad: GetMenuByName -> IsVisible -> MenuFX::PopAll sobre menu->[0x4].
// Devuelve 1 si lo cerro, para que el mismo boton sirva de toggle.
static int hud_pop_menu_if_visible(const char *menu) {
    if (!MenuManager_GetInstance || !MenuManager_GetMenuByName ||
        !MenuBase_IsVisible || !MenuFX_PopAll) return 0;
    void *mm = MenuManager_GetInstance();
    if (!mm) return 0;
    void *m = MenuManager_GetMenuByName(mm, menu);
    if (!m || !MenuBase_IsVisible(m)) return 0;
    void *fx = *(void **) ((char *) m + MENUBASE_OFF_MENUFX);
    if (!fx) return 0;
    l_debug("action_btn: PopAll(\"%s\")", menu);
    MenuFX_PopAll(fx);
    return 1;
}

static void hud_trigger_skill(int slot) {
    if (!NativeGetPlayerChar || !Character_CTRLIsAllowed ||
        !Character_SG_GetSkillInSlot || !v2Controller_Cmd_BeginSkill ||
        !v2Controller_Cmd_EndSkill) return;
    void *ch = NativeGetPlayerChar(0, 0);
    if (!ch) return;
    // Mismo gate que usa el HUD: sin el, apretar el boton durante una cinematica
    // o con el personaje bloqueado mete comandos que el motor no espera.
    if (!Character_CTRLIsAllowed(ch)) return;
    int skill_id = Character_SG_GetSkillInSlot(ch, slot);
    if (skill_id == -1) return; // slot vacio
    void *ctrl = *(void **) ((char *) ch + CHAR_OFF_CONTROLLER);
    if (!ctrl) return;
    l_debug("action_btn: skill slot %d -> skill id %d", slot, skill_id);
    v2Controller_Cmd_BeginSkill(ctrl, (unsigned int) skill_id);
    v2Controller_Cmd_EndSkill(ctrl, (unsigned int) skill_id);
}

// Toggle: si el menu ya esta arriba lo cierra, si no lo abre. Sin el cierre el
// boton fisico seria de ida nomas (FS_PushState solo empuja cuando el estado de
// juego esta al tope del StateMachine) y habria que salir con el tactil.
static void hud_toggle_menu_state(const char *menu, int away_from_hud) {
    if (hud_pop_menu_if_visible(menu)) return;
    if (!MenuBase_FS_PushState) return;
    l_debug("action_btn: PushState(\"%s\")", menu);
    MenuBase_FS_PushState(NULL, menu, NULL, NULL);
    if (away_from_hud && NativeAwayFromHud) NativeAwayFromHud(NULL);
}

typedef struct {
    float m_[4][2]; // [R, G, B, A] x [mult, add]
} gameswf_cxform;

// Opacidad de los controles inferiores del HUD. Por defecto 1% (casi invisibles:
// se juega con los botones fisicos); el combo L+R del loop principal la alterna
// a 100% para poder volver a usarlos con el tactil. Se reaplica cada frame desde
// stick_update(), asi que cambiar el alfa aca ya surte efecto al frame siguiente.
#define HUD_ALPHA_DIM  0.01f
#define HUD_ALPHA_FULL 1.0f

static gameswf_cxform s_cxform_hud = {
    {
        { 1.0f, 0.0f },
        { 1.0f, 0.0f },
        { 1.0f, 0.0f },
        { HUD_ALPHA_DIM, 0.0f }
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
            gameswf_character_set_cxform(ch, &s_cxform_hud);
        }
    }

    for (int j = 0; j < parent_count; j++) {
        gameswf_character_set_cxform(parents[j], &s_cxform_hud);
    }

    static int s_logged_opacity = 0;
    if (!s_logged_opacity && parent_count > 0) {
        l_info("[hud_opacity] alpha %d%% applied to %d bottom controls container(s)",
               (int) (s_cxform_hud.m_[3][0] * 100.0f + 0.5f), parent_count);
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
    Character_CTRLIsAllowed = so_sym_or_warn("_ZNK9Character13CTRLIsAllowedEv");
    Character_SG_GetSkillInSlot = so_sym_or_warn("_ZN9Character17SG_GetSkillInSlotEi");
    v2Controller_Cmd_BeginSkill = so_sym_or_warn("_ZN12v2Controller14Cmd_BeginSkillEj");
    v2Controller_Cmd_EndSkill   = so_sym_or_warn("_ZN12v2Controller12Cmd_EndSkillEj");
    MenuBase_FS_PushState   = so_sym_or_warn("_ZN8MenuBase12FS_PushStateEPKcS1_Pv");
    NativeAwayFromHud       = so_sym_or_warn("_Z17NativeAwayFromHudRKN7gameswf7fn_callE");
    MenuManager_GetInstance = so_sym_or_warn("_ZN11MenuManager11GetInstanceEv");
    MenuManager_GetMenuByName = so_sym_or_warn("_ZN11MenuManager13GetMenuByNameEPKc");
    MenuBase_IsVisible      = so_sym_or_warn("_ZNK8MenuBase9IsVisibleEv");
    MenuFX_PopAll           = so_sym_or_warn("_ZN6MenuFX6PopAllEv");
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

        // Combo L + R: alterna la opacidad de los controles inferiores del HUD
        // entre 1% (por defecto: se juega con los botones fisicos) y 100% (para
        // volver a usarlos con el tactil). Mientras el combo esta enganchado se
        // comen los eventos de L1/R1 para que no dispare ademas la pocion (R1)
        // ni el AKEYCODE_MENU (L1).
        {
            const unsigned int SHOULDERS = SCE_CTRL_L1 | SCE_CTRL_R1;
            static int combo_latched = 0;
            if ((pad.buttons & SHOULDERS) == SHOULDERS) {
                if (!combo_latched) {
                    combo_latched = 1;
                    s_cxform_hud.m_[3][0] = (s_cxform_hud.m_[3][0] < 0.5f)
                                          ? HUD_ALPHA_FULL : HUD_ALPHA_DIM;
                    l_warn("[hud_opacity] L+R -> bottom HUD controls alpha = %d%%",
                           (int) (s_cxform_hud.m_[3][0] * 100.0f + 0.5f));
                    // Si R1 ya habia mandado su touch DOWN antes de que entrara
                    // L1, hay que soltarlo aca a mano: al comernos su `released`
                    // el motor se quedaria con un dedo pegado en ese slot.
                    for (int i = 0; i < ACTION_BTN_MAP_COUNT; i++) {
                        if (!(action_btn_map[i].btn & SHOULDERS)) continue;
                        if (!s_action_down_sent[i]) continue;
                        int cx, cy;
                        if (!glutil_screen_touch_to_logical(action_btn_map[i].x,
                                                            action_btn_map[i].y, &cx, &cy)) {
                            cx = action_btn_map[i].x; cy = action_btn_map[i].y;
                        }
                        if (nativeOnTouch) nativeOnTouch(&jni, NULL, 0, cx, cy,
                                                          action_btn_map[i].pointer_id, 0, 0);
                        s_action_down_sent[i] = 0;
                    }
                }
            } else if ((pad.buttons & SHOULDERS) == 0) {
                combo_latched = 0;
            }
            if (combo_latched) {
                pressed &= ~SHOULDERS;
                released &= ~SHOULDERS;
            }
        }

        for (int i = 0; i < BTN_MAP_COUNT; i++) {
            if (pressed & btn_map[i].btn) pending_key_down = btn_map[i].keycode;
            if (released & btn_map[i].btn) pending_key_up = btn_map[i].keycode;
        }

        // Circulo / START / SELECT no pasan por touch sintetico (ver el bloque
        // de comentarios de hud_trigger_skill()). START+SELECT juntos sigue
        // siendo el atajo de salida de mas arriba, asi que ninguno de los dos
        // actua mientras el otro este apretado.
        if (pressed & SCE_CTRL_CIRCLE) hud_trigger_skill(CIRCLE_SKILL_SLOT);
        if ((pressed & SCE_CTRL_START) && !(pad.buttons & SCE_CTRL_SELECT))
            hud_toggle_menu_state("menu_Ingame", 0);
        if ((pressed & SCE_CTRL_SELECT) && !(pad.buttons & SCE_CTRL_START))
            hud_toggle_menu_state("menu_CharacterMenu", 1);

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
