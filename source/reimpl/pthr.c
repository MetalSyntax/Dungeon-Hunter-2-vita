/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022      GrapheneCt
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/pthr.h"

#include <stdlib.h>
#include <string.h>
#include <reent.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <stdatomic.h>

#include "utils/utils.h"
#include "utils/logger.h"

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
// threads are actually running -- it isn't called automatically by the
// linker's --whole-archive trick, and nothing in this project ever called
// it (confirmed: the game's own background update thread failed to spawn
// on literally its first attempt, logged repeatedly as "Not Created").
// Constructor priority 101 matches the documented vitasdk workaround,
// running this before other global constructors and well before main().
__attribute__((constructor(101)))
static void pthread_setup(void) {
    pthread_init();
    __sinit(_REENT);
}

#define PTHR_MAX_OBJECTS 1024

#define BIONIC_PTHREAD_COND_INITIALIZER              0
#define BIONIC_PTHREAD_MUTEX_INITIALIZER             0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER   0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER  0x8000

enum {
    BIONIC_PTHREAD_MUTEX_NORMAL = 0,
    BIONIC_PTHREAD_MUTEX_RECURSIVE = 1,
    BIONIC_PTHREAD_MUTEX_ERRORCHECK = 2,

    BIONIC_PTHREAD_MUTEX_ERRORCHECK_NP = BIONIC_PTHREAD_MUTEX_ERRORCHECK,
    BIONIC_PTHREAD_MUTEX_RECURSIVE_NP  = BIONIC_PTHREAD_MUTEX_RECURSIVE,

    BIONIC_PTHREAD_MUTEX_DEFAULT = BIONIC_PTHREAD_MUTEX_NORMAL
};

#define PTHR_INLINE static inline __attribute__((always_inline))

// Set de punteros con direccionamiento abierto (sondeo lineal) en vez del
// arreglo lineal que habia antes. El arreglo hacia que CADA
// pthread_mutex_lock() del motor (via _mutex_t_static_init ->
// isObjectInitialized) tomara un LwMutex GLOBAL y comparara hasta 1024
// punteros. Con Box2D + GameSWF + los resource managers del motor bloqueando
// mutexes miles de veces por frame, eso es trabajo O(n) serializado entre
// todos los hilos dentro del presupuesto de frame.
//
// Ademas explicaba la degradacion progresiva de FPS dentro de una misma
// sesion (visible en log_000/003/004: arranca en ~8-9 FPS y cae a ~5.6): a
// medida que se registran mas objetos, el barrido promedio se alarga, y un
// MISS siempre recorria las 1024 posiciones enteras.
//
// Capacidad potencia de 2 y con holgura sobre PTHR_MAX_OBJECTS para que el
// factor de carga se mantenga bajo y el sondeo sea corto.
#define PTHR_SET_CAP 8192
#define PTHR_SET_MASK (PTHR_SET_CAP - 1)
#define PTHR_TOMBSTONE ((void *) 1)

static void * pthr_set[PTHR_SET_CAP] = {0};

// Los punteros vienen de malloc() o son direcciones de objetos del motor:
// siempre alineados a >= 4 bytes, asi que descartar los bits bajos y mezclar
// da una distribucion pareja.
PTHR_INLINE unsigned pthr_hash(const void *p) {
    uintptr_t v = (uintptr_t) p >> 3;
    v ^= v >> 13;
    v *= 0x9E3779B1u;
    return (unsigned) (v & PTHR_SET_MASK);
}
static SceKernelLwMutexWork pthr_mutex;
static volatile short int pthr_mutex_inited = 0;

#define PTHR_LOCK \
    if (!pthr_mutex_inited) { \
        int ret = sceKernelCreateLwMutex(&pthr_mutex, "log_lock", 0, 0, NULL); \
        if (ret < 0) { \
            sceClibPrintf("Error: failed to create pthr mutex: 0x%x\n", ret); \
            return 0; \
        } \
        pthr_mutex_inited = 1; \
    } \
    sceKernelLockLwMutex(&pthr_mutex, 1, NULL);

#define PTHR_UNLOCK \
    if (pthr_mutex_inited) { \
        sceKernelUnlockLwMutex(&pthr_mutex, 1); \
    }

