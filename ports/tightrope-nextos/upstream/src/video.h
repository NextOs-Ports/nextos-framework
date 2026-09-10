/* video.h -- drawable ownership: raw fbdev EGL or the firmware's own SDL. */

#ifndef TR_VIDEO_H
#define TR_VIDEO_H

#include <stddef.h>
#include "nx_elf.h"

enum {
    TR_VIDEO_UNSET = 0,
    TR_VIDEO_RAW,   /* the game's SDL drives EGL on /dev/fb0 (Mali-450)      */
    TR_VIDEO_SDL,   /* the firmware's SDL2 owns window, context and present  */
};

int  tr_video_init(void);
int  tr_video_mode(void);
int  tr_video_drawable(int *w, int *h);
void tr_video_shutdown(void);
void *tr_video_gl_sym(const char *name);

/* Only meaningful in TR_VIDEO_SDL. */
const nx_import *tr_video_egl_table(size_t *n);
unsigned int tr_video_present(void);

/* android.c owns the ANativeWindow the game reads its size back from. */
void tr_window_set_size(int w, int h);

#endif /* TR_VIDEO_H */
