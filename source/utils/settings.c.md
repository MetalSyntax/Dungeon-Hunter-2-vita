# Documentación Técnica: `source/utils/settings.c`

**Archivo Origen:** [`source/utils/settings.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/settings.c)  
**Módulo:** Utilities / Configurator Settings  
**Propósito:** Carga, guardado y restauración de valores de configuración leídos desde el archivo `config.txt` generado por la aplicación configuradora.

---

## 1. Resumen de Comentarios `//` y Funciones

*Nota: Este archivo no contiene comentarios explícitos de tipo `//` en su código fuente original, utilizando comentarios multilínea `/* */` en su encabezado de derechos de autor. Se documentan a continuación todas sus funciones en formato Doxygen con su análisis técnico.*

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Macro `CONFIG_FILE_PATH` (Líneas 13-14)

```c
/**
 * @def CONFIG_FILE_PATH
 * @brief Ruta completa del archivo de texto que almacena las opciones de configuración del cargador.
 *        Se evalúa como `DATA_PATH "config.txt"`.
 */
#define CONFIG_FILE_PATH DATA_PATH"config.txt"
```

---

### 2.2 Funciones de Gestión de Ajustes (Líneas 18-48)

#### Función `settings_reset()` (Líneas 18-21)
```c
/**
 * @brief Restablece las variables globales de configuración a sus valores por defecto.
 *
 * Configura `setting_sampleSetting = 1` y `setting_sampleSetting2 = true`.
 */
void settings_reset();
```

#### Función `settings_load()` (Líneas 23-38)
```c
/**
 * @brief Carga las opciones de configuración desde `ux0:data/dungeon-hunter-2/config.txt`.
 *
 * @details Primero llama a `settings_reset()` para garantizar un estado válido. Abre el archivo 
 *          de texto en modo lectura y escanea parejas de clave-valor (`%[^ ] %d\n`) para 
 *          asignar los valores a las variables correspondientes.
 */
void settings_load();
```

#### Función `settings_save()` (Líneas 40-48)
```c
/**
 * @brief Guarda la configuración actual en el archivo `config.txt`.
 *
 * Escribe las parejas clave-valor formateadas en disco y cierra el descriptor de archivo.
 */
void settings_save();
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Proporciona un mecanismo simple de serialización clave-valor en texto plano para persistir opciones de configuración que el usuario puede ajustar previamente en el configurador nativo (`app0:/configurator.bin`).
- **Razón del comentario / Justificación Técnica**: Evita codificar parámetros fijos en el binario principal y permite ajustar opciones de rendimiento (como calidad gráfica, resoluciones internas o comportamientos de control) sin necesidad de recompilar el cargador.
