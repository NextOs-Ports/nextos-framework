/*
 * pthread_bridge.c -- arm64 bionic pthread objects on top of glibc.
 *
 * Adapted for Terraria on 2026-08-02 from the 100%-working Prizefighters 2
 * multi-firmware loader.  This modified version is distributed under GPL-3.0.
 *
 * Android and glibc use different object sizes/layouts (notably mutex, cond,
 * semaphore and pthread_attr_t).  Guest storage is therefore a handle for a
 * real host object.  Passing guest objects straight to glibc corrupts adjacent
 * Unity state and passing a guest pthread_attr_t can silently misconfigure or
 * fail worker creation.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define TERRARIA_BRIDGE_MAGIC 0x54425247u /* "TBRG" */

/* Bionic storage as used here: original word, magic, host pointer. */
typedef struct {
  uint32_t word0;
  uint32_t magic;
  void *real;
} ter_bhandle;

static pthread_mutex_t bridge_lock = PTHREAD_MUTEX_INITIALIZER;

static int bridge_sync_trace(void) {
  static int enabled = -1;
  if (enabled < 0)
    enabled = getenv("TER_SYNCTRACE") ? 1 : 0;
  return enabled;
}

static void bridge_trace(const char *operation, void *object, void *caller) {
  static unsigned events;
  unsigned event = __atomic_fetch_add(&events, 1, __ATOMIC_RELAXED);
  if (!bridge_sync_trace() || event >= 512)
    return;
  fprintf(stderr,
          "[pthread-bridge] %s tid=%ld object=%p caller=%p\n",
          operation, (long)syscall(SYS_gettid), object, caller);
}

static void *bridge_slot(void *object, size_t real_size,
                         void (*initialize)(void *, uint32_t)) {
  ter_bhandle *handle = object;
  if (handle->magic == TERRARIA_BRIDGE_MAGIC && handle->real)
    return handle->real;

  pthread_mutex_lock(&bridge_lock);
  if (!(handle->magic == TERRARIA_BRIDGE_MAGIC && handle->real)) {
    void *real = calloc(1, real_size);
    if (!real) {
      pthread_mutex_unlock(&bridge_lock);
      return NULL;
    }
    if (initialize)
      initialize(real, handle->word0);
    handle->real = real;
    __atomic_store_n(&handle->magic, TERRARIA_BRIDGE_MAGIC,
                     __ATOMIC_RELEASE);
  }
  pthread_mutex_unlock(&bridge_lock);
  return handle->real;
}

/* ---------------------------------------------------------------- mutex */

#define BIONIC_MUTEX_TYPE(word) (((word) >> 14) & 3)

static void bridge_mutex_initialize(void *real, uint32_t word) {
  pthread_mutexattr_t attribute;
  pthread_mutexattr_init(&attribute);
  switch (BIONIC_MUTEX_TYPE(word)) {
  case 1:
    pthread_mutexattr_settype(&attribute, PTHREAD_MUTEX_RECURSIVE);
    break;
  case 2:
    pthread_mutexattr_settype(&attribute, PTHREAD_MUTEX_ERRORCHECK);
    break;
  default:
    pthread_mutexattr_settype(&attribute, PTHREAD_MUTEX_NORMAL);
    break;
  }
  pthread_mutex_init(real, &attribute);
  pthread_mutexattr_destroy(&attribute);
}

static pthread_mutex_t *bridge_mutex(void *object) {
  return bridge_slot(object, sizeof(pthread_mutex_t),
                     bridge_mutex_initialize);
}

static int bridge_mutex_init(void *object, const void *attribute) {
  ter_bhandle *handle = object;
  handle->word0 = attribute ? (uint32_t)*(const int *)attribute : 0;
  handle->magic = 0;
  handle->real = NULL;
  return bridge_mutex(object) ? 0 : ENOMEM;
}

