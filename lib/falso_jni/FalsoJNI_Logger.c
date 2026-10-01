/*
 * FalsoJNI_Logger.c
 *
 * Fake Java Native Interface, providing JavaVM and JNIEnv objects.
 *
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <malloc.h>
#include <string.h>

#include "FalsoJNI_Logger.h"
#include "FalsoJNI.h"

#define COLOR_RED ""
#define COLOR_ORANGE ""
#define COLOR_BLUE ""
#define COLOR_END ""

extern void log_message(const char *level, const char *fmt, ...);

// Route FalsoJNI output into the port's log (stdout goes nowhere on Switch).
static void fjni_log_sink(char *text) {
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) text[--len] = 0;
    log_message("JNI", "%s", text);
}

static pthread_mutex_t _fjni_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static char _fjni_log_buffer_1[2048];
static char _fjni_log_buffer_2[2048];

#define LOG_LOCK   pthread_mutex_lock(&_fjni_log_mutex);
#define LOG_UNLOCK pthread_mutex_unlock(&_fjni_log_mutex);

#define LOG_PRINT \
    va_list list; \
    va_start(list, fmt); \
    vsnprintf(_fjni_log_buffer_2, sizeof(_fjni_log_buffer_2) - 1, _fjni_log_buffer_1, list); \
    va_end(list); \
    fjni_log_sink(_fjni_log_buffer_2);

void _fjni_log_info(const char *fi, int li, const char *fn, const char* fmt, ...) {
    (void)fi; (void)li; (void)fn;
#if FALSOJNI_DEBUGLEVEL <= FALSOJNI_DEBUG_INFO
    LOG_LOCK
    snprintf(_fjni_log_buffer_1, sizeof(_fjni_log_buffer_1) - 1,
             "%s[INFO] %s%s\n", COLOR_BLUE, fmt, COLOR_END);
    LOG_PRINT
    LOG_UNLOCK
#endif
}

void _fjni_log_warn(const char *fi, int li, const char *fn, const char* fmt, ...) {
#if FALSOJNI_DEBUGLEVEL <= FALSOJNI_DEBUG_WARN
    LOG_LOCK
    snprintf(_fjni_log_buffer_1, sizeof(_fjni_log_buffer_1) - 1,
             "%s[WARN][%s:%d][%s] %s%s\n", COLOR_ORANGE, fi, li, fn, fmt, COLOR_END);
    LOG_PRINT
    LOG_UNLOCK
#else
    (void)fi; (void)li; (void)fn; (void)fmt;
#endif
}

void _fjni_log_debug(const char *fi, int li, const char *fn, const char* fmt, ...) {
#if FALSOJNI_DEBUGLEVEL <= FALSOJNI_DEBUG_ALL
    LOG_LOCK
    snprintf(_fjni_log_buffer_1, sizeof(_fjni_log_buffer_1) - 1,
             "[DBG][%s:%d][%s] %s\n", fi, li, fn, fmt);
    LOG_PRINT
    LOG_UNLOCK
#else
    (void)fi; (void)li; (void)fn; (void)fmt;
#endif
}

void _fjni_log_error(const char *fi, int li, const char *fn, const char* fmt, ...) {
#if FALSOJNI_DEBUGLEVEL <= FALSOJNI_DEBUG_ERROR
    LOG_LOCK
    snprintf(_fjni_log_buffer_1, sizeof(_fjni_log_buffer_1) - 1,
             "%s[ERROR][%s:%d][%s] %s%s\n", COLOR_RED, fi, li, fn, fmt, COLOR_END);
    LOG_PRINT
    LOG_UNLOCK
#else
    (void)fi; (void)li; (void)fn; (void)fmt;
#endif
}
