# Documentación Técnica: `source/reimpl/log.c`

**Archivo Origen:** [`source/reimpl/log.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/log.c)  
**Módulo:** Reimplementation / Android Logging System (`liblog`)  
**Propósito:** Reimplementación de las funciones de registro de Android NDK (`__android_log_write`, `__android_log_print`, `__android_log_vprint`, `__android_log_assert`) redirigiendo su salida al sistema de logging nativo de SoLoader (`logger.h`).

---

## 1. Resumen de Comentarios `//` y Funciones

*Nota: Este archivo utiliza una macro `#define print_common` para consolidar el formateo de prioridades de registro de Android (`ANDROID_LOG_*`) a las macros nativas de Vita (`l_info`, `l_warn`, `l_error`, `l_debug`). Se documenta la lógica y sus funciones.*

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Macro de Redirección `print_common` (Líneas 15-35)

```c
/**
 * @def print_common
 * @brief Macro de procesamiento de nivel de prioridad de log de Android.
 *
 * Mapea las prioridades de Android NDK a las macros de SoLoader:
 * - `ANDROID_LOG_INFO` -> `l_info`
 * - `ANDROID_LOG_WARN` -> `l_warn`
 * - `ANDROID_LOG_ERROR` / `FATAL` -> `l_error`
 * - Otros niveles -> `l_debug`
 */
```

---

### 2.2 Funciones de Registro de Android (Líneas 37-84)

#### Función `__android_log_write()` (Líneas 37-40)
```c
/**
 * @brief Escribe una cadena de texto simple al log con una etiqueta y prioridad.
 *
 * @param[in] prio Prioridad del log de Android (`android_LogPriority`).
 * @param[in] tag Etiqueta identificadora del módulo (ej. `"Gameloft"`).
 * @param[in] text Mensaje de texto a registrar.
 *
 * @return Siempre 0.
 */
int __android_log_write(int prio, const char* tag, const char* text);
```

#### Función `__android_log_print()` (Líneas 42-53)
```c
/**
 * @brief Formatea e imprime una cadena estilo `printf` al log de Android.
 */
int __android_log_print(int prio, const char* tag, const char* fmt, ...);
```

#### Función `__android_log_assert()` (Líneas 65-84)
```c
/**
 * @brief Procesa un fallo de aserción nativo de Android.
 *
 * Muestra el mensaje de aserción fallida como log fatal (`l_fatal`) e interrumpe
 * la ejecución inmediatamente invocando `abort()`.
 *
 * @param[in] cond Condición evaluada.
 * @param[in] tag Etiqueta del sistema.
 * @param[in] fmt Cadena con formato del fallo.
 */
void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...);
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Intercepta todas las llamadas de depuración que la librería nativa de Android (`libDungeonHunter2.so`) realiza a través del sistema de registro estándar de Android (`liblog.so`).
- **Razón del comentario / Justificación Técnica**: La librería shared objeto de Android depende de las funciones exportadas por `liblog.so` (`__android_log_print`). En la Vita no existe el daemon `logcat` de Android. Redirigir estas llamadas al subsistema `logger.c` permite que todos los mensajes internos del motor del juego aparezcan reflejados en la consola TTY y en los archivos `log_XXX.txt` en la Vita.
