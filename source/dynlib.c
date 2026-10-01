#define _GNU_SOURCE
#include "dynlib.h"
#include "reimpl/log.h"
#include "reimpl/io.h"
#include "reimpl/asset_manager.h"
#include "reimpl/opensles.h"
#include "reimpl/pthr.h"
#include "reimpl/sys.h"
#include "reimpl/egl.h"
#include "reimpl/gldiag.h"
#include "reimpl/gltrace.h"
#include "utils/logger.h"
#include "utils/prof.h"
#include "utils/init.h"

#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <zlib.h>
#include <errno.h>
#include <ctype.h>
#include <wctype.h>
#include <wchar.h>
#include <locale.h>
#include <time.h>
#include <setjmp.h>
#include <fcntl.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

extern void *__cxa_allocate_exception(size_t);
extern void __cxa_free_exception(void *);
extern void __cxa_throw(void *, void *, void *);
extern void *__cxa_begin_catch(void *);
extern void __cxa_end_catch(void);

// ============================================================================
// Fake Android Standard Streams (__sF)
// ============================================================================
// Bionic's FILE is 152 bytes on LP64 and the guest indexes __sF itself, so the
// fake array has to use that stride rather than newlib's sizeof(FILE).
#define BIONIC_FILE_SIZE 152
static uint8_t s_sF_fake[3 * BIONIC_FILE_SIZE];

void init_android_streams(void) {
}

static inline FILE *check_stream(FILE *stream) {
    uintptr_t p = (uintptr_t)stream;
    uintptr_t base = (uintptr_t)s_sF_fake;
    if (p >= base && p < base + sizeof(s_sF_fake)) {
        switch ((p - base) / BIONIC_FILE_SIZE) {
            case 0:  return stdin;
            case 1:  return stdout;
            default: return stderr;
        }
    }
    return stream;
}

// ============================================================================
// Stream I/O Wrappers
// ============================================================================
static int wrap_fclose(FILE *f) { return fclose(check_stream(f)); }
static size_t wrap_fread(void *ptr, size_t sz, size_t n, FILE *f) { return fread(ptr, sz, n, check_stream(f)); }
static size_t wrap_fwrite(const void *ptr, size_t sz, size_t n, FILE *f) { return fwrite(ptr, sz, n, check_stream(f)); }
static int wrap_fseek(FILE *f, long off, int w) { return fseek(check_stream(f), off, w); }
static int wrap_fseeko(FILE *f, off_t off, int w) { return fseeko(check_stream(f), off, w); }
static long wrap_ftell(FILE *f) { return ftell(check_stream(f)); }
static off_t wrap_ftello(FILE *f) { return ftello(check_stream(f)); }
static int wrap_fflush(FILE *f) { return fflush(check_stream(f)); }
static int wrap_fputc(int c, FILE *f) { return fputc(c, check_stream(f)); }
static int wrap_fputs(const char *s, FILE *f) { return fputs(s, check_stream(f)); }
static int wrap_fgetc(FILE *f) { return fgetc(check_stream(f)); }
static char *wrap_fgets(char *s, int n, FILE *f) { return fgets(s, n, check_stream(f)); }
static int wrap_getc(FILE *f) { return getc(check_stream(f)); }
static int wrap_ungetc(int c, FILE *f) { return ungetc(c, check_stream(f)); }
static int wrap_feof(FILE *f) { return feof(check_stream(f)); }
static int wrap_ferror(FILE *f) { return ferror(check_stream(f)); }
static void wrap_clearerr(FILE *f) { clearerr(check_stream(f)); }
static int wrap_fileno(FILE *f) { return fileno(check_stream(f)); }
static void wrap_rewind(FILE *f) { rewind(check_stream(f)); }
static int wrap_setvbuf(FILE *f, char *b, int m, size_t s) { return setvbuf(check_stream(f), b, m, s); }

static int wrap_vfprintf(FILE *f, const char *fmt, va_list ap) {
    return vfprintf(check_stream(f), fmt, ap);
}

static int wrap_fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int res = vfprintf(check_stream(f), fmt, ap);
    va_end(ap);
    return res;
}

static int wrap_fscanf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int res = vfscanf(check_stream(f), fmt, ap);
    va_end(ap);
    return res;
}

static FILE *wrap_freopen(const char *p, const char *m, FILE *f) {
    char r[512];
    translate_path(p, r, sizeof(r));
    return freopen(r, m, check_stream(f));
}
static FILE *wrap_tmpfile(void) { return tmpfile(); }
static char *wrap_tmpnam(char *s) { return tmpnam(s); }
static int wrap_mkstemp(char *t) { return mkstemp(t); }

// ============================================================================
// File Descriptors / Path Translation Wrappers
// ============================================================================
static int wrap_rmdir(const char *p) {
    char r[512]; translate_path(p, r, sizeof(r)); return rmdir(r);
}
static int wrap_chmod(const char *p, mode_t m) { (void)p; (void)m; return 0; }
static mode_t wrap_umask(mode_t m) { (void)m; return 0; }
static int wrap_fsync(int fd) { return fsync(fd); }
static int wrap_fcntl(int fd, int cmd, ...) { (void)fd; (void)cmd; return 0; }
static int wrap_ioctl(int fd, int req, ...) { (void)fd; (void)req; return 0; }
static int wrap_pipe(int fds[2]) { fds[0] = 100; fds[1] = 101; return 0; }
static char *wrap_realpath(const char *path, char *resolved) {
    char r[512]; translate_path(path, r, sizeof(r));
    if (resolved) { strcpy(resolved, r); return resolved; }
    return strdup(r);
}

// ============================================================================
// Memory & Process / System
// ============================================================================
static pid_t wrap_getpid(void) { return 1000; }
static uid_t wrap_getuid(void) { return 1000; }
static uid_t wrap_geteuid(void) { return 1000; }
static gid_t wrap_getgid(void) { return 1000; }
static gid_t wrap_getegid(void) { return 1000; }
static int wrap_getpagesize(void) { return 4096; }
static int wrap_gethostname(char *name, size_t len) { strncpy(name, "NintendoSwitch", len); return 0; }
static void *wrap_getpwuid(uid_t uid) { (void)uid; return NULL; }
static int wrap_system(const char *cmd) { (void)cmd; return -1; }
static int wrap_tcgetattr(int fd, void *t) { (void)fd; (void)t; return -1; }
static int wrap_tcsetattr(int fd, int a, const void *t) { (void)fd; (void)a; (void)t; return -1; }
static int wrap_sigaction(int s, const void *a, void *o) { (void)s; (void)a; (void)o; return 0; }
static void *wrap_signal(int s, void *h) { (void)s; (void)h; return NULL; }

// ============================================================================
// ABI / C++ Runtime / Bionic Support
// ============================================================================
static int wrap_cxa_atexit(void (*f)(void *), void *arg, void *dso) { (void)f; (void)arg; (void)dso; return 0; }
static void wrap_cxa_finalize(void *dso) { (void)dso; }
static int wrap_cxa_thread_atexit_impl(void (*f)(void *), void *arg, void *dso) { (void)f; (void)arg; (void)dso; return 0; }
static void wrap_stack_chk_fail(void) {
    void *caller = __builtin_return_address(0);
    fatal_error("Stack smashed (__stack_chk_fail)! Caller: %p", caller);
}
static uintptr_t s_stack_chk_guard = 0xD00DF00DDEADBEEF;
static void wrap_android_set_abort_message(const char *msg) { l_error("Android abort: %s", msg ? msg : "(null)"); }
static size_t wrap_ctype_get_mb_cur_max(void) { return 4; }
static void wrap_openlog(const char *i, int o, int f) { (void)i; (void)o; (void)f; }
static void wrap_syslog(int p, const char *f, ...) { (void)p; (void)f; }
static void wrap_closelog(void) {}
static int wrap_isnanf(float f) { return isnan(f); }

