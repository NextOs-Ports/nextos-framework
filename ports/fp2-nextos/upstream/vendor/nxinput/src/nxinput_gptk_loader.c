/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L

#include "nxinput_gptk_loader.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GPTK_BASENAME "NEXTOSCONTROLLERS.gptk"

typedef struct gptk_sha256 {
  uint32_t state[8];
  uint64_t bit_count;
  unsigned char block[64];
  size_t used;
} gptk_sha256;

static uint32_t sha_rotr(uint32_t value, unsigned int count) {
  return (value >> count) | (value << (32u - count));
}

static uint32_t sha_load_be32(const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void sha_store_be32(unsigned char *p, uint32_t value) {
  p[0] = (unsigned char)(value >> 24);
  p[1] = (unsigned char)(value >> 16);
  p[2] = (unsigned char)(value >> 8);
  p[3] = (unsigned char)value;
}

static void sha_transform(gptk_sha256 *ctx, const unsigned char block[64]) {
  static const uint32_t constants[64] = {
      UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
      UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
      UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
      UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
      UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
      UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
      UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
      UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
      UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
      UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
      UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
      UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
      UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
      UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
      UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
      UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
      UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
      UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
      UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
      UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
      UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
      UINT32_C(0xc67178f2)};
  uint32_t words[64];
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
  uint32_t e;
  uint32_t f;
  uint32_t g;
  uint32_t h;
  size_t i;

  for (i = 0u; i < 16u; i++) {
    words[i] = sha_load_be32(block + i * 4u);
  }
  for (i = 16u; i < 64u; i++) {
    uint32_t s0 = sha_rotr(words[i - 15u], 7u) ^
                  sha_rotr(words[i - 15u], 18u) ^ (words[i - 15u] >> 3u);
    uint32_t s1 = sha_rotr(words[i - 2u], 17u) ^
                  sha_rotr(words[i - 2u], 19u) ^ (words[i - 2u] >> 10u);
    words[i] = words[i - 16u] + s0 + words[i - 7u] + s1;
  }
  a = ctx->state[0];
  b = ctx->state[1];
  c = ctx->state[2];
  d = ctx->state[3];
  e = ctx->state[4];
  f = ctx->state[5];
  g = ctx->state[6];
  h = ctx->state[7];
  for (i = 0u; i < 64u; i++) {
    uint32_t sum1 = sha_rotr(e, 6u) ^ sha_rotr(e, 11u) ^ sha_rotr(e, 25u);
    uint32_t choose = (e & f) ^ ((~e) & g);
    uint32_t temp1 = h + sum1 + choose + constants[i] + words[i];
    uint32_t sum0 = sha_rotr(a, 2u) ^ sha_rotr(a, 13u) ^ sha_rotr(a, 22u);
    uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    uint32_t temp2 = sum0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
  ctx->state[5] += f;
  ctx->state[6] += g;
  ctx->state[7] += h;
}

static void sha_init(gptk_sha256 *ctx) {
  static const uint32_t initial[8] = {
      UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85), UINT32_C(0x3c6ef372),
      UINT32_C(0xa54ff53a), UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
      UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19)};

  memcpy(ctx->state, initial, sizeof initial);
  ctx->bit_count = 0u;
  ctx->used = 0u;
}

static void sha_update(gptk_sha256 *ctx, const unsigned char *data,
                       size_t length) {
  ctx->bit_count += (uint64_t)length * UINT64_C(8);
  while (length > 0u) {
    size_t available = sizeof ctx->block - ctx->used;
    size_t take = length < available ? length : available;

    memcpy(ctx->block + ctx->used, data, take);
    ctx->used += take;
    data += take;
    length -= take;
    if (ctx->used == sizeof ctx->block) {
      sha_transform(ctx, ctx->block);
      ctx->used = 0u;
    }
  }
}

