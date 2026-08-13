# Documentación Técnica: `source/utils/glutil.h`

**Archivo Origen:** [`source/utils/glutil.h`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/glutil.h)  
**Módulo:** Utilities / OpenGL ES Wrappers & Graphics Diagnostics  
**Propósito:** Prototipos y wrappers de interceptación de OpenGL ES 2.0 (PVR_PSP2 / VitaGL) para depuración de renderizado, corrección de viewport/letterbox, escalado de coordenadas de pantalla a espacio lógico 960x640 y análisis del problema de "enemigos invisibles".

---

## 1. Resumen de Comentarios `//` y Funciones

| Línea | Comentario Original `//` (Extracto) | Función / Ámbito |
| :--- | :--- | :--- |
| **17-22** | `// Thin wrappers around the real glCompileShader/glLinkProgram...` | `glCompileShader_soloader`, `glLinkProgram_soloader` |
| **26-34** | `// Diagnostic-only (DEBUG_SOLOADER-gated internally...` | `glShaderSource_soloader`, `glAttachShader_soloader`, `glUseProgram_soloader` |
| **39-43** | `// The engine's own GSInit loading screen clears to a flat aquamarine color...` | `glClearColor_soloader` |
| **46-58** | `// DH2's appInit() (out_ghidra.c) picks a hardcoded logical rendering-canvas...` | `glViewport_soloader` |
| **61-69** | `// Same letterbox remap as glViewport_soloader, applied to glScissor...` | `glScissor_soloader` |
| **72-80** | `// Inverts the glViewport_soloader letterbox: converts a touch coordinate...` | `glutil_screen_touch_to_logical` |
| **83-91** | `// Diagnostic for the "2D UI draws fine, 3D world is invisible...` | `gl_log_render_diag` |
| **93-101**| `// Thin wrappers that log a frame-numbered trace specifically when...` | `glEnable_soloader`, `glDisable_soloader` |
| **104-109**| `// Thin passthrough wrappers around glDrawArrays/glDrawElements...` | `glDrawArrays_soloader`, `glDrawElements_soloader` |
| **113-150**| `// Diagnostic for the invisible-enemy bug... 2026-08-06 extension...` | `GLNodeDrawState`, `gl_diag_reset_render_track`, `gl_diag_get_render_track` |
| **153-161**| `// Sanity-checks every matrix uploaded via glUniformMatrix4fv...` | `glUniformMatrix4fv_soloader` |
| **164-169**| `// Sibling check for plain vec4 uniforms...` | `glUniform4fv_soloader` |
| **172-188**| `// Invisible-enemy investigation, next candidate after opacity was ruled out...` | `glCompressedTexImage2D_soloader` |
| **193-214**| `// New lead for the invisible/near-transparent-enemy investigation...` | `glVertexAttrib4f_soloader`, `glVertexAttrib4fv_soloader` |
| **216-226**| `// Logs every real glDepthRangef call (args + frame)...` | `glDepthRangef_soloader` |
| **228-238**| `// DOWNSAMPLE_RENDER only: redirects every glBindFramebuffer...` | `glBindFramebuffer_soloader` |
| **245**   | `#endif // SOLOADER_GLUTIL_H` | Guardas de inclusión |

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Compilador y Enlazador de Shaders (`glCompileShader_soloader` / `glLinkProgram_soloader`) (Líneas 17-24)

