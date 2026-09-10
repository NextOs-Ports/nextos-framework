/*
 * egl.c -- EGL and GLES for liblime's SDL video driver on the Mali-450.
 *
 * Tightrope Theatre is a GLES2 build: its shaders are `#version 100`/`120`,
 * it links libGLESv1_CM alongside libGLESv2, and the Utgard Mali-450 answers
 * both natively.  There is no ES3 to downgrade and no shader translation to
 * do, so this layer only has two jobs:
 *
 *   - hand the driver the framebuffer's own native window instead of the
 *     ANativeWindow shim SDL passes down, because on fbdev Mali expects its
 *     own two-u16 width/height record;
 *   - forward every other EGL and GL name to the system driver.
 *
 * We never set SDL_VIDEODRIVER and never pick a display: EGL_DEFAULT_DISPLAY
 * is what the fbdev driver wants.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/syscall.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "nx_elf.h"
#include "rcr.h"
#include "video.h"
#include "probe_ring.h"
#include "nx_frameprobe.h"
#include "nxgl_frame_proof_adapter.h"

static void *libegl;

static void *sys(const char *n)
{
    if (!libegl) {
        libegl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!libegl)
            libegl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
        if (!libegl)
            nx_die("cannot open the system libEGL: %s", dlerror());
    }
    void *f = dlsym(libegl, n);
    if (!f)
        nx_log("system EGL has no %s", n);
    return f;
}

/* The GL entry points come from the driver blob.  liblime imports both the
 * ES2 and the ES1 names, and on this image every libGLES* soname resolves into
 * the same Mali blob, so one handle answers for all of them. */
static void *libgl[3];

static void *gl_raw(const char *name)
{
    static const char *const cands[] = {
        "libGLESv2.so.2", "libGLESv1_CM.so.1", "libmali.so",
    };
    if (!name || name[0] != 'g' || name[1] != 'l')
        return NULL;
    for (size_t i = 0; i < sizeof cands / sizeof *cands; i++) {
        if (!libgl[i])
            libgl[i] = dlopen(cands[i], RTLD_NOW | RTLD_GLOBAL);
        if (!libgl[i])
            continue;
        void *f = dlsym(libgl[i], name);
        if (f)
            return f;
    }
    /* The OES spelling of an entry point that became core in GLES2: the Utgard
     * blob exports only the core name, and the game imports the extension
     * name.  Same function, same arguments -- this is a rename, not a stub. */
    if (strcmp(name, "glBlendFuncSeparateOES") == 0)
        return gl_raw("glBlendFuncSeparate");
    if (strcmp(name, "glBlendEquationOES") == 0)
        return gl_raw("glBlendEquation");
    return NULL;
}


/* --- the calls we rewrite ------------------------------------------------ */

static EGLSurface (*p_eglCreateWindowSurface)(EGLDisplay, EGLConfig,
                                              EGLNativeWindowType,
                                              const EGLint *);

static EGLSurface my_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig cfg,
                                            EGLNativeWindowType win,
                                            const EGLint *attrib)
{
    if (!p_eglCreateWindowSurface)
        p_eglCreateWindowSurface = sys("eglCreateWindowSurface");
    EGLNativeWindowType real = (EGLNativeWindowType)rcr_native_window();
    nx_log("eglCreateWindowSurface: SDL win=%p -> fbdev native %p",
           (void *)win, (void *)real);
    return p_eglCreateWindowSurface(dpy, cfg, real, attrib);
}

static EGLDisplay (*p_eglGetDisplay)(EGLNativeDisplayType);

