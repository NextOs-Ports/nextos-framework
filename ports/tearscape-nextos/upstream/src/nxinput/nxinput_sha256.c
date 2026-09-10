/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxinput_sha256.h"
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(uint32_t s[8], const uint8_t p[64]) {
  uint32_t w[64], a, b, c, d, e, f, g, h;
  int i;
  for (i = 0; i < 16; i++) {
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  }
  for (i = 16; i < 64; i++) {
    uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = s[0]; b = s[1]; c = s[2]; d = s[3];
  e = s[4]; f = s[5]; g = s[6]; h = s[7];
  for (i = 0; i < 64; i++) {
    uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + K[i] + w[i];
    uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  s[0] += a; s[1] += b; s[2] += c; s[3] += d;
  s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}

void nxinput_sha256_init(nxinput_sha256 *ctx) {
  static const uint32_t init[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                   0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                   0x1f83d9abu, 0x5be0cd19u};
  memcpy(ctx->state, init, sizeof init);
  ctx->length = 0u;
  ctx->buffered = 0u;
}

void nxinput_sha256_update(nxinput_sha256 *ctx, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  ctx->length += (uint64_t)len;
  while (len > 0u) {
    size_t take = 64u - ctx->buffered;
    if (take > len) {
      take = len;
    }
    memcpy(ctx->buffer + ctx->buffered, p, take);
    ctx->buffered += take;
    p += take;
    len -= take;
    if (ctx->buffered == 64u) {
      block(ctx->state, ctx->buffer);
      ctx->buffered = 0u;
    }
  }
}

void nxinput_sha256_final(nxinput_sha256 *ctx, uint8_t digest[32]) {
  uint64_t bits = ctx->length * 8u;
  uint8_t pad = 0x80u;
  uint8_t zero = 0u;
  uint8_t lenbuf[8];
  int i;
  nxinput_sha256_update(ctx, &pad, 1u);
  while (ctx->buffered != 56u) {
    nxinput_sha256_update(ctx, &zero, 1u);
  }
  for (i = 0; i < 8; i++) {
    lenbuf[i] = (uint8_t)(bits >> (56 - 8 * i));
  }
  nxinput_sha256_update(ctx, lenbuf, 8u);
  for (i = 0; i < 8; i++) {
    digest[i * 4] = (uint8_t)(ctx->state[i] >> 24);
    digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
    digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
    digest[i * 4 + 3] = (uint8_t)ctx->state[i];
  }
}

void nxinput_sha256_hex(const uint8_t digest[32], char *hex) {
  static const char *d = "0123456789abcdef";
  int i;
  for (i = 0; i < 32; i++) {
    hex[i * 2] = d[digest[i] >> 4];
    hex[i * 2 + 1] = d[digest[i] & 0xfu];
  }
  hex[64] = '\0';
}
