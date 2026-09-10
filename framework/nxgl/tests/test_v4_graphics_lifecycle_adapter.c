/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-GRAPHICS-04 lifecycle adapter gates against an injected fake SDL/GL.
 *
 * The fake models the Wayland lifecycle honestly: the drawable stays 1x1
 * until the guest's REAL swap runs; only the swap materializes the surface.
 * The spy proves call-through order (real swap strictly before the gate
 * observation), that the nxgl side performs zero clears/draws/swaps of its
 * own, that both SDL2 and SDL3 size symbols work, that the finalization is
 * one-shot under two concurrent presenters, and that a PROVED gate costs no
 * resolver work per frame. The legacy one-shot API is exercised at the end to
 * prove it still refuses a stuck 1x1 exactly as in 0.3.0. */
#include "nxgl_graphics_contract_adapter.h"
#include "nxgl_graphics_present_gate.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, name)                                     \
  do {                                                        \
    if (cond) {                                               \
      printf("ok %s\n", name);                                \
    } else {                                                  \
      printf("FAIL %s (line %d)\n", name, __LINE__);          \
      g_failures++;                                           \
    }                                                         \
  } while (0)

/* ------------------------------------------------------------ fake SDL/GL */

static int fake_sdl_major = 2;      /* which size symbol resolves */
static int fake_swapped;            /* guest presents so far */
static int fake_drawable_w = 1;     /* size before the first commit */
static int fake_drawable_h = 1;
static int fake_real_w = 640;       /* size after the first commit */
static int fake_real_h = 480;
static int count_clear;
static int count_draw;
static int count_swap;
static int count_resolve;           /* every resolver hit */
static int order_swap_seq = -1;     /* sequence stamp of the last real swap */
static int order_observe_seq = -1;  /* sequence stamp of the last gate call */
static int order_seq;

static const unsigned char *fake_glGetString(unsigned name) {
  if (name == 0x1F02u) {
    return (const unsigned char *)"OpenGL ES 3.1 Fake Renderer";
  }
  if (name == 0x1F01u) {
    return (const unsigned char *)"Fake Panfrost";
  }
  if (name == 0x8B8Cu) {
    return (const unsigned char *)"OpenGL ES GLSL ES 3.10";
  }
  return (const unsigned char *)"";
}

static unsigned fake_glCreateShader(unsigned type) { (void)type; return 7u; }
static void fake_glShaderSource(unsigned s, int n, const char *const *src,
                                const int *len) {
  (void)s; (void)n; (void)src; (void)len;
}
static void fake_glCompileShader(unsigned s) { (void)s; }
static void fake_glGetShaderiv(unsigned s, unsigned p, int *out) {
  (void)s; (void)p; *out = 1;
}
static unsigned fake_glCreateProgram(void) { return 8u; }
static void fake_glAttachShader(unsigned p, unsigned s) { (void)p; (void)s; }
static void fake_glLinkProgram(unsigned p) { (void)p; }
static void fake_glGetProgramiv(unsigned p, unsigned q, int *out) {
  (void)p; (void)q; *out = 1;
}
static void fake_glDeleteShader(unsigned s) { (void)s; }
static void fake_glDeleteProgram(unsigned p) { (void)p; }

static void fake_glClear(unsigned mask) { (void)mask; count_clear++; }
static void fake_glDrawArrays(unsigned m, int f, int c) {
  (void)m; (void)f; (void)c; count_draw++;
}

static void *fake_GetCurrentWindow(void) { return (void *)0x77; }

static void fake_current_size(int *w, int *h) {
  if (fake_swapped > 0) {
    *w = fake_real_w;
    *h = fake_real_h;
  } else {
    *w = fake_drawable_w;
    *h = fake_drawable_h;
  }
}

static void fake_sdl2_GetDrawableSize(void *win, int *w, int *h) {
  (void)win;
  fake_current_size(w, h);
}

static _Bool fake_sdl3_GetWindowSizeInPixels(void *win, int *w, int *h) {
  (void)win;
  fake_current_size(w, h);
  return 1;
}

