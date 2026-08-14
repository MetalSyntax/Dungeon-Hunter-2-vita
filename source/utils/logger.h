/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  logger.h
 * @brief Logging utilities.
 */

#ifndef SOLOADER_LOGGER_H
#define SOLOADER_LOGGER_H

#ifdef __cplusplus
extern "C" {
#endif

// Niveles de log configurables
#define LOG_LEVEL_QUIET   0  // Solo FATAL y ERROR críticos
#define LOG_LEVEL_MINIMAL 1  // RELEASE por defecto: ERROR, FATAL, WARN, SUCCESS (hitos de arranque). Sin spam de assets ni diag por frame.
#define LOG_LEVEL_VERBOSE 2  // DEBUG por defecto: Todo (INFO, DEBUG, [fcache], fopen/fclose, diagnósticos periódicos por frame).

#ifndef ACTIVE_LOG_LEVEL
  #if defined(DEBUG_SOLOADER)
    #define ACTIVE_LOG_LEVEL LOG_LEVEL_VERBOSE
  #elif defined(RELEASE_BUILD)
    #define ACTIVE_LOG_LEVEL LOG_LEVEL_MINIMAL
  #else
    #define ACTIVE_LOG_LEVEL LOG_LEVEL_VERBOSE
  #endif
#endif

#define LT_DEBUG   0
#define LT_INFO    1
#define LT_WARN    2
#define LT_ERROR   3
#define LT_FATAL   4
#define LT_SUCCESS 5
#define LT_WAIT    6

// Macros de logging compiladas condicionalmente según ACTIVE_LOG_LEVEL
#if ACTIVE_LOG_LEVEL >= LOG_LEVEL_VERBOSE
  #define l_debug(...)   _log_print(LT_DEBUG,   __VA_ARGS__)
  #define l_info(...)    _log_print(LT_INFO,    __VA_ARGS__)
#else
  #define l_debug(...)   ((void)0)
  #define l_info(...)    ((void)0)
#endif

#if ACTIVE_LOG_LEVEL >= LOG_LEVEL_MINIMAL
  #define l_warn(...)    _log_print(LT_WARN,    __VA_ARGS__)
  #define l_success(...) _log_print(LT_SUCCESS, __VA_ARGS__)
  #define l_wait(...)    _log_print(LT_WAIT,    __VA_ARGS__)
#else
  #define l_warn(...)    ((void)0)
  #define l_success(...) ((void)0)
  #define l_wait(...)    ((void)0)
#endif

#define l_error(...)   _log_print(LT_ERROR,   __VA_ARGS__)
#define l_fatal(...)   _log_print(LT_FATAL,   __VA_ARGS__)

void _log_print(int t, const char* fmt, ...)
                __attribute__ ((format (printf, 2, 3)));

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_LOGGER_H
