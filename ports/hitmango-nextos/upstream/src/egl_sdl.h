#ifndef HGO_EGL_SDL_H
#define HGO_EGL_SDL_H

#include <EGL/egl.h>

/* Select the proven backend split once: SDL owns KMS/Wayland contexts and
 * page flips, while the legacy SDL "mali" backend keeps raw EGL/fbdev. */
int hgo_sdl_video_init(void);
int hgo_sdl_video_active(void);
void *hgo_sdl_gl_proc(const char *name);
void *hgo_sdl_egl_proc(const char *name);
EGLBoolean hgo_sdl_swap_buffers(EGLDisplay display, EGLSurface surface);

#endif
