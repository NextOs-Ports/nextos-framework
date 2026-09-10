/*
 * android.c -- the libandroid.so / libmediandk.so surface libunity imports.
 *
 * libunity's dynamic table names 24 ASensor* entries, six ALooper*, seven
 * ANativeWindow* and the media NDK; almost all of them exist only so the
 * player can query hardware it will not find here.  What has to be real is
 * ANativeWindow (Unity sizes its back buffer from it) and ALooper (the main
 * thread's event pump).  Everything else answers "not available" in the way
 * the NDK documents, which is a path Unity already handles on phones without
 * the sensor in question.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#include "nx_elf.h"
#include "gb.h"

/* ------------------------------------------------------------ native window */

/* One window, owned by the video layer; ANativeWindow is just a view of it. */
typedef struct {
    int32_t refs;
    int32_t width, height, format;
} fp2_window;

static fp2_window the_window = { 1, 1280, 720, 1 /* RGBA_8888 */ };
static struct { unsigned short width, height; } fbdev_window = { 1280, 720 };

static void refresh_fb_size(void)
{
    int fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    struct fb_var_screeninfo v;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 &&
        v.xres > 0 && v.yres > 0 && v.xres < 32768 && v.yres < 32768) {
        the_window.width = (int32_t)v.xres;
        the_window.height = (int32_t)v.yres;
        fbdev_window.width = (unsigned short)v.xres;
        fbdev_window.height = (unsigned short)v.yres;
    }
    close(fd);
}

void fp2_window_set_size(int w, int h)
{
    the_window.width = w;
    the_window.height = h;
    if (w > 0 && h > 0 && w < 32768 && h < 32768) {
        fbdev_window.width = (unsigned short)w;
        fbdev_window.height = (unsigned short)h;
    }
}

void *fp2_window_handle(void) { return &the_window; }

/* Mali's fbdev EGLNativeWindowType is a two-u16 width/height record. */
void *fp2_native_window(void)
{
    refresh_fb_size();
    nx_log("fbdev native window %ux%u", fbdev_window.width,
           fbdev_window.height);
    return &fbdev_window;
}

static void *a_window_fromSurface(void *env, void *surface)
{
    (void)env; (void)surface;
    refresh_fb_size();
    __atomic_fetch_add(&the_window.refs, 1, __ATOMIC_RELAXED);
    return &the_window;
}
static void a_window_acquire(void *w)
{
    if (w) __atomic_fetch_add(&((fp2_window *)w)->refs, 1, __ATOMIC_RELAXED);
}
static void a_window_release(void *w)
{
    if (w) __atomic_fetch_sub(&((fp2_window *)w)->refs, 1, __ATOMIC_RELAXED);
}
static int32_t a_window_getWidth(void *w)  { return w ? ((fp2_window *)w)->width : 0; }
static int32_t a_window_getHeight(void *w) { return w ? ((fp2_window *)w)->height : 0; }
static int32_t a_window_getFormat(void *w) { return w ? ((fp2_window *)w)->format : 1; }
static int32_t a_window_setBuffersGeometry(void *w, int32_t width, int32_t height,
                                           int32_t fmt)
{
    fp2_window *p = w;
    if (!p)
        return -EINVAL;
    /* Unity asks for its render resolution here.  Honour it for the numbers it
     * reads back, but the presented surface size is the framebuffer's. */
    if (width > 0)  p->width = width;
    if (height > 0) p->height = height;
    if (fmt)        p->format = fmt;
    nx_log("ANativeWindow_setBuffersGeometry %dx%d fmt=%d", width, height, fmt);
    return 0;
}
static int32_t a_window_setBuffersTransform(void *w, int32_t t) { (void)w; (void)t; return 0; }
static int32_t a_window_lock(void *w, void *buf, void *rect) { (void)w; (void)buf; (void)rect; return -ENODEV; }
static int32_t a_window_unlockAndPost(void *w) { (void)w; return -ENODEV; }

/* -------------------------------------------------------------------- looper */

/* Unity prepares a looper on its main thread and polls it.  Nothing posts to
 * it here, so a token object plus a poll that reports "nothing ready" is the
 * honest answer; the caller treats that as an idle frame. */
typedef struct { int32_t refs; } fp2_looper;
static __thread fp2_looper *tls_looper;
static fp2_looper main_looper = { 1 };

static void *a_looper_forThread(void) { return tls_looper; }

/* Unity asks for the UI thread's looper during initJni and logs
 * "Couldn't retrieve native ALooper for UI thread." when it gets NULL.  On a
 * real device the UI thread always has one because the Android framework
 * prepared it long before any native code runs; here nothing ever calls
 * ALooper_prepare on the thread we present as the UI thread, so we prepare it
 * ourselves before handing control to Unity. */