static void sha_final(gptk_sha256 *ctx, unsigned char digest[32]) {
  uint64_t bit_count = ctx->bit_count;
  size_t i;

  ctx->block[ctx->used++] = 0x80u;
  if (ctx->used > 56u) {
    memset(ctx->block + ctx->used, 0, sizeof ctx->block - ctx->used);
    sha_transform(ctx, ctx->block);
    ctx->used = 0u;
  }
  memset(ctx->block + ctx->used, 0, 56u - ctx->used);
  for (i = 0u; i < 8u; i++) {
    ctx->block[63u - i] = (unsigned char)(bit_count >> (i * 8u));
  }
  sha_transform(ctx, ctx->block);
  for (i = 0u; i < 8u; i++) {
    sha_store_be32(digest + i * 4u, ctx->state[i]);
  }
}

static void sha_hex(const char *data, size_t length,
                    char hex[NXINPUT_GPTK_SHA256_HEX_SIZE]) {
  static const char digits[] = "0123456789abcdef";
  unsigned char digest[32];
  gptk_sha256 ctx;
  size_t i;

  sha_init(&ctx);
  sha_update(&ctx, (const unsigned char *)data, length);
  sha_final(&ctx, digest);
  for (i = 0u; i < sizeof digest; i++) {
    hex[i * 2u] = digits[digest[i] >> 4];
    hex[i * 2u + 1u] = digits[digest[i] & 15u];
  }
  hex[64] = '\0';
}

static int loader_fail(char *error, size_t error_size, int code,
                       const char *reason) {
  if (error != 0 && error_size > 0u) {
    (void)snprintf(error, error_size, "NXI%04d: %s", code, reason);
  }
  return code;
}

static int read_regular_at(int directory_fd, char *buffer, size_t *length,
                           int *present, int *missing, char *hash, char *error,
                           size_t error_size) {
  struct stat directory_status;
  struct stat file_status;
  size_t used = 0u;
  int fd;
  int result = 0;

  *length = 0u;
  *present = 0;
  *missing = 0;
  hash[0] = '\0';
  error[0] = '\0';
  if (directory_fd < 0 || fstat(directory_fd, &directory_status) != 0 ||
      !S_ISDIR(directory_status.st_mode)) {
    return loader_fail(error, error_size, NXINPUT_GPTK_ERR_IO,
                       "mapping directory is not a readable directory");
  }
  fd = openat(directory_fd, GPTK_BASENAME,
              O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    if (errno == ENOENT) {
      *missing = 1;
      return loader_fail(error, error_size, NXINPUT_GPTK_ERR_IO,
                         "mapping file is missing");
    }
    *present = 1;
    return loader_fail(error, error_size, NXINPUT_GPTK_ERR_IO,
                       "mapping file is unsafe or unreadable");
  }
  *present = 1;
  if (fstat(fd, &file_status) != 0 || !S_ISREG(file_status.st_mode)) {
    result = loader_fail(error, error_size, NXINPUT_GPTK_ERR_IO,
                         "mapping file is not regular");
  } else if (file_status.st_size < 0 ||
             (uintmax_t)file_status.st_size >
                 (uintmax_t)NXINPUT_GPTK_MAX_BYTES) {
    result = loader_fail(error, error_size, NXINPUT_GPTK_ERR_TOO_LARGE,
                         "mapping file exceeds 65536 bytes");
  }
  while (result == 0) {
    ssize_t got;

    if (used > (size_t)NXINPUT_GPTK_MAX_BYTES) {
      result = loader_fail(error, error_size, NXINPUT_GPTK_ERR_TOO_LARGE,
                           "mapping file exceeds 65536 bytes");
      break;
    }
    got = read(fd, buffer + used,
               (size_t)NXINPUT_GPTK_MAX_BYTES + 1u - used);
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got < 0) {
      result = loader_fail(error, error_size, NXINPUT_GPTK_ERR_IO,
                           "mapping file read failed");
      break;
    }
    if (got == 0) {
      break;
    }
    used += (size_t)got;
  }
  if (close(fd) != 0 && result == 0) {
    result = loader_fail(error, error_size, NXINPUT_GPTK_ERR_IO,
                         "mapping file close failed");
  }
  if (result == 0 && used > (size_t)NXINPUT_GPTK_MAX_BYTES) {
    result = loader_fail(error, error_size, NXINPUT_GPTK_ERR_TOO_LARGE,
                         "mapping file exceeds 65536 bytes");
  }
  if (result == 0) {
    *length = used;
    sha_hex(buffer, used, hash);
  }
  return result;
}

