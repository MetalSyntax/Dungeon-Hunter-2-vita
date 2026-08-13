# Documentación Técnica: `source/utils/utils.h`

**Archivo Origen:** [`source/utils/utils.h`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/utils.h)  
**Módulo:** Utilities / Helper Routines Header  
**Propósito:** Declaraciones y documentación pública para la suite de funciones auxiliares del cargador.

---

## 1. Resumen de Comentarios `//` y Funciones

| Línea | Comentario Original `//` | Función Asociada / Ámbito |
| :--- | :--- | :--- |
| **200** | `#endif // SOLOADER_UTILS_H` | Guardas de inclusión `#ifndef` |

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Guardas de Inclusión Header Guard (Línea 200)

#### Comentario Original (`//`):
```c
#endif // SOLOADER_UTILS_H
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @file utils.h
 * @brief Funciones de utilidad comunes para manipulación de archivos, tiempo, cadenas y stubs de importación.
 */
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Finaliza la definición condicional `#ifndef SOLOADER_UTILS_H`.
- **Razón del comentario / Justificación Técnica**: Protege contra inclusiones repetidas en múltiples unidades de traducción.

---

## 3. Bloques Doxygen para Funciones Declaradas en `utils.h`

```c
/**
 * @brief Obtiene la marca de tiempo Unix actual en milisegundos.
 * @return Milisegundos desde Epoch.
 */
uint64_t current_timestamp_ms();

/**
 * @brief Copia un archivo de origen a destino abriendo búferes dinámicos.
 * @param[in] path Ruta del archivo origen.
 * @param[in] destination Ruta del archivo destino.
 * @return `true` en caso de éxito.
 */
bool file_copy(const char * path, const char * destination);

/**
 * @brief Comprueba si existe un archivo en el sistema de archivos.
 * @param[in] path Ruta a consultar.
 * @return `true` si existe.
 */
bool file_exists(const char * path);

/**
 * @brief Lee el contenido completo de un archivo a un búfer.
 * @param[in] path Ruta del archivo.
 * @param[out] buffer Puntero a asignar.
 * @param[out] size Tamaño del archivo leal.
 * @return `true` si se leyó correctamente.
 */
bool file_load(const char * path, uint8_t ** buffer, size_t * size);

/**
 * @brief Crea directorios padre recursivamente.
 */
bool file_mkpath(const char * path, mode_t mode);

/**
 * @brief Guarda un búfer de datos en un archivo.
 */
bool file_save(const char * path, const uint8_t * buffer, size_t size);

/**
 * @brief Obtiene el tamaño en bytes de un archivo.
 */
size_t file_size(const char * path);

/**
 * @brief Calcula el hash SHA1 de un archivo.
 */
char * file_sha1sum(const char * path);

/**
 * @brief Verifica si una ruta es un directorio.
 */
bool is_dir(const char * path);

/**
 * @brief Consulta la presencia de un módulo del kernel por su nombre.
 */
bool module_loaded(const char * name);

/** @brief Stub que retorna 0. */
int ret0(void);
/** @brief Stub que retorna 1. */
int ret1(void);
/** @brief Stub que retorna -1. */
int retminus1(void);

/**
 * @brief Reemplaza todas las ocurrencias de una subcadena en una cadena.
 */
void str_replace(char ** str, const char * needle, const char * replacement);

/**
 * @brief Elimina las ocurrencias de una subcadena.
 */
void str_remove(char * str, const char * needle);

/**
 * @brief Comprueba si una cadena comienza con un prefijo.
 */
bool str_starts_with(const char * str, const char * prefix);

/**
 * @brief Comprueba si una cadena termina con un sufijo.
 */
bool str_ends_with(const char * str, const char * suffix);

/**
 * @brief Calcula el hash SHA1 de un búfer de memoria.
 */
char * str_sha1sum(const char * str, size_t size);
```
