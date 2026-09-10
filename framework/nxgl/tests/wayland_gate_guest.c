/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-GRAPHICS-04 hermetic Wayland fixture guest.
 *
 * Runs under an isolated headless Wayland compositor (its own socket, never
 * the user session). A REAL SDL2 window + GLES context is created; the gate's
 * preflight runs BEFORE any present and must come back pending -- control
 * returns to this guest, which then draws and presents its first real buffer
 * and only afterwards hands the observation to the gate.
 *
 *   present    -- draw + real SDL_GL_SwapWindow, then after_present until the
 *                 verdict. Exit 0 only on PROVED with the one-shot receipt
 *                 (printed as RECEIPT: <line>).
 *   no-present -- never presents. The gate must still be pending (never OK,
 *                 never a receipt) when the run ends. Exit 3 signals that
 *                 fail-closed hold to the harness; anything else is a bug.
 *
 * The guest draws its own clear; the nxgl side must fabricate nothing. */
#include "nxgl_graphics_contract_adapter.h"
#include "nxgl_graphics_present_gate.h"

#include <SDL.h>
#include <SDL_opengles2.h>

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  nxgl_graphics_contract contract;
  nxgl_graphics_present_gate gate;
  nxgl_graphics_gate_result result;
  nxgl_graphics_gate_status status;
  SDL_Window *window;
  SDL_GLContext context;
  char receipt[2048];
  char line[256];
  int no_present;
  int pre_w = -1, pre_h = -1;
  int i;

  if (argc != 2 || (strcmp(argv[1], "present") != 0 &&
                    strcmp(argv[1], "no-present") != 0)) {
    fprintf(stderr, "usage: %s present|no-present\n", argv[0]);
    return 2;
  }
  no_present = strcmp(argv[1], "no-present") == 0;

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 2;
  }
  {
    const char *driver = SDL_GetCurrentVideoDriver();
    printf("VIDEO-DRIVER: %s\n", driver != NULL ? driver : "(null)");
    if (driver == NULL || strcmp(driver, "wayland") != 0) {
      fprintf(stderr, "not running on the wayland backend\n");
      return 2;
    }
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  window = SDL_CreateWindow("nxgl-v4-wayland-fixture", SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED, 640, 480,
                            SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
  if (window == NULL) {
    fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    return 2;
  }
  SDL_ShowWindow(window);
  context = SDL_GL_CreateContext(window);
  if (context == NULL) {
    fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
    return 2;
  }

  (void)nxgl_graphics_contract_default(&contract);
  contract.api = NXGL_GRAPHICS_API_GLES;
  contract.profile = NXGL_GRAPHICS_PROFILE_ES;
  contract.version_major = 2;
  contract.version_minor = 0;
  contract.version_policy = NXGL_GRAPHICS_POLICY_MINIMUM;
  contract.shader_dialect = NXGL_SHADER_DIALECT_ESSL100;
  contract.drawable_ready_timeout_ms = 8000;

  /* PHASE 1: before any present. Must be PENDING -- never OK, never a
   * receipt -- and must return control to the guest immediately. */
  (void)nxgl_graphics_present_gate_init(&gate);
  (void)nxgl_graphics_gate_result_init(&result);
  status = nxgl_graphics_contract_adapter_pre_present(
      &contract, (uintptr_t)window, (uintptr_t)context, &gate, &result);
  pre_w = result.evidence.drawable_w;
  pre_h = result.evidence.drawable_h;
  printf("PREFLIGHT: status=%s reason=%s pre_drawable=%dx%d\n",
         nxgl_graphics_gate_status_name(status),
         nxgl_graphics_reason_name(result.reason), pre_w, pre_h);
  if (status != NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT) {
    fprintf(stderr, "preflight did not come back pending\n");
    return 2;
  }
  if (nxgl_graphics_present_gate_prepresent_line(&gate, line, sizeof line) >
      0u) {
    printf("%s\n", line);
  }

  if (no_present) {
    /* The guest never presents. Pump events for a while; the gate must stay
     * pending with no receipt: a window that never presents can never be
     * proven, and nothing here may fabricate the present. */
    for (i = 0; i < 50; i++) {
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
      }
      SDL_Delay(20);
    }
    if (gate.status != NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT ||
        gate.receipt_emitted != 0) {
      fprintf(stderr, "a window without a present changed the gate state\n");
      return 2;
    }
    printf("NO-PRESENT: state=awaiting-first-present final=0 receipt=absent\n");
    return 3; /* the harness reads 3 as the correct fail-closed hold */
  }

  /* The guest's own first frame: draw, then the REAL present, and only then
   * the observation. Keep presenting until the verdict (the deadline budget
   * belongs to the gate, counted from the FIRST present). */
  receipt[0] = '\0';
  for (i = 0; i < 1000; i++) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
    }
    glClearColor(0.0f, 0.25f, 0.5f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(window); /* the real present, by the guest */
    status = nxgl_graphics_contract_adapter_after_present(
        &contract, (uintptr_t)window, (uintptr_t)context, &gate, &result,
        receipt, sizeof receipt);
    if (status != NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT) {
      break;
    }
    SDL_Delay(10);
  }
  printf("VERDICT: status=%s reason=%s drawable=%dx%d pre_drawable=%dx%d\n",
         nxgl_graphics_gate_status_name(status),
         nxgl_graphics_reason_name(result.reason), result.evidence.drawable_w,
         result.evidence.drawable_h, pre_w, pre_h);
  if (status != NXGL_GRAPHICS_GATE_PROVED) {
    fprintf(stderr, "the gate did not conclude PROVED after the commit\n");
    return 1;
  }
  if (receipt[0] == '\0') {
    fprintf(stderr, "PROVED without the one-shot receipt\n");
    return 1;
  }
  printf("RECEIPT: %s\n", receipt);

  /* One more present: the verdict must be stable with no second receipt. */
  glClear(GL_COLOR_BUFFER_BIT);
  SDL_GL_SwapWindow(window);
  {
    char second[2048];
    status = nxgl_graphics_contract_adapter_after_present(
        &contract, (uintptr_t)window, (uintptr_t)context, &gate, &result,
        second, sizeof second);
    if (status != NXGL_GRAPHICS_GATE_PROVED || second[0] != '\0') {
      fprintf(stderr, "the conclusion was not one-shot\n");
      return 1;
    }
  }
  printf("ONE-SHOT: ok\n");

  SDL_GL_DeleteContext(context);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
