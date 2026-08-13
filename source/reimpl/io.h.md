# Documentación Técnica: `source/reimpl/io.h`

**Archivo Origen:** [`source/reimpl/io.h`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/io.h)  
**Módulo:** Reimplementation / File I/O Header  
**Propósito:** Definiciones de estructuras `stat64_bionic` y `dirent64_bionic`, constantes de tipo de archivo y prototipos para las funciones de E/S emuladas.

---

## 1. Resumen de Comentarios `//` y Funciones

| Línea | Comentario Original `//` | Función / Ámbito |
| :--- | :--- | :--- |
| **63** | `int16_t d_ino; // 2 bytes // offset 0x0` | `struct dirent64_bionic` |
| **64** | `int64_t d_off; // 8 bytes // offset 0x2` | `struct dirent64_bionic` |
| **65** | `uint64_t d_reclen; // 8 bytes // 0xA` | `struct dirent64_bionic` |
| **66** | `unsigned char d_type; // 1 byte // offset 0x12` | `struct dirent64_bionic` |
| **67** | `char d_name[256]; // 256 bytes // offset 0x13` | `struct dirent64_bionic` |
| **89-91** | `// Cache-handle-safe wrappers for the small read-only file cache in io.c...` | Wrappers de `fcache` |
| **125** | `#endif // SOLOADER_IO_H` | Guardas de inclusión `#ifndef` |

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Estructura `dirent64_bionic` y Alineación Binaria (Líneas 62-68)

#### Comentarios Originales (`//`):
```c
typedef struct __attribute__((__packed__)) dirent64_bionic {
    int16_t d_ino; // 2 bytes // offset 0x0
    int64_t d_off; // 8 bytes // offset 0x2
    uint64_t d_reclen; // 8 bytes // 0xA
    unsigned char d_type; // 1 byte // offset 0x12
    char d_name[256]; // 256 bytes // offset 0x13
} dirent64_bionic;
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @struct dirent64_bionic
 * @brief Estructura binaria de entrada de directorio (`dirent`) coincidente con Android Bionic en arquitectura ARMv7.
 *
 * @var dirent64_bionic::d_ino
 * @brief Número de inodo (2 bytes, desplazamiento `0x0`).
 * @var dirent64_bionic::d_off
 * @brief Desplazamiento del siguiente dirent (8 bytes, desplazamiento `0x2`).
 * @var dirent64_bionic::d_reclen
 * @brief Longitud de este registro (8 bytes, desplazamiento `0xA`).
 * @var dirent64_bionic::d_type
 * @brief Tipo de entrada de archivo (`DT_DIR`, `DT_REG`, etc.) (1 byte, desplazamiento `0x12`).
 * @var dirent64_bionic::d_name
 * @brief Nombre del archivo terminado en nulo (256 bytes, desplazamiento `0x13`).
 */
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Define la disposición exacta de memoria (`__packed__`) de la estructura de directorio que espera la librería compilada de Android.
- **Razón del comentario / Justificación Técnica**: En C, las estructuras en Newlib (PS Vita) y Bionic (Android) difieren en el tamaño de los campos y alineación de memoria (padding). Si se pasa un puntero de la estructura `dirent` nativa de la Vita a la función de lectura de directorio del motor de Android, el motor leería el nombre del archivo en la dirección incorrecta de la memoria, provocando fallos de acceso o nombres de archivo corruptos.

---

### 2.2 Declaraciones de Funciones Wrappers de stdio y POSIX

```c
/** @brief Abre un archivo devolviendo un descriptor de archivo POSIX remapeado. */
int open_soloader(const char * path, int oflag, ...);

/** @brief Abre un archivo devolviendo un puntero FILE* compatible con fcache. */
FILE * fopen_soloader(const char * filename, const char * mode);

/** @brief Obtiene información de estado de un archivo en formato stat64_bionic. */
int stat_soloader(const char * path, stat64_bionic * buf);

/** @brief Lee una entrada de directorio convertida a formato dirent64_bionic. */
struct dirent64_bionic * readdir_soloader(DIR *dir);
```
