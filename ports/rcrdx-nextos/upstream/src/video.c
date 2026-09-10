/*
 * video.c -- who owns the drawable.
 *
 * The port was born on an Amlogic Mali-450 whose only display path is fbdev:
 * there the game's SDL (the Android one, inside liblime) drives EGL directly
 * and Mali's EGLNativeWindowType is a two-u16 width/height record read from
 * /dev/fb0.  That is the RAW mode below and it is unchanged.
 *
 * Every other family we target -- KMSDRM (R36S/ArkOS Mali-G31, X5 G310),
 * Wayland/Panfrost -- cannot be driven that way: raw EGL there draws into a
 * surface that never becomes a page flip.  On those the firmware's own SDL2
 * owns window, context and present, and this file answers the game's EGL calls
 * from it.  That is the SDL mode.
 *
 * The choice is a capability probe, never a device name:
 *
 *     /dev/dri/card0 exists  ->  SDL owns the drawable
 *     otherwise              ->  raw fbdev EGL (the Mali-450 path)
 *
 * and if SDL mode cannot actually bring a window up, we log the failure and
 * fall back to raw once -- the "one logged retry" the porting guide allows.
 * RCR_VIDEO=raw|sdl overrides it for diagnosis only.
 *
 * Nothing here sets SDL_VIDEODRIVER: which backend SDL opens is the firmware's
 * business.  The drawable that comes back from SDL is the authority for size;
 * /sys/class/graphics/fb0/virtual_size is never consulted (it counts the
 * double buffer and reports twice the panel height).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/syscall.h>

#include <SDL2/SDL.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "nx_elf.h"
#include "rcr.h"
#include "video.h"

static int mode = RCR_VIDEO_UNSET;
static SDL_Window *window;
static SDL_GLContext glctx;
static int draw_w, draw_h;
static int portable_provider_active;

/* Which thread the GL context is bound to.  Measured on the R36S (Mali-G31
 * bifrost/GBM blob): releasing the context with SDL_GL_MakeCurrent(win, NULL)
 * and binding it again on the SAME thread faults inside the driver's own
 * eglMakeCurrent.  So the context is created on the thread that will run the
 * game and stays bound there; a bind request from that thread is answered as
 * "already current" instead of round-tripping through the driver. */
static pthread_t ctx_owner;
static int ctx_bound;

/* What the game asked eglChooseConfig for, so the window we bring up matches
 * its request instead of whatever the driver hands back first. */
static struct {
    int red, green, blue, alpha, depth, stencil, samples;
    int seen;
} want = { 8, 8, 8, 8, 16, 8, 0, 0 };

int rcr_video_mode(void) { return mode; }

/* ------------------------------------------------------------------ probe */

static int drm_present(void)
{
    return access("/dev/dri/card0", F_OK) == 0;
}

