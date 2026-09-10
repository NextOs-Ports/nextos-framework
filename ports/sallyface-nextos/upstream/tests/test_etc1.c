#include "etc1.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "test_etc1: %s\n", message);
        exit(1);
    }
}

static int distance_u8(uint8_t left, uint8_t right)
{
    return left > right ? left - right : right - left;
}

int main(void)
{
    require(sf_etc1_size(1, 1) == 8, "1x1 ocupa um bloco");
    require(sf_etc1_size(4, 4) == 8, "4x4 ocupa um bloco");
    require(sf_etc1_size(5, 4) == 16, "5x4 ocupa dois blocos");
    require(sf_etc1_size(5, 5) == 32, "5x5 ocupa quatro blocos");

    enum { WIDTH = 7, HEIGHT = 5 };
    uint8_t source[WIDTH * HEIGHT * 4];
    static const uint8_t colors[4][4] = {
        { 24, 72, 128, 0 },
        { 64, 112, 176, 85 },
        { 104, 152, 208, 170 },
        { 144, 192, 232, 255 },
    };
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int block = (x >= 4) + 2 * (y >= 4);
            uint8_t *pixel = &source[(y * WIDTH + x) * 4];
            for (int channel = 0; channel < 4; channel++)
                pixel[channel] = colors[block][channel];
        }
    }

    size_t encoded_size = sf_etc1_size(WIDTH, HEIGHT);
    uint8_t *rgb = calloc(1, encoded_size);
    uint8_t *alpha = calloc(1, encoded_size);
    require(rgb && alpha, "alocacao dos planos ETC1");
    sf_etc1_encode_rgba(source, WIDTH, HEIGHT, WIDTH * 4, rgb);
    sf_etc1_encode_alpha(source, WIDTH, HEIGHT, WIDTH * 4, alpha);

    uint8_t *decoded = sf_etc1_decode_pair(rgb, alpha, WIDTH, HEIGHT);
    require(decoded != NULL, "decode do par ETC1");
    for (int index = 0; index < WIDTH * HEIGHT; index++) {
        for (int channel = 0; channel < 4; channel++)
            require(distance_u8(decoded[index * 4 + channel],
                                source[index * 4 + channel]) <= 18,
                    "erro excessivo no round-trip de bloco uniforme");
    }
    free(decoded);

    decoded = sf_etc1_decode_pair(rgb, NULL, WIDTH, HEIGHT);
    require(decoded != NULL, "decode ETC1 opaco");
    for (int index = 0; index < WIDTH * HEIGHT; index++)
        require(decoded[index * 4 + 3] == 255, "alpha opaco precisa ser 255");
    free(decoded);

    uint8_t alpha8[WIDTH * HEIGHT];
    for (int index = 0; index < WIDTH * HEIGHT; index++)
        alpha8[index] = (uint8_t)(index * 7);
    decoded = sf_etc1_decode_rgb_alpha8(rgb, alpha8, WIDTH, HEIGHT);
    require(decoded != NULL, "decode ETC1 RGB + alpha8");
    for (int index = 0; index < WIDTH * HEIGHT; index++)
        require(decoded[index * 4 + 3] == alpha8[index],
                "alpha8 precisa permanecer byte-exato");
    free(decoded);
    free(alpha);
    free(rgb);

    puts("test_etc1: OK");
    return 0;
}
