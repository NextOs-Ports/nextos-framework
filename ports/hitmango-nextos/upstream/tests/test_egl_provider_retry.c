#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <GLES2/gl2.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "egl_sdl.h"

enum test_scenario {
    TEST_NORMAL,
    TEST_WINDOW_RETRY,
    TEST_INIT_RETRY,
    TEST_EXPLICIT_PROVIDER,
};

static enum test_scenario scenario;
static int init_calls;
static int quit_calls;
static int window_calls;
static const char *last_error = "fake SDL failure";

static void fail(const char *message)
{
    fprintf(stderr, "test_egl_provider_retry: %s\n", message);
    exit(1);
}

#define CHECK(condition) do { if (!(condition)) fail(#condition); } while (0)

static int portable_providers_active(void)
{
    const char *egl = getenv("SDL_VIDEO_EGL_DRIVER");
    const char *gles = getenv("SDL_VIDEO_GL_DRIVER");
    return egl && strcmp(egl, "libEGL.so") == 0 &&
           gles && strcmp(gles, "libGLESv2.so") == 0;
}

Uint32 SDL_WasInit(Uint32 flags)
{
    (void)flags;
    return 0;
}

SDL_bool SDL_SetHint(const char *name, const char *value)
{
    (void)name;
    (void)value;
    return SDL_TRUE;
}

int SDL_InitSubSystem(Uint32 flags)
{
    (void)flags;
    init_calls++;
    if (scenario == TEST_INIT_RETRY && !portable_providers_active())
        return -1;
    return 0;
}

void SDL_QuitSubSystem(Uint32 flags)
{
    (void)flags;
    quit_calls++;
}

const char *SDL_GetCurrentVideoDriver(void)
{
    return "KMSDRM";
}

int SDL_GetCurrentDisplayMode(int display_index, SDL_DisplayMode *mode)
{
    (void)display_index;
    memset(mode, 0, sizeof *mode);
    mode->w = 640;
    mode->h = 480;
    return 0;
}

int SDL_GL_SetAttribute(SDL_GLattr attr, int value)
{
    (void)attr;
    (void)value;
    return 0;
}

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int width,
                             int height, Uint32 flags)
{
    (void)title;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)flags;
    window_calls++;
    if (scenario == TEST_EXPLICIT_PROVIDER)
        return NULL;
    if (scenario == TEST_WINDOW_RETRY && !portable_providers_active())
        return NULL;
    return (SDL_Window *)(uintptr_t)0x11;
}

static const GLubyte *fake_gl_get_string(GLenum name)
{
    (void)name;
    return (const GLubyte *)"OpenGL ES 2.0 provider fixture";
}

SDL_GLContext SDL_GL_CreateContext(SDL_Window *window)
{
    (void)window;
    return (SDL_GLContext)(uintptr_t)0x22;
}

void *SDL_GL_GetProcAddress(const char *name)
{
    if (strcmp(name, "glGetString") == 0)
        return (void *)fake_gl_get_string;
    return NULL;
}

void SDL_GL_DeleteContext(SDL_GLContext context)
{
    (void)context;
}

int SDL_GL_GetAttribute(SDL_GLattr attr, int *value)
{
    *value = attr == SDL_GL_ALPHA_SIZE ? 8 : 8;
    return 0;
}

void SDL_DestroyWindow(SDL_Window *window)
{
    (void)window;
}

void SDL_GL_GetDrawableSize(SDL_Window *window, int *width, int *height)
{
    (void)window;
    *width = 640;
    *height = 480;
}

int SDL_GL_SetSwapInterval(int interval)
{
    (void)interval;
    return 0;
}

int SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context)
{
    (void)window;
    (void)context;
    return 0;
}

const char *SDL_GetError(void)
{
    return last_error;
}

void nx_log(const char *format, ...)
{
    (void)format;
}

void nx_die(const char *format, ...)
{
    (void)format;
    if (scenario == TEST_EXPLICIT_PROVIDER) {
        const char *egl = getenv("SDL_VIDEO_EGL_DRIVER");
        CHECK(egl && strcmp(egl, "vendor-egl") == 0);
        CHECK(getenv("SDL_VIDEO_GL_DRIVER") == NULL);
        CHECK(init_calls == 1);
        CHECK(quit_calls == 0);
        CHECK(window_calls == 5);
        exit(0);
    }
    fail("unexpected nx_die");
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    unsetenv("SDL_VIDEO_EGL_DRIVER");
    unsetenv("SDL_VIDEO_GL_DRIVER");

    if (strcmp(argv[1], "normal") == 0) {
        scenario = TEST_NORMAL;
    } else if (strcmp(argv[1], "window-retry") == 0) {
        scenario = TEST_WINDOW_RETRY;
    } else if (strcmp(argv[1], "init-retry") == 0) {
        scenario = TEST_INIT_RETRY;
    } else if (strcmp(argv[1], "explicit") == 0) {
        scenario = TEST_EXPLICIT_PROVIDER;
        CHECK(setenv("SDL_VIDEO_EGL_DRIVER", "vendor-egl", 1) == 0);
    } else {
        fail("unknown scenario");
    }

    CHECK(hgo_sdl_video_init() == 1);
    if (scenario == TEST_NORMAL) {
        CHECK(init_calls == 1);
        CHECK(quit_calls == 0);
        CHECK(window_calls == 1);
        CHECK(getenv("SDL_VIDEO_EGL_DRIVER") == NULL);
        CHECK(getenv("SDL_VIDEO_GL_DRIVER") == NULL);
    } else if (scenario == TEST_WINDOW_RETRY) {
        CHECK(init_calls == 2);
        CHECK(quit_calls == 1);
        CHECK(window_calls == 6);
        CHECK(portable_providers_active());
    } else if (scenario == TEST_INIT_RETRY) {
        CHECK(init_calls == 2);
        CHECK(quit_calls == 1);
        CHECK(window_calls == 1);
        CHECK(portable_providers_active());
    }
    return 0;
}
