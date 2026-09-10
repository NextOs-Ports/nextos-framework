/* etc1_encode.h -- encoder ETC1 (GL_ETC1_RGB8_OES, 0x8D64) PROPRIO, do zero.
 * Mali-450 Utgard amostra ETC1 nativo -> textura opaca a 4 bits/px (vs 32 RGBA8)
 * = ~8x menos RAM de VRAM. Escrito p/ casar EXATAMENTE o decoder do spec GL ETC1.
 * NAO e o encoder do Bully (aquele e bugado). */
#ifndef ETC1_ENCODE_H
#define ETC1_ENCODE_H
#include <stdint.h>

/* Codifica UM bloco 4x4 RGBA (row-major, 16 px, 4 bytes/px; alpha ignorado)
 * em 8 bytes ETC1. */
void etc1_encode_block_rgba(const uint8_t *block_rgba, uint8_t out[8]);

/* Codifica uma imagem RGBA w x h (w,h multiplos de 4) em ETC1.
 * out deve ter (w/4)*(h/4)*8 bytes. channels=3 ou 4 (stride do pixel). */
void etc1_encode_image(const uint8_t *rgba, int w, int h, int channels, uint8_t *out);

/* Liga (1, default) / desliga (0) o modo RÁPIDO do encoder (flip heurístico + sem
 * refino, ~4-5× mais rápido, qualidade ~igual). 0 = exaustivo (máxima qualidade). */
void etc1_set_fast(int fast);

#endif
