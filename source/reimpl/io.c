#include "io.h"
#include "../utils/init.h"
#include "../utils/logger.h"
#include "../utils/prof.h"
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <switch.h>
#include <stdio.h>
#include <malloc.h>
#include <dirent.h>
#include <sys/stat.h>

typedef struct {
    const char *android;
    const char *host;
} PathMapping;

// Longest prefixes first.
static const PathMapping s_path_map[] = {
    { ANDROID_FILES,                                         DATA_PATH "files" },
    { "/storage/emulated/0/Android/data/" ANDROID_PKG "/files", DATA_PATH "files" },
    { "/sdcard/Android/obb/" ANDROID_PKG,                    DATA_PATH "files" },
    { "/storage/emulated/0/Android/obb/" ANDROID_PKG,        DATA_PATH "files" },
    { "/sdcard/Android/data/" ANDROID_PKG,                   DATA_ROOT },
    { "/storage/emulated/0/Android/data/" ANDROID_PKG,       DATA_ROOT },
    { ANDROID_INTERNAL,                                      DATA_PATH "internal" },
    { ANDROID_CACHE,                                         DATA_PATH "cache" },
    { "/data/data/" ANDROID_PKG,                             DATA_PATH "internal" },
    { "/data/user/0/" ANDROID_PKG "/files",                  DATA_PATH "internal" },
    { "/data/user/0/" ANDROID_PKG "/cache",                  DATA_PATH "cache" },
    { ANDROID_APK,                                           DATA_PATH "base.apk" },
    { ANDROID_LIBDIR,                                        DATA_ROOT },
    { "/storage/emulated/0",                                 DATA_PATH "sdcard" },
    { ANDROID_SDCARD,                                        DATA_PATH "sdcard" },
    { "/proc",                                               SYSROOT_PATH "proc" },
    { "/sys",                                                SYSROOT_PATH "sys" },
};

const char *translate_path(const char *path, char *buffer, size_t buf_size) {
    if (!path || path[0] != '/') return path;

    for (size_t i = 0; i < sizeof(s_path_map) / sizeof(s_path_map[0]); i++) {
        size_t len = strlen(s_path_map[i].android);
        if (strncmp(path, s_path_map[i].android, len) != 0) continue;
        if (path[len] != '/' && path[len] != '\0') continue;

        const char *rest = path + len;
        while (rest[0] == '/' && rest[1] == '/') rest++;
        snprintf(buffer, buf_size, "%s%s", s_path_map[i].host, rest);
        return buffer;
    }

    static int s_warned = 0;
    if (s_warned < 40) {
        s_warned++;
        l_warn("[io] unmapped absolute path: %s", path);
    }
    return path;
}

// ============================================================================
// Bionic (arm64) layouts. newlib's struct stat / struct dirent / O_* flags all
// differ, so nothing here may be passed straight through.
// ============================================================================
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t  st_size;
    int32_t  st_blksize;
    int32_t  __pad2;
    int64_t  st_blocks;
    int64_t  st_atime_sec, st_atime_nsec;
    int64_t  st_mtime_sec, st_mtime_nsec;
    int64_t  st_ctime_sec, st_ctime_nsec;
    uint32_t __unused4, __unused5;
} bionic_stat;

typedef struct {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
} bionic_dirent;

typedef struct {
    DIR *dir;
    bool trace;
    bionic_dirent entry;
} guest_dir;

static void io_trace(const char *op, const char *path, const char *translated, bool ok) {
    // Only failures are interesting once the data layout is known.
    static int s_budget = 400;
    if (ok || s_budget <= 0) return;
    s_budget--;
    l_debug("[io] %s %s%s -> %s", op, ok ? "" : "FAILED ", path ? path : "(null)", translated ? translated : "(null)");
}

static void stat_to_bionic(const struct stat *in, bionic_stat *out) {
    memset(out, 0, sizeof(*out));
    out->st_dev = in->st_dev;
    out->st_ino = in->st_ino;
    out->st_mode = in->st_mode;
    out->st_nlink = in->st_nlink ? in->st_nlink : 1;
    out->st_uid = 1000;
    out->st_gid = 1000;
    out->st_size = in->st_size;
    out->st_blksize = 4096;
    out->st_blocks = (in->st_size + 511) / 512;
    out->st_atime_sec = in->st_atime;
    out->st_mtime_sec = in->st_mtime;
    out->st_ctime_sec = in->st_ctime;
}

static int open_flags_to_host(int f) {
    int out = f & 3; // O_RDONLY / O_WRONLY / O_RDWR
    if (f & 0100)     out |= O_CREAT;
    if (f & 0200)     out |= O_EXCL;
    if (f & 01000)    out |= O_TRUNC;
    if (f & 02000)    out |= O_APPEND;
    if (f & 04000)    out |= O_NONBLOCK;
    if (f & 040000)   out |= O_DIRECTORY;
    return out;
}

