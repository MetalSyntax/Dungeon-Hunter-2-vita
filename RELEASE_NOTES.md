# Dungeon Hunter 2 — PS Vita · v1.1.0 "Physical Controls & Persistence"

> Paste the section below into the GitHub release description. The title above goes in the
> release-title field; suggested tag: `v1.1.0`.

---

**All physical buttons now work — movement, attacks, skills, potion, pause and character menu —
and saves/options finally persist across reboots.**

## Physical controls, finished

Every action button now works, including the three that never did: **Circle** (skill 3),
**START** (pause / in-game menu) and **SELECT** (character screen). They were dead on every prior
build even though the previous attempt's synthetic touch events provably reached the engine.

The real cause: the touch layer (`TouchScreenBase`) indexes its slot array as `this + 48 *
pointer_id` **with no bounds check**, and its `clear()` loops exactly 8 times — so only pointer
ids `0..7` are valid. Circle/START/SELECT were assigned ids 7/8/9; the last two wrote past the
array and clobbered the engine's own touch counter, which is why the tap "arrived" in the log but
never did anything. Coordinates were never the problem — the previous synthetic-touch approach had
a structural ceiling.

Fixed by dropping synthetic touch for these three entirely and calling the engine directly instead,
mirroring exactly what its own Flash HUD buttons do (reverse-engineered from the HUD's own
ActionScript bytecode):

- **Circle** → the same `CTRLIsAllowed` → `SG_GetSkillInSlot` → `Cmd_BeginSkill`/`Cmd_EndSkill`
  sequence the skill-3 HUD button makes.
- **START / SELECT** → `MenuBase::FS_PushState("menu_Ingame" / "menu_CharacterMenu")`, and if the
  menu is already open, the same button closes it (`MenuFX::PopAll`) — one press to open, one to
  close.
- **L + R** → new combo, toggles the bottom HUD controls' opacity between 1% (default — play with
  physical buttons, HUD stays out of the way) and 100% (to use them with touch again).

The left stick and D-pad also got a real fix this release: instead of faking a touch drag on the
Flash virtual joystick (which GameSWF's hit-test never reliably accepted), the loader now writes
the engine's own `HUDControls` movement fields directly every frame, the same fields its own touch
handler fills in when a real finger drags the stick. Confirmed on hardware: both D-pad and analog
stick move the character correctly.

## Saves and options now persist (verified on hardware)

Progress, settings and language previously died in RAM: the game played fine in-session but
rebooted to an old save, always in English. Three separate causes, all fixed and confirmed with a
save → reboot → load cycle on a real Vita (Spanish kept, character level and items intact):

- **The port forced English on every boot.** The loader rewrote the saved language to English for
  the first 180 frames of each launch, so changing language could never stick. Now the saved
  language is read once and only reset if out of range.
- **The async save queue was never guaranteed to reach disk.** Player/level saves are queued in
  RAM (`Savegame::saveAll → AddJob`) and drained by fire-and-forget worker threads — and quitting
  with START+SELECT skipped `Application::Quit`, the only synchronous drain, entirely. The queue is
  now also drained on a timer when jobs are pending and unconditionally at exit.
- **Stale read cache.** The file cache never invalidated on write/delete, so a freshly written save
  could still load pre-save bytes in-session. Writes, deletes and renames now invalidate.

## Also fixed

- **Crash on quitting while a sound was playing.** `Application::Quit` stops every sound with a
  0ms fade, which frees its audio objects synchronously on the main thread — racing the engine's
  own audio thread, which was iterating that same list at the same moment. Confirmed from a crash
  dump (prefetch abort through a freed vtable). Level-transition sound stops already used a 500ms
  fade and never crashed, so quit-time stops now take the same safe path; nothing audible changes,
  and level transitions are untouched.
- **A long-standing source of multi-second freezes: every localization file was silently failing
  to load.** Every `text/<zone>.spanish`/`.symbols` lookup this whole project was hitting a
  relative path that resolves under `assets/` on the real device, which holds nothing but the save
  file — the actual files live under `data/text/`, and unlike other asset types, this one had no
  retry-on-failure path anywhere in the engine. A burst of ~12-18 of these misses in the same frame
  window was directly responsible for multi-hundred-millisecond stalls (one measured at 834ms
  average over 60 frames). Now redirected to the correct path on first failure, same pattern
  already used for a couple of other asset types.
- **vitaGL's CPU-only speedhacks are now baked into every build by default** (`MATH_SPEEDHACK`,
  `CIRCULAR_POOL_SPEEDHACK`, `NO_TEX_COMBINER`) — previously only in the opt-in `--hack-safe`
  variant. Confirmed via `nm -D` that this engine imports zero fixed-function GL entry points (100%
  GLSL), so the two math-related flags are free no-ops here and the real, measured win comes from
  `CIRCULAR_POOL_SPEEDHACK`'s vertex-data pooling. The plain `dungeon_hunter_2.vpk` build now
  performs the same as the old `_hack_safe` variant; `--hack-safe` is kept only for compatibility.
