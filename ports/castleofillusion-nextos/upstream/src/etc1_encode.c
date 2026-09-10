/* etc1_encode.c -- encoder ETC1 PROPRIO (do zero, correto). Ver etc1_encode.h.
 *
 * Formato ETC1 (8 bytes/bloco 4x4, big-endian no word de 64 bits):
 *   individual (diff=0): R1,R2,G1,G2,B1,B2 cada 4 bits.
 *   differential (diff=1): R1 5b + dR2 3b-signed; idem G,B.
 *   table1[3b] table2[3b] diff[1b] flip[1b]  (byte 3)
 *   plano MSB (bytes 4-5) + plano LSB (bytes 6-7); pixel p = x*4+y -> bit p.
 *   modifier = kMod[table][sel], sel=(msb<<1)|lsb (0..3).
 * Sub-blocos: flip=0 -> colunas {0,1}|{2,3}; flip=1 -> linhas {0,1}|{2,3}.
 */
#include "etc1_encode.h"
#include <string.h>

/* tabela de modifiers do ETC1, ordenada pelo indice de 2 bits (casa o decoder GL):
 * sel 0 -> +pequeno, 1 -> +grande, 2 -> -pequeno, 3 -> -grande. */
static const int kMod[8][4] = {
  {  2,   8,  -2,   -8}, {  5,  17,  -5,  -17}, {  9,  29,  -9,  -29}, { 13,  42, -13,  -42},
  { 18,  60, -18,  -60}, { 24,  80, -24,  -80}, { 33, 106, -33, -106}, { 47, 183, -47, -183}
};

static inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* modo RÁPIDO (default ON): escolhe o flip por heurística e pula o refino de base.
 * ~4-5× mais rápido que o exaustivo, qualidade ~igual (refino e o 2º flip davam ganho
 * marginal). etc1_set_fast(0) volta pro exaustivo (máxima qualidade). */
static int s_etc1_fast = 1;
void etc1_set_fast(int f) { s_etc1_fast = f; }

/* expande 4 bits -> 8 (replicacao de nibble) e 5 bits -> 8. */
static inline int exp4(int v4) { return (v4 << 4) | v4; }
static inline int exp5(int v5) { return (v5 << 3) | (v5 >> 2); }

/* Para um sub-bloco (n pixels), dada a cor base ja EXPANDIDA p/ 8 bits,
 * acha a melhor tabela + selectors; retorna erro SSD e preenche sel[n]. */
static long sub_best(const uint8_t *px[], int n, int br, int bg, int bb,
                     int *best_table, int sel_out[]) {
  long best_err = -1;
  for (int t = 0; t < 8; t++) {
    long err = 0; int sel[8];
    for (int i = 0; i < n; i++) {
      int r = px[i][0], g = px[i][1], b = px[i][2];
      long be = -1; int bs = 0;
      for (int s = 0; s < 4; s++) {
        int m = kMod[t][s];
        int dr = clamp8(br + m) - r, dg = clamp8(bg + m) - g, db = clamp8(bb + m) - b;
        long e = (long)dr * dr + (long)dg * dg + (long)db * db;
        if (be < 0 || e < be) { be = e; bs = s; }
      }
      err += be; sel[i] = bs;
    }
    if (best_err < 0 || err < best_err) {
      best_err = err; *best_table = t;
      for (int i = 0; i < n; i++) sel_out[i] = sel[i];
    }
  }
  return best_err;
}

/* media de cor de um conjunto de pixels */
static void avg_color(const uint8_t *px[], int n, int *r, int *g, int *b) {
  long sr = 0, sg = 0, sb = 0;
  for (int i = 0; i < n; i++) { sr += px[i][0]; sg += px[i][1]; sb += px[i][2]; }
  *r = (int)(sr / n); *g = (int)(sg / n); *b = (int)(sb / n);
}

static inline int q4(int v8) { int q = (v8 * 15 + 127) / 255; return q < 0 ? 0 : (q > 15 ? 15 : q); }
static inline int q5(int v8) { int q = (v8 * 31 + 127) / 255; return q < 0 ? 0 : (q > 31 ? 31 : q); }

/* refino de base (1 iteração Lloyd): dada a tabela+selectors, a base ideal é a
 * média de (pixel - modifier). Re-quantiza p/ 4 ou 5 bits. Melhora blocos de alta
 * variância (folhagem/pedras) sem custo de qualidade nos suaves. */
static void refine_base(const uint8_t *px[], const int *sel, int n, int table,
                        int bits, int outq[3]) {
  long s[3] = {0, 0, 0};
  for (int i = 0; i < n; i++) {
    int m = kMod[table][sel[i]];
    s[0] += px[i][0] - m; s[1] += px[i][1] - m; s[2] += px[i][2] - m;
  }
  for (int c = 0; c < 3; c++) {
    int v = clamp8((int)(s[c] / n));
    outq[c] = (bits == 4) ? q4(v) : q5(v);
  }
}

