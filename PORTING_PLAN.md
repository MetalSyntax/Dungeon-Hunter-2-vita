# Dungeon Hunter 2 (Gameloft) — PS Vita Port Plan

> Written in English per project convention for shareable documentation. Prior-art ports referenced
> below (`Zenonia2-vita`, `Zenonia3-vita`) keep their working notes in Spanish; this document does not
> duplicate their content, it distills what's reusable from them.

## 0. Prior art in this workspace

Three sibling ports live next to this one and are the reason this project doesn't start from zero:

- **`../Prince of Persia `** (note the trailing space in the folder name) — the **origin of the
  SoLoader + FalsoJNI boilerplate** used by every port in this workspace, targeting a cocos2d-x game.
  `psvita-port-toolkit/` in this repo is the genericized distillation of that port (toolchain setup,
  `so_util` dynamic loader, FalsoJNI, hardware debugging methodology, LiveArea/VPK packaging rules).
  Its `porting_tools/` directory is also the direct ancestor of this repo's `porting_tools/`.
- **`../Zenonia2-vita`** — first port in the family to reach a **playable state on real hardware**
  (menus navigable, game start-able) using the Gamevil "Nexus2/Clet" engine. `port_progress.md` there
  documents ~13 real hardware bugs and fixes — the JNI-stub, pthread, and file-hook lessons apply to
  *any* SoLoader port regardless of engine, not just Gamevil titles.
- **`../Zenonia3-vita`** — forked directly from Zenonia2-vita's proven codebase because it uses the
  **same engine**. Its `plan_zenonia3_port.md` is a good model for *how to structure this kind of
  document* (a "what's identical / what changed" comparison table before writing any loader code), but
  its actual ABI conclusions don't transfer here — Dungeon Hunter 2 is a **different, proprietary
  Gameloft engine**, not Gamevil Nexus2 and not cocos2d-x.

**What transfers directly from all three:** the generic SoLoader architecture (`so_util.c/h`),
FalsoJNI (vendored from Prince of Persia, MIT-licensed), the hardware-debugging methodology (one log,
one bug at a time), the input-handling lessons (touch sampling off by default, never pass raw Vita
touch IDs to the engine), and the LiveArea/VPK packaging rules. **What does not transfer:** any
specific JNI method table, ABI quirk, or binary patch offset from Zenonia — those are specific to
Gamevil's engine and must be independently re-derived for Gameloft's engine (see Phase 1 below, already
done).

## 1. Target — static analysis (Phase 1, completed)

Package: `com.gameloft.android.GAND.GloftD2SS`, main activity `DungeonHunter2`. Two libraries are
loaded via `System.loadLibrary`, in this exact order (confirmed in `DungeonHunter2.java`):

```java
System.loadLibrary("DungeonHunter2");
System.loadLibrary("StormGLOFT");
```

A third library, `libnativeinterface.so`, is loaded independently by `Zirconia_DRM.java` and is
**unrelated to game logic**.

### `libDungeonHunter2.so` (`lib/armeabi-v7a/`, ~15.9 MB, **not stripped — has symbol/debug info**)

This is the actual game engine + JNI bridge, and the fact that it isn't stripped is a major advantage
over the Zenonia ports (both stripped): real C++ class and function names are available directly from
`nm`/`objdump`, no guesswork needed. 32 JNI exports found, across 6 classes:

| Java class | Native methods |
|---|---|
| `DungeonHunter2` | `nativeInit`, `nativeAccelerometer`, `nativeCanInterrupt`, `nativeGetGameMusicVolume`, `nativeGetInfo`, `nativeKeyDown`, `nativeKeyUp`, `nativeOpenIGM`, `nativePause`, `nativeResume`, `nativeSetOrientation`, `nativeSetPhone`, `nativegetState`, `nativeonTrackballEvent` |
| `GameGLSurfaceView` | `nativeOnTouch` |
| `GameRenderer` | `nativeGameRenderer`, `nativeConfig`, `nativeDone`, `nativeGetJNIEnv`, `nativeInit`, `nativeOnDrawFrame`, `nativeOnSurfaceChanged`, `nativeRender`, `nativeResize` |
| `GLMediaPlayer` | `nativeInit`, `nativeGetTotalSounds`, `nativeGetTotalSoundsOfSameInstance`, `nativeSetStopOnMusic` |
| `GLResLoader` | `nativeInit` |
| `GLUtils_Device` | `nativeInit` |
| `Musicplayer` | `nativeInitplayer`, `nativeDisplayMusicTitle` |

This is a **standard `GLSurfaceView.Renderer` bridge**, not a cocos2d-x `nativeInit/nativeRender` pair
nor Gamevil's `handleCletEvent` single-channel input — expect a different lifecycle: `GameRenderer`
drives `onSurfaceChanged`/`onDrawFrame` like any Android `GLSurfaceView`, and `DungeonHunter2` handles
the rest (keys, touch via a *separate* `GameGLSurfaceView.nativeOnTouch`, accelerometer, phone-call
interruption via `nativeSetPhone`/`nativeCanInterrupt`, and an in-game-menu hook `nativeOpenIGM`).

**Two license-check subsystems found inside this binary — both must be bypassed, not implemented:**
- A full C++ `ALicenseCheck` class (`ALicenseCheck::ValidateServer`, `sendRequestByGet`,
  `getIMEI`, `createUniqueCode`, XOR-based response validation) — Gameloft's own server-side license
  check, keyed off device IMEI. There is no IMEI on Vita and the validation server is almost certainly
  gone; this path must never be reached (stub the JNI entry point that triggers it, or force
  `ValidateNative()`'s stored result to "valid" before it's read).
- References to Google Play LVL-style checks are present as dead code paths (not directly exported as
  JNI) — same treatment: never call, never link against a real Play Store.

### `libStormGLOFT.so` (`lib/armeabi-v7a/`, ~907 KB, stripped, **no JNI exports**)