// SIN LOCK a proposito. Lo caro del codigo original no era la tabla: eran los
// dos syscalls al kernel (sceKernelLockLwMutex/Unlock) y el barrido lineal de
// 1024 punteros, y eso pasaba en CADA pthread_mutex_lock del motor -- que,
// segun el desensamblado del .so, son todos los allocators de STLport, o sea
// CADA std::string/vector/map que el motor alloca.
//
// La tabla tiene tamano fijo y nunca rehashea, y cada slot se escribe con un
// store atomico alineado, asi que un lector concurrente ve o el valor viejo o
// el nuevo, nunca uno a medias. Los escritores (remember/forget) si toman el
// lock entre ellos.
int isObjectInitialized(const void * mut) {
    unsigned i = pthr_hash(mut);
    for (unsigned probe = 0; probe < PTHR_SET_CAP; ++probe) {
        void *slot = __atomic_load_n(&pthr_set[i], __ATOMIC_ACQUIRE);
        if (slot == NULL) return 0;            // hueco real: no esta
        if (slot == mut) return 1;
        i = (i + 1) & PTHR_SET_MASK;           // tombstone o colision: seguir
    }
    return 0;
}

int rememberObject(void * mut) {
    PTHR_LOCK
    unsigned i = pthr_hash(mut);
    int tomb = -1;
    for (unsigned probe = 0; probe < PTHR_SET_CAP; ++probe) {
        void *slot = pthr_set[i];
        if (slot == mut) { PTHR_UNLOCK return 1; }   // ya estaba
        if (slot == PTHR_TOMBSTONE) {
            if (tomb < 0) tomb = (int) i;            // reusable, pero seguir
        } else if (slot == NULL) {
            __atomic_store_n(&pthr_set[tomb >= 0 ? (unsigned) tomb : i], mut, __ATOMIC_RELEASE);
            PTHR_UNLOCK
            return 1;
        }
        i = (i + 1) & PTHR_SET_MASK;
    }
    if (tomb >= 0) {
        __atomic_store_n(&pthr_set[tomb], mut, __ATOMIC_RELEASE);
        PTHR_UNLOCK
        return 1;
    }
    PTHR_UNLOCK
    return 0;   // set lleno (no deberia pasar con PTHR_SET_CAP=8192)
}

int forgetObject(const void * mut) {
    PTHR_LOCK
    unsigned i = pthr_hash(mut);
    for (unsigned probe = 0; probe < PTHR_SET_CAP; ++probe) {
        void *slot = pthr_set[i];
        if (slot == NULL) break;
        if (slot == mut) {
            // Tombstone, no NULL: un NULL cortaria la cadena de sondeo de
            // cualquier otra clave que haya colisionado en este indice y
            // termine mas adelante, y esa clave dejaria de encontrarse.
            __atomic_store_n(&pthr_set[i], PTHR_TOMBSTONE, __ATOMIC_RELEASE);
            PTHR_UNLOCK
            return 1;
        }
        i = (i + 1) & PTHR_SET_MASK;
    }
    PTHR_UNLOCK
    return 0;
}

// null check for `attr` must be performed before this
PTHR_INLINE int _attr_t_static_init(pthread_attr_t_bionic * attr) {
    if (attr->magic != 0x42424242) {
        attr->magic = 0x42424242;
        attr->real_ptr = malloc(sizeof(pthread_attr_t));
        return pthread_attr_init(attr->real_ptr);
    }
    return 0;
}

// null check for `mutex` param must be performed before this, `attr` is fine as null
// `real_ptr` de las structs bionic ALIASEA el `int volatile value` de bionic, y
// un mutex/cond inicializado estaticamente por el motor trae ahi una de las
// constantes PTHREAD_*_INITIALIZER (0, 0x4000, 0x8000) -- no un puntero. Todo
// lo que nosotros guardamos ahi viene de malloc(), que en Vita devuelve
// direcciones del heap de usuario, siempre muy por encima de los primeros 64KB
// (esa zona no esta mapeada). Asi que este umbral distingue de forma confiable
// "puntero real que publicamos nosotros" de "constante de inicializador
// estatico". Es la misma suposicion que ya hacia el codigo original al leer
// *(int*)mutex y compararlo contra esas constantes.
#define PTHR_IS_LIVE_PTR(p) ((uintptr_t)(p) > 0x10000u)

