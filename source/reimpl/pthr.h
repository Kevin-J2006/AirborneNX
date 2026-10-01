#ifndef __REIMPL_PTHR_H__
#define __REIMPL_PTHR_H__

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/*
 * Bionic (LP64) pthread ABI implemented on top of libnx primitives.
 *
 * The guest only ever sees opaque storage of the bionic sizes
 * (pthread_mutex_t = 40 bytes, pthread_cond_t = 48, sem_t = 16,
 * pthread_attr_t = 56, pthread_mutexattr_t = 8, pthread_once_t = 4), so the
 * libnx objects are stored directly inside that storage. Bionic's static
 * initializers (all zero, or the type in bits 14-15 of the first word) are
 * valid unlocked states for this layout.
 */

// Installs the bionic TLS block for the main thread. Safe to call repeatedly.
void pthr_init_main(void);

// Must be called on a thread the guest did not create (e.g. the SDL audio
// thread) before it runs guest code, so the guest finds a valid TLS block.
void pthr_enter_host_thread(void);

int *pthr_errno(void);
int pthr_gettid(void);

int pthr_mutex_init(void *mutex, const void *attr);
int pthr_mutex_destroy(void *mutex);
int pthr_mutex_lock(void *mutex);
int pthr_mutex_trylock(void *mutex);
int pthr_mutex_unlock(void *mutex);
int pthr_mutex_timedlock(void *mutex, const struct timespec *abstime);

int pthr_mutexattr_init(void *attr);
int pthr_mutexattr_destroy(void *attr);
int pthr_mutexattr_settype(void *attr, int type);

int pthr_cond_init(void *cond, const void *attr);
int pthr_cond_destroy(void *cond);
int pthr_cond_wait(void *cond, void *mutex);
int pthr_cond_timedwait(void *cond, void *mutex, const struct timespec *abstime);
int pthr_cond_signal(void *cond);
int pthr_cond_broadcast(void *cond);

int pthr_sem_init(void *sem, int pshared, unsigned int value);
int pthr_sem_destroy(void *sem);
int pthr_sem_wait(void *sem);
int pthr_sem_trywait(void *sem);
int pthr_sem_post(void *sem);

int pthr_attr_init(void *attr);
int pthr_attr_destroy(void *attr);
int pthr_attr_setdetachstate(void *attr, int state);
int pthr_attr_getdetachstate(const void *attr, int *state);
int pthr_attr_setstacksize(void *attr, size_t size);

int pthr_create(long *thread, const void *attr, void *(*start)(void *), void *arg);
int pthr_join(long thread, void **retval);
int pthr_detach(long thread);
long pthr_self(void);
int pthr_equal(long a, long b);
void pthr_exit(void *retval);

int pthr_key_create(int *key, void (*destructor)(void *));
int pthr_key_delete(int key);
void *pthr_getspecific(int key);
int pthr_setspecific(int key, const void *value);

int pthr_once(void *once_control, void (*init_routine)(void));

int pthr_cxa_guard_acquire(uint64_t *guard);
void pthr_cxa_guard_release(uint64_t *guard);
void pthr_cxa_guard_abort(uint64_t *guard);

// Logs pc/lr and stack return addresses of every guest thread.
void pthr_dump_threads(uintptr_t text_base, size_t text_size);

#endif // __REIMPL_PTHR_H__