static void pack_block(int diff, int flip,
                       int c1[3], int c2[3], int t1, int t2,
                       const int sel[16], uint8_t out[8]) {
  uint8_t b[8]; memset(b, 0, 8);
  if (!diff) {                 /* c1,c2 = nibbles 0..15 */
    b[0] = (uint8_t)((c1[0] << 4) | (c2[0] & 0xF));
    b[1] = (uint8_t)((c1[1] << 4) | (c2[1] & 0xF));
    b[2] = (uint8_t)((c1[2] << 4) | (c2[2] & 0xF));
  } else {                     /* c1 = 5 bits; c2 = diff 3-bit signed */
    b[0] = (uint8_t)((c1[0] << 3) | (c2[0] & 0x7));
    b[1] = (uint8_t)((c1[1] << 3) | (c2[1] & 0x7));
    b[2] = (uint8_t)((c1[2] << 3) | (c2[2] & 0x7));
  }
  b[3] = (uint8_t)(((t1 & 7) << 5) | ((t2 & 7) << 2) | ((diff & 1) << 1) | (flip & 1));
  uint16_t msb = 0, lsb = 0;
  for (int p = 0; p < 16; p++) {
    if (sel[p] & 2) msb |= (uint16_t)(1u << p);
    if (sel[p] & 1) lsb |= (uint16_t)(1u << p);
  }
  b[4] = (uint8_t)(msb >> 8); b[5] = (uint8_t)(msb & 0xFF);
  b[6] = (uint8_t)(lsb >> 8); b[7] = (uint8_t)(lsb & 0xFF);
  memcpy(out, b, 8);
}

/* heurística: qual split deixa os 2 sub-blocos mais uniformes (menor variância).
 * flip=0 = colunas {0,1}|{2,3} (esq/dir); flip=1 = linhas (topo/baixo). */
static int pick_flip(const uint8_t *blk) {
#define PXF(x, y) (blk + (((y) * 4 + (x)) * 4))
  long errH = 0, errV = 0;
  /* esq/dir (flip=0) */
  for (int half = 0; half < 2; half++) {
    long s[3] = {0,0,0}; int x0 = half * 2;
    for (int x = x0; x < x0 + 2; x++) for (int y = 0; y < 4; y++) { const uint8_t *p = PXF(x,y); s[0]+=p[0]; s[1]+=p[1]; s[2]+=p[2]; }
    int m[3] = {(int)(s[0]/8),(int)(s[1]/8),(int)(s[2]/8)};
    for (int x = x0; x < x0 + 2; x++) for (int y = 0; y < 4; y++) { const uint8_t *p = PXF(x,y); errH += (p[0]-m[0])*(p[0]-m[0])+(p[1]-m[1])*(p[1]-m[1])+(p[2]-m[2])*(p[2]-m[2]); }
  }
  /* topo/baixo (flip=1) */
  for (int half = 0; half < 2; half++) {
    long s[3] = {0,0,0}; int y0 = half * 2;
    for (int y = y0; y < y0 + 2; y++) for (int x = 0; x < 4; x++) { const uint8_t *p = PXF(x,y); s[0]+=p[0]; s[1]+=p[1]; s[2]+=p[2]; }
    int m[3] = {(int)(s[0]/8),(int)(s[1]/8),(int)(s[2]/8)};
    for (int y = y0; y < y0 + 2; y++) for (int x = 0; x < 4; x++) { const uint8_t *p = PXF(x,y); errV += (p[0]-m[0])*(p[0]-m[0])+(p[1]-m[1])*(p[1]-m[1])+(p[2]-m[2])*(p[2]-m[2]); }
  }
#undef PXF
  return errH <= errV ? 0 : 1;
}

