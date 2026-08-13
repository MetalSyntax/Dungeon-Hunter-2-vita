# Technical Documentation - Dungeon Hunter 2 PS Vita Port

This directory contains technical documentation for each source file in the Dungeon Hunter 2 PlayStation Vita port project, built using SoLoader and FalsoJNI.

## Documentation Index

1. [**Docs/patch_comments.md**](patch_comments.md) - Technical analysis of `source/patch.c` (internal `.so` function patching, DRM/license bypasses, `HUDStyle` override, and analysis of diagnostic hooks disabled due to ARM stack misalignment).
2. [**Docs/audio_comments.md**](audio_comments.md) - Documentation of `source/audio.cpp` (WAV/IMA-ADPCM decoding, VoxN container parser, `sceAudioOut` mixer, `android/media/AudioTrack` shim, and `GLMediaPlayer` JNI bridge).
3. [**Docs/dynlib_comments.md**](dynlib_comments.md) - Documentation of `source/dynlib.c` (dynamic library symbol resolution for `libDungeonHunter2.so`, Bionic libc tables, POSIX, EGL, OpenGL ES 2.0, and pthreads).
4. [**Docs/java_comments.md**](java_comments.md) - Documentation of `source/java.c` (FalsoJNI bridge between `.so` and static Java callbacks for `GLResLoader`, `GLMediaPlayer`, `Musicplayer`, and `DungeonHunter2`).
5. [**Docs/main_comments.md**](main_comments.md) - Documentation of `source/main.c` (main entry point, `.so` boot sequence, Vita hardware overclocking, touch/button input mapping, and render loop).
6. [**Docs/video_comments.md**](video_comments.md) - Documentation of `source/video.cpp` (native video playback via `SceAvPlayer`, CDRAM/PHYCONT memory allocation, GPU-accelerated NV12 YUV conversion, and audio synchronization).

## Utilities & Reimplementation Documentation (`source/utils` & `source/reimpl`)

### Utilities (`source/utils/`)
- [`dialog.c.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/dialog.c.md) | [`Docs/utils/dialog.c.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/Docs/utils/dialog.c.md) - SceImeDialog & SceMsgDialog implementation & analog stick mode restore.
- [`dialog.h.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/dialog.h.md) - Dialog header guards and prototypes.
- [`glutil.c.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/glutil.c.md) - GLES2 wrappers, shader binary cache preloading, and "invisible enemies" diagnostic tracking.
- [`glutil.h.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/glutil.h.md) - Viewport 960x640 -> 960x544 letterbox remapping and touch coordinate conversion.
- [`init.c.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/init.c.md) - `.so` module loading at `0x98000000`, kubridge check, and hardware overclock setup (444 MHz).
- [`init.h.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/init.h.md) - Loader init routines API.
- [`logger.c.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/logger.c.md) - Thread-safe logging, TTY ANSI colors, and non-ANSI file output buffers (`log_XXX.txt`).
- [`logger.h.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/logger.h.md) - Log levels and `DEBUG_SOLOADER` macros.
- [`settings.c.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/settings.c.md) - Configurator settings loader (`config.txt`).
- [`settings.h.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/settings.h.md) - Configurator variables declarations.
- [`utils.c.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/utils.c.md) - File IO helpers, SHA1 hashing, string manipulation, and kernel module checks.
- [`utils.h.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/utils.h.md) - Helper utilities API header.

### Reimplementations (`source/reimpl/`)
- [`asset_manager.cpp.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/asset_manager.cpp.md) | [`Docs/reimpl/asset_manager.cpp.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/Docs/reimpl/asset_manager.cpp.md) - Android NDK `AAssetManager` C++ reimplementation.
- [`asset_manager.h.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/asset_manager.h.md) - `AAssetManager` API header.
- [`egl.c.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/egl.c.md) - EGL 1.4 API bridge, 220 DPI scaling, and context/surface stubs over VitaGL.
- [`egl.h.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/egl.h.md) - EGL constants and prototypes.
- [`errno.c.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/errno.c.md) - Bionic <-> Newlib errno translation table and `strerror` wrappers.
- [`errno.h.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/errno.h.md) - Errno translation API header.
- [`io.c.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/io.c.md) - POSIX IO reimplementation and `fcache` read-only RAM file cache for fast asset loading.
- [`io.h.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/io.h.md) - `stat64_bionic` & `dirent64_bionic` struct alignments.
- [`log.c.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/log.c.md) - Android `liblog` (`__android_log_print`) bridge.
- [`log.h.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/log.h.md) - Android log priority enum and declarations.
- [`mem.c.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/mem.c.md) - `mmap` / `munmap` emulation over `malloc`.
- [`mem.h.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/mem.h.md) - Memory emulation header.
- [`pthr.c.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/pthr.c.md) - POSIX threads bridge, forced `PTHREAD_CREATE_DETACHED` (kernel thread slot leak fix), and `__sinit` C++ exception fix.
- [`pthr.h.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/pthr.h.md) - Bionic <-> Newlib pthread struct adapters.
- [`sys.c.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/sys.c.md) - `clock_gettime` SceRtc Epoch adjustment (1969 years microsecond offset) and atomic primitives.
- [`sys.h.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/sys.h.md) - System functions API header.
- [`time64.c.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/time64.c.md) - 64-bit time functions (Y2038 bug mitigation).
- [`time64.h.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/time64.h.md) - `time64_t` API header.
- [`time64_config.h.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/time64_config.h.md) - 64-bit time configuration.
- [`bits/_ctype.c.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/bits/_ctype.c.md) - Bionic character classification tables.
- [`bits/_errno_bionic.h.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/bits/_errno_bionic.h.md) - Bionic errno constants.
- [`bits/_struct_converters.c.md`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/bits/_struct_converters.c.md) - Inline converters for `open()` flags, `stat`, and `dirent`.

