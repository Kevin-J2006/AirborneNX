#include "io.h"
#include "shopmod.h"
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
#include <errno.h>
#include <ctype.h>
#include <strings.h>

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

static const char *map_android_path(const char *path, char *buffer, size_t buf_size) {
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

const char *translate_path(const char *path, char *buffer, size_t buf_size) {
    return shopmod_redirect(map_android_path(path, buffer, buf_size));
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

// A downloaded track pack. On every boot the engine's asset manager deletes
// all the packs in dlcs/ and then mounts the ones listed in its metadata,
// expecting to download them again. With no network that destroys the user's
// only copy and leaves the tracks unavailable, so removals are refused: the
// packs stay, and the engine mounts them as if they had just been fetched.
static bool is_dlc_pack(const char *path) {
    if (!path) return false;
    size_t len = strlen(path);
    return len > 4 && strcmp(path + len - 4, ".jpk") == 0 && strstr(path, "dlcs/") != NULL;
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

// ============================================================================
// What is already known about the card.
//
// The engine asks the filesystem the same things over and over: entering one
// menu stats every track pack dozens of times and opens the same .bgin a
// hundred times, all from the thread that draws, and every one of those is a
// round trip to the filesystem service. Answers are remembered here until
// something the game does could change them.
// ============================================================================
#define IO_KEY_MAX   200
#define STAT_SLOTS   256
#define MISSING_MAX  8
#define WRITERS_MAX  16
#define PARK_MAX     24
#define MAP_SLOTS    48
#define MAP_MIN_SIZE (32 * 1024)            // smaller mappings are cheap to read again
#define MAP_BUDGET   (40 * 1024 * 1024)     // bytes kept for mappings nobody holds

typedef struct { char key[IO_KEY_MAX]; struct stat st; } stat_entry;
typedef struct { uintptr_t handle; char key[IO_KEY_MAX]; } writer_entry;
typedef struct { int fd; bool used; u64 stamp; char key[IO_KEY_MAX]; } parked_fd;
typedef struct {
    void *mem;              // NULL = free slot
    size_t length, size;
    off_t offset;
    int refs;               // mappings the game currently holds
    bool stale;             // the file changed: free on the last munmap
    u64 stamp;
    char key[IO_KEY_MAX];
} map_entry;

static Mutex s_known_lock;                          // guards everything below
static char s_host_cwd[IO_KEY_MAX];
static stat_entry s_stats[STAT_SLOTS];
static char s_missing[MISSING_MAX][IO_KEY_MAX];     // directories that do not exist
static unsigned s_missing_next;
static writer_entry s_writers[WRITERS_MAX];         // files open for writing
static parked_fd s_parked[PARK_MAX];                // closed by the game, kept open here
static u64 s_park_clock;
static map_entry s_maps[MAP_SLOTS];                 // file contents mapped by the game
static u32 s_generation;                            // bumped by every invalidation
static bool s_caches_off;                           // lost track of a writer

static void fdc_discard(int fd);

// Absolute form of a translated path, which is what the tables are keyed by
// (compared without case, like the card's filesystem does). False when the
// path cannot be keyed; the caller then goes straight to the filesystem.
static bool io_key(const char *translated, char *key) {
    if (!translated || !translated[0] || s_caches_off) return false;
    size_t n = 0;
    if (!strchr(translated, ':')) {
        if (translated[0] == '/') return false;
        if (!s_host_cwd[0] && !getcwd(s_host_cwd, sizeof(s_host_cwd))) return false;
        n = strlen(s_host_cwd);
        if (n + 2 >= IO_KEY_MAX) return false;
        memcpy(key, s_host_cwd, n);
        if (n && key[n - 1] != '/') key[n++] = '/';
    }
    for (const char *p = translated; *p; p++) {
        bool after_slash = n && key[n - 1] == '/';
        if (after_slash && p[0] == '/') continue;
        if (after_slash && p[0] == '.' && p[1] == '/') { p++; continue; }
        if (n + 1 >= IO_KEY_MAX) return false;
        key[n++] = *p;
    }
    while (n > 1 && key[n - 1] == '/' && key[n - 2] != ':') n--;
    key[n] = 0;
    return true;
}

static u32 key_hash(const char *key) {
    u32 hash = 2166136261u;
    for (; *key; key++) hash = (hash ^ (u8)tolower((unsigned char)*key)) * 16777619u;
    return hash;
}

// The three below need s_known_lock held.
static bool under_missing_dir(const char *key) {
    for (int i = 0; i < MISSING_MAX; i++) {
        size_t len = strlen(s_missing[i]);
        if (len && strncasecmp(key, s_missing[i], len) == 0 && key[len] == '/') return true;
    }
    return false;
}

static bool being_written(const char *key) {
    for (int i = 0; i < WRITERS_MAX; i++)
        if (s_writers[i].handle && strcasecmp(s_writers[i].key, key) == 0) return true;
    return false;
}

static void forget_answers(const char *key) {
    s_generation++;
    if (!key) {
        memset(s_missing, 0, sizeof(s_missing));
        memset(s_stats, 0, sizeof(s_stats));
        return;
    }
    // A file changing says nothing about which directories exist, and the
    // game rewrites its timers several times a second.
    stat_entry *e = &s_stats[key_hash(key) % STAT_SLOTS];
    if (strcasecmp(e->key, key) == 0) e->key[0] = 0;
}

// The game is about to change `key` (everything when NULL): drop what is
// known about it and really close the descriptors parked on it, since the
// filesystem refuses to write, rename or delete a file that is still open.
static void io_invalidate(const char *key) {
    int fds[PARK_MAX], count = 0;
    void *mems[MAP_SLOTS];
    int mem_count = 0;
    mutexLock(&s_known_lock);
    forget_answers(key);
    for (int i = 0; i < PARK_MAX; i++) {
        if (!s_parked[i].used || (key && strcasecmp(s_parked[i].key, key) != 0)) continue;
        s_parked[i].used = false;
        fds[count++] = s_parked[i].fd;
    }
    for (int i = 0; i < MAP_SLOTS; i++) {
        map_entry *m = &s_maps[i];
        if (!m->mem || (key && strcasecmp(m->key, key) != 0)) continue;
        if (m->refs > 0) { m->stale = true; continue; }
        mems[mem_count++] = m->mem;
        m->mem = NULL;
    }
    mutexUnlock(&s_known_lock);
    for (int i = 0; i < count; i++) fdc_discard(fds[i]);
    for (int i = 0; i < mem_count; i++) free(mems[i]);
}

// ----------------------------------------------------------------------------
// File mappings. mmap is emulated by reading the range into memory, and the
// engine maps a whole interface file (sprites.bgin is 12 MB) every time it
// wants one sprite out of it, then unmaps it. Read-only mappings are kept
// after munmap and handed out again for the same range of the same file.
// ----------------------------------------------------------------------------

// The slot with the least recently released mapping nobody holds; needs
// s_known_lock.
static map_entry *oldest_idle_map(void) {
    map_entry *oldest = NULL;
    for (int i = 0; i < MAP_SLOTS; i++) {
        map_entry *m = &s_maps[i];
        if (m->mem && m->refs == 0 && (!oldest || m->stamp < oldest->stamp)) oldest = m;
    }
    return oldest;
}

static void *map_take(const char *key, off_t offset, size_t length, u32 *generation) {
    void *mem = NULL;
    mutexLock(&s_known_lock);
    *generation = s_generation;
    for (int i = 0; i < MAP_SLOTS && !mem; i++) {
        map_entry *m = &s_maps[i];
        if (!m->mem || m->stale || m->offset != offset || m->length != length) continue;
        if (strcasecmp(m->key, key) != 0) continue;
        m->refs++;
        mem = m->mem;
    }
    mutexUnlock(&s_known_lock);
    return mem;
}

// Remembers a mapping just read, unless the file may have changed meanwhile.
static void map_add(const char *key, off_t offset, size_t length, void *mem, size_t size, u32 generation) {
    void *evicted = NULL;
    mutexLock(&s_known_lock);
    if (generation == s_generation && !s_caches_off && !being_written(key)) {
        map_entry *slot = NULL;
        for (int i = 0; i < MAP_SLOTS && !slot; i++)
            if (!s_maps[i].mem) slot = &s_maps[i];
        if (!slot && (slot = oldest_idle_map()) != NULL) evicted = slot->mem;
        if (slot) {
            slot->mem = mem;
            slot->length = length;
            slot->size = size;
            slot->offset = offset;
            slot->refs = 1;
            slot->stale = false;
            snprintf(slot->key, IO_KEY_MAX, "%s", key);
        }
    }
    mutexUnlock(&s_known_lock);
    free(evicted);
}

// The game unmapped `mem`. False if it is not a remembered mapping.
static bool map_release(void *mem) {
    void *freed[MAP_SLOTS];
    int count = 0;
    bool found = false;
    mutexLock(&s_known_lock);
    for (int i = 0; i < MAP_SLOTS && !found; i++) {
        map_entry *m = &s_maps[i];
        if (m->mem != mem) continue;
        found = true;
        m->stamp = ++s_park_clock;
        if (--m->refs == 0 && m->stale) {
            freed[count++] = m->mem;
            m->mem = NULL;
        }
    }
    for (;;) {
        size_t idle = 0;
        for (int i = 0; i < MAP_SLOTS; i++)
            if (s_maps[i].mem && s_maps[i].refs == 0) idle += s_maps[i].size;
        map_entry *oldest = idle > MAP_BUDGET ? oldest_idle_map() : NULL;
        if (!oldest) break;
        freed[count++] = oldest->mem;
        oldest->mem = NULL;
    }
    mutexUnlock(&s_known_lock);
    for (int i = 0; i < count; i++) free(freed[i]);
    return found;
}

// Out of memory: give back every mapping nobody holds.
static void map_drop_idle(void) {
    void *freed[MAP_SLOTS];
    int count = 0;
    mutexLock(&s_known_lock);
    for (int i = 0; i < MAP_SLOTS; i++) {
        if (!s_maps[i].mem || s_maps[i].refs > 0) continue;
        freed[count++] = s_maps[i].mem;
        s_maps[i].mem = NULL;
    }
    mutexUnlock(&s_known_lock);
    for (int i = 0; i < count; i++) free(freed[i]);
}

void io_invalidate_all(void) {
    io_invalidate(NULL);
}

static void writer_begin(uintptr_t handle, const char *key) {
    bool lost = false;
    mutexLock(&s_known_lock);
    int slot = -1;
    for (int i = 0; i < WRITERS_MAX && slot < 0; i++)
        if (!s_writers[i].handle) slot = i;
    if (slot >= 0 && key) {
        s_writers[slot].handle = handle;
        snprintf(s_writers[slot].key, IO_KEY_MAX, "%s", key);
    } else if (slot < 0 && !s_caches_off) {
        // Cannot tell any more which files are changing: stop remembering.
        s_caches_off = lost = true;
    }
    mutexUnlock(&s_known_lock);
    if (lost) {
        l_warn("[io] too many files open for writing; file caches disabled");
        io_invalidate(NULL);
    }
}

// True if `handle` (a descriptor or a FILE *) was open for writing; the
// caller closes it and then calls io_invalidate(key).
static bool writer_end(uintptr_t handle, char *key) {
    bool found = false;
    mutexLock(&s_known_lock);
    for (int i = 0; i < WRITERS_MAX && !found; i++) {
        if (s_writers[i].handle != handle) continue;
        memcpy(key, s_writers[i].key, IO_KEY_MAX);
        s_writers[i].handle = 0;
        found = true;
    }
    mutexUnlock(&s_known_lock);
    return found;
}

static int cached_stat(const char *translated, struct stat *st);

// `key` does not exist. If that is because its directory does not, remember
// the directory: the engine looks for every asset in an override folder
// nobody has before it reads its packs.
static void note_missing(const char *key) {
    char parent[IO_KEY_MAX];
    snprintf(parent, sizeof(parent), "%s", key);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent || slash[-1] == ':') return;
    *slash = 0;

    struct stat st;
    if (cached_stat(parent, &st) != 0) {
        mutexLock(&s_known_lock);
        if (!under_missing_dir(key)) {
            snprintf(s_missing[s_missing_next % MISSING_MAX], IO_KEY_MAX, "%s", parent);
            s_missing_next++;
        }
        mutexUnlock(&s_known_lock);
    }
    errno = ENOENT;
}

// True when `key` is inside a directory known not to exist.
static bool known_missing(const char *key) {
    mutexLock(&s_known_lock);
    bool missing = under_missing_dir(key);
    mutexUnlock(&s_known_lock);
    if (missing) errno = ENOENT;
    return missing;
}

static int cached_stat(const char *translated, struct stat *st) {
    char key[IO_KEY_MAX];
    if (!io_key(translated, key)) return stat(translated, st);

    stat_entry *e = &s_stats[key_hash(key) % STAT_SLOTS];
    mutexLock(&s_known_lock);
    if (under_missing_dir(key)) {
        mutexUnlock(&s_known_lock);
        errno = ENOENT;
        return -1;
    }
    if (strcasecmp(e->key, key) == 0) {
        *st = e->st;
        mutexUnlock(&s_known_lock);
        return 0;
    }
    u32 generation = s_generation;
    mutexUnlock(&s_known_lock);

    int ret = stat(translated, st);
    if (ret == 0) {
        mutexLock(&s_known_lock);
        if (generation == s_generation && !being_written(key)) {
            memcpy(e->key, key, IO_KEY_MAX);
            e->st = *st;
        }
        mutexUnlock(&s_known_lock);
    } else if (errno == ENOENT) {
        note_missing(key);
    }
    return ret;
}

// The game is about to delete or rename `translated` (or just did). A file
// only takes what is known about itself with it; a directory can change the
// answer for anything.
static void io_changing(const char *translated) {
    char key[IO_KEY_MAX];
    struct stat st;
    if (io_key(translated, key) && (cached_stat(translated, &st) != 0 || S_ISREG(st.st_mode)))
        io_invalidate(key);
    else
        io_invalidate(NULL);
}

// A directory was created: the ones remembered as missing may exist now.
static void io_directory_created(void) {
    mutexLock(&s_known_lock);
    s_generation++;
    memset(s_missing, 0, sizeof(s_missing));
    mutexUnlock(&s_known_lock);
}

void io_stream_closing(FILE *f, char *key) {
    if (!writer_end((uintptr_t)f, key)) key[0] = 0;
}

void io_stream_closed(const char *key) {
    if (key[0]) io_invalidate(key);
}

FILE *wrap_fopen(const char *path, const char *mode) {
    char buf[512], key[IO_KEY_MAX];
    const char *translated = translate_path(path, buf, sizeof(buf));
    bool writes = !mode || mode[0] != 'r' || strchr(mode, '+') != NULL;
    bool keyed = io_key(translated, key);
    if (keyed && !writes && known_missing(key)) return NULL;

    PROF_BEGIN();
    if (writes) io_invalidate(keyed ? key : NULL);
    FILE *f = fopen(translated, mode);
    if (f && writes) writer_begin((uintptr_t)f, keyed ? key : NULL);
    if (!f && keyed && !writes && errno == ENOENT) note_missing(key);
    PROF_END_FILE(PROF_OPEN, "fopen", translated, f != NULL);
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
//
// The window starts small and grows while the file is read in order: most
// opens only want a header or one small entry of a pack, and fetching 256 KB
// from the card for each of those costs far more than the calls it saves.
// ============================================================================
#define FDC_MAX_FD   1024
#define FDC_BUF_SIZE (256 * 1024)
#define FDC_MIN_READ (16 * 1024)

typedef struct {
    Mutex lock;
    off_t pos;       // position as the guest sees it
    off_t size;
    off_t buf_off;   // file offset of buf[0]
    size_t buf_len;
    size_t window;   // how much the next refill fetches
    uint8_t *buf;
    char *key;       // io_key of the file, NULL if it cannot be parked
    bool parked;     // closed by the guest, waiting to be opened again
} fd_cache;

static fd_cache *s_fd_cache[FDC_MAX_FD];
static Mutex s_fd_cache_lock;
static u64 s_read_calls, s_read_bytes, s_read_ipc;

static fd_cache *fdc_get(int fd) {
    if (fd < 0 || fd >= FDC_MAX_FD) return NULL;
    return __atomic_load_n(&s_fd_cache[fd], __ATOMIC_ACQUIRE);
}

static void fdc_attach(int fd, const char *key) {
    if (fd < 0 || fd >= FDC_MAX_FD) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) return;

    fd_cache *c = (fd_cache *)calloc(1, sizeof(fd_cache));
    if (!c) return;
    c->buf = (uint8_t *)malloc(FDC_BUF_SIZE);
    if (!c->buf) { free(c); return; }
    c->size = st.st_size;
    c->window = FDC_MIN_READ;
    c->key = key ? strdup(key) : NULL;

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
    free(c->key);
    free(c);
}

// Really closes a parked descriptor.
static void fdc_discard(int fd) {
    fdc_detach(fd, false);
    close(fd);
}

// The guest closed `fd`. Keep it open under its path instead, read-ahead
// buffer included, for the next time the engine opens that file; the oldest
// parked descriptor makes room. False if the caller has to close it.
static bool park_put(int fd, fd_cache *c) {
    int evicted = -1;
    mutexLock(&s_known_lock);
    if (s_caches_off || being_written(c->key)) {
        mutexUnlock(&s_known_lock);
        return false;
    }
    parked_fd *slot = NULL;
    for (int i = 0; i < PARK_MAX; i++) {
        if (!s_parked[i].used) { slot = &s_parked[i]; break; }
        if (!slot || s_parked[i].stamp < slot->stamp) slot = &s_parked[i];
    }
    if (slot->used) evicted = slot->fd;
    slot->used = true;
    slot->fd = fd;
    slot->stamp = ++s_park_clock;
    snprintf(slot->key, IO_KEY_MAX, "%s", c->key);
    c->parked = true;
    mutexUnlock(&s_known_lock);
    if (evicted >= 0) fdc_discard(evicted);
    return true;
}

// A descriptor parked on `key`, rewound, or -1.
static int park_take(const char *key) {
    int fd = -1;
    mutexLock(&s_known_lock);
    for (int i = 0; i < PARK_MAX && fd < 0; i++) {
        if (!s_parked[i].used || strcasecmp(s_parked[i].key, key) != 0) continue;
        s_parked[i].used = false;
        fd = s_parked[i].fd;
        fd_cache *c = s_fd_cache[fd];
        c->parked = false;
        c->pos = 0;
    }
    mutexUnlock(&s_known_lock);
    return fd;
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
            prof_io_bytes(0, (u64)n);
            done += n;
            offset += n;
            continue;
        }

        // Carrying on where the buffer ended: the file is being read in
        // order, so fetch more at once. Anything else starts small again.
        if (c->buf_len && offset == c->buf_off + (off_t)c->buf_len) {
            if (c->window < FDC_BUF_SIZE) c->window *= 4;
        } else {
            c->window = FDC_MIN_READ;
        }
        size_t want = c->window;
        if (want < count - done) want = (count - done + FDC_MIN_READ - 1) & ~(size_t)(FDC_MIN_READ - 1);
        if (want > FDC_BUF_SIZE) want = FDC_BUF_SIZE;

        ssize_t n = read(fd, c->buf, want);
        if (n <= 0) { c->buf_len = 0; break; }
        prof_io_bytes(0, (u64)n);
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

    char key[IO_KEY_MAX];
    if (writer_end((uintptr_t)fd, key)) {
        int ret = close(fd);
        io_invalidate(key);
        return ret;
    }

    fd_cache *c = fdc_get(fd);
    if (c && c->parked) { errno = EBADF; return -1; } // closed twice
    if (c && c->key && park_put(fd, c)) return 0;
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
    if (n > 0) prof_io_bytes((u64)n, 0);
    mutexUnlock(&c->lock);
    return n;
}

ssize_t wrap_pread(int fd, void *buf, size_t count, off_t offset) {
    fd_cache *c = fdc_get(fd);
    if (c) {
        PROF_BEGIN();
        mutexLock(&c->lock);
        ssize_t n = fdc_read_at(fd, c, buf, count, offset);
        mutexUnlock(&c->lock);
        PROF_END(PROF_READ);
        if (n > 0) prof_io_bytes((u64)n, 0);
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
    char key[IO_KEY_MAX];
    bool keyed = io_key(translated, key);
    bool writes = (flags & (3 | 0100 | 01000 | 02000)) != 0; // not plain O_RDONLY
    if (keyed && !writes && known_missing(key)) return -1;

    snprintf(s_inflight, sizeof(s_inflight), "open(%s, flags=0x%x)", translated, flags);
    s_inflight_since = armGetSystemTick();
    PROF_BEGIN();
    int fd = -1;
    if (writes) io_invalidate(keyed ? key : NULL);
    else if (keyed) fd = park_take(key);
    bool reused = fd >= 0;
    if (!reused) {
        fd = open(translated, open_flags_to_host(flags), 0666);
        if (fd >= 0 && writes) writer_begin((uintptr_t)fd, keyed ? key : NULL);
        else if (fd >= 0) fdc_attach(fd, keyed ? key : NULL);
        else if (keyed && !writes && errno == ENOENT) note_missing(key);
    }
    s_inflight_since = 0;
    io_trace("open", path, translated, fd >= 0);
    PROF_END_FILE(PROF_OPEN, reused ? "reopen" : "open", translated, fd >= 0);
    return fd;
}

int wrap_stat(const char *path, void *buf) {
    char b[512];
    const char *translated = translate_path(path, b, sizeof(b));
    struct stat st;
    PROF_BEGIN();
    int ret = cached_stat(translated, &st);
    PROF_END_FILE(PROF_STAT, "stat", translated, ret == 0);
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
    PROF_BEGIN();
    int ret = cached_stat(translated, &st);
    PROF_END_FILE(PROF_STAT, "access", translated, ret == 0);
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
    int ret = mkdir(clean, mode);
    io_directory_created();
    return ret;
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
    io_invalidate_all();
    mutexLock(&s_known_lock);
    s_host_cwd[0] = 0; // relative paths mean something else now
    mutexUnlock(&s_known_lock);
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
    if (is_dlc_pack(translated)) {
        l_info("[io] kept %s (the game asked to delete it)", translated);
        return 0;
    }
    io_changing(translated);
    int ret = remove(translated);
    io_changing(translated);
    io_trace("remove", pathname, translated, ret == 0);
    return ret;
}

int wrap_unlink(const char *pathname) {
    char buf[512];
    const char *translated = translate_path(pathname, buf, sizeof(buf));
    if (is_dlc_pack(translated)) {
        l_info("[io] kept %s (the game asked to delete it)", translated);
        return 0;
    }
    io_changing(translated);
    int ret = unlink(translated);
    io_changing(translated);
    io_trace("unlink", pathname, translated, ret == 0);
    return ret;
}

int wrap_rename(const char *oldpath, const char *newpath) {
    char buf1[512], buf2[512];
    const char *tr_old = translate_path(oldpath, buf1, sizeof(buf1));
    const char *tr_new = translate_path(newpath, buf2, sizeof(buf2));
    // FAT rename does not replace an existing destination.
    io_changing(tr_old);
    io_changing(tr_new);
    remove(tr_new);
    int ret = rename(tr_old, tr_new);
    io_changing(tr_old);
    io_changing(tr_new);
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
    (void)addr;
    bool from_file = !(flags & 0x20) && fd >= 0; // !MAP_ANONYMOUS
    fd_cache *c = from_file ? fdc_get(fd) : NULL;

    // Only what the game promised not to write to (PROT_READ) can be shared
    // between two mappings of the same range.
    bool shareable = c && c->key && prot == 1 && length >= MAP_MIN_SIZE;
    u32 generation = 0;
    if (shareable) {
        void *known = map_take(c->key, offset, length, &generation);
        if (known) return known;
    }

    size_t size = (length + 0xFFF) & ~(size_t)0xFFF;
    void *mem = memalign(0x1000, size);
    if (!mem) {
        map_drop_idle();
        mem = memalign(0x1000, size);
    }
    if (!mem) return (void *)-1;

    size_t done = 0;
    if (from_file) {
        PROF_BEGIN();
        if (c) {
            mutexLock(&c->lock);
            ssize_t n = fdc_read_at(fd, c, mem, length, offset);
            mutexUnlock(&c->lock);
            if (n > 0) done = (size_t)n;
            prof_io_bytes(done, 0);
        } else {
            off_t saved = lseek(fd, 0, SEEK_CUR);
            if (lseek(fd, offset, SEEK_SET) != (off_t)-1) {
                while (done < length) {
                    ssize_t n = read(fd, (char *)mem + done, length - done);
                    if (n <= 0) break;
                    done += n;
                }
            }
            lseek(fd, saved, SEEK_SET);
            prof_io_bytes(done, done);
        }
        PROF_END(PROF_READ);
    }
    memset((char *)mem + done, 0, size - done);

    if (shareable && done == length) map_add(c->key, offset, length, mem, size, generation);
    return mem;
}

int wrap_munmap(void *addr, size_t length) {
    (void)length;
    if (!map_release(addr)) free(addr);
    return 0;
}
