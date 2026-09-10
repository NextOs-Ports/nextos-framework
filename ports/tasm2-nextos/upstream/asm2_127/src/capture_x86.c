#include "capture_x86.h"

#if defined(__i386__)

#include <errno.h>
#include <GLES2/gl2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "x86_runtime_compat.h"

#define ASM2_CAPTURE_PATH_MAX 4096
#define ASM2_CAPTURE_MAX_PER_PROCESS 32u
#define ASM2_CAPTURE_MAX_PIXELS ((size_t)4096u * (size_t)4096u)

struct asm2_sha256 {
  uint32_t state[8];
  uint64_t bit_count;
  unsigned char block[64];
  size_t block_size;
};

struct asm2_capture_config {
  int initialized;
  int enabled;
  unsigned int completed;
  char request[ASM2_CAPTURE_PATH_MAX];
  char output[ASM2_CAPTURE_PATH_MAX];
};

typedef void (*asm2_gl_get_integerv_fn)(GLenum name, GLint *value);
typedef void (*asm2_gl_pixel_store_i_fn)(GLenum name, GLint value);
typedef void (*asm2_gl_bind_framebuffer_fn)(GLenum target,
                                             GLuint framebuffer);
typedef void (*asm2_gl_read_pixels_fn)(GLint x, GLint y, GLsizei width,
                                        GLsizei height, GLenum format,
                                        GLenum type, void *pixels);

static const uint32_t asm2_sha256_round_constants[64] = {
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
    UINT32_C(0xc67178f2),
};

static struct asm2_capture_config asm2_capture;

static uint32_t asm2_rotate_right(uint32_t value, unsigned int count) {
  return (value >> count) | (value << (32u - count));
}

static void asm2_sha256_transform(struct asm2_sha256 *sha,
                                  const unsigned char block[64]) {
  uint32_t words[64];
  for (size_t index = 0; index < 16; ++index) {
    const size_t offset = index * 4u;
    words[index] = (uint32_t)block[offset] << 24 |
                   (uint32_t)block[offset + 1u] << 16 |
                   (uint32_t)block[offset + 2u] << 8 |
                   (uint32_t)block[offset + 3u];
  }
  for (size_t index = 16; index < 64; ++index) {
    const uint32_t left = words[index - 15u];
    const uint32_t right = words[index - 2u];
    const uint32_t sigma0 =
        asm2_rotate_right(left, 7) ^ asm2_rotate_right(left, 18) ^ (left >> 3);
    const uint32_t sigma1 = asm2_rotate_right(right, 17) ^
                            asm2_rotate_right(right, 19) ^ (right >> 10);
    words[index] =
        words[index - 16u] + sigma0 + words[index - 7u] + sigma1;
  }

  uint32_t a = sha->state[0];
  uint32_t b = sha->state[1];
  uint32_t c = sha->state[2];
  uint32_t d = sha->state[3];
  uint32_t e = sha->state[4];
  uint32_t f = sha->state[5];
  uint32_t g = sha->state[6];
  uint32_t h = sha->state[7];
  for (size_t index = 0; index < 64; ++index) {
    const uint32_t sum1 =
        asm2_rotate_right(e, 6) ^ asm2_rotate_right(e, 11) ^
        asm2_rotate_right(e, 25);
    const uint32_t choice = (e & f) ^ (~e & g);
    const uint32_t temporary1 =
        h + sum1 + choice + asm2_sha256_round_constants[index] + words[index];
    const uint32_t sum0 =
        asm2_rotate_right(a, 2) ^ asm2_rotate_right(a, 13) ^
        asm2_rotate_right(a, 22);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temporary2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }
  sha->state[0] += a;
  sha->state[1] += b;
  sha->state[2] += c;
  sha->state[3] += d;
  sha->state[4] += e;
  sha->state[5] += f;
  sha->state[6] += g;
  sha->state[7] += h;
}

static void asm2_sha256_init(struct asm2_sha256 *sha) {
  *sha = (struct asm2_sha256){
      .state = {
          UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85),
          UINT32_C(0x3c6ef372), UINT32_C(0xa54ff53a),
          UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
          UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19),
      },
  };
}

static void asm2_sha256_update(struct asm2_sha256 *sha, const void *data,
                               size_t size) {
  const unsigned char *input = data;
  sha->bit_count += (uint64_t)size * 8u;
  while (size > 0) {
    size_t available = sizeof(sha->block) - sha->block_size;
    size_t amount = size < available ? size : available;
    memcpy(sha->block + sha->block_size, input, amount);
    sha->block_size += amount;
    input += amount;
    size -= amount;
    if (sha->block_size == sizeof(sha->block)) {
      asm2_sha256_transform(sha, sha->block);
      sha->block_size = 0;
    }
  }
}