static int wrap_vasprintf(char **strp, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int len = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (len < 0) return -1;
    *strp = (char *)malloc(len + 1);
    if (!*strp) return -1;
    return vsprintf(*strp, fmt, ap);
}

static char *wrap_if_indextoname(unsigned int ifindex, char *ifname) {
    (void)ifindex;
    if (ifname) strncpy(ifname, "wlan0", 6);
    return ifname;
}

extern char *strptime(const char *buf, const char *fmt, struct tm *tm);

// ============================================================================
// Locale & Extended Character Functions (_l)
// ============================================================================
static void *wrap_newlocale(int c, const char *l, void *b) { (void)c; (void)l; (void)b; return (void *)0x1000; }
static void wrap_freelocale(void *l) { (void)l; }
static void *wrap_uselocale(void *l) { (void)l; return (void *)0x1000; }

static int wrap_isdigit_l(int c, void *l) { (void)l; return isdigit(c); }
static int wrap_islower_l(int c, void *l) { (void)l; return islower(c); }
static int wrap_isupper_l(int c, void *l) { (void)l; return isupper(c); }
static int wrap_isxdigit_l(int c, void *l) { (void)l; return isxdigit(c); }
static int wrap_tolower_l(int c, void *l) { (void)l; return tolower(c); }
static int wrap_toupper_l(int c, void *l) { (void)l; return toupper(c); }

static int wrap_iswalpha_l(wint_t c, void *l) { (void)l; return iswalpha(c); }
static int wrap_iswblank_l(wint_t c, void *l) { (void)l; return iswblank(c); }
static int wrap_iswcntrl_l(wint_t c, void *l) { (void)l; return iswcntrl(c); }
static int wrap_iswdigit_l(wint_t c, void *l) { (void)l; return iswdigit(c); }
static int wrap_iswlower_l(wint_t c, void *l) { (void)l; return iswlower(c); }
static int wrap_iswprint_l(wint_t c, void *l) { (void)l; return iswprint(c); }
static int wrap_iswpunct_l(wint_t c, void *l) { (void)l; return iswpunct(c); }
static int wrap_iswspace_l(wint_t c, void *l) { (void)l; return iswspace(c); }
static int wrap_iswupper_l(wint_t c, void *l) { (void)l; return iswupper(c); }
static int wrap_iswxdigit_l(wint_t c, void *l) { (void)l; return iswxdigit(c); }
static wint_t wrap_towlower_l(wint_t c, void *l) { (void)l; return towlower(c); }
static wint_t wrap_towupper_l(wint_t c, void *l) { (void)l; return towupper(c); }

static int wrap_strcoll_l(const char *s1, const char *s2, void *l) { (void)l; return strcoll(s1, s2); }
static size_t wrap_strxfrm_l(char *d, const char *s, size_t n, void *l) { (void)l; return strxfrm(d, s, n); }
static size_t wrap_strftime_l(char *s, size_t m, const char *f, const struct tm *t, void *l) { (void)l; return strftime(s, m, f, t); }
static long double wrap_strtold_l(const char *s, char **e, void *l) { (void)l; return strtold(s, e); }
static long long wrap_strtoll_l(const char *s, char **e, int b, void *l) { (void)l; return strtoll(s, e, b); }
static unsigned long long wrap_strtoull_l(const char *s, char **e, int b, void *l) { (void)l; return strtoull(s, e, b); }
static int wrap_wcscoll_l(const wchar_t *s1, const wchar_t *s2, void *l) { (void)l; return wcscoll(s1, s2); }
static size_t wrap_wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, void *l) { (void)l; return wcsxfrm(d, s, n); }

// ============================================================================
// Android NDK / Epoll / Dynamic Linking Stubs
// ============================================================================
static void *wrap_dlopen(const char *f, int flag) { (void)flag; l_info("[dlopen] '%s'", f ? f : "(null)"); return (void *)0x5000; }
static void *wrap_dlsym(void *h, const char *s) { (void)h; l_info("[dlsym] '%s'", s ? s : "(null)"); return NULL; }
static int wrap_dlclose(void *h) { (void)h; return 0; }
static char *wrap_dlerror(void) { return NULL; }

static int wrap_epoll_create(int size) { (void)size; return 100; }
static int wrap_epoll_create1(int flags) { (void)flags; return 100; }
static int wrap_epoll_ctl(int epfd, int op, int fd, void *ev) { (void)epfd; (void)op; (void)fd; (void)ev; return 0; }
// There is no real epoll: report "no events" after waiting, never instantly.
// asio's reactor threads call this in a loop and would otherwise spin at 100%
// CPU and starve the game thread on the Switch's three cores.
static int wrap_epoll_wait(int epfd, void *ev, int max, int timeout_ms) {
    (void)epfd; (void)ev; (void)max;
    if (timeout_ms != 0) {
        u64 ms = (timeout_ms < 0 || timeout_ms > 10) ? 10 : (u64)timeout_ms;
        svcSleepThread(ms * 1000000ULL);
    }
    return 0;
}
static int wrap_poll_idle(void) {
    svcSleepThread(5000000ULL);
    return sys_net_fail();
}
static int wrap_eventfd(unsigned int init, int f) { (void)init; (void)f; return 101; }
static ssize_t wrap_sendfile(int out, int in, off_t *off, size_t count) { (void)out; (void)in; (void)off; (void)count; return -1; }

static void *wrap_ASensorManager_getInstance(void) { return (void *)0x2000; }
static void *wrap_ASensorManager_getDefaultSensor(void *mgr, int type) { (void)mgr; (void)type; return (void *)0x2001; }
static void *wrap_ASensorManager_createEventQueue(void *mgr, void *looper, int ident, void *cb, void *data) {
    (void)mgr; (void)looper; (void)ident; (void)cb; (void)data;
    return (void *)0x2002;
}
static int wrap_ASensorEventQueue_enableSensor(void *q, void *s) { (void)q; (void)s; return 0; }
static int wrap_ASensorEventQueue_disableSensor(void *q, void *s) { (void)q; (void)s; return 0; }
static int wrap_ASensorEventQueue_setEventRate(void *q, void *s, int32_t usec) { (void)q; (void)s; (void)usec; return 0; }
static ssize_t wrap_ASensorEventQueue_getEvents(void *q, void *e, size_t count) { (void)q; (void)e; (void)count; return 0; }
static void *wrap_ALooper_prepare(int o) { (void)o; return (void *)0x3000; }
static void *wrap_ALooper_forThread(void) { return (void *)0x3000; }


// ============================================================================
// Scheduling stubs & fatal guest exits
// ============================================================================
static int wrap_pthread_getschedparam(long t, int *policy, int *param) {
    (void)t; if (policy) *policy = 0; if (param) *param = 0; return 0;
}
static int wrap_pthread_setschedparam(long t, int policy, const int *param) {
    (void)t; (void)policy; (void)param; return 0;
}
static int wrap_sched_get_priority_min(int p) { (void)p; return 0; }
static int wrap_sched_get_priority_max(int p) { (void)p; return 31; }
static int wrap_sched_yield(void) { svcSleepThread(0); return 0; }

static void wrap_cxa_pure_virtual(void) {
    void *caller = __builtin_return_address(0);
    fatal_error("[C++] Pure virtual function call! Caller: %p", caller);
}

