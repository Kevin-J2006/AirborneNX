#include "sys.h"
#include "pthr.h"
#include "../utils/logger.h"
#include "so_util/so_util.h"

#include <switch.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>

// ============================================================================
// errno: newlib -> Linux numbering (1..34 are shared)
// ============================================================================
int sys_errno_to_linux(int e) {
    switch (e) {
        case EDEADLK:         return 35;
        case ENAMETOOLONG:    return 36;
        case ENOLCK:          return 37;
        case ENOSYS:          return 38;
        case ENOTEMPTY:       return 39;
        case ELOOP:           return 40;
        case EOVERFLOW:       return 75;
        case EILSEQ:          return 84;
        case ENOTSOCK:        return 88;
        case EDESTADDRREQ:    return 89;
        case EMSGSIZE:        return 90;
        case EPROTOTYPE:      return 91;
        case ENOPROTOOPT:     return 92;
        case EPROTONOSUPPORT: return 93;
        case EOPNOTSUPP:      return 95;
        case EAFNOSUPPORT:    return 97;
        case EADDRINUSE:      return 98;
        case EADDRNOTAVAIL:   return 99;
        case ENETDOWN:        return 100;
        case ENETUNREACH:     return 101;
        case ENETRESET:       return 102;
        case ECONNABORTED:    return 103;
        case ECONNRESET:      return 104;
        case ENOBUFS:         return 105;
        case EISCONN:         return 106;
        case ENOTCONN:        return 107;
        case ETIMEDOUT:       return 110;
        case ECONNREFUSED:    return 111;
        case EHOSTDOWN:       return 112;
        case EHOSTUNREACH:    return 113;
        case EALREADY:        return 114;
        case EINPROGRESS:     return 115;
        case ECANCELED:       return 125;
        default:              return e;
    }
}

// ============================================================================
// Clocks (bionic ids: 0 REALTIME, 1 MONOTONIC, 2/3 CPU time, 4 MONOTONIC_RAW,
// 5 REALTIME_COARSE, 6 MONOTONIC_COARSE, 7 BOOTTIME)
// ============================================================================
int sys_clock_gettime(int clk_id, struct timespec *tp) {
    if (!tp) return -1;
    if (clk_id == 0 || clk_id == 5)
        return clock_gettime(CLOCK_REALTIME, tp);

    u64 ns = armTicksToNs(armGetSystemTick());
    tp->tv_sec = ns / 1000000000ULL;
    tp->tv_nsec = ns % 1000000000ULL;
    return 0;
}

int sys_clock_getres(int clk_id, struct timespec *tp) {
    (void)clk_id;
    if (tp) {
        tp->tv_sec = 0;
        tp->tv_nsec = 1;
    }
    return 0;
}

// ============================================================================
// sysconf with bionic's _SC_* numbering
// ============================================================================
long sys_sysconf(int name) {
    switch (name) {
        case 6:   return 100;          // _SC_CLK_TCK
        case 39:                       // _SC_PAGESIZE
        case 40:  return 4096;         // _SC_PAGE_SIZE
        case 96:                       // _SC_NPROCESSORS_CONF
        case 97:  return 3;            // _SC_NPROCESSORS_ONLN (cores 0-2)
        case 98:  return (3LL * 1024 * 1024 * 1024) / 4096; // _SC_PHYS_PAGES
        case 99:  return (1LL * 1024 * 1024 * 1024) / 4096; // _SC_AVPHYS_PAGES
        default:
            l_warn("[sysconf] unhandled name %d", name);
            return -1;
    }
}

// ============================================================================
// Raw syscalls (arm64 numbers)
// ============================================================================
long sys_syscall(long num, ...) {
    switch (num) {
        case 178: return pthr_gettid(); // __NR_gettid
        case 172: return 1000;          // __NR_getpid
        default: {
            static int s_warned = 0;
            if (s_warned++ < 16) l_warn("[syscall] unhandled syscall %ld", num);
            *pthr_errno() = 38; // ENOSYS
            return -1;
        }
    }
}

// ============================================================================
// dl_iterate_phdr: the C++ unwinder uses this to find each module's
// PT_GNU_EH_FRAME, so exceptions cannot propagate without it.
// ============================================================================
typedef struct {
    uint64_t dlpi_addr;
    const char *dlpi_name;
    const Elf64_Phdr *dlpi_phdr;
    uint16_t dlpi_phnum;
    uint64_t dlpi_adds;
    uint64_t dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
} guest_dl_phdr_info;

int sys_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data) {
    int count = 0;
    for (so_module *m = so_module_list(); m; m = m->next) count++;

    for (so_module *m = so_module_list(); m; m = m->next) {
        guest_dl_phdr_info info;
        memset(&info, 0, sizeof(info));
        info.dlpi_addr = m->base_addr;
        info.dlpi_name = m->soname ? m->soname : "";
        info.dlpi_phdr = m->phdr;
        info.dlpi_phnum = m->ehdr->e_phnum;
        info.dlpi_adds = count;

        int ret = callback(&info, sizeof(info), data);
        if (ret) return ret;
    }
    return 0;
}

// ============================================================================
// Network stubs
// ============================================================================
int sys_net_fail(void) {
    *pthr_errno() = 101; // ENETUNREACH
    return -1;
}

int sys_socket(int domain, int type, int protocol) {
    (void)domain; (void)type; (void)protocol;
    return sys_net_fail();
}

void *sys_net_null(void) {
    return NULL;
}

int sys_getaddrinfo(const char *node, const char *service, const void *hints, void **res) {
    (void)node; (void)service; (void)hints;
    if (res) *res = NULL;
    return -2; // EAI_NONAME (bionic)
}

void sys_freeaddrinfo(void *res) {
    (void)res;
}
