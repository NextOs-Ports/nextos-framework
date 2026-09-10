/* Encoder ETC1 do zero (clean-room, so a partir da spec OES).
 *
 * Formato do bloco (64 bits, big-endian na memoria):
 *   bits 63..40: cores-base dos 2 subblocos
 *     modo individual (diff=0): R1[4] R2[4] G1[4] G2[4] B1[4] B2[4]
 *     modo diferencial (diff=1): R1[5] dR2[3] G1[5] dG2[3] B1[5] dB2[3]
 *   bits 39..37: tabela do subbloco 1   bits 36..34: tabela do subbloco 2
 *   bit  33: diff   bit 32: flip
 *   bits 31..16: MSB do indice de cada texel   bits 15..0: LSB
 *     texel i = coluna*4 + linha (column-major); bit i em cada plano.
 *   indice (msb,lsb): (0,0)=+pequeno (0,1)=+grande (1,0)=-pequeno (1,1)=-grande
 *   flip=0: subblocos 2x4 lado a lado; flip=1: 4x2 em cima/embaixo.
 *
 * Estrategia (rapida, qualidade boa pra arte de VN):
 *   - flip escolhido pela maior separacao de media entre as metades
 *   - base = media do subbloco (diferencial 555+333 quando cabe, senao 444)
 *   - busca completa de tabela (8) x modificador (4) por pixel, erro RGB
 */

#include "etc1.h"

#include <string.h>

static const int etc1_mod[8][2] = {
    {2, 8}, {5, 17}, {9, 29}, {13, 42},
    {18, 60}, {24, 80}, {33, 106}, {47, 183},
};

