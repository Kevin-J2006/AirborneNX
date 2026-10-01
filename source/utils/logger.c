#include "logger.h"
#include "init.h"
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

static FILE *s_logfile = NULL;
static Mutex s_log_lock;

void log_init(void) {
    s_logfile = fopen(DATA_PATH "airbornenx.log", "w");
    if (s_logfile) {
        fprintf(s_logfile, "\n================ AirborneNX Start ================\n");
        fflush(s_logfile);
    }
}

void log_vmessage(const char *level, const char *fmt, va_list args) {
    char buf[1024];
    int prefix_len = snprintf(buf, sizeof(buf), "[%s][%s] ", LOG_TAG, level);
    if (prefix_len > 0 && (size_t)prefix_len < sizeof(buf)) {
        vsnprintf(buf + prefix_len, sizeof(buf) - prefix_len - 2, fmt, args);
    }

    size_t len = strlen(buf);
    buf[len] = '\n';
    buf[len + 1] = '\0';
    len++;

    mutexLock(&s_log_lock);

    // Collapse runs of identical lines (the engine can retry a failing call
    // every frame, which would otherwise flood the SD card).
    static char s_last[1024];
    static unsigned s_repeats = 0;
    if (strcmp(buf, s_last) == 0) {
        s_repeats++;
        mutexUnlock(&s_log_lock);
        return;
    }
    if (s_repeats > 0 && s_logfile) {
        fprintf(s_logfile, "[%s] (previous line repeated %u more times)\n", LOG_TAG, s_repeats);
    }
    s_repeats = 0;
    snprintf(s_last, sizeof(s_last), "%s", buf);

    // 1. Output to Switch OS debug channel (intercepted by Yuzu/Eden/Ryujinx log!)
    svcOutputDebugString(buf, len);


    // 3. Output to log file on SD
    if (s_logfile) {
        static u64 s_last_flush = 0;
        fputs(buf, s_logfile);
        u64 now = armGetSystemTick();
        if (armTicksToNs(now - s_last_flush) > 250000000ULL) {
            fflush(s_logfile);
            s_last_flush = now;
        }
    }

    mutexUnlock(&s_log_lock);
}

void log_message(const char *level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_vmessage(level, fmt, args);
    va_end(args);
}

void fatal_error(const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int prefix_len = snprintf(buf, sizeof(buf), "[%s][FATAL] ", LOG_TAG);
    if (prefix_len > 0 && (size_t)prefix_len < sizeof(buf)) {
        vsnprintf(buf + prefix_len, sizeof(buf) - prefix_len - 2, fmt, args);
    }
    va_end(args);

    size_t len = strlen(buf);
    buf[len] = '\n';
    buf[len + 1] = '\0';
    len++;

    svcOutputDebugString(buf, len);
    fputs(buf, stderr);
    fflush(stderr);

    if (s_logfile) {
        fputs(buf, s_logfile);
        fflush(s_logfile);
        fclose(s_logfile);
        s_logfile = NULL;
    }

    svcSleepThread(5000000000ULL);
    exit(1);
}