static void asm2_sha256_final(struct asm2_sha256 *sha,
                              unsigned char digest[32]) {
  size_t position = sha->block_size;
  sha->block[position++] = 0x80;
  if (position > 56u) {
    memset(sha->block + position, 0, sizeof(sha->block) - position);
    asm2_sha256_transform(sha, sha->block);
    position = 0;
  }
  memset(sha->block + position, 0, 56u - position);
  for (size_t index = 0; index < 8; ++index)
    sha->block[56u + index] =
        (unsigned char)(sha->bit_count >> (56u - index * 8u));
  asm2_sha256_transform(sha, sha->block);

  for (size_t index = 0; index < 8; ++index) {
    digest[index * 4u] = (unsigned char)(sha->state[index] >> 24);
    digest[index * 4u + 1u] = (unsigned char)(sha->state[index] >> 16);
    digest[index * 4u + 2u] = (unsigned char)(sha->state[index] >> 8);
    digest[index * 4u + 3u] = (unsigned char)sha->state[index];
  }
}

static void asm2_capture_initialize(void) {
  if (asm2_capture.initialized)
    return;
  asm2_capture.initialized = 1;

  const char *request = getenv("ASM2_CAPTURE_REQUEST");
  const char *output = getenv("ASM2_CAPTURE_OUTPUT");
  if (!request || !request[0] || !output || !output[0])
    return;
  const size_t request_length = strlen(request);
  const size_t output_length = strlen(output);
  if (request_length >= sizeof(asm2_capture.request) ||
      output_length >= sizeof(asm2_capture.output) ||
      strcmp(request, output) == 0) {
    debugPrintf("ASM2_CAPTURE_CONFIG_ERROR invalid request/output path\n");
    return;
  }
  memcpy(asm2_capture.request, request, request_length + 1u);
  memcpy(asm2_capture.output, output, output_length + 1u);
  asm2_capture.enabled = 1;
  debugPrintf("ASM2_CAPTURE_ARMED request=%s output=%s\n",
              asm2_capture.request, asm2_capture.output);
}

static int asm2_capture_write_ppm(const unsigned char *rgba, int width,
                                  int height, char digest_hex[65],
                                  size_t *written_size) {
  char temporary[ASM2_CAPTURE_PATH_MAX];
  const int temporary_length =
      snprintf(temporary, sizeof(temporary), "%s.part.%ld",
               asm2_capture.output, (long)getpid());
  if (temporary_length < 0 ||
      (size_t)temporary_length >= sizeof(temporary)) {
    errno = ENAMETOOLONG;
    return -1;
  }

  FILE *output = fopen(temporary, "wb");
  if (!output)
    return -1;

  int result = -1;
  unsigned char *row = NULL;
  struct asm2_sha256 sha;
  asm2_sha256_init(&sha);
  char header[64];
  const int header_length =
      snprintf(header, sizeof(header), "P6\n%d %d\n255\n", width, height);
  if (header_length < 0 || (size_t)header_length >= sizeof(header)) {
    errno = EOVERFLOW;
    goto finished;
  }
  if (fwrite(header, 1, (size_t)header_length, output) !=
      (size_t)header_length)
    goto finished;
  asm2_sha256_update(&sha, header, (size_t)header_length);

  const size_t row_size = (size_t)width * 3u;
  row = malloc(row_size);
  if (!row)
    goto finished;
  for (int output_y = 0; output_y < height; ++output_y) {
    const int source_y = height - output_y - 1;
    const unsigned char *source =
        rgba + (size_t)source_y * (size_t)width * 4u;
    for (int x = 0; x < width; ++x) {
      row[(size_t)x * 3u] = source[(size_t)x * 4u];
      row[(size_t)x * 3u + 1u] = source[(size_t)x * 4u + 1u];
      row[(size_t)x * 3u + 2u] = source[(size_t)x * 4u + 2u];
    }
    if (fwrite(row, 1, row_size, output) != row_size)
      goto finished;
    asm2_sha256_update(&sha, row, row_size);
  }
  if (fflush(output) != 0 || fsync(fileno(output)) != 0)
    goto finished;
  if (fclose(output) != 0) {
    output = NULL;
    goto finished;
  }
  output = NULL;
  if (rename(temporary, asm2_capture.output) != 0)
    goto finished;

  unsigned char digest[32];
  asm2_sha256_final(&sha, digest);
  for (size_t index = 0; index < sizeof(digest); ++index)
    snprintf(digest_hex + index * 2u, 3, "%02x", digest[index]);
  digest_hex[64] = '\0';
  *written_size =
      (size_t)header_length + (size_t)height * (size_t)width * 3u;
  result = 0;

finished:
  {
    const int saved_errno = errno;
    free(row);
    if (output)
      fclose(output);
    if (result != 0)
      unlink(temporary);
    errno = saved_errno;
  }
  return result;
}