static int bridge_mutex_destroy(void *object) {
  ter_bhandle *handle = object;
  if (handle->magic == TERRARIA_BRIDGE_MAGIC && handle->real) {
    pthread_mutex_destroy(handle->real);
    free(handle->real);
    handle->real = NULL;
    handle->magic = 0;
  }
  return 0;
}

static int bridge_mutex_lock(void *object) {
  pthread_mutex_t *mutex = bridge_mutex(object);
  return mutex ? pthread_mutex_lock(mutex) : ENOMEM;
}

static int bridge_mutex_unlock(void *object) {
  pthread_mutex_t *mutex = bridge_mutex(object);
  return mutex ? pthread_mutex_unlock(mutex) : ENOMEM;
}

static int bridge_mutex_trylock(void *object) {
  pthread_mutex_t *mutex = bridge_mutex(object);
  return mutex ? pthread_mutex_trylock(mutex) : ENOMEM;
}

static int bridge_mutex_timedlock(void *object,
                                  const struct timespec *timeout) {
  pthread_mutex_t *mutex = bridge_mutex(object);
  return mutex ? pthread_mutex_timedlock(mutex, timeout) : ENOMEM;
}

static int bridge_mutexattr_init(int *attribute) {
  if (attribute)
    *attribute = 0;
  return 0;
}

static int bridge_mutexattr_destroy(int *attribute) {
  (void)attribute;
  return 0;
}

static int bridge_mutexattr_settype(int *attribute, int type) {
  if (attribute)
    *attribute = (type & 3) << 14;
  return 0;
}

static int bridge_mutexattr_gettype(const int *attribute, int *type) {
  if (type)
    *type = attribute ? BIONIC_MUTEX_TYPE((uint32_t)*attribute) : 0;
  return 0;
}

static int bridge_mutexattr_setpshared(int *attribute, int shared) {
  (void)attribute;
  (void)shared;
  return 0;
}

/* ----------------------------------------------------------------- cond */

typedef struct {
  pthread_cond_t condition;
  int monotonic;
} ter_bcond;

static void bridge_cond_initialize(void *real, uint32_t word) {
  ter_bcond *condition = real;
  pthread_condattr_t attribute;
  pthread_condattr_init(&attribute);
  condition->monotonic = (word & 1) == 0;
  pthread_condattr_setclock(&attribute,
                           condition->monotonic ? CLOCK_MONOTONIC
                                                : CLOCK_REALTIME);
  pthread_cond_init(&condition->condition, &attribute);
  pthread_condattr_destroy(&attribute);
}

static ter_bcond *bridge_condition(void *object) {
  return bridge_slot(object, sizeof(ter_bcond), bridge_cond_initialize);
}

static int bridge_cond_init(void *object, const void *attribute) {
  ter_bhandle *handle = object;
  handle->word0 = attribute ? (uint32_t)*(const int *)attribute : 0;
  handle->magic = 0;
  handle->real = NULL;
  return bridge_condition(object) ? 0 : ENOMEM;
}

static int bridge_cond_destroy(void *object) {
  ter_bhandle *handle = object;
  if (handle->magic == TERRARIA_BRIDGE_MAGIC && handle->real) {
    ter_bcond *condition = handle->real;
    pthread_cond_destroy(&condition->condition);
    free(condition);
    handle->real = NULL;
    handle->magic = 0;
  }
  return 0;
}

static int bridge_cond_signal(void *object) {
  ter_bcond *condition = bridge_condition(object);
  bridge_trace("cond_signal", object, __builtin_return_address(0));
  return condition ? pthread_cond_signal(&condition->condition) : ENOMEM;
}

static int bridge_cond_broadcast(void *object) {
  ter_bcond *condition = bridge_condition(object);
  bridge_trace("cond_broadcast", object, __builtin_return_address(0));
  return condition ? pthread_cond_broadcast(&condition->condition) : ENOMEM;
}

