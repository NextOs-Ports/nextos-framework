/* etc2_decode.c — decoder ETC2 → RGBA8888 na CPU (GLES2-universal, até Utgard).
 * Formatos: ETC2_RGB8 (0x9274), ETC2_RGBA8 (0x9278, EAC alpha),
 * ETC2_PUNCHTHROUGH_A1 (0x9276). Blocos 4x4; RGB=8B, RGBA8=16B (8B EAC + 8B cor).
 * Referência: Khronos Data Format Spec / OpenGL ES 3.0 §C.1. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const int etc_mod[8][2] = {
    {2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
static const int etc2_dist[8] = {3, 6, 11, 16, 23, 32, 41, 64};
static const int eac_tab[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14},   {-3, -7, -10, -13, 2, 6, 9, 12},
    {-2, -5, -8, -13, 1, 4, 7, 12},   {-2, -4, -6, -13, 1, 3, 5, 12},
    {-3, -6, -8, -12, 2, 5, 7, 11},   {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10},   {-3, -5, -8, -11, 2, 4, 7, 10},
    {-2, -6, -8, -10, 1, 5, 7, 9},    {-2, -5, -8, -10, 1, 4, 7, 9},
    {-2, -4, -8, -10, 1, 3, 7, 9},    {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},    {-1, -2, -3, -10, 0, 1, 2, 9},
    {-4, -6, -8, -9, 3, 5, 7, 8},     {-3, -5, -7, -9, 2, 4, 6, 8}};

static inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }
static inline int ext4(int v) { return (v << 4) | v; }
static inline int ext5(int v) { return (v << 3) | (v >> 2); }
static inline int ext6(int v) { return (v << 2) | (v >> 4); }
static inline int ext7(int v) { return (v << 1) | (v >> 6); }
static inline int sext3(int v) { return v >= 4 ? v - 8 : v; }

/* decodifica um bloco de COR 4x4 (8 bytes em src) p/ out RGBA (stride 16/линha
 * de bloco = out[(y*4+x)*4]); punch=1 ativa o modo punchthrough A1. */
