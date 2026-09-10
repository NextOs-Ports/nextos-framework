/*
 * text_render.c -- renderiza texto UTF-8 num buffer RGBA usando FreeType +
 * Roboto-Regular (a fonte default da plataforma Android que o Chrono Trigger
 * usa nas labels com fontName vazio). Serve o caminho nativo do jogo
 * (Cocos2dxBitmap.createTextBitmapShadowStroke -> nativeInitBitmapDC).
 */
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"

#include <stdio.h>

/* A fonte NAO e' dado do jogo: o Chrono Trigger deixa fontName vazio e conta
 * com a fonte default do Android (Roboto). O pacote publico resolve isso por
 * DETECCAO, nunca por caminho cravado nem redistribuindo fonte do Android:
 *   1) CHRONO_FONT (diagnostico/override do usuario);
 *   2) a fonte de licenca livre que acompanha o port (fonts/, SIL OFL 1.1);
 *   3) o layout historico NextOS (Roboto ao lado do binario);
 *   4) qualquer sans do proprio FIRMWARE, na ordem de semelhanca com a Roboto.
 */
static const char *const kFirmwareFonts[] = {
  "/usr/share/fonts/truetype/roboto/Roboto-Regular.ttf",
  "/usr/share/fonts/truetype/roboto/unhinted/RobotoTTF/Roboto-Regular.ttf",
  "/usr/share/fonts/roboto/Roboto-Regular.ttf",
  "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
  "/usr/share/fonts/noto/NotoSans-Regular.ttf",
  "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
  "/usr/share/fonts/dejavu/DejaVuSans.ttf",
  "/usr/share/fonts/TTF/DejaVuSans.ttf",
  "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
  "/usr/share/fonts/liberation-fonts/LiberationSans-Regular.ttf",
  "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
  NULL
};

static FT_Library g_ft;
static FT_Face g_face;
static int g_ready = -1;

/* FreeType por dlopen, nunca DT_NEEDED: libfreetype.so.6 nao consta nos
 * contratos de toda CFW (muOS/spruce/knulli) e um NEEDED ausente impede o
 * port de CARREGAR. Com a lib presente nada muda; sem ela, so este caminho
 * de bitmap de texto degrada (retorna sem bitmap) e o jogo segue. Os tipos
 * FT_* continuam vindo do header (compile-time). */
#include <dlfcn.h>
static FT_Error (*p_FT_Init_FreeType)(FT_Library *);
static FT_Error (*p_FT_New_Face)(FT_Library, const char *, FT_Long, FT_Face *);
static FT_Error (*p_FT_Set_Pixel_Sizes)(FT_Face, FT_UInt, FT_UInt);
static FT_Error (*p_FT_Load_Char)(FT_Face, FT_ULong, FT_Int32);

static int ft_dl_ready(void) {
  static int state; /* 0 = nao tentado, 1 = ok, -1 = ausente */
  if (state) return state > 0;
  void *h = dlopen("libfreetype.so.6", RTLD_NOW | RTLD_LOCAL);
  if (!h) h = dlopen("libfreetype.so", RTLD_NOW | RTLD_LOCAL);
  if (h) {
    p_FT_Init_FreeType = dlsym(h, "FT_Init_FreeType");
    p_FT_New_Face = dlsym(h, "FT_New_Face");
    p_FT_Set_Pixel_Sizes = dlsym(h, "FT_Set_Pixel_Sizes");
    p_FT_Load_Char = dlsym(h, "FT_Load_Char");
  }
  if (p_FT_Init_FreeType && p_FT_New_Face && p_FT_Set_Pixel_Sizes &&
      p_FT_Load_Char) {
    state = 1;
    return 1;
  }
  debugPrintf("text_render: libfreetype ausente na firmware -- "
              "bitmaps de texto Java desativados (jogo segue)\n");
  state = -1;
  return 0;
}

static int try_face(const char *path) {
  if (!path || !*path) return 0;
  if (p_FT_New_Face(g_ft, path, 0, &g_face)) return 0;
  debugPrintf("text_render: fonte '%s' glyphs=%ld\n", path, (long)g_face->num_glyphs);
  return 1;
}

static int ensure_ft(void) {
  if (g_ready >= 0) return g_ready;
  g_ready = 0;
  if (!ft_dl_ready()) return 0;
  if (p_FT_Init_FreeType(&g_ft)) { debugPrintf("text_render: FT_Init falhou\n"); return 0; }

  if (try_face(getenv("CHRONO_FONT"))) { g_ready = 1; return 1; }

  const char *gamedir = getenv("CHRONO_GAMEDIR");
  if (!gamedir || !*gamedir) gamedir = getenv("HOME");
  if (gamedir && *gamedir) {
    static const char *const kLocal[] = {
      "fonts/NotoSans-Regular.ttf", "fonts/Roboto-Regular.ttf",
      "Roboto-Regular.ttf", NULL
    };
    for (int i = 0; kLocal[i]; i++) {
      char path[1024];
      snprintf(path, sizeof path, "%s/%s", gamedir, kLocal[i]);
      if (try_face(path)) { g_ready = 1; return 1; }
    }
  }
  for (int i = 0; kFirmwareFonts[i]; i++)
    if (try_face(kFirmwareFonts[i])) { g_ready = 1; return 1; }

  debugPrintf("text_render: NENHUMA fonte encontrada (nem no port, nem no "
              "firmware) — a UI ficaria sem texto\n");
  return 0;
}