static void wrap_abort(void) {
    void *caller = __builtin_return_address(0);
    fatal_error("[guest abort] Game library invoked abort()! Caller: %p", caller);
}

static void wrap_exit(int code) {
    void *caller = __builtin_return_address(0);
    fatal_error("[guest exit] Game library invoked exit(%d)! Caller: %p", code, caller);
}

static int wrap_gettimeofday(struct timeval *tv, struct timezone *tz) {
    (void)tz;
    return gettimeofday(tv, NULL);
}

static int wrap_nanosleep(const struct timespec *req, struct timespec *rem) {
    if (req) svcSleepThread((u64)req->tv_sec * 1000000000ULL + (u64)req->tv_nsec);
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}
static int wrap_usleep(unsigned long usec) { svcSleepThread((u64)usec * 1000ULL); return 0; }
static unsigned int wrap_sleep(unsigned int sec) { svcSleepThread((u64)sec * 1000000000ULL); return 0; }

// GL_OES_mapbuffer is resolved through EGL because libGLESv2 does not export it.
static void *wrap_glMapBufferOES(GLenum target, GLenum access) {
    static void *(*fn)(GLenum, GLenum) = NULL;
    if (!fn) fn = (void *(*)(GLenum, GLenum))eglGetProcAddress("glMapBufferOES");
    return fn ? fn(target, access) : NULL;
}
static GLboolean wrap_glUnmapBufferOES(GLenum target) {
    static GLboolean (*fn)(GLenum) = NULL;
    if (!fn) fn = (GLboolean (*)(GLenum))eglGetProcAddress("glUnmapBufferOES");
    return fn ? fn(target) : GL_FALSE;
}

