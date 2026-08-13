/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching internal .so functions or bridging them to native for better compatibility.
 * @details Refer to technical documentation in Docs/patch_comments.md for full details on
 *          DRM bypasses, HUDStyle overrides, and disabled diagnostic hooks.
 */

#include <string.h>

#include <kubridge.h>
#include <so_util/so_util.h>

#include "utils/logger.h"
#include "utils/glutil.h"

extern so_module so_mod;

void ret_void(void) {
    return;
}

extern int ret1(void);

/**
 * @brief Application::GetSavedOption hook to force fixed HUD style.
 * @details On Android, if SavegameManager::hasOption("HUDStyle") is 0, the function returns garbage from r0.
 *          Although real hardware tests (log_093.txt) verified that the key exists and equals 0, this defensive
 *          hook forces HUDStyle=0 for the Vita's 960x544 screen.
 */
typedef int (*hasOption_t)(void *savegame_mgr, const char *key);
static hasOption_t s_hasOption;
static so_hook s_hook_get_saved_option;

static int hook_GetSavedOption(void *this_, const char *key) {
    int result = SO_CONTINUE(int, s_hook_get_saved_option, this_, key);
    if (strcmp(key, "HUDStyle") == 0) {
        void *savegame_mgr = *(void **)((char *)this_ + 0x4c);
        int has_option = s_hasOption(savegame_mgr, key);
        /**
         * @note Logs option state before override (log_092.txt / log_093.txt).
         */
        l_debug("[hook_HUDStyle] savegame_mgr=%p hasOption=%d pre_override_result=%d",
                savegame_mgr, has_option, result);
        /**
         * @brief Unconditionally force 3-icon phone HUD bar for 960x544 resolution.
         */
        if (result >= 2 || !has_option) {
            l_debug("[hook_HUDStyle] forcing HUDStyle=0 (has_option=%d raw_result=%d)",
                    has_option, result);
            return 0;
        }
    }
    return result;
}

#ifdef DEBUG_SOLOADER
/**
 * @file patch.c (SkinnedMesh Diagnostics)
 * @brief Skinned mesh rendering tracking for modular, X-Ray, and generic meshes.
 * @details Helps differentiate whether invisible enemies emit zero render calls
 *          or fail downstream in the OpenGL pipeline. Expanded capacity to 128 nodes.
 */
#define MAX_TRACKED_SCENE_NODES 128

static so_hook s_hook_modular_render;
static so_hook s_hook_xray_render;
static so_hook s_hook_skinned_render;

/**
 * @brief Registers and tracks unique scene node instances.
 * @param tag Identifier tag for the node type.
 * @param this_ptr Pointer to the node instance.
 * @param out_idx Output assigned index in tracking table.
 * @return 1 if first time seeing instance, 0 if previously known.
 */
static int render_diag_track(const char *tag, void *this_ptr, int *out_idx) {
    /**
     * @note Instance auditing per mesh variant (Modular, Xray, SkinnedMesh).
     *       Log_093.txt confirmed SkinnedMesh is the path used by enemies.
     */
    static struct { void *seen[MAX_TRACKED_SCENE_NODES]; int count; unsigned int calls; } state[3];
    static int overflowed[3];
    int idx = (tag[0] == 'X') ? 1 : (tag[0] == 'S') ? 2 : 0;
    state[idx].calls++;
    for (int i = 0; i < state[idx].count; i++) {
        if (state[idx].seen[i] == this_ptr) {
            *out_idx = idx * MAX_TRACKED_SCENE_NODES + i;
            return 0;
        }
    }
    if (state[idx].count < MAX_TRACKED_SCENE_NODES) {
        int i = state[idx].count++;
        state[idx].seen[i] = this_ptr;
        *out_idx = idx * MAX_TRACKED_SCENE_NODES + i;
        l_debug("[render_diag] %s render() this=%p (distinct=%d calls=%u)",
                tag, this_ptr, state[idx].count, state[idx].calls);
        return 1;
    }
    if (!overflowed[idx]) {
        overflowed[idx] = 1;
        l_debug("[render_diag] %s: MAX_TRACKED_SCENE_NODES (%d) hit, further distinct instances not logged",
                tag, MAX_TRACKED_SCENE_NODES);
    }
    *out_idx = -1;
    return 0;
}

