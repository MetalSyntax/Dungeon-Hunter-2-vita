/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
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

// Application::GetSavedOption(char const*) (out_ghidra.c:20949) returns
// SavegameManager::getOption() when the key is registered, but on a miss
// (SavegameManager::hasOption() == 0) it does a bare "return;" with no
// explicit return value -- i.e. whatever garbage was already in r0. The
// original theory here was that a missing "HUDStyle" default was picking a
// scrollable icon-list HUD carousel (>=2) instead of the fixed 3-icon
// phone-style bar (<2), and that THIS was the cause of the repeating icon
// column bug (see PORTING_PLAN.md Phase 6). **That theory is FALSIFIED**:
// log_093.txt (real hardware, with the l_debug line below active) shows
// hasOption("HUDStyle")==1 and the real stored value ==0 on every single
// call, all session -- the fixed-bar branch is genuinely selected already,
// and the icon-column bug is caused by something else entirely (still under
// investigation as of this comment, see PORTING_PLAN.md). This hook is being
// left in place as a harmless defensive no-op/safety net (it never actually
// overrides anything on real hardware, per log_093) rather than ripped out,
// since forcing the phone-style bar is still the objectively correct choice
// for Vita's screen if some other boot path/save state ever DOES leave
// "HUDStyle" unregistered or >=2 -- but do not treat this as the fix for the
// icon-column bug anymore.
typedef int (*hasOption_t)(void *savegame_mgr, const char *key);
static hasOption_t s_hasOption;
static so_hook s_hook_get_saved_option;

static int hook_GetSavedOption(void *this_, const char *key) {
    int result = SO_CONTINUE(int, s_hook_get_saved_option, this_, key);
    if (strcmp(key, "HUDStyle") == 0) {
        void *savegame_mgr = *(void **)((char *)this_ + 0x4c);
        int has_option = s_hasOption(savegame_mgr, key);
        // log_092.txt (2026-07-26/27): the repeating icon column persisted
        // with the original miss-only version of this hook (force 0 only
        // when hasOption()==0), which was never confirmed against a real
        // log -- no build before this one ever printed has_option/result for
        // "HUDStyle", so there was no way to tell "the key really is missing
        // and this hook isn't firing/matching" apart from "the key IS
        // registered with a genuine value >=2" (e.g. the same width>=960
        // bucket-selection quirk already seen elsewhere in this project --
        // see PORTING_PLAN.md's g_windowDimensions/distortion-fix entry --
        // could plausibly register a real 'tablet' HUDStyle default for a
        // 960-wide screen, not a missing/garbage one). This log line answers
        // that directly on the next hardware run.
        l_debug("[hook_HUDStyle] savegame_mgr=%p hasOption=%d pre_override_result=%d",
                savegame_mgr, has_option, result);
        // Force the fixed 3-icon phone-style bar unconditionally now,
        // regardless of which of the two cases above turns out to be true:
        // Vita's real screen (960x544, not a real tablet) has no correct use
        // for the scrollable carousel HUD either way, and this is a single-
        // key, additive override with no effect on any other saved option.
        if (result >= 2 || !has_option) {
            l_debug("[hook_HUDStyle] forcing HUDStyle=0 (has_option=%d raw_result=%d)",
                    has_option, result);
            return 0;
        }
    }
    return result;
}

