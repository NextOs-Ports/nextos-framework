/* nx_egl.c — shim libEGL.so: forwards to the Mali blob, lies about GLES3.
 * Godot (glad) dlopens libEGL.so and pulls everything from here. */
#define _GNU_SOURCE
#include "nx_shim.h"
#include "nx_egl_sdl.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern void nx_log(const char *fmt, ...);
extern void *nx_mali_egl_sym(const char *name);
extern void *nx_mali_sym(const char *name);

static void *nx_shim_gles_handle(void)
{
    static void *handle;
    static int attempted;
    if (!attempted) {
        attempted = 1;
        const char *defaults = getenv("NX_TEARSCAPE_DEFAULTS");
        const char *library = getenv("NX_TEARSCAPE_GLES2_LIBRARY");
        if (defaults && strcmp(defaults, "1") == 0 &&
            library && library[0] == '/')
            handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
        if (!handle)
            handle = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    }
    return handle;
}

#define FWD(ret, name, sdl_name, args, call) \
    ret name args { \
        if (nx_sdl_egl_enabled()) return sdl_name call; \
        static ret (*fn) args; \
        if (!fn) fn = (ret (*) args)nx_mali_egl_sym(#name); \
        return fn call; \
    }

FWD(EGLDisplay, eglGetDisplay, nx_sdl_egl_get_display,
     (EGLNativeDisplayType d), (d))
FWD(EGLBoolean, eglInitialize, nx_sdl_egl_initialize,
     (EGLDisplay d, EGLint *ma, EGLint *mi), (d, ma, mi))
FWD(EGLBoolean, eglTerminate, nx_sdl_egl_terminate,
     (EGLDisplay d), (d))
FWD(EGLBoolean, eglBindAPI, nx_sdl_egl_bind_api,
     (EGLenum api), (api))
FWD(EGLenum, eglQueryAPI, nx_sdl_egl_query_api, (void), ())
FWD(EGLBoolean, eglGetConfigs, nx_sdl_egl_get_configs,
     (EGLDisplay d, EGLConfig *c, EGLint sz, EGLint *n), (d, c, sz, n))
FWD(EGLBoolean, eglGetConfigAttrib, nx_sdl_egl_get_config_attrib,
     (EGLDisplay d, EGLConfig c, EGLint a, EGLint *v), (d, c, a, v))
EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType w, const EGLint *at)
{
    if (nx_sdl_egl_enabled())
        return nx_sdl_egl_create_window_surface(d, c, w, at);
    static EGLSurface (*fn)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
    if (!fn) fn = nx_mali_egl_sym("eglCreateWindowSurface");
    nx_log("eglCreateWindowSurface(win=%p)", (void *)w);
    EGLSurface r = fn(d, c, w, at);
    nx_log("eglCreateWindowSurface -> %p (err 0x%x)", r, eglGetError());
    return r;
}
FWD(EGLSurface, eglCreatePbufferSurface, nx_sdl_egl_create_pbuffer_surface,
     (EGLDisplay d, EGLConfig c, const EGLint *at), (d, c, at))
FWD(EGLBoolean, eglDestroySurface, nx_sdl_egl_destroy_surface,
     (EGLDisplay d, EGLSurface s), (d, s))
FWD(EGLBoolean, eglDestroyContext, nx_sdl_egl_destroy_context,
     (EGLDisplay d, EGLContext c), (d, c))
EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c)
{
    if (nx_sdl_egl_enabled())
        return nx_sdl_egl_make_current(d, dr, rd, c);
    static EGLBoolean (*fn)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    if (!fn) fn = nx_mali_egl_sym("eglMakeCurrent");
    nx_log("eglMakeCurrent(draw=%p ctx=%p)", dr, c);
    EGLBoolean r = fn(d, dr, rd, c);
    nx_log("eglMakeCurrent -> %d (err 0x%x)", r, eglGetError());
    return r;
}
EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s)
{
    static EGLBoolean (*fn)(EGLDisplay, EGLSurface);
    static unsigned long count;
    static void (*get_stats)(NxDrawStats *);
    static unsigned heavy_logs;
    static unsigned long perf_start_frame;
    static struct timespec perf_start;
    static int perf_enabled = -1;
    if (!get_stats) {
        void *h = nx_shim_gles_handle();
        if (h) get_stats = (void (*)(NxDrawStats *))dlsym(h, "nx_get_and_reset_draw_stats");
    }
    EGLBoolean r;
    if (nx_sdl_egl_enabled()) {
        r = nx_sdl_egl_swap_buffers(d, s);
    } else {
        if (!fn) fn = nx_mali_egl_sym("eglSwapBuffers");
        r = fn(d, s);
    }
    ++count;
    NxDrawStats st = {0};
    if (get_stats)
        get_stats(&st);
    if (count <= 20 || count % 300 == 0 ||
        (st.physical_draws >= 500 && heavy_logs++ < 12)) {
        nx_log("swap #%lu draws=%lu api=%lu inst_calls=%lu inst=%lu max=%lu vertices=%lu ae=%lu/%lu batch=%lu/%lu fallback=%lu hist=%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu",
               count, st.physical_draws, st.api_draws, st.instanced_calls,
               st.instanced_instances, st.instanced_max, st.instanced_vertices,
               st.instanced_arrays, st.instanced_elements,
               st.batched_calls, st.batched_vertices, st.batch_fallbacks,
               st.instanced_hist[0], st.instanced_hist[1],
               st.instanced_hist[2], st.instanced_hist[3],
               st.instanced_hist[4], st.instanced_hist[5],
               st.instanced_hist[6], st.instanced_hist[7]);
    }
    if (perf_enabled < 0)
        perf_enabled = getenv("NX_PERF_LOG") ? 1 : 0;
    if (perf_enabled) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!perf_start.tv_sec && !perf_start.tv_nsec) {
            perf_start = now;
            perf_start_frame = count;
        } else {
            double seconds = (double)(now.tv_sec - perf_start.tv_sec) +
                             (double)(now.tv_nsec - perf_start.tv_nsec) / 1000000000.0;
            if (seconds >= 2.0) {
                nx_log("perf frames=%lu seconds=%.3f fps=%.2f",
                       count - perf_start_frame, seconds,
                       (double)(count - perf_start_frame) / seconds);
                perf_start = now;
                perf_start_frame = count;
            }
        }
    }
    return r;
}
FWD(EGLBoolean, eglSwapInterval, nx_sdl_egl_swap_interval,
     (EGLDisplay d, EGLint i), (d, i))
FWD(EGLint, eglGetError, nx_sdl_egl_get_error, (void), ())
FWD(EGLBoolean, eglWaitGL, nx_sdl_egl_wait_gl, (void), ())
FWD(EGLBoolean, eglWaitNative, nx_sdl_egl_wait_native,
     (EGLint e), (e))
FWD(EGLBoolean, eglReleaseThread, nx_sdl_egl_release_thread, (void), ())
FWD(EGLContext, eglGetCurrentContext, nx_sdl_egl_get_current_context,
     (void), ())
FWD(EGLSurface, eglGetCurrentSurface, nx_sdl_egl_get_current_surface,
     (EGLint w), (w))
FWD(EGLDisplay, eglGetCurrentDisplay, nx_sdl_egl_get_current_display,
     (void), ())
FWD(EGLBoolean, eglQuerySurface, nx_sdl_egl_query_surface,
     (EGLDisplay d, EGLSurface s, EGLint a, EGLint *v), (d, s, a, v))
FWD(EGLBoolean, eglQueryContext, nx_sdl_egl_query_context,
     (EGLDisplay d, EGLContext c, EGLint a, EGLint *v), (d, c, a, v))
FWD(EGLBoolean, eglSurfaceAttrib, nx_sdl_egl_surface_attrib,
     (EGLDisplay d, EGLSurface s, EGLint a, EGLint v), (d, s, a, v))