static EGLDisplay my_eglGetDisplay(EGLNativeDisplayType d)
{
    (void)d;
    if (!p_eglGetDisplay)
        p_eglGetDisplay = sys("eglGetDisplay");
    return p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

static EGLBoolean (*p_eglSwapBuffers)(EGLDisplay, EGLSurface);
static unsigned long swaps;

static EGLBoolean my_eglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
    if (swaps == 0) {
        fprintf(stderr, "[rcr] first frame presented\n");
        /* Quem apresenta o frame e a thread que trava: publicar o tid permite
         * amostrar o PC dela de fora (/proc/<pid>/task/<tid>/syscall) e dizer
         * ONDE o jogo esta parado, em vez de adivinhar. */
        fprintf(stderr, "[rcr] render tid=%d\n", (int)syscall(SYS_gettid));
    }
    /* The in-process witness: it reads the frame the driver is about to
     * present, which a framebuffer capture cannot do reliably.  Off unless
     * NX_FRAMEPROBE=1, and never on in a release measurement of fps. */
    nx_frameprobe_before_swap();
    /* Framework frame proof: three samples over the whole run, so the stall
     * the diagnostic probe warns about is paid three times, not per second. */
    if (swaps == 300 || swaps == 600 || swaps == 900) {
        nxgl_frame_proof_sample(0, 0);
        nxgl_frame_proof_publish();
    }
    if (rcr_video_mode() != RCR_VIDEO_SDL && !p_eglSwapBuffers)
        p_eglSwapBuffers = sys("eglSwapBuffers");

    /* Split the frame in two so a slow frame says where it was slow: inside
     * the driver's present, or in everything the game did before it. */
    static struct timespec prev_swap_end;
    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    EGLBoolean r = rcr_video_mode() == RCR_VIDEO_SDL
        ? (EGLBoolean)rcr_video_present()
        : p_eglSwapBuffers(display, surface);
    clock_gettime(CLOCK_MONOTONIC, &after);
    swaps++;
    {
        double present_ms = (after.tv_sec - before.tv_sec) * 1e3 +
                            (after.tv_nsec - before.tv_nsec) / 1e6;
        if (present_ms > 1000.0)
            fprintf(stderr, "[rcr] the present of frame %lu took %.0f ms -- the "
                            "display did not page-flip\n", swaps, present_ms);
    }
    if (nx_verbose && swaps % 10 == 0) {
        double present = (after.tv_sec - before.tv_sec) * 1e3 +
                         (after.tv_nsec - before.tv_nsec) / 1e6;
        double frame = prev_swap_end.tv_sec
            ? (after.tv_sec - prev_swap_end.tv_sec) * 1e3 +
              (after.tv_nsec - prev_swap_end.tv_nsec) / 1e6
            : 0.0;
        static struct timespec first;
        if (!first.tv_sec)
            first = after;
        double run = (after.tv_sec - first.tv_sec) +
                     (after.tv_nsec - first.tv_nsec) / 1e9;
        nx_log("frame %lu at %.1fs (%.1f fps avg): %.1f ms total, "
               "%.1f ms in the driver's present",
               swaps, run, run > 0 ? swaps / run : 0.0, frame, present);
    }
    /* Carimbo do frame no anel, e o gatilho: um frame passando de 500ms e o
     * fenomeno raro que queremos: congela o anel preservando os ~2s
     * anteriores, para nao depender de alguem estar olhando na hora.
     * RCR_PROBE_KEEP=1 deixa o anel correndo (para medir varios). */
    {
        double dt = prev_swap_end.tv_sec
            ? (after.tv_sec - prev_swap_end.tv_sec) * 1e3 +
              (after.tv_nsec - prev_swap_end.tv_nsec) / 1e6
            : 0.0;
        rcr_probe_note(RCR_PROBE_SWAP, (uint32_t)swaps, (uint32_t)dt);
        static int keep = -1;
        if (keep < 0)
            keep = getenv("RCR_PROBE_KEEP") ? 1 : 0;
        if (!keep && dt > 500.0 && swaps > 120)
            rcr_probe_freeze();
    }
    prev_swap_end = after;
    if (rcr_max_frames > 0 && (long)swaps >= rcr_max_frames) {
        fprintf(stderr, "[rcr] frame limit reached (%lu swaps)\n", swaps);
        fflush(stderr);
        _exit(0);
    }
    return r;
}

unsigned long rcr_egl_swap_count(void) { return swaps; }

