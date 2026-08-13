# Documentación Técnica: `source/utils/logger.h`

**Archivo Origen:** [`source/utils/logger.h`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/logger.h)  
**Módulo:** Utilities / Logger API  
**Propósito:** Definición de niveles de log, macros de depuración en tiempo de compilación (`DEBUG_SOLOADER`) y prototipos de la API de registro.

---

## 1. Resumen de Comentarios `//` y Funciones

| Línea | Comentario Original `//` | Función Asociada / Ámbito |
| :--- | :--- | :--- |
| **52** | `#endif // SOLOADER_LOGGER_H` | Guardas de inclusión `#ifndef` |

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Guardas de Inclusión Header Guard (Línea 52)

#### Comentario Original (`//`):
```c
#endif // SOLOADER_LOGGER_H
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @file logger.h
 * @brief Interfaz pública del sistema de registros (logging) de SoLoader.
 */
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Finaliza la guarda `#ifndef SOLOADER_LOGGER_H`.
- **Razón del comentario / Justificación Técnica**: Mantiene la integridad del preprocesador al evitar inclusiones duplicadas de la cabecera.

---

## 3. Macros y Funciones de Logging (Documentación Doxygen)

```c
/**
 * @name Niveles de Logging
 * @{
 */
#define LT_DEBUG   0  /**< Mensajes de depuración detallada. */
#define LT_INFO    1  /**< Mensajes informativos generales. */
#define LT_WARN    2  /**< Advertencias sobre comportamientos anómalos pero no fatales. */
#define LT_ERROR   3  /**< Errores de ejecución recuperables. */
#define LT_FATAL   4  /**< Errores fatales irrecuperables (conducen al cierre de la app). */
#define LT_SUCCESS 5  /**< Mensajes de éxito en operaciones clave. */
#define LT_WAIT    6  /**< Mensajes de espera u operaciones en curso. */
/** @} */

#ifdef DEBUG_SOLOADER
/** @brief Emite un mensaje de nivel DEBUG cuando `DEBUG_SOLOADER` está definido. */
#define l_debug(...)   _log_print(LT_DEBUG,   __VA_ARGS__)
/** @brief Emite un mensaje de nivel INFO cuando `DEBUG_SOLOADER` está definido. */
#define l_info(...)    _log_print(LT_INFO,    __VA_ARGS__)
/** @brief Emite un mensaje de nivel WARN cuando `DEBUG_SOLOADER` está definido. */
#define l_warn(...)    _log_print(LT_WARN,    __VA_ARGS__)
/** @brief Emite un mensaje de nivel SUCCESS cuando `DEBUG_SOLOADER` está definido. */
#define l_success(...) _log_print(LT_SUCCESS, __VA_ARGS__)
/** @brief Emite un mensaje de nivel WAIT cuando `DEBUG_SOLOADER` está definido. */
#define l_wait(...)    _log_print(LT_WAIT,    __VA_ARGS__)
#else
#define l_debug(...)
#define l_info(...)
#define l_warn(...)
#define l_success(...)
#define l_wait(...)
#endif

/** @brief Emite un mensaje de error (siempre activo en compilaciones release). */
#define l_error(...)   _log_print(LT_ERROR,   __VA_ARGS__)
/** @brief Emite un mensaje de error fatal (siempre activo en compilaciones release). */
#define l_fatal(...)   _log_print(LT_FATAL,   __VA_ARGS__)

/**
 * @brief Función interna de formateo e impresión de registros.
 * 
 * @param[in] t Nivel de log (`LT_*`).
 * @param[in] fmt Cadena de formato estilo `printf`.
 */
void _log_print(int t, const char* fmt, ...) __attribute__ ((format (printf, 2, 3)));
```
