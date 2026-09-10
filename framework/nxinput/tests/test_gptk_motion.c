/* SPDX-License-Identifier: GPL-3.0-only */
/* Host unit tests for the V3 cursor/camera kinematics. Pure C99 + libm, no
 * SDL, no devices. The load-bearing test is FPS invariance: integrating a
 * constant deflection over the same wall-clock second at 30/60/120 FPS must
 * land within 2%. */
#include "nxinput_gptk_motion.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *expression, const char *file, int line) {
  (void)fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
  exit(1);
}

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!(expression))                                                        \
      fail(#expression, __FILE__, __LINE__);                                  \
  } while (0)

static void announce(const char *name) {
  (void)printf("gptk-motion: %s\n", name);
}

static int near_f(float a, float b, float tolerance) {
  float d = a - b;

  return d > -tolerance && d < tolerance;
}

/* Spec-like tuning kept slow enough that no resolution below clamps the
 * cursor during the 1-second full-deflection runs. */
static void spec_cursor_tuning(nxinput_gptk_cursor_tuning *t) {
  nxinput_gptk_cursor_tuning_defaults(t);
  t->speed = 0.5f;
  t->deadzone = 0.15f;
  t->response_curve = 1.6f;
  t->acceleration = 0.35f;
  t->smoothing_ms = 70.0f;
}

/* Integrate exactly `seconds` of constant deflection at a fixed rate and
 * return the traveled x distance. Start at the left edge, vertical center. */
static float integrate_x(const nxinput_gptk_cursor_tuning *t, float axis_x,
                         float axis_y, int rate, float seconds, int w,
                         int h) {
  nxinput_gptk_cursor_state state;
  float dt = 1.0f / (float)rate;
  int steps = (int)((float)rate * seconds + 0.5f);
  int i;

  nxinput_gptk_cursor_state_reset(&state, 0.0f, (float)h / 2.0f);
  for (i = 0; i < steps; i++) {
    CHECK(nxinput_gptk_cursor_step(t, axis_x, axis_y, dt, w, h, &state) == 0);
  }
  return state.x;
}

static void test_fps_invariance(void) {
  static const int resolutions[3][2] = {{640, 480}, {720, 720}, {1280, 720}};
  static const int rates[3] = {30, 60, 120};
  nxinput_gptk_cursor_tuning t;
  int r;

  announce("FPS invariance: 1s at 30/60/120 FPS within 2%");
  spec_cursor_tuning(&t);
  for (r = 0; r < 3; r++) {
    int w = resolutions[r][0];
    int h = resolutions[r][1];
    float d30 = integrate_x(&t, 1.0f, 0.0f, rates[0], 1.0f, w, h);
    float d60 = integrate_x(&t, 1.0f, 0.0f, rates[1], 1.0f, w, h);
    float d120 = integrate_x(&t, 1.0f, 0.0f, rates[2], 1.0f, w, h);

    (void)printf("gptk-motion:   %dx%d -> %.2f / %.2f / %.2f px\n", w, h,
                 (double)d30, (double)d60, (double)d120);
    /* Real motion happened and never hit the far clamp. */
    CHECK(d60 > (float)h * 0.25f);
    CHECK(d30 < (float)(w - 1) && d60 < (float)(w - 1) &&
          d120 < (float)(w - 1));
    /* Within 2% of each other, pairwise against the 60 FPS reference. */
    CHECK(fabsf(d30 - d60) <= 0.02f * d60);
    CHECK(fabsf(d120 - d60) <= 0.02f * d60);
  }
}