static int create_window(void)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, want.red);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, want.green);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, want.blue);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, want.alpha);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, want.depth);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, want.stencil);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    /* The panel mode is only the request: a compositor is free to give us
     * something else, and the drawable is the only number that is true. */
    int w = draw_w, h = draw_h;

    window = SDL_CreateWindow("Retro City Rampage DX",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              w > 0 ? w : 640, h > 0 ? h : 480,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP |
                              SDL_WINDOW_SHOWN);
    if (!window) {
        fprintf(stderr, "[rcr] SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }
    return 0;
}

/* Some KMSDRM firmwares expose the matching GPU provider through the
 * unversioned runtime names while their versioned EGL soname is an unusable
 * dispatcher.  Keep the firmware/default choice authoritative, then retry
 * these portable names exactly once.  If either provider was explicitly set
 * by the firmware or user, do not replace either one. */
static int enable_portable_provider_retry(void)
{
    if (getenv("SDL_VIDEO_EGL_DRIVER") || getenv("SDL_VIDEO_GL_DRIVER"))
        return 0;
    if (setenv("SDL_VIDEO_EGL_DRIVER", "libEGL.so", 1) != 0)
        return 0;
    if (setenv("SDL_VIDEO_GL_DRIVER", "libGLESv2.so", 1) != 0) {
        unsetenv("SDL_VIDEO_EGL_DRIVER");
        return 0;
    }
    portable_provider_active = 1;
    fprintf(stderr, "[rcr] retrying portable EGL/GLES provider names\n");
    return 1;
}

static void drop_portable_provider_retry(void)
{
    if (!portable_provider_active)
        return;
    unsetenv("SDL_VIDEO_EGL_DRIVER");
    unsetenv("SDL_VIDEO_GL_DRIVER");
    portable_provider_active = 0;
}

static void discard_failed_drawable(void)
{
    if (glctx) {
        SDL_GL_DeleteContext(glctx);
        glctx = NULL;
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = NULL;
    }
    ctx_bound = 0;
}

/* Report the config we really got, attribute by attribute.  Accepting "the
 * first match" is how a port ends up on RGBX8888 when the engine's shaders
 * need the alpha channel -- the mistake that cost a corrective release
 * elsewhere.  Here the numbers are printed so a field report can name them. */
static void log_exact_config(void)
{
    int r = 0, g = 0, b = 0, a = 0, d = 0, s = 0, db = 0, maj = 0, min = 0;
    SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &r);
    SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &g);
    SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &b);
    SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &a);
    SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &d);
    SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &s);
    SDL_GL_GetAttribute(SDL_GL_DOUBLEBUFFER, &db);
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &maj);
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &min);
    fprintf(stderr,
            "[rcr] EGLConfig exact: R%d G%d B%d A%d depth%d stencil%d "
            "double=%d ES%d.%d (asked R%d G%d B%d A%d depth%d stencil%d)\n",
            r, g, b, a, d, s, db, maj, min,
            want.red, want.green, want.blue, want.alpha, want.depth,
            want.stencil);
    if (want.alpha > 0 && a == 0)
        fprintf(stderr, "[rcr] WARNING: asked for an alpha channel and the "
                        "driver gave none\n");
}

static void log_gl_strings(void)
{
    const GLubyte *(*get_string)(GLenum) = rcr_video_gl_sym("glGetString");
    if (!get_string)
        return;
    const char *vendor = (const char *)get_string(GL_VENDOR);
    const char *renderer = (const char *)get_string(GL_RENDERER);
    const char *version = (const char *)get_string(GL_VERSION);
    const char *glsl = (const char *)get_string(GL_SHADING_LANGUAGE_VERSION);
    fprintf(stderr, "[rcr] GL vendor=%s renderer=%s version=%s glsl=%s\n",
            vendor ? vendor : "?", renderer ? renderer : "?",
            version ? version : "?", glsl ? glsl : "?");
    /* The engine's shaders are `#version 100`.  A desktop GL context would
     * compile none of them, and a black screen would be the only symptom. */
    if (version && !strstr(version, "OpenGL ES"))
        fprintf(stderr, "[rcr] WARNING: this is not an OpenGL ES context (%s); "
                        "the game's GLSL ES shaders will not compile\n",
                version);
}

/* Window and context are brought up TOGETHER, on the thread that will draw --
 * the game's render thread, at its first EGL call.  Three arrangements were
 * measured on the R36S (Mali-G31 bifrost/GBM) and all three fault inside the
 * driver:
 *
 *   - context created on the main thread, bound on the game thread;
 *   - context created on the main thread, released, re-bound on that same
 *     thread;
 *   - window on the main thread, context on the game thread.
 *
 * This blob wants one thread to own the whole chain, so that is what it gets.
 * Everything the port needs before then (the panel size) comes from
 * SDL_GetDesktopDisplayMode, which needs no window. */
