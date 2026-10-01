#include "prof.h"
#include "logger.h"

#if AIRBORNE_PROFILE

// A frame at the game's 30 FPS cap lasts 33 ms; anything past this is a hitch.
#define SPIKE_NS        45000000ULL
#define SUMMARY_FRAMES  900
#define MAX_SPIKE_LINES 1500

static const char *const s_names[PROF_COUNT] = {
    "swap", "compile", "link", "texture", "decode", "open", "read", "thread", "gl-wait",
};

static u64 s_ticks[PROF_COUNT];
static u32 s_calls[PROF_COUNT];

void prof_add(int kind, u64 ticks) {
    __atomic_fetch_add(&s_ticks[kind], ticks, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_calls[kind], 1, __ATOMIC_RELAXED);
}

// Called by the drawing thread once per presented frame.
void prof_frame(void) {
    static u64 s_last, s_frame, s_sum_ns, s_max_ns;
    static u32 s_spikes, s_lines;

    u64 now = armGetSystemTick();
    u64 ticks[PROF_COUNT];
    u32 calls[PROF_COUNT];
    for (int i = 0; i < PROF_COUNT; i++) {
        ticks[i] = __atomic_exchange_n(&s_ticks[i], 0, __ATOMIC_RELAXED);
        calls[i] = __atomic_exchange_n(&s_calls[i], 0, __ATOMIC_RELAXED);
    }
    if (!s_last) { s_last = now; return; }

    u64 frame_ns = armTicksToNs(now - s_last);
    s_last = now;
    s_frame++;
    s_sum_ns += frame_ns;
    if (frame_ns > s_max_ns) s_max_ns = frame_ns;

    if (frame_ns >= SPIKE_NS) {
        s_spikes++;
        if (s_lines < MAX_SPIKE_LINES) {
            s_lines++;
            char line[400];
            int n = snprintf(line, sizeof(line), "[prof] frame %lu took %lu ms:",
                             (unsigned long)s_frame, (unsigned long)(frame_ns / 1000000));
            for (int i = 0; i < PROF_COUNT && n < (int)sizeof(line); i++) {
                if (!calls[i]) continue;
                n += snprintf(line + n, sizeof(line) - n, " %s %lu ms (%u)", s_names[i],
                              (unsigned long)(armTicksToNs(ticks[i]) / 1000000), calls[i]);
            }
            l_info("%s", line);
        }
    }

    if ((s_frame % SUMMARY_FRAMES) == 0) {
        l_info("[prof] last %d frames: avg %lu.%lu ms, worst %lu ms, %u over %lu ms", SUMMARY_FRAMES,
               (unsigned long)(s_sum_ns / SUMMARY_FRAMES / 1000000),
               (unsigned long)(s_sum_ns / SUMMARY_FRAMES / 100000 % 10),
               (unsigned long)(s_max_ns / 1000000), s_spikes, (unsigned long)(SPIKE_NS / 1000000));
        s_sum_ns = s_max_ns = 0;
        s_spikes = 0;
    }
}

#endif // AIRBORNE_PROFILE
