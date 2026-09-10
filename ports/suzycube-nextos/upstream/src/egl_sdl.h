#ifndef SC_EGL_SDL_H
#define SC_EGL_SDL_H

#include <EGL/egl.h>

/* Select the proven backend split once: SDL owns KMS/Wayland contexts and
 * page flips, while the legacy SDL "mali" backend keeps raw EGL/fbdev. */
int sc_sdl_video_init(void);
int sc_sdl_video_active(void);
void *sc_sdl_gl_proc(const char *name);
void *sc_sdl_egl_proc(const char *name);
EGLBoolean sc_sdl_swap_buffers(EGLDisplay display, EGLSurface surface);

#endif
