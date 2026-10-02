#include "astc.h"
#include "egl.h"
#include "../utils/logger.h"
#include "../utils/prof.h"
#include <switch.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

// Mesa internals (libEGL.a carries all of Mesa). mesa_format is an enum.
extern unsigned _mesa_glenum_to_compressed_format(GLenum format);
extern void _mesa_unpack_astc_2d_ldr(uint8_t *dst_row, int dst_stride, const uint8_t *src_row, int src_stride,
                                     unsigned src_width, unsigned src_height, unsigned format);

extern void gl_lock_pause(void);
extern void gl_lock_resume(void);

// GL_COMPRESSED_RGBA_ASTC_4x4_KHR .. 12x12_KHR, in enum order.
static const struct { uint8_t w, h; } s_blocks[14] = {
    { 4, 4 }, { 5, 4 }, { 5, 5 }, { 6, 5 }, { 6, 6 }, { 8, 5 }, { 8, 6 },
    { 8, 8 }, { 10, 5 }, { 10, 6 }, { 10, 8 }, { 10, 10 }, { 12, 10 }, { 12, 12 },
};

// ============================================================================
// Decoding on every core.
//
// A texture the drawing thread uploads itself freezes the picture for as long
// as its decode lasts (a 2048x2048 menu background is two seconds on one
// core), and that thread can do nothing else meanwhile, so the idle cores
// help: rows of blocks are independent, and each thread takes the next band
// of rows until none are left.
//
// Uploads from the engine's loader thread stay on that one thread. Nothing
// waits for them, and taking every core for them would slow down the frames
// being drawn at the same time.
// ============================================================================
#define ASTC_WORKERS_MAX  3
#define ASTC_WORKER_STACK (128 * 1024)
#define ASTC_WORKER_PRIO  59            // the level guest threads run at
#define ASTC_MIN_PIXELS   (64 * 64)     // below this the hand-off costs more than it saves

typedef struct {
    uint8_t *dst;
    const uint8_t *src;
    unsigned width, height, block_h, format;
    size_t blocks_x, blocks_y;
    size_t band_rows;       // block rows per band
    size_t band_count;
    size_t next_band;       // atomic
} astc_job;

static Thread s_workers[ASTC_WORKERS_MAX];
static int s_worker_count = -1;         // -1: not started yet
static Mutex s_pool_lock;               // one job at a time
static Semaphore s_wake, s_done;
static astc_job s_job;

static void decode_bands(astc_job *job) {
    for (;;) {
        size_t band = __atomic_fetch_add(&job->next_band, 1, __ATOMIC_RELAXED);
        if (band >= job->band_count) return;

        size_t row = band * job->band_rows;
        unsigned y = (unsigned)(row * job->block_h);
        unsigned h = (unsigned)(job->band_rows * job->block_h);
        if (h > job->height - y) h = job->height - y;
        _mesa_unpack_astc_2d_ldr(job->dst + (size_t)y * job->width * 4, (int)(job->width * 4),
                                 job->src + row * job->blocks_x * 16, (int)(job->blocks_x * 16),
                                 job->width, h, job->format);
    }
}

static void worker_main(void *arg) {
    (void)arg;
    for (;;) {
        semaphoreWait(&s_wake);
        decode_bands(&s_job);
        semaphoreSignal(&s_done);
    }
}

static void start_workers(void) {
    u64 mask = 0;
    if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) mask = 0;
    int wanted = __builtin_popcountll(mask) - 1;
    if (wanted > ASTC_WORKERS_MAX) wanted = ASTC_WORKERS_MAX;

    semaphoreInit(&s_wake, 0);
    semaphoreInit(&s_done, 0);
    s_worker_count = 0;
    for (int i = 0; i < wanted; i++) {
        Thread *t = &s_workers[s_worker_count];
        if (R_FAILED(threadCreate(t, worker_main, NULL, NULL, ASTC_WORKER_STACK, ASTC_WORKER_PRIO, -2)))
            break;
        // Like the guest threads (see pthr_create): any core, not just the
        // one the thread happened to be created on.
        svcSetThreadCoreMask(t->handle, -1, (u32)mask);
        if (R_FAILED(threadStart(t))) {
            threadClose(t);
            break;
        }
        s_worker_count++;
    }
    l_info("[astc] decoding on %d thread(s)", s_worker_count + 1);
}

// Decodes job->src into job->dst; true if the other cores helped.
static bool decode(astc_job *job, bool all_cores) {
    job->band_rows = job->blocks_y;
    job->band_count = 1;
    job->next_band = 0;

    bool shared = false;
    if (all_cores && (size_t)job->width * job->height >= ASTC_MIN_PIXELS && mutexTryLock(&s_pool_lock)) {
        if (s_worker_count < 0) start_workers();
        size_t threads = (size_t)s_worker_count + 1;
        // A few bands per thread, so one that loses its core does not hold
        // the others up.
        if (threads > 1 && job->blocks_y >= threads * 2) {
            job->band_rows = job->blocks_y / (threads * 4);
            if (job->band_rows < 1) job->band_rows = 1;
            job->band_count = (job->blocks_y + job->band_rows - 1) / job->band_rows;

            s_job = *job;
            for (int i = 0; i < s_worker_count; i++) semaphoreSignal(&s_wake);
            decode_bands(&s_job);
            for (int i = 0; i < s_worker_count; i++) semaphoreWait(&s_done);
            shared = true;
        }
        mutexUnlock(&s_pool_lock);
    }
    if (!shared) decode_bands(job);
    return shared;
}

// The Switch GPU driver has no ASTC support: Mesa accepts the format and
// decompresses every upload to RGBA8 on the CPU, inside the GL call. For a
// 1024x1024 texture that is about half a second during which the caller
// holds the GL lock, so a texture streamed in by the loader thread froze the
// thread that draws for as long. Run the same decoder here with the lock
// released and hand the driver plain RGBA8, which it only has to copy.
void astc_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                 GLsizei height, GLint border, GLsizei imageSize, const void *data) {
    unsigned index = internalformat - 0x93B0; // GL_COMPRESSED_RGBA_ASTC_4x4_KHR
    uint8_t *pixels = NULL;
    if (index < 14 && data && width > 0 && height > 0 && border == 0) {
        unsigned bw = s_blocks[index].w, bh = s_blocks[index].h;
        size_t blocks_x = (width + bw - 1) / bw, blocks_y = (height + bh - 1) / bh;
        if ((size_t)imageSize == blocks_x * blocks_y * 16)
            pixels = (uint8_t *)malloc((size_t)width * height * 4);

        if (pixels) {
            astc_job job = {
                .dst = pixels, .src = (const uint8_t *)data,
                .width = (unsigned)width, .height = (unsigned)height, .block_h = bh,
                .format = _mesa_glenum_to_compressed_format(internalformat),
                .blocks_x = blocks_x, .blocks_y = blocks_y,
            };
            bool draws = egl_on_drawing_thread();
            gl_lock_pause();
            PROF_BEGIN();
            bool shared = decode(&job, draws);
            PROF_END(PROF_DECODE);
            prof_texture_size(width, height, shared);
            (void)shared;
            gl_lock_resume();
        }
    }

    if (!pixels) {
        glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
        return;
    }

    // Rows are width * 4 bytes, so any unpack alignment the engine set fits.
    glTexImage2D(target, level, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    free(pixels);
}
