#ifndef __LOGGER_H__
#define __LOGGER_H__

#include <stdio.h>
#include <stdarg.h>

#define LOG_TAG "AirborneNX"

#define l_info(fmt, ...)  log_message("INFO", fmt, ##__VA_ARGS__)
#define l_warn(fmt, ...)  log_message("WARN", fmt, ##__VA_ARGS__)
#define l_error(fmt, ...) log_message("ERROR", fmt, ##__VA_ARGS__)

// Debug builds (make DEBUG=1) trace every guest thread, failed file call and
// GL upload to the SD card; a release build must not, the writes stutter.
#ifndef AIRBORNE_DEBUG
#define AIRBORNE_DEBUG 0
#endif
#if AIRBORNE_DEBUG
#define l_debug(fmt, ...) log_message("DEBUG", fmt, ##__VA_ARGS__)
#else
#define l_debug(fmt, ...) ((void)0)
#endif

void log_init(void);
void log_message(const char *level, const char *fmt, ...);
void log_vmessage(const char *level, const char *fmt, va_list args);
void fatal_error(const char *fmt, ...);

#endif // __LOGGER_H__