static void etc2_color_block(const uint8_t *src, uint8_t out[16][4], int punch) {
  uint32_t hi = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) |
                ((uint32_t)src[2] << 8) | src[3];
  uint32_t lo = ((uint32_t)src[4] << 24) | ((uint32_t)src[5] << 16) |
                ((uint32_t)src[6] << 8) | src[7];
  int diff = (hi >> 1) & 1;  /* no punchthrough = flag opaque */
  int opaque = diff;
  int flip = hi & 1;
  for (int i = 0; i < 16; i++) { out[i][3] = 255; }

  if (!punch && !diff) {
    /* individual: 4+4 bits por canal */
    int r1 = ext4((hi >> 28) & 0xF), r2 = ext4((hi >> 24) & 0xF);
    int g1 = ext4((hi >> 20) & 0xF), g2 = ext4((hi >> 16) & 0xF);
    int b1 = ext4((hi >> 12) & 0xF), b2 = ext4((hi >> 8) & 0xF);
    int t1 = (hi >> 5) & 7, t2 = (hi >> 2) & 7;
    for (int p = 0; p < 16; p++) {
      int x = p >> 2, y = p & 3; /* coluna-major */
      int sub = flip ? (y >= 2) : (x >= 2);
      int msb = (lo >> (p + 16)) & 1, lsb = (lo >> p) & 1;
      int idx = (msb << 1) | lsb;
      int m = etc_mod[sub ? t2 : t1][idx & 1];
      if (!(idx & 2)) m = -m; /* idx 0,1 = -mod? não: tabela ETC1 */
      /* ETC1 index: 0=+small 1=-small? Ordem oficial: idx2b {0:+a,1:+b?}.
       * Convenção: modifier = tabela[t][idx&1], sinal = (idx&2)? -1 : +1 é
       * INVERTIDO — espec: idx 0=a,1=b,2=-a... usamos LUT direta: */
      static const int sign[4] = {1, 1, -1, -1};
      static const int mag[4] = {0, 1, 0, 1};
      m = etc_mod[sub ? t2 : t1][mag[idx]] * sign[idx];
      int r = sub ? r2 : r1, g = sub ? g2 : g1, b = sub ? b2 : b1;
      int o = (y & 3) * 4 + x; /* out é row-major [y][x] */
      out[o][0] = clamp8(r + m); out[o][1] = clamp8(g + m); out[o][2] = clamp8(b + m);
    }
    return;
  }

  /* differential / modos ETC2 */
  int r1_5 = (hi >> 27) & 0x1F, dr = sext3((hi >> 24) & 7);
  int g1_5 = (hi >> 19) & 0x1F, dg = sext3((hi >> 16) & 7);
  int b1_5 = (hi >> 11) & 0x1F, db = sext3((hi >> 8) & 7);
  int r2_5 = r1_5 + dr, g2_5 = g1_5 + dg, b2_5 = b1_5 + db;

  if (r2_5 < 0 || r2_5 > 31) {
    /* ===== T mode ===== */
    int r1 = ext4(((hi >> 27) & 0xC) | ((hi >> 24) & 0x3));
    int g1 = ext4((hi >> 20) & 0xF), b1 = ext4((hi >> 16) & 0xF);
    int r2 = ext4((hi >> 12) & 0xF), g2 = ext4((hi >> 8) & 0xF);
    int b2 = ext4((hi >> 4) & 0xF);
    int d = etc2_dist[(((hi >> 2) & 3) << 1) | (hi & 1)];
    uint8_t paint[4][3] = {
        {(uint8_t)r1, (uint8_t)g1, (uint8_t)b1},
        {clamp8(r2 + d), clamp8(g2 + d), clamp8(b2 + d)},
        {(uint8_t)r2, (uint8_t)g2, (uint8_t)b2},
        {clamp8(r2 - d), clamp8(g2 - d), clamp8(b2 - d)}};
    for (int p = 0; p < 16; p++) {
      int x = p >> 2, y = p & 3;
      int idx = ((((lo >> (p + 16)) & 1)) << 1) | ((lo >> p) & 1);
      int o = y * 4 + x;
      out[o][0] = paint[idx][0]; out[o][1] = paint[idx][1]; out[o][2] = paint[idx][2];
      if (punch && !opaque && idx == 2) { out[o][0] = out[o][1] = out[o][2] = 0; out[o][3] = 0; }
    }
    return;
  }
  if (g2_5 < 0 || g2_5 > 31) {
    /* ===== H mode ===== */
    int r1 = ext4((hi >> 27) & 0xF);
    int g1 = ext4((((hi >> 24) & 7) << 1) | ((hi >> 20) & 1));
    int b1 = ext4((((hi >> 19) & 1) << 3) | ((hi >> 15) & 7));
    int r2 = ext4((hi >> 11) & 0xF), g2 = ext4((hi >> 7) & 0xF);
    int b2 = ext4((hi >> 3) & 0xF);
    int di = (((hi >> 2) & 1) << 2) | ((hi & 1) << 1);
    int v1 = (r1 << 16) | (g1 << 8) | b1, v2 = (r2 << 16) | (g2 << 8) | b2;
    if (v1 >= v2) di |= 1;
    int d = etc2_dist[di];
    uint8_t paint[4][3] = {
        {clamp8(r1 + d), clamp8(g1 + d), clamp8(b1 + d)},
        {clamp8(r1 - d), clamp8(g1 - d), clamp8(b1 - d)},
        {clamp8(r2 + d), clamp8(g2 + d), clamp8(b2 + d)},
        {clamp8(r2 - d), clamp8(g2 - d), clamp8(b2 - d)}};
    for (int p = 0; p < 16; p++) {
      int x = p >> 2, y = p & 3;
      int idx = ((((lo >> (p + 16)) & 1)) << 1) | ((lo >> p) & 1);
      int o = y * 4 + x;
      out[o][0] = paint[idx][0]; out[o][1] = paint[idx][1]; out[o][2] = paint[idx][2];
      if (punch && !opaque && idx == 2) { out[o][0] = out[o][1] = out[o][2] = 0; out[o][3] = 0; }
    }
    return;
  }
  if (b2_5 < 0 || b2_5 > 31) {
    /* ===== planar ===== */
    int ro = ext6((hi >> 25) & 0x3F);
    int go = ext7((((hi >> 24) & 1) << 6) | ((hi >> 17) & 0x3F));
    int bo = ext6((((hi >> 16) & 1) << 5) | (((hi >> 11) & 3) << 3) | ((hi >> 7) & 7));
    int rh = ext6((((hi >> 2) & 0x1F) << 1) | (hi & 1));
    int gh = ext7((lo >> 25) & 0x7F);
    int bh = ext6((lo >> 19) & 0x3F);
    int rv = ext6((lo >> 13) & 0x3F);
    int gv = ext7((lo >> 6) & 0x7F);
    int bv = ext6(lo & 0x3F);
    for (int y = 0; y < 4; y++)
      for (int x = 0; x < 4; x++) {
        int o = y * 4 + x;
        out[o][0] = clamp8((x * (rh - ro) + y * (rv - ro) + 4 * ro + 2) >> 2);
        out[o][1] = clamp8((x * (gh - go) + y * (gv - go) + 4 * go + 2) >> 2);
        out[o][2] = clamp8((x * (bh - bo) + y * (bv - bo) + 4 * bo + 2) >> 2);
      }
    return;
  }
  /* ===== differential normal ===== */
  int r1 = ext5(r1_5), g1 = ext5(g1_5), b1 = ext5(b1_5);
  int r2 = ext5(r2_5), g2 = ext5(g2_5), b2 = ext5(b2_5);
  int t1 = (hi >> 5) & 7, t2 = (hi >> 2) & 7;
  static const int sign[4] = {1, 1, -1, -1};
  static const int mag[4] = {0, 1, 0, 1};
  for (int p = 0; p < 16; p++) {
    int x = p >> 2, y = p & 3;
    int sub = flip ? (y >= 2) : (x >= 2);
    int idx = ((((lo >> (p + 16)) & 1)) << 1) | ((lo >> p) & 1);
    int m = etc_mod[sub ? t2 : t1][mag[idx]] * sign[idx];
    int o = y * 4 + x;
    if (punch && !opaque) {
      if (idx == 2) { out[o][0] = out[o][1] = out[o][2] = 0; out[o][3] = 0; continue; }
      if (idx == 0) m = 0; /* punchthrough não-opaco: idx0 sem modifier */
    }
    int r = sub ? r2 : r1, g = sub ? g2 : g1, b = sub ? b2 : b1;
    out[o][0] = clamp8(r + m); out[o][1] = clamp8(g + m); out[o][2] = clamp8(b + m);
  }
}

