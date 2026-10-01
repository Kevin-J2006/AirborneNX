#include "pthr.h"
#include "../utils/prof.h"
#include "sys.h"
#include "../utils/logger.h"

#include <switch.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>

// Linux errno values the guest expects from pthread calls
#define L_EBUSY     16
#define L_EINVAL    22
#define L_EAGAIN    11
#define L_ENOMEM    12
#define L_ESRCH     3
#define L_ETIMEDOUT 110

#define GUEST_STACK_GUARD 0xD00DF00DDEADBEEFULL
#define MAX_KEYS 256
#define DEFAULT_STACK_SIZE (2 * 1024 * 1024)
#define MIN_STACK_SIZE     (256 * 1024)
#define GUEST_THREAD_PRIO   59

// ============================================================================
// Per-thread guest state. TPIDR_EL0 points at this (libnx itself uses
// TPIDRRO_EL0, so the two never collide).
// ============================================================================
typedef struct guest_thread {
    uintptr_t tls[64];          // bionic TLS slots; must stay first
    void *key_values[MAX_KEYS];
    int tid;
    int guest_errno;

    Thread nx;
    void *(*start)(void *);
    void *arg;
    void *retval;
    jmp_buf exit_jmp;
    bool detached;
    bool finished;
    bool is_guest_created;
    struct guest_thread *next_dead;
} guest_thread;

static guest_thread s_main_thread;
static bool s_main_inited = false;
static int s_next_tid = 1000;

#define MAX_LIVE_THREADS 128
static guest_thread *s_live[MAX_LIVE_THREADS];
static Mutex s_live_lock;

static Mutex s_dead_lock;
static guest_thread *s_dead_list = NULL;

static void (*s_key_destructors[MAX_KEYS])(void *);
static bool s_key_used[MAX_KEYS];
static Mutex s_key_lock;

static inline guest_thread *current_raw(void) {
    guest_thread *t;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(t));
    return t;
}

static void install_tls(guest_thread *t) {
    t->tls[0] = (uintptr_t)t;                // TLS_SLOT_SELF
    t->tls[1] = (uintptr_t)t;                // TLS_SLOT_THREAD_ID
    t->tls[2] = (uintptr_t)&t->guest_errno;  // TLS_SLOT_ERRNO
    t->tls[5] = GUEST_STACK_GUARD;           // TLS_SLOT_STACK_GUARD (0x28)
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(t));
}

void pthr_init_main(void) {
    if (!s_main_inited) {
        memset(&s_main_thread, 0, sizeof(s_main_thread));
        s_main_thread.tid = __atomic_fetch_add(&s_next_tid, 1, __ATOMIC_SEQ_CST);
        s_main_inited = true;
    }
    install_tls(&s_main_thread);
}

void pthr_enter_host_thread(void) {
    guest_thread *t = current_raw();
    if (t && t->tls[0] == (uintptr_t)t && t->tls[5] == GUEST_STACK_GUARD)
        return;
    t = (guest_thread *)calloc(1, sizeof(guest_thread));
    t->tid = __atomic_fetch_add(&s_next_tid, 1, __ATOMIC_SEQ_CST);
    install_tls(t);
}

static guest_thread *current(void) {
    guest_thread *t = current_raw();
    if (!t || t->tls[0] != (uintptr_t)t) {
        pthr_enter_host_thread();
        t = current_raw();
    }
    return t;
}

int *pthr_errno(void) {
    guest_thread *t = current();
    // Host libc reports failures through newlib's errno; fold any pending one
    // into the guest's (Linux-numbered) errno, then clear it so the next
    // failure is detected again.
    if (errno != 0) {
        t->guest_errno = sys_errno_to_linux(errno);
        errno = 0;
    }
    return &t->guest_errno;
}

int pthr_gettid(void) {
    return current()->tid;
}

// ============================================================================
// Mutexes
//   [0]  u32  bionic type bits (type << 14)
//   [4]  RMutex { Mutex lock; u32 counter; }
// ============================================================================
typedef struct {
    uint32_t type_bits;
    RMutex rm;
} guest_mutex;

#define MUTEX_TYPE(m) (((m)->type_bits >> 14) & 3)
#define MUTEX_RECURSIVE 1

int pthr_mutexattr_init(void *attr) {
    if (attr) *(int64_t *)attr = 0;
    return 0;
}

int pthr_mutexattr_destroy(void *attr) {
    (void)attr;
    return 0;
}

