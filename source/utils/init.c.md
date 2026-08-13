# Documentación Técnica: `source/utils/init.c`

**Archivo Origen:** [`source/utils/init.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/init.c)  
**Módulo:** Utilities / Subsystem & Loader Initialization  
**Propósito:** Secuencia principal de inicialización del cargador `SoLoader`, comprobación de dependencias (kernel bridge), carga del `.so` de Android en memoria física, reubicación de símbolos, parcheo en caliente y bootstrapping del entorno Java simulado (FalsoJNI) y gráficos VitaGL.

---

## 1. Resumen de Comentarios `//` y Funciones

| Línea | Comentario Original `//` | Función Asociada / Ámbito |
| :--- | :--- | :--- |
| **29** | `// Base address for the Android .so to be loaded at` | Macro `#define LOAD_ADDRESS 0x98000000` |
| **35** | `// Launch app0:configurator.bin on -config init param` | `soloader_init_all()` |
| **47** | `// Set default overclock values` | `soloader_init_all()` |

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Macro `LOAD_ADDRESS` (Línea 29-30)

#### Comentario Original (`//`):
```c
// Base address for the Android .so to be loaded at
#define LOAD_ADDRESS 0x98000000
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @def LOAD_ADDRESS
 * @brief Dirección base de memoria virtual donde se asigna y mapea la librería dinámica ELF de Android (`libDungeonHunter2.so`).
 *
 * @details La dirección `0x98000000` se selecciona cuidadosamente en el espacio de usuario de la PS Vita
 *          para evitar colisiones con los módulos `.suprx` del sistema VitaOS y la pila de ejecución de ejecutable principal.
 */
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Define la constante numérica `0x98000000` usada por `so_file_load` para reservar el bloque de memoria RAM virtual donde se desempaquetan las secciones `.text`, `.rodata`, `.data` y `.bss` de la librería compartida de ARM.
- **Razón del comentario / Justificación Técnica**: En el esquema de SoLoader en PS Vita, el ejecutable de Android `.so` no es cargado por un enlazador dinámico del sistema operativo sino relocalizado manualmente por `so_util`. Especificar una dirección fija en memoria de usuario garantiza que las referencias absolutas y reubicaciones relativas ARM (GOT/PLT) se resuelvan de forma limpia y predecible sin fragmentar la memoria RAM.

---

### 2.2 Verificación de Parámetros de LiveArea y Lanzamiento de Configurador (Línea 35-45)

#### Comentario Original (`//`):
```c
// Launch `app0:configurator.bin` on `-config` init param
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @brief Comprueba si la aplicación fue lanzada desde el LiveArea con el parámetro `-config`.
 *
 * Si se detecta el evento de arranque con dicho parámetro, invoca `sceAppMgrLoadExec()` 
 * para ejecutar el binario del configurador GUI independiente (`app0:/configurator.bin`).
 */
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Inicializa `AppUtil`, escucha eventos de inicio del sistema operativo PS Vita (`sceAppUtilReceiveAppEvent`) y examina los argumentos pasados a través del LiveArea (`template.xml`). Si la cadena contiene `-config`, cancela la ejecución del juego y salta al configurador de ajustes de la aplicación.
- **Razón del comentario / Justificación Técnica**: Permite integraciones avanzadas con botones personalizados en el LiveArea de la PS Vita (como un botón de "Ajustes de Render/Gráficos"). Permite al usuario ajustar opciones sin necesidad de arrancar todo el motor de juego.

---

### 2.3 Overclock de Hardware de PS Vita (Línea 47-51)

#### Comentario Original (`//`):
```c
// Set default overclock values
scePowerSetArmClockFrequency(444);
scePowerSetBusClockFrequency(222);
scePowerSetGpuClockFrequency(222);
scePowerSetGpuXbarClockFrequency(166);
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @brief Configura las frecuencias de reloj del hardware de PS Vita a sus límites máximos oficiales.
 *
 * @note Ajusta las frecuencias a:
 *       - CPU ARM Cortex-A9: 444 MHz
 *       - BUS de sistema: 222 MHz
 *       - GPU PowerVR SGX543MP4+: 222 MHz
 *       - GPU Crossbar (Xbar): 166 MHz
 */
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Ejecuta llamadas directas a las APIs del módulo `ScePower` para elevar las frecuencias de la CPU y GPU a la configuración máxima soportada por el SDK oficial de Sony.
- **Razón del comentario / Justificación Técnica**: Los motores de juego 3D pesados de Android diseñados para GPUs móviles requieren la máxima capacidad de cómputo posible en la PS Vita. Ejecutar a la velocidad base (333 MHz CPU) causaría severas caídas de FPS durante batallas intensas en Dungeon Hunter 2.

---

### 2.4 Bloque Doxygen Completo para `soloader_init_all()` (Línea 34-98)

```c
/**
 * @brief Inicializa por completo todos los subsistemas del motor para el port.
 *
 * Realiza la secuencia completa de arranque:
 * 1. Verificación del parámetro de inicio `-config` para lanzar el configurador.
 * 2. Aplicación de overclock de CPU/GPU (444/222/222/166 MHz).
 * 3. Inicialización opcional de FIOS (I/O acelerado de Sony).
 * 4. Comprobación de la presencia del plugin de kernel `kubridge.skprx`.
 * 5. Verificación de existencia del archivo `.so` del juego en disco.
 * 6. Carga de `libDungeonHunter2.so` en la dirección `0x98000000`.
 * 7. Carga de los ajustes guardados (`settings_load()`).
 * 8. Relocalización de símbolos y parches en caliente (`so_relocate`, `resolve_imports`, `so_patch`).
 * 9. Invalidation/Flush de cachés de instrucciones de CPU ARM (`so_flush_caches`).
 * 10. Inicialización de gráficos VitaGL (`gl_preload()`) y entorno FalsoJNI (`jni_init()`).
 */
void soloader_init_all();
```
