# Documentación Técnica: `source/reimpl/pthr.h`

**Archivo Origen:** [`source/reimpl/pthr.h`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/pthr.h)  
**Módulo:** Reimplementation / POSIX Threads Bridge Header  
**Propósito:** Definición de estructuras de datos adaptadoras entre Android Bionic y Newlib VitaSDK para hilos, mutexes y semáforos.

---

## 1. Resumen de Comentarios `//` y Funciones

| Línea | Comentario Original `//` | Función / Ámbito |
| :--- | :--- | :--- |
| **44** | `// pthread_t is same size on bionic and newlib` | Definiciones de `pthread_t` |
| **53** | `// pthread_t and sched_param are same size on bionic and newlib` | `pthread_getschedparam` / `setschedparam` |
| **57** | `// condattr_t is same size on bionic and newlib` | Atributos de condición |
| **61** | `// mutexattr_t is same size on bionic and newlib` | Atributos de mutex |
| **98** | `#endif // SOLOADER_PTHR_H` | Guardas de inclusión `#ifndef` |

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Estructuras Adaptadoras `pthread_attr_t_bionic`, `pthread_mutex_t_bionic`, `pthread_cond_t_bionic` (Líneas 27-43)

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @struct pthread_attr_t_bionic
 * @brief Adaptador para la estructura `pthread_attr_t` de Android Bionic.
 *
 * Encapsula el puntero real de Newlib `real_ptr` dentro del tamaño de bytes que la librería de Android espera.
 */
typedef struct {
    pthread_attr_t *real_ptr;
    int32_t magic;
    size_t stack_size;
    size_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
} pthread_attr_t_bionic;

/**
 * @struct pthread_mutex_t_bionic
 * @brief Adaptador para `pthread_mutex_t` de Bionic.
 */
typedef struct {
    pthread_mutex_t *real_ptr;
} pthread_mutex_t_bionic;

/**
 * @struct pthread_cond_t_bionic
 * @brief Adaptador para `pthread_cond_t` de Bionic.
 */
typedef struct {
    pthread_cond_t *real_ptr;
} pthread_cond_t_bionic;
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Define estructuras opacas que contienen un puntero `real_ptr` hacia el objeto real asignado por Newlib en VitaSDK.
- **Razón del comentario / Justificación Técnica**: En Android Bionic, `pthread_mutex_t` es una estructura de 4 bytes conteniendo una palabra entera de control de bloqueo atómico. En Newlib/VitaSDK, `pthread_mutex_t` es una estructura de mayor tamaño conteniendo descriptores del kernel de la Vita. Intercambiar directamente los punteros corrompería la memoria adyacente del motor de juego. Asignar dinámicamente el objeto Newlib y almacenar su dirección dentro de `real_ptr` resuelve la incompatibilidad de tamaño de la estructura.

---

### 2.2 Declaraciones de Funciones Wrappers de Pthreads y Semáforos

```c
/** @brief Crea un hilo forzando el estado detached en PS Vita. */
int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param);

/** @brief Maneja de forma segura la unión de hilos detached. */
int pthread_join_soloader(pthread_t thread, void **value_ptr);

/** @brief Inicializa un mutex mapeando el tipo (recursivo/normal). */
int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr);

/** @brief Bloquea un mutex emulado. */
int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex);

/** @brief Desbloquea un mutex emulado. */
int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex);

/** @brief Crea un semáforo del kernel de Vita OS (`sceKernelCreateSema`). */
int sem_init_soloader(int *sem, int pshared, unsigned int value);

/** @brief Libera un semáforo del kernel (`sceKernelDeleteSema`). */
int sem_destroy_soloader(int *sem);

/** @brief Incrementa la señal de un semáforo (`sceKernelSignalSema`). */
int sem_post_soloader(int *sem);

/** @brief Espera en un semáforo (`sceKernelWaitSema`). */
int sem_wait_soloader(int *sem);
```