void etc1_encode_block_rgba(const uint8_t *blk, uint8_t out[8]) {
  /* blk: row-major, pixel (x,y) em offset (y*4+x)*4. sel idx p = x*4+y. */
#define PX(x, y) (blk + (((y) * 4 + (x)) * 4))

  long best_total = -1;
  int best_diff = 0, best_flip = 0, best_t1 = 0, best_t2 = 0;
  int best_c1[3] = {0,0,0}, best_c2[3] = {0,0,0};
  int best_sel[16] = {0};

  int flip0 = 0, flip1 = 2;
  if (s_etc1_fast) { flip0 = pick_flip(blk); flip1 = flip0 + 1; }  /* só 1 flip */
  for (int flip = flip0; flip < flip1; flip++) {
    /* monta os 2 sub-blocos (8 px cada) + mapeamento p/ sel global */
    const uint8_t *s1[8], *s2[8]; int p1[8], p2[8];
    int n = 0, m = 0;
    for (int x = 0; x < 4; x++)
      for (int y = 0; y < 4; y++) {
        int first = flip ? (y < 2) : (x < 2);
        int pidx = x * 4 + y;
        if (first) { s1[n] = PX(x, y); p1[n] = pidx; n++; }
        else       { s2[m] = PX(x, y); p2[m] = pidx; m++; }
      }

    int a1[3], a2[3];
    avg_color(s1, 8, &a1[0], &a1[1], &a1[2]);
    avg_color(s2, 8, &a2[0], &a2[1], &a2[2]);

    /* ----- modo INDIVIDUAL (4 bits por canal, bases independentes) ----- */
    {
      int q1[3] = { q4(a1[0]), q4(a1[1]), q4(a1[2]) };
      int q2[3] = { q4(a2[0]), q4(a2[1]), q4(a2[2]) };
      int t1, t2, sel1[8], sel2[8];
      long err = sub_best(s1, 8, exp4(q1[0]), exp4(q1[1]), exp4(q1[2]), &t1, sel1)
               + sub_best(s2, 8, exp4(q2[0]), exp4(q2[1]), exp4(q2[2]), &t2, sel2);
      if (!s_etc1_fast) {                    /* refino (1 iteração) só no modo lento */
        refine_base(s1, sel1, 8, t1, 4, q1);
        refine_base(s2, sel2, 8, t2, 4, q2);
        err = sub_best(s1, 8, exp4(q1[0]), exp4(q1[1]), exp4(q1[2]), &t1, sel1)
            + sub_best(s2, 8, exp4(q2[0]), exp4(q2[1]), exp4(q2[2]), &t2, sel2);
      }
      if (best_total < 0 || err < best_total) {
        best_total = err; best_diff = 0; best_flip = flip;
        best_t1 = t1; best_t2 = t2;
        for (int k = 0; k < 3; k++) { best_c1[k] = q1[k]; best_c2[k] = q2[k]; }
        for (int i = 0; i < 8; i++) { best_sel[p1[i]] = sel1[i]; best_sel[p2[i]] = sel2[i]; }
      }
    }

    /* ----- modo DIFFERENTIAL (5 bits base1, base2 = base1 + diff[-4..3]) ----- */
    {
      int b1[3] = { q5(a1[0]), q5(a1[1]), q5(a1[2]) };
      int b2[3], d2[3]; int ok = 1;
      for (int k = 0; k < 3; k++) {
        int want = q5(a2[k]);
        int d = want - b1[k];
        if (d < -4) d = -4; if (d > 3) d = 3;
        b2[k] = b1[k] + d;
        if (b2[k] < 0 || b2[k] > 31) { ok = 0; break; }
        d2[k] = d & 0x7;
      }
      if (ok && !s_etc1_fast) {              /* refino só no modo lento */
        int t1, t2, sel1[8], sel2[8];
        sub_best(s1, 8, exp5(b1[0]), exp5(b1[1]), exp5(b1[2]), &t1, sel1);
        sub_best(s2, 8, exp5(b2[0]), exp5(b2[1]), exp5(b2[2]), &t2, sel2);
        /* refino: base1 livre (5b); base2 = base1 + diff[-4..3] (re-deriva o diff) */
        refine_base(s1, sel1, 8, t1, 5, b1);
        for (int k = 0; k < 3; k++) {
          long acc = 0; for (int i = 0; i < 8; i++) acc += s2[i][k] - kMod[t2][sel2[i]];
          int want = q5(clamp8((int)(acc / 8)));
          int d = want - b1[k]; if (d < -4) d = -4; if (d > 3) d = 3;
          int nb = b1[k] + d; if (nb < 0 || nb > 31) { ok = 0; break; }
          b2[k] = nb; d2[k] = d & 0x7;
        }
      }
      if (ok) {
        int t1, t2, sel1[8], sel2[8];
        long err = sub_best(s1, 8, exp5(b1[0]), exp5(b1[1]), exp5(b1[2]), &t1, sel1)
                 + sub_best(s2, 8, exp5(b2[0]), exp5(b2[1]), exp5(b2[2]), &t2, sel2);
        if (best_total < 0 || err < best_total) {
          best_total = err; best_diff = 1; best_flip = flip;
          best_t1 = t1; best_t2 = t2;
          best_c1[0] = b1[0]; best_c1[1] = b1[1]; best_c1[2] = b1[2];
          best_c2[0] = d2[0]; best_c2[1] = d2[1]; best_c2[2] = d2[2];
          for (int i = 0; i < 8; i++) { best_sel[p1[i]] = sel1[i]; best_sel[p2[i]] = sel2[i]; }
        }
      }
    }
  }

  pack_block(best_diff, best_flip, best_c1, best_c2, best_t1, best_t2, best_sel, out);
#undef PX
}

void etc1_encode_image(const uint8_t *rgba, int w, int h, int channels, uint8_t *out) {
  int bw = w / 4, bh = h / 4;
  uint8_t block[16 * 4];
  for (int by = 0; by < bh; by++) {
    for (int bx = 0; bx < bw; bx++) {
      /* monta o bloco 4x4 em RGBA contiguo (preenche alpha=255 se channels=3) */
      for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
          const uint8_t *src = rgba + (((size_t)(by * 4 + y) * w) + (bx * 4 + x)) * channels;
          uint8_t *dst = block + (y * 4 + x) * 4;
          dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
          dst[3] = (channels == 4) ? src[3] : 255;
        }
      }
      uint8_t *o = out + ((size_t)by * bw + bx) * 8;
      etc1_encode_block_rgba(block, o);
    }
  }
}
