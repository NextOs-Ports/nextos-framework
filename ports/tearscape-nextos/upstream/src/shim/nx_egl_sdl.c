/* Port-local SDL2/KMSDRM EGL facade.  SDL2 is a firmware capability loaded at
 * runtime; this object has no SDL headers and no SDL DT_NEEDED dependency. */
#define _GNU_SOURCE
#include "nx_egl_sdl.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

extern void nx_log(const char *fmt, ...);

typedef struct SDL_Window SDL_Window;
typedef void *SDL_GLContext;
typedef uint32_t Uint32;

enum {
    NX_SDL_INIT_VIDEO = 0x00000020u,
    NX_SDL_WINDOW_OPENGL = 0x00000002u,
    NX_SDL_WINDOW_FULLSCREEN_DESKTOP = 0x00001001u,
    NX_SDL_WINDOWPOS_CENTERED = 0x2fff0000u,
    NX_SDL_GL_RED_SIZE = 0,
    NX_SDL_GL_GREEN_SIZE = 1,
    NX_SDL_GL_BLUE_SIZE = 2,
    NX_SDL_GL_ALPHA_SIZE = 3,
    NX_SDL_GL_DOUBLEBUFFER = 5,
    NX_SDL_GL_DEPTH_SIZE = 6,
    NX_SDL_GL_STENCIL_SIZE = 7,
    NX_SDL_GL_CONTEXT_MAJOR_VERSION = 17,
    NX_SDL_GL_CONTEXT_MINOR_VERSION = 18,
    NX_SDL_GL_CONTEXT_PROFILE_MASK = 21,
    NX_SDL_GL_SHARE_WITH_CURRENT_CONTEXT = 22,
    NX_SDL_GL_CONTEXT_PROFILE_ES = 0x0004
};

typedef struct {
    void *handle;
    int loaded;
    int (*InitSubSystem)(Uint32);
    void (*QuitSubSystem)(Uint32);
    const char *(*GetError)(void);
    const char *(*GetCurrentVideoDriver)(void);
    int (*GL_SetAttribute)(int, int);
    SDL_Window *(*CreateWindow)(const char *, int, int, int, int, Uint32);
    void (*DestroyWindow)(SDL_Window *);
    SDL_GLContext (*GL_CreateContext)(SDL_Window *);
    void (*GL_DeleteContext)(SDL_GLContext);
    int (*GL_MakeCurrent)(SDL_Window *, SDL_GLContext);
    void (*GL_SwapWindow)(SDL_Window *);
    int (*GL_SetSwapInterval)(int);
    void *(*GL_GetProcAddress)(const char *);
    void (*GL_GetDrawableSize)(SDL_Window *, int *, int *);
    void (*GetWindowSize)(SDL_Window *, int *, int *);
} NxSdlApi;

typedef struct NxContext {
    uint32_t magic;
    int client_version;
    SDL_GLContext real;
    struct NxContext *share;
    struct NxContext *next;
} NxContext;

typedef struct NxSurface {
    uint32_t magic;
    int window;
    int width;
    int height;
    struct NxSurface *next;
} NxSurface;

#define NX_CONTEXT_MAGIC 0x4d4d5843u
#define NX_SURFACE_MAGIC 0x4d4d5853u

static NxSdlApi api;
static int video_ready;
static SDL_Window *window;
static NxContext *contexts;
static NxSurface *surfaces;
static int drawable_width = 640;
static int drawable_height = 480;
static int format_alpha = 8;
static int format_depth = 24;
static int format_stencil = 8;
static char display_token;
static char config_token;
static _Thread_local NxContext *current_context;
static _Thread_local NxSurface *current_draw;
static _Thread_local NxSurface *current_read;
static _Thread_local EGLint last_error = EGL_SUCCESS;

int nx_sdl_egl_enabled(void)
{
    const char *value = getenv("NX_TEARSCAPE_SDL_EGL");
    return value && strcmp(value, "1") == 0;
}

static const char *error_text(void)
{
    const char *text = api.GetError ? api.GetError() : NULL;
    return text && text[0] ? text : "unknown SDL2 error";
}

