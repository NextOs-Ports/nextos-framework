/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxinput_gptk_motion.h"

#include <math.h>

/* NaN-safe clamp of one axis into -1..1 (NaN becomes 0). */
static float motion_clamp_axis(float value) {
  if (!(value == value)) {
    return 0.0f; /* NaN */
  }
  if (value < -1.0f) {
    return -1.0f;
  }
  if (value > 1.0f) {
    return 1.0f;
  }
  return value;
}

static int motion_in_bounds(float value, float min, float max) {
  return value == value && value >= min && value <= max;
}

static int motion_cursor_tuning_valid(const nxinput_gptk_cursor_tuning *t) {
  return motion_in_bounds(t->speed, NXINPUT_GPTK_SPEED_MIN,
                          NXINPUT_GPTK_SPEED_MAX) &&
         motion_in_bounds(t->deadzone, NXINPUT_GPTK_DEADZONE_MIN,
                          NXINPUT_GPTK_DEADZONE_MAX) &&
         motion_in_bounds(t->response_curve, NXINPUT_GPTK_CURVE_MIN,
                          NXINPUT_GPTK_CURVE_MAX) &&
         motion_in_bounds(t->acceleration, NXINPUT_GPTK_ACCEL_MIN,
                          NXINPUT_GPTK_ACCEL_MAX) &&
         motion_in_bounds(t->smoothing_ms, NXINPUT_GPTK_SMOOTHING_MIN,
                          NXINPUT_GPTK_SMOOTHING_MAX);
}

static int motion_camera_tuning_valid(const nxinput_gptk_camera_tuning *t) {
  return motion_in_bounds(t->sensitivity_x, NXINPUT_GPTK_SENSITIVITY_MIN,
                          NXINPUT_GPTK_SENSITIVITY_MAX) &&
         motion_in_bounds(t->sensitivity_y, NXINPUT_GPTK_SENSITIVITY_MIN,
                          NXINPUT_GPTK_SENSITIVITY_MAX) &&
         motion_in_bounds(t->deadzone, NXINPUT_GPTK_DEADZONE_MIN,
                          NXINPUT_GPTK_DEADZONE_MAX) &&
         motion_in_bounds(t->response_curve, NXINPUT_GPTK_CURVE_MIN,
                          NXINPUT_GPTK_CURVE_MAX) &&
         (t->invert_x == 0u || t->invert_x == 1u) &&
         (t->invert_y == 0u || t->invert_y == 1u) &&
         (t->authority == (uint8_t)NXINPUT_GPTK_AUTHORITY_NEXTOS ||
          t->authority == (uint8_t)NXINPUT_GPTK_AUTHORITY_NATIVE);
}

void nxinput_gptk_cursor_state_reset(nxinput_gptk_cursor_state *state,
                                     float x, float y) {
  if (state == 0) {
    return;
  }
  state->x = x;
  state->y = y;
  state->vel_x = 0.0f;
  state->vel_y = 0.0f;
}

