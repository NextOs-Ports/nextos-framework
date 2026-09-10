/*
 * texture.h -- decodifica imagem (PNG/etc) -> int[] no formato de loadTexture do
 * MainActivity: [0]=w, [1]=h, [2..]=pixels ARGB (0xAARRGGBB, igual getPixels).
 */
#ifndef FF4A_TEXTURE_H
#define FF4A_TEXTURE_H

/* devolve buffer malloc'd de ints (count = w*h+2) ou NULL. out_count = nº de ints. */
int *decode_texture(const unsigned char *bytes, int len, int *out_count);

/* drawFont(text,size,textSize,baseline) -> int[]: [0]=largura medida,
 * [1..]=pixels ARGB (texto preto antialias, alpha=cobertura). size=lado quadrado. */
int *draw_font(const char *text, int size, int text_size, int baseline, int *out_count);

#endif
