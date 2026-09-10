/*
 * android.c -- the libandroid.so surface liblime imports.
 *
 * liblime names ANativeWindow, ALooper and the sensor NDK.  What has to be
 * real is ANativeWindow -- SDL's Android video driver sizes its window from it
 * and hands it to eglCreateWindowSurface -- and ALooper, which SDL prepares on
 * the thread that runs the event pump.  The sensors answer "not available" the
 * way the NDK documents, which is the path SDL already takes on a phone that
 * lacks the sensor.
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
#include "rcr.h"
#include "video.h"

/* ------------------------------------------------------------ native window */

/* One window, owned by the video layer; ANativeWindow is just a view of it. */
typedef struct {
    int32_t refs;
    int32_t width, height, format;
} rcr_window;

static rcr_window the_window = { 1, 1280, 720, 1 /* RGBA_8888 */ };
static struct { unsigned short width, height; } fbdev_window = { 1280, 720 };

/* Read once and remember.  The panel does not change mode under the game, and
 * the input thread asks for the size on every poll -- reopening the frame
 * buffer a few hundred times a second is both pointless and rude to the
 * driver that is presenting from it. */
static void refresh_fb_size(void)
{
    if (rcr_video_mode() == RCR_VIDEO_SDL)
        return;
    static int done;
    if (done)
        return;
    done = 1;
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

void rcr_window_set_size(int w, int h)
{
    the_window.width = w;
    the_window.height = h;
    if (w > 0 && h > 0 && w < 32768 && h < 32768) {
        fbdev_window.width = (unsigned short)w;
        fbdev_window.height = (unsigned short)h;
    }
}

void *rcr_window_handle(void) { return &the_window; }

/* Mali's fbdev EGLNativeWindowType is a two-u16 width/height record. */
void *rcr_native_window(void)
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
    if (w) __atomic_fetch_add(&((rcr_window *)w)->refs, 1, __ATOMIC_RELAXED);
}
static void a_window_release(void *w)
{
    if (w) __atomic_fetch_sub(&((rcr_window *)w)->refs, 1, __ATOMIC_RELAXED);
}
static int32_t a_window_getWidth(void *w)  { return w ? ((rcr_window *)w)->width : 0; }
static int32_t a_window_getHeight(void *w) { return w ? ((rcr_window *)w)->height : 0; }
static int32_t a_window_getFormat(void *w) { return w ? ((rcr_window *)w)->format : 1; }
static int32_t a_window_setBuffersGeometry(void *w, int32_t width, int32_t height,
                                           int32_t fmt)
{
    rcr_window *p = w;
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

/* SDL prepares a looper on its event thread and polls it.  Nothing posts to
 * it here, so a token object plus a poll that reports "nothing ready" is the
 * honest answer; the caller treats that as an idle frame. */
typedef struct { int32_t refs; } rcr_looper;
static __thread rcr_looper *tls_looper;
static rcr_looper main_looper = { 1 };

static void *a_looper_forThread(void) { return tls_looper; }
static void *a_looper_prepare(int opts)
{
    (void)opts;
    if (!tls_looper)
        tls_looper = &main_looper;
    return tls_looper;
}
static void a_looper_acquire(void *l) { if (l) ((rcr_looper *)l)->refs++; }
static void a_looper_release(void *l) { if (l) ((rcr_looper *)l)->refs--; }
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

/* SDL only takes the native Choreographer when it is available; without it it
 * falls back to its own frame pacing, which is what this port wants. */
static void *a_choreographer_getInstance(void) { return NULL; }

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
};

const nx_import *rcr_android_table(size_t *n)
{
    *n = sizeof tab / sizeof *tab;
    return tab;
}

void *rcr_android_sym(const char *name)
{
    for (size_t i = 0; i < sizeof tab / sizeof *tab; i++)
        if (strcmp(tab[i].name, name) == 0)
            return tab[i].addr;
    return NULL;
}

/* The panel size -- never a hardcoded resolution, and never
 * /sys/class/graphics/fb0/virtual_size, which counts the second buffer and
 * reports twice the panel height on this hardware.
 *
 * Order of authority: the real drawable SDL owns (KMSDRM/Wayland), then the
 * fbdev mode (Mali-450), then the built-in fallback. */
int rcr_screen_width(void)
{
    int w = 0, h = 0;
    if (rcr_video_drawable(&w, &h))
        return w;
    if (rcr_video_mode() != RCR_VIDEO_SDL)
        refresh_fb_size();
    return the_window.width;
}

int rcr_screen_height(void)
{
    int w = 0, h = 0;
    if (rcr_video_drawable(&w, &h))
        return h;
    if (rcr_video_mode() != RCR_VIDEO_SDL)
        refresh_fb_size();
    return the_window.height;
}
