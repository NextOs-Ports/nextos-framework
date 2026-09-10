#ifndef SS_ETC1_H
#define SS_ETC1_H

#include <stddef.h>
#include <stdint.h>

/* Encoder ETC1 (GL_OES_compressed_ETC1_RGB8_texture, 4bpp) escrito do zero
 * para o pipeline de textura do port (Mali-450). Sem dependencias externas.
 * Entrada sempre RGBA8888 com stride em bytes; o canal usado depende da
 * funcao (RGB ou so o alpha como cinza). */

/* bytes necessarios para w x h (blocos 4x4 de 8 bytes, arredonda pra cima) */
size_t ss_etc1_size(int w, int h);

/* comprime o RGB de um buffer RGBA8888 (alpha ignorado) */
void ss_etc1_encode_rgba(const uint8_t *rgba, int w, int h, size_t stride,
                         uint8_t *out);

/* comprime SO o canal alpha como cinza (A,A,A) — a "segunda camada" do
 * esquema ETC1 dupla-camada para alpha */
void ss_etc1_encode_alpha(const uint8_t *rgba, int w, int h, size_t stride,
                          uint8_t *out);

#endif