static int create_context(void)
{
    glctx = SDL_GL_CreateContext(window);
    if (!glctx) {
        fprintf(stderr, "[rcr] SDL_GL_CreateContext: %s\n", SDL_GetError());
        return -1;
    }
    SDL_GL_GetDrawableSize(window, &draw_w, &draw_h);
    if (draw_w <= 0 || draw_h <= 0)
        SDL_GetWindowSize(window, &draw_w, &draw_h);
    fprintf(stderr, "[rcr] GL context up on tid=%d, drawable %dx%d\n",
            (int)syscall(SYS_gettid), draw_w, draw_h);
    log_exact_config();
    log_gl_strings();
    /* VSync when the firmware offers it; not an error when it does not. */
    int want_vsync = 1;
    const char *vs = getenv("RCR_VSYNC");
    if (vs)
        want_vsync = atoi(vs);
    if (SDL_GL_SetSwapInterval(want_vsync) != 0)
        fprintf(stderr, "[rcr] no vsync available: %s\n", SDL_GetError());
    SDL_ShowCursor(SDL_DISABLE);
    ctx_owner = pthread_self();
    ctx_bound = 1;
    return 0;
}

static int ensure_gl(void);

int rcr_video_init(void)
{
    const char *forced = getenv("RCR_VIDEO");
    int wanted;

    if (forced && strcmp(forced, "raw") == 0)
        wanted = RCR_VIDEO_RAW;
    else if (forced && strcmp(forced, "sdl") == 0)
        wanted = RCR_VIDEO_SDL;
    else
        wanted = drm_present() ? RCR_VIDEO_SDL : RCR_VIDEO_RAW;

    fprintf(stderr, "[rcr] drawable probe: /dev/dri/card0 %s, /dev/fb0 %s -> %s%s\n",
            access("/dev/dri/card0", F_OK) == 0 ? "yes" : "no",
            access("/dev/fb0", F_OK) == 0 ? "yes" : "no",
            wanted == RCR_VIDEO_SDL ? "SDL owns the drawable"
                                   : "raw fbdev EGL",
            forced ? " (forced by RCR_VIDEO)" : "");

    if (wanted == RCR_VIDEO_RAW) {
        mode = RCR_VIDEO_RAW;
        return 0;
    }

    /* Video and audio come up independently on purpose: a dead inherited
     * Pulse must never keep the picture off the screen, and an unusable
     * display must never silence the game.  So this asks for VIDEO only. */
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0 &&
        SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[rcr] SDL video init failed (%s); falling back to raw "
                        "fbdev EGL\n", SDL_GetError());
        mode = RCR_VIDEO_RAW;
        return 0;
    }
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) != 0 || dm.w <= 0 || dm.h <= 0) {
        fprintf(stderr, "[rcr] SDL has no display mode (%s); falling back to raw "
                        "fbdev EGL\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        mode = RCR_VIDEO_RAW;
        return 0;
    }

    mode = RCR_VIDEO_SDL;
    draw_w = dm.w;
    draw_h = dm.h;
    fprintf(stderr, "[rcr] panel %dx%d@%d (SDL video driver: %s)\n",
            dm.w, dm.h, dm.refresh_rate,
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");

    /* Only discover the display here.  Android creates the EGL context from
     * SDLThread, and the G31 KMSDRM driver requires window+context+draw calls
     * to remain on that same thread.  ensure_gl() is therefore first reached
     * by the game's eglChooseConfig/eglCreate* sequence below. */
    rcr_window_set_size(draw_w, draw_h);
    return 0;
}

/* Brings the drawable up once, on whichever thread asks first. */
static int ensure_gl(void)
{
    static int tried;
    if (glctx)
        return 0;
    if (tried)
        return -1;
    tried = 1;
    if (getenv("RCR_NOWIN")) {          /* diagnosis only */
        fprintf(stderr, "[rcr] RCR_NOWIN: refusing to create the window\n");
        return -1;
    }
    if (create_window() != 0 || create_context() != 0) {
        const char *driver = SDL_GetCurrentVideoDriver();
        discard_failed_drawable();

        /* Cheapest repair first, and the one the Amlogic KMSDRM stack needs:
         * its GBM path refuses an RGBA8888 window while RGBX opens fine.  The
         * game asked for alpha through eglChooseConfig, but a window without
         * a destination alpha channel is still the config it can draw into
         * (the present forces alpha to one anyway).  Try that before touching
         * the provider names, which is a much larger change. */
        if (want.alpha > 0 && driver && strcasecmp(driver, "KMSDRM") == 0) {
            int requested_alpha = want.alpha;
            fprintf(stderr, "[rcr] window refused with alpha=%d (%s); "
                            "retrying without a destination alpha channel\n",
                    requested_alpha, SDL_GetError());
            want.alpha = 0;
            SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
            if (create_window() == 0 && create_context() == 0) {
                fprintf(stderr, "[rcr] recovered with alpha=0 window\n");
                rcr_window_set_size(draw_w, draw_h);
                return 0;
            }
            discard_failed_drawable();
            want.alpha = requested_alpha;
            SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, requested_alpha);
        }

        if (!driver || strcasecmp(driver, "KMSDRM") != 0 ||
            !enable_portable_provider_retry()) {
            fprintf(stderr, "[rcr] SDL could not bring a GL window up on this "
                            "firmware; the port cannot draw\n");
            return -1;
        }

        /* SDL caches its EGL/GL provider in the video subsystem.  Recreate
         * that subsystem after setting the fallback names, on this same game
         * thread, before recreating the window and context. */
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0 ||
            create_window() != 0 || create_context() != 0) {
            fprintf(stderr, "[rcr] portable EGL/GLES provider retry failed: "
                            "%s\n", SDL_GetError());
            discard_failed_drawable();
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            drop_portable_provider_retry();
            return -1;
        }
        fprintf(stderr, "[rcr] recovered with portable EGL/GLES provider "
                        "names\n");
    }
    rcr_window_set_size(draw_w, draw_h);
    return 0;
}