int pthr_mutexattr_settype(void *attr, int type) {
    if (!attr || type < 0 || type > 2) return L_EINVAL;
    *(int64_t *)attr = type;
    return 0;
}

int pthr_mutex_init(void *mutex, const void *attr) {
    if (!mutex) return L_EINVAL;
    guest_mutex *m = (guest_mutex *)mutex;
    memset(m, 0, sizeof(*m));
    if (attr) m->type_bits = ((uint32_t)(*(const int64_t *)attr) & 3) << 14;
    return 0;
}

int pthr_mutex_destroy(void *mutex) {
    (void)mutex;
    return 0;
}

int pthr_mutex_lock(void *mutex) {
    if (!mutex) return L_EINVAL;
    guest_mutex *m = (guest_mutex *)mutex;
    if (MUTEX_TYPE(m) == MUTEX_RECURSIVE)
        rmutexLock(&m->rm);
    else
        mutexLock(&m->rm.lock);
    return 0;
}

int pthr_mutex_trylock(void *mutex) {
    if (!mutex) return L_EINVAL;
    guest_mutex *m = (guest_mutex *)mutex;
    bool ok = (MUTEX_TYPE(m) == MUTEX_RECURSIVE) ? rmutexTryLock(&m->rm)
                                                 : mutexTryLock(&m->rm.lock);
    return ok ? 0 : L_EBUSY;
}

int pthr_mutex_unlock(void *mutex) {
    if (!mutex) return L_EINVAL;
    guest_mutex *m = (guest_mutex *)mutex;
    if (MUTEX_TYPE(m) == MUTEX_RECURSIVE)
        rmutexUnlock(&m->rm);
    else
        mutexUnlock(&m->rm.lock);
    return 0;
}

static int64_t ns_until(const struct timespec *abstime) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (int64_t)(abstime->tv_sec - now.tv_sec) * 1000000000LL +
           (int64_t)(abstime->tv_nsec - now.tv_nsec);
}

int pthr_mutex_timedlock(void *mutex, const struct timespec *abstime) {
    if (!mutex) return L_EINVAL;
    while (pthr_mutex_trylock(mutex) != 0) {
        if (abstime && ns_until(abstime) <= 0) return L_ETIMEDOUT;
        svcSleepThread(1000000ULL);
    }
    return 0;
}

// ============================================================================
// Condition variables: [4] CondVar
// ============================================================================
typedef struct {
    uint32_t bionic_state;
    CondVar cv;
} guest_cond;

int pthr_cond_init(void *cond, const void *attr) {
    (void)attr;
    if (!cond) return L_EINVAL;
    memset(cond, 0, sizeof(guest_cond));
    return 0;
}

int pthr_cond_destroy(void *cond) {
    (void)cond;
    return 0;
}

static int cond_wait_ns(void *cond, void *mutex, u64 timeout_ns) {
    if (!cond || !mutex) return L_EINVAL;
    guest_cond *c = (guest_cond *)cond;
    guest_mutex *m = (guest_mutex *)mutex;
    Result rc;

    if (MUTEX_TYPE(m) == MUTEX_RECURSIVE) {
        // Fully release the recursive lock for the duration of the wait.
        // Ownership lives in the underlying lock word, so only the recursion
        // count has to be parked while other threads use the mutex.
        u32 saved_count = m->rm.counter;
        m->rm.counter = 0;
        rc = condvarWaitTimeout(&c->cv, &m->rm.lock, timeout_ns);
        m->rm.counter = saved_count;
    } else {
        rc = condvarWaitTimeout(&c->cv, &m->rm.lock, timeout_ns);
    }
    return R_FAILED(rc) ? L_ETIMEDOUT : 0;
}

int pthr_cond_wait(void *cond, void *mutex) {
    cond_wait_ns(cond, mutex, UINT64_MAX);
    return 0;
}

int pthr_cond_timedwait(void *cond, void *mutex, const struct timespec *abstime) {
    if (!abstime) return L_EINVAL;
    int64_t ns = ns_until(abstime);
    if (ns <= 0) return L_ETIMEDOUT;
    return cond_wait_ns(cond, mutex, (u64)ns);
}

int pthr_cond_signal(void *cond) {
    if (!cond) return L_EINVAL;
    condvarWakeOne(&((guest_cond *)cond)->cv);
    return 0;
}

