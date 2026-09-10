/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* RTLD_DEFAULT */
#endif
#include "nxgl_frame_proof_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

/* Deliberately no SDL dependency: the adapter reads the framebuffer the port
 * already made current, so it works the same for an SDL port and a raw-EGL one.
 */
#include <dlfcn.h>

/* GL is resolved at runtime rather than linked. A so-loader port routes every
 * gl* call through its own shim and does not link a GLES library at all, so a
 * direct call here fails to link; a port that does link one still resolves the
 * same symbols through dlsym. The port may install its own resolver when its
 * shim is the only place the real entry points exist. */
#define NXGL_FP_RGBA 0x1908
#define NXGL_FP_UNSIGNED_BYTE 0x1401
#define NXGL_FP_VIEWPORT 0x0BA2

typedef void (*nxgl_fp_read_pixels)(int, int, int, int, unsigned, unsigned,
                                    void *);
typedef void (*nxgl_fp_get_integerv)(unsigned, int *);

static void *(*g_resolver)(const char *);

void nxgl_frame_proof_set_resolver(void *(*resolver)(const char *)) {
  g_resolver = resolver;
}

static void *resolve_gl(const char *name) {
  if (g_resolver) {
    void *found = g_resolver(name);
    if (found)
      return found;
  }
  void *found = dlsym(RTLD_DEFAULT, name);
  if (found)
    return found;
  /* An SDL port resolves GL through SDL_GL_GetProcAddress, and on some images
   * that is the only path to the driver's real entry points -- glReadPixels is
   * not a global symbol on the Mali-450 image. Reach it through dlsym so this
   * adapter still links into a port that does not include SDL headers. */
  void *(*sdl_get_proc)(const char *) =
      (void *(*)(const char *))dlsym(RTLD_DEFAULT, "SDL_GL_GetProcAddress");
  if (sdl_get_proc)
    return sdl_get_proc(name);
  return NULL;
}

/* Mirrors nxgl_classify_frame_proof_v2 / nxgl_classify_launch_context_v2. The
 * policy lives in nxgl and stays pure; this adapter is the half that has to
 * touch GL, so it is vendored into the port rather than linked into nxgl. */
#define FRAME_PROOF_MIN_NON_BLACK 0.5 /* percent of pixels */


/* Write the sampled frame as a PNG next to the log when NXLAUNCH_PROOF_DIR is
 * set. Stored (uncompressed) deflate blocks, so this needs no zlib and never
 * changes the port's link line; the file is a proof artifact, not a photo. */
static uint32_t crc_table[256];
static void crc_init(void) {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++)
      c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
}
static uint32_t crc_update(uint32_t crc, const unsigned char *buf, size_t len) {
  crc ^= 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++)
    crc = crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}
static uint32_t adler32(const unsigned char *buf, size_t len) {
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < len; i++) { a = (a + buf[i]) % 65521u; b = (b + a) % 65521u; }
  return (b << 16) | a;
}
static void be32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
  p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}
