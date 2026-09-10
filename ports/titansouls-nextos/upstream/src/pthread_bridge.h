#ifndef ASM2_PTHREAD_BRIDGE_H
#define ASM2_PTHREAD_BRIDGE_H

/*
 * pthread/sem ABI bridge for the ARM32 Android guest.
 *
 * Old 32-bit bionic represents mutexes, condition variables, semaphores and
 * several attribute objects in storage that is much smaller than the
 * corresponding glibc type.  These entry points must therefore be used in
 * place of the host symbols; passing a guest object to glibc directly is not
 * valid.
 */

#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct asm2_pthread_bridge_stats {
    uint64_t created;
    uint64_t active;
    uint64_t retired;
    uint64_t mutex_handoffs;
    uint32_t longest_active_bucket;
};

struct asm2_pthread_mutex_handoff {
    void *bridge_object;
    int active;
};

void asm2_pthread_bridge_get_stats(struct asm2_pthread_bridge_stats *stats);

/* Temporarily releases a guest mutex only when the caller is its tracked
 * owner at recursion depth one. Returns 1 when handed off, 0 when the caller
 * does not own it, or a negative errno value on failure. */
int asm2_pthread_mutex_handoff_begin(
    void *guest_mutex, struct asm2_pthread_mutex_handoff *handoff);
int asm2_pthread_mutex_handoff_end(
    struct asm2_pthread_mutex_handoff *handoff);

int asm2_pthread_attr_init(void *guest_attr);
int asm2_pthread_attr_destroy(void *guest_attr);
int asm2_pthread_attr_getdetachstate(const void *guest_attr, int *detachstate);
int asm2_pthread_attr_setdetachstate(void *guest_attr, int detachstate);
int asm2_pthread_attr_getstacksize(const void *guest_attr, size_t *stacksize);
int asm2_pthread_attr_setstacksize(void *guest_attr, size_t stacksize);

int asm2_pthread_mutexattr_init(void *guest_attr);
int asm2_pthread_mutexattr_destroy(void *guest_attr);
int asm2_pthread_mutexattr_gettype(const void *guest_attr, int *type);
int asm2_pthread_mutexattr_settype(void *guest_attr, int type);

int asm2_pthread_mutex_init(void *guest_mutex, const void *guest_attr);
int asm2_pthread_mutex_destroy(void *guest_mutex);
int asm2_pthread_mutex_lock(void *guest_mutex);
int asm2_pthread_mutex_trylock(void *guest_mutex);
int asm2_pthread_mutex_unlock(void *guest_mutex);

int asm2_pthread_cond_init(void *guest_cond, const void *guest_attr);
int asm2_pthread_cond_destroy(void *guest_cond);
int asm2_pthread_cond_wait(void *guest_cond, void *guest_mutex);
int asm2_pthread_cond_timedwait(void *guest_cond, void *guest_mutex,
                                const struct timespec *abstime);
int asm2_pthread_cond_signal(void *guest_cond);
int asm2_pthread_cond_broadcast(void *guest_cond);

int asm2_pthread_create(uint32_t *guest_thread, const void *guest_attr,
                        void *(*start_routine)(void *), void *arg);
int asm2_pthread_detach(uint32_t guest_thread);
int asm2_pthread_equal(uint32_t first, uint32_t second);
int asm2_pthread_getschedparam(uint32_t guest_thread, int *policy,
                              struct sched_param *param);
int asm2_pthread_setschedparam(uint32_t guest_thread, int policy,
                              const struct sched_param *param);
void *asm2_pthread_getspecific(uint32_t guest_key);
int asm2_pthread_setspecific(uint32_t guest_key, const void *value);
int asm2_pthread_join(uint32_t guest_thread, void **retval);
int asm2_pthread_key_create(uint32_t *guest_key,
                            void (*destructor)(void *));
int asm2_pthread_key_delete(uint32_t guest_key);
uint32_t asm2_pthread_self(void);
int asm2_pthread_once(volatile uint32_t *guest_once,
                      void (*init_routine)(void));

int asm2_sem_init(void *guest_sem, int pshared, unsigned int value);
int asm2_sem_destroy(void *guest_sem);
int asm2_sem_post(void *guest_sem);
int asm2_sem_trywait(void *guest_sem);
int asm2_sem_wait(void *guest_sem);
int asm2_sem_getvalue(void *guest_sem, int *value);
int asm2_sem_timedwait(void *guest_sem, const void *guest_abstime);

#ifdef __cplusplus
}
#endif

#endif