#ifdef DEBUG_SOLOADER
// Diagnostic for the "enemy nameplate/healthbar renders but no monster model
// appears" bug: this engine has TWO different skinned-mesh render entry
// points -- the plain modular one (BaseMeshSceneNode<CModularSkinnedMeshSceneNode>,
// a template instantiation, used by ordinary characters as far as static
// analysis could tell) and an "Xray"-prefixed variant (XrayModularSkinnedMeshSceneNode,
// found by tracing Character::Draw in out_ghidra.c, but it wasn't confirmed
// whether that's the path enemies specifically go through or a special-effect
// variant) -- so hook both and log every distinct scene-node instance whose
// render() is actually invoked. Compare the resulting per-path distinct-
// instance/call counts against how many characters/enemies are actually on
// screen in the next hardware test: this tells "an enemy's draw call is never
// issued at all" apart from "it's issued but produces nothing visible"
// (a bad mesh-buffer/material/transform further down), which static analysis
// of the decompiled sources alone couldn't resolve.
//
// log_093.txt (2026-07-27): the SkinnedMeshSceneNode path hit the old cap of
// 32 distinct instances well before the session ended (same silent-overflow
// class of bug already found and fixed once for the texture tracker in
// glutil.c -- see MAX_TRACKED_TEXTURES there) -- raised to 128 and added an
// overflow flag so a future cap-hit is visible in the log instead of just
// silently stopping at 32/32 forever.
#define MAX_TRACKED_SCENE_NODES 128

static so_hook s_hook_modular_render;
static so_hook s_hook_xray_render;
static so_hook s_hook_skinned_render;

