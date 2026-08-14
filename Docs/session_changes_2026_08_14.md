# Documentación de Cambios y Correcciones - Sesión 2026-08-14

## Resumen Ejecutivo
Esta sesión resolvió los problemas críticos de renderizado 3D, visibilidad de entidades, inversión de colores en efectos, paridad entre compilaciones Debug y Release, y adaptación a pantalla completa en PlayStation Vita.

---

## 1. Corrección de Invisibilidad de Enemigos y NPCs (AlphaSampler / Discard Fix)

### Causa Raíz
En el shader principal de personajes ([`GL_Diffuse_L1_iPhone_FS.glsl`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/com.gameloft.android.GAND.GloftD2SS/files/shaders.pak)), la lógica original de Gameloft realizaba:
```glsl
#ifdef ALPHA_MAP
    Diffuse.w = texture2D(AlphaSampler, vTexCoord0.xy).z;
#endif

#ifdef AT
    if (Diffuse.w < 0.8) discard;
#endif
```
- Gameloft muestreaba únicamente el canal azul (`.z`) de la textura secundaria en la unidad de textura 1.
- En el chip PowerVR SGX543 de la PS Vita, las texturas de alfa/luminancia entregan su valor en los canales `.a` o `.r`, dejando `.z` en `0.0`.
- Como consecuencia, `Diffuse.w` resultaba ser `0.0`, activando la condición `Diffuse.w < 0.8` y ejecutando `discard;` en **todos los píxeles de los modelos de enemigos y NPCs**.

### Solución Aplicada
Se modificó [`shaders.pak`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/com.gameloft.android.GAND.GloftD2SS/files/shaders.pak) aplicando un muestreo multicanal con fallback automático a opacidad completa:
```glsl
#ifdef ALPHA_MAP
    lowp vec4 _aTex = texture2D(AlphaSampler, vTexCoord0.xy);
    lowp float _aCh = max(max(_aTex.a, _aTex.r), max(_aTex.g, _aTex.z));
    Diffuse.w = (_aCh > 0.0) ? _aCh : 1.0;
#endif

#ifdef AT
    if (Diffuse.w < 0.05) discard;
#endif
```
El archivo fue empaquetado estrictamente en formato **`ZIP_STORED` (sin compresión / 0%)**, requerido por el lector de flujos directos del motor.

---

## 2. Corrección de Niebla Morada en Efectos de Luz Aditiva

### Causa Raíz
En [`GL_Diffuse_L1_iPhone_FS.glsl`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/com.gameloft.android.GAND.GloftD2SS/files/shaders.pak) y [`ProfileCOMMON_emul_FS.glsl`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/com.gameloft.android.GAND.GloftD2SS/files/shaders.pak), la niebla ambiental del pantano (de tonalidad violeta) se aplicaba incondicionalmente a todos los materiales mediante `mix(color.xyz, fogcolor.xyz, factor)`. En efectos, antorchas y luces con mezcla aditiva (`ADDITIVE` / `ADDITIVEBLEND`), la niebla morada se sumaba directamente al framebuffer, transformando destellos blancos y amarillos en halos morados opacos a distancia.

### Solución Aplicada
Se reescribió la rama de niebla aditiva en los shaders:
```glsl
#ifdef FG
    #ifdef ADDITIVEBLEND
        color.xyz *= (1.0 - vFogFactor);
    #else
        color.xyz = mix(color.xyz, fogcolor.xyz, vFogFactor);
    #endif
#endif
```
Las luces y partículas aditivas ahora se desvanecen limpiamente a negro `(0, 0, 0)` con la distancia, preservando sus colores blanco y dorado puros.

---

## 3. Redirección de Texturas del Hada Compañera

### Archivo Modificado
- [`source/reimpl/io.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/io.c#L368-L393)

### Detalle del Cambio
Las texturas ausentes en el dump original de Android fueron reasignadas a recursos gráficos adecuados:
- `char_faerie.tga` y `tex_faerie_001.tga` $\rightarrow$ `fx_radial_white.tga` (cuerpo del hada en blanco brillante).
- `fx_spark.tga` $\rightarrow$ `fx_magic_lenz_flares_002.tga` (halo y chispas en amarillo/dorado).

---

## 4. Paridad de Compilación y Estabilidad de Release

### Archivos Modificados
- [`CMakeLists.txt`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/CMakeLists.txt#L116-L125)
- [`source/utils/logger.h`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/logger.h#L28-L34)
- [`source/utils/logger.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/logger.c#L40-L44)

### Detalle del Cambio
1. **Unificación de flags de optimización:** Se fijaron `CMAKE_C_FLAGS_RELEASE`, `CMAKE_CXX_FLAGS_RELEASE`, `CMAKE_C_FLAGS_DEBUG` y `CMAKE_CXX_FLAGS_DEBUG` en `-O2 -g` uniforme, eliminando `-ffast-math` y las optimizaciones agresivas de `-O3` que alteraban la reordenación de instrucciones en ARMv7.
2. **Logging en Release sin divergencias:** Las macros `l_debug`, `l_info`, `l_warn`, etc. permanecen activas con las mismas firmas en todos los modos; en Release (`-DRELEASE_BUILD`), `_log_print()` retorna inmediatamente en su primera línea, eliminando todo el coste de I/O de disco sin alterar el código ejecutable ni el flujo de control.

---

## 5. Ajuste a Pantalla Completa (960x544 Nativo)

### Archivo Modificado
- [`source/utils/glutil.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/glutil.c#L1340-L1390)

### Detalle del Cambio
Se eliminó el offset horizontal de 72 píxeles (`x_offset = 72`) en [`glViewport_soloader`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/glutil.c#L1340) y [`glScissor_soloader`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/glutil.c#L1360). El juego ahora escala directamente a los **960x544 píxeles nativos** de la PlayStation Vita sin bordes negros laterales.

---

## 6. Aceleración de Cargas y Prevención de Suspensión

### Archivos Modificados
- [`source/main.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/main.c#L239)
- [`source/reimpl/io.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/io.c#L36-L42)

### Detalle del Cambio
1. **Prevención de suspensión:** Se introdujeron llamadas periódicas a `sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT)` en el bucle principal y en la precarga de archivos, evitando que la pantalla se atenúe o apague por inactividad durante las pantallas de carga.
2. **Ajuste de caché RAM (`fcache`):** Se configuró un límite de 256 KB por archivo y 32 MB total, optimizado para acelerar configs, scripts y trozos de animación sin saturar el hilo principal durante combate.

---

## 7. Control de Reproducción de Cinemáticas

### Archivos Modificados
- [`source/main.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/main.c)
- [`source/java.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/java.c#L127-L148)

### Detalle del Cambio
Se eliminó el forzado por cuadro de `videoDone = 1` en `main.c`. Los vídeos (`intro.mp4` / `opening.mp4`) se reproducen de manera fluida y completa con sonido y controles, y `*videoDone = 1` se establece con barrera de memoria (`kuKernelFlushCaches`) estrictamente tras finalizar la reproducción en `GLMediaPlayer_loadMovie`.