/*
 * POSIX permits spurious wakeups.  A 50 ms ceiling converts a lost signal into
 * a bounded hitch while the native Unity caller still owns and rechecks its
 * original predicate.
 */
static int bridge_cond_wait(void *object, void *mutex_object) {
  ter_bcond *condition = bridge_condition(object);
  pthread_mutex_t *mutex = bridge_mutex(mutex_object);
  if (!condition || !mutex)
    return ENOMEM;

  static _Thread_local void *last_wait_object;
  if (bridge_sync_trace() && last_wait_object != object) {
    last_wait_object = object;
    bridge_trace("cond_wait", object, __builtin_return_address(0));
  }

  struct timespec timeout;
  clock_gettime(condition->monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME,
                &timeout);
  timeout.tv_nsec += 50000000L;
  if (timeout.tv_nsec >= 1000000000L) {
    timeout.tv_nsec -= 1000000000L;
    timeout.tv_sec++;
  }
  int result = pthread_cond_timedwait(&condition->condition, mutex, &timeout);
  return result == ETIMEDOUT ? 0 : result;
}

static int bridge_cond_timedwait(void *object, void *mutex_object,
                                 const struct timespec *timeout) {
  ter_bcond *condition = bridge_condition(object);
  pthread_mutex_t *mutex = bridge_mutex(mutex_object);
  return condition && mutex
             ? pthread_cond_timedwait(&condition->condition, mutex, timeout)
             : ENOMEM;
}

static int bridge_condattr_init(int *attribute) {
  if (attribute)
    *attribute = 0;
  return 0;
}

static int bridge_condattr_destroy(int *attribute) {
  (void)attribute;
  return 0;
}

static int bridge_condattr_setclock(int *attribute, int clock_id) {
  if (attribute)
    *attribute = clock_id == CLOCK_REALTIME ? 1 : 0;
  return 0;
}

static int bridge_condattr_setpshared(int *attribute, int shared) {
  (void)attribute;
  (void)shared;
  return 0;
}

/* --------------------------------------------------------------- rwlock */

static void bridge_rwlock_initialize(void *real, uint32_t word) {
  (void)word;
  pthread_rwlock_init(real, NULL);
}

static pthread_rwlock_t *bridge_rwlock(void *object) {
  return bridge_slot(object, sizeof(pthread_rwlock_t),
                     bridge_rwlock_initialize);
}

static int bridge_rwlock_init(void *object, const void *attribute) {
  ter_bhandle *handle = object;
  (void)attribute;
  handle->word0 = 0;
  handle->magic = 0;
  handle->real = NULL;
  return bridge_rwlock(object) ? 0 : ENOMEM;
}

static int bridge_rwlock_destroy(void *object) {
  ter_bhandle *handle = object;
  if (handle->magic == TERRARIA_BRIDGE_MAGIC && handle->real) {
    pthread_rwlock_destroy(handle->real);
    free(handle->real);
    handle->real = NULL;
    handle->magic = 0;
  }
  return 0;
}

static int bridge_rwlock_rdlock(void *object) {
  pthread_rwlock_t *lock = bridge_rwlock(object);
  return lock ? pthread_rwlock_rdlock(lock) : ENOMEM;
}

static int bridge_rwlock_wrlock(void *object) {
  pthread_rwlock_t *lock = bridge_rwlock(object);
  return lock ? pthread_rwlock_wrlock(lock) : ENOMEM;
}

static int bridge_rwlock_unlock(void *object) {
  pthread_rwlock_t *lock = bridge_rwlock(object);
  return lock ? pthread_rwlock_unlock(lock) : ENOMEM;
}

static int bridge_rwlock_tryrdlock(void *object) {
  pthread_rwlock_t *lock = bridge_rwlock(object);
  return lock ? pthread_rwlock_tryrdlock(lock) : ENOMEM;
}

static int bridge_rwlock_trywrlock(void *object) {
  pthread_rwlock_t *lock = bridge_rwlock(object);
  return lock ? pthread_rwlock_trywrlock(lock) : ENOMEM;
}

