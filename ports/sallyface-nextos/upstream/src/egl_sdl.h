#ifndef SF_EGL_SDL_H
#define SF_EGL_SDL_H

#include <EGL/egl.h>

/* Select the proven backend split once: SDL owns KMS/Wayland contexts and
 * page flips, while the legacy SDL "mali" backend keeps raw EGL/fbdev. */
int sf_sdl_video_init(void);
int sf_sdl_video_active(void);
void sf_sdl_screen_size(int *width, int *height);
void sf_sdl_video_shutdown(void);
void *sf_sdl_gl_proc(const char *name);
void *sf_sdl_egl_proc(const char *name);
EGLBoolean sf_sdl_swap_buffers(EGLDisplay display, EGLSurface surface);

#endif