int nxinput_gptk_cursor_step(const nxinput_gptk_cursor_tuning *tuning,
                             float axis_x, float axis_y, float dt_seconds,
                             int drawable_w, int drawable_h,
                             nxinput_gptk_cursor_state *state) {
  float ax;
  float ay;
  float magnitude;
  float target_vx = 0.0f;
  float target_vy = 0.0f;
  float alpha = 1.0f;
  float old_vx;
  float old_vy;

  if (tuning == 0 || state == 0 || !(dt_seconds > 0.0f) ||
      drawable_w <= 0 || drawable_h <= 0) {
    return -1;
  }
  if (!motion_cursor_tuning_valid(tuning)) {
    return -1; /* fail closed: a cleared/garbage tuning moves nothing */
  }

  ax = motion_clamp_axis(axis_x);
  ay = motion_clamp_axis(axis_y);
  magnitude = sqrtf(ax * ax + ay * ay);
  if (magnitude > 1.0f) {
    ax /= magnitude;
    ay /= magnitude;
    magnitude = 1.0f;
  }

  if (magnitude > tuning->deadzone) {
    /* Radial deadzone with rescaling: 0 at the deadzone edge, 1 at full
     * deflection -- no resolution loss above the deadzone. */
    float normalized = (magnitude - tuning->deadzone) /
                       (1.0f - tuning->deadzone);
    float curved = powf(normalized, tuning->response_curve);
    /* Acceleration: extra gain that grows with deflection. At full
     * deflection the speed is speed * (1 + acceleration). Magnitude-based,
     * not time-based, so it cannot break FPS invariance. */
    float gain = curved * (1.0f + tuning->acceleration * curved);
    /* speed is screen-heights per second: scale by the drawable HEIGHT so
     * the same file feels identical at 480p and 720p. */
    float pixels_per_second = tuning->speed * (float)drawable_h;
    float inv_magnitude = 1.0f / magnitude;

    target_vx = ax * inv_magnitude * gain * pixels_per_second;
    target_vy = ay * inv_magnitude * gain * pixels_per_second;
  }

  /* Exponential smoothing toward the target velocity. alpha derived from
   * dt keeps the response identical across frame rates. */
  if (tuning->smoothing_ms > 0.0f) {
    float tau = tuning->smoothing_ms / 1000.0f;

    alpha = 1.0f - expf(-dt_seconds / tau);
  }
  old_vx = state->vel_x;
  old_vy = state->vel_y;
  state->vel_x = old_vx + (target_vx - old_vx) * alpha;
  state->vel_y = old_vy + (target_vy - old_vy) * alpha;

  /* Trapezoidal integration removes the O(dt) drift a plain Euler step
   * shows between 30 and 120 FPS. */
  state->x += 0.5f * (old_vx + state->vel_x) * dt_seconds;
  state->y += 0.5f * (old_vy + state->vel_y) * dt_seconds;

  if (state->x < 0.0f) {
    state->x = 0.0f;
  } else if (state->x > (float)(drawable_w - 1)) {
    state->x = (float)(drawable_w - 1);
  }
  if (state->y < 0.0f) {
    state->y = 0.0f;
  } else if (state->y > (float)(drawable_h - 1)) {
    state->y = (float)(drawable_h - 1);
  }
  return 0;
}

int nxinput_gptk_camera_transform(const nxinput_gptk_camera_tuning *tuning,
                                  float axis_x, float axis_y, float *out_x,
                                  float *out_y) {
  float ax;
  float ay;
  float magnitude;

  if (out_x == 0 || out_y == 0) {
    return -1;
  }
  *out_x = 0.0f;
  *out_y = 0.0f;
  if (tuning == 0 || !motion_camera_tuning_valid(tuning)) {
    return -1; /* fail closed */
  }

  if (tuning->authority == (uint8_t)NXINPUT_GPTK_AUTHORITY_NATIVE) {
    /* Native authority: the game's own menu governs deadzone, sensitivity
     * and inversion. NextOS applies NOTHING (deadzone treated as 0) so
     * tuning is never applied twice. */
    *out_x = axis_x;
    *out_y = axis_y;
    return 0;
  }

  ax = motion_clamp_axis(axis_x);
  ay = motion_clamp_axis(axis_y);
  magnitude = sqrtf(ax * ax + ay * ay);
  if (magnitude > 1.0f) {
    ax /= magnitude;
    ay /= magnitude;
    magnitude = 1.0f;
  }
  if (magnitude <= tuning->deadzone) {
    return 0; /* inside the radial deadzone: exactly zero, no drift */
  }

  {
    float normalized = (magnitude - tuning->deadzone) /
                       (1.0f - tuning->deadzone);
    float curved = powf(normalized, tuning->response_curve);
    float scale = curved / magnitude; /* rescaled: keeps direction, remaps
                                       * magnitude deadzone..1 -> 0..1 */
    float x = ax * scale * tuning->sensitivity_x;
    float y = ay * scale * tuning->sensitivity_y;

    *out_x = tuning->invert_x ? -x : x;
    *out_y = tuning->invert_y ? -y : y;
  }
  return 0;
}
