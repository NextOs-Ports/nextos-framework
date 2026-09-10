/*
 * SDL2-backed OpenGL ES bridge for Stardew Valley.
 *
 * There is intentionally no SDL/GLES include or link dependency in this
 * translation unit. main.c loads SDL2 with RTLD_GLOBAL; this bridge consumes
 * that already-loaded API through dlsym(RTLD_DEFAULT).
 */

#include "sdv_egl_bridge.h"
/* Recibo de video canonico do framework (nxgl). E ele que decide OK/BLACK e
 * publica o org.nextos.nxruntime.video-proof que o watchdog do launcher le. */
#include "nxgl_frame_proof_adapter.h"

#ifndef PORT_WINDOW_TITLE
#define PORT_WINDOW_TITLE "nextos_port"
#endif
#include "present_fbo.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern int sdv_gl_copy_known_framebuffers(unsigned int *out, int capacity);
extern int sdv_gl_texture_dimensions(unsigned int id, int *w, int *h);

typedef struct SDL_Window SDL_Window;
typedef void *SDL_GLContext;
typedef struct SDL_GameController SDL_GameController;
typedef struct SDL_Joystick SDL_Joystick;

typedef struct {
    uint32_t format;
    int w;
    int h;
    int refresh_rate;
    void *driverdata;
} SdvSDLDisplayMode;

enum {
    SB_SDL_INIT_VIDEO = 0x00000020u,
    SB_SDL_INIT_GAMECONTROLLER = 0x00002000u,
    SB_SDL_WINDOW_FULLSCREEN = 0x00000001u,
    SB_SDL_WINDOW_OPENGL = 0x00000002u,
    SB_SDL_WINDOWPOS_CENTERED = 0x2fff0000u,

    SB_SDL_GL_RED_SIZE = 0,
    SB_SDL_GL_GREEN_SIZE = 1,
    SB_SDL_GL_BLUE_SIZE = 2,
    SB_SDL_GL_ALPHA_SIZE = 3,
    SB_SDL_GL_DOUBLEBUFFER = 5,
    SB_SDL_GL_DEPTH_SIZE = 6,
    SB_SDL_GL_STENCIL_SIZE = 7,
    SB_SDL_GL_CONTEXT_MAJOR_VERSION = 17,
    SB_SDL_GL_CONTEXT_MINOR_VERSION = 18,
    SB_SDL_GL_CONTEXT_PROFILE_MASK = 21,
    SB_SDL_GL_CONTEXT_PROFILE_ES = 0x0004
};

typedef struct {
    int (*init_subsystem)(uint32_t flags);
    uint32_t (*was_init)(uint32_t flags);
    void (*quit_subsystem)(uint32_t flags);
    int (*get_desktop_display_mode)(int display_index,
                                    SdvSDLDisplayMode *mode);
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
    SDL_Joystick *(*game_controller_get_joystick)(
        SDL_GameController *controller);
    int (*game_controller_attached)(SDL_GameController *controller);
    void (*game_controller_update)(void);
    unsigned char (*game_controller_get_button)(SDL_GameController *controller,
                                                 int button);
    short (*game_controller_get_axis)(SDL_GameController *controller, int axis);
    int (*joystick_instance_id)(SDL_Joystick *joystick);
    int (*joystick_get_device_instance_id)(int joystick_index);
} SdvSDLApi;

typedef struct {
    uint32_t magic;
    unsigned int generation;
} SdvEglSurface;

#define SB_SURFACE_MAGIC UINT32_C(0x53445653)

static SdvSDLApi g_sdl;
static SDL_Window *g_window;
static SDL_GLContext g_context;
#define SB_MAX_GAMEPADS 4

static SDL_GameController *g_gamepads[SB_MAX_GAMEPADS];
static int g_gamepad_instance_ids[SB_MAX_GAMEPADS];
static unsigned int g_gamepad_mask;
static double g_next_gamepad_scan;
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

static double sdv_clock_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0.0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

/* Telemetria opt-in de baixo custo para separar "30 FPS estáveis" de
 * microtravadas. Uma linha a cada 300 swaps; o launcher final deixa off. */
