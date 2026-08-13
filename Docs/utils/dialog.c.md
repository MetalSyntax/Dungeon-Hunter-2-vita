# Documentación Técnica: `source/utils/dialog.c`

**Archivo Origen:** [`source/utils/dialog.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/dialog.c)  
**Módulo:** Utilities / Dialog Management  
**Propósito:** Gestión de diálogos nativos de la PlayStation Vita (SceImeDialog para entrada de texto y SceMsgDialog para mensajes de error fatales o alertas del sistema).

---

## 1. Resumen de Comentarios `//` y Funciones

| Línea | Comentario Original `//` | Función Asociada / Ámbito |
| :--- | :--- | :--- |
| **99** | `// For some reason analog stick stops working after ime` | `get_ime_dialog_result()` |

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Función `get_ime_dialog_result()` (Línea 89-103)

#### Comentario Original (`//`):
```c
// For some reason analog stick stops working after ime
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @brief Obtiene el resultado introducido por el usuario en el teclado en pantalla (IME).
 *
 * Si el diálogo IME ha finalizado y el usuario confirmó con el botón Enter, convierte 
 * el búfer UTF-16 a UTF-8. Además, restablece el modo de muestreo del control analógico.
 *
 * @note Reestablece explícitamente `sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE)`
 *       debido a un comportamiento del firmware de la PS Vita donde el sampling mode 
 *       del stick analógico se desactiva tras cerrar el teclado IME nativo.
 *
 * @return Ponte al búfer UTF-8 con el texto ingresado (`char *`), o `NULL` si el diálogo
 *         aún no ha concluido.
 */
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: La función comprueba si el diálogo de entrada de texto nativo de la Vita (`SceImeDialog`) ha terminado (`SCE_COMMON_DIALOG_STATUS_FINISHED`). Si es así y el usuario presionó la tecla Enter (`SCE_IME_DIALOG_BUTTON_ENTER`), convierte el resultado guardado en UTF-16 a la cadena UTF-8 `ime_input_text_utf8` usando `_utf16_to_utf8()`. Inmediatamente después llama a `sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE)`.
- **Razón del comentario / Justificación Técnica**: En la biblioteca del SDK nativo de la PS Vita (`vitasdk`), al invocar y cerrar el diálogo IME del sistema (`sceImeDialogTerm()`), el subsistema de entrada de la consola restablece por defecto el modo de control desactivando o limitando la lectura extendida de los joysticks analógicos. Sin llamar a `sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE)`, los sticks analógicos dejan de responder completamente en el juego tras usar el teclado (por ejemplo, al ingresar trucos o nombres de usuario).

---

### 2.2 Funciones Adicionales del Archivo (Documentadas en Doxygen)

#### Función `init_ime_dialog()` (Líneas 66-87)
```c
/**
 * @brief Inicializa el teclado en pantalla nativo (IME Dialog) de PS Vita.
 * 
 * Limpia los búferes UTF-16 y UTF-8, convierte el título y texto inicial a UTF-16, 
 * configura los parámetros del teclado (soporte de lenguajes, tipo de teclado latino)
 * e inicia el diálogo IME.
 * 
 * @param[in] title Título que se mostrará en el encabezado del teclado.
 * @param[in] initial_text Texto por defecto cargado en la caja de texto.
 * 
 * @return Código de retorno de `sceImeDialogInit()` (0 en caso de éxito).
 */
```

#### Función `fatal_error()` (Líneas 127-146)
```c
/**
 * @brief Muestra un mensaje de error fatal e interrumpe la ejecución del juego.
 * 
 * Formatea un mensaje de error estilo `printf`, asegura que VitaGL esté inicializado 
 * (`gl_init()`), despliega un diálogo nativo de mensaje OK (`init_msg_dialog()`) y procesa 
 * el intercambio de búferes gráficos (`gl_swap()`) hasta que el usuario presione OK. 
 * Finalmente finaliza el proceso de la consola mediante `sceKernelExitProcess(0)`.
 * 
 * @param[in] fmt Cadena de formato de estilo `printf`.
 * @param[in] ... Argumentos variables para el formato.
 */
```
