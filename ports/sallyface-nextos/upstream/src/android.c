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
#include "sf.h"
#include "media.h"

/* ------------------------------------------------------------ native window */

/* One window, owned by the video layer; ANativeWindow is just a view of it. */
typedef struct {
    int32_t refs;
    int32_t width, height, format;
} sf_window;

static sf_window the_window = { 1, 1280, 720, 1 /* RGBA_8888 */ };
static struct { unsigned short width, height; } fbdev_window = { 1280, 720 };
static int drawable_contract_active;
static int drawable_contract_locked;

static void refresh_fb_size(void)
{
    /* SDL/KMSDRM already measured the drawable that it actually presents.
     * Reading fb0 after that can describe the frontend's stale CRTC instead
     * and used to replace the coherent 4:3 contract with 1280x720. */
    if (drawable_contract_active)
        return;
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

void sf_window_configure(int w, int h, int lock_dimensions)
{
    if (w <= 0 || h <= 0 || w >= 32768 || h >= 32768)
        return;
    the_window.width = w;
    the_window.height = h;
    fbdev_window.width = (unsigned short)w;
    fbdev_window.height = (unsigned short)h;
    drawable_contract_active = 1;
    drawable_contract_locked = lock_dimensions != 0;
}

void sf_window_get_size(int *width, int *height)
{
    refresh_fb_size();
    if (width)
        *width = the_window.width;
    if (height)
        *height = the_window.height;
}

void *sf_window_handle(void) { return &the_window; }

/* Mali's fbdev EGLNativeWindowType is a two-u16 width/height record. */
void *sf_native_window(void)
{
    refresh_fb_size();
    nx_log("fbdev native window %ux%u", fbdev_window.width,
           fbdev_window.height);
    return &fbdev_window;
}

static void *a_window_fromSurface(void *env, void *surface)
{
    (void)env;
    void *media_window = sf_jni_media_window(surface);
    if (media_window) {
        sf_media_window_acquire(media_window);
        return media_window;
    }
    refresh_fb_size();
    __atomic_fetch_add(&the_window.refs, 1, __ATOMIC_RELAXED);
    return &the_window;
}
static void a_window_acquire(void *w)
{
    if (sf_media_is_window(w))
        sf_media_window_acquire(w);
    else if (w)
        __atomic_fetch_add(&((sf_window *)w)->refs, 1, __ATOMIC_RELAXED);
}
static void a_window_release(void *w)
{
    if (sf_media_is_window(w))
        sf_media_window_release(w);
    else if (w)
        __atomic_fetch_sub(&((sf_window *)w)->refs, 1, __ATOMIC_RELAXED);
}
static int32_t a_window_getWidth(void *w)
{
    return sf_media_is_window(w) ? sf_media_window_width(w)
                                 : w ? ((sf_window *)w)->width : 0;
}
static int32_t a_window_getHeight(void *w)
{
    return sf_media_is_window(w) ? sf_media_window_height(w)
                                 : w ? ((sf_window *)w)->height : 0;
}
static int32_t a_window_getFormat(void *w)
{
    return sf_media_is_window(w) ? 1 : w ? ((sf_window *)w)->format : 1;
}
static int32_t a_window_setBuffersGeometry(void *w, int32_t width, int32_t height,
                                           int32_t fmt)
{
    if (sf_media_is_window(w))
        return sf_media_window_set_geometry(w, width, height, fmt);
    sf_window *p = w;
    if (!p)
        return -EINVAL;
    /* The SDL facade has no separate Android buffer for SurfaceFlinger to
     * scale.  In fill mode its EGL surface and the real drawable are one and
     * the same, so accepting another buffer size would immediately make the
     * three size channels disagree.  Native rollback deliberately keeps the
     * old mutable behaviour. */
    if (!drawable_contract_locked) {
        if (width > 0)  p->width = width;
        if (height > 0) p->height = height;
    }
    if (fmt)        p->format = fmt;
    nx_log("ANativeWindow_setBuffersGeometry requested=%dx%d fmt=%d "
           "applied=%dx%d locked=%d", width, height, fmt,
           p->width, p->height, drawable_contract_locked);
    return 0;
}
static int32_t a_window_setBuffersTransform(void *w, int32_t t) { (void)w; (void)t; return 0; }
static int32_t a_window_lock(void *w, void *buf, void *rect) { (void)w; (void)buf; (void)rect; return -ENODEV; }
static int32_t a_window_unlockAndPost(void *w) { (void)w; return -ENODEV; }
static void *a_window_toSurface(void *env, void *window)
{
    (void)env;
    return sf_jni_surface_for_window(window);
}

/* -------------------------------------------------------------------- looper */

/* Unity prepares a looper on its main thread and polls it.  Nothing posts to
 * it here, so a token object plus a poll that reports "nothing ready" is the
 * honest answer; the caller treats that as an idle frame. */
typedef struct { int32_t refs; } sf_looper;
static __thread sf_looper *tls_looper;
static sf_looper main_looper = { 1 };

static void *a_looper_forThread(void) { return tls_looper; }
static void *a_looper_prepare(int opts)
{
    (void)opts;
    if (!tls_looper)
        tls_looper = &main_looper;
    return tls_looper;
}
static void a_looper_acquire(void *l) { if (l) ((sf_looper *)l)->refs++; }
static void a_looper_release(void *l) { if (l) ((sf_looper *)l)->refs--; }
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
static void *a_choreographer_getInstance(void) { return NULL; }

/* ------------------------------------------------------------ AAssetManager
 *
 * O FMOD Studio nativo abre os banks por "file:///android_asset/<nome>.bank" e
 * resolve esse prefixo pela AAssetManager do NDK, que ele pega por
 * AAssetManager_fromJava(env, Context.getAssets()).  Aqui o "APK" e' a pasta
 * assets/ do port em disco, entao cada AAsset e' apenas um arquivo aberto --
 * mesmo contrato, mesma sequencia de chamadas do jogo.
 */

typedef struct {
    FILE *fp;
    long length;
} sf_asset;

static void *a_assetmanager_fromJava(void *env, void *assetmanager)
{
    (void)env; (void)assetmanager;
    /* Um unico gerenciador: a pasta assets/ do port. */
    static int the_manager;
    nx_log("AAssetManager_fromJava -> %s", sf_datadir);
    return &the_manager;
}

static void *a_asset_open(void *mgr, const char *filename, int mode)
{
    (void)mgr; (void)mode;
    if (!filename || !*filename)
        return NULL;
    char path[2048];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", sf_datadir, filename) >=
        sizeof path)
        return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        nx_log("AAssetManager_open(\"%s\") -> ausente", filename);
        return NULL;
    }
    sf_asset *a = calloc(1, sizeof *a);
    if (!a) {
        fclose(fp);
        return NULL;
    }
    a->fp = fp;
    fseek(fp, 0, SEEK_END);
    a->length = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    nx_log("AAssetManager_open(\"%s\") -> %ld bytes", filename, a->length);
    return a;
}

