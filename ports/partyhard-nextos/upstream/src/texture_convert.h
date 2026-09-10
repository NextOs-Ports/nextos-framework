#ifndef ST_TEXTURE_CONVERT_H
#define ST_TEXTURE_CONVERT_H

#include <stddef.h>
#include <stdint.h>

size_t st_rgba4444_size(int width, int height);
size_t st_rgb565_size(int width, int height);
int st_rgba8888_to_rgba4444(const uint8_t *source, int width, int height,
                            uint16_t *destination,
                            size_t destination_size);
int st_rgba8888_to_rgb565(const uint8_t *source, int width, int height,
                          uint16_t *destination, size_t destination_size);
int st_rgba8888_is_opaque(const uint8_t *source, int width, int height);

#endif
