/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/sys.h"

#include <sys/errno.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <string.h>
#include <stdint.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/rtc.h>
#include <stdlib.h>

#include "utils/utils.h"
#include "utils/logger.h"

#define BIONIC_CLOCK_REALTIME           0
#define BIONIC_CLOCK_MONOTONIC          1
#define BIONIC_CLOCK_PROCESS_CPUTIME_ID 2
#define BIONIC_CLOCK_THREAD_CPUTIME_ID  3
#define BIONIC_CLOCK_MONOTONIC_RAW      4
#define BIONIC_CLOCK_REALTIME_COARSE    5
#define BIONIC_CLOCK_MONOTONIC_COARSE   6
#define BIONIC_CLOCK_BOOTTIME           7
#define BIONIC_CLOCK_REALTIME_ALARM     8
#define BIONIC_CLOCK_BOOTTIME_ALARM     9
#define BIONIC_CLOCK_SGI_CYCLE         10
#define BIONIC_CLOCK_TAI               11

// 1969 years in microseconds, used to adjust SCE tick to UNIX timestamp
#define __epoch 62135587294000000

int clock_gettime_soloader(clockid_t clock_id, struct timespec * tp) {
    switch (clock_id) {
        case BIONIC_CLOCK_MONOTONIC:
        case BIONIC_CLOCK_MONOTONIC_RAW:
        case BIONIC_CLOCK_MONOTONIC_COARSE:
        case BIONIC_CLOCK_BOOTTIME:
        case BIONIC_CLOCK_BOOTTIME_ALARM:
        case BIONIC_CLOCK_SGI_CYCLE:
        case BIONIC_CLOCK_PROCESS_CPUTIME_ID:
        case BIONIC_CLOCK_THREAD_CPUTIME_ID: {
            uint64_t proctime = sceKernelGetProcessTimeWide();

            tp->tv_sec = (proctime / 1000000);
            tp->tv_nsec = ((proctime - (tp->tv_sec * 1000000)) * 1000);
            break;
        }
        case BIONIC_CLOCK_REALTIME:
        case BIONIC_CLOCK_REALTIME_COARSE:
        case BIONIC_CLOCK_REALTIME_ALARM:
        case BIONIC_CLOCK_TAI: {
            SceRtcTick tick;
            sceRtcGetCurrentTick(&tick);
            tick.tick -= __epoch;

            tp->tv_sec = (tick.tick / 1000000);
            tp->tv_nsec = ((tick.tick - (tp->tv_sec * 1000000)) * 1000);
            break;
        }
        default:
            l_error("clock_gettime / unexpected clock id %i", clock_id);
    }

    return 0;
}

int clock_getres_soloader(clockid_t clock_id, struct timespec * res) {
    res->tv_sec = 0;
    res->tv_nsec = 1000;
    return 0;
}

clock_t clock_soloader(void) {
    return sceKernelGetProcessTimeLow();
}

int sigaction(int signum, const struct sigaction * act, struct sigaction * oldact) {
    l_warn("sigaction(%i, ...): not implemented", signum);
    return 0;
}

int __system_property_get_soloader(const char *name, char *value) {
    l_warn("__system_property_get(%s, %p): not implemented", name, value);
    strncpy(value, "psvita", 7);
    return 7;
}

void assert2(const char* f, int l, const char* func, const char* msg) {
    l_fatal("[%s:%i][%s] Assertion failed: %s", f, l, func, msg);
}

void syscall(int c) {
    l_warn("syscall(%i): not implemented", c);
}

void __stack_chk_fail_soloader() {
    l_fatal("Stack collapsed at address %p", __builtin_return_address(0));
}