int pthr_cond_broadcast(void *cond) {
    if (!cond) return L_EINVAL;
    condvarWakeAll(&((guest_cond *)cond)->cv);
    return 0;
}

// ============================================================================
// Semaphores: bionic sem_t is 16 bytes on LP64, same as libnx Semaphore.
// ============================================================================
_Static_assert(sizeof(Semaphore) <= 16, "Semaphore must fit in bionic sem_t");

int pthr_sem_init(void *sem, int pshared, unsigned int value) {
    (void)pshared;
    semaphoreInit((Semaphore *)sem, value);
    return 0;
}

int pthr_sem_destroy(void *sem) {
    (void)sem;
    return 0;
}

int pthr_sem_wait(void *sem) {
    semaphoreWait((Semaphore *)sem);
    return 0;
}

int pthr_sem_trywait(void *sem) {
    if (semaphoreTryWait((Semaphore *)sem)) return 0;
    current()->guest_errno = L_EAGAIN;
    return -1;
}

int pthr_sem_post(void *sem) {
    semaphoreSignal((Semaphore *)sem);
    return 0;
}

// ============================================================================
// Thread attributes: [0] u32 detached, [8] u64 stack size
// ============================================================================
typedef struct {
    uint32_t detached;
    uint32_t pad;
    uint64_t stack_size;
} guest_attr;

int pthr_attr_init(void *attr) {
    if (!attr) return L_EINVAL;
    memset(attr, 0, sizeof(guest_attr));
    return 0;
}

int pthr_attr_destroy(void *attr) {
    (void)attr;
    return 0;
}

int pthr_attr_setdetachstate(void *attr, int state) {
    if (!attr) return L_EINVAL;
    ((guest_attr *)attr)->detached = (state == 1); // PTHREAD_CREATE_DETACHED
    return 0;
}

int pthr_attr_getdetachstate(const void *attr, int *state) {
    if (!attr || !state) return L_EINVAL;
    *state = ((const guest_attr *)attr)->detached ? 1 : 0;
    return 0;
}

int pthr_attr_setstacksize(void *attr, size_t size) {
    if (!attr) return L_EINVAL;
    ((guest_attr *)attr)->stack_size = size;
    return 0;
}

// ============================================================================
// Threads
// ============================================================================
static void run_key_destructors(guest_thread *t) {
    for (int pass = 0; pass < 4; pass++) {
        bool again = false;
        for (int i = 0; i < MAX_KEYS; i++) {
            void *value = t->key_values[i];
            void (*dtor)(void *) = s_key_destructors[i];
            if (value && dtor && s_key_used[i]) {
                t->key_values[i] = NULL;
                dtor(value);
                again = true;
            }
        }
        if (!again) break;
    }
}

static void reap_dead_threads(void) {
    mutexLock(&s_dead_lock);
    guest_thread *list = s_dead_list;
    s_dead_list = NULL;
    mutexUnlock(&s_dead_lock);

    while (list) {
        guest_thread *next = list->next_dead;
        threadWaitForExit(&list->nx);
        threadClose(&list->nx);
        free(list);
        list = next;
    }
}

static void thread_entry(void *arg) {
    guest_thread *t = (guest_thread *)arg;
    install_tls(t);

    mutexLock(&s_live_lock);
    for (int i = 0; i < MAX_LIVE_THREADS; i++) {
        if (!s_live[i]) { s_live[i] = t; break; }
    }
    mutexUnlock(&s_live_lock);
    l_debug("[pthread] tid=%d running on core %u", t->tid, svcGetCurrentProcessorNumber());

    if (setjmp(t->exit_jmp) == 0)
        t->retval = t->start(t->arg);

    l_debug("[pthread] tid=%d exiting", t->tid);
    run_key_destructors(t);

    mutexLock(&s_live_lock);
    for (int i = 0; i < MAX_LIVE_THREADS; i++) {
        if (s_live[i] == t) { s_live[i] = NULL; break; }
    }
    mutexUnlock(&s_live_lock);

    mutexLock(&s_dead_lock);
    t->finished = true;
    if (t->detached) {
        t->next_dead = s_dead_list;
        s_dead_list = t;
    }
    mutexUnlock(&s_dead_lock);
}