static void test_deadzone(void) {
  nxinput_gptk_cursor_tuning t;
  float below;
  float above;
  float diagonal;

  announce("deadzone: radial, no drift below, nonzero just above");
  spec_cursor_tuning(&t);

  /* Magnitude 0.10 < deadzone 0.15: EXACTLY zero displacement over 1s. */
  below = integrate_x(&t, 0.10f, 0.0f, 60, 1.0f, 1280, 720);
  CHECK(below == 0.0f);

  /* Magnitude exactly at the edge stays inside (strict >). */
  below = integrate_x(&t, 0.15f, 0.0f, 60, 1.0f, 1280, 720);
  CHECK(below == 0.0f);

  /* Just above: small but nonzero. */
  above = integrate_x(&t, 0.20f, 0.0f, 60, 1.0f, 1280, 720);
  CHECK(above > 0.0f);
  CHECK(above < 40.0f); /* far below full-deflection travel */

  /* Radial correctness: (0.2, 0.2) has magnitude ~0.283 > 0.15, so a
   * per-axis deadzone of 0.15 would NOT eat it -- and neither may ours;
   * both axes move, symmetrically. */
  {
    nxinput_gptk_cursor_state state;
    int i;

    nxinput_gptk_cursor_state_reset(&state, 100.0f, 100.0f);
    for (i = 0; i < 60; i++) {
      CHECK(nxinput_gptk_cursor_step(&t, 0.2f, 0.2f, 1.0f / 60.0f, 1280,
                                     720, &state) == 0);
    }
    CHECK(state.x > 100.0f && state.y > 100.0f);
    CHECK(near_f(state.x - 100.0f, state.y - 100.0f, 0.01f));
    diagonal = state.x - 100.0f;
    /* The diagonal (magnitude 0.283) outruns the straight 0.2 push. */
    CHECK(diagonal > above);
  }
}

static void test_context_switch_reset(void) {
  nxinput_gptk_cursor_tuning t;
  nxinput_gptk_cursor_state moving;
  nxinput_gptk_cursor_state fresh;
  int i;

  announce("context switch reset zeroes velocity and smoothing");
  spec_cursor_tuning(&t);
  nxinput_gptk_cursor_state_reset(&moving, 100.0f, 100.0f);
  for (i = 0; i < 30; i++) {
    CHECK(nxinput_gptk_cursor_step(&t, 1.0f, 0.0f, 1.0f / 60.0f, 1280, 720,
                                   &moving) == 0);
  }
  CHECK(moving.vel_x > 0.0f);

  /* Without a reset, residual smoothed velocity keeps drifting on a zero
   * axis; after the reset the cursor is dead still. */
  fresh = moving;
  nxinput_gptk_cursor_state_reset(&fresh, fresh.x, fresh.y);
  CHECK(fresh.vel_x == 0.0f && fresh.vel_y == 0.0f);
  {
    float before_x = fresh.x;
    float drift_x = moving.x;

    CHECK(nxinput_gptk_cursor_step(&t, 0.0f, 0.0f, 1.0f / 60.0f, 1280, 720,
                                   &fresh) == 0);
    CHECK(nxinput_gptk_cursor_step(&t, 0.0f, 0.0f, 1.0f / 60.0f, 1280, 720,
                                   &moving) == 0);
    CHECK(fresh.x == before_x);   /* reset: no drift at all */
    CHECK(moving.x > drift_x);    /* un-reset: smoothing still carries */
  }
}

static void test_cursor_fail_closed(void) {
  nxinput_gptk_cursor_tuning t;
  nxinput_gptk_cursor_state state;

  announce("cursor step fails closed on bad args and garbage tuning");
  spec_cursor_tuning(&t);
  nxinput_gptk_cursor_state_reset(&state, 10.0f, 10.0f);
  CHECK(nxinput_gptk_cursor_step(0, 1.0f, 0.0f, 0.016f, 640, 480,
                                 &state) == -1);
  CHECK(nxinput_gptk_cursor_step(&t, 1.0f, 0.0f, 0.0f, 640, 480,
                                 &state) == -1);
  CHECK(nxinput_gptk_cursor_step(&t, 1.0f, 0.0f, 0.016f, 0, 480,
                                 &state) == -1);
  CHECK(nxinput_gptk_cursor_step(&t, 1.0f, 0.0f, 0.016f, 640, 480, 0) == -1);
  /* An all-zero tuning (failed parse memset) moves nothing. */
  {
    nxinput_gptk_cursor_tuning zeroed;

    memset(&zeroed, 0, sizeof zeroed);
    CHECK(nxinput_gptk_cursor_step(&zeroed, 1.0f, 0.0f, 0.016f, 640, 480,
                                   &state) == -1);
    CHECK(state.x == 10.0f && state.y == 10.0f);
  }
}