FILE *wrap_fopen(const char *path, const char *mode) {
    char buf[512];
    const char *translated = translate_path(path, buf, sizeof(buf));
    PROF_BEGIN();
    FILE *f = fopen(translated, mode);
    PROF_END(PROF_OPEN);
    io_trace("fopen", path, translated, f != NULL);
    return f;
}

// /dev/urandom does not exist on Horizon; libc++'s std::random_device (used
// by the game's static initializers) opens it, so serve it from the kernel RNG.
#define URANDOM_FD 0x7EADBEEF

static bool is_random_device(const char *path) {
    return path && (strcmp(path, "/dev/urandom") == 0 || strcmp(path, "/dev/random") == 0);
}

// ============================================================================
// Read-ahead cache for read-only descriptors.
//
// The engine walks its .jpk archives with a huge number of tiny read()/lseek()
// calls. Each one is an IPC round trip to the filesystem service, which makes
// loading crawl, so read-only files are served from a per-descriptor buffer.
// ============================================================================
#define FDC_MAX_FD   1024
#define FDC_BUF_SIZE (256 * 1024)

typedef struct {
    Mutex lock;
    off_t pos;       // position as the guest sees it
    off_t size;
    off_t buf_off;   // file offset of buf[0]
    size_t buf_len;
    uint8_t *buf;
} fd_cache;

static fd_cache *s_fd_cache[FDC_MAX_FD];
static Mutex s_fd_cache_lock;
static u64 s_read_calls, s_read_bytes, s_read_ipc;

static fd_cache *fdc_get(int fd) {
    if (fd < 0 || fd >= FDC_MAX_FD) return NULL;
    return __atomic_load_n(&s_fd_cache[fd], __ATOMIC_ACQUIRE);
}

static void fdc_attach(int fd) {
    if (fd < 0 || fd >= FDC_MAX_FD) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) return;

    fd_cache *c = (fd_cache *)calloc(1, sizeof(fd_cache));
    if (!c) return;
    c->buf = (uint8_t *)malloc(FDC_BUF_SIZE);
    if (!c->buf) { free(c); return; }
    c->size = st.st_size;

    mutexLock(&s_fd_cache_lock);
    s_fd_cache[fd] = c;
    mutexUnlock(&s_fd_cache_lock);
}

// Drops the cache and leaves the real descriptor at the guest's position.
static void fdc_detach(int fd, bool sync_position) {
    if (fd < 0 || fd >= FDC_MAX_FD) return;
    mutexLock(&s_fd_cache_lock);
    fd_cache *c = s_fd_cache[fd];
    s_fd_cache[fd] = NULL;
    mutexUnlock(&s_fd_cache_lock);
    if (!c) return;

    mutexLock(&c->lock);
    if (sync_position) lseek(fd, c->pos, SEEK_SET);
    mutexUnlock(&c->lock);
    free(c->buf);
    free(c);
}

// Reads `count` bytes at `offset`; caller holds c->lock.
static ssize_t fdc_read_at(int fd, fd_cache *c, void *out, size_t count, off_t offset) {
    size_t done = 0;
    while (done < count && offset < c->size) {
        if (offset >= c->buf_off && offset < c->buf_off + (off_t)c->buf_len) {
            size_t avail = (size_t)(c->buf_off + c->buf_len - offset);
            size_t n = (count - done < avail) ? count - done : avail;
            memcpy((uint8_t *)out + done, c->buf + (offset - c->buf_off), n);
            done += n;
            offset += n;
            continue;
        }

        if (lseek(fd, offset, SEEK_SET) == (off_t)-1) break;
        s_read_ipc++;

        if (count - done >= FDC_BUF_SIZE) {
            // Large request: read straight into the caller's buffer.
            ssize_t n = read(fd, (uint8_t *)out + done, count - done);
            if (n <= 0) break;
            done += n;
            offset += n;
            continue;
        }

        ssize_t n = read(fd, c->buf, FDC_BUF_SIZE);
        if (n <= 0) { c->buf_len = 0; break; }
        c->buf_off = offset;
        c->buf_len = (size_t)n;
    }
    return (ssize_t)done;
}

// Last open() that has not returned yet, for the stall watchdog.
static char s_inflight[600];
static u64 s_inflight_since;

const char *io_inflight(unsigned *seconds) {
    u64 since = s_inflight_since;
    if (!since) return NULL;
    if (seconds) *seconds = (unsigned)(armTicksToNs(armGetSystemTick() - since) / 1000000000ULL);
    return s_inflight;
}

void io_read_stats(unsigned long *calls, unsigned long *bytes, unsigned long *ipc) {
    if (calls) *calls = (unsigned long)s_read_calls;
    if (bytes) *bytes = (unsigned long)s_read_bytes;
    if (ipc) *ipc = (unsigned long)s_read_ipc;
}

