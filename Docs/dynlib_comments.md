# Technical Documentation: `source/dynlib.c`

This document explains the dynamic library symbol resolution and C/C++ import mapping for `libDungeonHunter2.so` implemented in [`source/dynlib.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/dynlib.c).

---

## 1. Purpose of `dynlib.c`

When the native Android library (`libDungeonHunter2.so`) is loaded into PS Vita memory via `so_util`, it requires resolving hundreds of imported symbols from the Android OS environment (Bionic libc, libm, liblog, libz, EGL, GLESv2, pthreads).

`dynlib.c` maps Android symbol strings to native PS Vita functions or custom `_soloader` wrappers.

---

## 2. Special Wrappers

### 2.1 File I/O (`_soloader` I/O)
- Functions such as `fopen`, `fread`, `fseek`, `ftell`, `fclose`, `fgets`, `fgetc`, `fwrite`, `feof`, `ferror`, `fflush`, `fileno`, `fputc`, `fputs`, `setvbuf`, `ungetc` are routed to `_soloader` wrappers in `source/reimpl/io.c`.
- **Reason:** The `fopen_soloader` file loading system supports cached reading for small asset files alongside direct disk access. The wrappers verify whether a `FILE*` handle is a real pointer or a cache index before calling `sceLibcBridge_*` or `newlib`.

### 2.2 Android Bionic Replacements
- `__assert2`: Extended Bionic assertion handler (`__assert2(file, line, func, msg)`). Redirects failures to the port's logging system (`l_fatal` and `fatal_error`).
- `__isfinitef`: Bionic 32-bit floating-point `isfinite()` helper.
- `uname`: Simulated implementation (*fake_utsname*) returning a consistent Linux ARMv7 environment (`sysname="Linux"`, `machine="armv7l"`, `release="3.4.0"`), preventing engine checks from interpreting syscall failures as errors.
- `JAVA_SOUNDS`: Reserved 4096-byte static buffer for data symbol imports exported by the `.so`.

---

## 3. Dynamic Link Table (`default_dynlib[]`)

The `default_dynlib[]` array links hundreds of symbols categorized into:
- **C++ Internals / ARM ABI:** C++ exception runtime (`__cxa_*`), type destructors, and ARM division/multiplication helpers (`__aeabi_*`).
- **Character Types (`ctype`):** Bionic character lookup tables (`_ctype_`, `_tolower_tab_`, `_toupper_tab_`).
- **Android Logging:** `__android_log_print`, `__android_log_write`, `__android_log_assert`.
- **Android Asset Manager:** `AAsset_read`, `AAssetManager_open`, etc.
- **Math Library (`libm`):** `sin`, `cos`, `atan2f`, `sqrtf`, `powf`, `sincosf`, etc.
- **Networking & Sockets:** `socket`, `bind`, `connect`, `send`, `recv`, `select`, `poll`, `gethostbyname`.
- **EGL & OpenGL ES 2.0:** EGL calls (`eglSwapBuffers`, `eglMakeCurrent`) and OpenGL ES (`glDrawElements`, `glCompileShader`, `glBindTexture`, `glUniform4fv`, etc.).
- **POSIX Threads (`pthreads`):** `pthread_create`, `pthread_mutex_lock`, `pthread_cond_wait`, `sem_wait`, etc.