static void test_camera_transform(void) {
  nxinput_gptk_camera_tuning t;
  float x1;
  float y1;
  float x2;
  float y2;

  announce("camera: inversion, sensitivity, rescaling, native authority");
  nxinput_gptk_camera_tuning_defaults(&t);
  t.deadzone = 0.15f;
  t.response_curve = 1.0f;

  /* Full deflection keeps full magnitude: rescaling loses no range. */
  CHECK(nxinput_gptk_camera_transform(&t, 1.0f, 0.0f, &x1, &y1) == 0);
  CHECK(near_f(x1, 1.0f, 0.0005f) && y1 == 0.0f);

  /* Inside the radial deadzone: exactly zero. */
  CHECK(nxinput_gptk_camera_transform(&t, 0.10f, 0.0f, &x1, &y1) == 0);
  CHECK(x1 == 0.0f && y1 == 0.0f);

  /* Sensitivity scales linearly. */
  CHECK(nxinput_gptk_camera_transform(&t, 0.6f, 0.3f, &x1, &y1) == 0);
  t.sensitivity_x = 2.0f;
  t.sensitivity_y = 2.0f;
  CHECK(nxinput_gptk_camera_transform(&t, 0.6f, 0.3f, &x2, &y2) == 0);
  CHECK(near_f(x2, 2.0f * x1, 0.0005f) && near_f(y2, 2.0f * y1, 0.0005f));
  t.sensitivity_x = 1.0f;
  t.sensitivity_y = 1.0f;

  /* Inversion flips the sign of exactly the inverted axis. */
  t.invert_x = 1u;
  CHECK(nxinput_gptk_camera_transform(&t, 0.6f, 0.3f, &x2, &y2) == 0);
  CHECK(near_f(x2, -x1, 0.0005f) && near_f(y2, y1, 0.0005f));
  t.invert_x = 0u;
  t.invert_y = 1u;
  CHECK(nxinput_gptk_camera_transform(&t, 0.6f, 0.3f, &x2, &y2) == 0);
  CHECK(near_f(x2, x1, 0.0005f) && near_f(y2, -y1, 0.0005f));
  t.invert_y = 0u;

  /* authority = native: raw pass-through, tuning applies NOTHING (deadzone
   * included), even with aggressive values configured. */
  t.authority = (uint8_t)NXINPUT_GPTK_AUTHORITY_NATIVE;
  t.sensitivity_x = 4.0f;
  t.invert_x = 1u;
  t.deadzone = 0.5f;
  CHECK(nxinput_gptk_camera_transform(&t, 0.37f, -0.9f, &x1, &y1) == 0);
  CHECK(x1 == 0.37f && y1 == -0.9f);
  CHECK(nxinput_gptk_camera_transform(&t, 0.05f, 0.0f, &x1, &y1) == 0);
  CHECK(x1 == 0.05f); /* below the configured deadzone, still raw */
}