/* --- prova de imagem imediatamente ANTES do present ---------------------
 *
 * "Tela preta com audio" e' falha terminal, e log de draw e' cego: so a
 * leitura dos pixels que vao ser apresentados prova que ha imagem. Esta sonda
 * le o framebuffer padrao logo antes do eglSwapBuffers, resume a estatistica
 * no log e grava o quadro para inspecao.
 *
 * Custo: glReadPixels serializa a GPU, e no Mali isso e' caro. Por isso ela e'
 * OPT-IN (SB_SHOT_DIR) e so dispara nos quadros listados em SB_SHOT_AT --
 * nunca no caminho normal de um pacote publicado.
 */
static void sdv_shot_before_present(void)
{
    static int configured, armed;
    static const char *dir;
    static unsigned wanted[16];
    static int wanted_n;

    if (!configured) {
        configured = 1;
        dir = getenv("SB_SHOT_DIR");
        const char *at = getenv("SB_SHOT_AT");
        if (dir && dir[0] && at && at[0]) {
            const char *p = at;
            while (*p && wanted_n < 16) {
                wanted[wanted_n++] = (unsigned)strtoul(p, NULL, 10);
                const char *c = strchr(p, ',');
                if (!c) break;
                p = c + 1;
            }
            armed = wanted_n > 0;
        }
    }
    if (!armed) return;

    int hit = 0;
    for (int i = 0; i < wanted_n; i++)
        if (wanted[i] == g_swap_count + 1) { hit = 1; break; }
    if (!hit) return;

    static void (*p_read)(int, int, int, int, unsigned, unsigned, void *);
    if (!p_read) {
        void *sym = dlsym(RTLD_DEFAULT, "glReadPixels");
        memcpy(&p_read, &sym, sizeof(p_read));
        if (!p_read) { armed = 0; return; }
    }

    int w = g_width > 0 ? g_width : 640;
    int h = g_height > 0 ? g_height : 480;
    size_t n = (size_t)w * (size_t)h * 4u;
    unsigned char *px = (unsigned char *)malloc(n);
    if (!px) return;

    p_read(0, 0, w, h, 0x1908u /* RGBA */, 0x1401u /* UNSIGNED_BYTE */, px);

    unsigned long long sum = 0;
    unsigned nonblack = 0;
    /* Assinatura de cor barata: quantos baldes 4x4x4 aparecem. Um quadro
     * chapado cai em 1-2 baldes; uma tela de jogo real passa de dezenas. */
    unsigned char seen[64];
    memset(seen, 0, sizeof(seen));
    unsigned buckets = 0;
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        unsigned r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];
        sum += r + g + b;
        if (r > 8 || g > 8 || b > 8) nonblack++;
        unsigned k = ((r >> 6) << 4) | ((g >> 6) << 2) | (b >> 6);
        if (!seen[k]) { seen[k] = 1; buckets++; }
    }
    double mean = (double)sum / ((double)w * h * 3.0);

    char path[512];
    snprintf(path, sizeof(path), "%s/frame_%06u.ppm", dir, g_swap_count + 1);
    FILE *f = fopen(path, "wb");
    if (f) {
        /* PPM com as linhas ja invertidas: o GL entrega de baixo p/ cima. */
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--)
            for (int x = 0; x < w; x++) {
                const unsigned char *q = px + ((size_t)y * w + x) * 4;
                fwrite(q, 1, 3, f);
            }
        fclose(f);
    }
    fprintf(stderr,
            "[sdv-shot] quadro=%u %dx%d nao_pretos=%u/%u (%.1f%%) media=%.1f "
            "baldes_de_cor=%u arquivo=%s\n",
            g_swap_count + 1, w, h, nonblack, (unsigned)(w * h),
            100.0 * nonblack / (double)(w * h), mean, buckets,
            f ? path : "(falhou)");
    free(px);
}

