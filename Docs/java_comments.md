# Technical Documentation: `source/java.c`

This document details the C/C++ native implementation of Java static method handlers (FalsoJNI) in [`source/java.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/java.c).

---

## 1. How FalsoJNI Works

The `libDungeonHunter2.so` native engine invokes static Java methods using JNI (`GetStaticMethodID`, `CallStaticVoidMethod`, `CallStaticIntMethod`, `CallStaticObjectMethod`). FalsoJNI intercepts these calls in C++, enabling a native simulation of the Android environment on the PS Vita.

Function names in `java.c` were determined by cross-referencing three sources:
1. Decompiled Java source code (`GLResLoader.java`, `GLMediaPlayer.java`, `Musicplayer.java`, `DungeonHunter2.java`).
2. String analysis from the native binary (`strings libDungeonHunter2.so`).
3. Ghidra decompilation in `out_ghidra.c` to verify exact parameter signatures expected by `GetStaticMethodID`.

---

## 2. Simulated Classes & Modules

### 2.1 `GLResLoader` (Resource Asset Loader)
- `GLResLoader_getResourceFull`: Loads an asset file from `DATA_PATH"assets/<name>"` into a dynamic Java byte array (`JavaDynArray`).
- `GLResLoader_getResourceLength`: Returns file size in bytes for a given asset.
- `GLResLoader_getResourceBytes`: Reads a chunk defined by offset and length from an asset.

### 2.2 `GLMediaPlayer` (Video Playback & Audio Bridge)
- `GLMediaPlayer_loadMovie`: Entry point for cutscenes and video playback. Delegates to `video_play(name)` in `video.cpp` (using `SceAvPlayer`) and updates the native global symbol `videoDone = 1` to notify the engine that video playback finished.
- `GLMediaPlayer_getWidth`: Returns the native screen width of the PS Vita (`960` pixels).
- `GLMediaPlayer_detectPhoneLang`: Returns `0` (Default English).
- `GLMediaPlayer_getSDFolder`: Returns base storage path (`DATA_PATH`).

### 2.3 `Musicplayer` (BGM Music Player)
- `Musicplayer_GetNumPlaylists`: Returns `0` to signal no external playlists, forcing the engine to use native sound paths.

### 2.4 `DungeonHunter2` (Main Game Callbacks)
- `DungeonHunter2_Exit`: Intercepts game exit requests and terminates the process cleanly via `sceKernelExitProcess(0)`.
- `DungeonHunter2_sendAppToBackground`, `OpenGLive`, `OpenIGP`, `NotifyTrophy`: Informative no-ops for Gameloft Live and cross-promo features not present in the Vita port.

### 2.5 Verizon Billing & Network Services (`VZ*`)
- Methods such as `VZIsInProgress`, `VZIsErrorOcurred`, `VZRequestLogin`, `VZRequestPurchaseGame`, etc.
- **Strategy:** Return an immediate error state or "never in progress" (`VZIsInProgress = 0`, `VZIsErrorOcurred = 1`). This prevents the engine from blocking in wait loops for microtransactions or Verizon network validation.

---

## 3. Symbol Mapping (`nameToMethodId[]`, `methodsInt[]`, `methodsObject[]`, `methodsVoid[]`)

FalsoJNI links internal enum IDs (`M_getResourceFull`, `M_loadMovie`, etc.) with their corresponding C++ function pointers and return types (`METHOD_TYPE_INT`, `METHOD_TYPE_OBJECT`, `METHOD_TYPE_VOID`).
