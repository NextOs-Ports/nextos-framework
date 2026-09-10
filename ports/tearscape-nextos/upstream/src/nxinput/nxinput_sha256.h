/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_SHA256_H
#define NXINPUT_SHA256_H
/* Minimal SHA-256 (FIPS 180-4) for provider/fixture identity. Pure C99. */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct nxinput_sha256 {
  uint32_t state[8];
  uint64_t length;
  uint8_t buffer[64];
  size_t buffered;
} nxinput_sha256;
void nxinput_sha256_init(nxinput_sha256 *ctx);
void nxinput_sha256_update(nxinput_sha256 *ctx, const void *data, size_t len);
void nxinput_sha256_final(nxinput_sha256 *ctx, uint8_t digest[32]);
/* hex must hold 65 bytes. */
void nxinput_sha256_hex(const uint8_t digest[32], char *hex);
#ifdef __cplusplus
}
#endif
#endif