static void png_chunk(FILE *f, const char *type, const unsigned char *data,
                      size_t len) {
  unsigned char hdr[8];
  be32(hdr, (uint32_t)len); memcpy(hdr + 4, type, 4);
  fwrite(hdr, 1, 8, f);
  if (len) fwrite(data, 1, len, f);
  uint32_t crc = crc_update(0, (const unsigned char *)type, 4);
  if (len) crc = crc_update(crc, data, len);
  unsigned char c[4]; be32(c, crc); fwrite(c, 1, 4, f);
}
static void write_frame_png(const char *path, int width, int height,
                            const unsigned char *rgba) {
  /* GL rows come bottom-up; PNG wants top-down. Each row: filter byte + RGB. */
  size_t row = 1 + (size_t)width * 3, raw_len = row * (size_t)height;
  unsigned char *raw = malloc(raw_len);
  if (!raw) return;
  for (int y = 0; y < height; y++) {
    unsigned char *dst = raw + (size_t)y * row;
    const unsigned char *src = rgba + (size_t)(height - 1 - y) * (size_t)width * 4;
    dst[0] = 0;
    for (int x = 0; x < width; x++) {
      dst[1 + x * 3] = src[x * 4]; dst[2 + x * 3] = src[x * 4 + 1];
      dst[3 + x * 3] = src[x * 4 + 2];
    }
  }
  /* zlib stream of stored blocks (max 65535 bytes each). */
  size_t blocks = (raw_len + 65534) / 65535;
  size_t zlen = 2 + raw_len + blocks * 5 + 4;
  unsigned char *z = malloc(zlen);
  if (!z) { free(raw); return; }
  size_t o = 0; z[o++] = 0x78; z[o++] = 0x01;
  for (size_t off = 0; off < raw_len; off += 65535) {
    size_t n = raw_len - off; if (n > 65535) n = 65535;
    z[o++] = (off + n == raw_len) ? 1 : 0;
    z[o++] = (unsigned char)(n & 0xFF); z[o++] = (unsigned char)(n >> 8);
    z[o++] = (unsigned char)(~n & 0xFF); z[o++] = (unsigned char)((~n >> 8) & 0xFF);
    memcpy(z + o, raw + off, n); o += n;
  }
  be32(z + o, adler32(raw, raw_len)); o += 4;
  FILE *f = fopen(path, "wb");
  if (f) {
    static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    unsigned char ihdr[13];
    fwrite(sig, 1, 8, f);
    be32(ihdr, (uint32_t)width); be32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    crc_init();
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, o);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    printf("gl: frame proof image written %s (%dx%d)\n", path, width, height);
    fflush(stdout);
  }
  free(z); free(raw);
}

static double g_best_non_black = -1.0;
static int g_samples;
static int g_unmeasured; /* probes that could not read the frame at all */
static int g_published;

/* A launch that could never put an image on the panel cannot be used to accuse
 * the port of drawing nothing. */
static const char *launch_context(int *conclusive) {
  if (getenv("SSH_CONNECTION") || getenv("SSH_TTY") || getenv("SSH_CLIENT")) {
    *conclusive = 0;
    return "remote";
  }
  if (getenv("NXLAUNCH_FRONTEND")) {
    *conclusive = 1;
    return "frontend";
  }
  {
    const char *tty = ttyname(0);
    if (tty && strncmp(tty, "/dev/tty", 8) == 0 && tty[8] >= '0' &&
        tty[8] <= '9') {
      *conclusive = 1;
      return "console";
    }
  }
  *conclusive = 0;
  return "unknown";
}

void nxgl_frame_proof_launch_receipt(void) {
  int conclusive = 0;
  const char *context = launch_context(&conclusive);
  printf("launch: context=%s can-prove-image=%s\n", context,
         conclusive ? "yes" : "no");
  fflush(stdout);
}