static int parse_and_validate(const char *buffer, size_t length,
                              const char *const *allowed,
                              size_t allowed_count, nxinput_gptk *map,
                              char *error, size_t error_size) {
  int result = nxinput_gptk_parse(buffer, length, map, error, error_size);

  if (result == 0) {
    result = nxinput_gptk_validate_actions(map, allowed, allowed_count, error,
                                           error_size);
    if (result != 0) {
      memset(map, 0, sizeof *map);
    }
  }
  return result;
}

const char *nxinput_gptk_load_source_name(nxinput_gptk_load_source source) {
  switch (source) {
    case NXINPUT_GPTK_LOAD_OWNER:
      return "owner";
    case NXINPUT_GPTK_LOAD_DEFAULT_OWNER_MISSING:
      return "default_owner_missing";
    case NXINPUT_GPTK_LOAD_DEFAULT_OWNER_REJECTED:
      return "default_owner_rejected";
    case NXINPUT_GPTK_LOAD_NONE:
    default:
      return "none";
  }
}

int nxinput_gptk_load_at(int owner_dir_fd, int defaults_dir_fd,
                         const char *const *allowed, size_t allowed_count,
                         nxinput_gptk *out,
                         nxinput_gptk_load_receipt *receipt) {
  char *buffer;
  nxinput_gptk owner_map;
  size_t length = 0u;
  int present = 0;
  int missing = 0;
  int result;

  if (out == 0 || receipt == 0) {
    if (out != 0) {
      memset(out, 0, sizeof *out);
    }
    return NXINPUT_GPTK_ERR_MALFORMED;
  }
  memset(out, 0, sizeof *out);
  memset(receipt, 0, sizeof *receipt);
  receipt->api_version = NXINPUT_GPTK_LOAD_API_VERSION;
  receipt->source = (uint8_t)NXINPUT_GPTK_LOAD_NONE;
  buffer = (char *)malloc((size_t)NXINPUT_GPTK_MAX_BYTES + 1u);
  if (buffer == 0) {
    result = loader_fail(receipt->default_error,
                         sizeof receipt->default_error,
                         NXINPUT_GPTK_ERR_IO,
                         "bounded mapping buffer allocation failed");
    receipt->default_error_code = result;
    receipt->result_code = result;
    return result;
  }

  result = read_regular_at(defaults_dir_fd, buffer, &length, &present, &missing,
                           receipt->default_sha256, receipt->default_error,
                           sizeof receipt->default_error);
  receipt->default_bytes = length;
  if (result == 0) {
    result = parse_and_validate(buffer, length, allowed, allowed_count,
                                out, receipt->default_error,
                                sizeof receipt->default_error);
  }
  receipt->default_error_code = result;
  if (result != 0) {
    receipt->result_code = result;
    free(buffer);
    return result;
  }

  result = read_regular_at(owner_dir_fd, buffer, &length, &present, &missing,
                           receipt->owner_sha256, receipt->owner_error,
                           sizeof receipt->owner_error);
  receipt->owner_present = present ? 1u : 0u;
  receipt->owner_bytes = length;
  if (result == 0) {
    result = parse_and_validate(buffer, length, allowed, allowed_count,
                                &owner_map, receipt->owner_error,
                                sizeof receipt->owner_error);
  }
  receipt->owner_error_code = result;
  if (result == 0) {
    *out = owner_map;
    receipt->source = (uint8_t)NXINPUT_GPTK_LOAD_OWNER;
    receipt->selected_bytes = receipt->owner_bytes;
    memcpy(receipt->selected_sha256, receipt->owner_sha256,
           sizeof receipt->selected_sha256);
  } else {
    receipt->source =
        (uint8_t)(missing ? NXINPUT_GPTK_LOAD_DEFAULT_OWNER_MISSING
                         : NXINPUT_GPTK_LOAD_DEFAULT_OWNER_REJECTED);
    receipt->selected_bytes = receipt->default_bytes;
    memcpy(receipt->selected_sha256, receipt->default_sha256,
           sizeof receipt->selected_sha256);
  }
  /* C4: the receipt records WHICH format the selected map declared. It is
   * derived from the map that actually won, never from the file that lost. */
  receipt->selected_gptk_schema = out->schema_version;
  receipt->result_code = 0;
  free(buffer);
  return 0;
}

