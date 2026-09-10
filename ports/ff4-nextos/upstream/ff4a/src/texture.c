/*
 * texture.c -- decode de imagem via stb_image, empacotando no formato de
 * loadTexture([B)[I do MainActivity: int[0]=w, int[1]=h, int[2..]=ARGB.
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "nxcompat_system_font.h"

#include "texture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int *decode_texture(const unsigned char *bytes, int len, int *out_count) {
  int w = 0, h = 0, ch = 0;
  unsigned char *rgba = stbi_load_from_memory(bytes, len, &w, &h, &ch, 4);
  if (!rgba) return NULL;
  int n = w * h;
  int *out = (int *)malloc((size_t)(n + 2) * 4);
  if (!out) {
    stbi_image_free(rgba);
    return NULL;
  }
  out[0] = w;
  out[1] = h;
  for (int i = 0; i < n; i++) {
    unsigned char r = rgba[i * 4 + 0], g = rgba[i * 4 + 1],
                  b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
    /* ARGB_8888 igual Android getPixels: (A<<24)|(R<<16)|(G<<8)|B */
    out[i + 2] = (a << 24) | (r << 16) | (g << 8) | b;
  }
  stbi_image_free(rgba);
  *out_count = n + 2;
  return out;
}

/* ---- drawFont: renderiza texto do sistema (TTF) igual Paint.drawText ----
 *
 * As fontes proprias do jogo sao bitmaps NCBR do DS; o port nao as usa e
 * desenha o texto com stb_truetype. Isso torna a fonte do SISTEMA um requisito
 * de runtime, e a busca por ela mora no framework -- ver
 * framework/nxcompat/include/nxcompat_system_font.h para o porque. */
static stbtt_fontinfo g_font;
static unsigned char *g_font_buf;
static int g_font_ok = -1;

static void font_init(void) {
  char path[NXCOMPAT_SYSTEM_FONT_PATH_MAX];
  FILE *f;
  long sz;

  if (g_font_ok >= 0) return;
  g_font_ok = 0;

  if (!nxcompat_system_font_find(NULL, "FF4A_FONT", NULL, 0, path,
                                 sizeof path)) {
    debugPrintf("[ff4a] fonte: NENHUMA encontrada -- o texto nao sera desenhado\n");
    return;
  }
  f = fopen(path, "rb");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) { fclose(f); return; }
  g_font_buf = (unsigned char *)malloc((size_t)sz);
  if (g_font_buf && fread(g_font_buf, 1, (size_t)sz, f) == (size_t)sz &&
      stbtt_InitFont(&g_font, g_font_buf, 0)) {
    debugPrintf("[ff4a] fonte: %s\n", path);
    g_font_ok = 1;
  } else {
    free(g_font_buf);
    g_font_buf = NULL;
  }
  fclose(f);
}

/* decode UTF-8 -> codepoint; avança *p */
static int utf8_next(const unsigned char **p) {
  const unsigned char *s = *p;
  int c = s[0];
  if (c < 0x80) { *p += 1; return c; }
  if ((c & 0xE0) == 0xC0) { *p += 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
  if ((c & 0xF0) == 0xE0) {
    *p += 3;
    return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
  }
  if ((c & 0xF8) == 0xF0) {
    *p += 4;
    return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) |
           (s[3] & 0x3F);
  }
  *p += 1;
  return c;
}

int *draw_font(const char *text, int size, int text_size, int baseline, int *out_count) {
  if (size <= 0) size = 1;
  int n = size * size;
  int *out = (int *)calloc((size_t)n + 1, 4); /* [0]=largura, [1..]=pixels */
  if (!out) return NULL;
  *out_count = n + 1;
  font_init();
  if (!g_font_ok || !text) {
    out[0] = 0;
    return out;
  }
  float scale = stbtt_ScaleForPixelHeight(&g_font, (float)text_size);
  int penx = 0;
  const unsigned char *p = (const unsigned char *)text;
  while (*p) {
    int cp = utf8_next(&p);
    int aw, lsb;
    stbtt_GetCodepointHMetrics(&g_font, cp, &aw, &lsb);
    int gx0, gy0, gx1, gy1;
    stbtt_GetCodepointBitmapBox(&g_font, cp, scale, scale, &gx0, &gy0, &gx1, &gy1);
    int gw = gx1 - gx0, gh = gy1 - gy0;
    if (gw > 0 && gh > 0) {
      unsigned char *gl = (unsigned char *)malloc((size_t)gw * gh);
      if (gl) {
        stbtt_MakeCodepointBitmap(&g_font, gl, gw, gh, gw, scale, scale, cp);
        int ox = penx + (int)(lsb * scale) + gx0;
        int oy = baseline + gy0;
        for (int y = 0; y < gh; y++) {
          int py = oy + y;
          if (py < 0 || py >= size) continue;
          for (int x = 0; x < gw; x++) {
            int px = ox + x;
            if (px < 0 || px >= size) continue;
            unsigned char cov = gl[y * gw + x];
            if (cov) out[1 + py * size + px] = ((int)cov << 24); /* preto, alpha=cov */
          }
        }
        free(gl);
      }
    }
    penx += (int)(aw * scale);
  }
  out[0] = penx; /* largura medida (measureText) */
  return out;
}
