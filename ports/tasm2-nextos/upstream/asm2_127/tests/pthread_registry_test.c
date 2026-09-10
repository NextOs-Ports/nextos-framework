#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "pthread_bridge.h"

static void require(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "failure: %s (errno=%d)\n", message, errno);
    exit(1);
  }
}

struct handoff_waiter {
  uint32_t *guest_mutex;
  int acquired;
};

struct cond_waiter {
  uint32_t guest_mutex;
  uint32_t guest_cond;
  int waiting;
  int ready;
  int woke;
};

struct timedwait_guard {
  uint32_t *guest_cond;
  int armed;
};

static void *handoff_waiter_thread(void *opaque) {
  struct handoff_waiter *waiter = opaque;
  if (asm2_pthread_mutex_lock(waiter->guest_mutex) == 0) {
    waiter->acquired = 1;
    (void)asm2_pthread_mutex_unlock(waiter->guest_mutex);
  }
  return NULL;
}

static void *cond_waiter_thread(void *opaque) {
  struct cond_waiter *waiter = opaque;
  if (asm2_pthread_mutex_lock(&waiter->guest_mutex) != 0)
    return NULL;
  __atomic_store_n(&waiter->waiting, 1, __ATOMIC_RELEASE);
  while (!waiter->ready) {
    if (asm2_pthread_cond_wait(&waiter->guest_cond,
                               &waiter->guest_mutex) != 0) {
      (void)asm2_pthread_mutex_unlock(&waiter->guest_mutex);
      return NULL;
    }
  }
  waiter->woke = 1;
  (void)asm2_pthread_mutex_unlock(&waiter->guest_mutex);
  return NULL;
}

static void *timedwait_guard_thread(void *opaque) {
  struct timedwait_guard *guard = opaque;
  const struct timespec delay = {0, 500000000L};
  (void)nanosleep(&delay, NULL);
  if (__atomic_load_n(&guard->armed, __ATOMIC_ACQUIRE))
    (void)asm2_pthread_cond_signal(guard->guest_cond);
  return NULL;
}

static uint64_t elapsed_milliseconds(const struct timespec *start,
                                     const struct timespec *end) {
  int64_t seconds = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
  int64_t nanoseconds = (int64_t)end->tv_nsec - (int64_t)start->tv_nsec;
  return (uint64_t)(seconds * 1000 + nanoseconds / 1000000);
}

