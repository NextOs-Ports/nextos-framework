/*
 * SDL2-backed OpenGL ES bridge for Stardew Valley.
 *
 * There is intentionally no SDL/GLES include or link dependency in this
 * translation unit. main.c loads SDL2 with RTLD_GLOBAL; this bridge consumes
 * that already-loaded API through dlsym(RTLD_DEFAULT).
 */

#include "sdv_egl_bridge.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

typedef struct SDL_Window SDL_Window;
typedef void *SDL_GLContext;
typedef struct SDL_GameController SDL_GameController;

typedef struct {
    uint32_t format;
    int w;
    int h;
    int refresh_rate;
    void *driverdata;
} SdvSDLDisplayMode;

enum {
    SDV_SDL_INIT_VIDEO = 0x00000020u,
    SDV_SDL_INIT_GAMECONTROLLER = 0x00002000u,
    SDV_SDL_WINDOW_FULLSCREEN = 0x00000001u,
    SDV_SDL_WINDOW_FULLSCREEN_DESKTOP = 0x00001001u,
    SDV_SDL_WINDOW_OPENGL = 0x00000002u,
    SDV_SDL_WINDOWPOS_CENTERED = 0x2fff0000u,

    SDV_SDL_GL_RED_SIZE = 0,
    SDV_SDL_GL_GREEN_SIZE = 1,
    SDV_SDL_GL_BLUE_SIZE = 2,
    SDV_SDL_GL_ALPHA_SIZE = 3,
    SDV_SDL_GL_DOUBLEBUFFER = 5,
    SDV_SDL_GL_DEPTH_SIZE = 6,
    SDV_SDL_GL_STENCIL_SIZE = 7,
    SDV_SDL_GL_CONTEXT_MAJOR_VERSION = 17,
    SDV_SDL_GL_CONTEXT_MINOR_VERSION = 18,
    SDV_SDL_GL_CONTEXT_PROFILE_MASK = 21,
    SDV_SDL_GL_CONTEXT_PROFILE_ES = 0x0004
};

typedef struct {
    int (*init_subsystem)(uint32_t flags);
    uint32_t (*was_init)(uint32_t flags);
    void (*quit_subsystem)(uint32_t flags);
    int (*get_desktop_display_mode)(int display_index,
                                    SdvSDLDisplayMode *mode);
    int (*set_hint)(const char *name, const char *value);
    void (*gl_get_drawable_size)(SDL_Window *window, int *w, int *h);
    const char *(*get_current_video_driver)(void);
    const char *(*get_error)(void);
    void (*gl_reset_attributes)(void);
    int (*gl_set_attribute)(int attr, int value);
    SDL_Window *(*create_window)(const char *title, int x, int y, int w,
                                 int h, uint32_t flags);
    void (*destroy_window)(SDL_Window *window);
    SDL_GLContext (*gl_create_context)(SDL_Window *window);
    int (*gl_make_current)(SDL_Window *window, SDL_GLContext context);
    void (*gl_delete_context)(SDL_GLContext context);
    void (*gl_swap_window)(SDL_Window *window);
    int (*gl_set_swap_interval)(int interval);
    void *(*gl_get_proc_address)(const char *name);
    void (*pump_events)(void);
    int (*poll_event)(void *event);
    int (*num_joysticks)(void);
    int (*is_game_controller)(int joystick_index);
    SDL_GameController *(*game_controller_open)(int joystick_index);
    void (*game_controller_close)(SDL_GameController *controller);
    const char *(*game_controller_name)(SDL_GameController *controller);
    int (*game_controller_attached)(SDL_GameController *controller);
    void (*game_controller_update)(void);
    unsigned char (*game_controller_get_button)(SDL_GameController *controller,
                                                 int button);
    short (*game_controller_get_axis)(SDL_GameController *controller, int axis);
} SdvSDLApi;

typedef struct {
    uint32_t magic;
    unsigned int generation;
} SdvEglSurface;

#define SDV_SURFACE_MAGIC UINT32_C(0x53445653)

static SdvSDLApi g_sdl;
static SDL_Window *g_window;
static SDL_GLContext g_context;
static SDL_GameController *g_gamepad;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int g_surface_generation;
static unsigned int g_swap_count;
static int g_width = 1280;
static int g_height = 720;
static float g_right_cursor_x = 640.0f;
static float g_right_cursor_y = 360.0f;
static int g_right_cursor_visible;
static int g_video_owned;
static int g_gamecontroller_owned;
static int g_symbols_ready;

static const char *sdv_sdl_error(void)
{
    const char *error = g_sdl.get_error ? g_sdl.get_error() : NULL;
    return (error && error[0]) ? error : "unknown SDL error";
}

static void *sdv_resolve(const char *name, int required)
{
    void *symbol = dlsym(RTLD_DEFAULT, name);
    if (!symbol && required)
        fprintf(stderr, "[sdv-egl] missing SDL symbol %s\n", name);
    return symbol;
}

/* POSIX specifies dlsym for functions; memcpy avoids ISO C's object/function
 * pointer cast diagnostics while retaining that POSIX behavior. */
#define SDV_RESOLVE_FUNCTION(member, name, required)                         \
    do {                                                                      \
        void *sdv_symbol_ = sdv_resolve((name), (required));                  \
        memcpy(&g_sdl.member, &sdv_symbol_, sizeof(g_sdl.member));            \
        if ((required) && !g_sdl.member)                                      \
            ok = 0;                                                           \
    } while (0)

