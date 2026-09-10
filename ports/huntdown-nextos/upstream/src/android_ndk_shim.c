#define _GNU_SOURCE
#include "android_ndk_shim.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum {
  HD_ALOOPER_POLL_WAKE = -1,
  HD_ALOOPER_POLL_TIMEOUT = -3,
  HD_ALOOPER_POLL_ERROR = -4,
};

typedef struct HdLooper {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  unsigned wake_pending;
  unsigned acquired_refs;
  unsigned owner_alive;
} HdLooper;

static pthread_once_t hd_looper_once = PTHREAD_ONCE_INIT;
static pthread_key_t hd_looper_key;

static void hd_looper_free(HdLooper *looper) {
  pthread_cond_destroy(&looper->condition);
  pthread_mutex_destroy(&looper->mutex);
  free(looper);
}

static void hd_looper_destructor(void *opaque) {
  HdLooper *looper = opaque;
  if (!looper) return;
  pthread_mutex_lock(&looper->mutex);
  looper->owner_alive = 0;
  int can_free = looper->acquired_refs == 0;
  pthread_mutex_unlock(&looper->mutex);
  if (can_free) hd_looper_free(looper);
}

static void hd_looper_init_key(void) {
  pthread_key_create(&hd_looper_key, hd_looper_destructor);
}

static HdLooper *hd_ALooper_forThread(void) {
  pthread_once(&hd_looper_once, hd_looper_init_key);
  return pthread_getspecific(hd_looper_key);
}

static HdLooper *hd_ALooper_prepare(int opts) {
  (void)opts; /* No fd callbacks are imported by this Unity player. */
  pthread_once(&hd_looper_once, hd_looper_init_key);
  HdLooper *looper = pthread_getspecific(hd_looper_key);
  if (looper) return looper;
  looper = calloc(1, sizeof *looper);
  if (!looper) return NULL;
  pthread_mutex_init(&looper->mutex, NULL);
  pthread_cond_init(&looper->condition, NULL);
  looper->owner_alive = 1;
  if (pthread_setspecific(hd_looper_key, looper) != 0) {
    hd_looper_free(looper);
    return NULL;
  }
  return looper;
}

void hd_android_prepare_main_looper(void) {
  if (!hd_ALooper_prepare(0)) {
    fprintf(stderr, "FATAL: could not prepare Android main looper\n");
    abort();
  }
}

static void hd_ALooper_acquire(HdLooper *looper) {
  if (!looper) return;
  pthread_mutex_lock(&looper->mutex);
  looper->acquired_refs++;
  pthread_mutex_unlock(&looper->mutex);
}

static void hd_ALooper_release(HdLooper *looper) {
  if (!looper) return;
  pthread_mutex_lock(&looper->mutex);
  if (looper->acquired_refs) looper->acquired_refs--;
  int can_free = !looper->owner_alive && looper->acquired_refs == 0;
  pthread_mutex_unlock(&looper->mutex);
  if (can_free) hd_looper_free(looper);
}

static void hd_ALooper_wake(HdLooper *looper) {
  if (!looper) return;
  pthread_mutex_lock(&looper->mutex);
  looper->wake_pending = 1;
  pthread_cond_broadcast(&looper->condition);
  pthread_mutex_unlock(&looper->mutex);
}

static int hd_ALooper_pollOnce(int timeout_ms, int *out_fd, int *out_events,
                               void **out_data) {
  if (out_fd) *out_fd = -1;
  if (out_events) *out_events = 0;
  if (out_data) *out_data = NULL;

  HdLooper *looper = hd_ALooper_forThread();
  if (!looper) return HD_ALOOPER_POLL_ERROR;

  pthread_mutex_lock(&looper->mutex);
  /* There is no Android framework event queue behind this token: the host
   * drives Unity through nativeRender.  In particular, an infinite Android
   * poll must report an idle iteration instead of parking the render thread
   * forever.  Positive waits retain wake/timeout behaviour for users that
   * explicitly request a bounded delay. */
  if (!looper->wake_pending && timeout_ms > 0) {
      struct timespec deadline;
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_sec += timeout_ms / 1000;
      deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
      if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
      }
      while (!looper->wake_pending) {
        int rc = pthread_cond_timedwait(&looper->condition, &looper->mutex,
                                        &deadline);
        if (rc == ETIMEDOUT) break;
        if (rc != 0) {
          pthread_mutex_unlock(&looper->mutex);
          return HD_ALOOPER_POLL_ERROR;
        }
      }
  }
  int woke = looper->wake_pending != 0;
  looper->wake_pending = 0;
  pthread_mutex_unlock(&looper->mutex);
  if (timeout_ms < 0) {
    static int logged;
    if (!logged++)
      fprintf(stderr,
              "[HD-LOOPER] unbounded poll has no Android queue; idle timeout\n");
  }
  return woke ? HD_ALOOPER_POLL_WAKE : HD_ALOOPER_POLL_TIMEOUT;
}

