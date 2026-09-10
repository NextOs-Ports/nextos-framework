#ifndef SF_ETC1_H
#define SF_ETC1_H

#include <stddef.h>
#include <stdint.h>

/* Encoder ETC1 (GL_ETC1_RGB8_OES, 4bpp) para Mali-450/GLES2.
 * Entrada RGBA8888; RGB e alpha podem ser comprimidos separadamente.
 * Alpha separado é usado como textura cinza na segunda camada ETC1. */
size_t sf_etc1_size(int w, int h);
void sf_etc1_encode_rgba(const uint8_t *rgba, int w, int h, size_t stride,
                          uint8_t *out);
void sf_etc1_encode_alpha(const uint8_t *rgba, int w, int h, size_t stride,
                           uint8_t *out);

/* Reconstrói RGBA8888 a partir do par ETC1 RGB + alpha-em-cinza. Alpha NULL
 * representa uma textura opaca. O caller recebe w*h*4 e libera com free(). */
uint8_t *sf_etc1_decode_pair(const uint8_t *rgb, const uint8_t *alpha,
                              int w, int h);

/* Variante para o caminho de alpha sem perdas: RGB continua ETC1, enquanto
 * alpha8 contém um byte exato por pixel (o mesmo conteúdo da textura LUMINANCE). */
uint8_t *sf_etc1_decode_rgb_alpha8(const uint8_t *rgb, const uint8_t *alpha8,
                                    int w, int h);

#endif