static void sdv_trace_frame_pacing(void)
{
    static int initialized, enabled;
    static double window_start, last_swap, max_interval;
    static unsigned int window_frames, late_frames;
    if (!initialized) {
        const char *v = getenv("SB_FPS_TRACE");
        enabled = v && v[0] && v[0] != '0';
        initialized = 1;
    }
    if (!enabled) return;
    double now = sdv_clock_seconds();
    if (g_swap_count <= 1 || window_start == 0.0 || now <= last_swap) {
        window_start = last_swap = now;
        window_frames = late_frames = 0;
        max_interval = 0.0;
        return;
    }
    double interval = now - last_swap;
    last_swap = now;
    if (interval > max_interval) max_interval = interval;
    if (interval > 0.042) late_frames++;
    window_frames++;
    if (window_frames >= 300) {
        double elapsed = now - window_start;
        fprintf(stderr,
                "[sdv-fps] %.2f fps max=%.1fms late=%u/300 swaps=%u\n",
                elapsed > 0.0 ? window_frames / elapsed : 0.0,
                max_interval * 1000.0, late_frames, g_swap_count);
        window_start = last_swap = now;
        window_frames = late_frames = 0;
        max_interval = 0.0;
    }
}

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
#define SB_RESOLVE_FUNCTION(member, name, required)                         \
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
    SB_RESOLVE_FUNCTION(init_subsystem, "SDL_InitSubSystem", 1);
    SB_RESOLVE_FUNCTION(was_init, "SDL_WasInit", 0);
    SB_RESOLVE_FUNCTION(quit_subsystem, "SDL_QuitSubSystem", 0);
    SB_RESOLVE_FUNCTION(get_desktop_display_mode,
                         "SDL_GetDesktopDisplayMode", 0);
    SB_RESOLVE_FUNCTION(get_current_video_driver,
                         "SDL_GetCurrentVideoDriver", 0);
    SB_RESOLVE_FUNCTION(get_error, "SDL_GetError", 0);
    SB_RESOLVE_FUNCTION(gl_reset_attributes, "SDL_GL_ResetAttributes", 0);
    SB_RESOLVE_FUNCTION(gl_set_attribute, "SDL_GL_SetAttribute", 1);
    SB_RESOLVE_FUNCTION(create_window, "SDL_CreateWindow", 1);
    SB_RESOLVE_FUNCTION(destroy_window, "SDL_DestroyWindow", 1);
    SB_RESOLVE_FUNCTION(gl_create_context, "SDL_GL_CreateContext", 1);
    SB_RESOLVE_FUNCTION(gl_make_current, "SDL_GL_MakeCurrent", 1);
    SB_RESOLVE_FUNCTION(gl_delete_context, "SDL_GL_DeleteContext", 1);
    SB_RESOLVE_FUNCTION(gl_swap_window, "SDL_GL_SwapWindow", 1);
    SB_RESOLVE_FUNCTION(gl_set_swap_interval,
                         "SDL_GL_SetSwapInterval", 0);
    SB_RESOLVE_FUNCTION(gl_get_proc_address, "SDL_GL_GetProcAddress", 1);
    SB_RESOLVE_FUNCTION(pump_events, "SDL_PumpEvents", 0);
    SB_RESOLVE_FUNCTION(poll_event, "SDL_PollEvent", 0);
    SB_RESOLVE_FUNCTION(num_joysticks, "SDL_NumJoysticks", 0);
    SB_RESOLVE_FUNCTION(is_game_controller, "SDL_IsGameController", 0);
    SB_RESOLVE_FUNCTION(game_controller_open, "SDL_GameControllerOpen", 0);
    SB_RESOLVE_FUNCTION(game_controller_close, "SDL_GameControllerClose", 0);
    SB_RESOLVE_FUNCTION(game_controller_name, "SDL_GameControllerName", 0);
    SB_RESOLVE_FUNCTION(game_controller_get_joystick,
                         "SDL_GameControllerGetJoystick", 0);
    SB_RESOLVE_FUNCTION(game_controller_attached,
                         "SDL_GameControllerGetAttached", 0);
    SB_RESOLVE_FUNCTION(game_controller_update,
                         "SDL_GameControllerUpdate", 0);
    SB_RESOLVE_FUNCTION(game_controller_get_button,
                         "SDL_GameControllerGetButton", 0);
    SB_RESOLVE_FUNCTION(game_controller_get_axis,
                         "SDL_GameControllerGetAxis", 0);
    SB_RESOLVE_FUNCTION(joystick_instance_id, "SDL_JoystickInstanceID", 0);
    SB_RESOLVE_FUNCTION(joystick_get_device_instance_id,
                         "SDL_JoystickGetDeviceInstanceID", 0);

    if (!ok) {
        memset(&g_sdl, 0, sizeof(g_sdl));
        return 0;
    }

    g_symbols_ready = 1;
    return 1;
}

#undef SB_RESOLVE_FUNCTION

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

