# Documentación Técnica: `source/reimpl/errno.c`

**Archivo Origen:** [`source/reimpl/errno.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/errno.c)  
**Módulo:** Reimplementation / Errno Translation Layer  
**Propósito:** Capa de traducción bidireccional entre los códigos de error del sistema `errno` de Newlib (PS Vita SDK) y Bionic (Android libc).

---

## 1. Resumen de Comentarios `//` y Funciones

| Línea | Comentario Original `//` | Función / Ámbito |
| :--- | :--- | :--- |
| **70** | `// { EFTYPE, 0, "" }, // EFTYPE is only for newlib internal usage` | Tabla `errno_translation` |

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Exclusión de `EFTYPE` en la Tabla de Traducción (Línea 70)

#### Comentario Original (`//`):
```c
// { EFTYPE, 0, "" }, // EFTYPE is only for newlib internal usage
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @struct newlib_errno_to_bionic_errno
 * @brief Elemento de la tabla de mapeo de códigos de error de Newlib (Vita) a Bionic (Android).
 *
 * @note La constante `EFTYPE` (Inappropriate file type or format) está presente en Newlib para uso
 *       interno de la C library de la Vita, pero no posee un equivalente estándar directo dentro
 *       del espacio de números de `errno` de Bionic en Android, por lo que se omite deliberadamente
 *       de la tabla de mapeo dinámico.
 */
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Desactiva mediante comentarios la entrada para `EFTYPE` dentro de la lista estática `errno_translation[]`.
- **Razón del comentario / Justificación Técnica**: Newlib (la librería C estándar que utiliza VitaSDK) define códigos de error propietarios heredados de BSD como `EFTYPE`. Android Bionic está basado en los valores numéricos de `errno` de Linux ARM. Dado que la aplicación compilada para Android no reconoce `EFTYPE`, intentar mapearlo a Bionic generaría advertencias de compilación o códigos invalidados.

---

### 2.2 Funciones de Traducción de Errno (Líneas 114-156)

#### Función `__errno_soloader()` (Líneas 114-124)
```c
/**
 * @brief Reemplaza la llamada a `__errno()` de Android Bionic.
 *
 * Lee la variable global `errno` del entorno Newlib de la PS Vita, busca su valor correspondiente 
 * en la tabla de traducción y devuelve un puntero a la constante Bionic equivalente.
 *
 * @return Puntero al entero con el código de error en formato Android Bionic.
 */
int * __errno_soloader(void);
```

#### Función `strerror_soloader()` (Líneas 126-134)
```c
/**
 * @brief Convierte un código de error de Bionic a su descripción legible en texto (`strerror`).
 *
 * @param[in] error_number Código de error numérico de Android Bionic (`EACCES_BIONIC`, `ENOENT_BIONIC`, etc.).
 * @return Cadena constante con la descripción textual del error (ej. `"No such file or directory"`).
 */
char * strerror_soloader(int error_number);
```

#### Función `strerror_r_soloader()` (Líneas 136-156)
```c
/**
 * @brief Versión segura entre hilos de `strerror` (`strerror_r`) para Bionic.
 *
 * Copia la descripción textual del error en el búfer proporcionado por el usuario (`buf`).
 *
 * @param[in]  error_number Código de error Bionic.
 * @param[out] buf Búfer de destino.
 * @param[in]  buf_len Tamaño máximo del búfer.
 *
 * @return 0 en caso de éxito; `ERANGE_BIONIC` si el búfer es demasiado pequeño.
 */
int strerror_r_soloader(int error_number, char* buf, size_t buf_len);
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Intercepta la consulta de errores del motor. Cuando una función del motor comprueba `if (errno == ENOENT)` (donde `ENOENT` en Android es `2` y en Newlib es `2`, pero otros como `EAGAIN` varían: 11 en Android vs 11 en Newlib, `ENOMEM`: 12 vs 12, `EWOULDBLOCK`, etc.), traduce exactamente los números numéricos.
- **Razón del comentario / Justificación Técnica**: Aunque muchos valores de errno coinciden entre Linux y BSD, códigos de red (`ECONNREFUSED`, `ETIMEDOUT`, `EHOSTUNREACH`) y E/S asíncrona difieren sustancialmente en valor numérico entre Newlib (PS Vita) y Bionic (Android Linux ARM). Sin esta capa de conversión, las comprobaciones de error del motor de juego fallaban ruidosamente o tomaban ramas de código incorrectas tras llamadas al sistema.