#### Comentario Original (`//`):
```c
// Thin wrappers around the real glCompileShader/glLinkProgram that also check
// GL_COMPILE_STATUS/GL_LINK_STATUS and log the info log on failure -- wired
// into dynlib.c's import table in place of the raw GL entry points so we get
// a definitive yes/no on whether the engine's real GLSL (see shaders.pak)
// actually compiles against the real PVR_PSP2 GLSL ES compiler, instead of
// guessing from a flat/wrong-colored screen.
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @brief Wrapper de compilación de shaders GLSL que audita el estado de compilación y registra errores.
 *
 * @param[in] shader Identificador del objeto Shader de OpenGL ES.
 *
 * @details Verifica `GL_COMPILE_STATUS`. Si falla la compilación del shader en el driver `PVR_PSP2`,
 *          obtiene el `glGetShaderInfoLog` y lo escribe al log principal. Reemplaza la llamada directa
 *          a `glCompileShader` en la tabla de importaciones de `dynlib.c`.
 */
void glCompileShader_soloader(GLuint shader);

/**
 * @brief Wrapper de enlace de programas GLSL que audita `GL_LINK_STATUS`.
 *
 * @param[in] program Identificador del objeto Program de OpenGL ES.
 */
void glLinkProgram_soloader(GLuint program);
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Intercepta la compilación de shaders GLSL y enlace de programas. Tras llamar a la API real del driver, consulta los estados de compilación/enlace y si detecta fallos, extrae el log detallado de error del compilador nativo de PowerVR.
- **Razón del comentario / Justificación Técnica**: Evita adivinar por qué la pantalla se ve negra o con colores incorrectos. Permite verificar si los shaders originales del juego en `shaders.pak` son compatibles con el compilador GLSL ES del SDK de PS Vita (`PVR_PSP2`).

---

### 2.2 Reescalado Letterbox/Pillarbox de Viewport y Scissor (`glViewport_soloader` / `glScissor_soloader`) (Líneas 46-70)

#### Comentarios Originales (`//`):
```c
// DH2's appInit() (out_ghidra.c) picks a hardcoded logical rendering-canvas
// size purely from the screen width it's told about -- for our real width
// (960, matching its "960-wide device" bucket) that logical canvas is
// 960x640, and the ENTIRE engine (2D UI and 3D world alike) issues all its
// glViewport calls in that 960x640 space via glitch::createDevice(). The
// Vita's real surface is 960x544 -- 96px short of that assumed height,
// which is why 2D/3D content looks squashed/stretched. This wrapper
// letterboxes (pillarboxes) every glViewport call from that assumed 960x640
// logical space into a centered, aspect-correct sub-rectangle of the real
// 960x544 physical screen...

// Same letterbox remap as glViewport_soloader, applied to glScissor...
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @brief Remapea las llamadas `glViewport` del lienzo lógico (960x640) a la pantalla física centrada de la PS Vita (960x544).
 *
 * @param[in] x Coordenada X del viewport lógico.
 * @param[in] y Coordenada Y del viewport lógico.
 * @param[in] width Ancho del viewport lógico.
 * @param[in] height Alto del viewport lógico.
 *
 * @note Mantiene la relación de aspecto original ajustando verticalmente la salida para evitar la deformación visual.
 */
void glViewport_soloader(GLint x, GLint y, GLsizei width, GLsizei height);

/**
 * @brief Ajusta las regiones de recorte (`glScissor`) para coincidir con la transformación letterbox de `glViewport_soloader`.
 */
void glScissor_soloader(GLint x, GLint y, GLsizei width, GLsizei height);
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Dungeon Hunter 2 asume internamente un lienzo de 960x640 píxeles cuando detecta una pantalla de 960px de ancho. Sin embargo, la PS Vita posee una pantalla con resolución de 960x544 (96 píxeles más corta en alto). Estas funciones interceptan `glViewport` y `glScissor`, aplicando una transformación matemática para centrar y escalar de forma visualmente correcta la escena sin estirar la imagen.
- **Razón del comentario / Justificación Técnica**: La tabla de dimensiones internas del motor de juego está compilada estáticamente dentro del `.so` y no se expone en la tabla de símbolos dinámicos (`.dynsym`). Por lo tanto, remapear dinámicamente las llamadas GL en `dynlib.c` es la única solución limpia sin alterar la estructura del binario desensamblado.

---

### 2.3 Conversión de Coordenadas de Pantalla a Espacio Lógico (`glutil_screen_touch_to_logical`) (Líneas 72-81)

#### Comentario Original (`//`):
```c
// Inverts the glViewport_soloader letterbox: converts a touch coordinate in
// real physical-screen space (0..959, 0..543) into the engine's logical
// 960x640 space that nativeOnTouch's hit-testing actually operates in...
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @brief Convierte coordenadas táctiles físicas (960x544) al espacio lógico del motor (960x640).
 *
 * @param[in]  screen_x Coordenada X recibida por la pantalla táctil de la PS Vita.
 * @param[in]  screen_y Coordenada Y recibida por la pantalla táctil de la PS Vita.
 * @param[out] out_x Puntero donde se almacenará la coordenada X convertida al lienzo de 960x640.
 * @param[out] out_y Puntero donde se almacenará la coordenada Y convertida al lienzo de 960x640.
 *
 * @return 1 si el toque ocurrió dentro de la región visible activa; 0 si cayó en las barras negras del letterbox.
 */
int glutil_screen_touch_to_logical(int screen_x, int screen_y, int *out_x, int *out_y);
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Invierte la transformación matemática de `glViewport_soloader`. Toma un punto de contacto táctil de la pantalla frontal de la Vita y calcula a qué coordenadas del mapa lógico 960x640 corresponde.
- **Razón del comentario / Justificación Técnica**: Soluciona un fallo crítico donde los botones en pantalla (HUD táctil) no registraban las pulsaciones o requerían presionar fuera del botón visible.

---

### 2.4 Rastreo y Diagnóstico del Bug de "Enemigos Invisibles" (`GLNodeDrawState` y Wrappers Uniforms/Attribs) (Líneas 113-214)

#### Comentarios Originales (`//`):
```c
// Diagnostic for the invisible-enemy bug (DEBUG_SOLOADER-gated...)...
// 2026-08-06 extension: draw_calls/last_texture alone showed most enemies DO issue a real draw...
// New lead for the invisible/near-transparent-enemy investigation...
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @struct GLNodeDrawState
 * @brief Estructura de rastreo de estado de renderizado para nodos SkinnedMesh (personajes y enemigos).
 */
typedef struct {
    unsigned draw_calls;           /**< Número de llamadas a glDraw* emitidas por el nodo. */
    GLint last_texture;            /**< ID de la última textura vinculada (`glBindTexture`). */
    GLsizei last_vertex_count;     /**< Número de vértices enviados en el último dibujado. */
    GLboolean last_blend_enabled;  /**< Indica si el mezclado de color (`GL_BLEND`) estaba activo. */
    GLint last_blend_src_rgb;      /**< Factor origen RGB de Blending. */
    GLint last_blend_dst_rgb;      /**< Factor destino RGB de Blending. */
    GLint last_blend_src_alpha;    /**< Factor origen Alpha de Blending. */
    GLint last_blend_dst_alpha;    /**< Factor destino Alpha de Blending. */
    GLboolean last_depth_test_enabled; /**< Estado de prueba de profundidad (`GL_DEPTH_TEST`). */
    GLboolean last_depth_write_mask;   /**< Máscara de escritura en búfer de profundidad (`glDepthMask`). */
} GLNodeDrawState;

/** @brief Reinicia los contadores de rastreo de renderizado por nodo. */
void gl_diag_reset_render_track(void);

/** @brief Obtiene la copia del estado de renderizado capturado. */
void gl_diag_get_render_track(GLNodeDrawState *out);

/** @brief Audita matrices 4x4 cargadas por `glUniformMatrix4fv` para detectar matrices degeneradas o valores NaN/Inf. */
void glUniformMatrix4fv_soloader(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);

/** @brief Audita atributos de vértices enviados por `glVertexAttrib4f`/`4fv` para detectar constantes nulas o transparentes. */
void glVertexAttrib4f_soloader(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void glVertexAttrib4fv_soloader(GLuint index, const GLfloat *v);
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Monitorea de forma no invasiva las llamadas de dibujado de mallas esqueléticas (`SkinnedMeshSceneNode`). Almacena los parámetros de blending, profundidad, matrices de transformación, formatos de textura comprimida PVRTC y atributos de color por vértice.
- **Razón del comentario / Justificación Técnica**: En el port original, los enemigos aparecían parcialmente transparentes o invisibles en pantalla. Este conjunto de wrappers permitió diagnosticar si el problema se debía a llamadas de dibujado omitidas por el motor, texturas corruptas en el paquete PVRTC, o colores por vértice no inicializados (que multiplicaban la opacidad por 0.0).
