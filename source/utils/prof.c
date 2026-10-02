#include "prof.h"
#include "logger.h"
#include <stdio.h>
#include <string.h>

#if AIRBORNE_PROFILE

// A frame at 60 FPS lasts 17 ms and at the 30 FPS cap 33 ms; anything past
// this is a hitch.
#define SPIKE_NS        45000000ULL
#define SUMMARY_NS      15000000000ULL
#define MAX_SPIKE_LINES 6000

static const char *const s_names[PROF_COUNT] = {
    "swap", "compile", "link", "texture", "decode", "open", "stat", "read", "thread", "gl-wait",
};

// Time of every thread, and the share of it spent by the thread that draws:
// only the second one is time the frame could not have been presented in.
static u64 s_ticks[PROF_COUNT], s_draw_ticks[PROF_COUNT];
static u32 s_calls[PROF_COUNT];
static __thread bool t_draws;

void prof_add(int kind, u64 ticks) {
    __atomic_fetch_add(&s_ticks[kind], ticks, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_calls[kind], 1, __ATOMIC_RELAXED);
    if (t_draws) __atomic_fetch_add(&s_draw_ticks[kind], ticks, __ATOMIC_RELAXED);
}

// ============================================================================
// Which files the engine touches, how often and at what cost.
// ============================================================================
#define FILE_SLOTS 1024
#define FILE_NAME  88
#define FILE_TOP   24

typedef struct {
    char name[FILE_NAME];   // "op tail-of-path"; empty = free slot
    u32 calls, fails, draw_calls;
    u64 ticks;
} file_stat;

static file_stat s_files[FILE_SLOTS];
static u32 s_file_count, s_file_dropped;
static Mutex s_file_lock;

void prof_file(const char *op, const char *path, bool ok, u64 ticks) {
    if (!path) path = "(null)";
    size_t len = strlen(path);
    size_t room = FILE_NAME - strlen(op) - 2;
    char name[FILE_NAME];
    snprintf(name, sizeof(name), "%s %s", op, len > room ? path + (len - room) : path);

    u32 hash = 2166136261u;
    for (const char *p = name; *p; p++) hash = (hash ^ (u8)*p) * 16777619u;

    mutexLock(&s_file_lock);
    file_stat *f = NULL;
    for (u32 i = 0; i < FILE_SLOTS; i++) {
        file_stat *slot = &s_files[(hash + i) % FILE_SLOTS];
        if (!slot->name[0]) {
            if (s_file_count >= FILE_SLOTS * 3 / 4) break;
            memcpy(slot->name, name, sizeof(name));
            s_file_count++;
            f = slot;
            break;
        }
        if (strcmp(slot->name, name) == 0) { f = slot; break; }
    }
    if (f) {
        f->calls++;
        if (!ok) f->fails++;
        if (t_draws) f->draw_calls++;
        f->ticks += ticks;
    } else {
        s_file_dropped++;
    }
    mutexUnlock(&s_file_lock);
}

extern void shadercache_stats(unsigned *loaded, unsigned *built);

static void dump_files(void) {
    mutexLock(&s_file_lock);
    u32 calls = 0, fails = 0;
    for (u32 i = 0; i < FILE_SLOTS; i++) {
        calls += s_files[i].calls;
        fails += s_files[i].fails;
    }
    if (calls)
        l_info("[prof] files: %u calls on %u paths, %u failed, %u untracked; costliest:", calls,
               s_file_count, fails, s_file_dropped);
    for (int n = 0; n < FILE_TOP; n++) {
        file_stat *best = NULL;
        for (u32 i = 0; i < FILE_SLOTS; i++)
            if (s_files[i].calls && (!best || s_files[i].ticks > best->ticks)) best = &s_files[i];
        if (!best) break;
        l_info("[prof]   %lu.%lu ms, %u calls (%u failed, %u on the drawing thread): %s",
               (unsigned long)(armTicksToNs(best->ticks) / 1000000),
               (unsigned long)(armTicksToNs(best->ticks) / 100000 % 10), best->calls, best->fails,
               best->draw_calls, best->name);
        best->calls = 0;
    }
    memset(s_files, 0, sizeof(s_files));
    s_file_count = s_file_dropped = 0;
    mutexUnlock(&s_file_lock);
}

// ============================================================================
// Read-ahead efficiency and texture sizes.
// ============================================================================
static u64 s_io_wanted, s_io_fetched;
static u32 s_tex_count, s_tex_parallel, s_tex_draw;
static u64 s_tex_pixels;