void nxgl_frame_proof_sample(int width, int height) {
  /* A port that does not carry its drawable size around can pass 0 and let the
   * current viewport answer: the frame about to be presented is exactly what
   * the viewport covers. */
  if (width <= 0 || height <= 0) {
    nxgl_fp_get_integerv get_integerv =
        (nxgl_fp_get_integerv)resolve_gl("glGetIntegerv");
    int viewport[4] = {0, 0, 0, 0};
    if (get_integerv)
      get_integerv(NXGL_FP_VIEWPORT, viewport);
    width = viewport[2];
    height = viewport[3];
  }
  if (width <= 0 || height <= 0 || width > 32768 || height > 32768) {
    printf("gl: frame probe unavailable (invalid drawable %dx%d)\n", width,
           height);
    fflush(stdout);
    return;
  }

  size_t pixels = (size_t)width * (size_t)height;
  unsigned char *buffer = malloc(pixels * 4);
  if (!buffer) {
    printf("gl: frame probe unavailable (allocation failed)\n");
    fflush(stdout);
    return;
  }

  nxgl_fp_read_pixels read_pixels =
      (nxgl_fp_read_pixels)resolve_gl("glReadPixels");
  if (!read_pixels) {
    g_unmeasured++;
    printf("gl: frame probe unavailable (glReadPixels unresolved)\n");
    fflush(stdout);
    free(buffer);
    return;
  }
  read_pixels(0, 0, width, height, NXGL_FP_RGBA, NXGL_FP_UNSIGNED_BYTE, buffer);

  size_t coloured = 0, opaque = 0, transparent = 0;
  for (size_t i = 0; i < pixels; i++) {
    const unsigned char *p = buffer + i * 4;
    if (p[0] || p[1] || p[2])
      coloured++;
    if (p[3] == 255)
      opaque++;
    else if (p[3] == 0)
      transparent++;
  }
  double non_black = (double)coloured * 100.0 / (double)pixels;
  g_samples++;
  {
    /* Save the second sample as the proof image: the first can still be a
     * legitimately black title card, and later ones add nothing a reader needs.
     * Written only when the harness asks (NXLAUNCH_PROOF_DIR), so an ordinary
     * launch from the frontend leaves no file behind. */
    const char *dir = getenv("NXLAUNCH_PROOF_DIR");
    if (dir && *dir && g_samples == 2) {
      char path[512];
      snprintf(path, sizeof path, "%s/frame-proof.png", dir);
      write_frame_png(path, width, height, buffer);
    }
  }
  free(buffer);
  if (non_black > g_best_non_black)
    g_best_non_black = non_black;

  printf("gl: frame probe %dx%d rgb_non_black=%.1f%% alpha255=%.1f%% "
         "alpha0=%.1f%%\n",
         width, height, non_black, (double)opaque * 100.0 / (double)pixels,
         (double)transparent * 100.0 / (double)pixels);
  fflush(stdout);
}

void nxgl_frame_proof_publish(void) {
  /* Deliberately not one-shot. An automated run is killed rather than closed,
   * and it is killed at an arbitrary moment, so the log has to carry a current
   * verdict at all times instead of one that only appears if the run survives
   * to a specific frame. Readers take the last verdict line. */
  int conclusive = 0;
  const char *context = launch_context(&conclusive);

  if (g_samples <= 0) {
    if (g_published)
      return;
    g_published = 1;
    if (g_unmeasured > 0)
      /* The frames existed and the probe ran; the readback itself failed.
       * That is a harness defect, never evidence about the port. */
      printf("gl: frame proof verdict=UNMEASURED samples=0 attempts=%d "
             "launch=%s (glReadPixels could not be resolved)\n",
             g_unmeasured, context);
    else
      printf("gl: frame proof verdict=UNKNOWN samples=0 launch=%s "
             "(run ended before the first probe)\n",
             context);
    fflush(stdout);
    return;
  }

  int black = g_best_non_black < FRAME_PROOF_MIN_NON_BLACK;
  const char *verdict = !black ? "OK" : (conclusive ? "BLACK" : "INCONCLUSIVE");

  printf("gl: frame proof verdict=%s samples=%d best_non_black=%.1f%% "
         "launch=%s\n",
         verdict, g_samples, g_best_non_black, context);
  if (black && !conclusive)
    printf("gl: this launch cannot prove an image (launch=%s); re-test from "
           "the device frontend before blaming the port\n",
           context);
  printf("NXEVENT {\"schema\":\"nx-event-v1\",\"source\":\"gl\","
         "\"phase\":\"runtime\",\"status\":\"%s\",\"reason_code\":%d,"
         "\"details\":{\"frame_proof\":\"%s\",\"samples\":%d,"
         "\"best_non_black_pct\":%.1f,\"launch_context\":\"%s\","
         "\"conclusive\":%s}}\n",
         (black && conclusive) ? "fail" : "ok",
         !black ? 6300 : (conclusive ? 6301 : 6302),
         !black ? "ok" : (conclusive ? "black" : "inconclusive"), g_samples,
         g_best_non_black, context, conclusive ? "true" : "false");
  fflush(stdout);
}