int rcr_video_drawable(int *w, int *h)
{
    if (mode != RCR_VIDEO_SDL || !window)
        return 0;
    int cw = 0, ch = 0;
    SDL_GL_GetDrawableSize(window, &cw, &ch);
    if (cw > 0 && ch > 0) {
        draw_w = cw;
        draw_h = ch;
    }
    if (w) *w = draw_w;
    if (h) *h = draw_h;
    return draw_w > 0 && draw_h > 0;
}

void rcr_video_shutdown(void)
{
    /* Deliberately minimal.  Tearing a Mali context down on the way out is how
     * the other NextOS ports deadlocked, and unbinding it is what faults on
     * the G31 blob; the process is about to _exit and the kernel reclaims the
     * DRM master and the console either way. */
    if (mode == RCR_VIDEO_SDL)
        fprintf(stderr, "[rcr] video: leaving the drawable to the kernel\n");
}

/* --------------------------------------------------- the game's EGL, on SDL */

/* Opaque tokens.  The game only ever passes these back to us. */
#define FAKE_DPY   ((EGLDisplay)(uintptr_t)0x7d000001)
#define FAKE_CFG   ((EGLConfig)(uintptr_t)0x7d000002)
#define FAKE_SURF  ((EGLSurface)(uintptr_t)0x7d000003)
#define FAKE_CTX   ((EGLContext)(uintptr_t)0x7d000004)

void *rcr_video_gl_sym(const char *name)
{
    static void *provider_handle;
    static void *handles[4];
    static const char *const cands[] = {
        "libGLESv2.so.2", "libGLESv2.so", "libGLESv1_CM.so.1", "libmali.so",
    };
    if (!name || name[0] != 'g' || name[1] != 'l')
        return NULL;

    /* In SDL-owned mode the provider selected by SDL is authoritative for
     * the guest as well.  This is especially important after the KMSDRM
     * portable-name recovery: resolving guest GL from the versioned
     * dispatcher would split one context across two provider objects. */
    if (mode == RCR_VIDEO_SDL) {
        const char *provider = getenv("SDL_VIDEO_GL_DRIVER");
        if (provider && *provider) {
            if (!provider_handle)
                provider_handle = dlopen(provider, RTLD_NOW | RTLD_GLOBAL);
            if (provider_handle) {
                void *f = dlsym(provider_handle, name);
                if (f)
                    return f;
            }
        }
    }
    for (size_t i = 0; i < sizeof cands / sizeof *cands; i++) {
        if (!handles[i])
            handles[i] = dlopen(cands[i], RTLD_NOW | RTLD_GLOBAL);
        if (!handles[i])
            continue;
        void *f = dlsym(handles[i], name);
        if (f)
            return f;
    }
    if (strcmp(name, "glBlendFuncSeparateOES") == 0)
        return rcr_video_gl_sym("glBlendFuncSeparate");
    if (strcmp(name, "glBlendEquationOES") == 0)
        return rcr_video_gl_sym("glBlendEquation");
    return NULL;
}