// Returns the tracked-instance index for (tag, this_ptr) via *out_idx, and 1
// if this is the first time this pointer has been seen for this tag (0 if
// already known) -- callers use the "first sighting" flag to decide when to
// log verbose geometry/texture info (every frame would flood the log for
// characters that render fine every single frame).
static int render_diag_track(const char *tag, void *this_ptr, int *out_idx) {
    // log_091.txt: with only the modular+Xray hooks in place, the modular
    // path fired for just 2 distinct instances the ENTIRE session (one of
    // them exactly once, never again) despite 688 distinct .bdae loads across
    // 8+ character types (moth, lizardman, troll, faeries, npcs, swampking,
    // root_troll...) -- Xray fired zero times. That's the modular scene-node
    // class the PLAYER's equippable-armor path uses (see out_ghidra.c symbol
    // name), not a generic "every character" path. Added this third hook for
    // BaseMeshSceneNode<CSkinnedMeshSceneNode> (the non-modular skinned mesh
    // variant, likely what fixed-mesh monsters use instead) to see if THAT's
    // where enemy renders actually go, and if a specific invisible enemy's
    // instance is present in that count at all. log_093.txt confirmed this IS
    // the generic path: 32+ distinct instances (hit the old cap) with call
    // counts climbing every frame, vs. the other two hooks' 1-2 instances.
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

// Diagnostic for "enemy renders a shadow but no model" (user-reported,
// log_102.txt): wraps each render() call's SO_CONTINUE with
// gl_diag_reset_render_track()/gl_diag_get_render_track() (glutil.c) to
// attribute actual GL draw-call/texture/vertex-count activity to this
// specific scene-node instance. A shadow decal is a separate, much simpler
// draw (flat quad/blob, no skinning) that plausibly goes through neither this
// render() nor a bound diffuse texture at all -- if that's genuinely why it
// stays visible while the model doesn't, the model's OWN render() call here
// should show draw_calls==0 (submesh list empty/culled -- nothing to draw at
// all) or a suspicious texture id, settling which half of the pipeline is at
// fault instead of guessing between "never draws" and "draws something
// invisible". zero_draw_reported logs the zero-draw-calls case exactly once
// per distinct instance (not every frame) since a genuinely broken enemy
// would otherwise flood the log every single frame it's on screen.
static unsigned char s_zero_draw_reported[3 * MAX_TRACKED_SCENE_NODES];

// A draw that legitimately runs but composites to invisible/see-through is a
// blend or depth-write problem, not a "nothing drew" one -- flag the specific
// combinations that plausibly explain "draws, but you can't see it":
// GL_ZERO as the alpha src factor (fragment alpha contributes nothing to the
// framebuffer no matter the shader's own output) or depth writes disabled
// while depth TEST stays on (this draw can be immediately painted over by
// anything behind it in submission order without ever failing its own depth
// test). Logged once per distinct instance alongside the raw state, not
// asserted as definitely the cause -- the raw values are there for manual
// correlation too.
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

// render(void*) mangles as "6renderEPv" -- one EXPLICIT void* parameter
// beyond the implicit `this` (likely a render-pass/context pointer), so both
// args must be captured and forwarded through SO_CONTINUE unchanged, or the
// real function would run with a garbage second argument.
static void modular_render_hook(void *this_, void *pass_ctx) {
    render_diag_track_geom("BaseMeshSceneNode<CModularSkinnedMeshSceneNode>", this_, s_hook_modular_render, pass_ctx);
}

static void xray_render_hook(void *this_, void *pass_ctx) {
    render_diag_track_geom("XrayModularSkinnedMeshSceneNode", this_, s_hook_xray_render, pass_ctx);
}

static void skinned_render_hook(void *this_, void *pass_ctx) {
    render_diag_track_geom("SkinnedMeshSceneNode", this_, s_hook_skinned_render, pass_ctx);
}

// Diagnostic for the invisible-enemy bug -- PROPS_GetOpacity/Character::GetOpacity
// hooks CLOSED as a dead end (2026-08-04, log_098.txt): both symbols resolve
// fine (confirmed via `nm -D`, real T/W entries) and the hook mechanism itself
// is proven working in this same build (render_diag/clip_diag below both fired
// plenty), but across a full ~2008-frame session spanning multiple checkpoints
// and character types, NEITHER opacity getter was called even once. Both are
// weak (`W`) symbols, consistent with the compiler having fully inlined every
// real call site and left an unreferenced leftover symbol behind -- hooking
// this address can never observe those inlined reads. Replaced with a hook on
// CharProperties::RecalcProperty(int) (out_ghidra.c:159220, a strong `T`
// symbol, so not inlined away) filtered to property 49 ("Opacity_Modifier").
//
// 2026-08-04 follow-up (log_099.txt): the first version of this hook only read
// the raw property-sheet slot directly (`this+0xb5c`, per PORTING_PLAN.md's
// offset derivation) and got `raw=0` for ALL 52 distinct instances seen that
// session, no exceptions -- including, presumably, the player, who is not
// reported invisible. That's inconsistent with "raw==0 means alpha==0 at
// render time" (every character would be invisible, not just enemies), which
// means either the offset derivation is wrong, or "Opacity_Modifier" really is
// an additive delta (0 = "no active modifier", the normal/expected case) and
// the real alpha PROPS_GetOpacity returns comes from combining it with
// something else this hook was never reading. Rather than guess further,
// PROPS_GetOpacity itself is now called DIRECTLY (not hooked -- the hook on it
// was removed above, so the original function at that address is untouched)
// right after RecalcProperty(this, 49) finishes, passing the same `this_`.
// This sidesteps the "getter is inlined at its real call sites, so hooking it
// never fires" problem entirely: we're not intercepting a call the engine
// makes, we're placing our own direct call to get the authoritative scaled
// value, logged alongside the raw slot so a mismatch between them settles
// whether the offset math was ever right.
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

// Diagnostic for the "instakill both ways" bug (user-reported, log_105.txt):
// player and enemies both die in ~1 hit, with damage numbers described as
// absurdly high -- "like a bad hack got applied". `nm -D` on the real .so
// found a real, non-inlined (T, not W) free function with exactly the right
// shape for this: CF__CalcDamage(Character*, Character*, int, int, int, bool,
// bool) at 0x3b1fb8 (mangled _Z14CF__CalcDamageP9CharacterS0_iiibb).
//
// 2026-08-07 (log_106.txt) CORRECTION #1: the first build of this hook
// logged its RETURN value as "the damage" and got consistently huge negative
// numbers (e.g. -2129131628). That looked like the bug itself, but it was a
// bug in the DIAGNOSTIC, not the game: out_ghidra.c:127028's body writes
// through the first parameter with raw offsets (`*(int*)param_1 = iVar7`,
// `*(int*)(param_1+4) = ...`, up to `+0x18`, a 0x1C-byte output struct) and
// ends with `return param_1;` -- it's an OUTPUT struct pointer being echoed
// back (same convention as the very next documented function,
// Character::_F_CalculateResult(AttackResult&, Character*, Character*, ...)
// -- nm/mangling still call its declared type "Character*", but the compiled
// body never treats it as a live Character), not the live "attacker"
// Character* nm's demangled signature implied. Confirmed empirically:
// 0x81180794 read as a signed int is exactly -2129131628, the "damage"
// value logged last time -- that was this pointer's own bit pattern printed
// back, nothing more.
//
// CORRECTION #2, same session, found by disassembling the real function
// (`objdump -d --start-address=0x3b1fb8`) instead of trusting Ghidra's
// pseudo-C further -- the prologue reads: r0/r1/r2 -> r4/r5/r9 (params 1-3),
// then `ldr r12,[sp,#0x50]` for param 5 (the mode/branch selector: the
// following cmp/bls/beq chain against 1/2/3 is exactly Ghidra's
// `param_5<2`/`==2`/`==3` branches), then `ldrb r11,[sp,#0x58]` and
// `ldrb r8,[sp,#0x5c]` for the two trailing bools. Relative to the first
// stack slot (param 5 at +0x50), that's +0x08 and +0x0c for the two bools --
// i.e. a genuinely unused 4-byte stack slot at +0x04 that nothing ever
// reads. A hook declared with 7 tightly-packed params (as the first fix did)
// has ITS OWN compiler place the two bools at +0x04/+0x08 instead, one slot
// early -- SO_CONTINUE would then hand the real function garbage for the
// very last bool (whatever leftover stack data happened to sit at +0x0c).
// Harmless in practice so far (a stray nonzero byte just reads as
// "true" for a bool, never dereferenced) but not something to leave in
// deliberately now that it's identified -- an explicit unused slot below
// closes the gap so SO_CONTINUE's re-invocation matches the real caller's
// stack layout exactly.
typedef struct {
    int damage;         // +0x00
    int unknown_04;
    int unknown_08;
    int unknown_0c;
    int dot[3];          // +0x10 CF_CalcDotDamage's 12-byte result, copied in verbatim
} CalcDamageResult;

// 2026-08-07 (log_107.txt) CORRECTION #3, now that params/return are read
// correctly: the numbers are real and reproducible, not another diagnostic
// bug -- mode==2 hits consistently return a clean damage=256 (0x100, the
// same "clamp to minimum" constant seen throughout Character_Formulas.cpp's
// decompiled body), while mode<2 hits ("normal attack", far more frequent in
// this log, matching what should be the common case) return wildly different
// billion-scale numbers every time (1339412702, 1866444032, ...). That
// selectivity -- one branch clean, the other branch broken -- means the bug
// is real and specific to the mode<2 formula, not an ABI mismatch (which
// would corrupt every call, not just one branch).
//
// Prime suspect found by reading Character_Formulas.cpp's decompiled body
// for that branch (out_ghidra.c:127108-127112):
//   uVar1 = *(ushort *)(param_2 + 0x14d0);          // "combo counter" field
//   if (uVar1 != 0) {
//       iVar7 = CharProperties::_GetProperty(..., 0x5e);
//       local_4c = (uint)uVar1 * iVar7 + local_4c;   // <-- unbounded multiply
//   }
// param_2 here is the ATTACKER (confirmed: same register as our `attacker`).
// The same offset (0x14d0) is incremented elsewhere in this file
// (Character::F_ApplyResult, out_ghidra.c:126918-126924) as a hit-streak/
// combo counter that resets to 0 on certain conditions -- if that reset
// condition doesn't fire the way it does on Android (different memory
// layout/init order is exactly the kind of thing that breaks silently across
// a port), or if the field is simply uninitialized garbage on a freshly
// spawned character on this build, this multiply has no bound at all. Log
// both operands (not just the final damage) so the next hardware run says
// definitively which one is garbage -- read BEFORE SO_CONTINUE since that's
// the value the real formula actually multiplies (F_ApplyResult increments
// it separately, later, for the *next* hit).
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

// 2026-08-07 (log_108.txt) follow-up: the combo-counter theory only covers
// the mode<2 branch, and this log shows the bug ISN'T mode-specific after
// all -- one mode==2 hit came back damage=256 (clean) and ANOTHER mode==2
// hit came back damage=1690618550 (garbage), same branch, same formula,
// wildly different outcomes. Whatever's wrong is a property VALUE, not a
// fixed code path -- both CF__CalcDamage branches (mode<2 and mode==2) pull
// a dozen-plus different property IDs (0x4f, 0x50, 0x61, 0x49, 0x5e, 0xae,
// 0xaf, ... out_ghidra.c:127063-127251) through this exact function, so
// instead of guessing which ID is bad from Ghidra's messy pseudo-C, hook the
// getter itself and flag ANY call (from CF__CalcDamage or anywhere else)
// that returns something outside a real stat's plausible range. This is a
// hot path (called for animation/UI stat reads too, not just damage), so it
// must stay silent on the overwhelmingly common in-range case -- only the
// rare extreme value should ever produce a log line.
// 2026-08-07 (log_109.txt) follow-up: this hook is real and working -- it's
// not narrow at all, it fired 14323 times in one session (character menu
// screenshot confirms the scale: HP -4067229, MP -667370, Defense rating
// -8898, Damage reduction -20187 -- the CHARACTER'S BASE STATS are wrong,
// not just combat damage math). At that volume the unthrottled version was
// almost certainly a real, separate contributor to the user's reported
// framerate drop (thousands of l_warn calls -- string formatting + log
// sink I/O -- every session), so this needed throttling regardless of the
// investigation.
//
// While throttling, found the more important signal: for the SAME (this_,
// property_id) pair, some property IDs read a STABLE garbage value every
// time (ids 5/6/18 always exactly 1835084/1835084/1507432 for one
// instance), while others are a DIFFERENT garbage value on every single
// call to the exact same id on the exact same instance (id 59: 1626864128,
// then 699528192, then -1041239552, then 1512960000, all within the same
// session). _GetProperty itself (out_ghidra.c:158231) is a trivial
// `*(int*)(char_properties_ref + m_dataOffsets[id] + 4)` -- no per-instance
// branching, no "not populated for this class" fallback -- so a value that
// CHANGES between reads of the same fixed address means something ELSE is
// actively writing into that memory between calls (real corruption /
// undersized allocation letting an adjacent system's writes bleed in), not
// just "this property was never initialized". That's now flagged
// explicitly instead of buried in repeat log spam.
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

// Diagnostic for the "heal button does nothing" bug (user-reported,
// log_109.txt, touch specifically -- not the physical-R1 stopgap in
// action_btn_map above). `nm -D` found the real AS2->native binding the
// HUD's potion icon calls: NativeUsePotion(gameswf::fn_call const&)
// (mangled _Z15NativeUsePotionRKN7gameswf7fn_callE, strong T symbol). Its
// decompiled body (out_ghidra.c:219100, Ghidra loses track of several
// float-ABI args here -- typical for this soft-float codebase, not trusted
// beyond the overall shape) reads the player character, then gates
// everything on `Character::HasPotion()` -- if that's false, the function
// returns having done nothing, no heal, no potion-count change, exactly the
// reported symptom. Both are real, non-inlined symbols, hooked the same
// way as every other diagnostic in this file. This settles three possible
// causes in one hardware round-trip: NativeUsePotion never firing at all
// (the touch/AS2 click dispatch never reaches native code -- an input bug,
// same family as the confirmed-dead-twice Cross/Square touch mapping) vs.
// firing but HasPotion() reading false (a data bug -- plausibly the SAME
// corrupted-property-read issue as the HP/MP/Defense stats, if potion count
// is itself a CharProperties-backed value) vs. firing with HasPotion() true
// but still not healing (a bug further inside NativeUsePotion's own body,
// not reachable from here without deeper hooks).
// 2026-08-08 (log_111.txt) follow-up: confirmed on real hardware -- both
// hooks fire, HasPotion() reads true, and the user reports potions ARE
// being consumed, but HP/MP never actually go up (HP reads exactly 0).
// Disassembled NativeUsePotion directly (`objdump -d --start-address
// 0x43d2c8`, more trustworthy here than Ghidra's cut-off pseudo-C) to see
// what it does after the HasPotion() check: it is NOT the function that
// applies healing at all -- it only computes
// `Character::GetHPPercent()`/`GetMPPercent()` and, if EITHER is below
// 100%, calls `v2Controller::Cmd_UsePotion()` (0x4056b0), which itself just
// forwards through a vtable slot (offset 0x4c) to whatever concrete
// controller class handles it -- the real heal/potion-consume logic lives
// several calls deeper than anything hooked so far. Rather than chase that
// vtable dispatch blind, read the two percentages this gate actually uses
// DIRECTLY (same non-hooked-direct-call pattern as PROPS_GetOpacity/
// _GetProperty above) right when HasPotion() fires -- if HP%/MP% themselves
// already read as garbage (NaN, wildly negative, etc.) instead of a sane
// 0.0-1.0 range, that's the same base-stat corruption as the character menu
// screenshot (HP -4067229) reaching all the way into this gate, and no
// amount of chasing the vtable call further would find a real bug to fix
// there -- the fix would have to be upstream, at the stat corruption itself.
typedef float (*GetPercent_t)(void *this_);
static so_hook s_hook_use_potion;
static so_hook s_hook_has_potion;
static GetPercent_t s_real_get_hp_percent;
static GetPercent_t s_real_get_mp_percent;

static void use_potion_hook(void *fn_call_ref) {
    l_warn("[potion_diag] NativeUsePotion CALLED (fn_call=%p) -- AS2 click dispatch reached native code",
           fn_call_ref);
    // NativeUsePotion's real return type is void -- SO_CONTINUE's macro
    // needs a concrete scalar type to declare its temporary, so `int` is
    // used here purely as a register-sized placeholder (same convention as
    // the render() hooks above) and the value is discarded.
    SO_CONTINUE(int, s_hook_use_potion, fn_call_ref);
}

// HasPotion() returns a bool in r0 -- SO_CONTINUE's `int` instantiation
// reads that register correctly regardless of the real return type's exact
// width (same convention already relied on throughout this file).
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

// Diagnostic for the repeating icon-column HUD bug (PORTING_PLAN.md Phase 6,
// 2026-07-27): extensive static analysis of the decompiled AS2 bytecode in
// dqhud.swf/dqshared.swf/dqmenus.swf (via a real Flash decompiler, not just
// the C++ pseudo-C) ruled out every native-binding hypothesis tried so far
// (NativeGetBuffs is dead code -- never called by any AS in this build; the
// "HUDStyle" skill-bar/carousel selector is confirmed NOT the cause on real
// hardware, log_093.txt; the status-message/achievement-toast widgets are
// single reusable popups, not repeaters). None of the AS classes actually
// shipped in this game's SWFs define a distinct "icon list" component other
// than the already-working skill bar. This means the actual duplication, if
// it's an AS2-level bug at all, has to go through one of gameswf's own
// engine-level "create a new child clip" primitives -- attachMovie()
// (gameswf::sprite_instance::attach_movie) or duplicateMovieClip()
// (gameswf::sprite_instance::clone_display_object) -- without a matching
// removeMovieClip() (gameswf::sprite_instance::remove_display_object). Rather
// than parse gameswf::tu_string's internal buffer layout (never fully
// confirmed from the decompiled sources, and getting it wrong risks
// dereferencing garbage on real hardware with no way to test it before the
// next session), this hook deliberately never touches the tu_string/character
// argument contents -- it only uses each call's `this` (the parent
// sprite_instance) as an opaque dictionary key and counts attach/clone calls
// against remove calls. If some parent's net count climbs without bound
// during a play session where the icon column is visible, that identifies
// the exact leaking call site (by instance pointer, cross-referenced against
// timing) without needing to have found the offending AS script by name.
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
    // License check bypass
    hook_addr((uintptr_t)so_symbol(&so_mod, "ALicenseCheck_ValidateLicense"), (uintptr_t)&ret_void);
    hook_addr((uintptr_t)so_symbol(&so_mod, "_ZN13ALicenseCheck14ValidateServerEb"), (uintptr_t)&ret_void);
    hook_addr((uintptr_t)so_symbol(&so_mod, "_ZN13ALicenseCheck14ValidateNativeEv"), (uintptr_t)&ret_void);
    hook_addr((uintptr_t)so_symbol(&so_mod, "_ZN13ALicenseCheck7LoadRMSEv"), (uintptr_t)&ret1);

    // "HUDStyle" garbage-return fix -- see the comment above hook_GetSavedOption.
    s_hasOption = (hasOption_t)so_symbol(&so_mod, "_ZNK15SavegameManager9hasOptionEPKc");
    s_hook_get_saved_option = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN11Application14GetSavedOptionEPKc"),
        (uintptr_t)&hook_GetSavedOption);