/**
 * @brief Shadow diagnostics without visible model (log_102.txt).
 * @details Captures draw call count and textures for given instance after executing SO_CONTINUE.
 */
static unsigned char s_zero_draw_reported[3 * MAX_TRACKED_SCENE_NODES];

/**
 * @brief Detects misaligned blending or depth test configurations.
 * @param st Captured OpenGL node draw state.
 * @return Descriptive string if state combination hides rendering, or NULL.
 */
static const char *render_geom_suspicious(const GLNodeDrawState *st) {
    if (st->last_blend_enabled && st->last_blend_src_alpha == GL_ZERO)
        return "blend enabled with src_alpha=GL_ZERO -- draw contributes zero alpha to framebuffer";
    if (st->last_depth_test_enabled && !st->last_depth_write_mask)
        return "depth test on but depth WRITE disabled -- later opaque draws can paint over this one";
    return NULL;
}

static void render_diag_track_geom(const char *tag, void *this_ptr, so_hook hook, void *pass_ctx) {
    int idx;
    int is_new = render_diag_track(tag, this_ptr, &idx);
    gl_diag_reset_render_track();
    SO_CONTINUE(int, hook, this_ptr, pass_ctx);
    GLNodeDrawState st;
    gl_diag_get_render_track(&st);
    if (is_new) {
        l_debug("[render_geom_diag] %s this=%p draw_calls=%u last_texture=%d last_vertex_count=%d "
                "blend=%d blend_src_rgb=0x%04x blend_dst_rgb=0x%04x blend_src_a=0x%04x blend_dst_a=0x%04x "
                "depth_test=%d depth_write=%d",
                tag, this_ptr, st.draw_calls, (int) st.last_texture, (int) st.last_vertex_count,
                st.last_blend_enabled, st.last_blend_src_rgb, st.last_blend_dst_rgb,
                st.last_blend_src_alpha, st.last_blend_dst_alpha,
                st.last_depth_test_enabled, st.last_depth_write_mask);
        const char *suspicious = render_geom_suspicious(&st);
        if (suspicious) {
            l_warn("[render_geom_diag] %s this=%p SUSPICIOUS: %s", tag, this_ptr, suspicious);
        }
    }
    if (st.draw_calls == 0 && idx >= 0 && !s_zero_draw_reported[idx]) {
        s_zero_draw_reported[idx] = 1;
        l_warn("[render_geom_diag] %s this=%p render() issued ZERO draw calls (submesh list empty or fully culled)",
               tag, this_ptr);
    }
}

/**
 * @note Strict forwarding of render pass context parameter (mangled: 6renderEPv).
 */
static void modular_render_hook(void *this_, void *pass_ctx) {
    render_diag_track_geom("BaseMeshSceneNode<CModularSkinnedMeshSceneNode>", this_, s_hook_modular_render, pass_ctx);
}

static void xray_render_hook(void *this_, void *pass_ctx) {
    render_diag_track_geom("XrayModularSkinnedMeshSceneNode", this_, s_hook_xray_render, pass_ctx);
}

static void skinned_render_hook(void *this_, void *pass_ctx) {
    render_diag_track_geom("SkinnedMeshSceneNode", this_, s_hook_skinned_render, pass_ctx);
}

/**
 * @brief CharProperties::RecalcProperty hook and direct opacity reading.
 * @details Hooks on PROPS_GetOpacity were ineffective due to weak symbol inlining.
 *          Replaced by intercepting RecalcProperty(prop_id=49) and calling getter directly.
 */
typedef void (*RecalcProperty_t)(void *this_, int prop_id);
typedef float (*GetOpacity_t)(void *this_);
static so_hook s_hook_recalc_property;
static GetOpacity_t s_real_get_opacity;

