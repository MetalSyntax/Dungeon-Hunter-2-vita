# Dungeon Hunter 2 — PS Vita Port

A native port of Gameloft's **Dungeon Hunter 2** (Android) to the PlayStation Vita, built on
[TheFloW's so-loader](https://github.com/TheOfficialFloW) technique: the original Android
`.so` libraries are relocated and run directly on the Vita, with a from-scratch
[FalsoJNI](https://github.com/v-atamanenko/FalsoJNI)-based bridge standing in for the parts of
the Android runtime the game calls into (rendering surface, touch input, asset loading, audio).

**This repo is loader/bridge source code only.** It does not contain, and will never contain,
Gameloft's copyrighted game assets, APK, or `.so` binaries — see [Getting the game data](#getting-the-game-data)
below for what you need to supply yourself.

## Status: playable, with known bugs — help wanted

The game boots, renders, and is playable end-to-end on real hardware (menu → character select →
dungeon combat), but it is **not polished** and has a few open issues that could use more eyes,
more test hardware, and fresh ideas:

1. **Repeating HUD icon column** — a column of duplicate badge/skill icons appears stacked under
   the character portrait. Root cause not yet found; several native and ActionScript-level
   hypotheses have been ruled out with real evidence (see `PORTING_PLAN.md` Phase 6).
2. **Low combat frame rate** — sustained ~7-15 fps in combat, target 20-30 fps. CPU/GPU clocks and
   compiler flags are already at their ceiling; the remaining bottleneck hasn't been isolated yet.
3. **Some enemies render invisible** — a health bar/nameplate and aggro ring appear, but no monster
   model is drawn. Actively being narrowed down via runtime instrumentation (see Phase 6).
4. **Intro cutscene plays audio only** — the video decode/audio path works, but the frame isn't
   visibly appearing on screen. New, not yet root-caused.

None of these block booting or playing the game — they're rough edges, not showstoppers. If you can
build for Vita, have a physical console to test on, or just want to dig into an interesting
reverse-engineering puzzle, **`PORTING_PLAN.md` is the full paper trail**: every hypothesis tried,
what was ruled out and how, and exactly what diagnostic hooks are already wired up and waiting for a
fresh hardware log. Issues and PRs are welcome.

## Building

Requirements:
- [VitaSDK](https://vitasdk.org/) (`arm-vita-eabi-gcc`/`g++`), with `vitaGL` and `kubridge`
  installed via `vdpm`.
- Sony's `libshacccg.suprx` (shader compiler), obtained separately and placed in the repo root —
  this is Sony's own binary and is not redistributed here.
- `libshacccg.suprx` and the `kubridge.skprx` taiHEN plugin both need to be installed on the target
  Vita/Vita3K under `ur0:`/`ux0:tai` before first run.

```bash
./build.sh
```

`build.sh` rsyncs the project to a space-free temp directory (VitaSDK's toolchain doesn't handle
paths with spaces well), configures with CMake, and copies the resulting `.vpk`/`eboot.bin` back into
`build/`. It will prompt for a Debug (verbose logging, `DEBUG_SOLOADER`) or Release build. See its
header comment for the experimental performance build-flag variants (`--no-vsync-test`,
`--downsample-test`).

`porting_tools/manage_vita.py` handles installing to a real Vita or Vita3K over FTP, pulling logs,
and downloading/symbolizing crash dumps — see `porting_tools/README.md`.

## Getting the game data

You need to legally own **Dungeon Hunter 2 HD** for Android to play this port. This repo does not
include the APK, its extracted contents, or any decompiled output — `.gitignore` excludes all of it
by design (see `PORTING_PLAN.md` §3). To build a playable install:

1. Obtain the APK from your own legally purchased copy.
2. Decompile it (`porting_tools/build/decompile_all.sh` wraps `jadx` + a Ghidra/angr container) if
   you need to cross-reference the original code — not required just to play.
3. Stage the game's asset files as loose files under `ux0:data/dungeon-hunter-2/` on the Vita (see
   `PORTING_PLAN.md` Phase 8 for the exact layout FalsoJNI's asset bridge expects).

## Credits

- [SoLoBoP](https://github.com/v-atamanenko) (Andy Nguyen, Rinnegatamante, Volodymyr Atamanenko) —
  the so-loader boilerplate this project is built on, MIT licensed (see `LICENSE`).
- [FalsoJNI](https://github.com/v-atamanenko/FalsoJNI) — the JNI bridge implementation.
- [vitaGL](https://github.com/Rinnegatamante/vitaGL) / [PVR_PSP2](https://github.com/GrapheneCt/PVR_PSP2) — GLES drivers for Vita.
- Dungeon Hunter 2 is © Gameloft. This project is an unofficial, non-commercial fan port and is not
  affiliated with or endorsed by Gameloft.

## License

The loader/bridge source code in this repository is MIT licensed — see `LICENSE`. This license
covers this project's own code only, not Dungeon Hunter 2 itself.
