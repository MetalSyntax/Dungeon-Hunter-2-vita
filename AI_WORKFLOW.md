# AI Workflow — Dungeon Hunter 2 PS Vita Port

This is a runbook for an AI coding agent (Claude Code) working on this repository. It describes *how*
to operate session to session, complementing `PORTING_PLAN.md` (the *what*). Written in English per
project convention.

## 0. Before doing anything else

1. Confirm `.claude/skills/psvita-porting/` exists and read `SKILL.md` — it should auto-activate for
   any porting/JNI/vitasdk-flavored task, but confirm it's there before relying on it.
2. Read `PORTING_PLAN.md` §1 (static analysis findings) before touching loader code. Do not re-derive
   what's already documented there (JNI export list, which libraries to skip, which subsystems are
   license/DRM traps) — extend it if new findings emerge, don't repeat the investigation from scratch.
3. Check `git status` and running processes before assuming you have the only copy of the truth. This
   project has previously had concurrent sessions (a human running `porting_tools/manage_vita.py`
   interactively in one terminal while an agent worked in another) reorganize the same directories at
   the same time. If something looks like it changed underneath you mid-task (a file you just read is
   gone, a script has content you didn't write), stop and ask before overwriting — don't assume you're
   the only writer.

## 1. Where things live

| What | Where |
|---|---|
| Port plan, phased checklist | `PORTING_PLAN.md` |
| Porting knowledge (SoLoader, JNI, vitaGL, LiveArea, hardware debugging) | `.claude/skills/psvita-porting/` (installed copy) and `psvita-port-toolkit/skills/psvita-porting/` (source copy — keep both in sync if you edit either) |
| Generic phase-by-phase guide (not project-specific) | `psvita-port-toolkit/PORTING_GUIDE.md` |
| Deployment/debug tooling (FTP upload, log/dump pull, Vita3K automation, decompile) | `porting_tools/` (`automation/`, `build/`, `misc/`, `tests/`, `manage_vita.py`) |
| Decompiled reference material (never edit, never commit) | `decompiled/apk_jadx/`, `decompiled/<lib>_<abi>/` |
| **The actual loader project** (SoLoBoP, MIT — not a fork of the Zenonia lineage, see `PORTING_PLAN.md` Phase 2) | `CMakeLists.txt` at repo root; game-specific code in `source/main.c`, `source/dynlib.c`, `source/java.c`, `source/patch.c`; generic reimplementations in `source/reimpl/*` and `source/utils/*`; vendored libs in `lib/so_util`, `lib/falso_jni`, `lib/libc_bridge`, `lib/fios`, `lib/sha1`, `lib/kubridge`; LiveArea assets in `extras/livearea/` |
| Build script (rsync-to-`/tmp` space-in-path workaround) | `porting_tools/build/build_and_install.sh` |
| Sibling ports for cross-reference (different engines AND a different loader lineage — read for methodology, not ABI facts or file layout) | `../Prince of Persia ` (origin of the psvita-port-toolkit/FalsoJNI), `../Zenonia2-vita`, `../Zenonia3-vita` |

## 2. Standing rules for this project

- **Never commit anything derived from Gameloft's binaries.** `.gitignore` already excludes the
  original APK/zip/cache, the extracted APK tree, the installed-app data dump, and all of
  `decompiled/`. If a new kind of derived artifact shows up (a new decompiler output folder, a
  repackaged asset directory), add it to `.gitignore` before it's ever staged — don't rely on
  remembering to `git rm` it later.
- **`decompiled/` is regenerable, not source.** Treat it like a build cache: read it for analysis,
  never hand-edit it, regenerate via `porting_tools/build/decompile_all.sh` if it's stale or missing.
  One subfolder per artifact (`apk_jadx/`, `libDungeonHunter2_armeabi-v7a/`, etc.) — never let two
  different `.so` files (e.g. the two ABI copies of `libnativeinterface.so`) share one output folder;
  that was a real bug already found and fixed once in `decompile_all.sh` (folder names now include the
  ABI).
- **Don't reuse Zenonia's JNI method tables, init order, or binary-patch offsets.** Dungeon Hunter 2 is
  a different, proprietary Gameloft engine — every ABI fact must come from this project's own
  `decompiled/` output or from live testing, never copied from a sibling port's `java.c`/`main.c`.
  What *does* transfer: the SoLoader architecture, FalsoJNI, the debugging methodology, and the
  input/packaging lessons — see `PORTING_PLAN.md` §0 for the exact boundary.
- **License/DRM code paths are traps, not features.** `ALicenseCheck` (Gameloft server-side, IMEI-keyed)
  and the Samsung Zirconia stub (`libnativeinterface.so`) must be bypassed, never implemented for real.
  If a session finds itself trying to make network license validation "work" on a Vita, stop — that's
  the wrong direction; go re-read `PORTING_PLAN.md` §1's license-check section.
- **One log, one bug, one fix per iteration** when bringing the port up on Vita3K or hardware — this is
  the methodology validated across all three sibling ports' `port_progress.md` files, not
  project-specific superstition. Don't try to pre-solve a list of anticipated bugs; let the log drive
  the next fix.
- **Verify on the actual artifact before generalizing.** If a Ghidra pseudo-C read or a `strings`/`nm`
  grep would settle a question about how this specific engine behaves, do that before assuming
  cocos2d-x or Gamevil Nexus2 conventions apply — this engine has already surprised the static analysis
  once (`libStormGLOFT.so` looked like a renderer by name, turned out to be an ARM hooking/anti-tamper
  library).

## 3. Typical session flow

1. **Orient**: read `PORTING_PLAN.md`'s checklist (§6) to see what phase is next.
2. **Investigate before coding**: for the current phase, check whether `PORTING_PLAN.md` §1 or
   `decompiled/` already answers the open question. If not, extend the analysis (grep symbols, read the
   relevant pseudo-C or jadx-decompiled Java class) and record the finding in `PORTING_PLAN.md` before
   writing loader code against it.
3. **Implement** against `psvita-port-toolkit`'s generic guidance, adapted to this engine's real ABI.
4. **Build and deploy** via `porting_tools/build/build_and_install.sh` or, for fast iteration,
   `porting_tools/build/deploy_and_launch_vita3k.sh`. Use `porting_tools/manage_vita.py` for FTP
   upload/log/dump retrieval against real hardware.
5. **Triage from the log**, one issue at a time, cross-referencing `references/hardware_debugging.md`
   and the sibling ports' `port_progress.md` bug classes before treating a symptom as novel.
6. **Update `PORTING_PLAN.md`'s checklist and any relevant phase section** with what was learned —
   future sessions (yours or a human's) should not have to re-discover it.

## 4. Escalate to the user instead of guessing when

- A decompiled artifact is ambiguous and the two plausible readings would lead to meaningfully
  different implementations (e.g. unclear argument packing in `nativeOnTouch`, unclear whether
  `libStormGLOFT.so` is safe to skip entirely on real hardware, not just in theory).
- Concurrent modifications are detected (see §0.3) — don't silently pick a side.
- Anything would touch real hardware in a way that isn't easily undone (flashing firmware-adjacent
  settings, deleting save data on a physical console, force-pushing shared branches).