#define PROP_OPACITY_MODIFIER 49
#define OPACITY_MODIFIER_OFFSET 0xb5c
#define MAX_TRACKED_RECALC 64
static void recalc_property_hook(void *this_, int prop_id) {
    SO_CONTINUE(int, s_hook_recalc_property, this_, prop_id);
    if (prop_id != PROP_OPACITY_MODIFIER) {
        return;
    }

    static struct { void *this_ptr; int raw; float opacity; } seen[MAX_TRACKED_RECALC];
    static int count;
    int raw = *(int *)((char *)this_ + OPACITY_MODIFIER_OFFSET);
    float opacity = s_real_get_opacity ? s_real_get_opacity(this_) : -1.0f;
    for (int i = 0; i < count; i++) {
        if (seen[i].this_ptr == this_) {
            if (seen[i].raw != raw || seen[i].opacity != opacity) {
                l_debug("[recalc_diag] Opacity_Modifier this=%p CHANGED raw %d -> %d, opacity %f -> %f",
                        this_, seen[i].raw, raw, seen[i].opacity, opacity);
                seen[i].raw = raw;
                seen[i].opacity = opacity;
            }
            return;
        }
    }
    if (count < MAX_TRACKED_RECALC) {
        seen[count].this_ptr = this_;
        seen[count].raw = raw;
        seen[count].opacity = opacity;
        count++;
        l_debug("[recalc_diag] Opacity_Modifier this=%p raw=%d opacity=%f (distinct=%d)%s",
                this_, raw, opacity, count, opacity == 0.0f ? " <-- ZERO OPACITY" : "");
    }
}

/**
 * @brief Interception of damage calculation CF__CalcDamage (0x3b1fb8) to debug instakills.
 * @details CalcDamageResult output structure (0x1C bytes) and exact ARM stack alignment.
 */
typedef struct {
    int damage;         // +0x00
    int unknown_04;
    int unknown_08;
    int unknown_0c;
    int dot[3];          // +0x10 CF_CalcDotDamage's 12-byte result, copied in verbatim
} CalcDamageResult;

/**
 * @note Invocation of combo multiplier (0x5e) and hit counter (0x14d0) in attack mode < 2.
 */
typedef int (*GetProperty_t)(void *this_, void *char_properties_ref, int id);
static GetProperty_t s_real_get_property;
#define PROP_ID_COMBO_MULTIPLIER 0x5e
#define COMBO_COUNTER_OFFSET 0x14d0
#define CHAR_PROPERTIES_OFFSET 0x560
#define CHARACTER_PROPERTIES_REF_OFFSET 0xff4

#define SUSPICIOUS_DAMAGE_THRESHOLD 2000
static so_hook s_hook_calc_damage;

static void *calc_damage_hook(void *out_result, void *attacker, void *defender, int p4_r3, int mode,
                               int stack_gap_unused, int p6, int p7) {
    unsigned short combo_counter = 0;
    int prop_combo_multiplier = -1;
    if (mode < 2 && attacker) {
        combo_counter = *(unsigned short *) ((char *) attacker + COMBO_COUNTER_OFFSET);
        if (s_real_get_property) {
            prop_combo_multiplier = s_real_get_property((char *) attacker + CHAR_PROPERTIES_OFFSET,
                                                          (char *) attacker + CHARACTER_PROPERTIES_REF_OFFSET,
                                                          PROP_ID_COMBO_MULTIPLIER);
        }
    }
    void *ret = SO_CONTINUE(void *, s_hook_calc_damage, out_result, attacker, defender, p4_r3, mode,
                             stack_gap_unused, p6, p7);
    int damage = out_result ? ((CalcDamageResult *) out_result)->damage : -1;
    l_debug("[damage_diag] CF__CalcDamage attacker=%p defender=%p mode=%d p4_r3=%d p6=%d p7=%d "
            "combo_counter=%u prop_0x5e=%d -> damage=%d",
            attacker, defender, mode, p4_r3, p6, p7, combo_counter, prop_combo_multiplier, damage);
    if (damage < 0 || damage > SUSPICIOUS_DAMAGE_THRESHOLD) {
        l_warn("[damage_diag] SUSPICIOUS damage=%d (attacker=%p defender=%p combo_counter=%u prop_0x5e=%d) "
               "-- likely the instakill bug",
               damage, attacker, defender, combo_counter, prop_combo_multiplier);
    }
    return ret;
}

/**
 * @brief CharProperties::_GetProperty hook to catch anomalous or unstable values.
 * @details Filters reads outside [-100000, 100000] range and detects live memory corruption.
 */
#define EXTREME_PROPERTY_THRESHOLD 100000
#define MAX_TRACKED_PROPERTIES 256
static so_hook s_hook_get_property;

