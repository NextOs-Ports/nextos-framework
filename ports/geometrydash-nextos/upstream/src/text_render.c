/*
 * text_render.c -- Cocos2dxBitmap.createTextBitmapShadowStroke.
 *
 * The game ships 124 BMFont .fnt files, so almost every label is drawn by the
 * engine itself and never reaches here; this path only serves stray system-font
 * labels.  We render with FreeType into RGBA and hand the buffer back through
 * Cocos2dxBitmap.nativeInitBitmapDC(width, height, byte[]), exactly like the
 * Java side would.
 *
 * The font is the device's own Liberation Sans (the APK carries no TTF).
 */
#define _GNU_SOURCE
#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "jni_shim.h"
#include "util.h"

/* NextOS primeiro; depois os caminhos dos outros firmwares (ArkOS/PortMaster,
 * Debian/Ubuntu, ROCKNIX), porque o mesmo binario roda em todos e um deles
 * sempre tem uma sans-serif. Sem nenhuma, os labels de sistema saem vazios. */
static const char *const FONT_CANDIDATES[] = {
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/LiberationSans-Regular.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/opt/system/Tools/PortMaster/resources/DejaVuSans.ttf",
    "/opt/system/Tools/PortMaster/pylibs/resources/DejaVuSans.ttf",
    NULL,
};

static FT_Library g_ft;
static FT_Face g_face;
static int g_ready = -1;

static int ensure_ft(void) {
  if (g_ready >= 0)
    return g_ready;
  g_ready = 0;
  if (FT_Init_FreeType(&g_ft)) {
    debugPrintf("[text] FT_Init_FreeType falhou\n");
    return 0;
  }
  const char *env = getenv("GD_FONT");
  if (env && !FT_New_Face(g_ft, env, 0, &g_face)) {
    debugPrintf("[text] fonte '%s'\n", env);
    g_ready = 1;
    return 1;
  }
  for (int i = 0; FONT_CANDIDATES[i]; i++) {
    if (!FT_New_Face(g_ft, FONT_CANDIDATES[i], 0, &g_face)) {
      debugPrintf("[text] fonte '%s'\n", FONT_CANDIDATES[i]);
      g_ready = 1;
      return 1;
    }
  }
  debugPrintf("[text] nenhuma fonte TTF encontrada -- labels de sistema vazios\n");
  return 0;
}

static unsigned utf8_next(const char **s) {
  const unsigned char *p = (const unsigned char *)*s;
  unsigned c = *p++;
  if (c < 0x80) {
  } else if ((c >> 5) == 0x6) {
    c = ((c & 0x1Fu) << 6) | (*p++ & 0x3Fu);
  } else if ((c >> 4) == 0xE) {
    c = (c & 0x0Fu) << 12;
    c |= (unsigned)(*p++ & 0x3F) << 6;
    c |= (unsigned)(*p++ & 0x3F);
  } else if ((c >> 3) == 0x1E) {
    c = (c & 0x07u) << 18;
    c |= (unsigned)(*p++ & 0x3F) << 12;
    c |= (unsigned)(*p++ & 0x3F) << 6;
    c |= (unsigned)(*p++ & 0x3F);
  }
  *s = (const char *)p;
  return c;
}

static void blend(unsigned char *dst, int r, int g, int b, unsigned char a) {
  if (a <= dst[3])
    return;
  dst[0] = (unsigned char)r;
  dst[1] = (unsigned char)g;
  dst[2] = (unsigned char)b;
  dst[3] = a;
}

