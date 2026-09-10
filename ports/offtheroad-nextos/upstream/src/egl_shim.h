/* egl_shim.h -- GLES2 context for the port (SDL2 owns the window). */
#ifndef __EGL_SHIM_H__
#define __EGL_SHIM_H__

#include <stdint.h>

int gd_gl_init(void);            /* creates the window; 0 on failure */
int gd_win_w(void);              /* real framebuffer width  */
int gd_win_h(void);              /* real framebuffer height */
int gd_engine_w(void);           /* size handed to the engine */
int gd_engine_h(void);
void gd_gl_swap(void);
void gd_gl_shutdown(void);

/* Cursor overlay drawn after the engine frame.  It restores the GL state on
 * its own (gl_state.c) -- the caller must NOT touch the engine's state cache. */
void gd_cursor_set(float x, float y, int visible);
int gd_cursor_draw(void); /* 1 when it drew */

#endif