static int a_asset_read(void *asset, void *buf, size_t count)
{
    sf_asset *a = asset;
    if (!a || !buf)
        return -1;
    size_t got = fread(buf, 1, count, a->fp);
    if (got == 0 && ferror(a->fp))
        return -1;
    return (int)got;
}

static long a_asset_seek(void *asset, long offset, int whence)
{
    sf_asset *a = asset;
    if (!a || fseek(a->fp, offset, whence) != 0)
        return -1;
    return ftell(a->fp);
}

static long a_asset_getLength(void *asset)
{
    sf_asset *a = asset;
    return a ? a->length : 0;
}

static long a_asset_getRemainingLength(void *asset)
{
    sf_asset *a = asset;
    return a ? a->length - ftell(a->fp) : 0;
}

static void a_asset_close(void *asset)
{
    sf_asset *a = asset;
    if (!a)
        return;
    fclose(a->fp);
    free(a);
}

static const void *a_asset_getBuffer(void *asset)
{
    (void)asset;
    /* Sem mmap: o FMOD so' usa getBuffer quando ele existe, e cai no
     * read/seek quando nao existe.  Bank de 159 MB nao vai para a RAM. */
    return NULL;
}

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
    A("ANativeWindow_toSurface",            a_window_toSurface),

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

    A("AAssetManager_fromJava",             a_assetmanager_fromJava),
    A("AAssetManager_open",                 a_asset_open),
    A("AAsset_read",                        a_asset_read),
    A("AAsset_seek",                        a_asset_seek),
    A("AAsset_seek64",                      a_asset_seek),
    A("AAsset_getLength",                   a_asset_getLength),
    A("AAsset_getLength64",                 a_asset_getLength),
    A("AAsset_getRemainingLength",          a_asset_getRemainingLength),
    A("AAsset_getRemainingLength64",        a_asset_getRemainingLength),
    A("AAsset_getBuffer",                   a_asset_getBuffer),
    A("AAsset_close",                       a_asset_close),
};

const nx_import *sf_android_table(size_t *n)
{
    static nx_import merged[sizeof tab / sizeof *tab + 96];
    static size_t merged_count;
    if (!merged_count) {
        for (size_t i = 0; i < sizeof tab / sizeof *tab; i++)
            merged[merged_count++] = tab[i];
        size_t media_count = 0;
        const nx_import *media = sf_media_table(&media_count);
        if (merged_count + media_count > sizeof merged / sizeof *merged)
            nx_die("android/media import table overflow");
        for (size_t i = 0; i < media_count; i++)
            merged[merged_count++] = media[i];
    }
    *n = merged_count;
    return merged;
}

void *sf_android_sym(const char *name)
{
    size_t count = 0;
    const nx_import *entries = sf_android_table(&count);
    for (size_t i = 0; i < count; i++)
        if (strcmp(entries[i].name, name) == 0)
            return entries[i].addr;
    return NULL;
}