// Salir SIN correr la cadena de atexit/__cxa_atexit.
//
// `exit()` de newlib corre todos los handlers registrados por __cxa_atexit --
// que en un port soloader incluye TODOS los destructores estaticos de C++ del
// .so de Android, porque dynlib.c cablea __cxa_atexit al de newlib. Esos
// destructores crashean: cuando el usuario cerro el juego desde el propio
// juego (log_010.log + su .psp2dmp), el dump dio prefetch abort 0x30003 con
// PC=0x0 -- salto a un puntero de funcion nulo -- en un hilo 'pthread', con la
// pila llena de destructores de locale de STLport (_Locale_time_destroy,
// _Locale_messages_destroy).
//
// Correr esos destructores no aporta NADA aca: el proceso se esta muriendo y el
// kernel de Vita libera memoria, hilos, puerto de audio y contexto GXM solo. Es
// riesgo puro a cambio de cero beneficio, asi que se va derecho a
// sceKernelExitProcess. El l_fatal de arriba ya baja el log a disco (el logger
// flushea todo salvo DEBUG/INFO), asi que no se pierde el rastro.
//
// Nota: el camino de salida normal del loop principal (START+SELECT en main.c)
// no pasa por aca -- termina en sceKernelExitDeleteThread, que tampoco corre
// destructores. Este wrapper cubre el caso de que el MOTOR pida salir.
void abort_soloader() {
    l_fatal("Abort called from address %p -- saliendo directo, sin destructores estaticos",
            __builtin_return_address(0));
    sceKernelExitProcess(1);
    __builtin_unreachable();
}

void exit_soloader(int status) {
    l_fatal("Exit(%i) called from %p -- saliendo directo, sin destructores estaticos",
            status, __builtin_return_address(0));
    sceKernelExitProcess(status);
    __builtin_unreachable();
}

int __atomic_dec(volatile int *ptr) {
    return __sync_fetch_and_sub(ptr, 1);
}

int __atomic_inc(volatile int *ptr) {
    return __sync_fetch_and_add(ptr, 1);
}

int __atomic_swap(int new_value, volatile int *ptr) {
    int old_value;
    do {
        old_value = *ptr;
    } while (__sync_val_compare_and_swap(ptr, old_value, new_value) != old_value);
    return old_value;
}

int __atomic_cmpxchg(int old_value, int new_value, volatile int* ptr) {
    /* We must return 0 on success */
    return __sync_val_compare_and_swap(ptr, old_value, new_value) != old_value;
}

char * getenv_soloader(const char * var) {
    l_warn("getenv(\"%s\"): not implemented.", var);
    return NULL;
}

int setenv_soloader(const char * name, const char * value, int overwrite) {
    l_warn("setenv(\"%s\", \"%s\"): not implemented.", name, value);
    return 0;
}

int getpagesize(void) {
    return PAGE_SIZE;
}

// El motor SI importa usleep y nanosleep (confirmado con
// `nm -D --undefined-only` sobre libDungeonHunter2.so), y los dos estaban
// cableados a ret0 en source/dynlib.c, o sea que NUNCA dormian. Cualquier
// espera del motor con la forma `while (!condicion) usleep(x)` -- patron
// clasico esperando un worker, una carga de asset o un buffer de audio -- se
// convertia asi en un busy-spin al 100% de CPU, quemando el core en vez de
// cederlo. Con el hilo principal fijado a USER_0 eso es especialmente caro.
//
// Se sigue el mismo criterio que Asphalt-5-Vita (source/reimpl/sys.c:85-95),
// que es el port comparable que llego a 60 FPS: las esperas de menos de 1ms se
// saltean (si es un spinlock corto, dormirlo cuesta mas que girar, porque el
// quantum del scheduler de Vita es mucho mayor que eso), y de 1ms para arriba
// se duerme de verdad.
int usleep_soloader(useconds_t usec) {
    if (usec < 1000) {
        return 0;
    }
    return sceKernelDelayThread(usec);
}

int nanosleep_soloader(const struct timespec *req, struct timespec *rem) {
    if (!req) {
        errno = EINVAL;
        return -1;
    }
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    if (req->tv_sec == 0 && req->tv_nsec < 1000000) {
        return 0;   // menos de 1ms: ver arriba
    }
    // sceKernelDelayThread toma microsegundos y su argumento es de 32 bits, asi
    // que se topea en ~71 minutos; ninguna espera real del motor se acerca, pero
    // se satura por las dudas en vez de desbordar.
    uint64_t us = (uint64_t) req->tv_sec * 1000000ull + (uint64_t) req->tv_nsec / 1000ull;
    if (us > 0xFFFFFFFFull) us = 0xFFFFFFFFull;
    return sceKernelDelayThread((SceUInt) us);
}

unsigned int sleep_soloader(unsigned int seconds) {
    if (seconds == 0) return 0;
    uint64_t us = (uint64_t) seconds * 1000000ull;
    if (us > 0xFFFFFFFFull) us = 0xFFFFFFFFull;
    sceKernelDelayThread((SceUInt) us);
    return 0;   // 0 = se durmio todo el tiempo pedido, sin interrupciones
}
