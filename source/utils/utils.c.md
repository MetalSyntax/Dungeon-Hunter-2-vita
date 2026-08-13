# Documentación Técnica: `source/utils/utils.c`

**Archivo Origen:** [`source/utils/utils.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/utils.c)  
**Módulo:** Utilities / Helper Routines  
**Propósito:** Funciones auxiliares generales para el sistema de archivos (copia, carga en RAM, hashing SHA1, creación recursiva de directorios), cálculo de marcas de tiempo en milisegundos, manipulación de cadenas de caracteres (`str_replace`, `str_remove`, `str_starts_with`, `str_ends_with`) y funciones stub de retorno estático (`ret0`, `ret1`, `retminus1`).

---

## 1. Resumen de Comentarios `//` y Funciones

*Nota: Este archivo no contiene comentarios de línea individuales que inicien por `//` dentro del código ejecutable, salvo las directivas condicionales del preprocesador `#ifdef USE_SCELIBC_IO`. Se documentan a continuación todas las funciones implementadas con su correspondiente bloque Doxygen y justificación técnica.*

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Funciones de Tiempo y Archivos

#### Función `current_timestamp_ms()` (Líneas 29-33)
```c
/**
 * @brief Obtiene la marca de tiempo Unix actual expresada en milisegundos.
 * 
 * @return Número de milisegundos transcurridos desde el 1 de enero de 1970 (Epoch UNIX).
 */
uint64_t current_timestamp_ms();
```

#### Función `file_copy()` (Líneas 35-65)
```c
/**
 * @brief Copia un archivo desde la ruta de origen a la de destino.
 * 
 * Crea automáticamente los directorios padre de destino si no existen.
 * 
 * @param[in] path Ruta completa del archivo de origen.
 * @param[in] destination Ruta completa del archivo de destino.
 * 
 * @return `true` en caso de éxito, `false` si falla la lectura o escritura.
 */
bool file_copy(const char * path, const char * destination);
```

#### Función `file_exists()` (Líneas 67-70)
```c
/**
 * @brief Comprueba la existencia de un archivo en el sistema de archivos de PS Vita.
 * 
 * Usa la llamada de kernel `sceIoGetstat`.
 * 
 * @param[in] path Ruta del archivo.
 * @return `true` si el archivo existe, `false` en caso contrario.
 */
bool file_exists(const char * path);
```

#### Función `file_load()` (Líneas 72-138)
```c
/**
 * @brief Carga completamente un archivo de disco en un búfer de memoria RAM recién asignado.
 * 
 * @details Si está definido `USE_SCELIBC_IO`, utiliza el puente `sceLibcBridge_fopen` para 
 *          evitar sobrecostos de E/S de Newlib.
 * 
 * @param[in]  path Ruta del archivo.
 * @param[out] buffer Dirección donde se almacenará el puntero a la memoria asignada (`malloc`).
 * @param[out] size Tamaño en bytes del archivo cargado.
 * 
 * @return `true` si el archivo se cargó correctamente.
 */
bool file_load(const char * path, uint8_t ** buffer, size_t * size);
```

#### Función `file_mkpath()` (Líneas 140-162)
```c
/**
 * @brief Crea de forma recursiva todos los directorios intermedios en una ruta.
 * 
 * @param[in] path Ruta del archivo o directorio objetivo.
 * @param[in] mode Permisos POSIX para los nuevos directorios (ej. `0755`).
 * 
 * @return `true` si la ruta completa de directorios fue creada o ya existía.
 */
bool file_mkpath(const char * path, mode_t mode);
```

#### Función `file_sha1sum()` (Líneas 224-238)
```c
/**
 * @brief Calcula el hash SHA1 en texto hexadecimal de un archivo.
 * 
 * @param[in] path Ruta del archivo.
 * @return Cadena de 40 caracteres hexadecimales (debe ser liberada con `free()`), o `NULL`.
 */
char * file_sha1sum(const char * path);
```

---

### 2.2 Funciones de Verificación de Módulos del Kernel de PS Vita (Líneas 251-256)

```c
/**
 * @brief Comprueba si un módulo del kernel de PS Vita (como `kubridge`) está cargado.
 * 
 * Invoca la función interna del kernel `_vshKernelSearchModuleByName`.
 * 
 * @param[in] name Nombre del módulo (ej. `"kubridge"`).
 * @return `true` si el módulo se encuentra activo en memoria.
 */
bool module_loaded(const char * name);
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Declara externamente la función indocumentada de VSH `_vshKernelSearchModuleByName` para consultar si el plugin de kernel `kubridge.skprx` fue cargado previamente en la PS Vita mediante `taiHEN` / `config.txt`.
- **Razón del comentario / Justificación Técnica**: El cargador SoLoader requiere funciones especiales de kernel provistas únicamente por `kubridge` para la asignación de memoria ejecutable del ELF de Android. Si `kubridge` no está presente, la aplicación debe fallar inmediatamente y notificar al usuario.

---

### 2.3 Funciones Stub / Dummy (Líneas 258-268)

```c
/**
 * @brief Función stub que no realiza ninguna operación y retorna 0.
 * @return 0
 */
int ret0(void);

/**
 * @brief Función stub que no realiza ninguna operación y retorna 1.
 * @return 1
 */
int ret1(void);

/**
 * @brief Función stub que no realiza ninguna operación y retorna -1.
 * @return -1
 */
int retminus1(void);
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Retornan números enteros constantes.
- **Razón del comentario / Justificación Técnica**: Se utilizan en la tabla de importaciones de `dynlib.c` o en parches de ensamblador ARM para anular funciones del motor de juego de Android incompatibles o innecesarias en PS Vita (como llamadas de analíticas de Google Play Services, comprobaciones de licencias DRM o servicios de publicidad).
