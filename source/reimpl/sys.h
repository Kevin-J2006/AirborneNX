#ifndef __REIMPL_SYS_H__
#define __REIMPL_SYS_H__

#include <stdint.h>
#include <stddef.h>
#include <time.h>

// Translates a newlib errno value to the Linux value bionic code expects.
int sys_errno_to_linux(int host_errno);

int sys_clock_gettime(int clk_id, struct timespec *tp);
int sys_clock_getres(int clk_id, struct timespec *tp);
long sys_sysconf(int name);
long sys_syscall(long num, ...);
int sys_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data);

// Network: the port runs offline, so every entry point fails cleanly with a
// Linux-style error instead of handing bionic-layout structs to BSD sockets.
int sys_socket(int domain, int type, int protocol);
int sys_net_fail(void);
void *sys_net_null(void);
int sys_getaddrinfo(const char *node, const char *service, const void *hints, void **res);
void sys_freeaddrinfo(void *res);

#endif // __REIMPL_SYS_H__
