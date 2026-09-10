/* pthr.c -- bionic<->glibc pthread wrappers (Linux port)
 *
 * Ported from gm666q/lswtcs-vita, adapted to plain Linux/glibc. The bionic
 * pthread struct layouts differ from glibc's, so every game-owned mutex/cond/
 * attr stores a pointer to a real glibc object in its first word (lazily
 * created on first use). The GL single-context ownership handover is serviced
 * at every blocking point so a thread never blocks while holding the context.
 *
 * On glibc the stack-protector cookie lives in the TCB that TPIDR_EL0 already
 * points at, so no fake TLS is installed (the false-canary trips are handled by
 * NOP-ing the __stack_chk_fail branches in main.c). Core affinity is left to
 * the kernel scheduler.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

#include "pthr.h"
#include "util.h"

static __thread int tls_is_render_thread = 0;

int pthr_is_render_thread(void) { return tls_is_render_thread; }

#define BIONIC_PTHREAD_MUTEX_INITIALIZER            0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER  0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER 0x8000

// glibc owns TPIDR_EL0; do NOT install a fake TLS block here.
void pthr_install_fake_tls(void) {}
void pthr_ensure_fake_tls(void) {}
void pthr_pin_bg_core(void) {}
void pthr_set_role_symbols(uintptr_t a, uintptr_t r, uintptr_t n) { (void)a; (void)r; (void)n; }

static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * A 32-bit bionic mutex/cond is only one 4-byte word.  That word is bionic's
 * own state, not spare storage for a glibc pointer.  Keeping the host pointer
 * there appeared to work for most of the engine, but FMOD legitimately changed
 * the word and turned the pointer into an odd address; the following glibc
 * unlock then faulted in ldrex.
 *
 * Keep host objects in side tables keyed by the address of the bionic object.
 * This is the same ABI-safe arrangement used by the proven NFS port, while the
 * KOTOR-specific render-thread handover remains in the public wrappers below.
 */
#define SYNC_BUCKET_COUNT 257

typedef struct MutexEntry {
  pthread_mutex_t_bionic *key;
  pthread_mutex_t real;
  struct MutexEntry *next;
} MutexEntry;

typedef struct CondEntry {
  pthread_cond_t_bionic *key;
  pthread_cond_t real;
  struct CondEntry *next;
} CondEntry;

static MutexEntry *mutex_entries[SYNC_BUCKET_COUNT];
static CondEntry *cond_entries[SYNC_BUCKET_COUNT];
static unsigned sync_trace_lines;

static unsigned sync_bucket(const void *key) {
  return (unsigned)(((uintptr_t)key >> 2) % SYNC_BUCKET_COUNT);
}

static int sync_trace_enabled(void) {
  const char *v = getenv("KOTOR_PTHREAD_TRACE");
  return v && v[0] && strcmp(v, "0") != 0;
}

static void sync_trace(const char *what, const void *key, const void *real,
                       uint32_t guest_state, const void *caller, int result) {
  if (!sync_trace_enabled() ||
      __sync_fetch_and_add(&sync_trace_lines, 1) >= 128)
    return;
  debugPrintf("pthr side-table: %s key=%p real=%p guest=0x%08x caller=%p ret=%d\n",
              what, key, real, guest_state, caller, result);
}

static MutexEntry *mutex_find_locked(pthread_mutex_t_bionic *key,
                                     MutexEntry ***link_out) {
  MutexEntry **link = &mutex_entries[sync_bucket(key)];
  while (*link && (*link)->key != key)
    link = &(*link)->next;
  if (link_out)
    *link_out = link;
  return *link;
}

static CondEntry *cond_find_locked(pthread_cond_t_bionic *key,
                                   CondEntry ***link_out) {
  CondEntry **link = &cond_entries[sync_bucket(key)];
  while (*link && (*link)->key != key)
    link = &(*link)->next;
  if (link_out)
    *link_out = link;
  return *link;
}

static int attr_static_init(pthread_attr_t_bionic *attr) {
  if (attr->magic != 0x42424242) {
    attr->magic = 0x42424242;
    attr->real_ptr = malloc(sizeof(pthread_attr_t));
    return pthread_attr_init(attr->real_ptr);
  }
  return 0;
}

