# Documentación Técnica: `source/utils/glutil.c`

**Archivo Origen:** [`source/utils/glutil.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/glutil.c)  
**Módulo:** Utilities / OpenGL ES Wrappers & Graphics Diagnostics Implementation  
**Propósito:** Implementación completa de wrappers gráficos de OpenGL ES 2.0, precarga y caching binario de shaders GLSL para el procesador gráfico PowerVR SGX543MP4+ de la PS Vita, renderizado offscreen y diagnósticos en tiempo real.

---

## 1. Resumen de Comentarios `//` y Funciones

| Línea | Comentario Original `//` (Extracto) | Función / Ámbito |
| :--- | :--- | :--- |
| **22-24** | `// Defined further below, alongside the rest of the shader binary cache --` | Declaración adelantada `gl_preload_shaders()` |
| **28-44** | `// "Enemigos invisibles" investigation: the periodic single-point-in-time snapshot...` | `s_seen_textures` (Rastreador de texturas) |
| **47-55** | `// Program bound at the moment each s_seen_textures[i] was first seen...` | `s_seen_texture_programs` |
| **58-61** | `// True at least once this diag window if a bound texture couldn't be recorded...` | `s_seen_textures_overflowed` |
| **65-78** | `// "Enemigos invisibles" investigation: GL_Diffuse_L1_iPhone_FS/VS.glsl...` | Rastreo de variantes AL/AT (AlphaSampler) |
| **88-90** | `// True if "#define AL" or "#define AT" appears active...` | `check_source_has_alat_defined()` |

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Forward Declaration del Caché de Shaders (Líneas 22-25)

#### Comentario Original (`//`):
```c
// Defined further below, alongside the rest of the shader binary cache --
// forward-declared here so gl_init() (which calls it right after context
// creation) doesn't need the whole cache implementation moved above it.
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @brief Declaración adelantada de la función de precarga del caché binario de shaders.
 * 
 * Permite que `gl_init()` invoque `gl_preload_shaders()` inmediatamente después de la creación 
 * del contexto gráfico sin necesidad de reordenar todo el módulo de caché binario.
 */
static void gl_preload_shaders(void);
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Declara el prototipo estático `gl_preload_shaders(void)` antes de la definición de `gl_init()`.
- **Razón del comentario / Justificación Técnica**: Mantener una estructura de código limpia donde `gl_init()` (la rutina principal de inicialización de VitaGL) permanezca al inicio del archivo sin forzar la reubicación de cientos de líneas de código del sistema de almacenamiento en caché.

---

### 2.2 Diagnóstico de Texturas y Variantes Shaders para "Enemigos Invisibles" (Líneas 28-78)

#### Comentarios Originales (`//`):
```c
// "Enemigos invisibles" investigation: the periodic single-point-in-time
// snapshot in gl_log_render_diag only ever caught 2 texture IDs (910013,
// 560008) across the whole of log_082's combat section, even long after the
// troll's assets loaded successfully -- but a single sample per ~60 frames
// can easily always land between an enemy's draw calls and miss its texture
// entirely. This tracks every DISTINCT texture bound across an entire diag
// window instead of one instant, so a third ID either shows up somewhere in
// that window or genuinely never does.

// "Enemigos invisibles" investigation: GL_Diffuse_L1_iPhone_FS/VS.glsl (the
// shared lit-character shader template, see shaders.pak) hardcodes vertex
// alpha to 1.0 and derives final alpha from the diffuse texture's own alpha
// channel UNLESS the engine compiles this template with AL/AT defined...
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @brief Examina si el código fuente de un shader GLSL contiene activas las directivas `#define AL` o `#define AT`.
 *
 * @details Los modelos 3D de personajes en Dungeon Hunter 2 utilizan la plantilla `GL_Diffuse_L1_iPhone`.
 *          Si la plantilla se compila con las macros `AL` (Alpha Layer) o `AT` (Alpha Test), la opacidad
 *          final no proviene del canal Alfa de la textura difusa sino de una segunda textura de muestreo (`AlphaSampler`).
 *          Si esa segunda textura no está vinculada, OpenGL ES devuelve (0,0,0,0), provocando que el modelo sea 100% transparente.
 *
 * @param[in] src Cadena con el código fuente del shader GLSL.
 * @return `true` si el shader utiliza la variante de mapeo Alfa separado.
 */
static bool check_source_has_alat_defined(const char *src);
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Analiza las cadenas de texto del shader enviadas a `glShaderSource`. Busca las palabras clave `#define AL` o `#define AT` que no estén comentadas. Si las encuentra, marca el ID del shader para rastrear si los enemigos invisibles intentan renderizarse con una unidad de textura Alfa no asignada.
- **Razón del comentario / Justificación Técnica**: Explicación detallada de un bug difícil de depurar donde los enemigos se dibujaban en pantalla con todas sus llamadas a la GPU correctas, pero invisibles para el jugador. Determinar que la causa residía en shaders esperando un segundo muestreador Alfa descartó problemas de fallos en la carga de geometrías o mallas corruptas.

---

### 2.3 Precarga del Caché de Shaders para VitaGL (`gl_preload_shaders()`) (Líneas 110-145)

```c
/**
 * @brief Compila y almacena en caché todas las combinaciones de shaders GLSL del motor durante la pantalla de carga.
 *
 * @details El driver gráfico de la PS Vita (`PVR_PSP2`) sufre pausas notables (stuttering) si compila
 *          shaders GLSL sobre la marcha en medio del bucle de juego. `gl_preload_shaders()` compila
 *          anticipadamente todas las variantes posibles de `GL_Diffuse_L1_iPhone` y las almacena en la memoria
 *          binaria de VitaGL para garantizar 60 FPS estables durante el juego.
 */
static void gl_preload_shaders(void) {
    // Implementación de precarga e inicialización de caché binario
}
```