/* bloco EAC alpha (8 bytes) → alphas[16] (row-major y*4+x) */
static void eac_alpha_block(const uint8_t *src, uint8_t alphas[16]) {
  int base = src[0];
  int mult = (src[1] >> 4) & 0xF;
  const int *tab = eac_tab[src[1] & 0xF];
  uint64_t bits = 0;
  for (int i = 2; i < 8; i++) bits = (bits << 8) | src[i];
  for (int p = 0; p < 16; p++) {
    /* índices 3-bit, pixel order coluna-major, MSB primeiro */
    int idx = (int)((bits >> (45 - p * 3)) & 7);
    int x = p >> 2, y = p & 3;
    alphas[y * 4 + x] = clamp8(base + tab[idx] * mult);
  }
}

/* API: decodifica uma textura ETC2 inteira p/ RGBA8888 (caller dá free).
 * fmt: 0x9274/0x9275=RGB8, 0x9276/0x9277=punchthrough, 0x9278/0x9279=RGBA8.
 * Retorna NULL se formato não suportado ou tamanho não bate. */
unsigned char *etc2_decode_rgba(unsigned fmt, int w, int h, const void *data,
                                int size) {
  int bw = (w + 3) / 4, bh = (h + 3) / 4;
  int has_eac = (fmt == 0x9278 || fmt == 0x9279);
  int punch = (fmt == 0x9276 || fmt == 0x9277);
  int rgb = (fmt == 0x9274 || fmt == 0x9275);
  if (!has_eac && !punch && !rgb) return NULL;
  int bsize = has_eac ? 16 : 8;
  if (size < bw * bh * bsize) return NULL;
  unsigned char *out = malloc((size_t)w * h * 4);
  if (!out) return NULL;
  const uint8_t *src = (const uint8_t *)data;
  for (int by = 0; by < bh; by++)
    for (int bx = 0; bx < bw; bx++) {
      const uint8_t *blk = src + ((size_t)by * bw + bx) * bsize;
      uint8_t px[16][4]; uint8_t al[16];
      if (has_eac) { eac_alpha_block(blk, al); blk += 8; }
      etc2_color_block(blk, px, punch);
      for (int y = 0; y < 4; y++) {
        int ty = by * 4 + y;
        if (ty >= h) break;
        for (int x = 0; x < 4; x++) {
          int tx = bx * 4 + x;
          if (tx >= w) break;
          unsigned char *d = out + ((size_t)ty * w + tx) * 4;
          d[0] = px[y * 4 + x][0]; d[1] = px[y * 4 + x][1];
          d[2] = px[y * 4 + x][2];
          d[3] = has_eac ? al[y * 4 + x] : px[y * 4 + x][3];
        }
      }
    }
  return out;
}