- Log files are now `.log` instead of `.txt`.

## Performance: 5-6 FPS → ~20-25 FPS in gameplay, ~60 in menus

This is unchanged since the last notes and remains the honest headline: sustained combat is
~20-25 FPS (up from 5-6), menus and light scenes reach ~60. **The GPU was never the bottleneck** —
`eglSwapBuffers` costs ~0.2ms per frame throughout, while CPU-side work (the engine tick) was
120-185ms. Two independent resolution-reduction tests changed combat FPS *not at all*, ruling out
fill-rate directly rather than assuming it. See `PORTING_PLAN.md` Phase 23 for the full write-up.

## Known issues

- **Framerate is ~20-25 FPS in gameplay, not 60.** The dominant remaining cost is understood but
  not fixed: the engine's real frustum culling has to stay bypassed (see below), so it animates and
  updates the *entire level* every frame regardless of the camera.
- **The engine's frustum culling is bypassed.** This is what makes enemies render reliably, but
  it's also the main remaining performance waste. Naively re-enabling it (`--culling-test 2`)
  measured *slower* and brings back the invisible-enemy bug, so the real fix is the stale bounding
  box (or a second, object-level visibility gate — see `PORTING_PLAN.md`) that made the bypass
  necessary in the first place.
- **Some enemies still render invisible** (health bar and aggro ring show, no model). The opacity
  hypothesis has been ruled out with hard evidence (52/52 sampled enemies at full opacity); the
  current leads are all culling-related.
- The repeating HUD icon column is still unresolved.

## Installing

You need to **legally own Dungeon Hunter 2 HD for Android.** This repository and this release contain
**no** Gameloft assets, APK, or `.so` binaries — only the loader/bridge. You supply the game data
yourself.

1. Install [taiHEN](https://github.com/yifanlu/taiHEN)'s `kubridge.skprx`, and place Sony's
   `libshacccg.suprx` under `ur0:data/` (obtained separately — it is Sony's binary and is not
   redistributed here).
2. Install the `.vpk`.
3. Stage the game's asset files as loose files under `ux0:data/dungeon-hunter-2/`. See
   `PORTING_PLAN.md` Phase 8 for the exact layout the asset bridge expects.

## For contributors

`PORTING_PLAN.md` documents every hypothesis tried for the open issues above, what was ruled out
and how, and exactly which diagnostic hooks are already wired up and waiting for a fresh hardware
log. If you have a physical Vita and want to help, that's the place to start.

## Credits

Built on [SoLoBoP](https://github.com/v-atamanenko) (Andy Nguyen, Rinnegatamante, Volodymyr
Atamanenko) and [FalsoJNI](https://github.com/v-atamanenko/FalsoJNI), rendering through
[vitaGL](https://github.com/Rinnegatamante/vitaGL) built from source. Particular thanks to the
**Asphalt-5-Vita** and **Asphalt-6-Vita** ports, whose own performance write-ups pointed directly
at several of the fixes in this and the previous release — they are the closest comparable ports
and their notes were worth more than any amount of guessing.

Dungeon Hunter 2 is © Gameloft. This is an unofficial, non-commercial fan port, not affiliated with
or endorsed by Gameloft. The loader/bridge source in this repository is MIT licensed; that covers
this project's own code only.