static void sdv_set_gl_attributes(int alpha, int depth, int stencil)
{
    if (g_sdl.gl_reset_attributes)
        g_sdl.gl_reset_attributes();

    g_sdl.gl_set_attribute(SB_SDL_GL_CONTEXT_PROFILE_MASK,
                           SB_SDL_GL_CONTEXT_PROFILE_ES);
    g_sdl.gl_set_attribute(SB_SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    g_sdl.gl_set_attribute(SB_SDL_GL_CONTEXT_MINOR_VERSION, 0);
    g_sdl.gl_set_attribute(SB_SDL_GL_RED_SIZE, 8);
    g_sdl.gl_set_attribute(SB_SDL_GL_GREEN_SIZE, 8);
    g_sdl.gl_set_attribute(SB_SDL_GL_BLUE_SIZE, 8);
    g_sdl.gl_set_attribute(SB_SDL_GL_ALPHA_SIZE, alpha);
    g_sdl.gl_set_attribute(SB_SDL_GL_DEPTH_SIZE, depth);
    g_sdl.gl_set_attribute(SB_SDL_GL_STENCIL_SIZE, stencil);
    g_sdl.gl_set_attribute(SB_SDL_GL_DOUBLEBUFFER, 1);
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

    initialized = g_sdl.was_init ? g_sdl.was_init(SB_SDL_INIT_VIDEO) : 0;
    if ((initialized & SB_SDL_INIT_VIDEO) == 0) {
        if (g_sdl.init_subsystem(SB_SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "[sdv-egl] SDL video init failed: %s\n",
                    sdv_sdl_error());
            goto out;
        }
        g_video_owned = 1;
    }
    initialized = g_sdl.was_init
        ? g_sdl.was_init(SB_SDL_INIT_GAMECONTROLLER) : 0;
    if ((initialized & SB_SDL_INIT_GAMECONTROLLER) == 0 &&
        g_sdl.init_subsystem(SB_SDL_INIT_GAMECONTROLLER) == 0)
        g_gamecontroller_owned = 1;

    g_width = 1280;
    g_height = 720;
    memset(&desktop, 0, sizeof(desktop));
    if (g_sdl.get_desktop_display_mode &&
        g_sdl.get_desktop_display_mode(0, &desktop) == 0 &&
        desktop.w > 0 && desktop.h > 0) {
        g_width = desktop.w;
        g_height = desktop.h;
    }
    {
        const char *forced_width = getenv("SB_WIDTH");
        const char *forced_height = getenv("SB_HEIGHT");
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
        g_window = g_sdl.create_window(
            PORT_WINDOW_TITLE, (int)SB_SDL_WINDOWPOS_CENTERED,
            (int)SB_SDL_WINDOWPOS_CENTERED, g_width, g_height,
            SB_SDL_WINDOW_OPENGL | SB_SDL_WINDOW_FULLSCREEN);
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

        {
            const char *forced_interval = getenv("SB_SWAP_INTERVAL");
            int has_override = forced_interval && forced_interval[0];
            int interval = has_override ? atoi(forced_interval) : 1;

            if (g_sdl.gl_set_swap_interval) {
                int swap_result = g_sdl.gl_set_swap_interval(interval);
                fprintf(stderr,
                        "[sdv-egl] swap interval=%d result=%d%s\n",
                        interval, swap_result,
                        has_override ? " [SB_SWAP_INTERVAL]" : "");
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
        /* O recibo do framework so pode nomear a janela e a pilha GL depois
         * que elas existem. */
        nxgl_frame_proof_set_video_context(
            g_width, g_height,
            g_sdl.get_current_video_driver
                ? g_sdl.get_current_video_driver() : NULL,
            "gles2", version);
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
        surface->magic = SB_SURFACE_MAGIC;
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
    typedef unsigned char (*IsFramebufferFn)(unsigned int);
    typedef void (*BindFramebufferFn)(unsigned int, unsigned int);
    typedef unsigned int (*CheckFramebufferStatusFn)(unsigned int);
    typedef void (*GetFramebufferAttachmentParameterivFn)(unsigned int,
                                                           unsigned int,
                                                           unsigned int,
                                                           int *);
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
    static IsFramebufferFn is_framebuffer;
    static BindFramebufferFn bind_framebuffer;
    static CheckFramebufferStatusFn check_framebuffer_status;
    static GetFramebufferAttachmentParameterivFn get_attachment;
    static int resolved;
    static unsigned int diagnostic_count;
    static int capture_done;

#define SB_GL_RESOLVE(dst, name) do {                                      \
        void *p_ = g_sdl.gl_get_proc_address(name);                          \
        memcpy(&(dst), &p_, sizeof(dst));                                    \
    } while (0)
    if (!resolved) {
        resolved = 1;
        SB_GL_RESOLVE(get_integerv, "glGetIntegerv");
        SB_GL_RESOLVE(get_booleanv, "glGetBooleanv");
        SB_GL_RESOLVE(get_floatv, "glGetFloatv");
        SB_GL_RESOLVE(is_enabled, "glIsEnabled");
        SB_GL_RESOLVE(enable, "glEnable");
        SB_GL_RESOLVE(disable, "glDisable");
        SB_GL_RESOLVE(color_mask, "glColorMask");
        SB_GL_RESOLVE(clear_color, "glClearColor");
        SB_GL_RESOLVE(clear, "glClear");
        SB_GL_RESOLVE(read_pixels, "glReadPixels");
        SB_GL_RESOLVE(is_framebuffer, "glIsFramebuffer");
        SB_GL_RESOLVE(bind_framebuffer, "glBindFramebuffer");
        SB_GL_RESOLVE(check_framebuffer_status,
                        "glCheckFramebufferStatus");
        SB_GL_RESOLVE(get_attachment,
                        "glGetFramebufferAttachmentParameteriv");
    }
#undef SB_GL_RESOLVE

    if (!get_integerv) return;
    int fbo = -1;
    get_integerv(0x8ca6u /* GL_FRAMEBUFFER_BINDING */, &fbo);

    const char *trace = getenv("SB_GL_TRACE");
    if (trace && trace[0] && trace[0] != '0' && read_pixels &&
        (diagnostic_count < 10 ||
         (g_swap_count >= 2400 && (g_swap_count % 120) == 0))) {
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

    const char *capture_path = getenv("SB_GL_CAPTURE");
    if (!capture_done && capture_path && capture_path[0] && read_pixels &&
        g_swap_count >= 3000) {
        int viewport[4] = {0, 0, 0, 0};
        get_integerv(0x0ba2u /* GL_VIEWPORT */, viewport);
        if (viewport[2] > 0 && viewport[3] > 0) {
            size_t pixels = (size_t)viewport[2] * (size_t)viewport[3];
            size_t bytes = pixels * 4u;
            unsigned char *rgba = (unsigned char *)malloc(bytes);
            if (rgba) {
                uint64_t rgb_sum = 0;
                size_t nonblack = 0;
                read_pixels(viewport[0], viewport[1], viewport[2], viewport[3],
                            0x1908u /* GL_RGBA */,
                            0x1401u /* GL_UNSIGNED_BYTE */, rgba);
                for (size_t i = 0; i < bytes; i += 4) {
                    rgb_sum += rgba[i] + rgba[i + 1] + rgba[i + 2];
                    if (rgba[i] || rgba[i + 1] || rgba[i + 2])
                        ++nonblack;
                }
                FILE *file = fopen(capture_path, "wb");
                if (file) {
                    size_t written = fwrite(rgba, 1, bytes, file);
                    fclose(file);
                    fprintf(stderr,
                            "[sdv-egl] capture %s %dx%d bytes=%zu/%zu nonblack=%zu/%zu rgb_sum=%llu\n",
                            capture_path, viewport[2], viewport[3], written,
                            bytes, nonblack, pixels,
                            (unsigned long long)rgb_sum);
                } else {
                    fprintf(stderr, "[sdv-egl] capture open failed: %s\n",
                            capture_path);
                }
                free(rgba);

                /* Apenas FBOs devolvidos por glGenFramebuffers do jogo. A
                 * dimensao vem da textura colorida registrada no upload, de
                 * modo que nenhum glReadPixels ultrapassa o attachment. */
                if (getenv("SB_FBO_TRACK") && is_framebuffer &&
                    bind_framebuffer && check_framebuffer_status &&
                    get_attachment) {
                    unsigned int ids[32];
                    int count = sdv_gl_copy_known_framebuffers(ids, 32);
                    for (int n = 0; n < count; ++n) {
                        unsigned int id = ids[n];
                        if (!is_framebuffer(id)) continue;
                        bind_framebuffer(0x8D40u /* GL_FRAMEBUFFER */, id);
                        unsigned int status = check_framebuffer_status(
                            0x8D40u /* GL_FRAMEBUFFER */);
                        int object_type = 0, texture = 0, fw = 0, fh = 0;
                        get_attachment(0x8D40u /* GL_FRAMEBUFFER */,
                                       0x8CE0u /* GL_COLOR_ATTACHMENT0 */,
                                       0x8CD0u /* OBJECT_TYPE */,
                                       &object_type);
                        get_attachment(0x8D40u /* GL_FRAMEBUFFER */,
                                       0x8CE0u /* GL_COLOR_ATTACHMENT0 */,
                                       0x8CD1u /* OBJECT_NAME */, &texture);
                        if (status != 0x8CD5u /* COMPLETE */ ||
                            object_type != 0x1702u /* GL_TEXTURE */ ||
                            !sdv_gl_texture_dimensions((unsigned)texture,
                                                      &fw, &fh)) {
                            fprintf(stderr,
                                    "[sdv-egl] capture known-fbo=%u status=%x type=%x tex=%d size=?\n",
                                    id, status, object_type, texture);
                            continue;
                        }
                        int cw = fw < 640 ? fw : 640;
                        int ch = fh < 360 ? fh : 360;
                        int cx = (fw - cw) / 2, cy = (fh - ch) / 2;
                        size_t cpixels = (size_t)cw * (size_t)ch;
                        size_t cbytes = cpixels * 4u;
                        unsigned char *fb = (unsigned char *)malloc(cbytes);
                        if (!fb) continue;
                        read_pixels(cx, cy, cw, ch, 0x1908u /* RGBA */,
                                    0x1401u /* UBYTE */, fb);
                        uint64_t csum = 0;
                        size_t cnonblack = 0;
                        for (size_t i = 0; i < cbytes; i += 4) {
                            csum += fb[i] + fb[i + 1] + fb[i + 2];
                            if (fb[i] || fb[i + 1] || fb[i + 2])
                                ++cnonblack;
                        }
                        char path[512];
                        snprintf(path, sizeof path, "%s.fbo%u", capture_path,
                                 id);
                        FILE *ff = fopen(path, "wb");
                        if (ff) {
                            fwrite(fb, 1, cbytes, ff);
                            fclose(ff);
                        }
                        fprintf(stderr,
                                "[sdv-egl] capture known-fbo=%u tex=%d full=%dx%d crop=%dx%d nonblack=%zu/%zu rgb_sum=%llu\n",
                                id, texture, fw, fh, cw, ch, cnonblack,
                                cpixels, (unsigned long long)csum);
                        free(fb);
                    }
                    bind_framebuffer(0x8D40u /* GL_FRAMEBUFFER */,
                                     fbo >= 0 ? (unsigned int)fbo : 0u);
                }
            }
            capture_done = 1;
        }
    }

    const char *force = getenv("SB_FORCE_ALPHA");
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

/* Crosshair pequeno e independente do cursor/foco do Stardew. glClear com
 * scissor evita shader/VBO extra e, importante no Mali antigo, restaura todo
 * estado GL tocado antes de devolver o backbuffer ao SDL. */
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

#define SB_CURSOR_GL_RESOLVE(dst, name) do {                               \
        void *p_ = g_sdl.gl_get_proc_address(name);                         \
        memcpy(&(dst), &p_, sizeof(dst));                                   \
    } while (0)
    if (!resolved) {
        resolved = 1;
        SB_CURSOR_GL_RESOLVE(get_integerv, "glGetIntegerv");
        SB_CURSOR_GL_RESOLVE(get_booleanv, "glGetBooleanv");
        SB_CURSOR_GL_RESOLVE(get_floatv, "glGetFloatv");
        SB_CURSOR_GL_RESOLVE(is_enabled, "glIsEnabled");
        SB_CURSOR_GL_RESOLVE(enable, "glEnable");
        SB_CURSOR_GL_RESOLVE(disable, "glDisable");
        SB_CURSOR_GL_RESOLVE(scissor, "glScissor");
        SB_CURSOR_GL_RESOLVE(color_mask, "glColorMask");
        SB_CURSOR_GL_RESOLVE(clear_color, "glClearColor");
        SB_CURSOR_GL_RESOLVE(clear, "glClear");
    }
#undef SB_CURSOR_GL_RESOLVE
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

#define SB_CURSOR_RECT(left_, top_, width_, height_, r_, g_, b_) do {       \
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
    SB_CURSOR_RECT(x - 13, y - 2, 27, 5, 0.0f, 0.0f, 0.0f);
    SB_CURSOR_RECT(x - 2, y - 13, 5, 27, 0.0f, 0.0f, 0.0f);
    SB_CURSOR_RECT(x - 11, y, 23, 1, 1.0f, 1.0f, 1.0f);
    SB_CURSOR_RECT(x, y - 11, 1, 23, 1.0f, 1.0f, 1.0f);
    SB_CURSOR_RECT(x - 1, y - 1, 3, 3, 0.2f, 0.9f, 1.0f);
#undef SB_CURSOR_RECT

    clear_color(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
    color_mask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
    scissor(old_scissor_box[0], old_scissor_box[1], old_scissor_box[2],
            old_scissor_box[3]);
    if (!old_scissor) disable(0x0c11u /* GL_SCISSOR_TEST */);
}

/* jni_shim.h; implementado em main.c. Roda na thread do game loop (o swap e
 * chamado dela), exatamente a thread que o fix precisa registrar. */
void sdv_fix_paris_mainthread(void);

int sdv_egl_swap(void *surface)
{
    int result = 0;

    sdv_fix_paris_mainthread();
    pthread_mutex_lock(&g_lock);
    if (surface && g_window && g_context && g_sdl.gl_swap_window) {
        sb_present_fullsize_fbo(g_width, g_height);
        sdv_prepare_present();
        sdv_draw_right_cursor();
        /* Recibo do framework: amostra por conta propria num cronograma
         * esparso e e' quem publica o veredito de imagem. */
        nxgl_frame_proof_before_present(g_width, g_height);
        sdv_shot_before_present();
        g_sdl.gl_swap_window(g_window);
        ++g_swap_count;
        sdv_trace_frame_pacing();
        const char *trace = getenv("SB_GL_TRACE");
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
    if (logical_surface->magic == SB_SURFACE_MAGIC) {
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
    for (int i = 0; i < SB_MAX_GAMEPADS; ++i) {
        if (g_gamepads[i] && g_sdl.game_controller_close)
            g_sdl.game_controller_close(g_gamepads[i]);
        g_gamepads[i] = NULL;
        g_gamepad_instance_ids[i] = -1;
    }
    g_gamepad_mask = 0;
    g_next_gamepad_scan = 0.0;
    sdv_drop_candidate();
    if (g_gamecontroller_owned && g_sdl.quit_subsystem)
        g_sdl.quit_subsystem(SB_SDL_INIT_GAMECONTROLLER);
    if (g_video_owned && g_sdl.quit_subsystem)
        g_sdl.quit_subsystem(SB_SDL_INIT_VIDEO);
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

static int sdv_gamepad_instance_id(SDL_GameController *controller)
{
    SDL_Joystick *joystick;

    if (!controller || !g_sdl.game_controller_get_joystick ||
        !g_sdl.joystick_instance_id)
        return -1;
    joystick = g_sdl.game_controller_get_joystick(controller);
    return joystick ? g_sdl.joystick_instance_id(joystick) : -1;
}

static int sdv_gamepad_already_open(SDL_GameController *controller,
                                    int instance_id)
{
    for (int i = 0; i < SB_MAX_GAMEPADS; ++i) {
        if (!g_gamepads[i]) continue;
        if (controller && g_gamepads[i] == controller) return 1;
        if (instance_id >= 0 &&
            g_gamepad_instance_ids[i] == instance_id) return 1;
    }
    return 0;
}

static int sdv_free_gamepad_slot(void)
{
    for (int i = 0; i < SB_MAX_GAMEPADS; ++i)
        if (!g_gamepads[i]) return i;
    return -1;
}

/* Executado sob g_lock. SDL retorna o mesmo GameController (com refcount)
 * quando um indice ja esta aberto; por isso fechamos candidatos duplicados. */
static void sdv_rescan_gamepads(double now)
{
    int count;

    g_next_gamepad_scan = now + 1.0;
    if (!g_sdl.num_joysticks || !g_sdl.is_game_controller ||
        !g_sdl.game_controller_open)
        return;
    count = g_sdl.num_joysticks();
    for (int device = 0; device < count; ++device) {
        int slot = sdv_free_gamepad_slot();
        int device_instance = -1;
        int opened_instance;
        SDL_GameController *controller;

        if (slot < 0) break;
        if (!g_sdl.is_game_controller(device)) continue;
        if (g_sdl.joystick_get_device_instance_id) {
            device_instance =
                g_sdl.joystick_get_device_instance_id(device);
            if (sdv_gamepad_already_open(NULL, device_instance)) continue;
        }
        controller = g_sdl.game_controller_open(device);
        if (!controller) continue;
        opened_instance = sdv_gamepad_instance_id(controller);
        if (opened_instance < 0) opened_instance = device_instance;
        if (sdv_gamepad_already_open(controller, opened_instance)) {
            if (g_sdl.game_controller_close)
                g_sdl.game_controller_close(controller);
            continue;
        }
        g_gamepads[slot] = controller;
        g_gamepad_instance_ids[slot] = opened_instance;
        fprintf(stderr,
                "[sdv-input] controller P%d: %s (device=%d instance=%d)\n",
                slot + 1,
                g_sdl.game_controller_name
                    ? g_sdl.game_controller_name(controller) : "unknown",
                device, opened_instance);
    }
}

int sdv_egl_poll_gamepads(SdvGamepadState *states, int capacity)
{
    unsigned int connected = 0;
    double now;

    if (!states || capacity <= 0) return 0;
    if (capacity > SB_MAX_GAMEPADS) capacity = SB_MAX_GAMEPADS;
    memset(states, 0, sizeof(*states) * (size_t)capacity);
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
            if (type >= 0x653u && type <= 0x655u)
                g_next_gamepad_scan = 0.0; /* add/remove/remap */
        }
    }
    for (int i = 0; i < SB_MAX_GAMEPADS; ++i) {
        if (!g_gamepads[i] || !g_sdl.game_controller_attached ||
            g_sdl.game_controller_attached(g_gamepads[i]))
            continue;
        if (g_sdl.game_controller_close)
            g_sdl.game_controller_close(g_gamepads[i]);
        g_gamepads[i] = NULL;
        g_gamepad_instance_ids[i] = -1;
        g_next_gamepad_scan = 0.0;
        fprintf(stderr, "[sdv-input] controller P%d disconnected\n", i + 1);
    }
    now = sdv_clock_seconds();
    if (g_next_gamepad_scan == 0.0 || now >= g_next_gamepad_scan)
        sdv_rescan_gamepads(now);
    if (g_sdl.game_controller_update) g_sdl.game_controller_update();
    if (g_sdl.game_controller_get_button &&
        g_sdl.game_controller_get_axis) {
        for (int i = 0; i < SB_MAX_GAMEPADS; ++i) {
            SdvGamepadState *state;
            if (!g_gamepads[i]) continue;
            connected |= 1u << i;
            if (i >= capacity) continue;
            state = &states[i];
            for (int button = 0; button <= 14; ++button)
                if (g_sdl.game_controller_get_button(g_gamepads[i], button))
                    state->buttons |= 1u << button;
            state->left_x =
                g_sdl.game_controller_get_axis(g_gamepads[i], 0);
            state->left_y =
                g_sdl.game_controller_get_axis(g_gamepads[i], 1);
            state->right_x =
                g_sdl.game_controller_get_axis(g_gamepads[i], 2);
            state->right_y =
                g_sdl.game_controller_get_axis(g_gamepads[i], 3);
            state->left_trigger =
                g_sdl.game_controller_get_axis(g_gamepads[i], 4);
            state->right_trigger =
                g_sdl.game_controller_get_axis(g_gamepads[i], 5);
        }
    }
    g_gamepad_mask = connected;
    pthread_mutex_unlock(&g_lock);
    return (int)connected;
}

void sdv_egl_set_gamepad_mask(unsigned int mask)
{
    pthread_mutex_lock(&g_lock);
    g_gamepad_mask = mask;
    pthread_mutex_unlock(&g_lock);
}

unsigned int sdv_egl_gamepad_mask(void)
{
    unsigned int mask;

    pthread_mutex_lock(&g_lock);
    mask = g_gamepad_mask;
    pthread_mutex_unlock(&g_lock);
    return mask;
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
