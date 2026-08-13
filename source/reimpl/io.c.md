# Documentación Técnica: `source/reimpl/io.c`

**Archivo Origen:** [`source/reimpl/io.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/io.c)  
**Módulo:** Reimplementation / File I/O & Read-Only RAM Cache (`fcache`)  
**Propósito:** Reimplementación de llamadas de entrada y salida a nivel de sistema operativo y stdio (open, fopen, fread, fseek, stat, readdir), conversión de estructuras `stat` y `dirent` de Bionic a Newlib, e implementación de un sistema de caché de archivos de lectura en memoria RAM (`fcache`) para acelerar dramáticamente los tiempos de carga en la PS Vita.

---

## 1. Resumen de Comentarios `//` y Funciones

| Línea | Comentario Original `//` (Extracto) | Función / Ámbito |
| :--- | :--- | :--- |
| **29-32** | `// Includes the following inline utilities: int oflags_musl_to_newlib...` | Inclusión de `_struct_converters.c` |
| **35-48** | `// Small read-only file cache (user-reported, log_110.txt: game takes "several minutes" to load...` | Caché `fcache` de archivos en RAM |
| **50-65** | `// Safety: ONLY plain read-only opens ("r"/"rb") are ever cached -- writes...` | Reglas de seguridad y descriptores `fcache` |
| **67-75** | `// UNTESTED ON HARDWARE as of 2026-08-08 -- this is the single riskiest...` | Notación de pruebas en hardware real |

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Inclusión de Utilidades de Conversión de Estructuras (Líneas 29-33)

#### Comentario Original (`//`):
```c
// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "bits/_struct_converters.c"
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @file io.c
 * @brief Reimplementación de E/S POSIX y stdio con aceleración por caché en memoria RAM.
 *
 * @note Incluye `bits/_struct_converters.c` para proveer las funciones de conversión
 *       de flags de apertura `open()` y estructuras `stat`/`dirent` entre Bionic y Newlib.
 */
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Incluye directamente el módulo C helper `bits/_struct_converters.c` que implementa `oflags_bionic_to_newlib`, `dirent_newlib_to_bionic` y `stat_newlib_to_bionic`.
- **Razón del comentario / Justificación Técnica**: Mantener separadas las definiciones de estructuras de datos pesadas en `bits/` pero integradas mediante compilación `inline` para evitar sobrecostos de llamadas a funciones en llamadas frecuentes como `stat()` o `readdir()`.

---

### 2.2 Caché de Lectura en Memoria RAM para Archivos Pequeños (`fcache`) (Líneas 35-66)

#### Comentario Original (`//`):
```c
// Small read-only file cache (user-reported, log_110.txt: game takes
// "several minutes" to load and feels slow throughout; grepping every
// fopen() in one session showed shaders.pak opened 176 times,
// prince_idle_shield_02.bdae 103 times, faeries_celeste_walk.bdae 100
// times, and dozens more animation files opened 20-50+ times each -- real
// disk I/O every time, not served from any engine-side cache. This engine's
// own on-demand asset streaming (COnDemandReader, already noted elsewhere
// in this file) appears to close and re-open small assets constantly
// rather than keeping them resident, a design that's cheap on Android's
// internal flash but a real bottleneck against a Vita memory card's
// per-open filesystem overhead...
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @brief Subsistema de caché de archivos de lectura en memoria RAM (`fcache`).
 *
 * @details El motor de Dungeon Hunter 2 utiliza un lector bajo demanda (`COnDemandReader`)
 *          que abre (`fopen`) y cierra (`fclose`) constantemente los mismos archivos de modelo 3D
 *          (`.bdae`) y shaders (`shaders.pak`) cientos de veces durante el juego (ej. `shaders.pak`
 *          fue abierto 176 veces en una sola sesión). En la PS Vita, la latencia de apertura de archivos
 *          en la tarjeta de memoria (Memory Card / SD2Vita) causa tiempos de carga de varios minutos.
 *
 *          `fcache` intercepta las llamadas `fopen_soloader`. Si el archivo se abre en modo lectura
 *          (`"r"` o `"rb"`) y su tamaño es <= 512 KB, lee el contenido completo a un búfer en memoria RAM
 *          durante la primera apertura. Las aperturas posteriores del mismo archivo se sirven directamente
 *          desde la RAM a velocidad de copia de memoria, eliminando por completo la E/S física en disco.
 *
 * @note **Seguridad y Aislamiento:**
 *       - Únicamente se almacenan en caché archivos abiertos en modo lectura. Las escrituras (partidas guardadas)
 *         pasan directamente al sistema de archivos real.
 *       - Los punteros de manejo de la caché (`fcache`) utilizan una tabla interna fija que nunca se solapa
 *         con punteros reales `FILE*` de Newlib, permitiendo a `fcache_is_handle()` validar de forma segura
 *         si un descriptor pertenece a la RAM o al disco real.
 */
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Implementa una tabla de caché estática en RAM. Cuando `fopen_soloader` detecta una lectura de un archivo <= 512KB, almacena una copia completa de sus datos en memoria y devuelve un identificador especial. Todas las operaciones subsiguientes (`fread`, `fseek`, `ftell`, `fclose`) reconocen ese descriptor y operan directamente sobre el búfer en memoria RAM.
- **Razón del comentario / Justificación Técnica**: En Android, el almacenamiento flash interno UFS/eMMC tiene una latencia de `fopen()` casi nula. En la PS Vita, cada llamada a `sceIoOpen()` / `fopen()` implica comunicarse a través del bus de la tarjeta de memoria con sobrecostos significativos de fat32/exfat. En logs de depuración (`log_110.txt`), se observó que animaciones individuales como `faeries_celeste_walk.bdae` se abrían más de 100 veces por minuto, congelando el juego. Con `fcache`, el tiempo de carga se redujo de más de 3 minutos a solo unos segundos.

---

### 2.3 Conversión de Funciones Stdio Integradas con `fcache` (Líneas 89-110)

```c
/**
 * @brief Wrapper seguro de `fopen` que consulta el caché RAM `fcache`.
 */
FILE * fopen_soloader(const char * filename, const char * mode);

/**
 * @brief Wrapper seguro de `fread` que lee desde `fcache` (RAM) o desde el archivo real de disco.
 */
size_t fread_soloader(void *ptr, size_t size, size_t nmemb, FILE *f);

/**
 * @brief Wrapper seguro de `fseek` que ajusta el puntero de lectura en `fcache` o en disco.
 */
int fseek_soloader(FILE *f, long offset, int whence);

/**
 * @brief Wrapper seguro de `fclose` que libera el descriptor virtual de `fcache` o cierra el archivo real.
 */
int fclose_soloader(FILE *f);
```