static int mutex_get(pthread_mutex_t_bionic *mutex,
                     const pthread_mutexattr_t *attr, int explicit_init,
                     pthread_mutex_t **real_out, const void *caller) {
  if (!mutex || !real_out)
    return EINVAL;
  const uint32_t guest_state =
      __atomic_load_n(&mutex->state, __ATOMIC_ACQUIRE);
  pthread_mutex_lock(&init_lock);
  MutexEntry *entry = mutex_find_locked(mutex, NULL);
  if (entry) {
    *real_out = &entry->real;
    pthread_mutex_unlock(&init_lock);
    return 0;
  }

  int kind = PTHREAD_MUTEX_NORMAL;
  if (attr) {
    pthread_mutexattr_gettype((pthread_mutexattr_t *)attr, &kind);
  } else if (!explicit_init) {
    switch ((int)guest_state) {
      case BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER:  kind = PTHREAD_MUTEX_RECURSIVE;  break;
      case BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER: kind = PTHREAD_MUTEX_ERRORCHECK; break;
      default:                                          kind = PTHREAD_MUTEX_NORMAL;     break;
    }
  }

  entry = calloc(1, sizeof(*entry));
  if (!entry) {
    pthread_mutex_unlock(&init_lock);
    return ENOMEM;
  }
  pthread_mutexattr_t ma;
  pthread_mutexattr_init(&ma);
  pthread_mutexattr_settype(&ma, kind);
  int ret = pthread_mutex_init(&entry->real, &ma);
  pthread_mutexattr_destroy(&ma);

  if (ret == 0) {
    entry->key = mutex;
    const unsigned bucket = sync_bucket(mutex);
    entry->next = mutex_entries[bucket];
    mutex_entries[bucket] = entry;
    *real_out = &entry->real;
    if (explicit_init)
      __atomic_store_n(&mutex->state, 0, __ATOMIC_RELEASE);
  } else {
    free(entry);
    entry = NULL;
    debugPrintf("pthr: mutex init for %p failed (%d)\n", (void *)mutex, ret);
  }
  pthread_mutex_unlock(&init_lock);
  sync_trace("mutex create", mutex, entry ? (void *)&entry->real : NULL,
             guest_state, caller, ret);
  return ret;
}

static int cond_get(pthread_cond_t_bionic *cond, const pthread_condattr_t *attr,
                    int explicit_init, pthread_cond_t **real_out,
                    const void *caller) {
  if (!cond || !real_out)
    return EINVAL;
  const uint32_t guest_state =
      __atomic_load_n(&cond->state, __ATOMIC_ACQUIRE);
  pthread_mutex_lock(&init_lock);
  CondEntry *entry = cond_find_locked(cond, NULL);
  if (entry) {
    *real_out = &entry->real;
    pthread_mutex_unlock(&init_lock);
    return 0;
  }

  entry = calloc(1, sizeof(*entry));
  if (!entry) {
    pthread_mutex_unlock(&init_lock);
    return ENOMEM;
  }
  int ret = pthread_cond_init(&entry->real, attr);

  if (ret == 0) {
    entry->key = cond;
    const unsigned bucket = sync_bucket(cond);
    entry->next = cond_entries[bucket];
    cond_entries[bucket] = entry;
    *real_out = &entry->real;
    if (explicit_init)
      __atomic_store_n(&cond->state, 0, __ATOMIC_RELEASE);
  } else {
    free(entry);
    entry = NULL;
    debugPrintf("pthr: cond init for %p failed (%d)\n", (void *)cond, ret);
  }
  pthread_mutex_unlock(&init_lock);
  sync_trace("cond create", cond, entry ? (void *)&entry->real : NULL,
             guest_state, caller, ret);
  return ret;
}

// ---------------------------------------------------------------------------
// thread creation
// ---------------------------------------------------------------------------

typedef struct {
  void *(*start)(void *);
  void *arg;
} ThreadStart;

static void *thread_trampoline(void *p) {
  ThreadStart s = *(ThreadStart *)p;
  free(p);
  void *ret = s.start(s.arg);
  // a thread exiting while owning the GL context would orphan it forever
  extern void egl_gl_ownership_release(void);
  egl_gl_ownership_release();
  return ret;
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr,
                            void *(*start)(void *), void *param) {
  ThreadStart *s = malloc(sizeof(*s));
  s->start = start;
  s->arg = param;

  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 2 * 1024 * 1024);
  if (attr) {
    attr_static_init((pthread_attr_t_bionic *)attr);
    size_t want = 0;
    if (attr->real_ptr && pthread_attr_getstacksize(attr->real_ptr, &want) == 0 &&
        want > 2 * 1024 * 1024)
      pthread_attr_setstacksize(&a, want);
  }

  int ret = pthread_create(thread, &a, thread_trampoline, s);
  pthread_attr_destroy(&a);
  if (ret != 0)
    free(s);
  return ret;
}

int pthread_join_soloader(pthread_t thread, void **value_ptr) {
  extern void egl_gl_ownership_park(void);
  egl_gl_ownership_park();
  return pthread_join(thread, value_ptr);
}
int pthread_detach_soloader(pthread_t thread) { return pthread_detach(thread); }
pthread_t pthread_self_soloader(void) { return pthread_self(); }

