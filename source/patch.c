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

static void render_diag_track(const char *tag, void *this_ptr) {
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
        if (state[idx].seen[i] == this_ptr) return;
    }
    if (state[idx].count < MAX_TRACKED_SCENE_NODES) {
        state[idx].seen[state[idx].count++] = this_ptr;
        l_debug("[render_diag] %s render() this=%p (distinct=%d calls=%u)",
                tag, this_ptr, state[idx].count, state[idx].calls);
    } else if (!overflowed[idx]) {
        overflowed[idx] = 1;
        l_debug("[render_diag] %s: MAX_TRACKED_SCENE_NODES (%d) hit, further distinct instances not logged",
                tag, MAX_TRACKED_SCENE_NODES);
    }
}

// render(void*) mangles as "6renderEPv" -- one EXPLICIT void* parameter
// beyond the implicit `this` (likely a render-pass/context pointer), so both
// args must be captured and forwarded through SO_CONTINUE unchanged, or the
// real function would run with a garbage second argument.
static void modular_render_hook(void *this_, void *pass_ctx) {
    render_diag_track("BaseMeshSceneNode<CModularSkinnedMeshSceneNode>", this_);
    SO_CONTINUE(int, s_hook_modular_render, this_, pass_ctx);
}

static void xray_render_hook(void *this_, void *pass_ctx) {
    render_diag_track("XrayModularSkinnedMeshSceneNode", this_);
    SO_CONTINUE(int, s_hook_xray_render, this_, pass_ctx);
}

static void skinned_render_hook(void *this_, void *pass_ctx) {
    render_diag_track("SkinnedMeshSceneNode", this_);
    SO_CONTINUE(int, s_hook_skinned_render, this_, pass_ctx);
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

    s_hook_attach_movie = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN7gameswf15sprite_instance12attach_movieERKNS_9tu_stringES1_i"),
        (uintptr_t)&hook_attach_movie);
    s_hook_clone_display_object = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN7gameswf15sprite_instance20clone_display_objectERKNS_9tu_stringEi"),
        (uintptr_t)&hook_clone_display_object);
    s_hook_remove_display_object = hook_addr(
        (uintptr_t)so_symbol(&so_mod, "_ZN7gameswf15sprite_instance21remove_display_objectEPNS_9characterE"),
        (uintptr_t)&hook_remove_display_object);
#endif
}
