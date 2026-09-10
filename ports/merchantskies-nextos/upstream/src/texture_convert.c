#include "texture_convert.h"

#include <limits.h>

static size_t pixel_count(int width, int height)
{
    if (width <= 0 || height <= 0 ||
        (size_t)width > SIZE_MAX / (size_t)height)
        return 0;
    return (size_t)width * (size_t)height;
}

size_t st_rgba4444_size(int width, int height)
{
    size_t pixels = pixel_count(width, height);
    if (!pixels || pixels > SIZE_MAX / sizeof(uint16_t))
        return 0;
    return pixels * sizeof(uint16_t);
}

size_t st_rgb565_size(int width, int height)
{
    return st_rgba4444_size(width, height);
}

int st_rgba8888_to_rgba4444(const uint8_t *source, int width, int height,
                            uint16_t *destination,
                            size_t destination_size)
{
    size_t pixels = pixel_count(width, height);
    size_t required = st_rgba4444_size(width, height);
    if (!source || !destination || !pixels || destination_size < required)
        return -1;

    for (size_t i = 0; i < pixels; i++) {
        const uint8_t *pixel = source + i * 4;
        destination[i] = (uint16_t)(((uint16_t)(pixel[0] >> 4) << 12) |
                                    ((uint16_t)(pixel[1] >> 4) << 8) |
                                    ((uint16_t)(pixel[2] >> 4) << 4) |
                                    ((uint16_t)(pixel[3] >> 4)));
    }
    return 0;
}

int st_rgba8888_to_rgb565(const uint8_t *source, int width, int height,
                          uint16_t *destination, size_t destination_size)
{
    size_t pixels = pixel_count(width, height);
    size_t required = st_rgb565_size(width, height);
    if (!source || !destination || !pixels || destination_size < required)
        return -1;

    for (size_t i = 0; i < pixels; i++) {
        const uint8_t *pixel = source + i * 4;
        destination[i] = (uint16_t)(((uint16_t)(pixel[0] >> 3) << 11) |
                                    ((uint16_t)(pixel[1] >> 2) << 5) |
                                    ((uint16_t)(pixel[2] >> 3)));
    }
    return 0;
}

int st_rgba8888_is_opaque(const uint8_t *source, int width, int height)
{
    size_t pixels = pixel_count(width, height);
    if (!source || !pixels)
        return 0;
    for (size_t i = 0; i < pixels; i++)
        if (source[i * 4 + 3] != 255)
            return 0;
    return 1;
}