int wrap_close(int fd) {
    if (fd == URANDOM_FD) return 0;
    fdc_detach(fd, false);
    return close(fd);
}

ssize_t wrap_read(int fd, void *buf, size_t count) {
    if (fd == URANDOM_FD) {
        randomGet(buf, count);
        return (ssize_t)count;
    }

    fd_cache *c = fdc_get(fd);
    if (!c) return read(fd, buf, count);

    PROF_BEGIN();
    mutexLock(&c->lock);
    ssize_t n = fdc_read_at(fd, c, buf, count, c->pos);
    PROF_END(PROF_READ);
    if (n > 0) c->pos += n;
    s_read_calls++;
    s_read_bytes += (n > 0) ? n : 0;
    mutexUnlock(&c->lock);
    return n;
}

ssize_t wrap_pread(int fd, void *buf, size_t count, off_t offset) {
    fd_cache *c = fdc_get(fd);
    if (c) {
        mutexLock(&c->lock);
        ssize_t n = fdc_read_at(fd, c, buf, count, offset);
        mutexUnlock(&c->lock);
        return n;
    }

    off_t cur = lseek(fd, 0, SEEK_CUR);
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) return -1;
    ssize_t ret = read(fd, buf, count);
    lseek(fd, cur, SEEK_SET);
    return ret;
}

off_t wrap_lseek(int fd, off_t offset, int whence) {
    fd_cache *c = fdc_get(fd);
    if (!c) return lseek(fd, offset, whence);

    mutexLock(&c->lock);
    off_t target;
    switch (whence) {
        case SEEK_SET: target = offset; break;
        case SEEK_CUR: target = c->pos + offset; break;
        case SEEK_END: target = c->size + offset; break;
        default:       target = -1; break;
    }
    if (target >= 0) c->pos = target;
    mutexUnlock(&c->lock);
    return target;
}

ssize_t wrap_write(int fd, const void *buf, size_t count) {
    fdc_detach(fd, true);
    return write(fd, buf, count);
}

FILE *wrap_fdopen(int fd, const char *mode) {
    fdc_detach(fd, true);
    return fdopen(fd, mode);
}

int wrap_open(const char *path, int flags, ...) {
    if (is_random_device(path)) return URANDOM_FD;
    char buf[512];
    const char *translated = translate_path(path, buf, sizeof(buf));
    int mode = 0;
    if (flags & 0100) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    (void)mode;
    snprintf(s_inflight, sizeof(s_inflight), "open(%s, flags=0x%x)", translated, flags);
    s_inflight_since = armGetSystemTick();
    PROF_BEGIN();
    int fd = open(translated, open_flags_to_host(flags), 0666);
    s_inflight_since = 0;
    io_trace("open", path, translated, fd >= 0);
    if (fd >= 0 && (flags & 3) == 0) fdc_attach(fd); // O_RDONLY
    PROF_END(PROF_OPEN);
    return fd;
}

int wrap_stat(const char *path, void *buf) {
    char b[512];
    const char *translated = translate_path(path, b, sizeof(b));
    struct stat st;
    int ret = stat(translated, &st);
    io_trace("stat", path, translated, ret == 0);
    if (ret == 0 && buf) stat_to_bionic(&st, (bionic_stat *)buf);
    return ret;
}

int wrap_fstat(int fd, void *buf) {
    struct stat st;
    int ret = fstat(fd, &st);
    if (ret == 0 && buf) stat_to_bionic(&st, (bionic_stat *)buf);
    return ret;
}

int wrap_access(const char *path, int mode) {
    char buf[512];
    const char *translated = translate_path(path, buf, sizeof(buf));
    (void)mode;
    struct stat st;
    int ret = stat(translated, &st);
    io_trace("access", path, translated, ret == 0);
    return ret;
}

int wrap_mkdir(const char *path, mode_t mode) {
    char buf[512];
    const char *translated = translate_path(path, buf, sizeof(buf));
    // The SD filesystem rejects a trailing slash on mkdir.
    char clean[512];
    snprintf(clean, sizeof(clean), "%s", translated);
    size_t len = strlen(clean);
    while (len > 1 && clean[len - 1] == '/' && clean[len - 2] != ':') clean[--len] = 0;
    return mkdir(clean, mode);
}

void *wrap_opendir(const char *name) {
    char buf[512];
    const char *translated = translate_path(name, buf, sizeof(buf));
    // The SD filesystem rejects a trailing slash here too.
    char clean[512];
    snprintf(clean, sizeof(clean), "%s", translated ? translated : "");
    size_t len = strlen(clean);
    while (len > 1 && clean[len - 1] == '/' && clean[len - 2] != ':') clean[--len] = 0;
    translated = clean;
    DIR *d = opendir(translated);
    io_trace("opendir", name, translated, d != NULL);
    if (!d) return NULL;
    guest_dir *g = (guest_dir *)calloc(1, sizeof(guest_dir));
    g->dir = d;
    g->trace = strstr(translated, "dlcs") != NULL;
    return g;
}