int pthread_equal_soloader(pthread_t t1, pthread_t t2) {
  if (t1 == t2) return 1;
  if (!t1 || !t2) return 0;
  return pthread_equal(t1, t2);
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy, struct sched_param *param) {
  return pthread_getschedparam(thread, policy, param);
}

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine)
    return -1;
  if (__sync_lock_test_and_set(once_control, 1) == 0)
    (*init_routine)();
  return 0;
}

// ---------------------------------------------------------------------------
// mutex / cond / attr
// ---------------------------------------------------------------------------

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr) { return pthread_mutexattr_init(attr); }
int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type) { return pthread_mutexattr_settype(attr, type); }
int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr) { return pthread_mutexattr_destroy(attr); }

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr) {
  if (!uid) return EINVAL;
  pthread_mutex_t *real = NULL;
  return mutex_get(uid, attr, 1, &real, __builtin_return_address(0));
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return 0;
  const uint32_t guest_state =
      __atomic_load_n(&mutex->state, __ATOMIC_ACQUIRE);
  pthread_mutex_lock(&init_lock);
  MutexEntry **link = NULL;
  MutexEntry *entry = mutex_find_locked(mutex, &link);
  if (!entry) {
    pthread_mutex_unlock(&init_lock);
    return 0;
  }
  int ret = pthread_mutex_destroy(&entry->real);
  if (ret == 0) {
    *link = entry->next;
    __atomic_store_n(&mutex->state, 0, __ATOMIC_RELEASE);
  }
  pthread_mutex_unlock(&init_lock);
  sync_trace("mutex destroy", mutex, &entry->real, guest_state,
             __builtin_return_address(0), ret);
  if (ret == 0)
    free(entry);
  return ret;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return EINVAL;
  pthread_mutex_t *real = NULL;
  int ret = mutex_get(mutex, NULL, 0, &real, __builtin_return_address(0));
  if (ret != 0)
    return ret;
  if (pthread_mutex_trylock(real) == 0)
    return 0;
  extern void egl_gl_ownership_park(void);
  egl_gl_ownership_park();
  extern int egl_gl_thread_holds_context(void);
  if (!tls_is_render_thread || !egl_gl_thread_holds_context())
    return pthread_mutex_lock(real);
  extern void egl_gl_service_handover(void);
  for (;;) {
    if (pthread_mutex_trylock(real) == 0)
      return 0;
    egl_gl_service_handover();
    struct timespec ts = { 0, 100 * 1000 };
    nanosleep(&ts, NULL);
  }
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return EINVAL;
  pthread_mutex_t *real = NULL;
  int ret = mutex_get(mutex, NULL, 0, &real, __builtin_return_address(0));
  return ret == 0 ? pthread_mutex_trylock(real) : ret;
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return EINVAL;
  pthread_mutex_t *real = NULL;
  int ret = mutex_get(mutex, NULL, 0, &real, __builtin_return_address(0));
  return ret == 0 ? pthread_mutex_unlock(real) : ret;
}

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond, const pthread_condattr_t *attr) {
  if (!cond) return EINVAL;
  pthread_cond_t *real = NULL;
  return cond_get(cond, attr, 1, &real, __builtin_return_address(0));
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond) {
  if (!cond) return 0;
  const uint32_t guest_state =
      __atomic_load_n(&cond->state, __ATOMIC_ACQUIRE);
  pthread_mutex_lock(&init_lock);
  CondEntry **link = NULL;
  CondEntry *entry = cond_find_locked(cond, &link);
  if (!entry) {
    pthread_mutex_unlock(&init_lock);
    return 0;
  }
  int ret = pthread_cond_destroy(&entry->real);
  if (ret == 0) {
    *link = entry->next;
    __atomic_store_n(&cond->state, 0, __ATOMIC_RELEASE);
  }
  pthread_mutex_unlock(&init_lock);
  sync_trace("cond destroy", cond, &entry->real, guest_state,
             __builtin_return_address(0), ret);
  if (ret == 0)
    free(entry);
  return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond) {
  if (!cond) return EINVAL;
  pthread_cond_t *real = NULL;
  int ret = cond_get(cond, NULL, 0, &real, __builtin_return_address(0));
  return ret == 0 ? pthread_cond_signal(real) : ret;
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond) {
  if (!cond) return EINVAL;
  pthread_cond_t *real = NULL;
  int ret = cond_get(cond, NULL, 0, &real, __builtin_return_address(0));
  return ret == 0 ? pthread_cond_broadcast(real) : ret;
}

#define RENDER_WAIT_SLICE_NS (4 * 1000 * 1000)

