#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Include the implementation so this host gate exercises the exact static
 * provider policy used by the adapter.  Unreferenced SDL/EGL sections are
 * discarded by the test link. */
#include "../src/video.c"

static void clear_provider_env(void)
{
    unsetenv("SDL_VIDEO_EGL_DRIVER");
    unsetenv("SDL_VIDEO_GL_DRIVER");
    portable_provider_active = 0;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    clear_provider_env();
    assert(enable_portable_provider_retry() == 1);
    assert(strcmp(getenv("SDL_VIDEO_EGL_DRIVER"), "libEGL.so") == 0);
    assert(strcmp(getenv("SDL_VIDEO_GL_DRIVER"), "libGLESv2.so") == 0);
    drop_portable_provider_retry();
    assert(getenv("SDL_VIDEO_EGL_DRIVER") == NULL);
    assert(getenv("SDL_VIDEO_GL_DRIVER") == NULL);

    clear_provider_env();
    assert(setenv("SDL_VIDEO_EGL_DRIVER", "explicit-egl", 1) == 0);
    assert(enable_portable_provider_retry() == 0);
    assert(strcmp(getenv("SDL_VIDEO_EGL_DRIVER"), "explicit-egl") == 0);
    assert(getenv("SDL_VIDEO_GL_DRIVER") == NULL);

    clear_provider_env();
    assert(setenv("SDL_VIDEO_GL_DRIVER", "explicit-gles", 1) == 0);
    assert(enable_portable_provider_retry() == 0);
    assert(getenv("SDL_VIDEO_EGL_DRIVER") == NULL);
    assert(strcmp(getenv("SDL_VIDEO_GL_DRIVER"), "explicit-gles") == 0);

    clear_provider_env();
    assert(setenv("SDL_VIDEO_EGL_DRIVER", "explicit-egl", 1) == 0);
    assert(setenv("SDL_VIDEO_GL_DRIVER", "explicit-gles", 1) == 0);
    assert(enable_portable_provider_retry() == 0);
    assert(strcmp(getenv("SDL_VIDEO_EGL_DRIVER"), "explicit-egl") == 0);
    assert(strcmp(getenv("SDL_VIDEO_GL_DRIVER"), "explicit-gles") == 0);

    clear_provider_env();
    assert(setenv("SDL_VIDEO_GL_DRIVER", argv[1], 1) == 0);
    mode = RCR_VIDEO_SDL;
    int (*probe)(void) = rcr_video_gl_sym("glRcrProviderProbe");
    assert(probe != NULL);
    assert(probe() == 0x524352);
    return 0;
}