// Cores this process may run on (0 if the kernel would not say).
static u32 process_core_mask(void) {
    static u64 s_mask = 0;
    if (!s_mask && R_FAILED(svcGetInfo(&s_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
        s_mask = 0;
    return (u32)s_mask;
}

static int pick_core(void) {
    static int s_next = 0;
    u32 mask = process_core_mask();
    if (!mask) return -2;
    for (int tries = 0; tries < 8; tries++) {
        int core = __atomic_fetch_add(&s_next, 1, __ATOMIC_SEQ_CST) & 3;
        if (mask & (1U << core)) return core;
    }
    return -2;
}

int pthr_create(long *thread, const void *attr, void *(*start)(void *), void *arg) {
    reap_dead_threads();

    guest_thread *t = (guest_thread *)calloc(1, sizeof(guest_thread));
    if (!t) return L_ENOMEM;

    const guest_attr *a = (const guest_attr *)attr;
    size_t stack = (a && a->stack_size) ? a->stack_size : DEFAULT_STACK_SIZE;
    if (stack < MIN_STACK_SIZE) stack = MIN_STACK_SIZE;
    stack = (stack + 0xFFF) & ~(size_t)0xFFF;

    t->start = start;
    t->arg = arg;
    t->detached = a && a->detached;
    t->is_guest_created = true;
    t->tid = __atomic_fetch_add(&s_next_tid, 1, __ATOMIC_SEQ_CST);

    // Horizon only time-slices threads at priority 59; at any other level a
    // busy guest thread would starve every equal-priority thread on its core
    // (including the host main thread). Run all guest threads there.
    PROF_BEGIN();
    int core = pick_core();
    Result rc = threadCreate(&t->nx, thread_entry, t, NULL, stack, GUEST_THREAD_PRIO, core);
    if (R_FAILED(rc)) {
        core = -2;
        rc = threadCreate(&t->nx, thread_entry, t, NULL, stack, GUEST_THREAD_PRIO, -2);
    }
    // The core above is only where the thread starts. A thread created this
    // way may run on that core alone, and the engine makes dozens of them:
    // whichever ones share the drawing thread's core take turns with it while
    // other cores sit idle. Allow every core so the kernel can move a thread
    // to an idle one.
    if (R_SUCCEEDED(rc) && core >= 0)
        svcSetThreadCoreMask(t->nx.handle, core, process_core_mask());
    PROF_END(PROF_THREAD);
    if (R_FAILED(rc)) {
        s32 prio = 0x2C;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        rc = threadCreate(&t->nx, thread_entry, t, NULL, stack, prio, -2);
    }
    if (R_FAILED(rc)) {
        l_error("[pthread] threadCreate failed: 0x%x (stack=0x%zx)", rc, stack);
        free(t);
        return L_EAGAIN;
    }

    if (thread) *thread = (long)t;
    l_debug("[pthread] create tid=%d entry=%p stack=0x%zx detached=%d", t->tid, (void *)start, stack, t->detached);

    rc = threadStart(&t->nx);
    if (R_FAILED(rc)) {
        l_error("[pthread] threadStart failed: 0x%x", rc);
        threadClose(&t->nx);
        free(t);
        return L_EAGAIN;
    }
    return 0;
}

int pthr_join(long thread, void **retval) {
    guest_thread *t = (guest_thread *)thread;
    if (!t || !t->is_guest_created) return L_ESRCH;
    threadWaitForExit(&t->nx);
    if (retval) *retval = t->retval;
    threadClose(&t->nx);
    free(t);
    return 0;
}

int pthr_detach(long thread) {
    guest_thread *t = (guest_thread *)thread;
    if (!t || !t->is_guest_created) return L_ESRCH;

    mutexLock(&s_dead_lock);
    if (!t->detached) {
        t->detached = true;
        if (t->finished) {
            t->next_dead = s_dead_list;
            s_dead_list = t;
        }
    }
    mutexUnlock(&s_dead_lock);
    return 0;
}

long pthr_self(void) {
    return (long)current();
}

int pthr_equal(long a, long b) {
    return a == b;
}

void pthr_exit(void *retval) {
    guest_thread *t = current();
    if (!t->is_guest_created) {
        l_warn("[pthread] pthread_exit on a host thread; sleeping forever");
        for (;;) svcSleepThread(1000000000ULL);
    }
    t->retval = retval;
    longjmp(t->exit_jmp, 1);
}

// ============================================================================
// Thread-specific keys
// ============================================================================
int pthr_key_create(int *key, void (*destructor)(void *)) {
    if (!key) return L_EINVAL;
    mutexLock(&s_key_lock);
    for (int i = 1; i < MAX_KEYS; i++) {
        if (!s_key_used[i]) {
            s_key_used[i] = true;
            s_key_destructors[i] = destructor;
            mutexUnlock(&s_key_lock);
            *key = i;
            return 0;
        }
    }
    mutexUnlock(&s_key_lock);
    l_error("[pthread] pthread_key_create: out of keys");
    return L_EAGAIN;
}

int pthr_key_delete(int key) {
    if (key <= 0 || key >= MAX_KEYS) return L_EINVAL;
    mutexLock(&s_key_lock);
    s_key_used[key] = false;
    s_key_destructors[key] = NULL;
    mutexUnlock(&s_key_lock);
    return 0;
}

void *pthr_getspecific(int key) {
    if (key <= 0 || key >= MAX_KEYS) return NULL;
    return current()->key_values[key];
}

int pthr_setspecific(int key, const void *value) {
    if (key <= 0 || key >= MAX_KEYS) return L_EINVAL;
    current()->key_values[key] = (void *)value;
    return 0;
}

// ============================================================================
// pthread_once: 0 = not run, 1 = running, 2 = done
// ============================================================================
int pthr_once(void *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine) return L_EINVAL;
    int *state = (int *)once_control;

    for (;;) {
        int cur = __atomic_load_n(state, __ATOMIC_ACQUIRE);
        if (cur == 2) return 0;
        if (cur == 0) {
            int expected = 0;
            if (__atomic_compare_exchange_n(state, &expected, 1, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                init_routine();
                __atomic_store_n(state, 2, __ATOMIC_RELEASE);
                return 0;
            }
            continue;
        }
        svcSleepThread(100000ULL);
    }
}

// ============================================================================
// C++ static-local guards. Byte 0 of the guard is the "initialized" flag the
// compiler checks inline; one global recursive lock serializes initializers.
// ============================================================================
static RMutex s_guard_lock;

int pthr_cxa_guard_acquire(uint64_t *guard) {
    uint8_t *done = (uint8_t *)guard;
    if (__atomic_load_n(done, __ATOMIC_ACQUIRE)) return 0;

    rmutexLock(&s_guard_lock);
    if (__atomic_load_n(done, __ATOMIC_ACQUIRE)) {
        rmutexUnlock(&s_guard_lock);
        return 0;
    }
    return 1;
}

void pthr_cxa_guard_release(uint64_t *guard) {
    __atomic_store_n((uint8_t *)guard, 1, __ATOMIC_RELEASE);
    rmutexUnlock(&s_guard_lock);
}

void pthr_cxa_guard_abort(uint64_t *guard) {
    (void)guard;
    rmutexUnlock(&s_guard_lock);
}

// ============================================================================
// Diagnostics: where is every guest thread right now?
// ============================================================================
void pthr_dump_threads(uintptr_t text_base, size_t text_size) {
    mutexLock(&s_live_lock);
    for (int i = 0; i < MAX_LIVE_THREADS; i++) {
        guest_thread *t = s_live[i];
        if (!t) continue;

        ThreadContext ctx;
        if (R_FAILED(threadPause(&t->nx))) continue;
        Result rc = threadDumpContext(&ctx, &t->nx);
        if (R_SUCCEEDED(rc)) {
            char line[512];
            int n = snprintf(line, sizeof(line), "tid=%d entry=+0x%lx pc=%lx lr=%lx stack:", t->tid,
                             (unsigned long)((uintptr_t)t->start - text_base),
                             (unsigned long)ctx.pc.x, (unsigned long)ctx.lr);
            // Without frame pointers, scan the stack for return addresses
            // that land inside the game's text segment.
            const uintptr_t *sp = (const uintptr_t *)ctx.sp;
            const uintptr_t *stack_end = (const uintptr_t *)((uintptr_t)t->nx.stack_mirror + t->nx.stack_sz);
            int found = 0;
            for (int k = 0; k < 600 && found < 14 && sp + k < stack_end; k++) {
                uintptr_t v = sp[k];
                if (v >= text_base && v < text_base + text_size && (v & 3) == 0) {
                    n += snprintf(line + n, sizeof(line) - n, " +0x%lx", (unsigned long)(v - text_base));
                    found++;
                }
            }
            l_info("[threads] %s", line);
        }
        threadResume(&t->nx);
    }
    mutexUnlock(&s_live_lock);
}