static void timespec_add_ns(struct timespec *ts, long ns) {
  ts->tv_nsec += ns;
  while (ts->tv_nsec >= 1000000000L) {
    ts->tv_sec += 1;
    ts->tv_nsec -= 1000000000L;
  }
}
static int timespec_before(const struct timespec *a, const struct timespec *b) {
  if (a->tv_sec != b->tv_sec)
    return a->tv_sec < b->tv_sec;
  return a->tv_nsec < b->tv_nsec;
}

static struct timespec monotonic_deadline_to_realtime(
    const struct timespec *mono_deadline) {
  struct timespec mono_now;
  struct timespec real_now;
  clock_gettime(CLOCK_MONOTONIC, &mono_now);
  clock_gettime(CLOCK_REALTIME, &real_now);

  if (!timespec_before(&mono_now, mono_deadline))
    return real_now;

  struct timespec delta = {
    .tv_sec = mono_deadline->tv_sec - mono_now.tv_sec,
    .tv_nsec = mono_deadline->tv_nsec - mono_now.tv_nsec,
  };
  if (delta.tv_nsec < 0) {
    delta.tv_sec -= 1;
    delta.tv_nsec += 1000000000L;
  }

  real_now.tv_sec += delta.tv_sec;
  timespec_add_ns(&real_now, delta.tv_nsec);
  return real_now;
}

int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex) {
  if (!cond || !mutex) return EINVAL;
  pthread_cond_t *real_cond = NULL;
  pthread_mutex_t *real_mutex = NULL;
  int ret = cond_get(cond, NULL, 0, &real_cond, __builtin_return_address(0));
  if (ret != 0)
    return ret;
  ret = mutex_get(mutex, NULL, 0, &real_mutex, __builtin_return_address(0));
  if (ret != 0)
    return ret;
  extern void egl_gl_ownership_park(void);
  egl_gl_ownership_park();
  extern int egl_gl_thread_holds_context(void);
  if (!tls_is_render_thread || !egl_gl_thread_holds_context())
    return pthread_cond_wait(real_cond, real_mutex);
  extern void egl_gl_service_handover(void);
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  timespec_add_ns(&ts, RENDER_WAIT_SLICE_NS);
  int r = pthread_cond_timedwait(real_cond, real_mutex, &ts);
  if (r == ETIMEDOUT) {
    egl_gl_service_handover();
    return 0;
  }
  return r;
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex,
                                    struct timespec *abstime) {
  if (!cond || !mutex) return EINVAL;
  pthread_cond_t *real_cond = NULL;
  pthread_mutex_t *real_mutex = NULL;
  int ret = cond_get(cond, NULL, 0, &real_cond, __builtin_return_address(0));
  if (ret != 0)
    return ret;
  ret = mutex_get(mutex, NULL, 0, &real_mutex, __builtin_return_address(0));
  if (ret != 0)
    return ret;
  extern void egl_gl_ownership_park(void);
  egl_gl_ownership_park();
  extern int egl_gl_thread_holds_context(void);
  if (!tls_is_render_thread || !abstime || !egl_gl_thread_holds_context())
    return pthread_cond_timedwait(real_cond, real_mutex, abstime);
  extern void egl_gl_service_handover(void);
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  timespec_add_ns(&ts, RENDER_WAIT_SLICE_NS);
  const int final_slice = !timespec_before(&ts, abstime);
  int r = pthread_cond_timedwait(real_cond, real_mutex,
                                 final_slice ? abstime : &ts);
  if (r == ETIMEDOUT && !final_slice) {
    egl_gl_service_handover();
    return 0;
  }
  return r;
}

int pthread_cond_timedwait_monotonic_soloader(
    pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex,
    const struct timespec *abstime) {
  if (!abstime)
    return EINVAL;
  struct timespec realtime_deadline =
      monotonic_deadline_to_realtime(abstime);
  return pthread_cond_timedwait_soloader(cond, mutex, &realtime_deadline);
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr) {
  if (!attr) return EINVAL;
  return attr_static_init(attr);
}
int pthread_attr_destroy_soloader(pthread_attr_t_bionic *attr) {
  if (!attr || attr->magic != 0x42424242) return 0;
  int ret = pthread_attr_destroy(attr->real_ptr);
  free(attr->real_ptr);
  attr->magic = 0;
  return ret;
}
int pthread_attr_setdetachstate_soloader(pthread_attr_t_bionic *attr, int state) {
  if (!attr) return -1;
  attr_static_init(attr);
  return pthread_attr_setdetachstate(attr->real_ptr, state);
}
int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
  if (!attr) return -1;
  attr_static_init(attr);
  return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}

// render-thread role hint set from main.c after resolve
void pthr_mark_render_thread(void) { tls_is_render_thread = 1; }