/* ------------------------------------------------------------ semaphore */

#define TERRARIA_SEM_MAX 512
static struct {
  void *key;
  sem_t *semaphore;
} bridge_semaphores[TERRARIA_SEM_MAX];
static int bridge_semaphore_count;

static sem_t *bridge_semaphore(void *object, unsigned initial, int creating) {
  pthread_mutex_lock(&bridge_lock);
  for (int index = 0; index < bridge_semaphore_count; index++) {
    if (bridge_semaphores[index].key == object) {
      sem_t *semaphore = bridge_semaphores[index].semaphore;
      if (creating) {
        int current = 0;
        sem_getvalue(semaphore, &current);
        for (unsigned value = current > 0 ? (unsigned)current : 0;
             value < initial; value++)
          sem_post(semaphore);
      }
      pthread_mutex_unlock(&bridge_lock);
      return semaphore;
    }
  }

  sem_t *semaphore = calloc(1, sizeof(*semaphore));
  if (!semaphore) {
    pthread_mutex_unlock(&bridge_lock);
    return NULL;
  }
  sem_init(semaphore, 0, initial);
  if (bridge_semaphore_count < TERRARIA_SEM_MAX) {
    bridge_semaphores[bridge_semaphore_count].key = object;
    bridge_semaphores[bridge_semaphore_count].semaphore = semaphore;
    bridge_semaphore_count++;
  } else {
    fprintf(stderr, "[pthread-bridge] semaphore table full\n");
  }
  pthread_mutex_unlock(&bridge_lock);
  return semaphore;
}

static int bridge_sem_init(void *object, int shared, unsigned value) {
  (void)shared;
  return bridge_semaphore(object, value, 1) ? 0 : ENOMEM;
}

/* Keep the address mapping alive; Android code may reuse static storage. */
static int bridge_sem_destroy(void *object) {
  (void)object;
  return 0;
}

static int bridge_sem_post(void *object) {
  sem_t *semaphore = bridge_semaphore(object, 0, 0);
  return semaphore ? sem_post(semaphore) : -1;
}

static int bridge_sem_wait(void *object) {
  sem_t *semaphore = bridge_semaphore(object, 0, 0);
  if (!semaphore) {
    errno = ENOMEM;
    return -1;
  }
  int result;
  do {
    result = sem_wait(semaphore);
  } while (result == -1 && errno == EINTR);
  return result;
}

static int bridge_sem_trywait(void *object) {
  sem_t *semaphore = bridge_semaphore(object, 0, 0);
  return semaphore ? sem_trywait(semaphore) : -1;
}

static int bridge_sem_timedwait(void *object,
                                const struct timespec *timeout) {
  sem_t *semaphore = bridge_semaphore(object, 0, 0);
  return semaphore ? sem_timedwait(semaphore, timeout) : -1;
}

static int bridge_sem_getvalue(void *object, int *value) {
  sem_t *semaphore = bridge_semaphore(object, 0, 0);
  return semaphore ? sem_getvalue(semaphore, value) : -1;
}

/* ------------------------------------------------------------------ attr */

/* 40 bytes used from bionic arm64's 56-byte pthread_attr_t. */
typedef struct {
  uint32_t flags;
  void *stack_base;
  size_t stack_size;
  size_t guard_size;
  int32_t scheduling_policy;
  int32_t scheduling_priority;
} ter_bionic_attr;

#define BIONIC_ATTR_DETACHED 1u

static int bridge_attr_init(void *attribute) {
  ter_bionic_attr *bionic = attribute;
  memset(bionic, 0, sizeof(*bionic));
  bionic->stack_size = 1024 * 1024;
  bionic->guard_size = (size_t)getpagesize();
  return 0;
}

static int bridge_attr_destroy(void *attribute) {
  (void)attribute;
  return 0;
}

