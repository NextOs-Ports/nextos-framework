#ifndef DT_EGL_SDL_H
#define DT_EGL_SDL_H

#include <EGL/egl.h>

/* Capability-selected video ownership. SDL owns the window, GLES contexts and
 * page flips when its real GLES2 probe succeeds; raw EGL remains the bounded
 * fallback. Device and firmware names are diagnostic only, never policy. */
int dt_sdl_video_init(void);
int dt_sdl_video_active(void);
void dt_sdl_video_size(int *width, int *height);
void *dt_sdl_gl_proc(const char *name);
void *dt_sdl_egl_proc(const char *name);
EGLBoolean dt_sdl_swap_buffers(EGLDisplay display, EGLSurface surface);

#endif