static EGLDisplay v_getDisplay(EGLNativeDisplayType d)
{
    (void)d;
    return FAKE_DPY;
}

static EGLBoolean v_initialize(EGLDisplay dpy, EGLint *maj, EGLint *min)
{
    (void)dpy;
    if (maj) *maj = 1;
    if (min) *min = 4;
    return EGL_TRUE;
}

static EGLBoolean v_true_1(EGLDisplay dpy) { (void)dpy; return EGL_TRUE; }
static EGLBoolean v_bindAPI(EGLenum api) { (void)api; return EGL_TRUE; }
static EGLenum v_queryAPI(void) { return EGL_OPENGL_ES_API; }
static EGLint v_getError(void) { return EGL_SUCCESS; }

static void note_attribs(const EGLint *a)
{
    if (!a)
        return;
    for (; a[0] != EGL_NONE; a += 2) {
        switch (a[0]) {
        case EGL_RED_SIZE:     want.red = a[1];     want.seen = 1; break;
        case EGL_GREEN_SIZE:   want.green = a[1];   want.seen = 1; break;
        case EGL_BLUE_SIZE:    want.blue = a[1];    want.seen = 1; break;
        case EGL_ALPHA_SIZE:   want.alpha = a[1];   want.seen = 1; break;
        case EGL_DEPTH_SIZE:   want.depth = a[1];   want.seen = 1; break;
        case EGL_STENCIL_SIZE: want.stencil = a[1]; want.seen = 1; break;
        case EGL_SAMPLES:      want.samples = a[1]; want.seen = 1; break;
        default: break;
        }
    }
}

static EGLBoolean v_chooseConfig(EGLDisplay dpy, const EGLint *attrib,
                                 EGLConfig *configs, EGLint size, EGLint *num)
{
    (void)dpy;
    note_attribs(attrib);
    ensure_gl();
    nx_log("eglChooseConfig: game asked R%d G%d B%d A%d depth%d stencil%d; "
           "answering with the SDL-owned config", want.red, want.green,
           want.blue, want.alpha, want.depth, want.stencil);
    if (configs && size > 0)
        configs[0] = FAKE_CFG;
    if (num)
        *num = (configs && size > 0) ? 1 : 1;
    return EGL_TRUE;
}

static EGLBoolean v_getConfigs(EGLDisplay dpy, EGLConfig *configs, EGLint size,
                               EGLint *num)
{
    (void)dpy;
    if (configs && size > 0)
        configs[0] = FAKE_CFG;
    if (num)
        *num = 1;
    return EGL_TRUE;
}

static EGLBoolean v_getConfigAttrib(EGLDisplay dpy, EGLConfig cfg,
                                    EGLint attr, EGLint *value)
{
    (void)dpy; (void)cfg;
    if (!value)
        return EGL_FALSE;
    int v = 0;
    switch (attr) {
    case EGL_RED_SIZE:     SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &v); break;
    case EGL_GREEN_SIZE:   SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &v); break;
    case EGL_BLUE_SIZE:    SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &v); break;
    case EGL_ALPHA_SIZE:   SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &v); break;
    case EGL_DEPTH_SIZE:   SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &v); break;
    case EGL_STENCIL_SIZE: SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &v); break;
    case EGL_BUFFER_SIZE: {
        int r = 0, g = 0, b = 0, a = 0;
        SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &r);
        SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &g);
        SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &b);
        SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &a);
        v = r + g + b + a;
        break;
    }
    case EGL_SAMPLES:          v = 0; break;
    case EGL_SAMPLE_BUFFERS:   v = 0; break;
    case EGL_CONFIG_ID:        v = 1; break;
    case EGL_NATIVE_VISUAL_ID: v = 0; break;
    case EGL_SURFACE_TYPE:     v = EGL_WINDOW_BIT; break;
    case EGL_RENDERABLE_TYPE:  v = EGL_OPENGL_ES2_BIT; break;
    case EGL_LEVEL:            v = 0; break;
    default:                   v = 0; break;
    }
    *value = v;
    return EGL_TRUE;
}

