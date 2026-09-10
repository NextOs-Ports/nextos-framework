#ifndef GB_EGL_SDL_H
#define GB_EGL_SDL_H

#include <EGL/egl.h>

/* Select the proven backend split once: SDL owns KMS/Wayland contexts and
 * page flips. Raw EGL is attempted only when the SDL GLES capability probe
 * fails; backend names are diagnostic data, never policy. */
int gb_sdl_video_init(void);
int gb_sdl_video_active(void);
void *gb_sdl_gl_proc(const char *name);
void *gb_sdl_egl_proc(const char *name);
EGLBoolean gb_sdl_swap_buffers(EGLDisplay display, EGLSurface surface);

#endif