void *wrap_readdir(void *dirp) {
    guest_dir *g = (guest_dir *)dirp;
    if (!g) return NULL;
    struct dirent *e = readdir(g->dir);
    if (!e) return NULL;
    memset(&g->entry, 0, sizeof(g->entry));
    g->entry.d_ino = e->d_ino ? e->d_ino : 1;
    g->entry.d_reclen = sizeof(bionic_dirent);
    g->entry.d_type = (e->d_type == DT_DIR) ? 4 : 8; // DT_DIR / DT_REG
    snprintf(g->entry.d_name, sizeof(g->entry.d_name), "%s", e->d_name);
    if (g->trace) l_debug("[io] readdir -> %s (type %d)", g->entry.d_name, g->entry.d_type);
    return &g->entry;
}

int wrap_closedir(void *dirp) {
    guest_dir *g = (guest_dir *)dirp;
    if (!g) return -1;
    int ret = closedir(g->dir);
    free(g);
    return ret;
}

// The engine chdir()s into its data folder and then uses relative paths, so
// the working directory has to really change. The guest keeps seeing the
// Android-style path it asked for.
static char s_guest_cwd[512] = "/";

int wrap_chdir(const char *path) {
    char buf[512];
    const char *translated = translate_path(path, buf, sizeof(buf));
    int ret = chdir(translated);
    l_info("[io] chdir %s -> %s (%s)", path ? path : "(null)", translated ? translated : "(null)",
           ret == 0 ? "ok" : "FAILED");
    if (ret == 0 && path) snprintf(s_guest_cwd, sizeof(s_guest_cwd), "%s", path);
    return ret;
}

char *wrap_getcwd(char *buf, size_t size) {
    if (!buf) return strdup(s_guest_cwd);
    snprintf(buf, size, "%s", s_guest_cwd);
    return buf;
}

int wrap_remove(const char *pathname) {
    char buf[512];
    const char *translated = translate_path(pathname, buf, sizeof(buf));
    int ret = remove(translated);
    io_trace("remove", pathname, translated, ret == 0);
    return ret;
}

int wrap_unlink(const char *pathname) {
    char buf[512];
    const char *translated = translate_path(pathname, buf, sizeof(buf));
    int ret = unlink(translated);
    io_trace("unlink", pathname, translated, ret == 0);
    return ret;
}

int wrap_rename(const char *oldpath, const char *newpath) {
    char buf1[512], buf2[512];
    const char *tr_old = translate_path(oldpath, buf1, sizeof(buf1));
    const char *tr_new = translate_path(newpath, buf2, sizeof(buf2));
    // FAT rename does not replace an existing destination.
    remove(tr_new);
    int ret = rename(tr_old, tr_new);
    io_trace("rename", oldpath, tr_new, ret == 0);
    return ret;
}

int wrap_statfs(const char *path, void *buf) {
    (void)path;
    if (buf) {
        uint64_t *f = (uint64_t *)buf;
        memset(f, 0, 120);
        f[0] = 0xEF53;                 // f_type
        f[1] = 4096;                   // f_bsize
        f[2] = 16ULL * 1024 * 1024;    // f_blocks
        f[3] = 4ULL * 1024 * 1024;     // f_bfree  (16 GB free)
        f[4] = 4ULL * 1024 * 1024;     // f_bavail
        f[5] = 1000000;                // f_files
        f[6] = 1000000;                // f_ffree
        f[8] = 255;                    // f_namelen
        f[9] = 4096;                   // f_frsize
    }
    return 0;
}

// mmap: anonymous mappings become heap blocks; file mappings are read in.
void *wrap_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr; (void)prot;
    size_t size = (length + 0xFFF) & ~(size_t)0xFFF;
    void *mem = memalign(0x1000, size);
    if (!mem) return (void *)-1;
    memset(mem, 0, size);

    if (!(flags & 0x20) && fd >= 0) { // !MAP_ANONYMOUS
        off_t saved = lseek(fd, 0, SEEK_CUR);
        if (lseek(fd, offset, SEEK_SET) != (off_t)-1) {
            size_t done = 0;
            while (done < length) {
                ssize_t n = read(fd, (char *)mem + done, length - done);
                if (n <= 0) break;
                done += n;
            }
        }
        lseek(fd, saved, SEEK_SET);
    }
    return mem;
}

int wrap_munmap(void *addr, size_t length) {
    (void)length;
    free(addr);
    return 0;
}
