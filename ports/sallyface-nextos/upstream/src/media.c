/*
 * media.c -- neutral libmediandk stub for Sally Face.
 *
 * Sally Face ships zero VideoClip assets (measured on the original data: no
 * .mp4/.webm/.ogv and no VideoClip object in the bundles), so the FFmpeg-backed
 * media bridge used by other Unity ports is dead weight here.  Unity still
 * imports the libmediandk symbols at relocation time, so the table has to
 * exist; every entry resolves to a definitive, never-blocking answer.
 */

#include <stddef.h>
#include <stdint.h>

#include "media.h"

static const nx_import SF_MEDIA_TABLE[1];

const nx_import *sf_media_table(size_t *count)
{
    if (count)
        *count = 0;
    return SF_MEDIA_TABLE;
}

void *sf_media_sym(const char *name) { (void)name; return NULL; }

int sf_media_is_window(void *window) { (void)window; return 0; }
void sf_media_window_acquire(void *window) { (void)window; }
void sf_media_window_release(void *window) { (void)window; }
int sf_media_window_width(void *window) { (void)window; return 0; }
int sf_media_window_height(void *window) { (void)window; return 0; }
int sf_media_window_set_geometry(void *window, int width, int height, int fmt)
{
    (void)window; (void)width; (void)height; (void)fmt;
    return -1;
}

void *sf_media_surface_texture_create(unsigned texture)
{
    (void)texture;
    return NULL;
}
void *sf_media_surface_from_texture(void *st) { (void)st; return NULL; }
void sf_media_surface_set_size(void *s, int w, int h)
{
    (void)s; (void)w; (void)h;
}
void sf_media_surface_set_java_listener(void *s, void *l, void *sto)
{
    (void)s; (void)l; (void)sto;
}
int64_t sf_media_surface_timestamp(void *s) { (void)s; return 0; }
int sf_media_surface_texture_update(void *s) { (void)s; return 0; }
void sf_media_update_textures(void) { }

void *sf_media_egl_get_native_client_buffer(void *hb) { (void)hb; return NULL; }
void *sf_media_egl_create_image(unsigned target, void *buf)
{
    (void)target; (void)buf;
    return NULL;
}
int sf_media_egl_destroy_image(void *image) { (void)image; return 0; }
int sf_media_egl_image_rgba(void *image, const unsigned char **pixels,
                            int *width, int *height, int *stride)
{
    (void)image; (void)pixels; (void)width; (void)height; (void)stride;
    return 0;
}