/* The guest's REAL present: the only thing that materializes the drawable. */
static void fake_real_SwapWindow(void *win) {
  (void)win;
  count_swap++;
  fake_swapped++;
  order_swap_seq = ++order_seq;
}

static void *fake_resolver(void *userdata, const char *name) {
  (void)userdata;
  count_resolve++;
  if (strcmp(name, "glGetString") == 0) return (void *)fake_glGetString;
  if (strcmp(name, "glCreateShader") == 0) return (void *)fake_glCreateShader;
  if (strcmp(name, "glShaderSource") == 0) return (void *)fake_glShaderSource;
  if (strcmp(name, "glCompileShader") == 0) return (void *)fake_glCompileShader;
  if (strcmp(name, "glGetShaderiv") == 0) return (void *)fake_glGetShaderiv;
  if (strcmp(name, "glCreateProgram") == 0) return (void *)fake_glCreateProgram;
  if (strcmp(name, "glAttachShader") == 0) return (void *)fake_glAttachShader;
  if (strcmp(name, "glLinkProgram") == 0) return (void *)fake_glLinkProgram;
  if (strcmp(name, "glGetProgramiv") == 0) return (void *)fake_glGetProgramiv;
  if (strcmp(name, "glDeleteShader") == 0) return (void *)fake_glDeleteShader;
  if (strcmp(name, "glDeleteProgram") == 0) return (void *)fake_glDeleteProgram;
  if (strcmp(name, "glClear") == 0) return (void *)fake_glClear;
  if (strcmp(name, "glDrawArrays") == 0) return (void *)fake_glDrawArrays;
  if (strcmp(name, "SDL_GL_GetCurrentWindow") == 0)
    return (void *)fake_GetCurrentWindow;
  if (fake_sdl_major == 2 && strcmp(name, "SDL_GL_GetDrawableSize") == 0)
    return (void *)fake_sdl2_GetDrawableSize;
  if (fake_sdl_major == 3 && strcmp(name, "SDL_GetWindowSizeInPixels") == 0)
    return (void *)fake_sdl3_GetWindowSizeInPixels;
  if (strcmp(name, "SDL_GL_SwapWindow") == 0)
    return (void *)fake_real_SwapWindow;
  /* No EGL in the fake: recorded as "", consistently in both phases. */
  return NULL;
}

static void fake_reset(int sdl_major) {
  fake_sdl_major = sdl_major;
  fake_swapped = 0;
  fake_drawable_w = 1;
  fake_drawable_h = 1;
  count_clear = 0;
  count_draw = 0;
  count_swap = 0;
  count_resolve = 0;
  order_swap_seq = -1;
  order_observe_seq = -1;
  order_seq = 0;
}

static void make_contract(nxgl_graphics_contract *c) {
  (void)nxgl_graphics_contract_default(c);
  c->api = NXGL_GRAPHICS_API_GLES;
  c->profile = NXGL_GRAPHICS_PROFILE_ES;
  c->version_major = 3;
  c->version_minor = 0;
  c->version_policy = NXGL_GRAPHICS_POLICY_MINIMUM;
  c->shader_dialect = NXGL_SHADER_DIALECT_ESSL300;
  c->drawable_ready_timeout_ms = 4000;
}

/* The port wrapper under proof: real present FIRST, observation second. */
static nxgl_graphics_gate_status wrapped_swap(
    const nxgl_graphics_contract *contract,
    nxgl_graphics_present_gate *gate, nxgl_graphics_gate_result *result,
    char *receipt, size_t cap) {
  void (*real_swap)(void *) =
      (void (*)(void *))fake_resolver(NULL, "SDL_GL_SwapWindow");
  nxgl_graphics_gate_status status;
  real_swap((void *)0x77);
  status = nxgl_graphics_contract_adapter_after_present(
      contract, 0x77, 0x99, gate, result, receipt, cap);
  order_observe_seq = ++order_seq;
  return status;
}