// ============================================================================
// Master Symbol Table
// ============================================================================
static so_default_dynlib s_default_dynlib[] = {
    // Android Streams & Stack Protection
    { "__sF", (uintptr_t)s_sF_fake },
    { "__errno", (uintptr_t)pthr_errno },
    { "__stack_chk_fail", (uintptr_t)&wrap_stack_chk_fail },
    { "__stack_chk_guard", (uintptr_t)&s_stack_chk_guard },
    { "__cxa_atexit", (uintptr_t)&wrap_cxa_atexit },
    { "__cxa_finalize", (uintptr_t)&wrap_cxa_finalize },
    { "__cxa_thread_atexit_impl", (uintptr_t)&wrap_cxa_thread_atexit_impl },
    { "__cxa_guard_acquire", (uintptr_t)pthr_cxa_guard_acquire },
    { "__cxa_guard_release", (uintptr_t)pthr_cxa_guard_release },
    { "__cxa_guard_abort", (uintptr_t)pthr_cxa_guard_abort },
    { "__cxa_pure_virtual", (uintptr_t)&wrap_cxa_pure_virtual },
    { "android_set_abort_message", (uintptr_t)&wrap_android_set_abort_message },
    { "dl_iterate_phdr", (uintptr_t)sys_dl_iterate_phdr },
    { "__ctype_get_mb_cur_max", (uintptr_t)&wrap_ctype_get_mb_cur_max },
    { "openlog", (uintptr_t)&wrap_openlog },
    { "syslog", (uintptr_t)&wrap_syslog },
    { "closelog", (uintptr_t)&wrap_closelog },
    { "getenv", (uintptr_t)getenv },

    // Android Logging
    { "__android_log_print", (uintptr_t)__android_log_print },
    { "__android_log_write", (uintptr_t)__android_log_write },
    { "__android_log_vprint", (uintptr_t)__android_log_vprint },
    { "__android_log_assert", (uintptr_t)__android_log_assert },

    // Android NDK Asset Manager
    { "AAssetManager_fromJava", (uintptr_t)AAssetManager_fromJava },
    { "AAssetManager_open", (uintptr_t)AAssetManager_open },
    { "AAssetManager_openDir", (uintptr_t)AAssetManager_openDir },
    { "AAssetDir_getNextFileName", (uintptr_t)AAssetDir_getNextFileName },
    { "AAssetDir_rewind", (uintptr_t)AAssetDir_rewind },
    { "AAssetDir_close", (uintptr_t)AAssetDir_close },
    { "AAsset_read", (uintptr_t)AAsset_read },
    { "AAsset_seek", (uintptr_t)AAsset_seek },
    { "AAsset_close", (uintptr_t)AAsset_close },
    { "AAsset_getBuffer", (uintptr_t)AAsset_getBuffer },
    { "AAsset_getLength", (uintptr_t)AAsset_getLength },
    { "AAsset_getRemainingLength", (uintptr_t)AAsset_getRemainingLength },

    // Android Native Window
    { "ANativeWindow_fromSurface", (uintptr_t)ANativeWindow_fromSurface },
    { "ANativeWindow_release", (uintptr_t)ANativeWindow_release },
    { "ANativeWindow_getWidth", (uintptr_t)ANativeWindow_getWidth },
    { "ANativeWindow_getHeight", (uintptr_t)ANativeWindow_getHeight },
    { "ANativeWindow_setBuffersGeometry", (uintptr_t)ANativeWindow_setBuffersGeometry },

    // Android Sensors & Looper
    { "ASensorManager_getInstance", (uintptr_t)&wrap_ASensorManager_getInstance },
    { "ASensorManager_getDefaultSensor", (uintptr_t)&wrap_ASensorManager_getDefaultSensor },
    { "ASensorManager_createEventQueue", (uintptr_t)&wrap_ASensorManager_createEventQueue },
    { "ASensorEventQueue_enableSensor", (uintptr_t)&wrap_ASensorEventQueue_enableSensor },
    { "ASensorEventQueue_disableSensor", (uintptr_t)&wrap_ASensorEventQueue_disableSensor },
    { "ASensorEventQueue_setEventRate", (uintptr_t)&wrap_ASensorEventQueue_setEventRate },
    { "ASensorEventQueue_getEvents", (uintptr_t)&wrap_ASensorEventQueue_getEvents },
    { "ALooper_prepare", (uintptr_t)&wrap_ALooper_prepare },
    { "ALooper_forThread", (uintptr_t)&wrap_ALooper_forThread },

    // OpenSL ES
    { "slCreateEngine", (uintptr_t)wrap_slCreateEngine },
    { "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE_ptr },
    { "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY_ptr },
    { "SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE_ptr },
    { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE_ptr },
    { "SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&SL_IID_ANDROIDCONFIGURATION_ptr },
    { "SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME_ptr },
    { "SL_IID_SEEK", (uintptr_t)&SL_IID_SEEK_ptr },
    { "SL_IID_RECORD", (uintptr_t)&SL_IID_RECORD_ptr },
    { "SL_IID_PREFETCHSTATUS", (uintptr_t)&SL_IID_PREFETCHSTATUS_ptr },
    { "SL_IID_METADATAEXTRACTION", (uintptr_t)&SL_IID_METADATAEXTRACTION_ptr },
    { "SL_IID_PITCH", (uintptr_t)&SL_IID_PITCH_ptr },

    // Standard I/O (wrapped for path translation & streams)
    { "fopen", (uintptr_t)wrap_fopen },
    { "open", (uintptr_t)wrap_open },
    { "stat", (uintptr_t)wrap_stat },
    { "access", (uintptr_t)wrap_access },
    { "mkdir", (uintptr_t)wrap_mkdir },
    { "opendir", (uintptr_t)wrap_opendir },
    { "closedir", (uintptr_t)wrap_closedir },
    { "readdir", (uintptr_t)wrap_readdir },
    { "unlink", (uintptr_t)wrap_unlink },
    { "rename", (uintptr_t)wrap_rename },
    { "remove", (uintptr_t)wrap_remove },
    { "rmdir", (uintptr_t)wrap_rmdir },
    { "chdir", (uintptr_t)wrap_chdir },
    { "getcwd", (uintptr_t)wrap_getcwd },
    { "chmod", (uintptr_t)wrap_chmod },
    { "umask", (uintptr_t)wrap_umask },
    { "realpath", (uintptr_t)wrap_realpath },
    { "fclose", (uintptr_t)wrap_fclose },
    { "fread", (uintptr_t)wrap_fread },
    { "fwrite", (uintptr_t)wrap_fwrite },
    { "fseek", (uintptr_t)wrap_fseek },
    { "fseeko", (uintptr_t)wrap_fseeko },
    { "ftell", (uintptr_t)wrap_ftell },
    { "ftello", (uintptr_t)wrap_ftello },
    { "fflush", (uintptr_t)wrap_fflush },
    { "fputc", (uintptr_t)wrap_fputc },
    { "fputs", (uintptr_t)wrap_fputs },
    { "fgetc", (uintptr_t)wrap_fgetc },
    { "fgets", (uintptr_t)wrap_fgets },
    { "getc", (uintptr_t)wrap_getc },
    { "ungetc", (uintptr_t)wrap_ungetc },
    { "feof", (uintptr_t)wrap_feof },
    { "ferror", (uintptr_t)wrap_ferror },
    { "clearerr", (uintptr_t)wrap_clearerr },
    { "fileno", (uintptr_t)wrap_fileno },
    { "rewind", (uintptr_t)wrap_rewind },
    { "setvbuf", (uintptr_t)wrap_setvbuf },
    { "vfprintf", (uintptr_t)wrap_vfprintf },
    { "fprintf", (uintptr_t)wrap_fprintf },
    { "fscanf", (uintptr_t)wrap_fscanf },
    { "fdopen", (uintptr_t)wrap_fdopen },
    { "freopen", (uintptr_t)wrap_freopen },
    { "tmpfile", (uintptr_t)wrap_tmpfile },
    { "tmpnam", (uintptr_t)wrap_tmpnam },
    { "mkstemp", (uintptr_t)wrap_mkstemp },
    { "close", (uintptr_t)wrap_close },
    { "read", (uintptr_t)wrap_read },
    { "write", (uintptr_t)wrap_write },
    { "pread", (uintptr_t)wrap_pread },
    { "lseek", (uintptr_t)wrap_lseek },
    { "lseek64", (uintptr_t)wrap_lseek },
    { "fsync", (uintptr_t)wrap_fsync },
    { "fstat", (uintptr_t)wrap_fstat },
    { "statfs", (uintptr_t)wrap_statfs },
    { "fcntl", (uintptr_t)wrap_fcntl },
    { "ioctl", (uintptr_t)wrap_ioctl },
    { "pipe", (uintptr_t)wrap_pipe },
    { "sprintf", (uintptr_t)sprintf },
    { "snprintf", (uintptr_t)snprintf },
    { "vsnprintf", (uintptr_t)vsnprintf },
    { "vsprintf", (uintptr_t)vsprintf },
    { "sscanf", (uintptr_t)sscanf },
    { "vsscanf", (uintptr_t)vsscanf },
    { "vasprintf", (uintptr_t)&wrap_vasprintf },
    { "printf", (uintptr_t)printf },
    { "puts", (uintptr_t)puts },
    { "putchar", (uintptr_t)putchar },
    { "putc", (uintptr_t)wrap_fputc },

    // Memory
    { "malloc", (uintptr_t)malloc },
    { "free", (uintptr_t)free },
    { "calloc", (uintptr_t)calloc },
    { "realloc", (uintptr_t)realloc },
    { "memalign", (uintptr_t)memalign },
    { "mmap", (uintptr_t)wrap_mmap },
    { "munmap", (uintptr_t)wrap_munmap },
    { "memcpy", (uintptr_t)memcpy },
    { "memmove", (uintptr_t)memmove },
    { "memset", (uintptr_t)memset },
    { "memcmp", (uintptr_t)memcmp },
    { "memchr", (uintptr_t)memchr },

    // Strings
    { "strlen", (uintptr_t)strlen },
    { "strcpy", (uintptr_t)strcpy },
    { "strncpy", (uintptr_t)strncpy },
    { "strcat", (uintptr_t)strcat },
    { "strncat", (uintptr_t)strncat },
    { "strcmp", (uintptr_t)strcmp },
    { "strncmp", (uintptr_t)strncmp },
    { "strcasecmp", (uintptr_t)strcasecmp },
    { "strncasecmp", (uintptr_t)strncasecmp },
    { "strchr", (uintptr_t)strchr },
    { "strrchr", (uintptr_t)strrchr },
    { "strstr", (uintptr_t)strstr },
    { "strdup", (uintptr_t)strdup },
    { "strspn", (uintptr_t)strspn },
    { "strcspn", (uintptr_t)strcspn },
    { "strpbrk", (uintptr_t)strpbrk },
    { "strtok_r", (uintptr_t)strtok_r },
    { "strtok", (uintptr_t)strtok },
    { "strerror", (uintptr_t)strerror },
    { "strerror_r", (uintptr_t)strerror_r },
    { "strtol", (uintptr_t)strtol },
    { "strtoul", (uintptr_t)strtoul },
    { "strtoll", (uintptr_t)strtoll },
    { "strtoull", (uintptr_t)strtoull },
    { "strtod", (uintptr_t)strtod },
    { "strtof", (uintptr_t)strtof },
    { "strtold", (uintptr_t)strtold },
    { "atoi", (uintptr_t)atoi },
    { "atol", (uintptr_t)atol },
    { "atoll", (uintptr_t)atoll },
    { "atof", (uintptr_t)atof },

    // Math
    { "sin", (uintptr_t)sin },
    { "sinf", (uintptr_t)sinf },
    { "sinh", (uintptr_t)sinh },
    { "sinhf", (uintptr_t)sinhf },
    { "cos", (uintptr_t)cos },
    { "cosf", (uintptr_t)cosf },
    { "cosh", (uintptr_t)cosh },
    { "tan", (uintptr_t)tan },
    { "tanf", (uintptr_t)tanf },
    { "tanh", (uintptr_t)tanh },
    { "asin", (uintptr_t)asin },
    { "asinf", (uintptr_t)asinf },
    { "acos", (uintptr_t)acos },
    { "acosf", (uintptr_t)acosf },
    { "atan", (uintptr_t)atan },
    { "atanf", (uintptr_t)atanf },
    { "atan2", (uintptr_t)atan2 },
    { "atan2f", (uintptr_t)atan2f },
    { "sqrt", (uintptr_t)sqrt },
    { "sqrtf", (uintptr_t)sqrtf },
    { "cbrt", (uintptr_t)cbrt },
    { "cbrtf", (uintptr_t)cbrtf },
    { "pow", (uintptr_t)pow },
    { "powf", (uintptr_t)powf },
    { "exp", (uintptr_t)exp },
    { "expf", (uintptr_t)expf },
    { "exp2", (uintptr_t)exp2 },
    { "exp2f", (uintptr_t)exp2f },
    { "log", (uintptr_t)log },
    { "logf", (uintptr_t)logf },
    { "log2", (uintptr_t)log2 },
    { "log10", (uintptr_t)log10 },
    { "log10f", (uintptr_t)log10f },
    { "floor", (uintptr_t)floor },
    { "floorf", (uintptr_t)floorf },
    { "ceil", (uintptr_t)ceil },
    { "ceilf", (uintptr_t)ceilf },
    { "round", (uintptr_t)round },
    { "roundf", (uintptr_t)roundf },
    { "trunc", (uintptr_t)trunc },
    { "truncf", (uintptr_t)truncf },
    { "fabs", (uintptr_t)fabs },
    { "fabsf", (uintptr_t)fabsf },
    { "fmod", (uintptr_t)fmod },
    { "fmodf", (uintptr_t)fmodf },
    { "ldexp", (uintptr_t)ldexp },
    { "ldexpf", (uintptr_t)ldexpf },
    { "frexp", (uintptr_t)frexp },
    { "modf", (uintptr_t)modf },
    { "isnan", (uintptr_t)isnan },
    { "__isnanf", (uintptr_t)&wrap_isnanf },

    // Ctype
    { "isalnum", (uintptr_t)isalnum },
    { "isalpha", (uintptr_t)isalpha },
    { "iscntrl", (uintptr_t)iscntrl },
    { "isdigit", (uintptr_t)isdigit },
    { "islower", (uintptr_t)islower },
    { "isprint", (uintptr_t)isprint },
    { "ispunct", (uintptr_t)ispunct },
    { "isspace", (uintptr_t)isspace },
    { "isupper", (uintptr_t)isupper },
    { "isxdigit", (uintptr_t)isxdigit },
    { "tolower", (uintptr_t)tolower },
    { "toupper", (uintptr_t)toupper },

    // Locales & _l extensions
    { "setlocale", (uintptr_t)setlocale },
    { "localeconv", (uintptr_t)localeconv },
    { "newlocale", (uintptr_t)&wrap_newlocale },
    { "freelocale", (uintptr_t)&wrap_freelocale },
    { "uselocale", (uintptr_t)&wrap_uselocale },
    { "isdigit_l", (uintptr_t)&wrap_isdigit_l },
    { "islower_l", (uintptr_t)&wrap_islower_l },
    { "isupper_l", (uintptr_t)&wrap_isupper_l },
    { "isxdigit_l", (uintptr_t)&wrap_isxdigit_l },
    { "tolower_l", (uintptr_t)&wrap_tolower_l },
    { "toupper_l", (uintptr_t)&wrap_toupper_l },
    { "iswalpha_l", (uintptr_t)&wrap_iswalpha_l },
    { "iswblank_l", (uintptr_t)&wrap_iswblank_l },
    { "iswcntrl_l", (uintptr_t)&wrap_iswcntrl_l },
    { "iswdigit_l", (uintptr_t)&wrap_iswdigit_l },
    { "iswlower_l", (uintptr_t)&wrap_iswlower_l },
    { "iswprint_l", (uintptr_t)&wrap_iswprint_l },
    { "iswpunct_l", (uintptr_t)&wrap_iswpunct_l },
    { "iswspace_l", (uintptr_t)&wrap_iswspace_l },
    { "iswupper_l", (uintptr_t)&wrap_iswupper_l },
    { "iswxdigit_l", (uintptr_t)&wrap_iswxdigit_l },
    { "towlower_l", (uintptr_t)&wrap_towlower_l },
    { "towupper_l", (uintptr_t)&wrap_towupper_l },
    { "strcoll_l", (uintptr_t)&wrap_strcoll_l },
    { "strxfrm_l", (uintptr_t)&wrap_strxfrm_l },
    { "strftime_l", (uintptr_t)&wrap_strftime_l },
    { "strtold_l", (uintptr_t)&wrap_strtold_l },
    { "strtoll_l", (uintptr_t)&wrap_strtoll_l },
    { "strtoull_l", (uintptr_t)&wrap_strtoull_l },
    { "wcscoll_l", (uintptr_t)&wrap_wcscoll_l },
    { "wcsxfrm_l", (uintptr_t)&wrap_wcsxfrm_l },

    // Wide Characters
    { "wcslen", (uintptr_t)wcslen },
    { "wmemchr", (uintptr_t)wmemchr },
    { "wmemcmp", (uintptr_t)wmemcmp },
    { "wmemcpy", (uintptr_t)wmemcpy },
    { "wmemmove", (uintptr_t)wmemmove },
    { "wmemset", (uintptr_t)wmemset },
    { "btowc", (uintptr_t)btowc },
    { "wctob", (uintptr_t)wctob },
    { "mbrlen", (uintptr_t)mbrlen },
    { "mbrtowc", (uintptr_t)mbrtowc },
    { "mbtowc", (uintptr_t)mbtowc },
    { "wcrtomb", (uintptr_t)wcrtomb },
    { "mbsrtowcs", (uintptr_t)mbsrtowcs },
    { "mbsnrtowcs", (uintptr_t)mbsnrtowcs },
    { "wcsnrtombs", (uintptr_t)wcsnrtombs },
    { "wcstod", (uintptr_t)wcstod },
    { "wcstof", (uintptr_t)wcstof },
    { "wcstol", (uintptr_t)wcstol },
    { "wcstold", (uintptr_t)wcstold },
    { "wcstoll", (uintptr_t)wcstoll },
    { "wcstoul", (uintptr_t)wcstoul },
    { "wcstoull", (uintptr_t)wcstoull },
    { "swprintf", (uintptr_t)swprintf },

    // Sockets & Network
    { "socket", (uintptr_t)sys_socket },
    { "connect", (uintptr_t)sys_net_fail },
    { "bind", (uintptr_t)sys_net_fail },
    { "listen", (uintptr_t)sys_net_fail },
    { "accept", (uintptr_t)sys_net_fail },
    { "send", (uintptr_t)sys_net_fail },
    { "sendto", (uintptr_t)sys_net_fail },
    { "sendmsg", (uintptr_t)sys_net_fail },
    { "recv", (uintptr_t)sys_net_fail },
    { "recvfrom", (uintptr_t)sys_net_fail },
    { "recvmsg", (uintptr_t)sys_net_fail },
    { "shutdown", (uintptr_t)sys_net_fail },
    { "getsockopt", (uintptr_t)sys_net_fail },
    { "setsockopt", (uintptr_t)sys_net_fail },
    { "getsockname", (uintptr_t)sys_net_fail },
    { "getpeername", (uintptr_t)sys_net_fail },
    { "gethostbyname", (uintptr_t)sys_net_null },
    { "gethostbyaddr", (uintptr_t)sys_net_null },
    { "getaddrinfo", (uintptr_t)sys_getaddrinfo },
    { "freeaddrinfo", (uintptr_t)sys_freeaddrinfo },
    { "inet_addr", (uintptr_t)inet_addr },
    { "inet_ntoa", (uintptr_t)inet_ntoa },
    { "inet_ntop", (uintptr_t)inet_ntop },
    { "if_indextoname", (uintptr_t)&wrap_if_indextoname },
    { "select", (uintptr_t)wrap_poll_idle },
    { "poll", (uintptr_t)wrap_poll_idle },
    { "sendfile", (uintptr_t)&wrap_sendfile },

    // Epoll & Eventfd
    { "epoll_create", (uintptr_t)&wrap_epoll_create },
    { "epoll_create1", (uintptr_t)&wrap_epoll_create1 },
    { "epoll_ctl", (uintptr_t)&wrap_epoll_ctl },
    { "epoll_wait", (uintptr_t)&wrap_epoll_wait },
    { "eventfd", (uintptr_t)&wrap_eventfd },

    // Dynamic Linking
    { "dlopen", (uintptr_t)&wrap_dlopen },
    { "dlsym", (uintptr_t)&wrap_dlsym },
    { "dlclose", (uintptr_t)&wrap_dlclose },
    { "dlerror", (uintptr_t)&wrap_dlerror },

    // POSIX Threads & Semaphores
    { "pthread_create", (uintptr_t)pthr_create },
    { "pthread_join", (uintptr_t)pthr_join },
    { "pthread_detach", (uintptr_t)pthr_detach },
    { "pthread_self", (uintptr_t)pthr_self },
    { "pthread_equal", (uintptr_t)pthr_equal },
    { "pthread_mutex_init", (uintptr_t)pthr_mutex_init },
    { "pthread_mutex_destroy", (uintptr_t)pthr_mutex_destroy },
    { "pthread_mutex_lock", (uintptr_t)pthr_mutex_lock },
    { "pthread_mutex_unlock", (uintptr_t)pthr_mutex_unlock },
    { "pthread_mutex_trylock", (uintptr_t)pthr_mutex_trylock },
    { "pthread_mutex_timedlock", (uintptr_t)pthr_mutex_timedlock },
    { "pthread_mutexattr_init", (uintptr_t)pthr_mutexattr_init },
    { "pthread_mutexattr_destroy", (uintptr_t)pthr_mutexattr_destroy },
    { "pthread_mutexattr_settype", (uintptr_t)pthr_mutexattr_settype },
    { "pthread_attr_init", (uintptr_t)pthr_attr_init },
    { "pthread_attr_destroy", (uintptr_t)pthr_attr_destroy },
    { "pthread_attr_setdetachstate", (uintptr_t)pthr_attr_setdetachstate },
    { "pthread_attr_getdetachstate", (uintptr_t)pthr_attr_getdetachstate },
    { "pthread_attr_setstacksize", (uintptr_t)pthr_attr_setstacksize },
    { "pthread_exit", (uintptr_t)pthr_exit },
    { "pthread_cond_init", (uintptr_t)pthr_cond_init },
    { "pthread_cond_destroy", (uintptr_t)pthr_cond_destroy },
    { "pthread_cond_signal", (uintptr_t)pthr_cond_signal },
    { "pthread_cond_broadcast", (uintptr_t)pthr_cond_broadcast },
    { "pthread_cond_wait", (uintptr_t)pthr_cond_wait },
    { "pthread_cond_timedwait", (uintptr_t)pthr_cond_timedwait },
    { "pthread_key_create", (uintptr_t)pthr_key_create },
    { "pthread_key_delete", (uintptr_t)pthr_key_delete },
    { "pthread_getspecific", (uintptr_t)pthr_getspecific },
    { "pthread_setspecific", (uintptr_t)pthr_setspecific },
    { "pthread_once", (uintptr_t)pthr_once },
    { "pthread_getschedparam", (uintptr_t)&wrap_pthread_getschedparam },
    { "pthread_setschedparam", (uintptr_t)&wrap_pthread_setschedparam },
    { "sched_get_priority_min", (uintptr_t)&wrap_sched_get_priority_min },
    { "sched_get_priority_max", (uintptr_t)&wrap_sched_get_priority_max },
    { "sched_yield", (uintptr_t)wrap_sched_yield },
    { "sem_init", (uintptr_t)pthr_sem_init },
    { "sem_destroy", (uintptr_t)pthr_sem_destroy },
    { "sem_wait", (uintptr_t)pthr_sem_wait },
    { "sem_trywait", (uintptr_t)pthr_sem_trywait },
    { "sem_post", (uintptr_t)pthr_sem_post },

    // Process & System
    { "getpid", (uintptr_t)&wrap_getpid },
    { "gettid", (uintptr_t)pthr_gettid },
    { "getuid", (uintptr_t)&wrap_getuid },
    { "geteuid", (uintptr_t)&wrap_geteuid },
    { "getgid", (uintptr_t)&wrap_getgid },
    { "getegid", (uintptr_t)&wrap_getegid },
    { "getpagesize", (uintptr_t)&wrap_getpagesize },
    { "gethostname", (uintptr_t)&wrap_gethostname },
    { "getpwuid", (uintptr_t)&wrap_getpwuid },
    { "sysconf", (uintptr_t)sys_sysconf },
    { "syscall", (uintptr_t)sys_syscall },
    { "system", (uintptr_t)&wrap_system },
    { "tcgetattr", (uintptr_t)&wrap_tcgetattr },
    { "tcsetattr", (uintptr_t)&wrap_tcsetattr },
    { "sigaction", (uintptr_t)&wrap_sigaction },
    { "signal", (uintptr_t)&wrap_signal },
    { "abort", (uintptr_t)&wrap_abort },
    { "exit", (uintptr_t)&wrap_exit },
    { "setjmp", (uintptr_t)setjmp },
    { "longjmp", (uintptr_t)longjmp },
    { "bsearch", (uintptr_t)bsearch },
    { "qsort", (uintptr_t)qsort },
    { "rand", (uintptr_t)rand },
    { "srand", (uintptr_t)srand },

    // Time
    { "gettimeofday", (uintptr_t)wrap_gettimeofday },
    { "clock_gettime", (uintptr_t)sys_clock_gettime },
    { "nanosleep", (uintptr_t)wrap_nanosleep },
    { "usleep", (uintptr_t)wrap_usleep },
    { "sleep", (uintptr_t)wrap_sleep },
    { "time", (uintptr_t)time },
    { "clock", (uintptr_t)clock },
    { "difftime", (uintptr_t)difftime },
    { "asctime", (uintptr_t)asctime },
    { "gmtime", (uintptr_t)gmtime },
    { "gmtime_r", (uintptr_t)gmtime_r },
    { "localtime", (uintptr_t)localtime },
    { "localtime_r", (uintptr_t)localtime_r },
    { "mktime", (uintptr_t)mktime },
    { "strftime", (uintptr_t)strftime },
    { "strptime", (uintptr_t)strptime },

    // Zlib
    { "compress", (uintptr_t)compress },
    { "uncompress", (uintptr_t)uncompress },
    { "deflateInit_", (uintptr_t)deflateInit_ },
    { "deflateInit2_", (uintptr_t)deflateInit2_ },
    { "deflate", (uintptr_t)deflate },
    { "deflateEnd", (uintptr_t)deflateEnd },
    { "inflateInit_", (uintptr_t)inflateInit_ },
    { "inflateInit2_", (uintptr_t)inflateInit2_ },
    { "inflate", (uintptr_t)inflate },
    { "inflateEnd", (uintptr_t)inflateEnd },
    { "crc32", (uintptr_t)crc32 },

    // EGL
    { "eglGetDisplay", (uintptr_t)wrap_eglGetDisplay },
    { "eglInitialize", (uintptr_t)wrap_eglInitialize },
    { "eglTerminate", (uintptr_t)eglTerminate },
    { "eglChooseConfig", (uintptr_t)wrap_eglChooseConfig },
    { "eglGetConfigAttrib", (uintptr_t)wrap_eglGetConfigAttrib },
    { "eglCreateWindowSurface", (uintptr_t)wrap_eglCreateWindowSurface },
    { "eglCreatePbufferSurface", (uintptr_t)wrap_eglCreatePbufferSurface },
    { "eglCreateContext", (uintptr_t)wrap_eglCreateContext },
    { "eglMakeCurrent", (uintptr_t)wrap_eglMakeCurrent },
    { "eglGetCurrentContext", (uintptr_t)wrap_eglGetCurrentContext },
    { "eglSwapBuffers", (uintptr_t)wrap_eglSwapBuffers },
    { "eglDestroySurface", (uintptr_t)wrap_eglDestroySurface },
    { "eglDestroyContext", (uintptr_t)wrap_eglDestroyContext },
    { "eglGetProcAddress", (uintptr_t)eglGetProcAddress },
    { "eglGetError", (uintptr_t)eglGetError },
    { "eglQueryString", (uintptr_t)eglQueryString },
    { "eglQuerySurface", (uintptr_t)wrap_eglQuerySurface },

    // OpenGL ES 2.0
    { "glActiveTexture", (uintptr_t)GLTRACE_FN(glActiveTexture) },
    { "glAttachShader", (uintptr_t)glAttachShader },
    { "glBindAttribLocation", (uintptr_t)glBindAttribLocation },
    { "glBindBuffer", (uintptr_t)GLTRACE_FN(glBindBuffer) },
    { "glBindFramebuffer", (uintptr_t)GLTRACE_FN(glBindFramebuffer) },
    { "glBindRenderbuffer", (uintptr_t)glBindRenderbuffer },
    { "glBindTexture", (uintptr_t)GLTRACE_FN(glBindTexture) },
    { "glBlendColor", (uintptr_t)GLTRACE_FN(glBlendColor) },
    { "glBlendEquation", (uintptr_t)GLTRACE_FN(glBlendEquation) },
    { "glBlendEquationSeparate", (uintptr_t)glBlendEquationSeparate },
    { "glBlendFunc", (uintptr_t)GLTRACE_FN(glBlendFunc) },
    { "glBlendFuncSeparate", (uintptr_t)GLTRACE_FN(glBlendFuncSeparate) },
    { "glBufferData", (uintptr_t)glBufferData },
    { "glBufferSubData", (uintptr_t)glBufferSubData },
    { "glCheckFramebufferStatus", (uintptr_t)GLDIAG_FN(glCheckFramebufferStatus) },
    { "glClear", (uintptr_t)GLTRACE_FN(glClear) },
    { "glClearColor", (uintptr_t)GLTRACE_FN(glClearColor) },
    { "glClearDepthf", (uintptr_t)glClearDepthf },
    { "glClearStencil", (uintptr_t)glClearStencil },
    { "glColorMask", (uintptr_t)GLTRACE_FN(glColorMask) },
    { "glCompileShader", (uintptr_t)GLDIAG_FN(glCompileShader) },
    { "glCompressedTexImage2D", (uintptr_t)GL_COMPRESSED_TEX_IMPORT },
    { "glCompressedTexSubImage2D", (uintptr_t)glCompressedTexSubImage2D },
    { "glCopyTexImage2D", (uintptr_t)glCopyTexImage2D },
    { "glCopyTexSubImage2D", (uintptr_t)glCopyTexSubImage2D },
    { "glCreateProgram", (uintptr_t)glCreateProgram },
    { "glCreateShader", (uintptr_t)glCreateShader },
    { "glCullFace", (uintptr_t)GLTRACE_FN(glCullFace) },
    { "glDeleteBuffers", (uintptr_t)glDeleteBuffers },
    { "glDeleteFramebuffers", (uintptr_t)glDeleteFramebuffers },
    { "glDeleteProgram", (uintptr_t)glDeleteProgram },
    { "glDeleteRenderbuffers", (uintptr_t)glDeleteRenderbuffers },
    { "glDeleteShader", (uintptr_t)glDeleteShader },
    { "glDeleteTextures", (uintptr_t)glDeleteTextures },
    { "glDepthFunc", (uintptr_t)GLTRACE_FN(glDepthFunc) },
    { "glDepthMask", (uintptr_t)GLTRACE_FN(glDepthMask) },
    { "glDepthRangef", (uintptr_t)glDepthRangef },
    { "glDetachShader", (uintptr_t)glDetachShader },
    { "glDisable", (uintptr_t)GLTRACE_FN(glDisable) },
    { "glDisableVertexAttribArray", (uintptr_t)GLTRACE_FN(glDisableVertexAttribArray) },
    { "glDrawArrays", (uintptr_t)GLTRACE_FN(glDrawArrays) },
    { "glDrawElements", (uintptr_t)GLTRACE_FN(glDrawElements) },
    { "glEnable", (uintptr_t)GLTRACE_FN(glEnable) },
    { "glEnableVertexAttribArray", (uintptr_t)GLTRACE_FN(glEnableVertexAttribArray) },
    { "glFinish", (uintptr_t)glFinish },
    { "glFlush", (uintptr_t)glFlush },
    { "glFramebufferRenderbuffer", (uintptr_t)glFramebufferRenderbuffer },
    { "glFramebufferTexture2D", (uintptr_t)GLDIAG_FN(glFramebufferTexture2D) },
    { "glFrontFace", (uintptr_t)GLTRACE_FN(glFrontFace) },
    { "glGenBuffers", (uintptr_t)glGenBuffers },
    { "glGenerateMipmap", (uintptr_t)GLDIAG_FN(glGenerateMipmap) },
    { "glGenFramebuffers", (uintptr_t)glGenFramebuffers },
    { "glGenRenderbuffers", (uintptr_t)glGenRenderbuffers },
    { "glGenTextures", (uintptr_t)glGenTextures },
    { "glGetActiveAttrib", (uintptr_t)glGetActiveAttrib },
    { "glGetActiveUniform", (uintptr_t)glGetActiveUniform },
    { "glGetAttachedShaders", (uintptr_t)glGetAttachedShaders },
    { "glGetAttribLocation", (uintptr_t)glGetAttribLocation },
    { "glGetBooleanv", (uintptr_t)glGetBooleanv },
    { "glGetBufferParameteriv", (uintptr_t)glGetBufferParameteriv },
    { "glGetError", (uintptr_t)glGetError },
    { "glGetFloatv", (uintptr_t)glGetFloatv },
    { "glGetFramebufferAttachmentParameteriv", (uintptr_t)glGetFramebufferAttachmentParameteriv },
    { "glGetIntegerv", (uintptr_t)glGetIntegerv },
    { "glGetProgramiv", (uintptr_t)glGetProgramiv },
    { "glGetProgramInfoLog", (uintptr_t)glGetProgramInfoLog },
    { "glGetRenderbufferParameteriv", (uintptr_t)glGetRenderbufferParameteriv },
    { "glGetShaderiv", (uintptr_t)glGetShaderiv },
    { "glGetShaderInfoLog", (uintptr_t)glGetShaderInfoLog },
    { "glGetShaderPrecisionFormat", (uintptr_t)glGetShaderPrecisionFormat },
    { "glGetShaderSource", (uintptr_t)glGetShaderSource },
    { "glGetString", (uintptr_t)wrap_glGetString },
    { "glGetTexParameterfv", (uintptr_t)glGetTexParameterfv },
    { "glGetTexParameteriv", (uintptr_t)glGetTexParameteriv },
    { "glGetUniformfv", (uintptr_t)glGetUniformfv },
    { "glGetUniformiv", (uintptr_t)glGetUniformiv },
    { "glGetUniformLocation", (uintptr_t)glGetUniformLocation },
    { "glGetVertexAttribfv", (uintptr_t)glGetVertexAttribfv },
    { "glGetVertexAttribiv", (uintptr_t)glGetVertexAttribiv },
    { "glGetVertexAttribPointerv", (uintptr_t)glGetVertexAttribPointerv },
    { "glHint", (uintptr_t)glHint },
    { "glIsBuffer", (uintptr_t)glIsBuffer },
    { "glIsEnabled", (uintptr_t)glIsEnabled },
    { "glIsFramebuffer", (uintptr_t)glIsFramebuffer },
    { "glIsProgram", (uintptr_t)glIsProgram },
    { "glIsRenderbuffer", (uintptr_t)glIsRenderbuffer },
    { "glIsShader", (uintptr_t)glIsShader },
    { "glIsTexture", (uintptr_t)glIsTexture },
    { "glLineWidth", (uintptr_t)glLineWidth },
    { "glLinkProgram", (uintptr_t)GLDIAG_FN(glLinkProgram) },
    { "glPixelStorei", (uintptr_t)glPixelStorei },
    { "glPolygonOffset", (uintptr_t)glPolygonOffset },
    { "glReadPixels", (uintptr_t)glReadPixels },
    { "glReleaseShaderCompiler", (uintptr_t)glReleaseShaderCompiler },
    { "glRenderbufferStorage", (uintptr_t)GLDIAG_FN(glRenderbufferStorage) },
    { "glSampleCoverage", (uintptr_t)glSampleCoverage },
    { "glScissor", (uintptr_t)GLTRACE_FN(glScissor) },
    { "glShaderBinary", (uintptr_t)glShaderBinary },
    { "glShaderSource", (uintptr_t)glShaderSource },
    { "glStencilFunc", (uintptr_t)GLTRACE_FN(glStencilFunc) },
    { "glStencilFuncSeparate", (uintptr_t)glStencilFuncSeparate },
    { "glStencilMask", (uintptr_t)glStencilMask },
    { "glStencilMaskSeparate", (uintptr_t)glStencilMaskSeparate },
    { "glStencilOp", (uintptr_t)GLTRACE_FN(glStencilOp) },
    { "glStencilOpSeparate", (uintptr_t)glStencilOpSeparate },
    { "glTexImage2D", (uintptr_t)GLDIAG_FN(glTexImage2D) },
    { "glTexParameterf", (uintptr_t)GLDIAG_FN(glTexParameterf) },
    { "glTexParameterfv", (uintptr_t)glTexParameterfv },
    { "glTexParameteri", (uintptr_t)GLDIAG_FN(glTexParameteri) },
    { "glTexParameteriv", (uintptr_t)glTexParameteriv },
    { "glTexSubImage2D", (uintptr_t)GLDIAG_FN(glTexSubImage2D) },
    { "glUniform1f", (uintptr_t)GLTRACE_FN(glUniform1f) },
    { "glUniform1fv", (uintptr_t)GLTRACE_FN(glUniform1fv) },
    { "glUniform1i", (uintptr_t)GLTRACE_FN(glUniform1i) },
    { "glUniform1iv", (uintptr_t)GLTRACE_FN(glUniform1iv) },
    { "glUniform2f", (uintptr_t)GLTRACE_FN(glUniform2f) },
    { "glUniform2fv", (uintptr_t)GLTRACE_FN(glUniform2fv) },
    { "glUniform2i", (uintptr_t)glUniform2i },
    { "glUniform2iv", (uintptr_t)glUniform2iv },
    { "glUniform3f", (uintptr_t)GLTRACE_FN(glUniform3f) },
    { "glUniform3fv", (uintptr_t)GLTRACE_FN(glUniform3fv) },
    { "glUniform3i", (uintptr_t)glUniform3i },
    { "glUniform3iv", (uintptr_t)glUniform3iv },
    { "glUniform4f", (uintptr_t)GLTRACE_FN(glUniform4f) },
    { "glUniform4fv", (uintptr_t)GLTRACE_FN(glUniform4fv) },
    { "glUniform4i", (uintptr_t)glUniform4i },
    { "glUniform4iv", (uintptr_t)glUniform4iv },
    { "glUniformMatrix2fv", (uintptr_t)glUniformMatrix2fv },
    { "glUniformMatrix3fv", (uintptr_t)GLTRACE_FN(glUniformMatrix3fv) },
    { "glUniformMatrix4fv", (uintptr_t)GLTRACE_FN(glUniformMatrix4fv) },
    { "glUseProgram", (uintptr_t)GLTRACE_FN(glUseProgram) },
    { "glValidateProgram", (uintptr_t)glValidateProgram },
    { "glVertexAttrib1f", (uintptr_t)glVertexAttrib1f },
    { "glVertexAttrib1fv", (uintptr_t)glVertexAttrib1fv },
    { "glVertexAttrib2f", (uintptr_t)glVertexAttrib2f },
    { "glVertexAttrib2fv", (uintptr_t)glVertexAttrib2fv },
    { "glVertexAttrib3f", (uintptr_t)glVertexAttrib3f },
    { "glVertexAttrib3fv", (uintptr_t)glVertexAttrib3fv },
    { "glVertexAttrib4f", (uintptr_t)GLTRACE_FN(glVertexAttrib4f) },
    { "glVertexAttrib4fv", (uintptr_t)GLTRACE_FN(glVertexAttrib4fv) },
    { "glVertexAttribPointer", (uintptr_t)GLTRACE_FN(glVertexAttribPointer) },
    { "glViewport", (uintptr_t)GLTRACE_FN(glViewport) },
    { "glMapBufferOES", (uintptr_t)&wrap_glMapBufferOES },
    { "glUnmapBufferOES", (uintptr_t)&wrap_glUnmapBufferOES },
};

// ============================================================================
// GL serialization (see stubs.s)
// ============================================================================
#define GL_THUNK_COUNT 256
extern void gl_thunks_start(void);
uintptr_t gl_real_table[GL_THUNK_COUNT];
static RMutex s_gl_lock;

static __thread int t_gl_depth;

// The outermost entry also hands the real context to this thread.
void gl_lock_enter(void) {
#if AIRBORNE_PROFILE
    if (!rmutexTryLock(&s_gl_lock)) {
        PROF_BEGIN();
        rmutexLock(&s_gl_lock);
        PROF_END(PROF_GLWAIT);
    }
#else
    rmutexLock(&s_gl_lock);
#endif
    if (t_gl_depth++ == 0) egl_ctx_acquire();
}

void gl_lock_leave(void) {
    t_gl_depth--;
    rmutexUnlock(&s_gl_lock);
}

// For an import that has long work to do which is not GL (see astc.c): let
// other threads use GL meanwhile. Only valid from the outermost call.
void gl_lock_pause(void) { gl_lock_leave(); }
void gl_lock_resume(void) { gl_lock_enter(); }

static bool needs_gl_lock(const char *name) {
    if (strncmp(name, "gl", 2) == 0) return true;
    static const char *const egl[] = {
        "eglSwapBuffers", "eglMakeCurrent", "eglCreateContext", "eglDestroyContext",
        "eglCreateWindowSurface", "eglDestroySurface", "eglCreatePbufferSurface", "eglTerminate",
    };
    for (size_t i = 0; i < sizeof(egl) / sizeof(egl[0]); i++)
        if (strcmp(name, egl[i]) == 0) return true;
    return false;
}

static void install_gl_thunks(void) {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;

    int n = 0;
    for (size_t i = 0; i < sizeof(s_default_dynlib) / sizeof(s_default_dynlib[0]); i++) {
        if (!needs_gl_lock(s_default_dynlib[i].symbol)) continue;
        if (n >= GL_THUNK_COUNT) fatal_error("Too many GL imports for the thunk table");
        gl_real_table[n] = s_default_dynlib[i].func;
        s_default_dynlib[i].func = (uintptr_t)gl_thunks_start + 8 * n;
        n++;
    }
    l_info("GL serialization: %d imports routed through locking thunks", n);
}

void resolve_dynamic_dependencies(so_module *mod) {
    init_android_streams();
    install_gl_thunks();
    l_info("Resolving dynamic symbols for '%s' (%zu entries in default library)...",
           mod->soname ? mod->soname : "mod",
           sizeof(s_default_dynlib) / sizeof(s_default_dynlib[0]));
    so_resolve(mod, s_default_dynlib, sizeof(s_default_dynlib) / sizeof(s_default_dynlib[0]), 0);
}