static void test_camera_pure_function(void) {
  nxinput_gptk_camera_tuning t;
  float first_x;
  float first_y;
  float x;
  float y;
  int i;

  announce("camera transform is pure: repeated calls, no hidden state");
  nxinput_gptk_camera_tuning_defaults(&t);
  t.response_curve = 1.6f;
  CHECK(nxinput_gptk_camera_transform(&t, 0.6f, 0.3f, &first_x,
                                      &first_y) == 0);
  for (i = 0; i < 100; i++) {
    /* Interleave a different tuning to flush any (forbidden) static
     * carry-over between calls. */
    nxinput_gptk_camera_tuning other;

    nxinput_gptk_camera_tuning_defaults(&other);
    other.sensitivity_x = 7.5f;
    other.invert_y = 1u;
    CHECK(nxinput_gptk_camera_transform(&other, -0.9f, 0.9f, &x, &y) == 0);
    CHECK(nxinput_gptk_camera_transform(&t, 0.6f, 0.3f, &x, &y) == 0);
    CHECK(x == first_x && y == first_y);
  }

  /* Fail closed on bad input. */
  CHECK(nxinput_gptk_camera_transform(0, 0.6f, 0.3f, &x, &y) == -1);
  CHECK(x == 0.0f && y == 0.0f);
  {
    nxinput_gptk_camera_tuning zeroed;

    memset(&zeroed, 0, sizeof zeroed);
    CHECK(nxinput_gptk_camera_transform(&zeroed, 0.6f, 0.3f, &x, &y) == -1);
    CHECK(x == 0.0f && y == 0.0f);
  }
}

/* V3 (blocker 7): the stick-vector path is INTEGRATED into the dispatcher. */
static float g_vec_x, g_vec_y;
static int g_vec_hits;
static char g_vec_action[80];
static int g_button_hits;
static char g_button_action[80];
static void vec_sink(void *user, const char *action, float ax, float ay) {
  (void)user;
  g_vec_x = ax;
  g_vec_y = ay;
  g_vec_hits++;
  strncpy(g_vec_action, action, sizeof g_vec_action - 1u);
}

static void button_sink(void *user, const char *action, int pressed,
                        float value) {
  (void)user;
  (void)value;
  if (pressed) {
    g_button_hits++;
    (void)snprintf(g_button_action, sizeof g_button_action, "%s", action);
  }
}

