/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxinput_core.h"

#include <math.h>
#include <string.h>

static float clampf(float value, float minimum, float maximum) {
  if (!(value >= minimum))
    return minimum;
  if (value > maximum)
    return maximum;
  return value;
}

float nxinput_core_axis(int16_t raw) {
  if (raw < 0)
    return (float)raw / 32768.0f;
  if (raw > 0)
    return (float)raw / 32767.0f;
  return 0.0f;
}

float nxinput_core_trigger(int16_t raw, float deadzone) {
  float value;

  if (raw <= 0)
    return 0.0f;

  value = (float)raw / 32767.0f;
  if (value <= deadzone)
    return 0.0f;
  return clampf((value - deadzone) / (1.0f - deadzone), 0.0f, 1.0f);
}

void nxinput_core_filter_stick(nxinput_stick_filter *filter, int16_t raw_x,
                               int16_t raw_y, float enter_deadzone,
                               float exit_deadzone, float *output_x,
                               float *output_y) {
  float input_x;
  float input_y;
  float raw_magnitude;
  float magnitude;
  float scaled_magnitude;

  if (!filter || !output_x || !output_y)
    return;

  input_x = nxinput_core_axis(raw_x);
  input_y = nxinput_core_axis(raw_y);
  raw_magnitude = sqrtf(input_x * input_x + input_y * input_y);

  if (filter->active) {
    if (raw_magnitude <= exit_deadzone)
      filter->active = 0;
  } else if (raw_magnitude >= enter_deadzone) {
    filter->active = 1;
  }

  if (!filter->active || raw_magnitude <= 0.0f) {
    *output_x = 0.0f;
    *output_y = 0.0f;
    return;
  }

  magnitude = clampf(raw_magnitude, 0.0f, 1.0f);
  scaled_magnitude =
      clampf((magnitude - exit_deadzone) / (1.0f - exit_deadzone),
             0.0f, 1.0f);
  *output_x = (input_x / raw_magnitude) * scaled_magnitude;
  *output_y = (input_y / raw_magnitude) * scaled_magnitude;
}

void nxinput_core_pad_init(nxinput_core_pad *pad) {
  if (!pad)
    return;
  memset(pad, 0, sizeof(*pad));
  pad->cursor_x = 0.5f;
  pad->cursor_y = 0.5f;
}

void nxinput_core_set_button(nxinput_core_pad *pad, nxinput_button button,
                             int down, int cursor_is_menu,
                             int *quit_requested) {
  uint32_t bit;
  int was_down;
  int chord_down;

  if (!pad || button < 0 || button >= NXINPUT_BUTTON_COUNT)
    return;

  bit = NXINPUT_BUTTON_BIT(button);
  was_down = (pad->buttons & bit) != 0u;
  down = down != 0;

  if (down != was_down) {
    if (down) {
      pad->buttons |= bit;
      pad->pressed_latch |= bit;
      if (button == NXINPUT_BUTTON_RIGHT_STICK && cursor_is_menu)
        pad->cursor_click_latch = 1;
    } else {
      pad->buttons &= ~bit;
      pad->released_latch |= bit;
    }
  }

  chord_down =
      (pad->buttons & NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_BACK)) != 0u &&
      (pad->buttons & NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_START)) != 0u;
  if (chord_down && !pad->quit_chord_active) {
    pad->quit_chord_active = 1;
    if (quit_requested)
      *quit_requested = 1;
  } else if (!chord_down) {
    pad->quit_chord_active = 0;
  }
}

void nxinput_core_cursor_reset_motion(nxinput_core_pad *pad,
                                      int clear_click) {
  if (!pad)
    return;
  pad->cursor_velocity_x = 0.0f;
  pad->cursor_velocity_y = 0.0f;
  if (clear_click)
    pad->cursor_click_latch = 0;
}

void nxinput_core_release_all(nxinput_core_pad *pad) {
  if (!pad)
    return;

  pad->released_latch |= pad->buttons;
  pad->buttons = 0u;
  /* A press not yet delivered must not become a ghost action after a focus or
   * device lifecycle boundary. Releases remain observable. */
  pad->pressed_latch = 0u;
  pad->left_x = 0.0f;
  pad->left_y = 0.0f;
  pad->right_x = 0.0f;
  pad->right_y = 0.0f;
  pad->left_trigger = 0.0f;
  pad->right_trigger = 0.0f;
  pad->left_filter.active = 0;
  pad->right_filter.active = 0;
  pad->quit_chord_active = 0;
  nxinput_core_cursor_reset_motion(pad, 1);
}

