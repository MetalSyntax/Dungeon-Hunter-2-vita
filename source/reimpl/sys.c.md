# Documentación Técnica: `source/reimpl/sys.c`

**Archivo Origen:** [`source/reimpl/sys.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/sys.c)  
**Módulo:** Reimplementation / System Functions & Time Services  
**Propósito:** Reimplementación de funciones de sistema POSIX/Android (`clock_gettime`, `clock_getres`, `clock`, `sigaction`, `__system_property_get`, `syscall`, `__stack_chk_fail`, `getenv`, `setenv`) y primarias atómicas (`__atomic_dec`, `__atomic_inc`, `__atomic_swap`, `__atomic_cmpxchg`).

---

## 1. Resumen de Comentarios `//` y Funciones

| Línea | Comentario Original `//` | Función / Ámbito |
| :--- | :--- | :--- |
| **38** | `// 1969 years in microseconds, used to adjust SCE tick to UNIX timestamp` | Macro `#define __epoch` |

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Conversión de Tiempo SCE a UNIX Epoch (Líneas 38-67)

#### Comentario Original (`//`):
```c
// 1969 years in microseconds, used to adjust SCE tick to UNIX timestamp
#define __epoch 62135587294000000
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @def __epoch
 * @brief Diferencia en microsegundos entre la época del reloj en tiempo real de la PS Vita (SceRtc, año 0001) y la época UNIX (1 de enero de 1970).
 *
 * @details El reloj interno de hardware de la PS Vita (`sceRtcGetCurrentTick`) calcula los ticks transcurridos desde
 *          el año 1 AD en microsegundos. Para obtener una marca de tiempo POSIX/UNIX compatible con `clock_gettime(CLOCK_REALTIME)`,
 *          es necesario sustraer `62135587294000000` microsegundos (equivalente a 1969 años transcurridos).
 */
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Define la constante `62135587294000000` empleada en `clock_gettime_soloader()` para casos `BIONIC_CLOCK_REALTIME`. Sustrae este valor del tick de hardware devuelto por `sceRtcGetCurrentTick(&tick)`.
- **Razón del comentario / Justificación Técnica**: En Android Bionic, `clock_gettime(CLOCK_REALTIME)` retorna los segundos transcurridos desde el 1 de enero de 1970 (UNIX Epoch). En VitaOS, la API de reloj en tiempo real (`SceRtc`) cuenta microsegundos a partir del año 0001 AD. Sin este ajuste de época, las llamadas del motor para verificar fechas, sincronizar animaciones temporales o conectar con servidores reportarían años en el futuro distante (~año 3995), provocando fallos en la lógica interna del juego.

---

### 2.2 Funciones de Reloj y Propiedades de Sistema (Líneas 40-95)

#### Función `clock_gettime_soloader()` (Líneas 40-73)
```c
/**
 * @brief Reimplementación de `clock_gettime` para relojes Bionic en PS Vita.
 *
 * @param[in]  clock_id Identificador del reloj (`BIONIC_CLOCK_MONOTONIC`, `BIONIC_CLOCK_REALTIME`, etc.).
 * @param[out] tp Estructura `timespec` donde se devolverá el tiempo en segundos y nanosegundos.
 *
 * @return 0 en caso de éxito.
 */
int clock_gettime_soloader(clockid_t clock_id, struct timespec * tp);
```

#### Función `__system_property_get_soloader()` (Líneas 90-94)
```c
/**
 * @brief Emulación de la consulta de propiedades de sistema de Android (`__system_property_get`).
 *
 * Escribe `"psvita"` en el búfer de destino.
 *
 * @param[in]  name Nombre de la propiedad (ej. `"ro.build.version.release"`).
 * @param[out] value Búfer de salida.
 *
 * @return Longitud de la cadena devuelta (7 bytes).
 */
int __system_property_get_soloader(const char *name, char *value);
```

---

### 2.3 Operaciones Atómicas emuladas (Líneas 118-137)

```c
/** @brief Decrementa atómicamente un entero en memoria. */
int __atomic_dec(volatile int *ptr);

/** @brief Incrementa atómicamente un entero en memoria. */
int __atomic_inc(volatile int *ptr);

/** @brief Intercambia atómicamente el valor de un entero en memoria (`Compare-And-Swap`). */
int __atomic_swap(int new_value, volatile int *ptr);

/** @brief Intercambia atómicamente un valor si coincide con el valor esperado. Retorna 0 en caso de éxito. */
int __atomic_cmpxchg(int old_value, int new_value, volatile int* ptr);
```