static int load_sdl(void)
{
    static const char *const libraries[] = {
        "libSDL2-2.0.so.0", "libSDL2.so", NULL
    };
    if (api.loaded)
        return api.handle != NULL;
    api.loaded = 1;
    for (int i = 0; libraries[i] && !api.handle; ++i)
        api.handle = dlopen(libraries[i], RTLD_NOW | RTLD_LOCAL);
    if (!api.handle) {
        nx_log("sdl-egl: firmware SDL2 load failed: %s", dlerror());
        last_error = EGL_NOT_INITIALIZED;
        return 0;
    }

#define REQUIRED(field, symbol) do { \
        *(void **)(&api.field) = dlsym(api.handle, symbol); \
        if (!api.field) { \
            nx_log("sdl-egl: firmware SDL2 misses %s", symbol); \
            last_error = EGL_NOT_INITIALIZED; \
            dlclose(api.handle); \
            api.handle = NULL; \
            return 0; \
        } \
    } while (0)
    REQUIRED(InitSubSystem, "SDL_InitSubSystem");
    REQUIRED(QuitSubSystem, "SDL_QuitSubSystem");
    REQUIRED(GetError, "SDL_GetError");
    REQUIRED(GetCurrentVideoDriver, "SDL_GetCurrentVideoDriver");
    REQUIRED(GL_SetAttribute, "SDL_GL_SetAttribute");
    REQUIRED(CreateWindow, "SDL_CreateWindow");
    REQUIRED(DestroyWindow, "SDL_DestroyWindow");
    REQUIRED(GL_CreateContext, "SDL_GL_CreateContext");
    REQUIRED(GL_DeleteContext, "SDL_GL_DeleteContext");
    REQUIRED(GL_MakeCurrent, "SDL_GL_MakeCurrent");
    REQUIRED(GL_SwapWindow, "SDL_GL_SwapWindow");
    REQUIRED(GL_SetSwapInterval, "SDL_GL_SetSwapInterval");
    REQUIRED(GL_GetProcAddress, "SDL_GL_GetProcAddress");
#undef REQUIRED
    *(void **)(&api.GL_GetDrawableSize) =
        dlsym(api.handle, "SDL_GL_GetDrawableSize");
    *(void **)(&api.GetWindowSize) = dlsym(api.handle, "SDL_GetWindowSize");
    nx_log("sdl-egl: firmware SDL2 loaded dynamically");
    return 1;
}

static int init_video(void)
{
    if (video_ready)
        return 1;
    if (!load_sdl())
        return 0;
    if (api.InitSubSystem(NX_SDL_INIT_VIDEO) == 0) {
        video_ready = 1;
        nx_log("sdl-egl: video initialized driver=%s",
               api.GetCurrentVideoDriver() ?
               api.GetCurrentVideoDriver() : "(null)");
        return 1;
    }

    nx_log("sdl-egl: inherited video init failed: %s", error_text());
    const char *inherited = getenv("SDL_VIDEODRIVER");
    if (inherited && inherited[0]) {
        nx_log("sdl-egl: SDL_VIDEODRIVER=%s failed; retrying firmware auto",
               inherited);
        api.QuitSubSystem(NX_SDL_INIT_VIDEO);
        unsetenv("SDL_VIDEODRIVER");
        if (api.InitSubSystem(NX_SDL_INIT_VIDEO) == 0) {
            video_ready = 1;
            nx_log("sdl-egl: retry initialized driver=%s",
                   api.GetCurrentVideoDriver() ?
                   api.GetCurrentVideoDriver() : "(null)");
            return 1;
        }
        nx_log("sdl-egl: firmware-auto retry failed: %s", error_text());
    }
    last_error = EGL_NOT_INITIALIZED;
    return 0;
}

