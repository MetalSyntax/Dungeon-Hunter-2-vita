/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/logger.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>

#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#define COLOR_RED    "\x1B[38;5;196m"
#define COLOR_PINK   "\x1B[38;5;212m"
#define COLOR_ORANGE "\x1B[38;5;202m"
#define COLOR_BLUE   "\x1B[38;5;32m"
#define COLOR_GREEN  "\x1B[32m"
#define COLOR_CYAN   "\x1B[36m"

#define COLOR_END    "\033[0m"

static SceKernelLwMutexWork _log_mutex;
static atomic_bool _log_mutex_ready = ATOMIC_VAR_INIT(false);
static FILE *log_file = NULL;

// Buffer A is used to adjust the format string.
static char buffer_a[2048];
// Buffer B is used to compile the final log using the updated format string.
static char buffer_b[2048];
// Buffer C is used for file output (no colors).
static char buffer_c[2048];
static char buffer_d[2048];

void _log_print(int t, const char* fmt, ...) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) {
            sceClibPrintf("Error: failed to create log mutex: 0x%x\n", ret);
            return;
        }
        
        sceIoMkdir("ux0:data/dungeon-hunter-2", 0777);
        sceIoMkdir("ux0:data/dungeon-hunter-2/logs", 0777);
        
        char log_path[128];
        for (int i = 0; i < 1000; ++i) {
            snprintf(log_path, sizeof(log_path), "ux0:data/dungeon-hunter-2/logs/log_%03d.txt", i);
            FILE *f = fopen(log_path, "r");
            if (f) {
                fclose(f);
            } else {
                log_file = fopen(log_path, "w");
                break;
            }
        }
        
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);

    switch (t) {
        case LT_DEBUG:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s• debug%s    %s\n", COLOR_PINK, COLOR_END, fmt);
            sceClibSnprintf(buffer_c, sizeof(buffer_c), " • debug    %s\n", fmt); break;
        case LT_INFO:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %sℹ info%s     %s\n", COLOR_BLUE, COLOR_END, fmt);
            sceClibSnprintf(buffer_c, sizeof(buffer_c), " ℹ info     %s\n", fmt); break;
        case LT_WARN:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⚠ warning%s  %s\n", COLOR_ORANGE, COLOR_END, fmt);
            sceClibSnprintf(buffer_c, sizeof(buffer_c), " ⚠ warning  %s\n", fmt); break;
        case LT_ERROR:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s⨯ error%s    %s\n", COLOR_RED, COLOR_END, fmt);
            sceClibSnprintf(buffer_c, sizeof(buffer_c), " ⨯ error    %s\n", fmt); break;
        case LT_FATAL:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! fatal%s    %s\n", COLOR_RED, COLOR_END, fmt);
            sceClibSnprintf(buffer_c, sizeof(buffer_c), " ! fatal    %s\n", fmt); break;
        case LT_SUCCESS:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s! success%s  %s\n", COLOR_GREEN, COLOR_END, fmt);
            sceClibSnprintf(buffer_c, sizeof(buffer_c), " ! success  %s\n", fmt); break;
        case LT_WAIT:
            sceClibSnprintf(buffer_a, sizeof(buffer_a), " %s… waiting%s  %s\n", COLOR_CYAN, COLOR_END, fmt);
            sceClibSnprintf(buffer_c, sizeof(buffer_c), " … waiting  %s\n", fmt); break;
        default:
            if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
                sceKernelUnlockLwMutex(&_log_mutex, 1);
            }
            return;
    }

    va_list list;
    
    va_start(list, fmt);
    sceClibVsnprintf(buffer_b, sizeof(buffer_b), buffer_a, list);
    va_end(list);
    
    va_start(list, fmt);
    sceClibVsnprintf(buffer_d, sizeof(buffer_d), buffer_c, list);
    va_end(list);
    
    sceClibPrintf(buffer_b);
    
    if (log_file) {
        fprintf(log_file, "%s", buffer_d);
        fflush(log_file);
    }

    if (atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        sceKernelUnlockLwMutex(&_log_mutex, 1);
    }
}

