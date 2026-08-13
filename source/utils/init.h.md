# Documentación Técnica: `source/utils/init.h`

**Archivo Origen:** [`source/utils/init.h`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/utils/init.h)  
**Módulo:** Utilities / Subsystem & Loader Initialization  
**Propósito:** Declaraciones públicas para las rutinas de inicialización y parcheo de la librería binaria de Android en PS Vita.

---

## 1. Resumen de Comentarios `//` y Funciones

| Línea | Comentario Original `//` | Función Asociada / Ámbito |
| :--- | :--- | :--- |
| **32** | `#endif // SOLOADER_INIT_H` | Guardas de inclusión `#ifndef` |

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Guardas de Inclusión Header Guard (Línea 32)

#### Comentario Original (`//`):
```c
#endif // SOLOADER_INIT_H
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @file init.h
 * @brief Rutinas de inicialización y bootstrapping global de SoLoader.
 */
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Cierra el bloque condicional `#ifndef SOLOADER_INIT_H`.
- **Razón del comentario / Justificación Técnica**: Evita la inclusión múltiple de las definiciones de inicialización en distintos archivos fuente.

---

## 3. Declaraciones de Funciones en Doxygen

```c
/**
 * @brief Resuelve las importaciones de símbolos dinámicos (libc, POSIX, EGL, GLES2) del módulo `.so`.
 * 
 * @param[in,out] mod Puntero a la estructura del módulo dinámico cargado (`so_module`).
 */
void resolve_imports(so_module *mod);

/**
 * @brief Aplica parches binarios en caliente en el código ensamblador ARM del `.so`.
 * 
 * @details Modifica instrucciones o desvía llamadas a funciones específicas dentro del ELF
 *          cargado para saltar DRM/licencias, solucionar desalineaciones de pila o corregir lógica incompatible.
 */
void so_patch();

/**
 * @brief Inicializa por completo todo el cargador, subsistemas de hardware y puentes JNI.
 */
void soloader_init_all();
```