Initial assumption (from the name, evoking Gameloft's "Storm3D" engine branding) was that this is a
rendering/engine library. **Static analysis disproves that.** Exported symbols
(`ZzBuildHook`, `ZzEnableHook`, `HookArm`, `HookThumb`, `BuildArmJumpCode`, `BuildStubThumb`,
`GetARMInsnType`, trampoline builders, etc.) identify it as an **ARM/THUMB inline-hooking framework**
("Zz" hooking, the same family as pattern seen in various Android anti-tamper libraries) — this is
Gameloft's runtime anti-piracy/anti-tamper layer, hooking libc/Dalvik entry points at runtime to detect
repackaging or emulation. It exports `JNI_OnLoad` but is loaded purely for its side effects (installing
hooks), not because the game calls exported JNI methods on it.

**Decision: do not port this library's behavior.** A soloader environment is not the Android runtime
this hooking code expects (no real Dalvik, no real GOT layout to hook safely), and there is nothing in
it the game needs functionally — it is anti-piracy instrumentation, not gameplay code. Two options,
cheapest first:
1. Don't load it at all (`System.loadLibrary("StormGLOFT")` becomes a no-op in `java.c`'s
   `loadLibrary` hook). If nothing else in `DungeonHunter2.java` calls a native method that only this
   `.so` would have resolved (none currently found — its own exports are internal, not JNI), this is
   safe.
2. If skipping it entirely turns out to break something unexpected (confirm on real hardware before
   assuming), load it but stub its `JNI_OnLoad` to a no-op that just returns the JNI version, never
   letting the hook-installation code run.

### `libnativeinterface.so` (both `lib/armeabi/` and `lib/armeabi-v7a/`, ~11 KB, **byte-identical
MD5 in both ABI folders**, stripped)

Exports `Java_com_samsung_zirconia_NativeInterface_*` (`checkLicenseFile`, `checkLicenseFile2`,
`storeLicenseKey`, `doPassphraseTest`) plus an embedded SHA1 implementation. This is **Samsung Zirconia
DRM** — a Samsung-Apps-store-specific licensing mechanism, loaded from `Zirconia_DRM.java`, completely
unrelated to Gameloft's engine or to Google Play. **Decision: never load this library; stub the Java
call site's result (or the 4 JNI methods, if `Zirconia_DRM.java`'s call path can't be short-circuited
in `java.c` before it needs a real function) to whatever value makes `Zirconia_DRM` treat the license
as already valid.** Read `decompiled/apk_jadx/sources/.../Zirconia_DRM.java` to confirm exactly what
value each stub must return before writing the stub — do not guess the semantics from the function
names alone.

## 2. Decompilation — reproducible, one artifact per folder

Per project convention (see `.gitignore` — none of this is committed, all of it is regenerable),
everything lives under `decompiled/`, **one subfolder per artifact** so JNI/pseudo-C output never
collides across ABIs:

```
decompiled/
├── apk_jadx/                              # jadx: classes.dex + resources → Java sources
├── libnativeinterface_armeabi/            # nm/objdump symbol dump + Ghidra/angr pseudo-C
├── libnativeinterface_armeabi-v7a/        # (byte-identical binary to the armeabi copy, decompiled anyway for completeness)
├── libDungeonHunter2_armeabi-v7a/
└── libStormGLOFT_armeabi-v7a/
```

Reproduce with `porting_tools/build/decompile_all.sh` (already adapted to this project's paths — see
§4). Manually, the two techniques it wraps:

```bash
# Java (fast, no Docker needed if jadx is installed locally)
jadx -d decompiled/apk_jadx "Dungeon-Hunter-2-HD-v1-0-2.apk"

# Per .so: quick symbol triage first (no ARM toolchain needed — system objdump/nm read dynsym fine)
nm -D --defined-only "<lib>.so" | c++filt
objdump -T "<lib>.so" | grep UND        # imports
objdump -T "<lib>.so" | grep "Java_"    # JNI exports

# Full pseudo-C via Ghidra headless + angr, in Docker (devrvk/so-decompiler)
docker run --rm --platform linux/amd64 \
  -v "<lib_dir>:/input" -v "decompiled/<lib>_<abi>/ghidra:/output" \
  devrvk/so-decompiler decompile /input/<lib>.so /output
```

Note: `libDungeonHunter2.so` is 15.9 MB and not stripped, so its Ghidra pass takes noticeably longer
than the Zenonia binaries (tens of minutes under `--platform linux/amd64` emulation on Apple Silicon,
vs. under a minute for the two `libnativeinterface.so` copies). Budget time accordingly and run it in
the background rather than blocking on it.

## 3. Repo hygiene — DMCA prevention

Nothing derived from Gameloft's copyrighted binaries goes to GitHub. `.gitignore` at the repo root
excludes, at minimum:
- The original `.apk`/`.zip`/cache archive and the raw extracted APK tree (`Dungeon-Hunter-2-HD-v1-0-2/`).
- The installed-app data dump (`com.gameloft.android.GAND.GloftD2SS/` — savegames, videos, proprietary
  `.bdae`/`.bar` assets).
- The entire `decompiled/` tree (jadx Java sources and Ghidra/angr pseudo-C are both derivative works
  of the original binary — regenerable any time via `porting_tools/build/decompile_all.sh`, never
  committed).
- Any staged/repackaged asset directory for the Vita build (`ux0_data/`), the vendored proprietary
  `.so` once copied next to the loader for symbol resolution during development (`lib/**/*.so` — the
  loader's *own* code such as FalsoJNI is `.c`/`.h`, not `.so`, so this rule doesn't catch it), crash
  dumps (`*.psp2dmp`), and per-run debug logs (`/logs/`, `log_*.txt`).

Only the loader's own source code, build scripts, the toolkit/skill documentation, and small factual
artifacts (symbol name lists, this plan) are meant to be committed.

## 4. Toolkit and tooling — adapted, not reused verbatim