static int bridge_attr_setstacksize(void *attribute, size_t size) {
  ((ter_bionic_attr *)attribute)->stack_size = size;
  return 0;
}

static int bridge_attr_getstacksize(void *attribute, size_t *size) {
  *size = ((ter_bionic_attr *)attribute)->stack_size;
  return 0;
}

static int bridge_attr_setguardsize(void *attribute, size_t size) {
  ((ter_bionic_attr *)attribute)->guard_size = size;
  return 0;
}

static int bridge_attr_setdetachstate(void *attribute, int state) {
  ter_bionic_attr *bionic = attribute;
  if (state)
    bionic->flags |= BIONIC_ATTR_DETACHED;
  else
    bionic->flags &= ~BIONIC_ATTR_DETACHED;
  return 0;
}

static int bridge_attr_getdetachstate(void *attribute, int *state) {
  *state = (((ter_bionic_attr *)attribute)->flags & BIONIC_ATTR_DETACHED)
               ? 1
               : 0;
  return 0;
}

static int bridge_attr_setschedparam(void *attribute, const void *parameter) {
  ((ter_bionic_attr *)attribute)->scheduling_priority =
      *(const int *)parameter;
  return 0;
}

static int bridge_attr_setschedpolicy(void *attribute, int policy) {
  ((ter_bionic_attr *)attribute)->scheduling_policy = policy;
  return 0;
}

static int bridge_attr_getstack(void *attribute, void **base, size_t *size) {
  ter_bionic_attr *bionic = attribute;
  *base = bionic->stack_base;
  *size = bionic->stack_size;
  return 0;
}

static int bridge_getattr_np(pthread_t thread, void *attribute) {
  ter_bionic_attr *bionic = attribute;
  pthread_attr_t host;
  bridge_attr_init(bionic);
  if (pthread_getattr_np(thread, &host) == 0) {
    void *base = NULL;
    size_t size = 0;
    pthread_attr_getstack(&host, &base, &size);
    bionic->stack_base = base;
    bionic->stack_size = size;
    pthread_attr_destroy(&host);
  }
  return 0;
}

static int bridge_create(pthread_t *thread, const void *attribute,
                         void *(*start)(void *), void *argument) {
  pthread_attr_t host;
  pthread_attr_init(&host);

  size_t requested_stack = 0;
  uint32_t requested_flags = 0;
  if (attribute) {
    const ter_bionic_attr *bionic = attribute;
    requested_stack = bionic->stack_size;
    requested_flags = bionic->flags;
    if (bionic->stack_size >= 16384)
      pthread_attr_setstacksize(&host, bionic->stack_size);
    if (bionic->guard_size)
      pthread_attr_setguardsize(&host, bionic->guard_size);
    if (bionic->flags & BIONIC_ATTR_DETACHED)
      pthread_attr_setdetachstate(&host, PTHREAD_CREATE_DETACHED);
  }

  int result = pthread_create(thread, &host, start, argument);
  if (getenv("TER_THREADLOG"))
    fprintf(stderr,
            "[pthread-bridge] create attr=%p stack=%zu flags=%#x "
            "start=%p arg=%p -> %d thread=%p\n",
            attribute, requested_stack, requested_flags, (void *)start,
            argument, result,
            (void *)(uintptr_t)(result == 0 && thread ? *thread : 0));
  pthread_attr_destroy(&host);
  return result;
}

static int bridge_setname_np(pthread_t thread, const char *name) {
  char short_name[16];
  snprintf(short_name, sizeof(short_name), "%s", name ? name : "");
  return pthread_setname_np(thread, short_name);
}

/* --------------------------------------------------------------- publish */

struct terraria_pthread_import {
  const char *name;
  void *function;
};

