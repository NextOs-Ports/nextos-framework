#ifndef FP2_EGL_SDL_H
#define FP2_EGL_SDL_H

#include <EGL/egl.h>

/* Select the proven backend split once: SDL owns KMS/Wayland contexts and
 * page flips, while the legacy SDL "mali" backend keeps raw EGL/fbdev. */
int fp2_sdl_video_init(void);
int fp2_sdl_video_active(void);
void *fp2_sdl_gl_proc(const char *name);
void *fp2_sdl_egl_proc(const char *name);
EGLBoolean fp2_sdl_swap_buffers(EGLDisplay display, EGLSurface surface);

#endif