static int get_property_hook(void *this_, void *char_properties_ref, int id) {
    int value = SO_CONTINUE(int, s_hook_get_property, this_, char_properties_ref, id);
    if (value <= EXTREME_PROPERTY_THRESHOLD && value >= -EXTREME_PROPERTY_THRESHOLD) {
        return value;
    }

    static struct { void *this_ptr; int id; int last_value; int change_count; } seen[MAX_TRACKED_PROPERTIES];
    static int count;
    static int overflowed;

    for (int i = 0; i < count; i++) {
        if (seen[i].this_ptr == this_ && seen[i].id == id) {
            if (seen[i].last_value != value) {
                seen[i].change_count++;
                if (seen[i].change_count <= 3) {
                    l_warn("[prop_diag] this=%p property_id=%d value CHANGED %d -> %d on repeat read of the "
                           "SAME address -- something else is writing this memory, not just uninitialized",
                           this_, id, seen[i].last_value, value);
                }
                seen[i].last_value = value;
            }
            return value;
        }
    }
    if (count < MAX_TRACKED_PROPERTIES) {
        seen[count].this_ptr = this_;
        seen[count].id = id;
        seen[count].last_value = value;
        seen[count].change_count = 0;
        count++;
        l_warn("[prop_diag] this=%p property_id=%d -> EXTREME value=%d (distinct=%d)",
               this_, id, value, count);
    } else if (!overflowed) {
        overflowed = 1;
        l_warn("[prop_diag] MAX_TRACKED_PROPERTIES (%d) hit -- further distinct (this,id) pairs not logged",
               MAX_TRACKED_PROPERTIES);
    }
    return value;
}

/**
 * @brief Potion button diagnostic and NativeUsePotion / HasPotion validation.
 * @details Validates if touch events reach native code and evaluates GetHPPercent / GetMPPercent.
 */
typedef float (*GetPercent_t)(void *this_);
static so_hook s_hook_use_potion;
static so_hook s_hook_has_potion;
static GetPercent_t s_real_get_hp_percent;
static GetPercent_t s_real_get_mp_percent;

static void use_potion_hook(void *fn_call_ref) {
    l_warn("[potion_diag] NativeUsePotion CALLED (fn_call=%p) -- AS2 click dispatch reached native code",
           fn_call_ref);
    /**
     * @note SO_CONTINUE uses `int` as register container for void functions.
     */
    SO_CONTINUE(int, s_hook_use_potion, fn_call_ref);
}

/**
 * @note HasPotion() returns bool in r0 (SO_CONTINUE uses int as container).
 */
static int has_potion_hook(void *this_) {
    int result = SO_CONTINUE(int, s_hook_has_potion, this_);
    float hp_pct = s_real_get_hp_percent ? s_real_get_hp_percent(this_) : -1.0f;
    float mp_pct = s_real_get_mp_percent ? s_real_get_mp_percent(this_) : -1.0f;
    l_warn("[potion_diag] Character::HasPotion this=%p -> %d (HP_percent=%.6f MP_percent=%.6f, "
           "NativeUsePotion only calls Cmd_UsePotion if either is < 1.0 -- the real heal happens deeper, "
           "not yet hooked)",
           this_, result, hp_pct, mp_pct);
    return result;
}

/**
 * @brief GameSWF MovieClip lifecycle tracking (attach_movie, clone, remove).
 * @details Measures creation vs destruction rate of clips to detect UI memory leaks.
 */
static so_hook s_hook_attach_movie;
static so_hook s_hook_clone_display_object;
static so_hook s_hook_remove_display_object;

#define MAX_TRACKED_CLIP_PARENTS 32
static void clip_diag_track(const char *tag, void *this_ptr, int delta) {
    static struct { void *this_ptr; int net; unsigned int creates; unsigned int removes; } state[MAX_TRACKED_CLIP_PARENTS];
    static int count;
    int i;
    for (i = 0; i < count; i++) {
        if (state[i].this_ptr == this_ptr) break;
    }
    if (i == count) {
        if (count >= MAX_TRACKED_CLIP_PARENTS) return;
        state[i].this_ptr = this_ptr;
        state[i].net = 0;
        state[i].creates = 0;
        state[i].removes = 0;
        count++;
    }
    if (delta > 0) state[i].creates++; else state[i].removes++;
    state[i].net += delta;
    l_debug("[clip_diag] %s parent=%p net=%d creates=%u removes=%u",
            tag, this_ptr, state[i].net, state[i].creates, state[i].removes);
}