/* ------------------------------------------------------- concurrency rig */

struct swap_thread_args {
  const nxgl_graphics_contract *contract;
  nxgl_graphics_present_gate *gate;
  pthread_barrier_t *barrier;
  int got_receipt;
};

static void *swap_thread(void *arg) {
  struct swap_thread_args *a = (struct swap_thread_args *)arg;
  nxgl_graphics_gate_result result;
  char receipt[2048];
  (void)nxgl_graphics_gate_result_init(&result);
  (void)pthread_barrier_wait(a->barrier);
  (void)wrapped_swap(a->contract, a->gate, &result, receipt, sizeof receipt);
  a->got_receipt = receipt[0] != '\0';
  return NULL;
}

int main(void) {
  nxgl_graphics_contract contract;
  nxgl_graphics_present_gate gate;
  nxgl_graphics_gate_result result;
  nxgl_graphics_gate_status status;
  char receipt[2048];

  make_contract(&contract);
  (void)nxgl_graphics_gate_result_init(&result);
  nxgl_graphics_contract_adapter_set_resolver_ex(fake_resolver, NULL);
  (void)setenv("NXOBS_RUN_ID", "porttest-9-9-9", 1);
  (void)setenv("NX_GENERATION", "gen-lifecycle", 1);
  (void)setenv("NX_PORT_ID", "lifecycleport", 1);
  (void)setenv("NX_PORT_VERSION", "0.0.1", 1);

  /* 1. SDL2 happy path. Preflight while the drawable is still the 1x1
   * placeholder: AWAITING is returned immediately -- control goes back to the
   * guest, which is the whole point of the boundary. */
  (void)nxgl_graphics_present_gate_init(&gate);
  status = nxgl_graphics_contract_adapter_pre_present(&contract, 0x77, 0x99,
                                                      &gate, &result);
  CHECK(status == NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT &&
            result.evidence.sdl_major == 2 &&
            result.evidence.drawable_w == 1 &&
            result.evidence.drawable_h == 1,
        "SDL2 preflight is pending on the 1x1 placeholder");
  CHECK(count_swap == 0 && count_clear == 0 && count_draw == 0,
        "preflight fabricated no clear, draw or swap");

  /* 2. The guest presents; only then does the drawable materialize and only
   * then does the gate conclude -- order proven by the spy sequence. */
  status = wrapped_swap(&contract, &gate, &result, receipt, sizeof receipt);
  CHECK(status == NXGL_GRAPHICS_GATE_PROVED &&
            strstr(receipt, "phase=post-first-present") != NULL &&
            strstr(receipt, "first_present=1") != NULL &&
            strstr(receipt, "pre_drawable=1x1") != NULL &&
            strstr(receipt, "drawable=640x480") != NULL &&
            strstr(receipt, "run_id=porttest-9-9-9") != NULL &&
            strstr(receipt, "port_id=lifecycleport") != NULL,
        "real present then observation concludes PROVED with one receipt");
  CHECK(order_swap_seq > 0 && order_observe_seq > order_swap_seq,
        "spy order: the real swap ran strictly before the observation");
  CHECK(count_swap == 1 && count_clear == 0 && count_draw == 0,
        "exactly one swap, and it was the guest's, not the gate's");

  /* 3. After PROVED the per-frame cost is nil: no resolver work at all. */
  {
    int resolves_before = count_resolve;
    void (*real_swap)(void *) =
        (void (*)(void *))fake_resolver(NULL, "SDL_GL_SwapWindow");
    resolves_before = count_resolve;
    real_swap((void *)0x77);
    status = nxgl_graphics_contract_adapter_after_present(
        &contract, 0x77, 0x99, &gate, &result, receipt, sizeof receipt);
    CHECK(status == NXGL_GRAPHICS_GATE_PROVED && receipt[0] == '\0' &&
              count_resolve == resolves_before,
          "a PROVED gate costs no resolver call per frame and no receipt");
  }

  /* 4. SDL3: only SDL_GetWindowSizeInPixels resolves; same lifecycle. */
  fake_reset(3);
  (void)nxgl_graphics_present_gate_init(&gate);
  status = nxgl_graphics_contract_adapter_pre_present(&contract, 0x77, 0x99,
                                                      &gate, &result);
  CHECK(status == NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT &&
            result.evidence.sdl_major == 3,
        "SDL3 preflight resolves SDL_GetWindowSizeInPixels");
  status = wrapped_swap(&contract, &gate, &result, receipt, sizeof receipt);
  CHECK(status == NXGL_GRAPHICS_GATE_PROVED &&
            strstr(receipt, "sdl=3") != NULL,
        "SDL3 lifecycle concludes with the same one-shot receipt");

  /* 5. Two concurrent presenters: exactly one finalization, one receipt. */
  fake_reset(2);
  (void)nxgl_graphics_present_gate_init(&gate);
  (void)nxgl_graphics_contract_adapter_pre_present(&contract, 0x77, 0x99,
                                                   &gate, &result);
  {
    pthread_barrier_t barrier;
    pthread_t t1, t2;
    struct swap_thread_args a1, a2;
    (void)pthread_barrier_init(&barrier, NULL, 2);
    a1.contract = &contract; a1.gate = &gate; a1.barrier = &barrier;
    a1.got_receipt = 0;
    a2 = a1;
    (void)pthread_create(&t1, NULL, swap_thread, &a1);
    (void)pthread_create(&t2, NULL, swap_thread, &a2);
    (void)pthread_join(t1, NULL);
    (void)pthread_join(t2, NULL);
    (void)pthread_barrier_destroy(&barrier);
    CHECK(a1.got_receipt + a2.got_receipt == 1 &&
              gate.status == NXGL_GRAPHICS_GATE_PROVED,
          "two concurrent presents: one finalization, one receipt");
  }

  /* 6. A discardable probe context never authorizes the guest's real
   * context: the real pair needs its own preflight. */
  fake_reset(2);
  {
    nxgl_graphics_present_gate probe_gate, real_gate;
    (void)nxgl_graphics_present_gate_init(&probe_gate);
    (void)nxgl_graphics_contract_adapter_pre_present(&contract, 0x11, 0x12,
                                                     &probe_gate, &result);
    /* probe destroyed with its context */
    nxgl_graphics_present_gate_reset(&probe_gate);
    (void)nxgl_graphics_present_gate_init(&real_gate);
    status = nxgl_graphics_contract_adapter_after_present(
        &contract, 0x77, 0x99, &real_gate, &result, receipt, sizeof receipt);
    CHECK(status == NXGL_GRAPHICS_GATE_REJECTED &&
              result.reason == NXGL_GRAPHICS_GATE_MISUSE,
          "the real context is never authorized by a discarded probe");
  }

  /* 7. Legacy one-shot API: byte-compatible behavior, still refuses a stuck
   * 1x1 (short real timeout so the gate elapses on the real clock). */
  fake_reset(2);
  {
    nxgl_graphics_evidence ev;
    nxgl_graphics_contract short_contract;
    nxgl_graphics_reason reason;
    char json[4096];
    make_contract(&short_contract);
    short_contract.drawable_ready_timeout_ms = 50;
    reason = nxgl_graphics_contract_adapter_evidence(&short_contract, &ev,
                                                     json, sizeof json);
    CHECK(reason == NXGL_GRAPHICS_DRAWABLE_STUCK_1X1 &&
              strstr(json, "\"reason\":\"drawable-stuck-1x1\"") != NULL,
          "the legacy one-shot API still refuses a stuck 1x1");
  }

  nxgl_graphics_contract_adapter_set_resolver_ex(NULL, NULL);
  if (g_failures != 0) {
    printf("test_v4_graphics_lifecycle_adapter: %d FAILURES\n", g_failures);
    return 1;
  }
  printf("test_v4_graphics_lifecycle_adapter: ALL PASS\n");
  return 0;
}