void text_render_shadow_stroke(void *env, const char *text, const char *font,
                               int size, float r, float g, float b, int align,
                               int req_w, int req_h, int shadow, float sdx,
                               float sdy, float sblur, int stroke, float sr,
                               float sg, float sb, float ssize,
                               jni_bitmap_dc_fn dc) {
  (void)font;
  (void)sblur;
  if (!dc)
    return;
  if (!text)
    text = "";
  if (!ensure_ft()) {
    /* Still answer, with an empty bitmap: the engine expects the callback. */
    void *arr = jni_shim_new_int_array(NULL, 1);
    dc(env, NULL, 1, 1, arr);
    return;
  }

  if (size < 6)
    size = 6;
  if (size > 200)
    size = 200;
  FT_Set_Pixel_Sizes(g_face, 0, (FT_UInt)size);
  int ascent = (int)(g_face->size->metrics.ascender >> 6);
  int descent = -(int)(g_face->size->metrics.descender >> 6);
  int lineh = (int)(g_face->size->metrics.height >> 6);
  if (lineh < ascent + descent)
    lineh = ascent + descent;

  int maxw = 0, curw = 0, lines = 1;
  for (const char *s = text; *s;) {
    unsigned cp = utf8_next(&s);
    if (cp == '\n') {
      if (curw > maxw)
        maxw = curw;
      curw = 0;
      lines++;
      continue;
    }
    if (FT_Load_Char(g_face, cp, FT_LOAD_DEFAULT))
      continue;
    curw += (int)(g_face->glyph->advance.x >> 6);
  }
  if (curw > maxw)
    maxw = curw;

  int pad = 2;
  if (stroke && ssize > 0.0f)
    pad += (int)(ssize + 0.5f);
  if (shadow)
    pad += (int)(sdx > sdy ? sdx : sdy) + 1;

  int W = maxw + 2 * pad;
  int H = lines * lineh + 2 * pad;
  /* The engine's requested size is a minimum, not a clamp: honour it when it
   * is larger so its own layout maths still lines up. */
  if (req_w > W)
    W = req_w;
  if (req_h > H)
    H = req_h;
  if (W < 1)
    W = 1;
  if (H < 1)
    H = 1;
  if (W > 2048)
    W = 2048;
  if (H > 2048)
    H = 2048;

  unsigned char *rgba = calloc((size_t)W * (size_t)H, 4);
  if (!rgba)
    return;

  int cr = (int)(r * 255.0f + 0.5f), cg = (int)(g * 255.0f + 0.5f),
      cb = (int)(b * 255.0f + 0.5f);
  int kr = (int)(sr * 255.0f + 0.5f), kg = (int)(sg * 255.0f + 0.5f),
      kb = (int)(sb * 255.0f + 0.5f);
  if (cr > 255) cr = 255; if (cg > 255) cg = 255; if (cb > 255) cb = 255;
  if (cr < 0) cr = 0; if (cg < 0) cg = 0; if (cb < 0) cb = 0;

  int start_x = pad;
  if (align == 1)
    start_x = (W - maxw) / 2;
  else if (align == 2)
    start_x = W - maxw - pad;
  if (start_x < 0)
    start_x = 0;

  /* Pass 0 = stroke/shadow silhouette, pass 1 = the glyph itself. */
  int passes = (stroke && ssize > 0.0f) ? 2 : 1;
  int spread = (int)(ssize + 0.5f);
  if (spread > 4)
    spread = 4;

  for (int pass = 0; pass < passes; pass++) {
    int is_outline = (passes == 2 && pass == 0);
    int pen_x = start_x, line = 0;
    int pen_y = pad + ascent;
    for (const char *s = text; *s;) {
      unsigned cp = utf8_next(&s);
      if (cp == '\n') {
        line++;
        pen_y = pad + ascent + line * lineh;
        pen_x = start_x;
        continue;
      }
      if (FT_Load_Char(g_face, cp, FT_LOAD_RENDER))
        continue;
      FT_GlyphSlot gl = g_face->glyph;
      FT_Bitmap *bm = &gl->bitmap;
      int gx = pen_x + gl->bitmap_left;
      int gy = pen_y - gl->bitmap_top;
      for (unsigned row = 0; row < bm->rows; row++) {
        const unsigned char *src = bm->buffer + (int)row * bm->pitch;
        for (unsigned col = 0; col < bm->width; col++) {
          unsigned char a = src[col];
          if (!a)
            continue;
          if (is_outline) {
            for (int dy = -spread; dy <= spread; dy++)
              for (int dx = -spread; dx <= spread; dx++) {
                int py = gy + (int)row + dy, px = gx + (int)col + dx;
                if (py < 0 || py >= H || px < 0 || px >= W)
                  continue;
                blend(rgba + ((size_t)py * W + px) * 4, kr, kg, kb, a);
              }
          } else {
            int py = gy + (int)row, px = gx + (int)col;
            if (py < 0 || py >= H || px < 0 || px >= W)
              continue;
            blend(rgba + ((size_t)py * W + px) * 4, cr, cg, cb, a);
          }
        }
      }
      pen_x += (int)(gl->advance.x >> 6);
    }
  }

  void *arr = jni_shim_new_int_array((const int *)rgba, W * H);
  dc(env, NULL, W, H, arr);
  free(rgba);
}