int main(void) {
  uint32_t long_lived = 0;
  uint32_t churn = 0;
  require(asm2_pthread_mutex_init(&long_lived, NULL) == 0,
          "initialize long-lived mutex");

  require(asm2_pthread_mutex_lock(&long_lived) == 0,
          "lock mutex for ownership handoff");
  struct asm2_pthread_mutex_handoff handoff;
  require(asm2_pthread_mutex_handoff_begin(&long_lived, &handoff) == 1,
          "release owned guest mutex for handoff");
  struct handoff_waiter waiter = {&long_lived, 0};
  pthread_t waiter_thread;
  require(pthread_create(&waiter_thread, NULL, handoff_waiter_thread,
                         &waiter) == 0,
          "start handoff waiter");
  require(pthread_join(waiter_thread, NULL) == 0, "join handoff waiter");
  require(waiter.acquired, "another thread acquired handed-off mutex");
  require(asm2_pthread_mutex_handoff_end(&handoff) == 0,
          "reacquire handed-off guest mutex");
  require(asm2_pthread_mutex_unlock(&long_lived) == 0,
          "unlock reacquired guest mutex");
  require(asm2_pthread_mutex_handoff_begin(&long_lived, &handoff) == 0,
          "reject handoff by non-owner");

  struct cond_waiter cond_waiter = {0};
  require(asm2_pthread_mutex_init(&cond_waiter.guest_mutex, NULL) == 0,
          "initialize condition mutex");
  require(asm2_pthread_cond_init(&cond_waiter.guest_cond, NULL) == 0,
          "initialize condition variable");
  pthread_t condition_thread;
  require(pthread_create(&condition_thread, NULL, cond_waiter_thread,
                         &cond_waiter) == 0,
          "start condition waiter");
  while (!__atomic_load_n(&cond_waiter.waiting, __ATOMIC_ACQUIRE))
    sched_yield();
  require(asm2_pthread_mutex_lock(&cond_waiter.guest_mutex) == 0,
          "lock condition mutex for signal");
  cond_waiter.ready = 1;
  require(asm2_pthread_cond_signal(&cond_waiter.guest_cond) == 0,
          "signal condition variable");
  require(asm2_pthread_mutex_unlock(&cond_waiter.guest_mutex) == 0,
          "unlock condition mutex after signal");
  require(pthread_join(condition_thread, NULL) == 0,
          "join condition waiter");
  require(cond_waiter.woke, "condition waiter reacquired tracked mutex");

  struct timeval wall_now;
  require(gettimeofday(&wall_now, NULL) == 0,
          "read realtime clock for guest deadline");
  struct timespec deadline = {wall_now.tv_sec,
                              wall_now.tv_usec * 1000L + 50000000L};
  if (deadline.tv_nsec >= 1000000000L) {
    ++deadline.tv_sec;
    deadline.tv_nsec -= 1000000000L;
  }
  struct timedwait_guard guard = {&cond_waiter.guest_cond, 1};
  pthread_t guard_thread;
  require(pthread_create(&guard_thread, NULL, timedwait_guard_thread,
                         &guard) == 0,
          "start timed-wait deadlock guard");
  require(asm2_pthread_mutex_lock(&cond_waiter.guest_mutex) == 0,
          "lock condition mutex for timed wait");
  struct timespec wait_start;
  struct timespec wait_end;
  require(clock_gettime(CLOCK_MONOTONIC, &wait_start) == 0,
          "measure timed-wait start");
  int timedwait_result = asm2_pthread_cond_timedwait(
      &cond_waiter.guest_cond, &cond_waiter.guest_mutex, &deadline);
  require(clock_gettime(CLOCK_MONOTONIC, &wait_end) == 0,
          "measure timed-wait end");
  require(asm2_pthread_mutex_unlock(&cond_waiter.guest_mutex) == 0,
          "unlock condition mutex after timed wait");
  __atomic_store_n(&guard.armed, 0, __ATOMIC_RELEASE);
  require(pthread_join(guard_thread, NULL) == 0,
          "join timed-wait deadlock guard");
  require(timedwait_result == ETIMEDOUT,
          "realtime guest deadline expired normally");
  require(elapsed_milliseconds(&wait_start, &wait_end) < 400,
          "realtime deadline was not interpreted as monotonic epoch");

  require(asm2_pthread_cond_destroy(&cond_waiter.guest_cond) == 0,
          "destroy condition variable");
  require(asm2_pthread_mutex_destroy(&cond_waiter.guest_mutex) == 0,
          "destroy condition mutex");

  for (unsigned int index = 0; index < 50000; ++index) {
    require(churn == 0, "destroy cleared guest mutex slot");
    require(asm2_pthread_mutex_init(&churn, NULL) == 0,
            "initialize churn mutex");
    require(asm2_pthread_mutex_lock(&churn) == 0, "lock churn mutex");
    require(asm2_pthread_mutex_unlock(&churn) == 0,
            "unlock churn mutex");
    require(asm2_pthread_mutex_destroy(&churn) == 0,
            "destroy churn mutex");
  }

  require(asm2_pthread_mutex_lock(&long_lived) == 0,
          "lock long-lived mutex after churn");
  require(asm2_pthread_mutex_unlock(&long_lived) == 0,
          "unlock long-lived mutex after churn");

  struct asm2_pthread_bridge_stats stats;
  asm2_pthread_bridge_get_stats(&stats);
  require(stats.created == 50003 && stats.active == 1 &&
              stats.retired == 50002 && stats.mutex_handoffs == 1,
          "registry counters before final destroy");
  require(stats.longest_active_bucket <= 1,
          "retired objects do not lengthen active lookup");
  require(asm2_pthread_mutex_destroy(&long_lived) == 0,
          "destroy long-lived mutex");
  asm2_pthread_bridge_get_stats(&stats);
  require(stats.active == 0 && stats.retired == 50003 &&
              stats.longest_active_bucket == 0,
          "registry counters after final destroy");

  puts("pthread bridge: handoff, realtime cond waits and O(1) active lookup OK");
  return 0;
}