static int sdv_resolve_sdl(void)
{
    int ok = 1;

    if (g_symbols_ready)
        return 1;

    memset(&g_sdl, 0, sizeof(g_sdl));
    SDV_RESOLVE_FUNCTION(init_subsystem, "SDL_InitSubSystem", 1);
    SDV_RESOLVE_FUNCTION(was_init, "SDL_WasInit", 0);
    SDV_RESOLVE_FUNCTION(quit_subsystem, "SDL_QuitSubSystem", 0);
    SDV_RESOLVE_FUNCTION(get_desktop_display_mode,
                         "SDL_GetDesktopDisplayMode", 0);
    SDV_RESOLVE_FUNCTION(set_hint, "SDL_SetHint", 0);
    SDV_RESOLVE_FUNCTION(gl_get_drawable_size, "SDL_GL_GetDrawableSize", 0);
    SDV_RESOLVE_FUNCTION(get_current_video_driver,
                         "SDL_GetCurrentVideoDriver", 0);
    SDV_RESOLVE_FUNCTION(get_error, "SDL_GetError", 0);
    SDV_RESOLVE_FUNCTION(gl_reset_attributes, "SDL_GL_ResetAttributes", 0);
    SDV_RESOLVE_FUNCTION(gl_set_attribute, "SDL_GL_SetAttribute", 1);
    SDV_RESOLVE_FUNCTION(create_window, "SDL_CreateWindow", 1);
    SDV_RESOLVE_FUNCTION(destroy_window, "SDL_DestroyWindow", 1);
    SDV_RESOLVE_FUNCTION(gl_create_context, "SDL_GL_CreateContext", 1);
    SDV_RESOLVE_FUNCTION(gl_make_current, "SDL_GL_MakeCurrent", 1);
    SDV_RESOLVE_FUNCTION(gl_delete_context, "SDL_GL_DeleteContext", 1);
    SDV_RESOLVE_FUNCTION(gl_swap_window, "SDL_GL_SwapWindow", 1);
    SDV_RESOLVE_FUNCTION(gl_set_swap_interval,
                         "SDL_GL_SetSwapInterval", 0);
    SDV_RESOLVE_FUNCTION(gl_get_proc_address, "SDL_GL_GetProcAddress", 1);
    SDV_RESOLVE_FUNCTION(pump_events, "SDL_PumpEvents", 0);
    SDV_RESOLVE_FUNCTION(poll_event, "SDL_PollEvent", 0);
    SDV_RESOLVE_FUNCTION(num_joysticks, "SDL_NumJoysticks", 0);
    SDV_RESOLVE_FUNCTION(is_game_controller, "SDL_IsGameController", 0);
    SDV_RESOLVE_FUNCTION(game_controller_open, "SDL_GameControllerOpen", 0);
    SDV_RESOLVE_FUNCTION(game_controller_close, "SDL_GameControllerClose", 0);
    SDV_RESOLVE_FUNCTION(game_controller_name, "SDL_GameControllerName", 0);
    SDV_RESOLVE_FUNCTION(game_controller_attached,
                         "SDL_GameControllerGetAttached", 0);
    SDV_RESOLVE_FUNCTION(game_controller_update,
                         "SDL_GameControllerUpdate", 0);
    SDV_RESOLVE_FUNCTION(game_controller_get_button,
                         "SDL_GameControllerGetButton", 0);
    SDV_RESOLVE_FUNCTION(game_controller_get_axis,
                         "SDL_GameControllerGetAxis", 0);

    if (!ok) {
        memset(&g_sdl, 0, sizeof(g_sdl));
        return 0;
    }

    g_symbols_ready = 1;
    return 1;
}

#undef SDV_RESOLVE_FUNCTION

/*
 * Android/Bionic code reads its stack guard from tpidr_el0 + 0x28. On this
 * glibc host that slot overlaps TLS state touched by the Mali/SDL GL calls.
 * Keep the Bionic-visible value stable around the two known offenders.
 */
static __attribute__((noinline)) SDL_GLContext
sdv_guarded_create_context(SDL_Window *window)
{
#if defined(__aarch64__)
    uintptr_t thread_pointer;
    uintptr_t stack_guard;

    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(thread_pointer));
    stack_guard = *(volatile uintptr_t *)(thread_pointer + 0x28u);
#endif

    SDL_GLContext context = g_sdl.gl_create_context(window);

#if defined(__aarch64__)
    *(volatile uintptr_t *)(thread_pointer + 0x28u) = stack_guard;
    __asm__ volatile("" ::: "memory");
#endif
    return context;
}

static __attribute__((noinline)) int
sdv_guarded_make_current(SDL_Window *window, SDL_GLContext context)
{
#if defined(__aarch64__)
    uintptr_t thread_pointer;
    uintptr_t stack_guard;

    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(thread_pointer));
    stack_guard = *(volatile uintptr_t *)(thread_pointer + 0x28u);
#endif

    int result = g_sdl.gl_make_current(window, context);

#if defined(__aarch64__)
    *(volatile uintptr_t *)(thread_pointer + 0x28u) = stack_guard;
    __asm__ volatile("" ::: "memory");
#endif
    return result;
}

/* ---- resolucao: cadeia de fontes, nunca numero fixo ------------------------
 * Um 1280x720 cravado vira zoom gigante num painel 640x480 e um quarto de tela
 * num 1080p. A ordem segue a doutrina de suportando_outros_devices:
 *   SDL_GetDesktopDisplayMode -> /sys/class/drm (conector "connected")
 *   -> /sys/class/graphics/fb0 -> 1280x720 (e LOGA que caiu aqui).
 * O SDL_InitSubSystem(VIDEO) explicito ja acontece antes daqui — sem ele a
 * query falha em silencio e o fallback vence sozinho. */
static int sdv_res_from_drm(int *w, int *h)
{
    DIR *dir = opendir("/sys/class/drm");
    struct dirent *entry;
    int found = 0;

    if (!dir)
        return 0;
    while (!found && (entry = readdir(dir))) {
        char path[512];
        char line[128];
        FILE *f;

        if (strncmp(entry->d_name, "card", 4) != 0 || !strchr(entry->d_name, '-'))
            continue;
        snprintf(path, sizeof(path), "/sys/class/drm/%s/status", entry->d_name);
        f = fopen(path, "r");
        if (!f)
            continue;
        line[0] = '\0';
        if (!fgets(line, sizeof(line), f) || strncmp(line, "connected", 9) != 0) {
            fclose(f);
            continue;
        }
        fclose(f);

        snprintf(path, sizeof(path), "/sys/class/drm/%s/modes", entry->d_name);
        f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(line, sizeof(line), f)) {          /* 1a linha = modo preferido */
            int mw = 0, mh = 0;
            if (sscanf(line, "%dx%d", &mw, &mh) == 2 && mw > 0 && mh > 0) {
                *w = mw;
                *h = mh;
                found = 1;
                fprintf(stderr, "[sdv-egl] resolucao por DRM (%s): %dx%d\n",
                        entry->d_name, mw, mh);
            }
        }
        fclose(f);
    }
    closedir(dir);
    return found;
}