void fp2_android_prepare_main_looper(void)
{
    if (!tls_looper)
        tls_looper = &main_looper;
}
static void *a_looper_prepare(int opts)
{
    (void)opts;
    if (!tls_looper)
        tls_looper = &main_looper;
    return tls_looper;
}
static void a_looper_acquire(void *l) { if (l) ((fp2_looper *)l)->refs++; }
static void a_looper_release(void *l) { if (l) ((fp2_looper *)l)->refs--; }
static void a_looper_wake(void *l) { (void)l; }
static int a_looper_pollOnce(int timeout_ms, int *fd, int *events, void **data)
{
    if (fd) *fd = -1;
    if (events) *events = 0;
    if (data) *data = NULL;
    if (timeout_ms > 0) {
        struct timespec ts = { timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
    return -3;   /* ALOOPER_POLL_TIMEOUT */
}
static int a_looper_pollAll(int t, int *fd, int *ev, void **d)
{
    return a_looper_pollOnce(t, fd, ev, d);
}
static int a_looper_addFd(void *l, int fd, int id, int ev, void *cb, void *d)
{
    (void)l; (void)fd; (void)id; (void)ev; (void)cb; (void)d;
    return 1;
}
static int a_looper_removeFd(void *l, int fd) { (void)l; (void)fd; return 1; }

/* ------------------------------------------------------------------- sensors */

/* No accelerometer, gyroscope or light sensor on the box.  Returning NULL from
 * getDefaultSensor is exactly what a phone without that sensor does. */
static void *a_sensormanager_getInstance(void) { static int t; return &t; }
static void *a_sensormanager_getInstanceForPackage(const char *p) { (void)p; return a_sensormanager_getInstance(); }
static void *a_sensormanager_getDefaultSensor(void *m, int type) { (void)m; (void)type; return NULL; }
static int a_sensormanager_getSensorList(void *m, void ***list)
{
    (void)m;
    if (list) *list = NULL;
    return 0;
}
static void *a_sensormanager_createEventQueue(void *m, void *l, int id, void *cb, void *d)
{
    (void)m; (void)l; (void)id; (void)cb; (void)d;
    static int q;
    return &q;
}
static int a_sensormanager_destroyEventQueue(void *m, void *q) { (void)m; (void)q; return 0; }
static int a_eventqueue_enableSensor(void *q, void *s) { (void)q; (void)s; return -EINVAL; }
static int a_eventqueue_disableSensor(void *q, void *s) { (void)q; (void)s; return -EINVAL; }
static int a_eventqueue_setEventRate(void *q, void *s, int32_t us) { (void)q; (void)s; (void)us; return -EINVAL; }
static int a_eventqueue_hasEvents(void *q) { (void)q; return 0; }
static ssize_t a_eventqueue_getEvents(void *q, void *ev, size_t n) { (void)q; (void)ev; (void)n; return 0; }
static const char *a_sensor_getName(void *s) { (void)s; return ""; }
static const char *a_sensor_getVendor(void *s) { (void)s; return ""; }
static int a_sensor_getType(void *s) { (void)s; return 0; }
static float a_sensor_getResolution(void *s) { (void)s; return 0.0f; }
static int a_sensor_getMinDelay(void *s) { (void)s; return 0; }

/* --------------------------------------------------------------------- trace */

static void a_trace_begin(const char *n) { (void)n; }
static void a_trace_end(void) { }
static int a_trace_isEnabled(void) { return 0; }

/* Unity's Java side normally drives nativeRender from the Choreographer; our
 * main loop does that instead, so the native Choreographer is never needed. */
/* ---- AChoreographer (native NDK) ------------------------------------------
 * Unity 2022 drove frames through the Java android/view/Choreographer proxy
 * (see jni.c).  Unity 6 additionally imports the NATIVE NDK entry points, and
 * imports them WEAK -- so leaving them unresolved is legal and silent, and the
 * engine simply never gets a vsync callback.  The observed symptom is a busy
 * spin inside initJni with 100% of samples in Unity's context-type getter.
 *
 * Posting is one-shot on Android: the callback re-posts itself from inside the
 * callback.  Snapshot the queue before invoking so a re-post lands on the next
 * tick instead of looping forever inside this one.  Delivery is driven by the
 * same 60 Hz thread that already paces the Java doFrame. */
#define FP2_CHOREO_MAX 16

typedef void (*fp2_frame_cb)(int64_t nanos, void *data);

static struct {
    fp2_frame_cb cb;
    void *data;
} choreo_q[FP2_CHOREO_MAX];
static int choreo_n;
static pthread_mutex_t choreo_lock = PTHREAD_MUTEX_INITIALIZER;
static int choreo_instance;   /* address used as the opaque AChoreographer* */

static void *a_choreographer_getInstance(void) { return &choreo_instance; }

static void a_choreographer_post(void *chor, fp2_frame_cb cb, void *data)
{
    (void)chor;
    if (!cb)
        return;
    pthread_mutex_lock(&choreo_lock);
    if (choreo_n < FP2_CHOREO_MAX) {
        choreo_q[choreo_n].cb = cb;
        choreo_q[choreo_n].data = data;
        choreo_n++;
    }
    pthread_mutex_unlock(&choreo_lock);
}

/* Called once per tick by the choreographer driver in jni.c. */
void fp2_choreographer_native_tick(int64_t nanos)
{
    struct { fp2_frame_cb cb; void *data; } batch[FP2_CHOREO_MAX];
    int n;
    pthread_mutex_lock(&choreo_lock);
    n = choreo_n;
    for (int i = 0; i < n; i++) {
        batch[i].cb = choreo_q[i].cb;
        batch[i].data = choreo_q[i].data;
    }
    choreo_n = 0;
    pthread_mutex_unlock(&choreo_lock);
    for (int i = 0; i < n; i++)
        batch[i].cb(nanos, batch[i].data);
}


/* ------------------------------------------------------------------- mediandk */

/* Media playback is a separate physical gate for this target.  Until a real
 * host decoder is integrated and verified, return the NDK unavailable result
 * instead of fabricating a codec object that Unity could dereference. */
/* The NDK media API used to be stubbed here with null/fail answers.  It now
 * has a real implementation in mediandk_shim.c, and leaving dead stubs in this
 * table would race it: both entries land in the same sorted import table and
 * whichever one the lookup happens to pick decides whether video works. */

/* -------------------------------------------------------------------- table */

#define A(n, f) { n, (void *)(uintptr_t)(f) }


static const nx_import tab[] = {
    A("ANativeWindow_fromSurface",          a_window_fromSurface),
    A("ANativeWindow_acquire",              a_window_acquire),
    A("ANativeWindow_release",              a_window_release),
    A("ANativeWindow_getWidth",             a_window_getWidth),
    A("ANativeWindow_getHeight",            a_window_getHeight),
    A("ANativeWindow_getFormat",            a_window_getFormat),
    A("ANativeWindow_setBuffersGeometry",   a_window_setBuffersGeometry),
    A("ANativeWindow_setBuffersTransform",  a_window_setBuffersTransform),
    A("ANativeWindow_lock",                 a_window_lock),
    A("ANativeWindow_unlockAndPost",        a_window_unlockAndPost),

    A("ALooper_forThread",                  a_looper_forThread),
    A("ALooper_prepare",                    a_looper_prepare),
    A("ALooper_acquire",                    a_looper_acquire),
    A("ALooper_release",                    a_looper_release),
    A("ALooper_wake",                       a_looper_wake),
    A("ALooper_pollOnce",                   a_looper_pollOnce),
    A("ALooper_pollAll",                    a_looper_pollAll),
    A("ALooper_addFd",                      a_looper_addFd),
    A("ALooper_removeFd",                   a_looper_removeFd),

    A("ASensorManager_getInstance",           a_sensormanager_getInstance),
    A("ASensorManager_getInstanceForPackage", a_sensormanager_getInstanceForPackage),
    A("ASensorManager_getDefaultSensor",      a_sensormanager_getDefaultSensor),
    A("ASensorManager_getSensorList",         a_sensormanager_getSensorList),
    A("ASensorManager_createEventQueue",      a_sensormanager_createEventQueue),
    A("ASensorManager_destroyEventQueue",     a_sensormanager_destroyEventQueue),
    A("ASensorEventQueue_enableSensor",       a_eventqueue_enableSensor),
    A("ASensorEventQueue_disableSensor",      a_eventqueue_disableSensor),
    A("ASensorEventQueue_setEventRate",       a_eventqueue_setEventRate),
    A("ASensorEventQueue_hasEvents",          a_eventqueue_hasEvents),
    A("ASensorEventQueue_getEvents",          a_eventqueue_getEvents),
    A("ASensor_getName",                      a_sensor_getName),
    A("ASensor_getVendor",                    a_sensor_getVendor),
    A("ASensor_getType",                      a_sensor_getType),
    A("ASensor_getResolution",                a_sensor_getResolution),
    A("ASensor_getMinDelay",                  a_sensor_getMinDelay),

    A("ATrace_beginSection",                a_trace_begin),
    A("ATrace_endSection",                  a_trace_end),
    A("ATrace_isEnabled",                   a_trace_isEnabled),
    A("AChoreographer_getInstance",         a_choreographer_getInstance),
    A("AChoreographer_postFrameCallback",   a_choreographer_post),
    A("AChoreographer_postFrameCallback64", a_choreographer_post),

};

const nx_import *fp2_android_table(size_t *n)
{
    *n = sizeof tab / sizeof *tab;
    return tab;
}

void *fp2_android_sym(const char *name)
{
    for (size_t i = 0; i < sizeof tab / sizeof *tab; i++)
        if (strcmp(tab[i].name, name) == 0)
            return tab[i].addr;
    return NULL;
}
