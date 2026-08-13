# Documentación Técnica: `source/reimpl/egl.c`

**Archivo Origen:** [`source/reimpl/egl.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/egl.c)  
**Módulo:** Reimplementation / EGL API Bridge  
**Propósito:** Emulación y stubs de la API Khronos EGL (Khronos Native Window System Interface) sobre VitaGL.

---

## 1. Resumen de Comentarios `//` y Funciones

| Línea | Comentario Original `//` | Función / Ámbito |
| :--- | :--- | :--- |
| **83** | `// ignored when creating the surface, return default` | `eglQuerySurface()` (Atributo `EGL_MULTISAMPLE_RESOLVE`) |
| **88** | `*value = 220 * EGL_DISPLAY_SCALING; // VITA DPI is 220` | `eglQuerySurface()` (Atributos DPI) |
| **91** | `// Please don't ask why * EGL_DISPLAY_SCALING, the document says it` | `eglQuerySurface()` (Atributo Aspect Ratio) |
| **98** | `// ignored when creating the surface, return default` | `eglQuerySurface()` (Atributo `EGL_VG_COLORSPACE`) |
| **102**| `// ignored when creating the surface, return default` | `eglQuerySurface()` (Atributo `EGL_VG_ALPHA_FORMAT`) |
| **277**| `// Just something that is a valid pointer which can be freed later` | `eglCreateContext()` |
| **283**| `// Just something that is a valid pointer which can be freed later` | `eglCreateWindowSurface()` |

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Consulta de Atributos de Superficie (`eglQuerySurface`) (Líneas 53-115)

#### Comentarios Originales (`//`):
```c
// ignored when creating the surface, return default
*value = EGL_MULTISAMPLE_RESOLVE_DEFAULT;

*value = 220 * EGL_DISPLAY_SCALING; // VITA DPI is 220

// Please don't ask why * EGL_DISPLAY_SCALING, the document says it
*value = 960 / 544 * EGL_DISPLAY_SCALING;
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @brief Consulta las propiedades y atributos de la superficie EGL emulada de la PS Vita.
 *
 * @param[in]  dpy Descriptor de la pantalla EGL (`EGLDisplay`).
 * @param[in]  eglSurface Descriptor de la superficie (`EGLSurface`).
 * @param[in]  attribute Atributo EGL a consultar (`EGL_WIDTH`, `EGL_HEIGHT`, `EGL_HORIZONTAL_RESOLUTION`, etc.).
 * @param[out] value Puntero a la variable donde se devolverá el valor numérico.
 *
 * @note La densidad de pantalla de la pantalla OLED/LCD original de la PS Vita es de 220 DPI.
 *       Las especificaciones EGL requieren multiplicar los valores de DPI y aspecto por `EGL_DISPLAY_SCALING` (10000).
 *
 * @return `EGL_TRUE` si el atributo es válido, `EGL_FALSE` si no es soportado.
 */
EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface eglSurface,
                           EGLint attribute, EGLint *value);
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Responde a las peticiones del motor de juego sobre la resolución física (960x544), DPI horizontal/vertical (220 DPI) y relación de aspecto de la pantalla de la Vita.
- **Razón del comentario / Justificación Técnica**: En Android, el motor consulta a EGL la densidad de píxeles (`EGL_HORIZONTAL_RESOLUTION`) para escalar la interfaz de usuario (HUD, fuentes y botones). En las especificaciones de EGL, los valores de resolución física no se entregan en flotantes directos sino escalados por una constante entera (`EGL_DISPLAY_SCALING = 10000`). Retornar 220 DPI asegura que los textos de Dungeon Hunter 2 se muestren en el tamaño correcto en la Vita.

---

### 2.2 Creación de Contextos y Superficies Stub (`eglCreateContext` / `eglCreateWindowSurface`) (Líneas 274-286)

#### Comentarios Originales (`//`):
```c
EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config,
                            EGLContext share_context,
                            const EGLint *attrib_list) {
    // Just something that is a valid pointer which can be freed later
    return strdup("ctx");
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                  void * win, const EGLint *attrib_list) {
    // Just something that is a valid pointer which can be freed later
    return strdup("surface");
}
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @brief Crea un contexto gráfico EGL dummy para satisfacer al motor de Android.
 * 
 * En PS Vita, el contexto gráfico real de OpenGL ES es gestionado directamente por VitaGL (`gl_init()`).
 * Esta función retorna un puntero de memoria válido que puede ser destruido posteriormente con `free()`.
 * 
 * @return Puntero no nulo a un objeto `EGLContext` ficticio.
 */
EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint *attrib_list);

/**
 * @brief Crea una superficie de ventana EGL dummy.
 * 
 * @return Puntero no nulo a un objeto `EGLSurface` ficticio.
 */
EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, void * win, const EGLint *attrib_list);
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Devuelve cadenas asignadas dinámicamente (`strdup("ctx")` y `strdup("surface")`) en lugar de manejar verdaderos contextos de ventana nativos de Android.
- **Razón del comentario / Justificación Técnica**: En Android, las aplicaciones deben crear una ventana nativa `ANativeWindow` y un contexto EGL formal para poder dibujar con OpenGL ES. En la PS Vita, VitaGL gestiona directamente la memoria de marco de framebuffer (`SceDisplay`) sin necesidad de EGL real. Devolver un puntero válido evita que la comprobación `if (ctx == EGL_NO_CONTEXT)` en el motor aborte el juego.