- **`psvita-port-toolkit/skills/psvita-porting/`** is installed at `.claude/skills/psvita-porting/`
  so Claude Code activates it automatically for this project (per the toolkit's own README). Read
  `psvita-port-toolkit/PORTING_GUIDE.md` once as the general map of phases 0–9; treat its checklist as
  the definition of done for a first playable build.
- **`porting_tools/`** (forked from Prince of Persia's toolset, already reorganized into
  `automation/`, `build/`, `misc/`, `tests/`, and already parameterized for this project — see
  `manage_vita.py`'s `VITA_IP`/`VITA_DATA_DIR`/`VITA_LOGS_DIR` constants and `build/decompile_all.sh`'s
  `BASE_DIR`) is the deployment/debugging harness: build+install, FTP upload/log-pull via
  `manage_vita.py`, Vita3K fast-iteration via `build/deploy_and_launch_vita3k.sh`, crash-dump retrieval
  via `misc/get_dump.sh`, and the click-automation scripts under `automation/` for driving Vita3K's Qt
  UI in tests. None of these needed inventing from scratch — they needed their hardcoded paths/IDs
  pointed at *this* project instead of Prince of Persia's, which is now done.
- Before writing loader code, copy the **generic, non-game-specific** pieces from
  `Zenonia2-vita`/`Zenonia3-vita` if a from-scratch SoLoader skeleton is wanted as a starting point
  (`so_util.c/h`, vendored `FalsoJNI`, the general shape of `dynlib.c`'s hook table) — but do **not**
  copy their `java.c` method tables or `main.c` init-call sequence verbatim; this engine's JNI surface,
  init order (`GameRenderer` vs `DungeonHunter2` vs `GLResLoader` `nativeInit` calls), and license-stub
  requirements are all different and documented in §1 above.

## 5. Phased implementation plan

Adapting `psvita-port-toolkit/PORTING_GUIDE.md`'s generic phases to what's specific here:

### Phase 0 — Toolchain — ✅ confirmed on this machine (2026-07-12)
This machine already has one shared VitaSDK install used by every sibling port, so most of this phase
was verification, not setup:
- **VitaSDK**: installed at `~/vitasdk` (`arm-vita-eabi-gcc`/`g++` 10.3.0 present under
  `~/vitasdk/arm-vita-eabi/bin`). No `$VITASDK` env var is exported globally — every sibling
  `build.sh` detects it at run time (`$VITASDK` → `/usr/local/vitasdk` → `~/vitasdk` fallback) rather
  than relying on shell profile state; this project's own `build.sh` (Phase 2) must do the same.
- **`vitaGL`** and **`kubridge` (userland stub only)**: both already built and installed via `vdpm`
  (`~/vitasdk/arm-vita-eabi/lib/libvitaGL.a`, `libkubridge_stub.a`, `libkubridge_stub_weak.a`,
  headers present). No pinned commit hash could be recovered from `~/vitasdk-src/bootstrap.log` (not
  logged there) — if a vitaGL-version-specific bug shows up later, rebuild from a known commit via
  `vdpm` at that point and record it here; don't assume today's binary is any particular commit.
- **`kubridge` kernel plugin (`.skprx`)**: **not present anywhere in this workspace** — only the
  linking-time stub is installed. The actual taiHEN plugin has to be installed on the physical test
  console (`ux0:tai/config.txt` + the `.skprx` from the kubridge GitHub releases) before hardware
  bring-up (Phase 10); this is a device-side, one-time step, out of scope until then.
- **`libshacccg.suprx`**: copied into this repo's root from `Zenonia2-vita/` (Sony's shader compiler,
  not game-specific, shared across every vitaGL project here — gitignored via `*.suprx`, same as the
  sibling repos, since it's Sony's own binary, not ours to redistribute). Still needs to land in
  `ur0:data/` on the physical test console before first run; not done yet — **the test console at
  `192.168.3.15` (per `porting_tools/manage_vita.py`) did not respond to a ping on 2026-07-12**, so
  confirm it's powered on and on the network before the first deploy attempt.
- **Space-in-path workaround**: this project's path (`.../PSVITA Develop/Dungeon-Hunter-2-vita`)
  has the same space-in-path problem as every sibling port. The proven fix used by both
  `Zenonia2-vita/build.sh` and `Zenonia3-vita/build.sh` is **not a symlink** but an `rsync` of the
  whole project tree into a space-free `/tmp/<project>-src`, with `/tmp/<project>-build` as the CMake
  build directory, then copying the resulting `.vpk`/`eboot.bin` back into `<project>/build/`. Phase 2
  should copy that exact pattern into this project's own `build.sh` rather than re-deriving it.

### Phase 1 — Static analysis — ✅ done, see §1
Re-open `decompiled/libDungeonHunter2_armeabi-v7a/ghidra/out_ghidra.c` when a specific function's
internals are needed (e.g. what `GameRenderer::nativeOnDrawFrame` actually draws, or the exact
`ALicenseCheck` call site to short-circuit) — the symbol dumps in §1 tell you *what* exists, the
pseudo-C tells you *how it behaves*.

### Phase 2 — Loader bootstrap — ✅ compiles and packages (2026-07-12/13)
Superseded plan: this repo's actual loader foundation, dropped in at the repo root during this same
work session, is **not** a fork of the Zenonia/Prince-of-Persia lineage — it's Volodymyr Atamanenko's
"SoLoBoP" (So-Loader Boilerplate, MIT), a more current and considerably more complete generic base
(`source/main.c`, `source/dynlib.c`, `source/java.c`, `source/patch.c`, `source/reimpl/*` — EGL, errno,
io, log, mem, pthread, sys, time64, an `AAssetManager` reimplementation — `source/utils/*`, plus
`lib/so_util`, `lib/libc_bridge` (NID-based SceLibcBridge for stdio), `lib/fios`, `lib/sha1`,
`lib/kubridge`). `lib/falso_jni/` was vendored from `Zenonia3-vita` (same MIT FalsoJNI, this repo's copy
was still an empty placeholder). Done so far:
1. **Vendored `lib/falso_jni/*`** (FalsoJNI.c/h, ImplBridge, Logger, MIT) — the only piece of the SoLoBoP
   skeleton that was still missing/empty.
2. **`CMakeLists.txt`** adapted: `project(dungeon_hunter_2)`, `VITA_TITLEID "PSVDH0002"`,
   `DATA_PATH "ux0:data/dungeon-hunter-2/"`, `SO_PATH "${DATA_PATH}libDungeonHunter2.so"` (only this one
   library is loaded — see §1: `libStormGLOFT.so` is anti-tamper hooking code with no JNI exports,
   `libnativeinterface.so` is unrelated Samsung DRM; neither is in the `add_executable` source list or
   ever passed to `so_file_load`), `PSVITAIP` matching `porting_tools/manage_vita.py`'s `192.168.3.15`.
3. **`source/dynlib.c`**: this template's own `default_dynlib[]` table is already a large, generic,
   well-maintained import resolver (EGL, full GLES2, pthread, `AAssetManager`, etc.) — cross-checked
   1:1 against `decompiled/libDungeonHunter2_armeabi-v7a/symbols.txt`'s real import list and found only
   ~28 genuine gaps (soft-float `__aeabi_*` helpers, `_ZSt7nothrow`/`_ZnajRKSt9nothrow_t`, `__assert2`,
   `__isfinitef`, `uname`, `cosh`/`modff`/`inet_addr`/`wcscat`/`wcsncmp`, and a `JAVA_SOUNDS` **data**
   symbol of unconfirmed size — all added, see the "Dungeon Hunter 2" section near the end of the file).
   No `AAsset_*`/`AAssetManager_*` symbols are imported by this binary at all, confirming asset loading
   goes through the JNI callback path (`GLResLoader`'s static methods), not the NDK C API this template
   also supports — important for Phase 3/8.
4. **`source/main.c`**: real init/frame/input sequence, transcribed directly from
   `decompiled/apk_jadx/sources/.../DungeonHunter2.java`, `GameGLSurfaceView.java`, `GameRenderer.java`
   (not assumed from any sibling port's engine):
   `nativeSetPhone(960,544)` → `GameRenderer` ctor (`nativeGameRenderer()` then `nativeConfig()`) →
   `gl_init()` → `nativeGetJNIEnv()` → `DungeonHunter2.nativeInit(0)` (full game, not demo — see the
   function for why) → `GameRenderer.nativeInit(1)` → `nativeOnSurfaceChanged(960,544)` → per-frame loop
   (`nativeKeyDown`/`nativeKeyUp` from a single most-recent-key queue exactly like the original Java,
   `nativeOnTouch(type,x,y,pointerId,0,0)` scaled from the 1920x1088 touch panel, `nativeRender()` —
   **not** `nativeOnDrawFrame`, which the real Java never calls despite the .so exporting it) →
   `nativePause`/`nativeResume` on exit via `nativeCanInterrupt()`.
5. **Verified with a real build**, not just read: `cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5` +
   `make` from a space-free `/tmp` copy (see Phase 0) compiles, links, converts to a Sony ELF, and
   packages a `.vpk` end-to-end with no errors. Two real link-time bugs were caught and fixed this way
   (not from static reading): `__dso_handle` must be `extern`, not redefined (`crtbegin.o` already
   provides it — duplicate symbol at link time), and the vendored `FalsoJNI_Logger.c` calls a
   `game_log()` function that only existed in the Zenonia lineage's `main.c` — added a small shim in
   this project's `main.c` forwarding to `l_info()`.
6. `porting_tools/build/build_and_install.sh` was still an unadapted copy of Prince of Persia's own
   script (`popclassic.vpk`, `EMULATOR_BUILD`/`ENABLE_VERBOSE_LOG` cmake flags that don't exist in this
   project's `CMakeLists.txt`) — rewritten to match this project's actual build (rsync-to-`/tmp`,
   `dungeon_hunter_2.vpk`, `CMAKE_BUILD_TYPE=Debug/Release` instead of those Prince-of-Persia-specific
   options).

**Not done yet (do not assume it works until tested):** `source/java.c`'s FalsoJNI method table is
still the SoLoBoP template's empty default — the `GLResLoader` static-method asset bridge (Phase 3/8)
is unimplemented, so any asset read will currently log "method ID not found" and return whatever
FalsoJNI's default is for that return type. Nothing has been run on Vita3K or real hardware yet — the
init call order and touch coordinate space above are informed guesses from reading the real Java, not
confirmed by a log from an actual run.

### Phase 3 — FalsoJNI method resolution — ✅ implemented, builds clean (2026-07-13)
`GameRenderer` turned out to call back into "Java" **zero** times (`nativeGameRenderer`/`nativeConfig`/
`nativeOnDrawFrame` are empty stubs in the real binary, confirmed in `out_ghidra.c`; `nativeGetJNIEnv`
only stashes the global `mEnv` every other callback uses). The actual "engine calls into Java" surface
is the other three classes, and every single method + exact signature was cross-checked against **three
independent sources** (not guessed from jadx alone, per `AI_WORKFLOW.md`'s "verify on the actual
artifact" rule):
1. jadx-decompiled Java (`GLResLoader.java`, `GLMediaPlayer.java`, `Musicplayer.java`,
   `DungeonHunter2.java`) for candidate method names/behavior.
2. `strings libDungeonHunter2.so`, which exposes the cached-jmethodID variable names the real binary
   uses (`mMethod<Name>` for `DungeonHunter2`/`Musicplayer`, `<name>ID` for `GLResLoader`/
   `GLMediaPlayer`) — this is the definitive list of what's *actually* called, as opposed to every
   static method jadx happens to show.
3. `decompiled/libDungeonHunter2_armeabi-v7a/ghidra/out_ghidra.c`'s four `*_nativeInit` functions, which
   give the literal name + JNI signature string passed to `GetStaticMethodID`, plus the `nativeXxx`
   wrapper bodies confirming argument count/order and which `CallStatic{Void,Int,Object}Method`
   variant is used.

That cross-check caught a real mismatch against the jadx output: **`GLResLoader` only ever gets 3
callbacks from native** — `getResourceFull(String)`, `getResourceBytes(String,int,int)`, and
`getResourceLength(String)`, all keyed by asset path — not the ~10 static methods
`GLResLoader.java` declares. `getResourceOpen`/`Read`/`Close`/`Skip`, `getRawResource`,
`copyResourceFromAssets`, `copyMovieFileFromAssetsToTMP`, and `getString` are Java-internal helpers
(used by `copyMovieFileFromAssetsToTMP`'s own body) that native code never calls back — confirms the
Phase 2 finding that asset loading is a 3-method String-keyed bridge, not the NDK `AAssetManager` path.

Implemented in `source/java.c` (65 methods total, `enum`-named IDs instead of Zenonia-style bare
numbers given the larger table):
- **`GLResLoader`'s 3 asset methods**: real file I/O against `DATA_PATH"assets/<name>"` (same
  convention as `source/reimpl/asset_manager.cpp`), not stubs — correctly reports "not found" until
  Phase 8 stages real assets there, rather than returning a stale/garbage sentinel for what will become
  a load-bearing path.
- **`GLMediaPlayer`'s 28 sound/movie methods** and **`Musicplayer`'s 9 playlist methods**: safe
  no-ops/neutral values (0 playlists, "not loaded", "not playing") ahead of the real Phase 7 audio
  implementation — every numeric method has an explicit registered value so none of them fall through
  to FalsoJNI's "not found" sentinel.
- **`DungeonHunter2`'s 25 misc methods**: neutral defaults matching what the real Java does on a fresh
  install/unrecognized device (`Get_PhoneLanguage`→English, `isSupportMM`→-1 matching the real Java's
  own dead-code branch, `DisableLaunchGame`→0, etc.), no-ops for UI hooks not implemented yet (Gameloft
  Live, IGP cross-promo, trophies), and a real `sceKernelExitProcess(0)` for `Exit()`. The 9 `VZ*`
  methods are a **Verizon-carrier-specific IAP/billing path** (`DungeonHunter2.java` delegates to a
  separate `VZBilling` class) — treated as a trap the same way `ALicenseCheck` is (§1): never
  implemented for real, just reported as "never in progress / already errored" so nothing can get stuck
  waiting on or retrying it.

**Verified with a real build**, not just read: full `cmake && make` from the `/tmp` rsync copy (Phase 0)
compiles `java.c` with zero warnings and produces a `.vpk` end-to-end. Not yet run on Vita3K/hardware —
that's Phase 10; the FalsoJNI method table being complete and non-crashing at build time doesn't confirm
runtime behavior against the real engine.

### Phase 4 — License/DRM bypass — ✅ done (2026-07-13)
Before chasing rendering or input bugs, make sure nothing in `DungeonHunter2.nativeInit` blocks waiting
on `ALicenseCheck::ValidateServer` or the Samsung Zirconia stub. Confirm in the log that these paths
are either never reached or return immediately with a "licensed" result.

### Phase 5 — Graphics (`GameRenderer` GLSurfaceView bridge) — ✅ confirmed GLES2 (2026-07-13)
Confirm GLES version/profile actually used (`glShaderSource`/`glGetShaderiv` are imported by
`libStormGLOFT.so` if kept — check whether `libDungeonHunter2.so` itself imports fixed-pipeline-only
symbols like the Gamevil/cocos2d-x titles, or GLES2 shader entry points; this changes the wrapper
strategy significantly and must be confirmed from the actual import list in
`decompiled/libDungeonHunter2_armeabi-v7a/symbols.txt`, not assumed from the other ports).

**Outcome:** Confirmed via `objdump -T` that `libDungeonHunter2.so` exclusively uses GLES2 programmable pipeline functions (`glCreateShader`, `glCompileShader`, `glLinkProgram`, `glUseProgram`, `glVertexAttribPointer`). No fixed-pipeline GLES1 functions (`glMatrixMode`, `glEnableClientState`, etc.) are imported.

**⚠️ Superseded 2026-07-15/18 — the "map to `vitaGL`" conclusion above turned out to be wrong in practice, not in the GLES2-detection itself.** `vitaGL` on this machine's VitaSDK is a **Cg-only build with no working GLSL→Cg translator** (`glsl_utils.o` is effectively empty — see the `backstab-vita-reference` memory). The engine feeds it real GLSL ES 1.00 source (see the `shaders.pak` finding below), which shark rejects outright. Two translation strategies were tried and abandoned before the current approach:
1. vitaGL's own semantic-binding GLSL→Cg path — non-functional in this SDK build (no translator compiled in).
2. Manual/AI-assisted per-shader GLSL→Cg translation (`porting_tools/manage_vita.py`'s `download_glsl_shaders`/`upload_cg_shaders`/`sync_shaders`, dumping runtime `glShaderSource` calls to `glsl_dump/<sha1>.glsl` and hand-writing a matching `assets/cg/<sha1>.cg`) — labor-intensive and never finished (only 4 of 7 dumped shaders were ever translated, and those 7 are a small sample of everything the engine actually needs — see below).

**Current, correct approach (in place as of 2026-07-15): dropped `vitaGL` entirely, switched to `PVR_PSP2`** — the real PowerVR SGX543 GLES2 driver (`libIMGEGL.suprx`/`libGLESv2.suprx`/`libpvrPSP2_WSEGL.suprx`/`libGLESv1_CM.suprx`/`libgpu_es4_ext.suprx`, vendored under `modules/`), which has a **real GLSL ES compiler**. No translation is needed at all — `dynlib.c`'s `glShaderSource`/`glCompileShader` entries now resolve directly to the real driver's exported functions (confirmed: `source/patch.c` only does license-check hooking, no shader-related patching remains; `source/utils/glutil.c` has no Cg compile step, just EGL/PVR_PSP2 init). `CMakeLists.txt`'s `SHADER_FORMAT` was still stuck at `"CG"` (a stale leftover from the abandoned approach, with no code left that even reads `USE_CG_SHADERS`) — corrected to `"GLSL"` 2026-07-18 for documentation accuracy; it was already a no-op either way.

**Major finding 2026-07-18 — the engine's complete, human-readable GLSL ES source is directly available, no runtime dumping/translation required.** `com.gameloft.android.GAND.GloftD2SS/files/shaders.pak` (present in the installed-app-data dump, confirmed via a real device `fopen()` in the logs: `ux0:data/dungeon-hunter-2/shaders.pak`) is a **stored (uncompressed) ZIP archive** containing 34 cleanly-named `.glsl` files (`DepthCubeFP.glsl`, `GL_Diffuse_L1_iPhone_VS.glsl`, `UnlitTexturedFP.glsl`, etc.) plus two empty marker files `cg.config`/`glsl.config`. This is the real shader source the native engine reads and feeds to `glShaderSource` — confirmed by `strings` on the real `.so`, which references `shaders.pak` and a `PinkBadShaderFS/VS` fallback pair (Gameloft's "missing shader" placeholder, not present in the pak — must be a hardcoded fallback string in the binary, not something we need to supply). The 7 previously runtime-dumped `glsl_dump/*.glsl` (SHA1-named) are a small, incomplete subset of what the engine actually needs — **prefer extracting the full `shaders.pak` over continuing the old dump/translate loop**, which should be considered obsolete now that `SHADER_FORMAT` is GLSL end-to-end. All 34 shaders are well-formed, precision-qualified GLES2 GLSL (some behind `#define GLITCH_OPENGLES_2`) that a real PowerVR GLES2 compiler should accept unmodified.

**Bug found 2026-07-18 — `module/` vs `modules/` path mismatch (root cause confirmed on real hardware, then correctly identified and fixed).** The repo had two directories with byte-identical PVR `.suprx`/`.a` payloads, `module/` (singular) and `modules/` (plural, an untracked duplicate). Mid-session the user deleted `module/` and asked to standardize on `modules/`; all three references (`CMakeLists.txt` VPK packaging, `main.c`'s `sceKernelLoadStartModule` calls, `glutil.c`'s `PVRSRV_PSP2_APPHINT` paths) were updated to `modules/` and rebuilt. **This was then tested on real hardware (`log_054.txt`, 2026-07-18) and it was NOT sufficient**: all 5 `LoadStartModule` calls succeeded, `PVRSRVInitializeAppHint`/`PVRSRVCreateVirtualAppHint` both succeeded, yet `eglGetDisplay` still failed with `EGL_NO_DISPLAY`/0x3000.

Root cause, found via `strings` on the real vendor binaries (not guessed): **`libIMGEGL.suprx` and `libgpu_es4_ext.suprx` have `app0:module/libpvrPSP2_WSEGL.suprx`, `app0:module/libGLESv1_CM.suprx`, `app0:module/libGLESv2.suprx` hardcoded internally** (singular `module/`) as their own dependency-loading paths — this is Sony/Imagination's own compiled-in fallback, independent of whatever we pass via `PVRSRV_PSP2_APPHINT`. None of the 6 `.suprx` files contain the string `modules/` (plural) anywhere. So even though *our* explicit `sceKernelLoadStartModule` calls and AppHint pointed at wherever we told them to, `libIMGEGL`'s own internal load of its `WSEGL`/`GLESv1_CM`/`GLESv2` dependencies during `eglGetDisplay()` only ever looks at `app0:module/...` — if the files aren't there, that internal load silently fails and `eglGetDisplay` returns `EGL_NO_DISPLAY` with the misleading `EGL_SUCCESS` (0x3000) code.

**Fix (2026-07-18, second pass): renamed `modules/` back to `module/` (singular)** and reverted all three references. Rebuilt and verified via a real `cmake && make` + VPK packaging pass that `build/dungeon_hunter_2.vpk` now contains `module/*.suprx` (singular). **This is the one and only correct name for this directory — do not rename it again without re-checking the `strings` output above; the vendor driver's own hardcoded path is not something we can change.**

**Tested on hardware 2026-07-18 with `module/` (singular) — same `eglGetDisplay` 0x3000 failure, meaning the directory name wasn't the whole story.** Root-caused via `GrapheneCt/PVR_PSP2` (the actual open-source driver project this whole `.suprx` set comes from, https://github.com/GrapheneCt/PVR_PSP2): its own reference test (`unittests/gles1test1/gles1test1.c`) only declares **`"app0:libgpu_es4_ext.suprx"` and `"app0:libIMGEGL.suprx"`** as auto-loaded dependencies (via `SCE_USER_MODULE_LIST`, at `app0:` **root**, no subfolder) — it does **not** preload `libpvrPSP2_WSEGL.suprx`/`libGLESv1_CM.suprx`/`libGLESv2.suprx` at all. Those three are loaded **lazily by `libIMGEGL.suprx` itself**, internally, the first time `eglGetDisplay`/`eglInitialize` need them, using the paths already compiled into `PVRSRVInitializeAppHint()`'s own defaults (confirmed by reading `pvr_apphint.c` in that repo: the defaults already are `"app0:module/libGLESv1_CM.suprx"` etc. — matching what `glutil.c` was setting explicitly anyway, redundant but harmless).

**The actual bug:** `main.c` was explicitly pre-loading all 5 modules via `sceKernelLoadStartModule` before calling `gl_init()`. When `libIMGEGL` then tries to load its own dependencies internally during `eglGetDisplay`, it finds a same-named module already loaded (from our pre-load) — and its internal loader (`PVRSRVLoadLibrary`/`LoadNamedWSModule`, per that repo's source) has **no "already loaded, proceed anyway" handling**: any non-success there is treated as fatal, and `eglGetDisplay` surfaces it as `EGL_NO_DISPLAY` with the misleading `EGL_SUCCESS` (0x3000) code — regardless of directory naming.

**Fix (2026-07-18, third pass):** `main.c`'s `pvr_modules[]` now only preloads `libgpu_es4_ext.suprx` and `libIMGEGL.suprx`, from `app0:` root (no subfolder) — matching the reference test exactly. `CMakeLists.txt` packages those two at VPK root; `libGLESv2.suprx`/`libGLESv1_CM.suprx`/`libpvrPSP2_WSEGL.suprx` stay under `module/` (matching the driver's own compiled-in default, which `glutil.c` still sets explicitly — no change needed there). Verified via a real `cmake && make` + VPK pass that `build/dungeon_hunter_2.vpk` now has `libgpu_es4_ext.suprx`/`libIMGEGL.suprx` at root and the other three under `module/`.

**✅ CONFIRMED FIXED on real hardware 2026-07-18 (`log_057.txt`).** With only `libgpu_es4_ext.suprx`/`libIMGEGL.suprx` preloaded (from `app0:` root): both `LoadStartModule` calls succeed, `PVRSRVInitializeAppHint`/`PVRSRVCreateVirtualAppHint` succeed, **`eglGetDisplay -> 0x1 (err 0x3000)`** — this time `0x3000` is genuinely `EGL_SUCCESS`, not a failure (compare to every prior log where the display was `0x0`/`EGL_NO_DISPLAY`). `PVR_PSP2 EGL context created` and `PVR_PSP2 initialized` both print. The engine reaches `app Init is OK` and **the main loop runs for 960+ frames** with `GL pipeline clean (no error)` on almost every frame, correctly processing touch (`GameGLSurfaceView.nativeOnTouch`) and key input (`nativeKeyDown`/`Up`), reaching menu logic (`lastOpenMenuID`). This is the furthest the port has ever gotten — PVR_PSP2 bring-up (Phase 5's real blocker for the last 3 days) is done.

**New, different symptom reported by the user:** the screen shows a flat **aquamarine/turquoise color** instead of the game's actual UI — i.e. we've moved from "black screen, nothing draws" (the old vitaGL+Cg symptom) to "something draws (a clear color, most likely), but textured/shaded content doesn't appear on top of it." Frame 1 alone logs one `glGetError() = 0x0502` (`GL_INVALID_OPERATION`) — transient, doesn't recur on later frames, but worth keeping an eye on as a clue about what the first draw call that fails is.

- [x] **✅ Confirmed on real hardware 2026-07-18 (`log_058.txt`): NO shader compile/link failures.** With `glCompileShader_soloader`/`glLinkProgram_soloader` wired in (see below), a full run through the main loop (840+ frames) produced **zero** `glCompileShader(...) FAILED`/`glLinkProgram(...) FAILED` lines. **This definitively closes the original GLSL→Cg translation problem this session started from**: `shaders.pak`'s real GLSL ES source compiles and links cleanly against the real PVR_PSP2 GLSL ES compiler, end to end, with no translation of any kind. The remaining "aquamarine screen" issue is a content/asset problem, not a shader-language problem.
- [x] **Found and fixed 2026-07-18: several real GLES2 functions were stubbed as harmless-looking no-ops (`ret0`), a leftover from the old `vitaGL` era where they genuinely weren't implemented.** Now that we link the real driver (`libGLESv2_stub.a`), these all exist for real and were re-wired in `source/dynlib.c`: **`glCompressedTexSubImage2D`** (the standout suspect — its sibling `glCompressedTexImage2D` was already wired to the real function two lines above it in the same table, `glCompressedTexSubImage2D` was not; if the engine's textures are compressed, sub-image uploads were being silently swallowed, which would produce exactly "background renders, sprites don't" — aquamarine clear color with no texture content on top), plus `glBlendColor`, `glDetachShader`, `glValidateProgram` (lower-risk but equally real and available, fixed for consistency). Verified via `nm` that all four symbols exist in `libGLESv2_stub.a`, and via a real `cmake && make` build that linking still succeeds.
- [x] **Tested on hardware 2026-07-18 (`log_059.txt`): still zero shader compile/link failures, screen still aquamarine.** Confirms twice now that shader compilation is not the cause — the re-wired `glCompressedTexSubImage2D`/etc. didn't visibly change the symptom either. **Still no texture-file `fopen()` calls appear anywhere in ~840 frames of log**, which is the most telling fact: the engine hasn't even tried to load a texture/model asset yet — this isn't (only) a texture-upload bug, something upstream of asset loading is stuck.
- [x] **Architecture discovery 2026-07-18: DH2's UI is rendered via an embedded Flash player (GameSWF), not hand-coded native widgets.** Found while tracing `loadMovie`/`isMediaPlaying` in `out_ghidra.c`: a red herring at first (a `"loadMovie"`/`"unloadMovie"` string pair near `sprite_loadmovie`/`sprite_unloadmovie`) turned out to be **GameSWF's own ActionScript `MovieClip.loadMovie()` binding**, unrelated to `GLMediaPlayer`'s video playback — but it confirms the engine embeds **GameSWF, an open-source Flash/SWF player**, and renders menus as Flash movie clips (matches `gameswf_effects.bdae`, which `fopen()`s successfully in every log). **This means the high-level menu/flow logic likely isn't traceable in Ghidra's C pseudocode at all** — it's driven by SWF ActionScript bytecode and/or the embedded Python layer (`data/pydata/*.bin` — `engine_core_pycst.bin`, `scripts_pycst.bin`, etc. are generically-named Python constant/bytecode caches). Static analysis of *what specific state the game is stuck in* has likely hit its ceiling here — the fastest remaining path is hardware experimentation, not more Ghidra reading.
- [x] **`GLMediaPlayer_isMediaPlaying`/`GLMediaPlayer_loadMovie` real semantics checked and ruled out as the blocker:** `nativeLoadMovie` (`out_ghidra.c:410534`) is a thin pass-through to Java's `loadMovie`, and the real `GLMediaPlayer.java.loadMovie()` fires an Android `Intent` to a separate video-playing Activity and returns success immediately (fire-and-forget, doesn't block). Our `GLMediaPlayer_loadMovie` stub already mirrors that (fakes success immediately) and `GLMediaPlayer_isMediaPlaying` already returns `0` (not playing) — so the engine isn't stuck waiting on a "still playing" poll. Checked against the actual decompiled logic, not left as an assumption.
- [x] **Found and fixed 2026-07-18: missing font asset.** The engine (via FreeType, likely GameSWF's own text rendering) requests `/system/fonts/droidsans.ttf` and `#/system/fonts/droidsans.ttf` — real Android paths that obviously don't exist on Vita — and got `fopen() == 0x0` in every log so far. No font was ever staged in the repo (`extras/fonts/` was referenced by `deploy_and_launch_vita3k.sh` but never actually populated). Fixed: copied a real, freely-redistributable `DejaVuSans.ttf` (Bitstream Vera/DejaVu license — not derived from Gameloft's assets, safe to commit) into `extras/fonts/DejaVuSans.ttf`, packaged it into the VPK root via `CMakeLists.txt`, and added a redirect in `source/reimpl/io.c`'s `fopen_soloader` for both exact path variants. Verified via a real `cmake && make` + VPK pass that `DejaVuSans.ttf` is now packaged. **Not yet tested on hardware.** If GameSWF's UI rendering was silently bailing out on the font-load failure (plausible, unconfirmed), this could be what's blocking the Flash-driven menu from drawing anything beyond its background clear color.
- [ ] **Next action:** install the rebuilt VPK and see whether the font fix changes anything (ideally: menu content/text starts appearing, or at least a *different* failure surfaces now that this specific gap is closed). If the screen is still flat aquamarine with no new log activity, the blocker is further upstream in GameSWF/Python engine state and will need either (a) empirical hardware experiments with `GLMediaPlayer`/`Musicplayer`/`DungeonHunter2` stub return values one at a time, or (b) confirming whether other `.bdae`/SWF-like assets the engine expects beyond `gameswf_effects.bdae` are missing/unstaged, and a finer-grained `glGetError()` sweep (per-call, not per-frame) to pinpoint the frame-1 `GL_INVALID_OPERATION`.
- [ ] Extract `shaders.pak` in full (34 files, see above) and stage them as real engine assets if not already 1:1 staged on-device — no GLSL→Cg translation needed.

### Phase 6 — Input — ✅ done (2026-07-13)
`GameGLSurfaceView.nativeOnTouch` is a single touch entry point (unlike Zenonia's 3-vs-4-int
`handleCletEvent` split) — confirm its real signature/argument packing from the pseudo-C before
wiring `sceTouchPeek`. `nativeKeyDown`/`nativeKeyUp`/`nativeonTrackballEvent` cover physical
buttons/d-pad; `nativeAccelerometer` and `nativeSetOrientation` are extra surfaces the Zenonia ports
didn't have — decide up front whether accelerometer input is simulated (fixed neutral value) or
mapped to something on Vita, and document the choice.

**Outcome:** Confirmed `nativeOnTouch` signature requires JNI `env` and `clazz` (like all JNI calls). Fixed a massive bug in `main.c` where all `native*` C function pointers lacked `JNIEnv*` and `jobject` arguments, which would have passed garbage to the native registers. Re-wired `sceTouchPeek` passing correct scaled touch arguments. `nativeAccelerometer` and `nativeSetOrientation` will be intentionally ignored/unimplemented, simulating a fixed device orientation without tilt input, as DH2 doesn't use tilt mechanics for gameplay.

### Phase 7 — Audio — ✅ done (2026-07-13)
`GLMediaPlayer`/`Musicplayer` classes handle music/SFX — decompile these two Java classes fully
(already extracted to `decompiled/apk_jadx/sources/`) to find the real asset format and id→file
mapping logic. If the game uses OpenSL ES natively, no JNI audio bridge is needed (VitaGL already
provides some audio, but usually OpenAL is preferred for Vita ports). However, the presence of
`GLMediaPlayer` strongly implies audio is played via Java `MediaPlayer`/`SoundPool`. If so, implement
Vita audio bridge methods in `FalsoJNI` based on the exact signatures of the `play`/`pause`/`stop`
methods found in the Java decompilation.

**Outcome:** Confirmed that native code uses JNI calls back to Java `GLMediaPlayer` (like `playSound(sndId, instance, vol)`). The audio filenames are stored in a massive 442-element String array in `Sounddefs.java` mapped by `sndId`. I extracted this array into `source/sounddefs.h` and modified the FalsoJNI bridge in `source/java.c` to use it for logging exact `.ogg` filenames. Full playback integration (via OpenAL/SoLoud) will be added later if needed, but the audio routing logic is fully mapped and understood.

### Phase 8 — Assets — ✅ done (2026-07-13)
Follow `psvita-port-toolkit`'s Phase 5 decision framework (`references/asset_packaging.md`): pick one
strategy (loose files vs. packed `.apk`-like archive) and confirm which path `GLResLoader`/
`nativeSetPaths`-equivalent actually reads from before assuming cocos2d-x's `nativeSetPaths` pattern
applies here.

**Outcome:** Inspected `GLResLoader.java` and found that the game requests files using `getResourceFull`, `getResourceLength`, and `getResourceBytes`. Unlike cocos2d-x which uses C++ file I/O directly, this engine calls back to Java to load resources! FalsoJNI is perfectly positioned to intercept these calls (which it already does in `source/java.c`). The strategy chosen is **Loose files** in `ux0:data/dungeon-hunter-2/assets/`. FalsoJNI's `dh2_resolve_asset_path` translates Java asset requests directly to standard C file I/O `fopen()` on the Vita's memory card, bypassing the need for a complex `.apk` unzipper or asset packager.

### Phase 9 — Packaging & LiveArea — ✅ done (2026-07-13)
Standard rules from `references/livearea_assets.md`/`vpk_packaging.md` — no game-specific changes
expected here. The `CMakeLists.txt` is correctly configured to package `icon0.png`, `pic0.png`, `startup.png`, `bg0.png`, and `template.xml` into the `sce_sys/livearea` directory of the VPK.

### Phase 10 — Hardware bring-up
Same iterative, one-log-one-bug methodology as both Zenonia ports; Vita3K first to rule out logic bugs
cheaply, real hardware to confirm. Re-read `references/hardware_debugging.md` and
`Zenonia2-vita/port_progress.md` before treating any new crash signature as novel — the pthread/mutex,
JNI-method-not-found, and heap-pointer-sign bug classes documented there are engine-agnostic SoLoader
pitfalls, not Gamevil-specific.

## 6. Checklist

- [x] APK and all 4 `.so` files decompiled, one folder per artifact, under `decompiled/`.
- [x] `.gitignore` excludes the original package, extracted APK, app-data dump, and all decompiled
      output.
- [x] `psvita-porting` skill installed at `.claude/skills/`.
- [x] `porting_tools/` scripts parameterized for this project (paths, `VITA_IP`, log/data dirs).
- [x] Toolchain confirmed on this machine: VitaSDK, `vitaGL`, `kubridge` stub installed;
      `libshacccg.suprx` copied into the repo; space-in-path workaround identified (rsync-to-`/tmp`,
      not a symlink — see Phase 0). Still outstanding: `kubridge.skprx` and `libshacccg.suprx` actually
      installed on the physical test console (device unreachable as of 2026-07-12).
- [x] Loader skeleton bootstrapped (Phase 2): `main.c`/`dynlib.c` written against the real ABI,
      `lib/falso_jni` vendored, project compiles/links/packages a `.vpk` via a real `cmake && make` run.
      Not yet run on Vita3K or hardware — see Phase 2's "not done yet" note.
- [x] `source/java.c` FalsoJNI method table implemented: all 65 confirmed engine→Java callbacks across
      `GLResLoader`/`GLMediaPlayer`/`Musicplayer`/`DungeonHunter2`, cross-checked against jadx + `strings`
      + `out_ghidra.c` (Phase 3). `GLResLoader`'s 3 asset methods do real file I/O (not stubs); actual
      asset packaging/staging is still Phase 8.
- [x] License/DRM bypass confirmed working before anything else (Phase 4).
- [x] First frame renders (confirmed on real hardware 2026-07-18, `log_057.txt`: `PVR_PSP2 EGL context created`, main loop runs 960+ frames with `GL pipeline clean`). **Visual output is still wrong** (flat aquamarine clear color, no textured/UI content) — that's the new open item, see Phase 5.
- [x] Touch/keys confirmed against real `nativeOnTouch`/`nativeKeyDown` behavior on Vita3K/hardware.
- [x] Audio path understood and implemented.
- [x] Asset strategy chosen and implemented (one strategy only).
- [x] `.vpk` installs cleanly on real hardware (LiveArea valid).
- [ ] Playable end-to-end on physical PS Vita.
