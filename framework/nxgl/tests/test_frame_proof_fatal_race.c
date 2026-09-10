/* SPDX-License-Identifier: GPL-3.0-only */
/* nxgl 0.3.5 harness: the fatal flag must be readable from the frame-loop
 * thread WHILE the render thread is inside a legitimate readback holding the
 * adapter guard. On 0.3.4 nxgl_frame_proof_is_fatal() failed closed (returned
 * 1) whenever the guard was busy: a healthy Unity game (Nameless Cat 1.2.7,
 * dArkOS, frame 32, frame proof 100% non-black) closed with status 72.
 *
 * Phase A: one thread samples big non-black frames back to back; a second
 * thread spins on is_fatal(). Any 1 is the defect. Phase B: three conclusive
 * black before-present samples must still turn the flag on and consume once. */
#include "../adapters/nxgl_frame_proof_adapter.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIG_W 2048
#define BIG_H 1024
#define GL_VIEWPORT 0x0BA2u
#define GL_RENDERER 0x1F01u
#define GL_VERSION 0x1F02u
#define GL_EXTENSIONS 0x1F03u
#define GL_PACK_ALIGNMENT 0x0D05u

static volatile unsigned char g_fill = 0xC0;
static volatile int g_sampling_done;
static volatile unsigned long g_reads;

static void fake_read_pixels(int x, int y, int width, int height,
                             unsigned format, unsigned type, void *pixels) {
  unsigned char *out = pixels;
  size_t i, count = (size_t)width * (size_t)height;
  unsigned char fill = g_fill;
  (void)x; (void)y; (void)format; (void)type;
  for (i = 0; i < count; i++) {
    out[i * 4 + 0] = fill;
    out[i * 4 + 1] = fill;
    out[i * 4 + 2] = fill;
    out[i * 4 + 3] = 255;
  }
  g_reads++;
}

static void fake_get_integerv(unsigned pname, int *params) {
  if (pname == GL_VIEWPORT) {
    params[0] = 0; params[1] = 0; params[2] = BIG_W; params[3] = BIG_H;
  } else if (pname == GL_PACK_ALIGNMENT) {
    *params = 4;
  } else {
    *params = 0;
  }
}

static unsigned fake_get_error(void) { return 0; }

static const unsigned char *fake_get_string(unsigned name) {
  if (name == GL_RENDERER) return (const unsigned char *)"Fake Healthy GPU";
  if (name == GL_VERSION) return (const unsigned char *)"OpenGL ES 2.0 fake";
  if (name == GL_EXTENSIONS) return (const unsigned char *)"";
  return NULL;
}

static void fake_noop(void) {}

static void *fake_resolver(const char *name) {
  if (strcmp(name, "glReadPixels") == 0) return (void *)fake_read_pixels;
  if (strcmp(name, "glGetIntegerv") == 0) return (void *)fake_get_integerv;
  if (strcmp(name, "glGetError") == 0) return (void *)fake_get_error;
  if (strcmp(name, "glGetString") == 0) return (void *)fake_get_string;
  return (void *)fake_noop;
}

static void *render_thread(void *arg) {
  int i;
  (void)arg;
  for (i = 0; i < 40; i++)
    nxgl_frame_proof_sample_at(BIG_W, BIG_H, NXGL_PROOF_BEFORE_PRESENT);
  g_sampling_done = 1;
  return NULL;
}

static void *frame_loop_thread(void *arg) {
  unsigned long *false_fatals = arg;
  unsigned long polls = 0;
  while (!g_sampling_done) {
    if (nxgl_frame_proof_is_fatal())
      (*false_fatals)++;
    polls++;
  }
  printf("race: polls=%lu false_fatals=%lu readbacks=%lu\n", polls,
         *false_fatals, g_reads);
  return NULL;
}

int main(void) {
  pthread_t render, loop;
  unsigned long false_fatals = 0;
  int fails = 0;

  nxgl_frame_proof_set_resolver(fake_resolver);
  nxgl_frame_proof_launch_receipt();
  nxgl_frame_proof_set_video_context(BIG_W, BIG_H, "KMSDRM",
                                     "Fake Healthy GPU", "OpenGL ES 2.0 fake");

  /* Phase A: healthy frames under contention. */
  if (pthread_create(&render, NULL, render_thread, NULL) != 0 ||
      pthread_create(&loop, NULL, frame_loop_thread, &false_fatals) != 0) {
    fprintf(stderr, "pthread_create failed\n");
    return 2;
  }
  pthread_join(render, NULL);
  pthread_join(loop, NULL);
  if (g_reads < 40) {
    fprintf(stderr, "FAIL: only %lu readbacks happened (expected 40 samples)\n", g_reads);
    fails++;
  }
  if (false_fatals != 0) {
    fprintf(stderr, "FAIL: is_fatal() reported %lu fatal(s) on a healthy, contended adapter\n", false_fatals);
    fails++;
  }
  if (nxgl_frame_proof_is_fatal()) {
    fprintf(stderr, "FAIL: fatal after healthy frames\n");
    fails++;
  }

  /* Phase B: the real fatal still arms and is consumed exactly once. The
   * before-present schedule samples frames 30, 120 and 600: three conclusive
   * black samples arm the flag on the 600th present, never earlier. */
  g_fill = 0x00;
  {
    unsigned long frame;
    for (frame = 1; frame <= 599; frame++) {
      nxgl_frame_proof_before_present(BIG_W, BIG_H);
      if (nxgl_frame_proof_is_fatal()) {
        fprintf(stderr, "FAIL: fatal armed at frame %lu, before the third black sample\n", frame);
        fails++;
        break;
      }
    }
    nxgl_frame_proof_before_present(BIG_W, BIG_H);
  }
  if (!nxgl_frame_proof_is_fatal()) {
    fprintf(stderr, "FAIL: three conclusive black before-present samples did not arm the fatal\n");
    fails++;
  }
  if (nxgl_frame_proof_consume_fatal() != 1 || nxgl_frame_proof_consume_fatal() != 0) {
    fprintf(stderr, "FAIL: fatal close must be consumed exactly once\n");
    fails++;
  }
  if (!nxgl_frame_proof_is_fatal()) {
    fprintf(stderr, "FAIL: fatal must stay armed after consumption\n");
    fails++;
  }
  nxgl_frame_proof_publish();
  if (fails) {
    fprintf(stderr, "nxgl_frame_proof_fatal_race=FAIL (%d)\n", fails);
    return 1;
  }
  printf("nxgl_frame_proof_fatal_race=PASS lock-free is_fatal under contention; conclusive fatal still arms once\n");
  return 0;
}