const char *eglQueryString(EGLDisplay d, EGLint name)
{
    if (nx_sdl_egl_enabled())
        return nx_sdl_egl_query_string(d, name);
    static const char *(*fn)(EGLDisplay, EGLint);
    if (!fn) fn = nx_mali_egl_sym("eglQueryString");
    /* Hide client extensions: the blob advertises EGL_EXT_platform_base but
     * its eglGetPlatformDisplayEXT returns EGL_NO_DISPLAY. Forcing glad down
     * the plain eglGetDisplay() fallback is the working path. */
    if (d == EGL_NO_DISPLAY && name == EGL_EXTENSIONS) {
        nx_log("eglQueryString(client EXTENSIONS) -> hidden");
        return "EGL_NX_platform_fbdev";
    }
    const char *s = fn(d, name);
    nx_log("eglQueryString(%d) = %s", name, s ? s : "(null)");
    return s;
}

/* Some loaders resolve this by name regardless; make it work. */
EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void *native, const EGLint *attribs)
{
    (void)platform; (void)native; (void)attribs;
    nx_log("eglGetPlatformDisplayEXT -> eglGetDisplay(DEFAULT)");
    return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native, const EGLAttrib *attribs)
{
    (void)platform; (void)native; (void)attribs;
    nx_log("eglGetPlatformDisplay -> eglGetDisplay(DEFAULT)");
    return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

/* Rewrite ES3 bits/attribs down to ES2 before the blob sees them. */
static void rewrite_attribs(const EGLint *in, EGLint *out, int max, int ctx)
{
    int i = 0;
    if (in) {
        for (; i < max - 2 && in[i] != EGL_NONE; i += 2) {
            EGLint k = in[i], v = in[i + 1];
            if (!ctx && (k == EGL_RENDERABLE_TYPE || k == EGL_CONFORMANT) &&
                (v & 0x40) /* EGL_OPENGL_ES3_BIT */) {
                v = (v & ~0x40) | EGL_OPENGL_ES2_BIT;
                nx_log("eglChooseConfig: ES3_BIT -> ES2_BIT (attr 0x%x)", k);
            }
            if (ctx && (k == EGL_CONTEXT_CLIENT_VERSION || k == 0x3098) && v >= 3) {
                nx_log("eglCreateContext: client version %d -> 2", v);
                v = 2;
            }
            if (ctx && k == 0x30FB /* EGL_CONTEXT_MINOR_VERSION */) {
                v = 0;
            }
            out[i] = k;
            out[i + 1] = v;
        }
    }
    out[i] = EGL_NONE;
}

EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *at, EGLConfig *cfg,
                           EGLint sz, EGLint *n)
{
    EGLint tmp[128];
    rewrite_attribs(at, tmp, 128, 0);
    if (nx_sdl_egl_enabled())
        return nx_sdl_egl_choose_config(d, tmp, cfg, sz, n);
    static EGLBoolean (*fn)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    if (!fn) fn = nx_mali_egl_sym("eglChooseConfig");
    EGLBoolean r = fn(d, tmp, cfg, sz, n);
    nx_log("eglChooseConfig -> %d (n=%d)", r, n ? *n : -1);
    return r;
}

EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share,
                            const EGLint *at)
{
    EGLint tmp[128];
    rewrite_attribs(at, tmp, 128, 1);
    if (nx_sdl_egl_enabled())
        return nx_sdl_egl_create_context(d, c, share, tmp);
    static EGLContext (*fn)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    if (!fn) fn = nx_mali_egl_sym("eglCreateContext");
    EGLContext r = fn(d, c, share, tmp);
    nx_log("eglCreateContext -> %p (err 0x%x)", r, eglGetError());
    return r;
}

/* gl* names resolve to OUR shim first (we live in the same .so set);
 * everything else falls back to the blob. */
void (*eglGetProcAddress(const char *name))(void)
{
    /* The port adapter publishes the exact shim path. This keeps host/setup
     * libraries isolated and avoids relying on firmware LD_LIBRARY_PATH order. */
    void *self_gles = nx_shim_gles_handle();
    if (self_gles) {
        void *p = dlsym(self_gles, name);
        if (p) return (void (*)(void))p;
    }
    if (nx_sdl_egl_enabled()) {
        void *p = nx_sdl_egl_get_proc_address(name);
        if (p) return (void (*)(void))p;
    }
    void *p = dlsym(RTLD_DEFAULT, name);
    if (p) return (void (*)(void))p;
    static void (*(*fn)(const char *))(void);
    if (!fn) fn = nx_mali_egl_sym("eglGetProcAddress");
    return fn ? fn(name) : NULL;
}
