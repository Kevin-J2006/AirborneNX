#ifndef __PROF_H__
#define __PROF_H__

#include <switch.h>
#include <stdbool.h>

// Frame-spike profiler (make PROFILE=1). Costs a few tick reads per frame and
// only writes to the log when a frame takes too long, saying where the time
// of that frame went, so a stutter seen on the console can be explained from
// the log afterwards.
#ifndef AIRBORNE_PROFILE
#define AIRBORNE_PROFILE 0
#endif

enum {
    PROF_SWAP,      // inside eglSwapBuffers (includes waiting for vsync)
    PROF_COMPILE,   // glCompileShader
    PROF_LINK,      // glLinkProgram
    PROF_TEXTURE,   // texture uploads
    PROF_DECODE,    // ASTC decompression, outside the GL lock
    PROF_OPEN,      // opening files
    PROF_STAT,      // stat / access
    PROF_READ,      // reading files
    PROF_THREAD,    // creating guest threads
    PROF_GLWAIT,    // waiting for the GL lock (all threads)
    PROF_COUNT
};

#if AIRBORNE_PROFILE
void prof_add(int kind, u64 ticks);
void prof_frame(void);
// Per-path totals of the file calls, listed in the periodic summary.
void prof_file(const char *op, const char *path, bool ok, u64 ticks);
// Bytes the guest asked for / bytes actually fetched from the SD card.
void prof_io_bytes(u64 wanted, u64 fetched);
void prof_texture_size(int width, int height, bool parallel);
#define PROF_BEGIN()   u64 prof_t0 = armGetSystemTick()
#define PROF_END(kind) prof_add(kind, armGetSystemTick() - prof_t0)
#define PROF_END_FILE(kind, op, path, ok) do { \
        u64 prof_dt = armGetSystemTick() - prof_t0; \
        prof_add(kind, prof_dt); \
        prof_file(op, path, ok, prof_dt); \
    } while (0)
#else
#define prof_frame()   ((void)0)
#define prof_io_bytes(wanted, fetched) ((void)0)
#define prof_texture_size(width, height, parallel) ((void)0)
#define PROF_BEGIN()   ((void)0)
#define PROF_END(kind) ((void)0)
#define PROF_END_FILE(kind, op, path, ok) ((void)0)
#endif

#endif // __PROF_H__