static EGLSurface v_createWindowSurface(EGLDisplay dpy, EGLConfig cfg,
                                        EGLNativeWindowType win,
                                        const EGLint *attrib)
{
    (void)dpy; (void)cfg; (void)win; (void)attrib;
    if (ensure_gl() != 0)
        return EGL_NO_SURFACE;
    nx_log("eglCreateWindowSurface -> the SDL-owned window (%dx%d)",
           draw_w, draw_h);
    return FAKE_SURF;
}

static EGLSurface v_createPbufferSurface(EGLDisplay dpy, EGLConfig cfg,
                                         const EGLint *attrib)
{
    (void)dpy; (void)cfg; (void)attrib;
    nx_log("eglCreatePbufferSurface is not offered in SDL-owned mode");
    return EGL_NO_SURFACE;
}

static EGLBoolean v_destroySurface(EGLDisplay dpy, EGLSurface s)
{
    (void)dpy; (void)s;
    return EGL_TRUE;
}

static EGLBoolean v_querySurface(EGLDisplay dpy, EGLSurface s, EGLint attr,
                                 EGLint *value)
{
    (void)dpy; (void)s;
    if (!value)
        return EGL_FALSE;
    int w = draw_w, h = draw_h;
    rcr_video_drawable(&w, &h);
    switch (attr) {
    case EGL_WIDTH:  *value = w; break;
    case EGL_HEIGHT: *value = h; break;
    case EGL_CONFIG_ID: *value = 1; break;
    default: *value = 0; break;
    }
    return EGL_TRUE;
}

static EGLContext v_createContext(EGLDisplay dpy, EGLConfig cfg,
                                  EGLContext share, const EGLint *attrib)
{
    (void)dpy; (void)cfg; (void)share; (void)attrib;
    if (ensure_gl() != 0)
        return EGL_NO_CONTEXT;
    nx_log("eglCreateContext -> the SDL-owned GLES2 context");
    return FAKE_CTX;
}

static EGLBoolean v_destroyContext(EGLDisplay dpy, EGLContext ctx)
{
    (void)dpy; (void)ctx;
    /* Not destroyed here: the Mali driver's teardown is the deadlock the other
     * NextOS ports met on the way out, and the process exits right after. */
    return EGL_TRUE;
}

static EGLBoolean v_makeCurrent(EGLDisplay dpy, EGLSurface draw,
                                EGLSurface read, EGLContext ctx)
{
    (void)dpy; (void)read;
    if (ensure_gl() != 0)
        return EGL_FALSE;
    if (ctx == EGL_NO_CONTEXT || draw == EGL_NO_SURFACE) {
        /* The game unbinds on its way out.  Honouring that literally faults
         * inside the G31 driver, and there is nothing to gain: the context
         * dies with the process a moment later. */
        nx_log("eglMakeCurrent(release) ignored -- the context stays bound");
        return EGL_TRUE;
    }
    if (ctx_bound && pthread_equal(ctx_owner, pthread_self())) {
        nx_log("eglMakeCurrent: already current on this thread");
        return EGL_TRUE;
    }
    if (SDL_GL_MakeCurrent(window, glctx) != 0) {
        fprintf(stderr, "[rcr] SDL_GL_MakeCurrent: %s\n", SDL_GetError());
        return EGL_FALSE;
    }
    ctx_owner = pthread_self();
    ctx_bound = 1;
    return EGL_TRUE;
}

