# Technical Documentation: `source/patch.c`

This document provides a comprehensive technical breakdown of the function hooks and patches in [`source/patch.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/patch.c).

---

## 1. File Summary
`source/patch.c` performs low-level patches on internal functions within the native library `libDungeonHunter2.so`. Its main objectives are:
1. Bypass Gameloft DRM and license verification checks.
2. Intercept `Application::GetSavedOption` to force the appropriate UI HUD layout for the PS Vita's 960x544 resolution screen.
3. Serve as a diagnostic infrastructure for investigating rendering bugs, character stats, and touch events (disabled in production to prevent memory corruption).

---

## 2. Active Patches

### 2.1 Gameloft DRM License Bypass
- `ALicenseCheck_ValidateLicense` -> Redirected to `ret_void()`.
- `_ZN13ALicenseCheck14ValidateServerEb` -> Redirected to `ret_void()`.
- `_ZN13ALicenseCheck14ValidateNativeEv` -> Redirected to `ret_void()`.
- `_ZN13ALicenseCheck7LoadRMSEv` -> Redirected to `ret1()`.

### 2.2 HUDStyle Patch
- **Original Symbol:** `_ZN11Application14GetSavedOptionEPKc`
- **Hook Function:** `hook_GetSavedOption`
- **Details:** In the decompiled Android code (`out_ghidra.c:20949`), if `SavegameManager::hasOption("HUDStyle")` returns 0, the function exits without setting an explicit return value in `r0`, leaving garbage register data. This hook forces `HUDStyle = 0` (fixed 3-icon phone-style bar) to prevent the Vita from defaulting to tablet carousel modes (>= 2).

---

## 3. Disabled / Commented-out Diagnostic Hooks

Below is a detailed breakdown of the debugging hooks wrapped under `#ifdef DEBUG_SOLOADER`, explaining their original purpose and why they were disabled.

| Hook / Function | Target Symbol | Original Objective | Technical Reason for Disabling |
| :--- | :--- | :--- | :--- |
| `s_hook_modular_render` | `_ZN17BaseMeshSceneNode...6renderEPv` | Intercept player modular armor mesh rendering to audit draw calls and textures. | **ARM Memory Corruption (log_119.txt):** Injecting `kubridge` trampolines into functions executed every draw call causes ARM stack desynchronization on the Vita, corrupting heap data and altering character stats (MP/Points). |
| `s_hook_xray_render` | `_ZN31XrayModularSkinnedMeshSceneNode6renderEPv` | Track X-Ray effect rendering for modular meshes. | Same stack corruption issue under high-frequency render loop calls. |
| `s_hook_skinned_render` | `_ZN17BaseMeshSceneNode...CSkinnedMeshSceneNode...6renderEPv` | Track monster and NPC mesh rendering. | Same cause of interference within the main OpenGL render loop. |
| `s_hook_recalc_property` | `_ZN14CharProperties14RecalcPropertyEi` | Audit opacity modifiers (property 49) for invisible enemies. | Disabled after `log_099.txt` confirmed opacity was not the cause of invisibility, preventing unwanted writes to `CharProperties`. |
| `s_hook_calc_damage` | `_Z14CF__CalcDamageP9CharacterS0_iiibb` | Debug infinite damage / instakill bug. | Root cause was determined to be heap memory corruption caused by active hooks, not the damage formula itself. |
| `s_hook_get_property` | `_ZNK14CharProperties12_GetProperty...` | Detect overflowed character stat reads (> 100,000). | **Performance Degradation (log_109.txt):** Triggered over 14,000 calls per session, introducing severe log formatting stutter. |
| `s_hook_use_potion` / `s_hook_has_potion` | `_Z15NativeUsePotion...` / `_ZNK9Character9HasPotionEv` | Debug touch-based health potion button failure. | Confirmed in `log_111.txt` that touch presses reached C++, but failed due to max HP memory corruption. |
| `s_hook_attach_movie` / `clone` / `remove` | `_ZN7gameswf15sprite_instance...` | Measure memory leaks in GameSWF MovieClip lifecycle. | Disabled to eliminate overhead and prevent dereferencing unstable GameSWF engine pointers. |
