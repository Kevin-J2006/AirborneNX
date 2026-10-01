#include "log.h"
#include "../utils/logger.h"
#include <stdio.h>
#include <stdarg.h>

int __android_log_write(int prio, const char *tag, const char *text) {
    (void)prio;
    log_message("Android", "[%s] %s", tag ? tag : "Log", text ? text : "");
    return 0;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    log_message("Android", "[%s] %s", tag ? tag : "Log", msg);
    return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    (void)prio;
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    log_message("Android", "[%s] %s", tag ? tag : "Log", msg);
    return 0;
}

void __android_log_assert(const char *cond, const char *tag, const char *fmt, ...) {
    char msg[1024] = {0};
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
    }
    log_message("ASSERT", "[%s] condition (%s) failed: %s",
                tag ? tag : "Android", cond ? cond : "", msg);
}
