# Dungeon Hunter 2 — PS Vita · v1.1.0 "Playable Framerate"

> Paste the section below into the GitHub release description. The title above goes in the
> release-title field; suggested tag: `v1.1.0`.

---

**Gameplay framerate went from 5-6 FPS to ~20-25 FPS, with menus and light scenes now hitting ~60.**

This release is almost entirely a performance pass. The surprise finding: **the GPU was never the
bottleneck.** `eglSwapBuffers` costs ~0.2 ms per frame throughout, while the CPU was spending
120-185 ms. Two separate experiments confirmed it instead of assuming it — rendering at 480x272
(a quarter of the pixels, verified correct on screen) changed combat FPS *not at all*. Every real
gain here came from CPU-side work.

## Which VPK should I install?

**`dungeon_hunter_2_hack_safe.vpk`** — this is the recommended build and the fastest one.

"Hack" refers to [vitaGL](https://github.com/Rinnegatamante/vitaGL)'s optional speedhack build
flags, not to anything sketchy. The selection rule was deliberately conservative: **only flags whose
effect is cheaper CPU code — never one that changes how the GPU is addressed or how its memory is
managed.** A visual glitch is recoverable; a GPU hang is not. `dungeon_hunter_2.vpk` is the plain
build if you want to compare.

## What actually made it faster

Roughly in order of measured impact:

1. **vitaGL safe speedhacks** (`MATH_SPEEDHACK`, `CIRCULAR_POOL_SPEEDHACK`, `NO_TEX_COMBINER`) — the
   single largest win, isolated with a controlled test.
2. **Deleted our own per-frame logging.** Three diagnostic lines were being written *every frame*,
   each doing a synchronous `fflush()` to `ux0:` — three blocking storage writes per frame, inside
   the very frame budget being optimised. They accounted for **93% of all log output**. This alone
   took 5-6 FPS to 8-9.
3. **Fixed an O(n) hot path in the pthread bridge.** Every `pthread_mutex_lock` the engine made was
   taking a global kernel mutex (two syscalls) and linearly scanning up to 1024 pointers. Why that
   mattered so much: disassembly showed *all* of the engine's `pthread_mutex_lock` call sites live
   inside STLport's allocators — so **every `std::string`, `vector` and `map` allocation** paid that
   cost, and the engine's UI layer is a GameSWF ActionScript interpreter, which allocates constantly.
   Now an O(1) lock-free hash lookup. This also explains a long-standing mystery: framerate used to
   *decay* over a single session, because the registry kept growing.
4. **`-O2` → `-O3 -ffast-math`.** Debug and Release had been compiling with identical flags.
5. **Re-gated the GL debug instrumentation**, which was running ~10 `glGet*` calls per draw call in
   Release builds.

## Also fixed

- **Crash when quitting from inside the game.** Root-caused from the crash dump to a jump through a
  null function pointer during C++ static-destructor teardown (STLport locale destructors). Exit now
  goes straight to `sceKernelExitProcess` — running the Android library's global destructors buys
  nothing when the process is already dying, and the Vita kernel reclaims memory, threads, audio and
  the GXM context by itself.
- **A real race in the pthread bridge**, pre-existing: a mutex pointer was published before the mutex
  was initialised. Destroy/unlock also no longer dereference a static-initialiser constant as if it
  were a pointer.
- **Left analog stick now moves the character**, driving the on-screen virtual joystick.
- Reduced-resolution rendering now actually works (`--downsample-test`, default 720x408, which keeps
  the Vita's exact aspect ratio). It is *not* enabled in the shipped builds, because it turned out
  not to help — but it's there and correct now if someone wants it for battery life.
- Log files are now `.log` instead of `.txt`.

## Known issues

- **Framerate is ~20-25 FPS in gameplay, not 60.** Menus and light scenes reach ~60; heavy combat
  still dips into the low teens, and loading transitions still stall. Being upfront about this: the
  dominant remaining cost is understood but not yet fixed — see below.
- **The engine's frustum culling is bypassed**, which is what makes enemies render reliably, but it
  means the engine animates and updates the *entire level* every frame regardless of the camera.
  That's the main remaining waste. Re-enabling culling naively measured *slower* and brings the
  invisible-enemy bug back, so the real fix is the stale bounding box that made the bypass necessary
  in the first place.
- **The analog stick works by driving the virtual joystick**, so it depends on that HUD element
  existing. A direct-to-engine input path was found and attempted (the engine has a full character
  command API compiled in) but did not work on hardware; the findings and next experiments are
  documented for whoever wants to try.
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

`PORTING_PLAN.md` **Phase 23** is the full write-up of this release, and it deliberately documents
what was *tried and rejected* alongside what worked, with the measurement for each — implementing
real `usleep`/`nanosleep` costs 5x the framerate; restoring update-side culling halves it; raising
the file cache to 96 MB changed nothing despite 369 MB of measured uncacheable re-reads per session.
It also retracts an earlier conclusion in the repo that had ruled out fill-rate on the basis of a
test that, it turns out, was silently running at full resolution.

If you have a physical Vita and want to help, that document lists exactly which diagnostics are
already wired up and waiting for a fresh hardware log.

## Credits

Built on [SoLoBoP](https://github.com/v-atamanenko) (Andy Nguyen, Rinnegatamante, Volodymyr
Atamanenko) and [FalsoJNI](https://github.com/v-atamanenko/FalsoJNI), rendering through
[vitaGL](https://github.com/Rinnegatamante/vitaGL). Particular thanks to the
**Asphalt-5-Vita** port, whose own performance write-up pointed directly at several of the fixes
above — it is the closest comparable port and its notes were worth more than any amount of guessing.

Dungeon Hunter 2 is © Gameloft. This is an unofficial, non-commercial fan port, not affiliated with
or endorsed by Gameloft. The loader/bridge source in this repository is MIT licensed; that covers
this project's own code only.
