#ifndef HUNTDOWN_VIDEO_GL_H
#define HUNTDOWN_VIDEO_GL_H

/*
 * Presenter for the authored startup movies on every backend where SDL owns
 * the scanout (KMSDRM, Wayland, X11).  The fbdev/Mali route keeps writing the
 * decoded frames straight into /dev/fb0 and never reaches this file.
 *
 * Every entry point must run on the thread that currently owns the GL context,
 * which is the same thread Unity swaps from.
 */

/* Prepare (or resize) the RGBA frame texture.  Returns 1 when the presenter is
 * usable; 0 after a logged failure, which makes the caller fall back. */
int hd_video_gl_begin(int width, int height);

/* Upload `rgba` (width*height*4, top-down) when non-NULL and draw the frame as
 * a full-screen quad.  Returns 1 when something was drawn. */
int hd_video_gl_present(const unsigned char *rgba);

/* Release the texture/program.  Safe to call without a previous begin. */
void hd_video_gl_end(void);

#endif