static int sdv_res_from_fb0(int *w, int *h)
{
    /* "U:640x480p-0" */
    FILE *f = fopen("/sys/class/graphics/fb0/mode", "r");
    char line[128];
    int mw = 0, mh = 0;

    if (!f)
        return 0;
    line[0] = '\0';
    if (fgets(line, sizeof(line), f)) {
        const char *p = strchr(line, ':');
        if (p && sscanf(p + 1, "%dx%d", &mw, &mh) == 2 && mw > 0 && mh > 0) {
            *w = mw;
            *h = mh;
            fprintf(stderr, "[sdv-egl] resolucao por fb0: %dx%d\n", mw, mh);
        }
    }
    fclose(f);
    return (mw > 0 && mh > 0);
}

/* Escolhe a lib GL cliente (GLES via EGL) ANTES da 1a janela.
 * Em Mesa/Panfrost um pedido de ES pode voltar contexto OpenGL DESKTOP: cria com
 * sucesso, mas os shaders GLSL ES do MonoGame nao compilam = tela preta. Isto
 * NAO viola a regra #6: escolhe a lib GL, nao o driver de DISPLAY. */
/* Fullscreen EXCLUSIVO faz mode-set e da' EGL_BAD_MATCH em kmsdrm/wayland; o
 * FULLSCREEN_DESKTOP e' borderless e nao mexe no modo. Mas o caminho do
 * Mali-450/fbdev esta' validado em campo com o exclusivo, entao ele continua
 * sendo o PRIMEIRO ali — trocar a ordem so' onde nao ha' o que regredir.
 * Nos dois casos a outra opcao fica como fallback. SDV_EXCL_FS=1 forca. */
static void sdv_window_flag_order(uint32_t out[2])
{
    const char *drv = g_sdl.get_current_video_driver
                          ? g_sdl.get_current_video_driver() : NULL;
    int fbdev_like = drv && (strcmp(drv, "mali") == 0 ||
                             strcmp(drv, "fbdev") == 0 ||
                             strcmp(drv, "directfb") == 0);

    if (getenv("SDV_EXCL_FS"))
        fbdev_like = 1;
    if (fbdev_like) {
        out[0] = SDV_SDL_WINDOW_FULLSCREEN;
        out[1] = SDV_SDL_WINDOW_FULLSCREEN_DESKTOP;
    } else {
        out[0] = SDV_SDL_WINDOW_FULLSCREEN_DESKTOP;
        out[1] = SDV_SDL_WINDOW_FULLSCREEN;
    }
}

static void sdv_prefer_gles_lib(void)
{
    if (!g_sdl.set_hint || getenv("SDV_NO_FORCE_GLES"))
        return;
    g_sdl.set_hint("SDL_OPENGL_ES_DRIVER", "1");
    g_sdl.set_hint("SDL_VIDEO_X11_FORCE_EGL", "1");
}