/* Unity 6 installs an internal wake fd on its looper.  The host drives the
 * player/choreographer itself, so accepting registration without fabricating
 * callbacks is the same no-event contract used by the proven Unity 6 port. */
static int hd_ALooper_addFd(HdLooper *looper, int fd, int ident, int events,
                            void *callback, void *data) {
  (void)looper; (void)fd; (void)ident; (void)events; (void)callback; (void)data;
  return 1;
}

static int hd_ALooper_removeFd(HdLooper *looper, int fd) {
  (void)looper; (void)fd;
  return 1;
}

/* Huntdown does not consume accelerometer/gyroscope input.  Report a genuinely
 * unavailable sensor service with the return type each NDK entry expects. */
static void hd_sensor_log_unavailable(void) {
  static int logged;
  if (!logged++)
    fprintf(stderr, "[HD-SENSOR] Android sensor service unavailable\n");
}

static void *hd_sensor_null(void) {
  hd_sensor_log_unavailable();
  return NULL;
}

static int hd_sensor_list(void *manager, const void *const **list) {
  (void)manager;
  if (list) *list = NULL;
  return 0;
}

static int hd_sensor_error(void) { return -EINVAL; }
static int hd_sensor_no_events(void) { return 0; }
static int hd_sensor_zero(void) { return 0; }
static float hd_sensor_zero_float(void) { return 0.0f; }
static const char *hd_sensor_no_string(void) { return NULL; }
static void hd_ndk_noop(void) {}

#define HD_NDK_SYM(name, function) { name, (void *)(function) }

static const HdAndroidNdkSymbol hd_symbols[] = {
    HD_NDK_SYM("ALooper_acquire", hd_ALooper_acquire),
    HD_NDK_SYM("ALooper_forThread", hd_ALooper_forThread),
    HD_NDK_SYM("ALooper_pollOnce", hd_ALooper_pollOnce),
    HD_NDK_SYM("ALooper_addFd", hd_ALooper_addFd),
    HD_NDK_SYM("ALooper_removeFd", hd_ALooper_removeFd),
    HD_NDK_SYM("ALooper_prepare", hd_ALooper_prepare),
    HD_NDK_SYM("ALooper_release", hd_ALooper_release),
    HD_NDK_SYM("ALooper_wake", hd_ALooper_wake),

    HD_NDK_SYM("ASensorManager_getInstance", hd_sensor_null),
    HD_NDK_SYM("ASensorManager_createEventQueue", hd_sensor_null),
    HD_NDK_SYM("ASensorManager_getSensorList", hd_sensor_list),
    HD_NDK_SYM("ASensorManager_getDefaultSensor", hd_sensor_null),
    HD_NDK_SYM("ASensorManager_destroyEventQueue", hd_sensor_error),
    HD_NDK_SYM("ASensorEventQueue_hasEvents", hd_sensor_no_events),
    HD_NDK_SYM("ASensorEventQueue_getEvents", hd_sensor_no_events),
    HD_NDK_SYM("ASensorEventQueue_enableSensor", hd_sensor_error),
    HD_NDK_SYM("ASensorEventQueue_disableSensor", hd_sensor_error),
    HD_NDK_SYM("ASensorEventQueue_setEventRate", hd_sensor_error),
    HD_NDK_SYM("ASensor_getType", hd_sensor_zero),
    HD_NDK_SYM("ASensor_getResolution", hd_sensor_zero_float),
    HD_NDK_SYM("ASensor_getMinDelay", hd_sensor_zero),
    HD_NDK_SYM("ASensor_getName", hd_sensor_no_string),
    HD_NDK_SYM("ASensor_getVendor", hd_sensor_no_string),

    HD_NDK_SYM("__google_potentially_blocking_region_begin", hd_ndk_noop),
    HD_NDK_SYM("__google_potentially_blocking_region_end", hd_ndk_noop),
};

const HdAndroidNdkSymbol *hd_android_ndk_symbols(size_t *count) {
  if (count) *count = sizeof hd_symbols / sizeof hd_symbols[0];
  return hd_symbols;
}