PTHR_INLINE int _mutex_t_static_init(pthread_mutex_t_bionic * mutex, const pthread_mutexattr_t * attr) {
    int ret = 0, kind = PTHREAD_MUTEX_NORMAL;

    // El registro es la UNICA autoridad sobre "ya inicializado", igual que en el
    // codigo original. Una version anterior de este fix dedujo eso del valor de
    // mutex->real_ptr (asumiendo que un puntero de malloc siempre queda por
    // encima de 0x10000): era fragil, porque un mutex con basura sin inicializar
    // en ese campo se tomaba como ya listo. Lo que se arreglo aca es el COSTO de
    // la consulta (ahora O(1) y sin syscalls), no quien decide.
    if (isObjectInitialized(mutex)) {
        //logv_debug("mutex already initialized: %p", mutex);
        return ret;
    }

    if (attr) {
        pthread_mutexattr_gettype((pthread_mutexattr_t *) attr, &kind);
    } else {
        if (* (int *) mutex == BIONIC_PTHREAD_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_NORMAL;
        else if (* (int *) mutex == BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_RECURSIVE;
        else if (* (int *) mutex == BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_ERRORCHECK;
    }

    pthread_mutex_t *p = malloc(sizeof(pthread_mutex_t));
    if (!p) return ENOMEM;

    pthread_mutexattr_t mutattr;
    pthread_mutexattr_init(&mutattr);
    pthread_mutexattr_settype(&mutattr, kind);
    ret = pthread_mutex_init(p, &mutattr);
    pthread_mutexattr_destroy(&mutattr);

    if (ret == 0) {
        // real_ptr ANTES de rememberObject, para conservar la invariante del
        // codigo original: "esta en el registro" implica "real_ptr ya es un
        // puntero valido". Al reves, otro hilo (o esta misma funcion mas tarde)
        // podria ver el objeto como registrado y usar un real_ptr que todavia es
        // la constante del inicializador estatico.
        __atomic_store_n(&mutex->real_ptr, p, __ATOMIC_RELEASE);
        rememberObject(mutex);
    } else {
        free(p);
        l_error("mutex initialization for %p has failed", mutex);
    }

    return ret;
}

// null check for `cond` param must be performed before this, `attr` is fine as null
PTHR_INLINE int _cond_t_static_init(pthread_cond_t_bionic * cond, const pthread_condattr_t * attr) {
    int ret = 0;

    if (isObjectInitialized(cond)) {
        //logv_debug("cond already initialized: %p", cond);
        return ret;
    }

    pthread_cond_t *p = malloc(sizeof(pthread_cond_t));
    if (!p) return ENOMEM;

    ret = pthread_cond_init(p, attr);

    if (ret == 0) {
        __atomic_store_n(&cond->real_ptr, p, __ATOMIC_RELEASE);  // ver el mutex
        rememberObject(cond);
    } else {
        free(p);
        l_error("cond initialization for %p has failed", cond);
    }

    return ret;
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param) {
    int ret;

    // Honor the game's requested stack size when it bothers to set one;
    // otherwise use a generous default. (The engine's savegame workers pass
    // attr == NULL, so they land on the default.)
    size_t stacksize = 512 * 1024;

    if (attr) {
        _attr_t_static_init((pthread_attr_t_bionic *) attr);
        if (attr->real_ptr) {
            size_t s = 0;
            if (pthread_attr_getstacksize(attr->real_ptr, &s) == 0 && s >= 16 * 1024) {
                stacksize = s;
            }
        }
    }

    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stacksize);

    // Force DETACHED regardless of what the game asked for. This engine
    // spawns one fire-and-forget worker per savegame job (Savegame::UpdateJobs,
    // ~190 of them while writing the initial save) via updateJob_thread::Start(),
    // which overwrites the stored handle each call and NEVER pthread_join()s
    // the previous one. Left joinable, every finished worker stays a zombie
    // holding a kernel thread slot; after ~120 the kernel returns EAGAIN
    // ("Not Created" spam, hung save flush, black screen). Detached threads
    // self-reap on exit, so only the few actually running at once cost a slot.
    // Any later pthread_join() on these is a harmless no-op (see below).
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);

    // Safety net in case we still momentarily run out of thread slots: give
    // already-finished detached workers a chance to reap, then retry. Never
    // run the start routine synchronously -- Savegame::UpdateJobs() has an
    // inQueue reentrancy guard and is driven by a `while (UpdateJobs())` flush
    // loop, so running it inline on the caller just spins without draining.
    for (int attempt = 0; ; ++attempt) {
        ret = pthread_create(thread, &a, start, param);
        if (ret != EAGAIN || attempt >= 20) {
            break;
        }
        sceKernelDelayThread(2 * 1000); // 2 ms
    }

    pthread_attr_destroy(&a);

    if (ret != 0) {
        l_error("pthread_create failed: %d (start=%p, param=%p)", ret, start, param);
    }

    return ret;
}

int pthread_join_soloader(pthread_t thread, void **value_ptr)
{
    // We create every thread detached (see pthread_create_soloader), so a real
    // join would fail with EINVAL. This engine only ever fire-and-forgets its
    // workers, so treat join as a successful no-op instead of surfacing an
    // error the game doesn't expect.
    int ret = pthread_join(thread, value_ptr);
    if (ret != 0) {
        if (value_ptr) {
            *value_ptr = NULL;
        }
        return 0;
    }
    return ret;
}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_init(attr);
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type)
{
    return pthread_mutexattr_settype(attr, type);
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_destroy(attr);
}