static void sdv_set_gl_attributes(int alpha, int depth, int stencil)
{
    if (g_sdl.gl_reset_attributes)
        g_sdl.gl_reset_attributes();

    g_sdl.gl_set_attribute(SDV_SDL_GL_CONTEXT_PROFILE_MASK,
                           SDV_SDL_GL_CONTEXT_PROFILE_ES);
    g_sdl.gl_set_attribute(SDV_SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    g_sdl.gl_set_attribute(SDV_SDL_GL_CONTEXT_MINOR_VERSION, 0);
    g_sdl.gl_set_attribute(SDV_SDL_GL_RED_SIZE, 8);
    g_sdl.gl_set_attribute(SDV_SDL_GL_GREEN_SIZE, 8);
    g_sdl.gl_set_attribute(SDV_SDL_GL_BLUE_SIZE, 8);
    g_sdl.gl_set_attribute(SDV_SDL_GL_ALPHA_SIZE, alpha);
    g_sdl.gl_set_attribute(SDV_SDL_GL_DEPTH_SIZE, depth);
    g_sdl.gl_set_attribute(SDV_SDL_GL_STENCIL_SIZE, stencil);
    g_sdl.gl_set_attribute(SDV_SDL_GL_DOUBLEBUFFER, 1);
}

static const char *sdv_gl_version(void)
{
    typedef const unsigned char *(*GlGetStringFn)(unsigned int name);
    GlGetStringFn get_string = NULL;
    void *symbol = g_sdl.gl_get_proc_address("glGetString");

    if (symbol)
        memcpy(&get_string, &symbol, sizeof(get_string));
    if (!get_string)
        return NULL;

    /* GL_VERSION, written literally to avoid a GLES header dependency. */
    return (const char *)get_string(0x1f02u);
}

static void sdv_drop_candidate(void)
{
    if (g_context) {
        sdv_guarded_make_current(g_window, NULL);
        g_sdl.gl_delete_context(g_context);
        g_context = NULL;
    }
    if (g_window) {
        g_sdl.destroy_window(g_window);
        g_window = NULL;
    }
}

int sdv_egl_init(void)
{
    static const struct {
        int alpha;
        int depth;
        int stencil;
    } ladder[] = {
        {8, 24, 8},
        {0, 24, 8},
        {0, 16, 8},
        {8, 16, 8},
        {0, 16, 0},
        {8, 16, 0}
    };
    SdvSDLDisplayMode desktop;
    uint32_t initialized;
    size_t rung;
    int result = 0;

    pthread_mutex_lock(&g_lock);

    if (g_window && g_context) {
        result = 1;
        goto out;
    }
    if (!sdv_resolve_sdl())
        goto out;

    initialized = g_sdl.was_init ? g_sdl.was_init(SDV_SDL_INIT_VIDEO) : 0;
    if ((initialized & SDV_SDL_INIT_VIDEO) == 0) {
        if (g_sdl.init_subsystem(SDV_SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "[sdv-egl] SDL video init failed: %s\n",
                    sdv_sdl_error());
            goto out;
        }
        g_video_owned = 1;
    }
    initialized = g_sdl.was_init
        ? g_sdl.was_init(SDV_SDL_INIT_GAMECONTROLLER) : 0;
    if ((initialized & SDV_SDL_INIT_GAMECONTROLLER) == 0 &&
        g_sdl.init_subsystem(SDV_SDL_INIT_GAMECONTROLLER) == 0)
        g_gamecontroller_owned = 1;

    sdv_prefer_gles_lib();

    g_width = 0;
    g_height = 0;
    memset(&desktop, 0, sizeof(desktop));
    if (g_sdl.get_desktop_display_mode &&
        g_sdl.get_desktop_display_mode(0, &desktop) == 0 &&
        desktop.w > 0 && desktop.h > 0) {
        g_width = desktop.w;
        g_height = desktop.h;
    }
    if (g_width <= 0 || g_height <= 0)
        sdv_res_from_drm(&g_width, &g_height);
    if (g_width <= 0 || g_height <= 0)
        sdv_res_from_fb0(&g_width, &g_height);
    if (g_width <= 0 || g_height <= 0) {
        g_width = 1280;
        g_height = 720;
        fprintf(stderr, "[sdv-egl] AVISO: nenhuma fonte de resolucao respondeu, "
                        "usando %dx%d\n", g_width, g_height);
    }
    {
        const char *forced_width = getenv("SDV_WIDTH");
        const char *forced_height = getenv("SDV_HEIGHT");
        int width = forced_width ? atoi(forced_width) : 0;
        int height = forced_height ? atoi(forced_height) : 0;
        if (width > 0 && height > 0) {
            fprintf(stderr,
                    "[sdv-egl] desktop=%dx%d, forcing requested mode=%dx%d\n",
                    g_width, g_height, width, height);
            g_width = width;
            g_height = height;
        }
    }

    /* Clear a half-created state left by an earlier failed initialization. */
    sdv_drop_candidate();

    for (rung = 0; rung < sizeof(ladder) / sizeof(ladder[0]); ++rung) {
        const char *version;

        sdv_set_gl_attributes(ladder[rung].alpha, ladder[rung].depth,
                              ladder[rung].stencil);
        {
            uint32_t fs[2];
            size_t i;

            sdv_window_flag_order(fs);
            g_window = NULL;
            for (i = 0; i < 2 && !g_window; ++i)
                g_window = g_sdl.create_window(
                    "Stardew Valley", (int)SDV_SDL_WINDOWPOS_CENTERED,
                    (int)SDV_SDL_WINDOWPOS_CENTERED, g_width, g_height,
                    SDV_SDL_WINDOW_OPENGL | fs[i]);
        }
        if (!g_window) {
            fprintf(stderr,
                    "[sdv-egl] rung %zu ES2 a%d d%d s%d: window failed: %s\n",
                    rung, ladder[rung].alpha, ladder[rung].depth,
                    ladder[rung].stencil, sdv_sdl_error());
            continue;
        }

        g_context = sdv_guarded_create_context(g_window);
        if (!g_context) {
            fprintf(stderr,
                    "[sdv-egl] rung %zu ES2 a%d d%d s%d: context failed: %s\n",
                    rung, ladder[rung].alpha, ladder[rung].depth,
                    ladder[rung].stencil, sdv_sdl_error());
            sdv_drop_candidate();
            continue;
        }

        if (sdv_guarded_make_current(g_window, g_context) != 0) {
            fprintf(stderr,
                    "[sdv-egl] rung %zu ES2 a%d d%d s%d: make-current failed: %s\n",
                    rung, ladder[rung].alpha, ladder[rung].depth,
                    ladder[rung].stencil, sdv_sdl_error());
            sdv_drop_candidate();
            continue;
        }

        version = sdv_gl_version();
        if (!version || strncmp(version, "OpenGL ES", 9) != 0) {
            fprintf(stderr,
                    "[sdv-egl] rung %zu rejected non-ES context ('%s')\n",
                    rung, version ? version : "null");
            sdv_drop_candidate();
            continue;
        }

        /* Autoridade final e' o DRAWABLE, nao o tamanho pedido: em wayland o
         * compositor pode devolver outro tamanho, e de forma assincrona (o
         * configure chega depois do create). Drena os eventos ate estabilizar. */
        if (g_sdl.gl_get_drawable_size) {
            int dw = 0, dh = 0, tries;

            for (tries = 0; tries < 30; ++tries) {
                if (g_sdl.pump_events)
                    g_sdl.pump_events();
                g_sdl.gl_get_drawable_size(g_window, &dw, &dh);
                if (dw > 0 && dh > 0 && dw == g_width && dh == g_height)
                    break;
                usleep(10 * 1000);
            }
            if (dw > 0 && dh > 0 && (dw != g_width || dh != g_height)) {
                fprintf(stderr,
                        "[sdv-egl] drawable real %dx%d (pedido %dx%d) — vale o real\n",
                        dw, dh, g_width, g_height);
                g_width = dw;
                g_height = dh;
            }
        }

        {
            const char *forced_interval = getenv("SDV_SWAP_INTERVAL");
            int has_override = forced_interval && forced_interval[0];
            int interval = has_override ? atoi(forced_interval) : 1;

            if (g_sdl.gl_set_swap_interval) {
                int swap_result = g_sdl.gl_set_swap_interval(interval);
                fprintf(stderr,
                        "[sdv-egl] swap interval=%d result=%d%s\n",
                        interval, swap_result,
                        has_override ? " [SDV_SWAP_INTERVAL]" : "");
            } else {
                fprintf(stderr,
                        "[sdv-egl] swap interval=%d unavailable\n", interval);
            }
        }

        fprintf(stderr,
                "[sdv-egl] ready %dx%d driver=%s ES2 a%d d%d s%d GL='%s'\n",
                g_width, g_height,
                g_sdl.get_current_video_driver
                    ? g_sdl.get_current_video_driver()
                    : "unknown",
                ladder[rung].alpha, ladder[rung].depth,
                ladder[rung].stencil, version);
        result = 1;
        break;
    }

    if (!result) {
        fprintf(stderr, "[sdv-egl] all OpenGL ES 2 configurations failed\n");
        sdv_drop_candidate();
        goto out;
    }

    /* SDL creates the context current on this (main) thread. Release it so
     * the single context can migrate to MonoGame's render thread. */
    if (sdv_guarded_make_current(g_window, NULL) != 0) {
        fprintf(stderr, "[sdv-egl] initial context release failed: %s\n",
                sdv_sdl_error());
        sdv_drop_candidate();
        result = 0;
        goto out;
    }
    fprintf(stderr, "[sdv-egl] context released for render thread\n");

out:
    pthread_mutex_unlock(&g_lock);
    return result;
}

void *sdv_egl_create_context(void)
{
    void *context;

    pthread_mutex_lock(&g_lock);
    context = (g_window && g_context) ? g_context : NULL;
    pthread_mutex_unlock(&g_lock);
    return context;
}

void *sdv_egl_create_surface(void)
{
    SdvEglSurface *surface;

    pthread_mutex_lock(&g_lock);
    if (!g_window || !g_context) {
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    surface = (SdvEglSurface *)calloc(1, sizeof(*surface));
    if (surface) {
        surface->magic = SDV_SURFACE_MAGIC;
        surface->generation = ++g_surface_generation;
        fprintf(stderr, "[sdv-egl] surface %u created\n",
                surface->generation);
    }
    pthread_mutex_unlock(&g_lock);
    return surface;
}

int sdv_egl_make_current(void *context, void *surface)
{
    int result;

    pthread_mutex_lock(&g_lock);
    if (!context && !surface) {
        result = g_window && g_sdl.gl_make_current
                     ? sdv_guarded_make_current(g_window, NULL) == 0
                     : 0;
        pthread_mutex_unlock(&g_lock);
        return result;
    }

    if (!g_window || context != g_context || !surface) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    result = sdv_guarded_make_current(g_window, g_context) == 0;
    if (!result)
        fprintf(stderr, "[sdv-egl] render-thread make-current failed: %s\n",
                sdv_sdl_error());
    pthread_mutex_unlock(&g_lock);
    return result;
}

/* O compositor OSD do Amlogic mistura fb0 pelo alpha de cada pixel. Alguns
 * pipelines MonoGame deixam o alpha final em zero embora o RGB esteja certo,
 * produzindo scanout preto. Preservamos todo o estado tocado e forçamos apenas
 * o alpha do backbuffer para 1 antes do present. O readback inicial também
 * separa "jogo desenhou preto" de "compositor descartou RGB por alpha". */
static void sdv_prepare_present(void)
{
    typedef void (*GetIntegervFn)(unsigned int, int *);
    typedef void (*GetBooleanvFn)(unsigned int, unsigned char *);
    typedef void (*GetFloatvFn)(unsigned int, float *);
    typedef unsigned char (*IsEnabledFn)(unsigned int);
    typedef void (*EnableDisableFn)(unsigned int);
    typedef void (*ColorMaskFn)(unsigned char, unsigned char,
                                unsigned char, unsigned char);
    typedef void (*ClearColorFn)(float, float, float, float);
    typedef void (*ClearFn)(unsigned int);
    typedef void (*ReadPixelsFn)(int, int, int, int, unsigned int,
                                 unsigned int, void *);
    static GetIntegervFn get_integerv;
    static GetBooleanvFn get_booleanv;
    static GetFloatvFn get_floatv;
    static IsEnabledFn is_enabled;
    static EnableDisableFn enable;
    static EnableDisableFn disable;
    static ColorMaskFn color_mask;
    static ClearColorFn clear_color;
    static ClearFn clear;
    static ReadPixelsFn read_pixels;
    static int resolved;
    static unsigned int diagnostic_count;

#define SDV_GL_RESOLVE(dst, name) do {                                      \
        void *p_ = g_sdl.gl_get_proc_address(name);                          \
        memcpy(&(dst), &p_, sizeof(dst));                                    \
    } while (0)
    if (!resolved) {
        resolved = 1;
        SDV_GL_RESOLVE(get_integerv, "glGetIntegerv");
        SDV_GL_RESOLVE(get_booleanv, "glGetBooleanv");
        SDV_GL_RESOLVE(get_floatv, "glGetFloatv");
        SDV_GL_RESOLVE(is_enabled, "glIsEnabled");
        SDV_GL_RESOLVE(enable, "glEnable");
        SDV_GL_RESOLVE(disable, "glDisable");
        SDV_GL_RESOLVE(color_mask, "glColorMask");
        SDV_GL_RESOLVE(clear_color, "glClearColor");
        SDV_GL_RESOLVE(clear, "glClear");
        SDV_GL_RESOLVE(read_pixels, "glReadPixels");
    }
#undef SDV_GL_RESOLVE

    if (!get_integerv) return;
    int fbo = -1;
    get_integerv(0x8ca6u /* GL_FRAMEBUFFER_BINDING */, &fbo);

    const char *trace = getenv("SDV_GL_TRACE");
    if (trace && trace[0] && trace[0] != '0' && diagnostic_count < 10 &&
        read_pixels) {
        int viewport[4] = {0, 0, 0, 0};
        unsigned char rgba[4] = {0, 0, 0, 0};
        unsigned char write_mask[4] = {0, 0, 0, 0};
        int scissor_box[4] = {0, 0, 0, 0};
        unsigned char scissor_on = 0;
        unsigned int error = 0;
        get_integerv(0x0ba2u /* GL_VIEWPORT */, viewport);
        if (viewport[2] > 0 && viewport[3] > 0) {
            if (get_booleanv)
                get_booleanv(0x0c23u /* GL_COLOR_WRITEMASK */, write_mask);
            get_integerv(0x0c10u /* GL_SCISSOR_BOX */, scissor_box);
            if (is_enabled)
                scissor_on = is_enabled(0x0c11u /* GL_SCISSOR_TEST */);
            read_pixels(viewport[0] + viewport[2] / 2,
                        viewport[1] + viewport[3] / 2, 1, 1,
                        0x1908u /* GL_RGBA */, 0x1401u /* GL_UNSIGNED_BYTE */,
                        rgba);
            {
                typedef unsigned int (*GetErrorFn)(void);
                GetErrorFn get_error = NULL;
                void *p = g_sdl.gl_get_proc_address("glGetError");
                if (p) memcpy(&get_error, &p, sizeof(get_error));
                if (get_error) error = get_error();
            }
            fprintf(stderr,
                    "[sdv-egl] present fbo=%d viewport=%d,%d %dx%d mask=%u%u%u%u scissor=%u:%d,%d,%dx%d center=%u,%u,%u,%u err=%x\n",
                    fbo, viewport[0], viewport[1], viewport[2], viewport[3],
                    write_mask[0], write_mask[1], write_mask[2], write_mask[3],
                    scissor_on, scissor_box[0], scissor_box[1], scissor_box[2],
                    scissor_box[3], rgba[0], rgba[1], rgba[2], rgba[3], error);
            diagnostic_count++;
        }
    }

    const char *force = getenv("SDV_FORCE_ALPHA");
    if ((force && force[0] == '0') || fbo != 0 || !get_booleanv || !get_floatv ||
        !is_enabled || !enable || !disable || !color_mask || !clear_color || !clear)
        return;

    unsigned char old_mask[4] = {1, 1, 1, 1};
    float old_clear[4] = {0, 0, 0, 0};
    unsigned char scissor = is_enabled(0x0c11u /* GL_SCISSOR_TEST */);
    get_booleanv(0x0c23u /* GL_COLOR_WRITEMASK */, old_mask);
    get_floatv(0x0c22u /* GL_COLOR_CLEAR_VALUE */, old_clear);
    if (scissor) disable(0x0c11u);
    color_mask(0, 0, 0, 1);
    clear_color(0.0f, 0.0f, 0.0f, 1.0f);
    clear(0x00004000u /* GL_COLOR_BUFFER_BIT */);
    clear_color(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
    color_mask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    if (scissor) enable(0x0c11u);
}

/* Seta de mouse em pixel-art, independente do cursor/foco do Stardew. A ponta
 * superior esquerda continua sendo o hotspot exato enviado no toque de R3.
 * glClear com scissor evita shader/VBO/texture extra e, importante no Mali
 * antigo, restaura todo estado GL tocado antes de devolver o backbuffer ao
 * SDL. O desenho usa poucos retangulos sobrepostos: contorno preto, miolo
 * dourado quente e um pequeno brilho ciano no hotspot. */
static void sdv_draw_right_cursor(void)
{
    typedef void (*GetIntegervFn)(unsigned int, int *);
    typedef void (*GetBooleanvFn)(unsigned int, unsigned char *);
    typedef void (*GetFloatvFn)(unsigned int, float *);
    typedef unsigned char (*IsEnabledFn)(unsigned int);
    typedef void (*EnableDisableFn)(unsigned int);
    typedef void (*ScissorFn)(int, int, int, int);
    typedef void (*ColorMaskFn)(unsigned char, unsigned char,
                                unsigned char, unsigned char);
    typedef void (*ClearColorFn)(float, float, float, float);
    typedef void (*ClearFn)(unsigned int);
    static GetIntegervFn get_integerv;
    static GetBooleanvFn get_booleanv;
    static GetFloatvFn get_floatv;
    static IsEnabledFn is_enabled;
    static EnableDisableFn enable;
    static EnableDisableFn disable;
    static ScissorFn scissor;
    static ColorMaskFn color_mask;
    static ClearColorFn clear_color;
    static ClearFn clear;
    static int resolved;
    int fbo = -1;
    int old_scissor_box[4] = {0, 0, 0, 0};
    unsigned char old_mask[4] = {1, 1, 1, 1};
    float old_clear[4] = {0, 0, 0, 0};
    unsigned char old_scissor;
    int x;
    int y;

    if (!g_right_cursor_visible) return;

#define SDV_CURSOR_GL_RESOLVE(dst, name) do {                               \
        void *p_ = g_sdl.gl_get_proc_address(name);                         \
        memcpy(&(dst), &p_, sizeof(dst));                                   \
    } while (0)
    if (!resolved) {
        resolved = 1;
        SDV_CURSOR_GL_RESOLVE(get_integerv, "glGetIntegerv");
        SDV_CURSOR_GL_RESOLVE(get_booleanv, "glGetBooleanv");
        SDV_CURSOR_GL_RESOLVE(get_floatv, "glGetFloatv");
        SDV_CURSOR_GL_RESOLVE(is_enabled, "glIsEnabled");
        SDV_CURSOR_GL_RESOLVE(enable, "glEnable");
        SDV_CURSOR_GL_RESOLVE(disable, "glDisable");
        SDV_CURSOR_GL_RESOLVE(scissor, "glScissor");
        SDV_CURSOR_GL_RESOLVE(color_mask, "glColorMask");
        SDV_CURSOR_GL_RESOLVE(clear_color, "glClearColor");
        SDV_CURSOR_GL_RESOLVE(clear, "glClear");
    }
#undef SDV_CURSOR_GL_RESOLVE
    if (!get_integerv || !get_booleanv || !get_floatv || !is_enabled ||
        !enable || !disable || !scissor || !color_mask || !clear_color ||
        !clear)
        return;

    get_integerv(0x8ca6u /* GL_FRAMEBUFFER_BINDING */, &fbo);
    if (fbo != 0 || g_width <= 0 || g_height <= 0)
        return;

    get_integerv(0x0c10u /* GL_SCISSOR_BOX */, old_scissor_box);
    get_booleanv(0x0c23u /* GL_COLOR_WRITEMASK */, old_mask);
    get_floatv(0x0c22u /* GL_COLOR_CLEAR_VALUE */, old_clear);
    old_scissor = is_enabled(0x0c11u /* GL_SCISSOR_TEST */);
    if (!old_scissor) enable(0x0c11u /* GL_SCISSOR_TEST */);
    color_mask(1, 1, 1, 1);

    x = (int)(g_right_cursor_x + 0.5f);
    y = (int)(g_right_cursor_y + 0.5f);

#define SDV_CURSOR_RECT(left_, top_, width_, height_, r_, g_, b_) do {       \
        int l_ = (left_);                                                    \
        int t_ = (top_);                                                     \
        int rgt_ = l_ + (width_);                                            \
        int bot_ = t_ + (height_);                                           \
        if (l_ < 0) l_ = 0;                                                  \
        if (t_ < 0) t_ = 0;                                                  \
        if (rgt_ > g_width) rgt_ = g_width;                                  \
        if (bot_ > g_height) bot_ = g_height;                                \
        if (rgt_ > l_ && bot_ > t_) {                                        \
            scissor(l_, g_height - bot_, rgt_ - l_, bot_ - t_);              \
            clear_color((r_), (g_), (b_), 1.0f);                             \
            clear(0x00004000u /* GL_COLOR_BUFFER_BIT */);                     \
        }                                                                    \
    } while (0)
    /* Silhueta escalonada: uma seta classica apontando para (x,y). */
    SDV_CURSOR_RECT(x - 1, y - 1,  4, 4, 0.0f, 0.0f, 0.0f);
    SDV_CURSOR_RECT(x - 1, y + 2,  7, 4, 0.0f, 0.0f, 0.0f);
    SDV_CURSOR_RECT(x - 1, y + 5, 10, 4, 0.0f, 0.0f, 0.0f);
    SDV_CURSOR_RECT(x - 1, y + 8, 13, 4, 0.0f, 0.0f, 0.0f);
    SDV_CURSOR_RECT(x - 1, y + 11, 16, 4, 0.0f, 0.0f, 0.0f);
    SDV_CURSOR_RECT(x - 1, y + 14, 19, 5, 0.0f, 0.0f, 0.0f);
    SDV_CURSOR_RECT(x + 5, y + 16,  6, 8, 0.0f, 0.0f, 0.0f);
    SDV_CURSOR_RECT(x + 8, y + 20,  6, 8, 0.0f, 0.0f, 0.0f);
    SDV_CURSOR_RECT(x + 11, y + 24, 6, 7, 0.0f, 0.0f, 0.0f);

    /* Preenchimento Stardew: creme/dourado, sempre legivel em mapa e menu. */
    SDV_CURSOR_RECT(x + 1, y + 2,  2, 2, 1.0f, 0.86f, 0.32f);
    SDV_CURSOR_RECT(x + 1, y + 4,  4, 3, 1.0f, 0.86f, 0.32f);
    SDV_CURSOR_RECT(x + 1, y + 7,  7, 3, 1.0f, 0.86f, 0.32f);
    SDV_CURSOR_RECT(x + 1, y + 10, 10, 3, 1.0f, 0.86f, 0.32f);
    SDV_CURSOR_RECT(x + 1, y + 13, 13, 3, 1.0f, 0.86f, 0.32f);
    SDV_CURSOR_RECT(x + 1, y + 16, 15, 2, 1.0f, 0.86f, 0.32f);
    SDV_CURSOR_RECT(x + 7, y + 18,  3, 5, 1.0f, 0.86f, 0.32f);
    SDV_CURSOR_RECT(x + 10, y + 22, 3, 5, 1.0f, 0.86f, 0.32f);
    SDV_CURSOR_RECT(x + 13, y + 26, 2, 3, 1.0f, 0.86f, 0.32f);
    SDV_CURSOR_RECT(x, y, 2, 2, 0.2f, 0.9f, 1.0f);
#undef SDV_CURSOR_RECT

    clear_color(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
    color_mask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    scissor(old_scissor_box[0], old_scissor_box[1], old_scissor_box[2],
            old_scissor_box[3]);
    if (!old_scissor) disable(0x0c11u /* GL_SCISSOR_TEST */);
}

/* Captura de diagnostico sob demanda, feita na propria thread de render.
 * KMSDRM nao espelha necessariamente o scanout em /dev/fb0, portanto fbgrab
 * pode devolver uma tela preta mesmo quando o jogo esta desenhando. Criar
 * /dev/shm/sdv-shot gera um PPM verticalmente corrigido do backbuffer em
 * /dev/shm/sdv-shot.ppm. Sem o gatilho, o custo se resume a um access() a
 * cada 15 presents. */
static void sdv_capture_backbuffer_if_requested(void)
{
    typedef void (*GetIntegervFn)(unsigned int, int *);
    typedef void (*PixelStoreiFn)(unsigned int, int);
    typedef void (*ReadPixelsFn)(int, int, int, int, unsigned int,
                                 unsigned int, void *);
    static GetIntegervFn get_integerv;
    static PixelStoreiFn pixel_storei;
    static ReadPixelsFn read_pixels;
    static int resolved;
    const char *trigger = "/dev/shm/sdv-shot";
    const char *temporary = "/dev/shm/sdv-shot.ppm.tmp";
    const char *output = "/dev/shm/sdv-shot.ppm";
    int viewport[4] = {0, 0, 0, 0};
    int framebuffer = -1;
    int old_pack_alignment = 4;
    unsigned char *rgba = NULL;
    unsigned char *rgb_row = NULL;
    FILE *stream = NULL;
    int ok = 0;

    if (g_swap_count % 15u != 0 || access(trigger, F_OK) != 0)
        return;
    unlink(trigger);

#define SDV_SHOT_GL_RESOLVE(dst, name) do {                                 \
        void *p_ = g_sdl.gl_get_proc_address(name);                          \
        memcpy(&(dst), &p_, sizeof(dst));                                    \
    } while (0)
    if (!resolved) {
        resolved = 1;
        SDV_SHOT_GL_RESOLVE(get_integerv, "glGetIntegerv");
        SDV_SHOT_GL_RESOLVE(pixel_storei, "glPixelStorei");
        SDV_SHOT_GL_RESOLVE(read_pixels, "glReadPixels");
    }
#undef SDV_SHOT_GL_RESOLVE
    if (!get_integerv || !pixel_storei || !read_pixels)
        goto out;

    get_integerv(0x8ca6u /* GL_FRAMEBUFFER_BINDING */, &framebuffer);
    get_integerv(0x0ba2u /* GL_VIEWPORT */, viewport);
    if (framebuffer != 0 || viewport[2] <= 0 || viewport[3] <= 0)
        goto out;
    if ((size_t)viewport[2] > SIZE_MAX / 4u / (size_t)viewport[3])
        goto out;

    rgba = malloc((size_t)viewport[2] * (size_t)viewport[3] * 4u);
    rgb_row = malloc((size_t)viewport[2] * 3u);
    if (!rgba || !rgb_row)
        goto out;

    get_integerv(0x0d05u /* GL_PACK_ALIGNMENT */, &old_pack_alignment);
    pixel_storei(0x0d05u /* GL_PACK_ALIGNMENT */, 1);
    read_pixels(viewport[0], viewport[1], viewport[2], viewport[3],
                0x1908u /* GL_RGBA */, 0x1401u /* GL_UNSIGNED_BYTE */, rgba);
    pixel_storei(0x0d05u /* GL_PACK_ALIGNMENT */, old_pack_alignment);

    stream = fopen(temporary, "wb");
    if (!stream || fprintf(stream, "P6\n%d %d\n255\n",
                           viewport[2], viewport[3]) < 0)
        goto out;
    for (int y = viewport[3] - 1; y >= 0; --y) {
        const unsigned char *source =
            rgba + (size_t)y * (size_t)viewport[2] * 4u;
        for (int x = 0; x < viewport[2]; ++x) {
            rgb_row[(size_t)x * 3u + 0u] = source[(size_t)x * 4u + 0u];
            rgb_row[(size_t)x * 3u + 1u] = source[(size_t)x * 4u + 1u];
            rgb_row[(size_t)x * 3u + 2u] = source[(size_t)x * 4u + 2u];
        }
        if (fwrite(rgb_row, 3u, (size_t)viewport[2], stream) !=
            (size_t)viewport[2])
            goto out;
    }
    if (fclose(stream) != 0) {
        stream = NULL;
        goto out;
    }
    stream = NULL;
    if (rename(temporary, output) != 0)
        goto out;
    ok = 1;

out:
    if (stream) fclose(stream);
    if (!ok) unlink(temporary);
    free(rgb_row);
    free(rgba);
    fprintf(stderr, "[sdv-egl] screenshot %s: %s\n",
            ok ? "saved" : "failed", output);
}

