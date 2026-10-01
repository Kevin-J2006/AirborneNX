#ifndef __REIMPL_LOG_H__
#define __REIMPL_LOG_H__

#include <stdarg.h>

int __android_log_write(int prio, const char *tag, const char *text);
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap);
void __android_log_assert(const char *cond, const char *tag, const char *fmt, ...);

#endif // __REIMPL_LOG_H__
