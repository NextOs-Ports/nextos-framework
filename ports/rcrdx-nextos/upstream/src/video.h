/* video.h -- drawable ownership: raw fbdev EGL or the firmware's own SDL. */

#ifndef RCR_VIDEO_H
#define RCR_VIDEO_H

#include <stddef.h>
#include "nx_elf.h"

enum {
    RCR_VIDEO_UNSET = 0,
    RCR_VIDEO_RAW,   /* the game's SDL drives EGL on /dev/fb0 (Mali-450)      */
    RCR_VIDEO_SDL,   /* the firmware's SDL2 owns window, context and present  */
};

int  rcr_video_init(void);
int  rcr_video_mode(void);
int  rcr_video_drawable(int *w, int *h);
void rcr_video_shutdown(void);
void *rcr_video_gl_sym(const char *name);

/* Only meaningful in RCR_VIDEO_SDL. */
const nx_import *rcr_video_egl_table(size_t *n);
unsigned int rcr_video_present(void);

/* android.c owns the ANativeWindow the game reads its size back from. */
void rcr_window_set_size(int w, int h);

#endif /* RCR_VIDEO_H */