static EGLContext v_getCurrentContext(void) { return FAKE_CTX; }
static EGLSurface v_getCurrentSurface(EGLint which) { (void)which; return FAKE_SURF; }
static EGLDisplay v_getCurrentDisplay(void) { return FAKE_DPY; }

static EGLBoolean v_queryContext(EGLDisplay dpy, EGLContext ctx, EGLint attr,
                                 EGLint *value)
{
    (void)dpy; (void)ctx;
    if (!value)
        return EGL_FALSE;
    switch (attr) {
    case EGL_CONTEXT_CLIENT_TYPE:    *value = EGL_OPENGL_ES_API; break;
    case EGL_CONTEXT_CLIENT_VERSION: *value = 2; break;
    case EGL_CONFIG_ID:              *value = 1; break;
    default:                         *value = 0; break;
    }
    return EGL_TRUE;
}

static const char *v_queryString(EGLDisplay dpy, EGLint name)
{
    (void)dpy;
    switch (name) {
    case EGL_VENDOR:      return "NextOS (SDL-owned)";
    case EGL_VERSION:     return "1.4";
    case EGL_CLIENT_APIS: return "OpenGL_ES";
    case EGL_EXTENSIONS:  return "";
    default:              return "";
    }
}

static EGLBoolean v_surfaceAttrib(EGLDisplay dpy, EGLSurface s, EGLint a,
                                  EGLint v)
{
    (void)dpy; (void)s; (void)a; (void)v;
    return EGL_TRUE;
}

static EGLBoolean v_swapInterval(EGLDisplay dpy, EGLint interval)
{
    (void)dpy;
    return SDL_GL_SetSwapInterval(interval) == 0 ? EGL_TRUE : EGL_FALSE;
}

static EGLBoolean v_true_void(void) { return EGL_TRUE; }

/* The present itself.  egl.c keeps the frame accounting and the finger; this
 * is only the hand-off to whoever owns the window. */
EGLBoolean rcr_video_present(void)
{
    if (!window)
        return EGL_FALSE;
    SDL_GL_SwapWindow(window);
    return EGL_TRUE;
}

#define V(n, f) { n, (void *)(uintptr_t)(f) }

static const nx_import sdl_egl[] = {
    V("eglGetDisplay",           v_getDisplay),
    V("eglInitialize",           v_initialize),
    V("eglTerminate",            v_true_1),
    V("eglBindAPI",              v_bindAPI),
    V("eglQueryAPI",             v_queryAPI),
    V("eglGetError",             v_getError),
    V("eglChooseConfig",         v_chooseConfig),
    V("eglGetConfigs",           v_getConfigs),
    V("eglGetConfigAttrib",      v_getConfigAttrib),
    V("eglCreateWindowSurface",  v_createWindowSurface),
    V("eglCreatePbufferSurface", v_createPbufferSurface),
    V("eglDestroySurface",       v_destroySurface),
    V("eglQuerySurface",         v_querySurface),
    V("eglCreateContext",        v_createContext),
    V("eglDestroyContext",       v_destroyContext),
    V("eglMakeCurrent",          v_makeCurrent),
    V("eglGetCurrentContext",    v_getCurrentContext),
    V("eglGetCurrentSurface",    v_getCurrentSurface),
    V("eglGetCurrentDisplay",    v_getCurrentDisplay),
    V("eglQueryContext",         v_queryContext),
    V("eglQueryString",          v_queryString),
    V("eglSurfaceAttrib",        v_surfaceAttrib),
    V("eglSwapInterval",         v_swapInterval),
    V("eglReleaseThread",        v_true_void),
    V("eglWaitClient",           v_true_void),
    V("eglWaitGL",               v_true_void),
    V("eglWaitNative",           v_true_void),
};

const nx_import *rcr_video_egl_table(size_t *n)
{
    *n = sizeof sdl_egl / sizeof *sdl_egl;
    return sdl_egl;
}