void prof_io_bytes(u64 wanted, u64 fetched) {
    __atomic_fetch_add(&s_io_wanted, wanted, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_io_fetched, fetched, __ATOMIC_RELAXED);
}

void prof_texture_size(int width, int height, bool parallel) {
    __atomic_fetch_add(&s_tex_count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_tex_pixels, (u64)width * height, __ATOMIC_RELAXED);
    if (parallel) __atomic_fetch_add(&s_tex_parallel, 1, __ATOMIC_RELAXED);
    if (t_draws) __atomic_fetch_add(&s_tex_draw, 1, __ATOMIC_RELAXED);
}

// Called by the drawing thread once per presented frame.
void prof_frame(void) {
    static u64 s_last, s_frame, s_sum_ns, s_max_ns, s_summary_at;
    static u32 s_spikes, s_lines, s_summary_frames;

    t_draws = true;
    u64 now = armGetSystemTick();
    u64 ticks[PROF_COUNT], draw_ticks[PROF_COUNT];
    u32 calls[PROF_COUNT];
    for (int i = 0; i < PROF_COUNT; i++) {
        ticks[i] = __atomic_exchange_n(&s_ticks[i], 0, __ATOMIC_RELAXED);
        draw_ticks[i] = __atomic_exchange_n(&s_draw_ticks[i], 0, __ATOMIC_RELAXED);
        calls[i] = __atomic_exchange_n(&s_calls[i], 0, __ATOMIC_RELAXED);
    }
    if (!s_last) { s_last = s_summary_at = now; return; }

    u64 frame_ns = armTicksToNs(now - s_last);
    s_last = now;
    s_frame++;
    s_summary_frames++;
    s_sum_ns += frame_ns;
    if (frame_ns > s_max_ns) s_max_ns = frame_ns;

    if (frame_ns >= SPIKE_NS) {
        s_spikes++;
        if (s_lines < MAX_SPIKE_LINES) {
            s_lines++;
            // "kind total-ms/drawing-thread-ms (calls)"
            char line[480];
            int n = snprintf(line, sizeof(line), "[prof] frame %lu took %lu ms:",
                             (unsigned long)s_frame, (unsigned long)(frame_ns / 1000000));
            for (int i = 0; i < PROF_COUNT && n < (int)sizeof(line); i++) {
                if (!calls[i]) continue;
                n += snprintf(line + n, sizeof(line) - n, " %s %lu/%lu ms (%u)", s_names[i],
                              (unsigned long)(armTicksToNs(ticks[i]) / 1000000),
                              (unsigned long)(armTicksToNs(draw_ticks[i]) / 1000000), calls[i]);
            }
            l_info("%s", line);
        }
    }

    if (armTicksToNs(now - s_summary_at) >= SUMMARY_NS) {
        l_info("[prof] last %u frames: avg %lu.%lu ms, worst %lu ms, %u over %lu ms", s_summary_frames,
               (unsigned long)(s_sum_ns / s_summary_frames / 1000000),
               (unsigned long)(s_sum_ns / s_summary_frames / 100000 % 10),
               (unsigned long)(s_max_ns / 1000000), s_spikes, (unsigned long)(SPIKE_NS / 1000000));
        l_info("[prof] reads: game asked for %lu KB, %lu KB fetched from the card; "
               "ASTC: %u textures, %lu KB of pixels, %u decoded on all cores, %u by the drawing thread",
               (unsigned long)(__atomic_exchange_n(&s_io_wanted, 0, __ATOMIC_RELAXED) / 1024),
               (unsigned long)(__atomic_exchange_n(&s_io_fetched, 0, __ATOMIC_RELAXED) / 1024),
               __atomic_exchange_n(&s_tex_count, 0, __ATOMIC_RELAXED),
               (unsigned long)(__atomic_exchange_n(&s_tex_pixels, 0, __ATOMIC_RELAXED) * 4 / 1024),
               __atomic_exchange_n(&s_tex_parallel, 0, __ATOMIC_RELAXED),
               __atomic_exchange_n(&s_tex_draw, 0, __ATOMIC_RELAXED));
        unsigned loaded = 0, built = 0;
        shadercache_stats(&loaded, &built);
        if (loaded || built)
            l_info("[prof] shader programs: %u from the cache, %u built from source", loaded, built);
        dump_files();
        s_summary_at = now;
        s_summary_frames = 0;
        s_sum_ns = s_max_ns = 0;
        s_spikes = 0;
    }
}

#endif // AIRBORNE_PROFILE