/* SDL resolves its EGL entry points through eglGetProcAddress as well as
 * dlsym, and the driver's version would hand back the raw eglSwapBuffers --
 * bypassing the swap hook and, with it, the frame probe.  Answer from our own
 * table first, exactly as the loader does for the PLT. */
static void *(*p_eglGetProcAddress)(const char *);

static void *my_eglGetProcAddress(const char *name)
{
    void *ours = name ? rcr_egl_sym(name) : NULL;
    if (ours)
        return ours;
    if (!p_eglGetProcAddress)
        p_eglGetProcAddress = sys("eglGetProcAddress");
    return p_eglGetProcAddress ? p_eglGetProcAddress(name) : NULL;
}

/* --- everything else is a straight forward ------------------------------ */

static const char *const passthrough[] = {
    "eglInitialize", "eglTerminate", "eglChooseConfig", "eglGetConfigs",
    "eglGetConfigAttrib", "eglCreateContext",
    "eglCreatePbufferSurface", "eglDestroySurface", "eglQuerySurface",
    "eglBindAPI", "eglQueryAPI", "eglDestroyContext", "eglMakeCurrent",
    "eglGetCurrentContext", "eglGetCurrentSurface", "eglGetCurrentDisplay",
    "eglQueryContext", "eglGetError",
    "eglQueryString", "eglSurfaceAttrib", "eglSwapInterval",
    "eglReleaseThread", "eglWaitClient", "eglWaitGL", "eglWaitNative",
    "eglCreateImageKHR", "eglDestroyImageKHR", "eglCreateSyncKHR",
    "eglDestroySyncKHR", "eglClientWaitSyncKHR", "eglGetSyncAttribKHR",
};

#define MAXTAB (sizeof passthrough / sizeof *passthrough + 48)
static nx_import tab[MAXTAB];
static size_t tab_n;

void rcr_egl_init(void)
{
    tab_n = 0;
    /* Ours in both modes: the swap is where the frame accounting, the finger
     * and the frame probe live, and eglGetProcAddress has to answer from this
     * table or the driver would hand the game the raw entry points back. */
    tab[tab_n++] = (nx_import){ "eglSwapBuffers", (void *)my_eglSwapBuffers };
    tab[tab_n++] = (nx_import){ "eglGetProcAddress",
                                (void *)my_eglGetProcAddress };

    if (rcr_video_mode() == RCR_VIDEO_SDL) {
        /* KMSDRM/Wayland: the firmware's SDL owns window, context and present,
         * so every remaining EGL call is answered from it.  Raw EGL here would
         * draw into a surface that never becomes a page flip. */
        size_t n = 0;
        const nx_import *v = rcr_video_egl_table(&n);
        for (size_t i = 0; i < n && tab_n < MAXTAB; i++) {
            if (strcmp(v[i].name, "eglSwapBuffers") == 0)
                continue;
            tab[tab_n++] = v[i];
        }
        nx_log("egl table: %zu entries (SDL owns the drawable)", tab_n);
        return;
    }

    /* fbdev/Mali-450: the game's own SDL drives the driver directly. */
    tab[tab_n++] = (nx_import){ "eglCreateWindowSurface",
                                (void *)my_eglCreateWindowSurface };
    tab[tab_n++] = (nx_import){ "eglGetDisplay", (void *)my_eglGetDisplay };
    for (size_t i = 0; i < sizeof passthrough / sizeof *passthrough; i++) {
        void *f = sys(passthrough[i]);
        if (f)
            tab[tab_n++] = (nx_import){ passthrough[i], f };
    }
    nx_log("egl table: %zu entries (raw fbdev EGL)", tab_n);
}

const nx_import *rcr_egl_table(size_t *n)
{
    *n = tab_n;
    return tab;
}

void *rcr_egl_sym(const char *name)
{
    for (size_t i = 0; i < tab_n; i++)
        if (strcmp(tab[i].name, name) == 0)
            return tab[i].addr;
    if (rcr_video_mode() == RCR_VIDEO_SDL)
        return rcr_video_gl_sym(name);
    return gl_raw(name);
}