#ifdef DEBUG_SOLOADER
    s_hook_modular_render = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN17BaseMeshSceneNodeIN6glitch7collada28CModularSkinnedMeshSceneNodeEE6renderEPv"),
        (uintptr_t)&modular_render_hook);
    s_hook_xray_render = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN31XrayModularSkinnedMeshSceneNode6renderEPv"),
        (uintptr_t)&xray_render_hook);
    s_hook_skinned_render = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN17BaseMeshSceneNodeIN6glitch7collada21CSkinnedMeshSceneNodeEE6renderEPv"),
        (uintptr_t)&skinned_render_hook);

    s_hook_recalc_property = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN14CharProperties14RecalcPropertyEi"),
        (uintptr_t)&recalc_property_hook);
    s_real_get_opacity = (GetOpacity_t)so_symbol(&so_mod, "_ZNK14CharProperties16PROPS_GetOpacityEv");

    s_hook_calc_damage = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_Z14CF__CalcDamageP9CharacterS0_iiibb"),
        (uintptr_t)&calc_damage_hook);
    uintptr_t get_property_addr = (uintptr_t)so_symbol(
        &so_mod, "_ZNK14CharProperties12_GetPropertyERKN7Structs19CharacterPropertiesEi");
    s_real_get_property = (GetProperty_t)get_property_addr;
    s_hook_get_property = hook_addr(get_property_addr, (uintptr_t)&get_property_hook);

    s_hook_attach_movie = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN7gameswf15sprite_instance12attach_movieERKNS_9tu_stringES1_i"),
        (uintptr_t)&hook_attach_movie);
    s_hook_clone_display_object = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN7gameswf15sprite_instance20clone_display_objectERKNS_9tu_stringEi"),
        (uintptr_t)&hook_clone_display_object);
    s_hook_remove_display_object = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN7gameswf15sprite_instance21remove_display_objectEPNS_9characterE"),
        (uintptr_t)&hook_remove_display_object);

    s_hook_use_potion = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_Z15NativeUsePotionRKN7gameswf7fn_callE"),
        (uintptr_t)&use_potion_hook);
    s_hook_has_potion = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZNK9Character9HasPotionEv"),
        (uintptr_t)&has_potion_hook);
    s_real_get_hp_percent = (GetPercent_t)so_symbol(&so_mod, "_ZNK9Character12GetHPPercentEv");
    s_real_get_mp_percent = (GetPercent_t)so_symbol(&so_mod, "_ZNK9Character12GetMPPercentEv");
#endif
}
