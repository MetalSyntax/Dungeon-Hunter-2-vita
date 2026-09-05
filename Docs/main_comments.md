# Technical Documentation: `source/main.c`

This document details the main initialization, CPU/GPU clock control, touch/button input handling, and main game loop implemented in [`source/main.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/main.c).

---

## 1. Initialization Sequence

The `main()` entry point performs the following boot sequence:

1. **Clock Frequency Setup (Maximum Safe Overclock):**
   - Sets Vita hardware clock frequencies to the maximum official SDK limits:
     - CPU ARM: 444 MHz (`scePowerSetArmClockFrequency(444)`)
     - System BUS: 222 MHz (`scePowerSetBusClockFrequency(222)`)
     - GPU: 222 MHz (`scePowerSetGpuClockFrequency(222)`)
     - GPU Crossbar: 166 MHz (`scePowerSetGpuXbarClockFrequency(166)`)
2. **Thread & Working Directory Setup:**
   - Calls `pthread_init()` to prevent `EAGAIN` (error code 11) during `pthread_create`.
   - Changes working directory to `DATA_PATH "assets/"` ensuring relative `fopen()` calls for `.bdae` models succeed.
3. **Native & PVR Module Loading:**
   - Calls `soloader_init_all()` to load `libDungeonHunter2.so` and resolve imports.
   - Pre-loads Vita GPU driver modules: `libgpu_es4_ext.suprx` and `libIMGEGL.suprx`.
   - Initializes graphics environment `gl_init()` (PVR_PSP2), `video_init()`, and `audio_init()`.
4. **JNI Engine Registration Sequence:**
   - Executes JNI initialization functions in exact Android boot order:
     `nativeGetInfo` -> `nativeSetPhone(960, 544)` -> `nativeGameRenderer` -> `nativeConfig` -> `nativeGetJNIEnv` -> `nativeGLMediaPlayerInit` -> `nativeGLResLoaderInit` -> `nativeMusicplayerInit` -> `nativeGLUtilsDeviceInit` -> `nativeInit(0)` -> `nativeRendererInit(1)` -> `nativeOnSurfaceChanged(960, 544)`.

---

## 2. Input System (Physical & Touch Controls)

### 2.1 Physical Buttons & Android KeyEvents
- D-Pad, Start, Select, and face buttons are polled via `sceCtrlPeekBufferPositive`.
- Directional buttons emit `android.view.KeyEvent` codes (`AKEYCODE_DPAD_UP`, `AKEYCODE_DPAD_DOWN`, etc.) via `nativeKeyDown` and `nativeKeyUp`.
- **`nativeKeyDown` Analysis:** Static Ghidra analysis revealed `appKeyPressed` is an empty no-op function in this Android build. Therefore, primary combat actions cannot be triggered via standard keyboard/gamepad events.

### 2.2 Synthetic Touch Pressing for Combat Actions (`action_btn_map`)
Dungeon Hunter 2 combat actions (primary attack, block, dodge, potion usage) are designed exclusively for Flash HUD on-screen touch buttons (`dqhud.swf`). To enable physical button play, each physical Vita button is mapped to synthetic touch events at exact screen coordinates:

| Physical Button | Screen Coordinate (960x544) | Pointer ID | Mapped Action |
| :--- | :--- | :--- | :--- |
| **Cross (`X`)** | (180, 445) | `1` | Bottom-left red sphere (Primary attack) |
| **Square (`Square`)** | (683, 373) | `2` | Sword+flame icon (Block / Heavy attack) |
| **Triangle (`Triangle`)** | (760, 453) | `3` | Sword+helmet icon (Dodge / Special ability) |
| **L1** | (810, 273) | `4` | Gold rune-wheel icon (Skill slot) |
| **R1** | (820, 50) | `5` | Health potion quick-use icon (Potion usage) |

### 2.3 Touch Screen Coordinate Conversion
The Vita front touch screen has a native resolution of 1920x1088. Touch inputs are scaled to 960x544 and converted to the engine's logical resolution (960x640) using `glutil_screen_touch_to_logical` for accurate UI button detection.

---

## 3. Main Loop Logic (`while (1)`)

- **Emergency Exit Combination:** Holding `START + SELECT` simultaneously exits the main loop; before terminating, the port now persists settings (`SavegameManager::saveSettings`) and drains the async save queue (`Savegame::FlushJobs(NULL)`, same call `Application::Quit` makes) so progress queued in RAM is not lost.
- **Language sanitize (one-shot):** After the `SavegameManager` instance exists, the persisted language is read once via `SavegameManager::getLanguage()` and only reset to English (0) if out of range (>7, same clamp the engine uses in its own `menu_language` flow). A previous 180-frame forced-English overwrote the user's choice on every boot, making language changes un-persistable by design; removed.
- **Periodic async-save drain:** Every 600 frames, if the `AddJob` hook flagged queued save jobs, `Savegame::FlushJobs(NULL)` runs on the main thread -- the same drain `Application::Quit` performs -- so `Savegame::saveAll` jobs reach disk even if the engine's fire-and-forget `UpdateJobs` workers never run. Cheap no-op when workers are healthy.
- **Continuous FPS Measurement:** Measures real frame rendering rates (`[fps]`) using `sceRtcGetCurrentTick`, outputting results to logs every 3 seconds.
- **OpenGL Error Auditing:** Audits `glGetError()` every 60 frames to detect potential GLES2 pipeline issues.