/* decodifica 1 codepoint UTF-8; avanca *s */
static unsigned utf8_next(const char **s) {
  const unsigned char *p = (const unsigned char *)*s;
  unsigned c = *p++;
  if (c < 0x80) { }
  else if ((c >> 5) == 0x6) { c = ((c & 0x1F) << 6) | (*p++ & 0x3F); }
  else if ((c >> 4) == 0xE) { c = ((c & 0x0F) << 12); c |= (*p++ & 0x3F) << 6; c |= (*p++ & 0x3F); }
  else if ((c >> 3) == 0x1E) { c = ((c & 0x07) << 18); c |= (*p++ & 0x3F) << 12; c |= (*p++ & 0x3F) << 6; c |= (*p++ & 0x3F); }
  *s = (const char *)p;
  return c;
}

/* Renderiza 'utf8' tam 'px' cor (r,g,b). Devolve RGBA malloc'd (premult NAO),
 * preenche *outW/*outH. align: 0=left 1=center 2=right (afeta so layout no canvas).
 * Se reqW/reqH > 0, usa esse tamanho de canvas; senao calcula. Caller faz free. */
unsigned char *chrono_render_text(const char *utf8, int px, int r, int g, int b,
                                  int align, int reqW, int reqH,
                                  int *outW, int *outH) {
  if (!ensure_ft() || !utf8) return NULL;
  if (px < 6) px = 6; if (px > 200) px = 200;
  p_FT_Set_Pixel_Sizes(g_face, 0, px);
  int ascent = g_face->size->metrics.ascender >> 6;
  int descent = -(g_face->size->metrics.descender >> 6);
  int lineh = g_face->size->metrics.height >> 6;
  if (lineh < ascent + descent) lineh = ascent + descent;

  /* medir: largura por linha (\n quebra), numero de linhas */
  int maxw = 0, curw = 0, lines = 1;
  for (const char *s = utf8; *s;) {
    unsigned cp = utf8_next(&s);
    if (cp == '\n') { if (curw > maxw) maxw = curw; curw = 0; lines++; continue; }
    if (p_FT_Load_Char(g_face, cp, FT_LOAD_DEFAULT)) continue;
    curw += g_face->glyph->advance.x >> 6;
  }
  if (curw > maxw) maxw = curw;

  /* SEMPRE auto-dimensiona pelo conteudo: os reqW/reqH vindos do jogo sao
     pequenos/ruins (clipam o texto). O jogo posiciona o label pelo tamanho real. */
  (void)reqW; (void)reqH;
  int W = maxw + 2;
  int H = lines * lineh + 2;
  if (W < 1) W = 1; if (H < 1) H = 1;
  if (W > 2048) W = 2048; if (H > 2048) H = 2048;
  unsigned char *rgba = calloc((size_t)W * H * 4, 1);
  if (!rgba) return NULL;

  int line = 0;
  int pen_y = ascent;        /* baseline da 1a linha */
  int line_w0 = maxw;        /* largura da 1a linha p/ alinhar (simplif: usa maxw) */
  (void)line_w0;
  int pen_x = 0;
  /* alinhamento horizontal do bloco inteiro dentro do canvas */
  if (align == 1) pen_x = (W - maxw) / 2; else if (align == 2) pen_x = W - maxw;
  if (pen_x < 0) pen_x = 0;
  int start_x = pen_x;

  for (const char *s = utf8; *s;) {
    unsigned cp = utf8_next(&s);
    if (cp == '\n') { line++; pen_y = ascent + line * lineh; pen_x = start_x; continue; }
    if (p_FT_Load_Char(g_face, cp, FT_LOAD_RENDER)) continue;
    FT_GlyphSlot gl = g_face->glyph;
    FT_Bitmap *bm = &gl->bitmap;
    int gx = pen_x + gl->bitmap_left;
    int gy = pen_y - gl->bitmap_top;
    for (unsigned row = 0; row < bm->rows; row++) {
      int py = gy + (int)row;
      if (py < 0 || py >= H) continue;
      const unsigned char *src = bm->buffer + row * bm->pitch;
      for (unsigned col = 0; col < bm->width; col++) {
        int pxp = gx + (int)col;
        if (pxp < 0 || pxp >= W) continue;
        unsigned char a = src[col];
        if (!a) continue;
        unsigned char *dst = rgba + ((size_t)py * W + pxp) * 4;
        /* sobrepoe (max no alpha) */
        if (a >= dst[3]) { dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = a; }
      }
    }
    pen_x += gl->advance.x >> 6;
  }

  if (getenv("CHRONO_TEXTLOG"))
    debugPrintf("render '%s' -> maxw=%d lines=%d W=%d H=%d px=%d\n", utf8, maxw, lines, W, H, px);
  if (outW) *outW = W;
  if (outH) *outH = H;
  return rgba;
}