static int json_append(char *json, size_t json_size, size_t *used,
                       const char *text) {
  size_t length = strlen(text);

  if (*used > json_size || length >= json_size - *used) {
    return -1;
  }
  memcpy(json + *used, text, length);
  *used += length;
  json[*used] = '\0';
  return 0;
}

static int json_append_escaped(char *json, size_t json_size, size_t *used,
                               const char *text) {
  static const char hex[] = "0123456789abcdef";

  while (*text != '\0') {
    unsigned char byte = (unsigned char)*text++;
    char escaped[7];
    const char *part = escaped;

    if (byte == '"' || byte == '\\') {
      escaped[0] = '\\';
      escaped[1] = (char)byte;
      escaped[2] = '\0';
    } else if (byte < 0x20u) {
      escaped[0] = '\\';
      escaped[1] = 'u';
      escaped[2] = '0';
      escaped[3] = '0';
      escaped[4] = hex[byte >> 4];
      escaped[5] = hex[byte & 15u];
      escaped[6] = '\0';
    } else {
      escaped[0] = (char)byte;
      escaped[1] = '\0';
    }
    if (json_append(json, json_size, used, part) != 0) {
      return -1;
    }
  }
  return 0;
}

int nxinput_gptk_load_receipt_json(const nxinput_gptk_load_receipt *receipt,
                                   char *json, size_t json_size) {
  char numbers[512];
  size_t used = 0u;
  nxinput_gptk_load_source source;

  if (receipt == 0 || json == 0 || json_size == 0u ||
      receipt->api_version != NXINPUT_GPTK_LOAD_API_VERSION ||
      receipt->source >
          (uint8_t)NXINPUT_GPTK_LOAD_DEFAULT_OWNER_REJECTED) {
    return -1;
  }
  json[0] = '\0';
  source = (nxinput_gptk_load_source)receipt->source;
  (void)snprintf(numbers, sizeof numbers,
                 "{\"schema\":\"%s\",\"source\":\"%s\","
                 "\"result_code\":%d,\"owner_present\":%s,"
                 "\"owner_error_code\":%d,\"default_error_code\":%d,"
                 "\"owner_bytes\":%llu,\"default_bytes\":%llu,"
                 "\"selected_bytes\":%llu,"
                 "\"selected_gptk_schema\":%u,\"owner_sha256\":\"",
                 NXINPUT_GPTK_LOAD_RECEIPT_SCHEMA,
                 nxinput_gptk_load_source_name(source), receipt->result_code,
                 receipt->owner_present ? "true" : "false",
                 receipt->owner_error_code, receipt->default_error_code,
                 (unsigned long long)receipt->owner_bytes,
                 (unsigned long long)receipt->default_bytes,
                 (unsigned long long)receipt->selected_bytes,
                 (unsigned int)receipt->selected_gptk_schema);
  if (json_append(json, json_size, &used, numbers) != 0 ||
      json_append_escaped(json, json_size, &used, receipt->owner_sha256) != 0 ||
      json_append(json, json_size, &used, "\",\"default_sha256\":\"") != 0 ||
      json_append_escaped(json, json_size, &used,
                          receipt->default_sha256) != 0 ||
      json_append(json, json_size, &used, "\",\"selected_sha256\":\"") != 0 ||
      json_append_escaped(json, json_size, &used,
                          receipt->selected_sha256) != 0 ||
      json_append(json, json_size, &used, "\",\"owner_error\":\"") != 0 ||
      json_append_escaped(json, json_size, &used, receipt->owner_error) != 0 ||
      json_append(json, json_size, &used, "\",\"default_error\":\"") != 0 ||
      json_append_escaped(json, json_size, &used,
                          receipt->default_error) != 0 ||
      json_append(json, json_size, &used, "\"}") != 0) {
    if (json_size > 0u) {
      json[0] = '\0';
    }
    return -1;
  }
  return 0;
}