int sdv_egl_swap(void *surface)
{
    int result = 0;

    pthread_mutex_lock(&g_lock);
    if (surface && g_window && g_context && g_sdl.gl_swap_window) {
        sdv_prepare_present();
        sdv_draw_right_cursor();
        sdv_capture_backbuffer_if_requested();
        g_sdl.gl_swap_window(g_window);
        ++g_swap_count;
        const char *trace = getenv("SDV_GL_TRACE");
        if (g_swap_count == 1 ||
            (trace && trace[0] && trace[0] != '0' &&
             (g_swap_count <= 10 || g_swap_count % 300 == 0)))
            fprintf(stderr, "[sdv-egl] swap #%u\n", g_swap_count);
        result = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

void sdv_egl_destroy_surface(void *surface)
{
    SdvEglSurface *logical_surface = (SdvEglSurface *)surface;

    if (!logical_surface)
        return;
    if (logical_surface->magic == SDV_SURFACE_MAGIC) {
        fprintf(stderr, "[sdv-egl] surface %u destroyed\n",
                logical_surface->generation);
        logical_surface->magic = 0;
    }
    free(logical_surface);
}

void sdv_egl_destroy_context(void *context)
{
    pthread_mutex_lock(&g_lock);
    if (context && context == g_context) {
        sdv_guarded_make_current(g_window, NULL);
        g_sdl.gl_delete_context(g_context);
        g_context = NULL;
        fprintf(stderr, "[sdv-egl] context destroyed\n");
    }
    pthread_mutex_unlock(&g_lock);
}

void sdv_egl_destroy(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_gamepad && g_sdl.game_controller_close) {
        g_sdl.game_controller_close(g_gamepad);
        g_gamepad = NULL;
    }
    sdv_drop_candidate();
    if (g_gamecontroller_owned && g_sdl.quit_subsystem)
        g_sdl.quit_subsystem(SDV_SDL_INIT_GAMECONTROLLER);
    if (g_video_owned && g_sdl.quit_subsystem)
        g_sdl.quit_subsystem(SDV_SDL_INIT_VIDEO);
    g_gamecontroller_owned = 0;
    g_video_owned = 0;
    g_width = 1280;
    g_height = 720;
    g_right_cursor_x = 640.0f;
    g_right_cursor_y = 360.0f;
    g_right_cursor_visible = 0;
    g_swap_count = 0;
    fprintf(stderr, "[sdv-egl] shutdown complete\n");
    pthread_mutex_unlock(&g_lock);
}

int sdv_egl_ready(void)
{
    int ready;

    pthread_mutex_lock(&g_lock);
    ready = g_window != NULL && g_context != NULL;
    pthread_mutex_unlock(&g_lock);
    return ready;
}

void *sdv_egl_window(void)
{
    void *window;

    pthread_mutex_lock(&g_lock);
    window = g_window;
    pthread_mutex_unlock(&g_lock);
    return window;
}

int sdv_egl_width(void)
{
    int width;

    pthread_mutex_lock(&g_lock);
    width = g_width;
    pthread_mutex_unlock(&g_lock);
    return width;
}

int sdv_egl_height(void)
{
    int height;

    pthread_mutex_lock(&g_lock);
    height = g_height;
    pthread_mutex_unlock(&g_lock);
    return height;
}

int sdv_egl_poll_gamepad(SdvGamepadState *state)
{
    int connected = 0;

    if (!state) return 0;
    memset(state, 0, sizeof(*state));
    pthread_mutex_lock(&g_lock);

    if (g_sdl.pump_events) g_sdl.pump_events();
    if (g_sdl.poll_event) {
        union {
            long double alignment;
            unsigned char bytes[128];
        } event;
        while (g_sdl.poll_event(event.bytes)) {
            uint32_t type;
            memcpy(&type, event.bytes, sizeof(type));
            if (type == 0x100u) { /* SDL_QUIT */
                pthread_mutex_unlock(&g_lock);
                return -1;
            }
        }
    }
    if (g_gamepad && g_sdl.game_controller_attached &&
        !g_sdl.game_controller_attached(g_gamepad)) {
        if (g_sdl.game_controller_close)
            g_sdl.game_controller_close(g_gamepad);
        g_gamepad = NULL;
        fprintf(stderr, "[sdv-input] controller disconnected\n");
    }
    if (!g_gamepad && g_sdl.num_joysticks && g_sdl.is_game_controller &&
        g_sdl.game_controller_open) {
        int count = g_sdl.num_joysticks();
        for (int i = 0; i < count; ++i) {
            if (!g_sdl.is_game_controller(i)) continue;
            g_gamepad = g_sdl.game_controller_open(i);
            if (g_gamepad) {
                fprintf(stderr, "[sdv-input] controller: %s\n",
                        g_sdl.game_controller_name
                            ? g_sdl.game_controller_name(g_gamepad) : "unknown");
                break;
            }
        }
    }
    if (g_gamepad && g_sdl.game_controller_get_button &&
        g_sdl.game_controller_get_axis) {
        if (g_sdl.game_controller_update) g_sdl.game_controller_update();
        for (int button = 0; button <= 14; ++button)
            if (g_sdl.game_controller_get_button(g_gamepad, button))
                state->buttons |= 1u << button;
        state->left_x = g_sdl.game_controller_get_axis(g_gamepad, 0);
        state->left_y = g_sdl.game_controller_get_axis(g_gamepad, 1);
        state->right_x = g_sdl.game_controller_get_axis(g_gamepad, 2);
        state->right_y = g_sdl.game_controller_get_axis(g_gamepad, 3);
        state->left_trigger = g_sdl.game_controller_get_axis(g_gamepad, 4);
        state->right_trigger = g_sdl.game_controller_get_axis(g_gamepad, 5);
        connected = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return connected;
}

void sdv_egl_set_right_cursor(float x, float y, int visible)
{
    pthread_mutex_lock(&g_lock);
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    if (g_width > 0 && x > (float)(g_width - 1))
        x = (float)(g_width - 1);
    if (g_height > 0 && y > (float)(g_height - 1))
        y = (float)(g_height - 1);
    g_right_cursor_x = x;
    g_right_cursor_y = y;
    g_right_cursor_visible = visible != 0;
    pthread_mutex_unlock(&g_lock);
}

void *sdv_egl_get_proc_address(const char *name)
{
    if (!name || !g_symbols_ready || !g_sdl.gl_get_proc_address)
        return NULL;
    return g_sdl.gl_get_proc_address(name);
}