void asm2_x86_capture_backbuffer_if_requested(int width, int height) {
  asm2_capture_initialize();
  if (!asm2_capture.enabled ||
      access(asm2_capture.request, F_OK) != 0)
    return;
  if (asm2_capture.completed >= ASM2_CAPTURE_MAX_PER_PROCESS) {
    debugPrintf("ASM2_CAPTURE_ERROR stage=limit maximum=%u\n",
                ASM2_CAPTURE_MAX_PER_PROCESS);
    unlink(asm2_capture.request);
    asm2_capture.enabled = 0;
    return;
  }

  if (width <= 0 || height <= 0 ||
      (size_t)width > SIZE_MAX / (size_t)height) {
    debugPrintf("ASM2_CAPTURE_ERROR stage=dimensions width=%d height=%d\n",
                width, height);
    unlink(asm2_capture.request);
    return;
  }
  const size_t pixel_count = (size_t)width * (size_t)height;
  if (pixel_count > SIZE_MAX / 4u ||
      pixel_count > ASM2_CAPTURE_MAX_PIXELS) {
    debugPrintf("ASM2_CAPTURE_ERROR stage=dimensions width=%d height=%d "
                "pixels=%zu maximum=%zu\n",
                width, height, pixel_count, ASM2_CAPTURE_MAX_PIXELS);
    unlink(asm2_capture.request);
    return;
  }

  asm2_gl_get_integerv_fn get_integerv =
      (asm2_gl_get_integerv_fn)asm2_x86_gl_resolve_now("glGetIntegerv");
  asm2_gl_pixel_store_i_fn pixel_store_i =
      (asm2_gl_pixel_store_i_fn)asm2_x86_gl_resolve_now("glPixelStorei");
  asm2_gl_bind_framebuffer_fn bind_framebuffer =
      (asm2_gl_bind_framebuffer_fn)asm2_x86_gl_resolve_now(
          "glBindFramebuffer");
  asm2_gl_read_pixels_fn read_pixels =
      (asm2_gl_read_pixels_fn)asm2_x86_gl_resolve_now("glReadPixels");
  if (!get_integerv || !pixel_store_i || !bind_framebuffer || !read_pixels) {
    debugPrintf("ASM2_CAPTURE_ERROR stage=resolve get=%p pack=%p bind=%p "
                "read=%p\n",
                (void *)get_integerv, (void *)pixel_store_i,
                (void *)bind_framebuffer, (void *)read_pixels);
    unlink(asm2_capture.request);
    return;
  }

  unsigned char *rgba = malloc(pixel_count * 4u);
  if (!rgba) {
    debugPrintf("ASM2_CAPTURE_ERROR stage=allocate bytes=%zu error=%s\n",
                pixel_count * 4u, strerror(errno));
    unlink(asm2_capture.request);
    return;
  }

  GLint old_framebuffer = 0;
  GLint old_pack_alignment = 4;
  get_integerv(GL_FRAMEBUFFER_BINDING, &old_framebuffer);
  get_integerv(GL_PACK_ALIGNMENT, &old_pack_alignment);
  bind_framebuffer(GL_FRAMEBUFFER, 0);
  pixel_store_i(GL_PACK_ALIGNMENT, 1);
  read_pixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  pixel_store_i(GL_PACK_ALIGNMENT, old_pack_alignment);
  bind_framebuffer(GL_FRAMEBUFFER, (GLuint)old_framebuffer);

  char digest_hex[65];
  size_t written_size = 0;
  const int write_result =
      asm2_capture_write_ppm(rgba, width, height, digest_hex, &written_size);
  const int saved_errno = errno;
  free(rgba);
  const int consume_result = unlink(asm2_capture.request);
  if (write_result != 0) {
    debugPrintf("ASM2_CAPTURE_ERROR stage=write output=%s error=%s "
                "trigger_consumed=%d\n",
                asm2_capture.output, strerror(saved_errno),
                consume_result == 0 || errno == ENOENT);
    return;
  }
  ++asm2_capture.completed;
  debugPrintf("ASM2_CAPTURE_OK output=%s width=%d height=%d bytes=%zu "
              "sha256=%s framebuffer_before=%d trigger_consumed=%d "
              "capture=%u/%u\n",
              asm2_capture.output, width, height, written_size, digest_hex,
              old_framebuffer, consume_result == 0 || errno == ENOENT,
              asm2_capture.completed, ASM2_CAPTURE_MAX_PER_PROCESS);
}

#endif
