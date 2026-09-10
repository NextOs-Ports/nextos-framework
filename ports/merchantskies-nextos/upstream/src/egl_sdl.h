#ifndef ST_EGL_SDL_H
#define ST_EGL_SDL_H

#include <stdint.h>
#include <EGL/egl.h>

/* Select the proven backend split once: SDL owns KMS/Wayland contexts and
 * page flips, while the legacy SDL "mali" backend keeps raw EGL/fbdev. */
int st_sdl_video_init(void);
int st_sdl_video_active(void);
void *st_sdl_gl_proc(const char *name);
void *st_sdl_egl_proc(const char *name);
uintptr_t st_sdl_current_context_token(void);
EGLBoolean st_sdl_swap_buffers(EGLDisplay display, EGLSurface surface);

#endif
