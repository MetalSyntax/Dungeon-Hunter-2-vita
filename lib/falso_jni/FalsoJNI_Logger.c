/*
 * FalsoJNI_Logger.c
 *
 * Fake Java Native Interface, providing JavaVM and JNIEnv objects.
 *
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * Routed into this port's own game_log() (main.c) instead of a private
 * sceClibPrintf-only path, so JNI dispatch traces land in
 * ux0:data/zenonia-2/log.txt too, not just the console.
 */

#include <stdarg.h>

#include "FalsoJNI_Logger.h"
#include "FalsoJNI.h"

extern void game_log(const char *fmt, ...);

#define FJNI_LOG_MSG_SIZE 512

void _fjni_log_info(const char *fi, int li, const char *fn, const char* fmt, ...) {
#if FALSOJNI_DEBUGLEVEL <= FALSOJNI_DEBUG_INFO
    char msg[FJNI_LOG_MSG_SIZE];
    va_list list;
    va_start(list, fmt);
    vsnprintf(msg, sizeof(msg), fmt, list);
    va_end(list);
    game_log("[JNI][%s] %s\n", fn, msg);
#endif
}

void _fjni_log_warn(const char *fi, int li, const char *fn, const char* fmt, ...) {
#if FALSOJNI_DEBUGLEVEL <= FALSOJNI_DEBUG_WARN
    char msg[FJNI_LOG_MSG_SIZE];
    va_list list;
    va_start(list, fmt);
    vsnprintf(msg, sizeof(msg), fmt, list);
    va_end(list);
    game_log("[JNI WARN][%s] %s\n", fn, msg);
#endif
}

void _fjni_log_debug(const char *fi, int li, const char *fn, const char* fmt, ...) {
#if FALSOJNI_DEBUGLEVEL <= FALSOJNI_DEBUG_ALL
    char msg[FJNI_LOG_MSG_SIZE];
    va_list list;
    va_start(list, fmt);
    vsnprintf(msg, sizeof(msg), fmt, list);
    va_end(list);
    game_log("[JNI][%s] %s\n", fn, msg);
#endif
}

void _fjni_log_error(const char *fi, int li, const char *fn, const char* fmt, ...) {
#if FALSOJNI_DEBUGLEVEL <= FALSOJNI_DEBUG_ERROR
    char msg[FJNI_LOG_MSG_SIZE];
    va_list list;
    va_start(list, fmt);
    vsnprintf(msg, sizeof(msg), fmt, list);
    va_end(list);
    game_log("[JNI ERR][%s] %s\n", fn, msg);
#endif
}
