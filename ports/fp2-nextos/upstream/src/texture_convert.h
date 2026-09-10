#ifndef FP2_TEXTURE_CONVERT_H
#define FP2_TEXTURE_CONVERT_H

#include <stddef.h>
#include <stdint.h>

size_t fp2_rgba4444_size(int width, int height);
size_t fp2_rgb565_size(int width, int height);
int fp2_rgba8888_to_rgba4444(const uint8_t *source, int width, int height,
                            uint16_t *destination,
                            size_t destination_size);
int fp2_rgba8888_to_rgb565(const uint8_t *source, int width, int height,
                          uint16_t *destination, size_t destination_size);
int fp2_rgba8888_is_opaque(const uint8_t *source, int width, int height);

#endif