static void test_dispatcher_vector_integration(void) {
  static const char text[] =
      "format = NEXTOS_CONTROLLERS/1\n"
      "[menu]\nRIGHT_STICK = cursor.move\nR3 = cursor.click\n"
      "A = ui.confirm\nUP = ui.up\n"
      "[gameplay]\nRIGHT_STICK = camera.move\nR3 = camera.reset\n"
      "A = player.jump\nUP = player.up\n";
  nxinput_gptk map;
  nxinput_gptk_dispatcher d;
  char err[128];

  if (nxinput_gptk_parse(text, strlen(text), &map, err, sizeof err) != 0) {
    (void)fprintf(stderr, "vector: parse failed: %s\n", err);
    exit(1);
  }
  nxinput_gptk_dispatcher_init(&d, &map);
  if (nxinput_gptk_dispatcher_configure_motion(&d, 1280, 720) != 0) {
    (void)fprintf(stderr, "vector: configure_motion failed\n");
    exit(1);
  }
  nxinput_gptk_dispatcher_register_vector(&d, "cursor.move", vec_sink, NULL);
  nxinput_gptk_dispatcher_register_vector(&d, "camera.move", vec_sink, NULL);
  nxinput_gptk_dispatcher_register(&d, "cursor.click", button_sink, NULL);
  nxinput_gptk_dispatcher_register(&d, "camera.reset", button_sink, NULL);
  nxinput_gptk_dispatcher_register(&d, "ui.confirm", button_sink, NULL);
  nxinput_gptk_dispatcher_register(&d, "ui.up", button_sink, NULL);
  nxinput_gptk_dispatcher_register(&d, "player.jump", button_sink, NULL);
  nxinput_gptk_dispatcher_register(&d, "player.up", button_sink, NULL);

  /* MENU: the right stick drives the cursor. The physical stick must be
   * reported as suppressed (guest must not read it raw) and one full-right
   * push over 0.5s moves the cursor to the right of centre. */
  if (nxinput_gptk_dispatcher_physical_suppressed(&d) != 1) {
    (void)fprintf(stderr, "vector: menu stick not suppressed\n");
    exit(1);
  }
  g_vec_hits = 0;
  nxinput_gptk_dispatcher_feed_stick(&d, (int)NXINPUT_GPTK_RIGHT_STICK,
                                     1.0f, 0.0f, 0.5f);
  if (g_vec_hits != 1 || strcmp(g_vec_action, "cursor.move") != 0 ||
      g_vec_x <= 640.0f) {
    (void)fprintf(stderr, "vector: cursor not driven right (x=%.1f hits=%d)\n",
                  (double)g_vec_x, g_vec_hits);
    exit(1);
  }
  /* R3 clicks the menu cursor. A and D-pad keep their declared menu actions;
   * none is hidden by the stick-only raw-suppression mask. */
  if (nxinput_gptk_dispatcher_control_suppressed(&d, NXINPUT_GPTK_R3) ||
      nxinput_gptk_dispatcher_control_suppressed(&d, NXINPUT_GPTK_A) ||
      nxinput_gptk_dispatcher_control_suppressed(&d, NXINPUT_GPTK_UP)) {
    (void)fprintf(stderr, "vector: cursor stole R3/A/D-pad\n");
    exit(1);
  }
  g_button_hits = 0;
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_R3, 1, 1.0f);
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_R3, 0, 0.0f);
  if (g_button_hits != 1 || strcmp(g_button_action, "cursor.click") != 0) {
    (void)fprintf(stderr, "vector: R3 did not click cursor\n");
    exit(1);
  }
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 1, 1.0f);
  if (g_button_hits != 2 || strcmp(g_button_action, "ui.confirm") != 0) {
    (void)fprintf(stderr, "vector: menu A stolen\n");
    exit(1);
  }
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 0, 0.0f);
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_UP, 1, 1.0f);
  if (g_button_hits != 3 || strcmp(g_button_action, "ui.up") != 0) {
    (void)fprintf(stderr, "vector: menu D-pad stolen\n");
    exit(1);
  }
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_UP, 0, 0.0f);

  /* GAMEPLAY: the same stick now feeds the camera transform, not the cursor;
   * a right push yields a positive camera x axis, negative none. */
  nxinput_gptk_dispatcher_set_context(&d, NXINPUT_GPTK_CONTEXT_GAMEPLAY);
  if (nxinput_gptk_dispatcher_physical_suppressed(&d) != 1) {
    (void)fprintf(stderr, "vector: gameplay stick not suppressed\n");
    exit(1);
  }
  g_vec_hits = 0;
  nxinput_gptk_dispatcher_feed_stick(&d, (int)NXINPUT_GPTK_RIGHT_STICK,
                                     1.0f, 0.0f, 0.5f);
  if (g_vec_hits != 1 || strcmp(g_vec_action, "camera.move") != 0 ||
      g_vec_x <= 0.0f) {
    (void)fprintf(stderr, "vector: camera not shaped (x=%.3f)\n",
                  (double)g_vec_x);
    exit(1);
  }
  /* Gameplay restores the native camera/action namespace; cursor.click is
   * unreachable here and R3/A/D-pad reach gameplay sinks. */
  g_button_hits = 0;
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_R3, 1, 1.0f);
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_R3, 0, 0.0f);
  if (g_button_hits != 1 || strcmp(g_button_action, "camera.reset") != 0) {
    (void)fprintf(stderr, "vector: gameplay R3 did not return to camera\n");
    exit(1);
  }
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 1, 1.0f);
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_A, 0, 0.0f);
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_UP, 1, 1.0f);
  if (g_button_hits != 3 || strcmp(g_button_action, "player.up") != 0) {
    (void)fprintf(stderr, "vector: gameplay A/D-pad stolen\n");
    exit(1);
  }
  nxinput_gptk_dispatcher_feed(&d, NXINPUT_GPTK_UP, 0, 0.0f);

  /* A context with the stick unmapped is NOT suppressed and delivers nothing
   * on the vector path (CURSOR context here has no stick mapping). */
  nxinput_gptk_dispatcher_set_context(&d, NXINPUT_GPTK_CONTEXT_CURSOR);
  if (nxinput_gptk_dispatcher_physical_suppressed(&d) != 0) {
    (void)fprintf(stderr, "vector: unmapped stick reported as suppressed\n");
    exit(1);
  }
  g_vec_hits = 0;
  nxinput_gptk_dispatcher_feed_stick(&d, (int)NXINPUT_GPTK_RIGHT_STICK,
                                     1.0f, 0.0f, 0.5f);
  if (g_vec_hits != 0) {
    (void)fprintf(stderr, "vector: delivered on an unmapped stick\n");
    exit(1);
  }
}

