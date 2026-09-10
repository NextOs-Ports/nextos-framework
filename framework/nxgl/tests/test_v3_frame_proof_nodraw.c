/* SPDX-License-Identifier: GPL-3.0-only */
/* V3-GRAPHICS-01 harness: a provider that ACCEPTS every call, reports a
 * healthy renderer string, and never draws (readback all-black) must be
 * failed by the frame proof -- the exact provider the discovery receipt alone
 * cannot catch. Also exercises the appended sample_point receipt field. */
#include "../adapters/nxgl_frame_proof_adapter.h"

#include <stdio.h>
#include <string.h>

#define FAKE_W 64
#define FAKE_H 64
#define GL_VIEWPORT 0x0BA2u
#define GL_RENDERER 0x1F01u
#define GL_VERSION 0x1F02u
#define GL_EXTENSIONS 0x1F03u
#define GL_PACK_ALIGNMENT 0x0D05u

static unsigned char g_fill; /* what the fake GPU "drew" (rgb; alpha=255) */

static void fake_read_pixels(int x, int y, int width, int height,
                             unsigned format, unsigned type, void *pixels) {
  unsigned char *out = pixels;
  size_t i, count = (size_t)width * (size_t)height;

  (void)x;
  (void)y;
  (void)format;
  (void)type;
  for (i = 0; i < count; i++) {
    out[i * 4 + 0] = g_fill;
    out[i * 4 + 1] = g_fill;
    out[i * 4 + 2] = g_fill;
    out[i * 4 + 3] = 255; /* opaque: the panel is black, not transparent */
  }
}

static void fake_get_integerv(unsigned pname, int *params) {
  if (pname == GL_VIEWPORT) {
    params[0] = 0;
    params[1] = 0;
    params[2] = FAKE_W;
    params[3] = FAKE_H;
  } else if (pname == GL_PACK_ALIGNMENT) {
    *params = 4;
  } else {
    /* Default draw/read FBO, pack PBO and row/skip state. */
    *params = 0;
  }
}

static unsigned fake_get_error(void) { return 0; }

static const unsigned char *fake_get_string(unsigned name) {
  if (name == GL_RENDERER)
    return (const unsigned char *)"Fake Healthy GPU";
  if (name == GL_VERSION)
    return (const unsigned char *)"OpenGL ES 2.0 fake";
  if (name == GL_EXTENSIONS)
    return (const unsigned char *)"";
  return NULL;
}

/* Accepts the call, does nothing: the shape of the no-draw provider. */
static void fake_noop(void) {}

static void *fake_resolver(const char *name) {
  if (strcmp(name, "glReadPixels") == 0)
    return (void *)fake_read_pixels;
  if (strcmp(name, "glGetIntegerv") == 0)
    return (void *)fake_get_integerv;
  if (strcmp(name, "glGetError") == 0)
    return (void *)fake_get_error;
  if (strcmp(name, "glGetString") == 0)
    return (void *)fake_get_string;
  return (void *)fake_noop;
}

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "nodraw";

  nxgl_frame_proof_set_resolver(fake_resolver);
  nxgl_frame_proof_launch_receipt();
  nxgl_frame_proof_set_video_context(FAKE_W, FAKE_H, "KMSDRM",
                                     "Fake Healthy GPU",
                                     "OpenGL ES 2.0 fake");

  if (strcmp(mode, "nodraw") == 0) {
    /* Healthy strings, calls accepted, nothing ever drawn. */
    g_fill = 0x00;
    nxgl_frame_proof_sample_at(FAKE_W, FAKE_H, NXGL_PROOF_BEFORE_PRESENT);
    nxgl_frame_proof_sample_at(FAKE_W, FAKE_H, NXGL_PROOF_BEFORE_PRESENT);
    nxgl_frame_proof_sample_at(FAKE_W, FAKE_H, NXGL_PROOF_BEFORE_PRESENT);
  } else if (strcmp(mode, "draws") == 0) {
    g_fill = 0xC0;
    nxgl_frame_proof_sample_at(FAKE_W, FAKE_H, NXGL_PROOF_BEFORE_PRESENT);
    nxgl_frame_proof_sample_at(FAKE_W, FAKE_H, NXGL_PROOF_BEFORE_PRESENT);
  } else if (strcmp(mode, "after") == 0) {
    g_fill = 0xC0;
    nxgl_frame_proof_sample_at(FAKE_W, FAKE_H, NXGL_PROOF_AFTER_PRESENT);
  } else if (strcmp(mode, "legacy") == 0) {
    /* Historical entry point: the point is honestly "unspecified". */
    g_fill = 0xC0;
    nxgl_frame_proof_sample(FAKE_W, FAKE_H);
  } else {
    fprintf(stderr, "unknown mode %s\n", mode);
    return 2;
  }

  nxgl_frame_proof_publish();
  return 0;
}
