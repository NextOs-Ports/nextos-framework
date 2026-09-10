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
#include "tr.h"
#include "video.h"
#include "probe_ring.h"
#include "nx_frameprobe.h"

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
    return NULL;
}

/* --- the pad-driven finger ----------------------------------------------- */

/* Drawn with scissor rectangles and clears: no shaders, no vertex state, and
 * nothing the game has bound is disturbed beyond three values that are saved
 * and put back.  Only runs while the finger is actually on screen, so a level
 * pays nothing for it -- glGet* is a pipeline stall on this GPU. */
static void clear_runs(const uint16_t *mask, int rows, int x, int y, int scale,
                       int width, int height,
                       void (*scissor)(GLint, GLint, GLsizei, GLsizei),
                       void (*clear)(GLbitfield))
{
    for (int row = 0; row < rows; row++) {
        uint16_t bits = mask[row];
        for (int col = 0; col < 16;) {
            while (col < 16 && !(bits & (UINT16_C(1) << col)))
                col++;
            int start = col;
            while (col < 16 && (bits & (UINT16_C(1) << col)))
                col++;
            if (start == col)
                continue;
            int sx = x + start * scale, top = y + row * scale;
            int sw = (col - start) * scale, sh = scale;
            if (sx < 0) { sw += sx; sx = 0; }
            if (top < 0) { sh += top; top = 0; }
            if (sx + sw > width) sw = width - sx;
            if (top + sh > height) sh = height - top;
            if (sw <= 0 || sh <= 0)
                continue;
            scissor(sx, height - top - sh, sw, sh);
            clear(GL_COLOR_BUFFER_BIT);
        }
    }
}

