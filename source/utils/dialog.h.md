# Documentación Técnica: `source/utils/dialog.h`

**Archivo Origen:** [`source/utils/dialog.h`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/dialog.h)  
**Módulo:** Utilities / Dialog Management  
**Propósito:** Cabecera con las declaraciones de funciones para diálogos nativos de mensaje de error y teclado IME en la PS Vita.

---

## 1. Resumen de Comentarios `//` y Funciones

| Línea | Comentario Original `//` | Función Asociada / Ámbito |
| :--- | :--- | :--- |
| **36** | `#endif // SOLOADER_DIALOG_H` | Guardas de inclusión `#ifndef` |

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Guardas de Inclusión Header Guard (Línea 36)

#### Comentario Original (`//`):
```c
#endif // SOLOADER_DIALOG_H
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @file dialog.h
 * @brief Declaraciones públicas de funciones de diálogo de UI nativa para el cargador SoLoader.
 */
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Indica el final de la guarda de compilación `#ifndef SOLOADER_DIALOG_H`.
- **Razón del comentario / Justificación Técnica**: Garantiza que las declaraciones del archivo de cabecera `dialog.h` solo sean procesadas una vez por el preprocesador de C/C++, evitando errores de redefinición de símbolos durante la compilación.

---

## 3. Declaraciones de Funciones en Doxygen

```c
/**
 * @brief Inicializa y muestra el teclado nativo IME de PS Vita.
 * 
 * @param[in] title Texto del título mostrado en la ventana del IME.
 * @param[in] initial_text Texto inicial precargado.
 * @return 0 si la inicialización fue exitosa, o código de error nativo de Vita.
 */
int init_ime_dialog(const char *title, const char *initial_text);

/**
 * @brief Obtiene el texto escrito por el usuario en el teclado IME y restaura los analógicos.
 * 
 * @return Puntero a la cadena UTF-8 con el texto o NULL si el diálogo no ha finalizado.
 */
char *get_ime_dialog_result(void);

/**
 * @brief Inicializa un diálogo nativo de mensaje simple (OK).
 * 
 * @param[in] msg Mensaje a presentar al usuario.
 * @return 0 si la inicialización fue exitosa.
 */
int init_msg_dialog(const char *msg);

/**
 * @brief Comprueba si el usuario aceptó el diálogo de mensaje.
 * 
 * @return 1 si el usuario presionó OK y se cerró el diálogo, 0 en otro caso.
 */
int get_msg_dialog_result(void);

/**
 * @brief Despliega un mensaje de error crítico en pantalla y detiene la aplicación.
 * 
 * @param[in] fmt Cadena con formato printf.
 */
void fatal_error(const char *fmt, ...) __attribute__((noreturn));
```