int pthread_kill_soloader(pthread_t thread, int sig)
{
    return pthread_kill(thread, sig);
}

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr)
{
    if (!uid) return EINVAL;
    return _mutex_t_static_init(uid, attr);
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return 0;
    // PTHR_IS_LIVE_PTR, no un simple chequeo de NULL: un mutex que el motor
    // inicializo estaticamente y nunca uso trae una constante de inicializador
    // (0x4000/0x8000) en real_ptr, y pasarsela a pthread_mutex_destroy()/free()
    // es dereferenciar un puntero salvaje.
    // forgetObject SIEMPRE, pase lo que pase con real_ptr. Una version anterior
    // de este fix hacia early-return aca sin desregistrar: la entrada quedaba
    // viva para siempre y, cuando el heap reutilizaba esa direccion para otro
    // objeto, isObjectInitialized devolvia true sobre un mutex nuevo cuyo
    // real_ptr todavia era 0 -> pthread_mutex_lock(0) -> crash en el arranque.
    forgetObject(mutex);
    if (!PTHR_IS_LIVE_PTR(mutex->real_ptr)) {
        mutex->real_ptr = 0x0;
        return 0;
    }
    int ret = pthread_mutex_destroy(mutex->real_ptr);
    free(mutex->real_ptr);
    mutex->real_ptr = 0x0;
    return ret;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    _mutex_t_static_init(mutex, NULL);
    return pthread_mutex_lock(mutex->real_ptr);
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    _mutex_t_static_init(mutex, NULL);
    return pthread_mutex_trylock(mutex->real_ptr);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    if (!PTHR_IS_LIVE_PTR(mutex->real_ptr)) return EINVAL;
    return pthread_mutex_unlock(mutex->real_ptr);
}



int pthread_condattr_init_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_init(attr);
}

int pthread_condattr_destroy_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_destroy(attr);
}

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond,
                               const pthread_condattr_t *attr)
{
    if (!cond) return EINVAL;

    return _cond_t_static_init(cond, attr);
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return 0;
    forgetObject(cond);   // siempre -- ver el comentario del mutex
    if (!PTHR_IS_LIVE_PTR(cond->real_ptr)) {
        cond->real_ptr = 0x0;
        return 0;
    }
    int ret = pthread_cond_destroy(cond->real_ptr);
    free(cond->real_ptr);
    cond->real_ptr = 0x0;
    return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;

    _cond_t_static_init(cond, NULL);

    return pthread_cond_signal(cond->real_ptr);
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex, struct timespec *abstime)
{
    if (!cond || !mutex) return EINVAL;

    _cond_t_static_init(cond, NULL);
    _mutex_t_static_init(mutex, NULL);

    return pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr, abstime);
}