static const struct terraria_pthread_import bridge_imports[] = {
    {"pthread_mutex_init", (void *)bridge_mutex_init},
    {"pthread_mutex_destroy", (void *)bridge_mutex_destroy},
    {"pthread_mutex_lock", (void *)bridge_mutex_lock},
    {"pthread_mutex_unlock", (void *)bridge_mutex_unlock},
    {"pthread_mutex_trylock", (void *)bridge_mutex_trylock},
    {"pthread_mutex_timedlock", (void *)bridge_mutex_timedlock},
    {"pthread_mutexattr_init", (void *)bridge_mutexattr_init},
    {"pthread_mutexattr_destroy", (void *)bridge_mutexattr_destroy},
    {"pthread_mutexattr_settype", (void *)bridge_mutexattr_settype},
    {"pthread_mutexattr_gettype", (void *)bridge_mutexattr_gettype},
    {"pthread_mutexattr_setpshared", (void *)bridge_mutexattr_setpshared},
    {"pthread_cond_init", (void *)bridge_cond_init},
    {"pthread_cond_destroy", (void *)bridge_cond_destroy},
    {"pthread_cond_wait", (void *)bridge_cond_wait},
    {"pthread_cond_timedwait", (void *)bridge_cond_timedwait},
    {"pthread_cond_signal", (void *)bridge_cond_signal},
    {"pthread_cond_broadcast", (void *)bridge_cond_broadcast},
    {"pthread_condattr_init", (void *)bridge_condattr_init},
    {"pthread_condattr_destroy", (void *)bridge_condattr_destroy},
    {"pthread_condattr_setclock", (void *)bridge_condattr_setclock},
    {"pthread_condattr_setpshared", (void *)bridge_condattr_setpshared},
    {"pthread_rwlock_init", (void *)bridge_rwlock_init},
    {"pthread_rwlock_destroy", (void *)bridge_rwlock_destroy},
    {"pthread_rwlock_rdlock", (void *)bridge_rwlock_rdlock},
    {"pthread_rwlock_wrlock", (void *)bridge_rwlock_wrlock},
    {"pthread_rwlock_unlock", (void *)bridge_rwlock_unlock},
    {"pthread_rwlock_tryrdlock", (void *)bridge_rwlock_tryrdlock},
    {"pthread_rwlock_trywrlock", (void *)bridge_rwlock_trywrlock},
    {"sem_init", (void *)bridge_sem_init},
    {"sem_destroy", (void *)bridge_sem_destroy},
    {"sem_post", (void *)bridge_sem_post},
    {"sem_wait", (void *)bridge_sem_wait},
    {"sem_trywait", (void *)bridge_sem_trywait},
    {"sem_timedwait", (void *)bridge_sem_timedwait},
    {"sem_getvalue", (void *)bridge_sem_getvalue},
    {"pthread_once", (void *)pthread_once},
    {"pthread_attr_init", (void *)bridge_attr_init},
    {"pthread_attr_destroy", (void *)bridge_attr_destroy},
    {"pthread_attr_setstacksize", (void *)bridge_attr_setstacksize},
    {"pthread_attr_getstacksize", (void *)bridge_attr_getstacksize},
    {"pthread_attr_setguardsize", (void *)bridge_attr_setguardsize},
    {"pthread_attr_setdetachstate", (void *)bridge_attr_setdetachstate},
    {"pthread_attr_getdetachstate", (void *)bridge_attr_getdetachstate},
    {"pthread_attr_setschedparam", (void *)bridge_attr_setschedparam},
    {"pthread_attr_setschedpolicy", (void *)bridge_attr_setschedpolicy},
    {"pthread_attr_getstack", (void *)bridge_attr_getstack},
    {"pthread_getattr_np", (void *)bridge_getattr_np},
    {"pthread_create", (void *)bridge_create},
    {"pthread_setname_np", (void *)bridge_setname_np},
};

const struct terraria_pthread_import *
terraria_pthread_bridge_imports(size_t *count) {
  *count = sizeof(bridge_imports) / sizeof(bridge_imports[0]);
  return bridge_imports;
}