static void test_dispatcher_per_stick_suppression(void) {
  /* Blocker 7 (per-control): the framework owns ONLY the stick the live
   * context hands to a cursor.* or camera.* action; the other stick stays
   * native and MUST NOT appear in the suppression mask. Here the right stick
   * drives the camera while the left stick is a plain game action. */
  static const char text[] =
      "format = NEXTOS_CONTROLLERS/1\n"
      "[menu]\nA = ui.confirm\n"
      "[gameplay]\nRIGHT_STICK = camera.move\nLEFT_STICK = player.move\n"
      "A = player.jump\n";
  nxinput_gptk map;
  nxinput_gptk_dispatcher d;
  char err[128];
  uint32_t mask;
  const uint32_t left_bit = (uint32_t)1u << (unsigned)NXINPUT_GPTK_LEFT_STICK;
  const uint32_t right_bit = (uint32_t)1u << (unsigned)NXINPUT_GPTK_RIGHT_STICK;

  if (nxinput_gptk_parse(text, strlen(text), &map, err, sizeof err) != 0) {
    (void)fprintf(stderr, "per-stick: parse failed: %s\n", err);
    exit(1);
  }
  nxinput_gptk_dispatcher_init(&d, &map);
  nxinput_gptk_dispatcher_set_context(&d, NXINPUT_GPTK_CONTEXT_GAMEPLAY);

  mask = nxinput_gptk_dispatcher_suppressed_mask(&d);
  if ((mask & right_bit) == 0u) {
    (void)fprintf(stderr, "per-stick: right stick (camera) not owned\n");
    exit(1);
  }
  if ((mask & left_bit) != 0u) {
    (void)fprintf(stderr,
                  "per-stick: left stick STOLEN despite a game mapping "
                  "(mask=%#x)\n", (unsigned)mask);
    exit(1);
  }
  if (nxinput_gptk_dispatcher_control_suppressed(
          &d, (int)NXINPUT_GPTK_RIGHT_STICK) != 1 ||
      nxinput_gptk_dispatcher_control_suppressed(
          &d, (int)NXINPUT_GPTK_LEFT_STICK) != 0) {
    (void)fprintf(stderr, "per-stick: per-control query disagrees with mask\n");
    exit(1);
  }
  /* Whole-pad convenience still says "some stick owned". */
  if (nxinput_gptk_dispatcher_physical_suppressed(&d) != 1) {
    (void)fprintf(stderr, "per-stick: whole-pad guard wrong\n");
    exit(1);
  }
  /* Out-of-range control never claims suppression. */
  if (nxinput_gptk_dispatcher_control_suppressed(&d, -1) != 0 ||
      nxinput_gptk_dispatcher_control_suppressed(
          &d, (int)NXINPUT_GPTK_CONTROL_COUNT) != 0) {
    (void)fprintf(stderr, "per-stick: out-of-range control not rejected\n");
    exit(1);
  }
}

int main(void) {
  test_fps_invariance();
  test_deadzone();
  test_context_switch_reset();
  test_cursor_fail_closed();
  test_camera_transform();
  test_camera_pure_function();
  test_dispatcher_vector_integration();
  test_dispatcher_per_stick_suppression();
  (void)puts("gptk motion tests: ok (incl. per-stick suppression mask)");
  return 0;
}