int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex)
{
    if (!cond || !mutex) return EINVAL;

    _cond_t_static_init(cond, NULL);
    _mutex_t_static_init(mutex, NULL);

    return pthread_cond_wait(cond->real_ptr, mutex->real_ptr);
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;

    _cond_t_static_init(cond, NULL);

    return pthread_cond_broadcast(cond->real_ptr);
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return EINVAL;

    return _attr_t_static_init(attr);
}

int pthread_attr_destroy_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return 0;
    if (attr->magic != 0x42424242) return 0;

    int ret = pthread_attr_destroy(attr->real_ptr);
    free(attr->real_ptr);
    attr->magic = 0x0;

    return ret;
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t_bionic *attr, int state)
{
    if (!attr) return -1;
    _attr_t_static_init(attr);
    return pthread_attr_setdetachstate(attr->real_ptr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
    if (!attr) return -1;
    _attr_t_static_init(attr);
    return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}

int pthread_setschedparam_soloader(pthread_t thread, int policy,
                                   const struct sched_param *param)
{
   return pthread_setschedparam(thread, policy, param);
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy,
                                   struct sched_param *param)
{
    return pthread_getschedparam(thread, policy, param);
}

int pthread_detach_soloader(pthread_t thread)
{
    if (thread == (pthread_t)0xDEADBEEF) return 0;
    return pthread_detach(thread);
}

int pthread_equal_soloader(const pthread_t t1, const pthread_t t2)
{
    if (t1 == t2)
        return 1;
    if (!t1 || !t2)
        return 0;
    return pthread_equal(t1, t2);
}

pthread_t pthread_self_soloader()
{
    return pthread_self();
}

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine)
        return -1;
    if (__sync_lock_test_and_set(once_control, 1) == 0)
        (*init_routine)();
    return 0;
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(pthread_t thread, const char* thread_name) {
    if (thread == 0 || thread_name == NULL) {
        return EINVAL;
    }
    size_t thread_name_len = strlen(thread_name);
    if (thread_name_len >= MAX_TASK_COMM_LEN) {
        return ERANGE;
    }

    sceClibPrintf("PTHREAD: pthread_setname_np with name %s for thread:0x%x\n", thread_name, pthread_self());

    return 0;
}

int sem_destroy_soloader(int * uid) {
    if (sceKernelDeleteSema(*uid) < 0)
        return -1;
    return 0;
}

int sem_getvalue_soloader (int * uid, int * sval) {
    SceKernelSemaInfo info;
    info.size = sizeof(SceKernelSemaInfo);

    if (sceKernelGetSemaInfo(*uid, &info) < 0) return -1;
    if (!sval) sval = malloc(sizeof(int32_t));
    *sval = info.currentCount;
    return 0;
}

int sem_init_soloader (int * uid, int pshared, unsigned int value) {
    *uid = sceKernelCreateSema("sema", 0, (int) value, 0x7fffffff, NULL);
    if (*uid < 0)
        return -1;
    return 0;
}

int sem_post_soloader (int * uid) {
    if (sceKernelSignalSema(*uid, 1) < 0)
        return -1;
    return 0;
}

int sem_timedwait_soloader (int * uid, const struct timespec * abstime) {
    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) >= 0)
        return 0;
    if (!abstime) return -1;
    long long now = (long long) current_timestamp_ms() * 1000; // us
    long long _timeout = abstime->tv_sec * 1000 * 1000 + abstime->tv_nsec / 1000; // us
    if (_timeout-now >= 0) return -1;
    uint timeout_real = _timeout - now;
    if (sceKernelWaitSema(*uid, 1, &timeout_real) < 0)
        return -1;
    return 0;
}

int sem_trywait_soloader (int * uid) {
    uint timeout = 1000;
    if (sceKernelWaitSema(*uid, 1, &timeout) < 0)
        return -1;
    return 0;
}

int sem_wait_soloader (int * uid) {
    if (sceKernelWaitSema(*uid, 1, NULL) < 0)
        return -1;
    return 0;
}
