# Documentación Técnica: `source/reimpl/pthr.c`

**Archivo Origen:** [`source/reimpl/pthr.c`](file:///Volumes/Seagate/PSVITA%20Develop/Dungeon-Hunter-2-vita/source/reimpl/pthr.c)  
**Módulo:** Reimplementation / POSIX Threads (`pthread`) & Semaphores Bridge  
**Propósito:** Capa de traducción entre las estructuras de hilos, mutexes y variables de condición de Android Bionic y la implementación `pthread` de VitaSDK (Newlib/PSP2 Kernel). Incluye correcciones críticas para evitar fugas de hilos de kernel, cuelgues durante el guardado de partida y excepciones C++ en VitaGL.

---

## 1. Resumen de Comentarios `//` y Funciones

| Línea | Comentario Original `//` (Extracto) | Función / Ámbito |
| :--- | :--- | :--- |
| **23-28** | `// __sinit initializes the current reent's stdio/state; pairs with pthread_init...` | `pthread_setup()` (Solución Excepciones C++) |
| **31-39** | `// pthread-embedded (VitaSDK's pthread implementation) needs its internal...` | `pthread_setup()` (Constructor con prioridad 101) |
| **197-199**| `// Honor the game's requested stack size when it bothers to set one...` | `pthread_create_soloader()` (Stack size) |
| **216-225**| `// Force DETACHED regardless of what the game asked for. This engine...` | `pthread_create_soloader()` (Threads Detached) |
| **227-231**| `// Safety net in case we still momentarily run out of thread slots...` | `pthread_create_soloader()` (Bucle de reintento `EAGAIN`) |
| **251-254**| `// We create every thread detached (see pthread_create_soloader)...` | `pthread_join_soloader()` (No-op exitoso) |

---

## 2. Análisis Detallado y Conversión a Bloques Doxygen

### 2.1 Constructor Global de Inicialización de Pthreads (`pthread_setup`) (Líneas 23-44)

#### Comentarios Originales (`//`):
```c
// __sinit initializes the current reent's stdio/state; pairs with pthread_init
// as the documented vitasdk fix for "C++ exceptions + threads" (vita-toolchain
// issue #103). Needed because vitaGL's GLSL translator path throws C++
// exceptions internally, and without this the very first throw crashes inside
// __cxa_allocate_exception (a concurrence_lock_error on the emergency-pool
// mutex) instead of unwinding.
extern void __sinit(struct _reent *);

// pthread-embedded (VitaSDK's pthread implementation) needs its internal
// thread-pool state set up via pthread_init() before any pthread_create()
// call, or every single one fails with EAGAIN regardless of how few
// threads are actually running...
// Constructor priority 101 matches the documented vitasdk workaround...
__attribute__((constructor(101)))
static void pthread_setup(void) {
    pthread_init();
    __sinit(_REENT);
}
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @brief Constructor con prioridad 101 que inicializa la pila `pthread` de VitaSDK y la estructura reentrante de Newlib.
 *
 * @details Realiza dos operaciones de infraestructura críticas antes de la ejecución de `main()`:
 *          1. Invocación explícita de `pthread_init()`: La implementación de pthreads de VitaSDK requiere
 *             inicializar el pool interno de hilos antes de cualquier llamada a `pthread_create()`. Sin esto,
 *             toda creación de hilos falla inmediatamente con error `EAGAIN` (código 11).
 *          2. Invocación de `__sinit(_REENT)`: Corrige el error conocido #103 de VitaSDK sobre excepciones C++
 *             en entornos multihilo. Dado que la librería de traducción GLSL de VitaGL lanza excepciones C++
 *             internamente, sin `__sinit()` la primera excepción arrojada provoca un bloqueo irrecuperable
 *             (`concurrence_lock_error`) dentro de `__cxa_allocate_exception`.
 */
__attribute__((constructor(101))) static void pthread_setup(void);
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Define una función constructora de C (`__attribute__((constructor(101)))`) que se ejecuta automáticamente antes del punto de entrada `main()`. Llama a `pthread_init()` y `__sinit(_REENT)`.
- **Razón del comentario / Justificación Técnica**: En el SDK de PS Vita, la biblioteca pthreads no se inicializa automáticamente al cargar el binario. Además, el traductor de shaders GLSL de VitaGL utiliza C++ con bloques `try/catch`. Sin la llamada inicializadora a `__sinit`, la memoria de emergencia para desenredo de excepciones (`unwinding`) no se reserva en la estructura `_REENT` del hilo principal, produciendo un cuelgue inmediato del juego al compilar el primer shader.

---

### 2.2 Creación de Hilos Forzadamente Desconectados (`pthread_create_soloader`) (Líneas 194-247)

#### Comentarios Originales (`//`):
```c
// Force DETACHED regardless of what the game asked for. This engine
// spawns one fire-and-forget worker per savegame job (Savegame::UpdateJobs,
// ~190 of them while writing the initial save) via updateJob_thread::Start(),
// which overwrites the stored handle each call and NEVER pthread_join()s
// the previous one. Left joinable, every finished worker stays a zombie
// holding a kernel thread slot; after ~120 the kernel returns EAGAIN
// ("Not Created" spam, hung save flush, black screen). Detached threads
// self-reap on exit, so only the few actually running at once cost a slot.
// Any later pthread_join() on these is a harmless no-op (see below).
```

#### Conversión a Bloque Doxygen (`/** ... */`):
```c
/**
 * @brief Reimplementación de `pthread_create` para Android Bionic con manejo forzado de estado DETACHED.
 *
 * @details El motor de Dungeon Hunter 2 genera un hilo trabajador independiente por cada tarea de guardado
 *          (`Savegame::UpdateJobs`, ~190 hilos durante la creación de una partida). El motor sobrescribe los
 *          punteros y **nunca** invoca `pthread_join()` en los hilos terminados.
 *
 *          Si los hilos se crean en estado acoplable (`joinable`), cada hilo finalizado permanece como un proceso
 *          zombie reteniendo una ranura de hilo del kernel de VitaOS. Al alcanzar el límite del kernel (~120 hilos),
 *          el sistema empieza a fallar con `EAGAIN`, congelando el guardado y provocando pantalla negra.
 *
 *          `pthread_create_soloader` fuerza el estado `PTHREAD_CREATE_DETACHED` en todos los hilos, garantizando
 *          que el kernel libere automáticamente la memoria del hilo tan pronto como este termina su ejecución.
 */
int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param);
```

#### Explicación Detallada (Razón y Funcionamiento):
- **¿Qué hace el código aquí?**: Intercepta la creación de hilos desde la librería `.so`. Ignora la preferencia de acoplamiento del motor y aplica `pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED)`. Adicionalmente, incluye un bucle con retardo de 2ms y 20 reintentos si el kernel retorna temporalmente `EAGAIN`.
- **Razón del comentario / Justificación Técnica**: Resuelve uno de los bugs de estabilidad más graves del port de Dungeon Hunter 2 en PS Vita. Sin esta modificación, guardar la partida o cambiar de zona llenaba la tabla de hilos del kernel de la consola, impidiendo que el motor creara nuevos hilos y dejando la pantalla completamente congelada en negro.
