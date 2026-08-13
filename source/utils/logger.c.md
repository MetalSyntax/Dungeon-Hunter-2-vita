# Documentación Técnica: `source/utils/logger.c`

**Archivo Origen:** [`source/utils/logger.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/logger.c)  
**Módulo:** Utilities / Thread-Safe Multi-Output Logger  
**Propósito:** Sistema de registro de eventos (logging) seguro entre hilos de ejecución, formateo con colores ANSI para consola TTY/TTYBridge y salida simultánea limpia sin códigos ANSI a archivos de texto en `ux0:data/dungeon-hunter-2/logs/`.

---

## 1. Resumen de Comentarios `//` y Funciones

| Línea | Comentario Original `//` | Función Asociada / Ámbito |
| :--- | :--- | :--- |
| **32** | `// Buffer A is used to adjust the format string.` | `buffer_a` (Búfer estático) |
| **34** | `// Buffer B is used to compile the final log using the updated format string.` | `buffer_b` (Búfer estático) |
| **36** | `// Buffer C is used for file output (no colors).` | `buffer_c` (Búfer estático) |

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Búferes Estáticos de Formateo de Logs (Líneas 32-38)

#### Comentarios Originales (`//`):
```c
// Buffer A is used to adjust the format string.
static char buffer_a[2048];
// Buffer B is used to compile the final log using the updated format string.
static char buffer_b[2048];
// Buffer C is used for file output (no colors).
static char buffer_c[2048];
static char buffer_d[2048];
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @var buffer_a
 * @brief Búfer intermedio para agregar prefijos con colores ANSI y etiquetas a la cadena de formato.
 *
 * @var buffer_b
 * @brief Búfer final que almacena el mensaje de log procesado con colores ANSI para salida por consola (`sceClibPrintf`).
 *
 * @var buffer_c
 * @brief Búfer intermedio para formatear la cadena sin caracteres de escape ANSI para archivos.
 *
 * @var buffer_d
 * @brief Búfer final limpio formateado para ser escrito directamente en el archivo de texto en disco (`log_file`).
 */
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Define cuatro búferes estáticos globales de 2048 bytes utilizados por `_log_print()`. `buffer_a` prepende la etiqueta de nivel de log (ej. `[info]`, `[error]`) junto a secuencias de escape de color ANSI (como `\x1B[32m` para verde). Luego `buffer_b` compila los argumentos variables mediante `vsnprintf`. Por separado, `buffer_c` y `buffer_d` realizan el mismo proceso pero excluyendo los códigos de colores ANSI para que la salida en el archivo de texto en disco (`log_XXX.txt`) sea texto plano legible.
- **Razón del comentario / Justificación Técnica**: En el desarrollo en la PS Vita, visualizar logs en tiempo real vía consola USB/TTY requiere códigos de colores ANSI para diferenciar rápidamente advertencias y errores. Sin embargo, guardar secuencias ANSI dentro de un archivo de texto en la tarjeta de memoria de la PS Vita distorsiona la lectura del log en editores de texto estándar. El uso de búferes dedicados resuelve ambos objetivos simultáneamente.

---

### 2.2 Función `_log_print()` (Líneas 40-116)

```c
/**
 * @brief Imprime mensajes de log formateados a la consola de depuración y al archivo de log activo en disco.
 *
 * @details La primera invocación inicializa de forma perezosa (lazy init) un Mutex liviano nativo de Vita (`_log_mutex`),
 *          crea el directorio de almacenamiento (`ux0:data/dungeon-hunter-2/logs`) y genera un nuevo archivo secuencial
 *          `log_000.txt`, `log_001.txt`, etc., para evitar sobrescribir logs de sesiones previas.
 *
 * @param[in] t Tipo/Nivel de log (`LT_DEBUG`, `LT_INFO`, `LT_WARN`, `LT_ERROR`, `LT_FATAL`, `LT_SUCCESS`, `LT_WAIT`).
 * @param[in] fmt Cadena con formato estilo `printf`.
 * @param[in] ... Argumentos variables a formatear.
 */
void _log_print(int t, const char* fmt, ...);
```

#### Razón Técnica y Protección Multihilo:
La función utiliza una variable atómica `_log_mutex_ready` y `sceKernelCreateLwMutex` para asegurar que las llamadas desde hilos secundarios del motor (como hilos de audio, IO o renderizado) no provoquen condiciones de carrera (race conditions) ni corrupción de memoria en los búferes compartidos.