static void *hook_attach_movie(void *this_, void *id, void *newname, int depth) {
    void *result = SO_CONTINUE(void *, s_hook_attach_movie, this_, id, newname, depth);
    clip_diag_track("attach_movie", this_, 1);
    return result;
}

static void *hook_clone_display_object(void *this_, void *newname, int depth) {
    void *result = SO_CONTINUE(void *, s_hook_clone_display_object, this_, newname, depth);
    clip_diag_track("clone_display_object", this_, 1);
    return result;
}

static void *hook_remove_display_object(void *this_, void *character_ptr) {
    void *result = SO_CONTINUE(void *, s_hook_remove_display_object, this_, character_ptr);
    clip_diag_track("remove_display_object", this_, -1);
    return result;
}
#endif // DEBUG_SOLOADER

void so_patch(void) {
    /**
     * @brief Bypass Gameloft DRM license validation checks.
     */
    hook_addr((uintptr_t)so_symbol(&so_mod, "ALicenseCheck_ValidateLicense"), (uintptr_t)&ret_void);
    hook_addr((uintptr_t)so_symbol(&so_mod, "_ZN13ALicenseCheck14ValidateServerEb"), (uintptr_t)&ret_void);
    hook_addr((uintptr_t)so_symbol(&so_mod, "_ZN13ALicenseCheck14ValidateNativeEv"), (uintptr_t)&ret_void);
    hook_addr((uintptr_t)so_symbol(&so_mod, "_ZN13ALicenseCheck7LoadRMSEv"), (uintptr_t)&ret1);

    /**
     * @brief Patch "HUDStyle" return value.
     */
    s_hasOption = (hasOption_t)so_symbol(&so_mod, "_ZNK15SavegameManager9hasOptionEPKc");
    s_hook_get_saved_option = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN11Application14GetSavedOptionEPKc"),
        (uintptr_t)&hook_GetSavedOption);

#ifdef DEBUG_SOLOADER
    /**
     * @note Mesh rendering, GameSWF, and character property hooks are disabled.
     * @reason Inline kubridge trampolines on high-frequency calls cause ARM stack desynchronization
     *         and heap corruption in PS Vita runtime (log_119.txt).
     */
    /*
    s_hook_modular_render = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN17BaseMeshSceneNodeIN6glitch7collada28CModularSkinnedMeshSceneNodeEE6renderEPv"),
        (uintptr_t)&modular_render_hook);
    s_hook_xray_render = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN31XrayModularSkinnedMeshSceneNode6renderEPv"),
        (uintptr_t)&xray_render_hook);
    s_hook_skinned_render = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN17BaseMeshSceneNodeIN6glitch7collada21CSkinnedMeshSceneNodeEE6renderEPv"),
        (uintptr_t)&skinned_render_hook);
    */

    s_real_get_opacity = (GetOpacity_t)so_symbol(&so_mod, "_ZNK14CharProperties16PROPS_GetOpacityEv");

    uintptr_t get_property_addr = (uintptr_t)so_symbol(
        &so_mod, "_ZNK14CharProperties12_GetPropertyERKN7Structs19CharacterPropertiesEi");
    s_real_get_property = (GetProperty_t)get_property_addr;

    /*
    s_hook_attach_movie = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN7gameswf15sprite_instance12attach_movieERKNS_9tu_stringES1_i"),
        (uintptr_t)&hook_attach_movie);
    s_hook_clone_display_object = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN7gameswf15sprite_instance20clone_display_objectERKNS_9tu_stringEi"),
        (uintptr_t)&hook_clone_display_object);
    s_hook_remove_display_object = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN7gameswf15sprite_instance21remove_display_objectEPNS_9characterE"),
        (uintptr_t)&hook_remove_display_object);
    */

    s_real_get_hp_percent = (GetPercent_t)so_symbol(&so_mod, "_ZNK9Character12GetHPPercentEv");
    s_real_get_mp_percent = (GetPercent_t)so_symbol(&so_mod, "_ZNK9Character12GetMPPercentEv");
#endif
}