static void set_format(int alpha, int depth, int stencil)
{
    api.GL_SetAttribute(NX_SDL_GL_CONTEXT_PROFILE_MASK,
                        NX_SDL_GL_CONTEXT_PROFILE_ES);
    api.GL_SetAttribute(NX_SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    api.GL_SetAttribute(NX_SDL_GL_CONTEXT_MINOR_VERSION, 0);
    api.GL_SetAttribute(NX_SDL_GL_RED_SIZE, 8);
    api.GL_SetAttribute(NX_SDL_GL_GREEN_SIZE, 8);
    api.GL_SetAttribute(NX_SDL_GL_BLUE_SIZE, 8);
    api.GL_SetAttribute(NX_SDL_GL_ALPHA_SIZE, alpha);
    api.GL_SetAttribute(NX_SDL_GL_DEPTH_SIZE, depth);
    api.GL_SetAttribute(NX_SDL_GL_STENCIL_SIZE, stencil);
    api.GL_SetAttribute(NX_SDL_GL_DOUBLEBUFFER, 1);
}

static int create_real_context(NxContext *context)
{
    if (context->real)
        return 1;
    if (!window)
        return 1; /* The logical EGL context is lazy until a window exists. */
    if (context->share && !create_real_context(context->share))
        return 0;
    set_format(format_alpha, format_depth, format_stencil);
    if (context->share && context->share->real) {
        api.GL_MakeCurrent(window, context->share->real);
        api.GL_SetAttribute(NX_SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    } else {
        api.GL_SetAttribute(NX_SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    }
    context->real = api.GL_CreateContext(window);
    api.GL_SetAttribute(NX_SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    if (!context->real) {
        nx_log("sdl-egl: ES2 context creation failed: %s", error_text());
        return 0;
    }
    return 1;
}

static void drop_real_contexts(void)
{
    if (window)
        api.GL_MakeCurrent(window, NULL);
    for (NxContext *context = contexts; context; context = context->next) {
        if (context->real) {
            api.GL_DeleteContext(context->real);
            context->real = NULL;
        }
    }
    current_context = NULL;
    current_draw = NULL;
    current_read = NULL;
}

static int open_window(EGLNativeWindowType native_window)
{
    static const struct {
        int alpha;
        int depth;
        int stencil;
    } formats[] = {
        {8, 24, 8}, {8, 16, 0}, {0, 16, 0}, {0, 0, 0}
    };
    if (window)
        return 1;
    if (!init_video())
        return 0;

    if (native_window) {
        const uint16_t *size = (const uint16_t *)native_window;
        if (size[0] > 0 && size[0] < 16384 &&
            size[1] > 0 && size[1] < 16384) {
            drawable_width = size[0];
            drawable_height = size[1];
        }
    }

    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        format_alpha = formats[i].alpha;
        format_depth = formats[i].depth;
        format_stencil = formats[i].stencil;
        set_format(format_alpha, format_depth, format_stencil);
        window = api.CreateWindow(
            "Tearscape", (int)NX_SDL_WINDOWPOS_CENTERED,
            (int)NX_SDL_WINDOWPOS_CENTERED, drawable_width, drawable_height,
            NX_SDL_WINDOW_OPENGL | NX_SDL_WINDOW_FULLSCREEN_DESKTOP);
        if (!window) {
            nx_log("sdl-egl: fullscreen window a%d/d%d/s%d failed: %s",
                   format_alpha, format_depth, format_stencil, error_text());
            continue;
        }

        int contexts_ok = 1;
        for (NxContext *context = contexts; context; context = context->next) {
            if (!create_real_context(context)) {
                contexts_ok = 0;
                break;
            }
        }
        if (contexts_ok)
            break;
        drop_real_contexts();
        api.DestroyWindow(window);
        window = NULL;
    }
    if (!window) {
        last_error = EGL_BAD_ALLOC;
        return 0;
    }

    api.GL_MakeCurrent(window, NULL);
    int width = 0;
    int height = 0;
    if (api.GL_GetDrawableSize)
        api.GL_GetDrawableSize(window, &width, &height);
    if ((width <= 0 || height <= 0) && api.GetWindowSize)
        api.GetWindowSize(window, &width, &height);
    if (width > 0 && height > 0) {
        drawable_width = width;
        drawable_height = height;
    }
    nx_log("sdl-egl: driver=%s fullscreen=%dx%d ES2 rgba=8/8/8/%d depth=%d stencil=%d",
           api.GetCurrentVideoDriver() ?
           api.GetCurrentVideoDriver() : "(null)",
           drawable_width, drawable_height, format_alpha,
           format_depth, format_stencil);
    return 1;
}

static void remove_surface(NxSurface *surface)
{
    NxSurface **link = &surfaces;
    while (*link && *link != surface)
        link = &(*link)->next;
    if (*link)
        *link = surface->next;
}

static void remove_context(NxContext *context)
{
    NxContext **link = &contexts;
    while (*link && *link != context)
        link = &(*link)->next;
    if (*link)
        *link = context->next;
}

static void cleanup(void)
{
    drop_real_contexts();
    while (contexts) {
        NxContext *next = contexts->next;
        free(contexts);
        contexts = next;
    }
    while (surfaces) {
        NxSurface *next = surfaces->next;
        free(surfaces);
        surfaces = next;
    }
    if (window) {
        api.DestroyWindow(window);
        window = NULL;
    }
    if (video_ready) {
        api.QuitSubSystem(NX_SDL_INIT_VIDEO);
        video_ready = 0;
    }
}

static int write_all(int fd, const void *data, size_t size)
{
    const unsigned char *cursor = data;
    while (size > 0) {
        ssize_t written = write(fd, cursor, size);
        if (written <= 0)
            return 0;
        cursor += written;
        size -= (size_t)written;
    }
    return 1;
}

/* Explicit one-shot diagnostic only.  Nothing is read back during normal
 * gameplay; a caller must provide an absolute, not-yet-existing PPM path. */
static void capture_ppm_once(void)
{
    static int attempted;
    const char *path = getenv("NX_TEARSCAPE_SDL_CAPTURE_PPM");
    if (attempted || !path || path[0] != '/')
        return;

    const char *request = getenv("NX_TEARSCAPE_SDL_CAPTURE_REQUEST");
    if (request) {
        if (request[0] != '/') {
            nx_log("sdl-egl: capture request must be an absolute path");
            return;
        }
        int request_fd = open(request, O_RDONLY | O_NONBLOCK |
                              O_CLOEXEC | O_NOFOLLOW);
        if (request_fd < 0)
            return; /* Armed but not triggered yet. */
        struct stat request_metadata;
        int regular = syscall(SYS_fstat, request_fd, &request_metadata) == 0 &&
            S_ISREG(request_metadata.st_mode);
        close(request_fd);
        if (!regular)
            return;
        if (unlink(request) != 0) {
            nx_log("sdl-egl: capture request could not be consumed path=%s",
                   request);
            return;
        }
    }
    attempted = 1;
    if (drawable_width <= 0 || drawable_height <= 0 ||
        drawable_width > 8192 || drawable_height > 8192) {
        nx_log("sdl-egl: capture rejected invalid size %dx%d",
               drawable_width, drawable_height);
        return;
    }

    typedef void (*ReadPixels)(int, int, int, int, unsigned int,
                               unsigned int, void *);
    typedef void (*PixelStorei)(unsigned int, int);
    typedef void (*GetIntegerv)(unsigned int, int *);
    typedef unsigned int (*GetError)(void);
    ReadPixels read_pixels = (ReadPixels)api.GL_GetProcAddress("glReadPixels");
    PixelStorei pixel_store =
        (PixelStorei)api.GL_GetProcAddress("glPixelStorei");
    GetIntegerv get_integer =
        (GetIntegerv)api.GL_GetProcAddress("glGetIntegerv");
    GetError get_error = (GetError)api.GL_GetProcAddress("glGetError");
    if (!read_pixels || !pixel_store || !get_integer || !get_error) {
        nx_log("sdl-egl: capture unavailable (GL readback symbols missing)");
        return;
    }

    size_t rgba_row_size = (size_t)drawable_width * 4u;
    size_t rgb_row_size = (size_t)drawable_width * 3u;
    if (rgba_row_size / 4u != (size_t)drawable_width ||
        rgb_row_size / 3u != (size_t)drawable_width ||
        (size_t)drawable_height > SIZE_MAX / rgba_row_size) {
        nx_log("sdl-egl: capture size overflow");
        return;
    }
    size_t pixels_size = rgba_row_size * (size_t)drawable_height;
    unsigned char *pixels = malloc(pixels_size);
    unsigned char *rgb_row = malloc(rgb_row_size);
    if (!pixels || !rgb_row) {
        nx_log("sdl-egl: capture allocation failed");
        free(pixels);
        free(rgb_row);
        return;
    }
    for (int i = 0; i < 8 && get_error() != 0; ++i) {}
    int old_pack_alignment = 4;
    get_integer(0x0D05u /* GL_PACK_ALIGNMENT */, &old_pack_alignment);
    pixel_store(0x0D05u /* GL_PACK_ALIGNMENT */, 1);
    read_pixels(0, 0, drawable_width, drawable_height,
                0x1908u /* GL_RGBA */, 0x1401u /* GL_UNSIGNED_BYTE */, pixels);
    unsigned int readback_error = get_error();
    pixel_store(0x0D05u /* GL_PACK_ALIGNMENT */, old_pack_alignment);
    if (readback_error != 0) {
        nx_log("sdl-egl: capture glReadPixels failed error=0x%x",
               readback_error);
        free(pixels);
        free(rgb_row);
        return;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                  0600);
    int ok = fd >= 0;
    char header[64];
    int header_size = snprintf(header, sizeof(header), "P6\n%d %d\n255\n",
                               drawable_width, drawable_height);
    if (ok && (header_size <= 0 || (size_t)header_size >= sizeof(header) ||
               !write_all(fd, header, (size_t)header_size)))
        ok = 0;
    for (int y = drawable_height - 1; ok && y >= 0; --y) {
        const unsigned char *rgba = pixels + (size_t)y * rgba_row_size;
        for (int x = 0; x < drawable_width; ++x) {
            rgb_row[(size_t)x * 3u] = rgba[(size_t)x * 4u];
            rgb_row[(size_t)x * 3u + 1u] = rgba[(size_t)x * 4u + 1u];
            rgb_row[(size_t)x * 3u + 2u] = rgba[(size_t)x * 4u + 2u];
        }
        if (!write_all(fd, rgb_row, rgb_row_size))
            ok = 0;
    }
    if (fd >= 0 && close(fd) != 0)
        ok = 0;
    free(pixels);
    free(rgb_row);
    if (!ok && fd >= 0)
        unlink(path);
    nx_log("sdl-egl: capture %s path=%s size=%dx%d",
           ok ? "written" : "failed", path,
           drawable_width, drawable_height);
}

EGLDisplay nx_sdl_egl_get_display(EGLNativeDisplayType native_display)
{
    (void)native_display;
    return (EGLDisplay)&display_token;
}

EGLBoolean nx_sdl_egl_initialize(EGLDisplay display, EGLint *major, EGLint *minor)
{
    if (display != (EGLDisplay)&display_token || !init_video()) {
        last_error = EGL_NOT_INITIALIZED;
        return EGL_FALSE;
    }
    if (major) *major = 1;
    if (minor) *minor = 4;
    nx_log("sdl-egl: eglInitialize -> 1.4");
    return EGL_TRUE;
}

EGLBoolean nx_sdl_egl_terminate(EGLDisplay display)
{
    (void)display;
    cleanup();
    return EGL_TRUE;
}

EGLBoolean nx_sdl_egl_bind_api(EGLenum requested_api)
{
    if (requested_api != EGL_OPENGL_ES_API) {
        last_error = EGL_BAD_PARAMETER;
        return EGL_FALSE;
    }
    return EGL_TRUE;
}

EGLenum nx_sdl_egl_query_api(void)
{
    return EGL_OPENGL_ES_API;
}

EGLBoolean nx_sdl_egl_get_configs(EGLDisplay display, EGLConfig *configs,
                                  EGLint config_size, EGLint *num_config)
{
    (void)display;
    if (num_config) *num_config = 1;
    if (configs && config_size > 0)
        configs[0] = (EGLConfig)&config_token;
    return EGL_TRUE;
}

EGLBoolean nx_sdl_egl_get_config_attrib(EGLDisplay display, EGLConfig config,
                                        EGLint attribute, EGLint *value)
{
    (void)display;
    (void)config;
    if (!value) {
        last_error = EGL_BAD_PARAMETER;
        return EGL_FALSE;
    }
    switch (attribute) {
    case EGL_BUFFER_SIZE: *value = 24 + format_alpha; break;
    case EGL_RED_SIZE:
    case EGL_GREEN_SIZE:
    case EGL_BLUE_SIZE: *value = 8; break;
    case EGL_ALPHA_SIZE: *value = format_alpha; break;
    case EGL_DEPTH_SIZE: *value = format_depth; break;
    case EGL_STENCIL_SIZE: *value = format_stencil; break;
    case EGL_CONFIG_CAVEAT: *value = EGL_NONE; break;
    case EGL_CONFIG_ID: *value = 1; break;
    case EGL_LEVEL: *value = 0; break;
    case EGL_NATIVE_RENDERABLE: *value = EGL_FALSE; break;
    case EGL_NATIVE_VISUAL_ID:
    case EGL_NATIVE_VISUAL_TYPE: *value = 0; break;
    case EGL_SAMPLES:
    case EGL_SAMPLE_BUFFERS: *value = 0; break;
    case EGL_SURFACE_TYPE: *value = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; break;
    case EGL_RENDERABLE_TYPE:
    case EGL_CONFORMANT: *value = EGL_OPENGL_ES2_BIT; break;
    case EGL_COLOR_BUFFER_TYPE: *value = EGL_RGB_BUFFER; break;
    case EGL_TRANSPARENT_TYPE: *value = EGL_NONE; break;
    case EGL_MAX_PBUFFER_WIDTH: *value = drawable_width; break;
    case EGL_MAX_PBUFFER_HEIGHT: *value = drawable_height; break;
    case EGL_MAX_PBUFFER_PIXELS: *value = drawable_width * drawable_height; break;
    case EGL_MIN_SWAP_INTERVAL: *value = 0; break;
    case EGL_MAX_SWAP_INTERVAL: *value = 1; break;
    default: *value = 0; break;
    }
    return EGL_TRUE;
}

EGLBoolean nx_sdl_egl_choose_config(EGLDisplay display,
                                    const EGLint *attributes,
                                    EGLConfig *configs, EGLint config_size,
                                    EGLint *num_config)
{
    (void)display;
    (void)attributes;
    if (configs && config_size > 0)
        configs[0] = (EGLConfig)&config_token;
    if (num_config) *num_config = 1;
    nx_log("sdl-egl: eglChooseConfig -> logical ES2 config");
    return EGL_TRUE;
}

EGLContext nx_sdl_egl_create_context(EGLDisplay display, EGLConfig config,
                                     EGLContext share_context,
                                     const EGLint *attributes)
{
    (void)display;
    (void)config;
    (void)attributes;
    NxContext *context = calloc(1, sizeof(*context));
    if (!context) {
        last_error = EGL_BAD_ALLOC;
        return EGL_NO_CONTEXT;
    }
    context->magic = NX_CONTEXT_MAGIC;
    context->client_version = 2;
    context->share = (NxContext *)share_context;
    context->next = contexts;
    contexts = context;
    if (window && !create_real_context(context)) {
        remove_context(context);
        free(context);
        last_error = EGL_BAD_ALLOC;
        return EGL_NO_CONTEXT;
    }
    nx_log("sdl-egl: eglCreateContext -> logical=%p ES2", context);
    return (EGLContext)context;
}

EGLSurface nx_sdl_egl_create_window_surface(EGLDisplay display,
                                            EGLConfig config,
                                            EGLNativeWindowType native_window,
                                            const EGLint *attributes)
{
    (void)display;
    (void)config;
    (void)attributes;
    if (!open_window(native_window))
        return EGL_NO_SURFACE;
    NxSurface *surface = calloc(1, sizeof(*surface));
    if (!surface) {
        last_error = EGL_BAD_ALLOC;
        return EGL_NO_SURFACE;
    }
    surface->magic = NX_SURFACE_MAGIC;
    surface->window = 1;
    surface->width = drawable_width;
    surface->height = drawable_height;
    surface->next = surfaces;
    surfaces = surface;
    nx_log("sdl-egl: eglCreateWindowSurface -> logical=%p", surface);
    return (EGLSurface)surface;
}

EGLSurface nx_sdl_egl_create_pbuffer_surface(EGLDisplay display,
                                             EGLConfig config,
                                             const EGLint *attributes)
{
    (void)display;
    (void)config;
    NxSurface *surface = calloc(1, sizeof(*surface));
    if (!surface) {
        last_error = EGL_BAD_ALLOC;
        return EGL_NO_SURFACE;
    }
    surface->magic = NX_SURFACE_MAGIC;
    surface->width = 1;
    surface->height = 1;
    if (attributes) {
        for (int i = 0; attributes[i] != EGL_NONE; i += 2) {
            if (attributes[i] == EGL_WIDTH) surface->width = attributes[i + 1];
            if (attributes[i] == EGL_HEIGHT) surface->height = attributes[i + 1];
        }
    }
    surface->next = surfaces;
    surfaces = surface;
    return (EGLSurface)surface;
}

EGLBoolean nx_sdl_egl_destroy_surface(EGLDisplay display, EGLSurface surface)
{
    (void)display;
    NxSurface *logical = (NxSurface *)surface;
    if (logical && logical->magic == NX_SURFACE_MAGIC) {
        if (current_draw == logical) current_draw = NULL;
        if (current_read == logical) current_read = NULL;
        remove_surface(logical);
        logical->magic = 0;
        free(logical);
    }
    return EGL_TRUE;
}

EGLBoolean nx_sdl_egl_destroy_context(EGLDisplay display, EGLContext context)
{
    (void)display;
    NxContext *logical = (NxContext *)context;
    if (logical && logical->magic == NX_CONTEXT_MAGIC) {
        if (current_context == logical) {
            if (window) api.GL_MakeCurrent(window, NULL);
            current_context = NULL;
        }
        if (logical->real) api.GL_DeleteContext(logical->real);
        remove_context(logical);
        logical->magic = 0;
        free(logical);
    }
    return EGL_TRUE;
}

EGLBoolean nx_sdl_egl_make_current(EGLDisplay display, EGLSurface draw,
                                   EGLSurface read, EGLContext context)
{
    (void)display;
    if (context == EGL_NO_CONTEXT) {
        int result = window ? api.GL_MakeCurrent(window, NULL) : 0;
        current_context = NULL;
        current_draw = NULL;
        current_read = NULL;
        return result == 0 ? EGL_TRUE : EGL_FALSE;
    }
    NxContext *logical = (NxContext *)context;
    if (!logical || logical->magic != NX_CONTEXT_MAGIC ||
        !window || !create_real_context(logical)) {
        last_error = EGL_BAD_CONTEXT;
        return EGL_FALSE;
    }
    if (api.GL_MakeCurrent(window, logical->real) != 0) {
        nx_log("sdl-egl: SDL_GL_MakeCurrent failed: %s", error_text());
        last_error = EGL_BAD_ACCESS;
        return EGL_FALSE;
    }
    current_context = logical;
    current_draw = (NxSurface *)draw;
    current_read = (NxSurface *)read;
    nx_log("sdl-egl: eglMakeCurrent draw=%p context=%p -> SDL OK",
           draw, context);
    return EGL_TRUE;
}

EGLBoolean nx_sdl_egl_swap_buffers(EGLDisplay display, EGLSurface surface)
{
    (void)display;
    (void)surface;
    if (!window || !current_context) {
        last_error = EGL_BAD_CURRENT_SURFACE;
        return EGL_FALSE;
    }
    capture_ppm_once();
    api.GL_SwapWindow(window);
    return EGL_TRUE;
}

EGLBoolean nx_sdl_egl_swap_interval(EGLDisplay display, EGLint interval)
{
    (void)display;
    if (!window)
        return EGL_TRUE;
    if (api.GL_SetSwapInterval(interval) == 0)
        return EGL_TRUE;
    nx_log("sdl-egl: swap interval %d failed: %s", interval, error_text());
    last_error = EGL_BAD_PARAMETER;
    return EGL_FALSE;
}

EGLint nx_sdl_egl_get_error(void)
{
    EGLint result = last_error;
    last_error = EGL_SUCCESS;
    return result;
}

EGLBoolean nx_sdl_egl_wait_gl(void) { return EGL_TRUE; }
EGLBoolean nx_sdl_egl_wait_native(EGLint engine)
{
    (void)engine;
    return EGL_TRUE;
}

EGLBoolean nx_sdl_egl_release_thread(void)
{
    if (window) api.GL_MakeCurrent(window, NULL);
    current_context = NULL;
    current_draw = NULL;
    current_read = NULL;
    return EGL_TRUE;
}

EGLContext nx_sdl_egl_get_current_context(void)
{
    return (EGLContext)current_context;
}

EGLSurface nx_sdl_egl_get_current_surface(EGLint readdraw)
{
    return (EGLSurface)(readdraw == EGL_READ ? current_read : current_draw);
}

EGLDisplay nx_sdl_egl_get_current_display(void)
{
    return current_context ? (EGLDisplay)&display_token : EGL_NO_DISPLAY;
}

EGLBoolean nx_sdl_egl_query_surface(EGLDisplay display, EGLSurface surface,
                                    EGLint attribute, EGLint *value)
{
    (void)display;
    NxSurface *logical = (NxSurface *)surface;
    if (!logical || logical->magic != NX_SURFACE_MAGIC || !value) {
        last_error = EGL_BAD_SURFACE;
        return EGL_FALSE;
    }
    switch (attribute) {
    case EGL_WIDTH: *value = logical->width; break;
    case EGL_HEIGHT: *value = logical->height; break;
    case EGL_CONFIG_ID: *value = 1; break;
    case EGL_SWAP_BEHAVIOR: *value = EGL_BUFFER_DESTROYED; break;
    default: *value = 0; break;
    }
    return EGL_TRUE;
}

EGLBoolean nx_sdl_egl_query_context(EGLDisplay display, EGLContext context,
                                    EGLint attribute, EGLint *value)
{
    (void)display;
    NxContext *logical = (NxContext *)context;
    if (!logical || logical->magic != NX_CONTEXT_MAGIC || !value) {
        last_error = EGL_BAD_CONTEXT;
        return EGL_FALSE;
    }
    switch (attribute) {
    case EGL_CONTEXT_CLIENT_TYPE: *value = EGL_OPENGL_ES_API; break;
    case EGL_CONTEXT_CLIENT_VERSION: *value = logical->client_version; break;
    case EGL_CONFIG_ID: *value = 1; break;
    default: *value = 0; break;
    }
    return EGL_TRUE;
}

EGLBoolean nx_sdl_egl_surface_attrib(EGLDisplay display, EGLSurface surface,
                                     EGLint attribute, EGLint value)
{
    (void)display;
    (void)surface;
    (void)attribute;
    (void)value;
    return EGL_TRUE;
}

const char *nx_sdl_egl_query_string(EGLDisplay display, EGLint name)
{
    if (display == EGL_NO_DISPLAY && name == EGL_EXTENSIONS)
        return "EGL_NX_platform_fbdev";
    switch (name) {
    case EGL_VENDOR: return "NextOS Tearscape SDL2 facade";
    case EGL_VERSION: return "1.4";
    case EGL_EXTENSIONS: return "";
    case EGL_CLIENT_APIS: return "OpenGL_ES";
    default: return NULL;
    }
}

void *nx_sdl_egl_get_proc_address(const char *name)
{
    return load_sdl() ? api.GL_GetProcAddress(name) : NULL;
}