void nxinput_core_set_axes(
    nxinput_core_pad *pad,
    const int16_t raw_axes[NXINPUT_CORE_AXIS_COUNT],
    float enter_deadzone, float exit_deadzone, float trigger_deadzone) {
  if (!pad || !raw_axes)
    return;

  nxinput_core_filter_stick(&pad->left_filter, raw_axes[0], raw_axes[1],
                            enter_deadzone, exit_deadzone, &pad->left_x,
                            &pad->left_y);
  nxinput_core_filter_stick(&pad->right_filter, raw_axes[2], raw_axes[3],
                            enter_deadzone, exit_deadzone, &pad->right_x,
                            &pad->right_y);
  pad->left_trigger = nxinput_core_trigger(raw_axes[4], trigger_deadzone);
  pad->right_trigger = nxinput_core_trigger(raw_axes[5], trigger_deadzone);
}

void nxinput_core_cursor_update(nxinput_core_pad *pad, float delta_seconds,
                                float speed, float smoothing,
                                nxinput_cursor_state *state) {
  float magnitude;
  float desired_x;
  float desired_y;
  float alpha;
  float old_x;
  float old_y;

  if (!pad || !state)
    return;

  if (!(delta_seconds >= 0.0f))
    delta_seconds = 0.0f;
  /* A debugger pause or a stalled frame must not fling the cursor across the
   * screen. Normal motion remains frame-time based. */
  if (delta_seconds > 0.1f)
    delta_seconds = 0.1f;

  magnitude = sqrtf(pad->right_x * pad->right_x +
                    pad->right_y * pad->right_y);
  magnitude = clampf(magnitude, 0.0f, 1.0f);

  /* Multiplying the already radial-normalized vector by its magnitude gives a
   * quadratic response: precise near center, full speed at the rim. */
  desired_x = pad->right_x * magnitude * speed;
  desired_y = pad->right_y * magnitude * speed;
  if (smoothing <= 0.0f || delta_seconds <= 0.0f)
    alpha = delta_seconds > 0.0f ? 1.0f : 0.0f;
  else
    alpha = delta_seconds / (smoothing + delta_seconds);

  pad->cursor_velocity_x +=
      (desired_x - pad->cursor_velocity_x) * alpha;
  pad->cursor_velocity_y +=
      (desired_y - pad->cursor_velocity_y) * alpha;

  old_x = pad->cursor_x;
  old_y = pad->cursor_y;
  pad->cursor_x += pad->cursor_velocity_x * delta_seconds;
  pad->cursor_y += pad->cursor_velocity_y * delta_seconds;

  if (pad->cursor_x <= 0.0f) {
    pad->cursor_x = 0.0f;
    if (pad->cursor_velocity_x < 0.0f)
      pad->cursor_velocity_x = 0.0f;
  } else if (pad->cursor_x >= 1.0f) {
    pad->cursor_x = 1.0f;
    if (pad->cursor_velocity_x > 0.0f)
      pad->cursor_velocity_x = 0.0f;
  }

  if (pad->cursor_y <= 0.0f) {
    pad->cursor_y = 0.0f;
    if (pad->cursor_velocity_y < 0.0f)
      pad->cursor_velocity_y = 0.0f;
  } else if (pad->cursor_y >= 1.0f) {
    pad->cursor_y = 1.0f;
    if (pad->cursor_velocity_y > 0.0f)
      pad->cursor_velocity_y = 0.0f;
  }

  state->active = 1;
  state->moved = fabsf(pad->cursor_x - old_x) > 0.000001f ||
                 fabsf(pad->cursor_y - old_y) > 0.000001f;
  state->click_pending = pad->cursor_click_latch;
  state->x = pad->cursor_x;
  state->y = pad->cursor_y;
  state->velocity_x = pad->cursor_velocity_x;
  state->velocity_y = pad->cursor_velocity_y;
}
