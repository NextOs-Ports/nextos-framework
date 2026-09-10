#ifndef SF_MEDIA_H
#define SF_MEDIA_H

#include <stddef.h>
#include <stdint.h>

#include "nx_elf.h"

/* libmediandk surface exported to libunity. */
const nx_import *sf_media_table(size_t *count);
void *sf_media_sym(const char *name);

/* Fake native windows used by the two Android video output paths supported by
 * Unity 2022: AImageReader/AHardwareBuffer and Java SurfaceTexture. */
int sf_media_is_window(void *window);
void sf_media_window_acquire(void *window);
void sf_media_window_release(void *window);
int sf_media_window_width(void *window);
int sf_media_window_height(void *window);
int sf_media_window_set_geometry(void *window, int width, int height,
                                 int format);

void *sf_media_surface_texture_create(unsigned texture);
void *sf_media_surface_from_texture(void *surface_texture);
void sf_media_surface_set_size(void *surface, int width, int height);
void sf_media_surface_set_java_listener(void *surface, void *listener,
                                        void *surface_texture_object);
int64_t sf_media_surface_timestamp(void *surface);
int sf_media_surface_texture_update(void *surface);
void sf_media_update_textures(void);

/* EGLImage bridge for AHardwareBuffer-backed AImageReader output. */
void *sf_media_egl_get_native_client_buffer(void *hardware_buffer);
void *sf_media_egl_create_image(unsigned target, void *client_buffer);
int sf_media_egl_destroy_image(void *image);
int sf_media_egl_image_rgba(void *image, const unsigned char **pixels,
                            int *width, int *height, int *stride);

#endif