static inline int clampi(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static inline int sq(int v) { return v * v; }

/* expande quantizado pra 8 bits igual o decoder faz */
static inline int expand4(int v) { return (v << 4) | v; }
static inline int expand5(int v) { return (v << 3) | (v >> 2); }

size_t ss_etc1_size(int w, int h) {
  size_t bw = (size_t)((w + 3) / 4);
  size_t bh = (size_t)((h + 3) / 4);
  return bw * bh * 8u;
}

/* Um subbloco = 8 pixels (RGB ja extraidos). Escolhe tabela e indices que
 * minimizam o erro contra a base expandida (er,eg,eb). Retorna erro total;
 * indices em idx[8] (0..3 na convencao msb/lsb acima). */
static unsigned subblock_fit(const uint8_t px[8][3], int er, int eg, int eb,
                             int *table_out, uint8_t idx[8]) {
  unsigned best_err = ~0u;
  int best_table = 0;
  uint8_t best_idx[8] = {0};

  for (int t = 0; t < 8; t++) {
    unsigned err = 0;
    uint8_t cur[8];
    for (int i = 0; i < 8; i++) {
      unsigned pbest = ~0u;
      uint8_t pidx = 0;
      for (int m = 0; m < 4; m++) {
        /* m: 0=+peq 1=+grande 2=-peq 3=-grande */
        int d = etc1_mod[t][m & 1];
        if (m & 2)
          d = -d;
        int r = clampi(er + d, 0, 255);
        int g = clampi(eg + d, 0, 255);
        int b = clampi(eb + d, 0, 255);
        unsigned e = (unsigned)(sq(r - px[i][0]) + sq(g - px[i][1]) +
                                sq(b - px[i][2]));
        if (e < pbest) {
          pbest = e;
          pidx = (uint8_t)m;
        }
      }
      err += pbest;
      if (err >= best_err)
        break; /* poda */
      cur[i] = pidx;
    }
    if (err < best_err) {
      best_err = err;
      best_table = t;
      memcpy(best_idx, cur, 8);
    }
  }

  *table_out = best_table;
  memcpy(idx, best_idx, 8);
  return best_err;
}

/* junta um bloco 4x4 a partir dos pixels (row-major rgb[16][3]) */
static void encode_block(const uint8_t rgb[16][3], uint8_t out[8]) {
  /* medias das metades nas duas orientacoes pra escolher o flip */
  long sum_l[3] = {0, 0, 0}, sum_r[3] = {0, 0, 0};
  long sum_t[3] = {0, 0, 0}, sum_b[3] = {0, 0, 0};
  for (int y = 0; y < 4; y++) {
    for (int x = 0; x < 4; x++) {
      const uint8_t *p = rgb[y * 4 + x];
      long *hx = (x < 2) ? sum_l : sum_r;
      long *hy = (y < 2) ? sum_t : sum_b;
      for (int c = 0; c < 3; c++) {
        hx[c] += p[c];
        hy[c] += p[c];
      }
    }
  }
  long dx = 0, dy = 0;
  for (int c = 0; c < 3; c++) {
    long a = sum_l[c] - sum_r[c];
    long b = sum_t[c] - sum_b[c];
    dx += a < 0 ? -a : a;
    dy += b < 0 ? -b : b;
  }
  int flip = (dy > dx) ? 1 : 0;

  /* pixels de cada subbloco, na ordem texel (column-major dentro do 4x4) */
  uint8_t sb[2][8][3];
  int n0 = 0, n1 = 0;
  long avg[2][3] = {{0, 0, 0}, {0, 0, 0}};
  for (int x = 0; x < 4; x++) {
    for (int y = 0; y < 4; y++) {
      const uint8_t *p = rgb[y * 4 + x];
      int half = flip ? (y >= 2) : (x >= 2);
      int *n = half ? &n1 : &n0;
      memcpy(sb[half][*n], p, 3);
      for (int c = 0; c < 3; c++)
        avg[half][c] += p[c];
      (*n)++;
    }
  }
  (void)n0;
  (void)n1;

  int a0[3], a1[3];
  for (int c = 0; c < 3; c++) {
    a0[c] = (int)((avg[0][c] + 4) / 8);
    a1[c] = (int)((avg[1][c] + 4) / 8);
  }

  /* tenta diferencial 555+333 */
  int q0[3], q1[3], diff = 1;
  for (int c = 0; c < 3; c++) {
    q0[c] = clampi((a0[c] * 31 + 127) / 255, 0, 31);
    q1[c] = clampi((a1[c] * 31 + 127) / 255, 0, 31);
    int d = q1[c] - q0[c];
    if (d < -4 || d > 3)
      diff = 0;
  }

  int e0[3], e1[3];
  if (diff) {
    for (int c = 0; c < 3; c++) {
      e0[c] = expand5(q0[c]);
      e1[c] = expand5(q1[c]);
    }
  } else {
    for (int c = 0; c < 3; c++) {
      q0[c] = clampi((a0[c] * 15 + 127) / 255, 0, 15);
      q1[c] = clampi((a1[c] * 15 + 127) / 255, 0, 15);
      e0[c] = expand4(q0[c]);
      e1[c] = expand4(q1[c]);
    }
  }

  int t0, t1;
  uint8_t idx0[8], idx1[8];
  subblock_fit((const uint8_t(*)[3])sb[0], e0[0], e0[1], e0[2], &t0, idx0);
  subblock_fit((const uint8_t(*)[3])sb[1], e1[0], e1[1], e1[2], &t1, idx1);

  /* monta os 64 bits */
  if (diff) {
    out[0] = (uint8_t)((q0[0] << 3) | ((q1[0] - q0[0]) & 7));
    out[1] = (uint8_t)((q0[1] << 3) | ((q1[1] - q0[1]) & 7));
    out[2] = (uint8_t)((q0[2] << 3) | ((q1[2] - q0[2]) & 7));
  } else {
    out[0] = (uint8_t)((q0[0] << 4) | q1[0]);
    out[1] = (uint8_t)((q0[1] << 4) | q1[1]);
    out[2] = (uint8_t)((q0[2] << 4) | q1[2]);
  }
  out[3] = (uint8_t)((t0 << 5) | (t1 << 2) | (diff << 1) | flip);

  /* planos de indice: texel i = x*4+y; subbloco 0 = texels 0..7 (flip=0)
   * ou texels com y<2 (flip=1) */
  uint32_t msb = 0, lsb = 0;
  int c0 = 0, c1 = 0;
  for (int x = 0; x < 4; x++) {
    for (int y = 0; y < 4; y++) {
      int i = x * 4 + y;
      int half = flip ? (y >= 2) : (x >= 2);
      uint8_t v = half ? idx1[c1++] : idx0[c0++];
      if (v & 2)
        msb |= 1u << i;
      if (v & 1)
        lsb |= 1u << i;
    }
  }
  out[4] = (uint8_t)(msb >> 8);
  out[5] = (uint8_t)(msb & 0xff);
  out[6] = (uint8_t)(lsb >> 8);
  out[7] = (uint8_t)(lsb & 0xff);
}

/* extrai um bloco 4x4 do buffer (borda replicada quando w/h nao multiplo
 * de 4), channel_alpha=1 usa (A,A,A) */
static void fetch_block(const uint8_t *rgba, int w, int h, size_t stride,
                        int bx, int by, int channel_alpha,
                        uint8_t rgb[16][3]) {
  for (int y = 0; y < 4; y++) {
    int sy = by * 4 + y;
    if (sy >= h)
      sy = h - 1;
    const uint8_t *row = rgba + (size_t)sy * stride;
    for (int x = 0; x < 4; x++) {
      int sx = bx * 4 + x;
      if (sx >= w)
        sx = w - 1;
      const uint8_t *p = row + (size_t)sx * 4u;
      uint8_t *d = rgb[y * 4 + x];
      if (channel_alpha) {
        d[0] = d[1] = d[2] = p[3];
      } else {
        d[0] = p[0];
        d[1] = p[1];
        d[2] = p[2];
      }
    }
  }
}

static void encode_all(const uint8_t *rgba, int w, int h, size_t stride,
                       int channel_alpha, uint8_t *out) {
  int bw = (w + 3) / 4;
  int bh = (h + 3) / 4;
  uint8_t rgb[16][3];
  for (int by = 0; by < bh; by++) {
    for (int bx = 0; bx < bw; bx++) {
      fetch_block(rgba, w, h, stride, bx, by, channel_alpha, rgb);
      encode_block((const uint8_t(*)[3])rgb, out);
      out += 8;
    }
  }
}

void ss_etc1_encode_rgba(const uint8_t *rgba, int w, int h, size_t stride,
                         uint8_t *out) {
  encode_all(rgba, w, h, stride, 0, out);
}

void ss_etc1_encode_alpha(const uint8_t *rgba, int w, int h, size_t stride,
                          uint8_t *out) {
  encode_all(rgba, w, h, stride, 1, out);
}