static void draw_finger(void)
{
    float fx, fy;
    if (!tr_input_cursor(&fx, &fy))
        return;

    static const uint16_t outline[] = {
        0x0001, 0x0003, 0x0007, 0x000f, 0x001f, 0x003f, 0x007f, 0x00ff,
        0x01ff, 0x03ff, 0x07ff, 0x0fff, 0x1fff, 0x00ff, 0x01ef, 0x03c7,
        0x03c3, 0x0781, 0x0780, 0x0300,
    };
    static const uint16_t fill[] = {
        0x0000, 0x0000, 0x0002, 0x0006, 0x000e, 0x001e, 0x003e, 0x007e,
        0x00fe, 0x01fe, 0x03fe, 0x07fe, 0x01fe, 0x003e, 0x00c6, 0x0182,
        0x0180, 0x0300, 0x0300, 0x0000,
    };
    static void (*get_int)(GLenum, GLint *);
    static void (*get_float)(GLenum, GLfloat *);
    static void (*get_bool)(GLenum, GLboolean *);
    static GLboolean (*is_enabled)(GLenum);
    static void (*enable)(GLenum);
    static void (*disable)(GLenum);
    static void (*scissor)(GLint, GLint, GLsizei, GLsizei);
    static void (*clear_color)(GLfloat, GLfloat, GLfloat, GLfloat);
    static void (*color_mask)(GLboolean, GLboolean, GLboolean, GLboolean);
    static void (*clear)(GLbitfield);
    if (!get_int) {
        get_int = gl_raw("glGetIntegerv");
        get_float = gl_raw("glGetFloatv");
        get_bool = gl_raw("glGetBooleanv");
        is_enabled = gl_raw("glIsEnabled");
        enable = gl_raw("glEnable");
        disable = gl_raw("glDisable");
        scissor = gl_raw("glScissor");
        clear_color = gl_raw("glClearColor");
        color_mask = gl_raw("glColorMask");
        clear = gl_raw("glClear");
    }
    if (!get_int || !get_float || !get_bool || !is_enabled || !enable ||
        !disable || !scissor || !clear_color || !color_mask || !clear)
        return;

    /* O custo deste desenho ja foi medido e absolvido: glGet* 0,01ms/frame e
     * os clears 0,08ms/frame.  O cronometro que provou isso saiu daqui -- eram
     * tres leituras de relogio por frame para uma pergunta ja respondida. */
    GLint framebuffer = 0;
    get_int(GL_FRAMEBUFFER_BINDING, &framebuffer);
    if (framebuffer != 0)
        return;                       /* the game is drawing to its own target */

    GLint viewport[4] = { 0, 0, 1280, 720 };
    GLint old_scissor[4];
    GLfloat old_clear[4];
    GLboolean old_mask[4];
    GLboolean had_scissor = is_enabled(GL_SCISSOR_TEST);
    get_int(GL_VIEWPORT, viewport);
    get_int(GL_SCISSOR_BOX, old_scissor);
    get_float(GL_COLOR_CLEAR_VALUE, old_clear);
    get_bool(GL_COLOR_WRITEMASK, old_mask);

    int width = viewport[2], height = viewport[3];
    int x = viewport[0] + (int)fx;
    int y = (int)fy;
    int scale = width >= 1000 ? 2 : 1;

    /* Pressionando, o dedo fica ambar: sem isso nao ha como saber, olhando a
     * tela, se o toque saiu ou se o botao nao chegou. */
    int pressed = tr_input_cursor_pressed();

    enable(GL_SCISSOR_TEST);
    color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    clear_color(0.05f, 0.03f, 0.05f, 1.0f);
    clear_runs(outline, 20, x, y, scale, width, height, scissor, clear);
    if (pressed)
        clear_color(1.0f, 0.62f, 0.11f, 1.0f);
    else
        clear_color(0.99f, 0.94f, 0.80f, 1.0f);
    clear_runs(fill, 20, x, y, scale, width, height, scissor, clear);

    clear_color(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
    color_mask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    scissor(old_scissor[0], old_scissor[1], old_scissor[2], old_scissor[3]);
    if (!had_scissor)
        disable(GL_SCISSOR_TEST);

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
    EGLNativeWindowType real = (EGLNativeWindowType)tr_native_window();
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
        fprintf(stderr, "[tr] first frame presented\n");
        /* Quem apresenta o frame e a thread que trava: publicar o tid permite
         * amostrar o PC dela de fora (/proc/<pid>/task/<tid>/syscall) e dizer
         * ONDE o jogo esta parado, em vez de adivinhar. */
        fprintf(stderr, "[tr] render tid=%d\n", (int)syscall(SYS_gettid));
    }
    draw_finger();
    /* The in-process witness: it reads the frame the driver is about to
     * present, which a framebuffer capture cannot do reliably.  Off unless
     * NX_FRAMEPROBE=1, and never on in a release measurement of fps. */
    nx_frameprobe_before_swap();
    if (tr_video_mode() != TR_VIDEO_SDL && !p_eglSwapBuffers)
        p_eglSwapBuffers = sys("eglSwapBuffers");

    /* Split the frame in two so a slow frame says where it was slow: inside
     * the driver's present, or in everything the game did before it. */
    static struct timespec prev_swap_end;
    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    EGLBoolean r = tr_video_mode() == TR_VIDEO_SDL
        ? (EGLBoolean)tr_video_present()
        : p_eglSwapBuffers(display, surface);
    clock_gettime(CLOCK_MONOTONIC, &after);
    swaps++;
    {
        double present_ms = (after.tv_sec - before.tv_sec) * 1e3 +
                            (after.tv_nsec - before.tv_nsec) / 1e6;
        if (present_ms > 1000.0)
            fprintf(stderr, "[tr] the present of frame %lu took %.0f ms -- the "
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
     * TR_PROBE_KEEP=1 deixa o anel correndo (para medir varios). */
    {
        double dt = prev_swap_end.tv_sec
            ? (after.tv_sec - prev_swap_end.tv_sec) * 1e3 +
              (after.tv_nsec - prev_swap_end.tv_nsec) / 1e6
            : 0.0;
        tr_probe_note(TR_PROBE_SWAP, (uint32_t)swaps, (uint32_t)dt);
        static int keep = -1;
        if (keep < 0)
            keep = getenv("TR_PROBE_KEEP") ? 1 : 0;
        if (!keep && dt > 500.0 && swaps > 120)
            tr_probe_freeze();
    }
    prev_swap_end = after;
    if (tr_max_frames > 0 && (long)swaps >= tr_max_frames) {
        fprintf(stderr, "[tr] frame limit reached (%lu swaps)\n", swaps);
        fflush(stderr);
        _exit(0);
    }
    return r;
}

unsigned long tr_egl_swap_count(void) { return swaps; }

/* SDL resolves its EGL entry points through eglGetProcAddress as well as
 * dlsym, and the driver's version would hand back the raw eglSwapBuffers --
 * bypassing the swap hook and, with it, the frame probe.  Answer from our own
 * table first, exactly as the loader does for the PLT. */
static void *(*p_eglGetProcAddress)(const char *);

static void *my_eglGetProcAddress(const char *name)
{
    void *ours = name ? tr_egl_sym(name) : NULL;
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

void tr_egl_init(void)
{
    tab_n = 0;
    /* Ours in both modes: the swap is where the frame accounting, the finger
     * and the frame probe live, and eglGetProcAddress has to answer from this
     * table or the driver would hand the game the raw entry points back. */
    tab[tab_n++] = (nx_import){ "eglSwapBuffers", (void *)my_eglSwapBuffers };
    tab[tab_n++] = (nx_import){ "eglGetProcAddress",
                                (void *)my_eglGetProcAddress };

    if (tr_video_mode() == TR_VIDEO_SDL) {
        /* KMSDRM/Wayland: the firmware's SDL owns window, context and present,
         * so every remaining EGL call is answered from it.  Raw EGL here would
         * draw into a surface that never becomes a page flip. */
        size_t n = 0;
        const nx_import *v = tr_video_egl_table(&n);
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

const nx_import *tr_egl_table(size_t *n)
{
    *n = tab_n;
    return tab;
}

void *tr_egl_sym(const char *name)
{
    for (size_t i = 0; i < tab_n; i++)
        if (strcmp(tab[i].name, name) == 0)
            return tab[i].addr;
    return gl_raw(name);
}
