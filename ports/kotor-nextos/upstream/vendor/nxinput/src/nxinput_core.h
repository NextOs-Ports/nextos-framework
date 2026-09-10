/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_CORE_H
#define NXINPUT_CORE_H

#include <stdint.h>

#include "nxinput.h"

#define NXINPUT_CORE_AXIS_COUNT 6u

typedef struct nxinput_stick_filter {
  int active;
} nxinput_stick_filter;

typedef struct nxinput_core_pad {
  uint32_t buttons;
  uint32_t pressed_latch;
  uint32_t released_latch;

  float left_x;
  float left_y;
  float right_x;
  float right_y;
  float left_trigger;
  float right_trigger;

  nxinput_stick_filter left_filter;
  nxinput_stick_filter right_filter;
  int quit_chord_active;

  float cursor_x;
  float cursor_y;
  float cursor_velocity_x;
  float cursor_velocity_y;
  int cursor_click_latch;
} nxinput_core_pad;

float nxinput_core_axis(int16_t raw);
float nxinput_core_trigger(int16_t raw, float deadzone);
void nxinput_core_filter_stick(nxinput_stick_filter *filter, int16_t raw_x,
                               int16_t raw_y, float enter_deadzone,
                               float exit_deadzone, float *output_x,
                               float *output_y);

void nxinput_core_pad_init(nxinput_core_pad *pad);
void nxinput_core_set_button(nxinput_core_pad *pad, nxinput_button button,
                             int down, int cursor_is_menu,
                             int *quit_requested);
void nxinput_core_release_all(nxinput_core_pad *pad);
void nxinput_core_set_axes(
    nxinput_core_pad *pad,
    const int16_t raw_axes[NXINPUT_CORE_AXIS_COUNT],
    float enter_deadzone, float exit_deadzone, float trigger_deadzone);

void nxinput_core_cursor_reset_motion(nxinput_core_pad *pad,
                                      int clear_click);
void nxinput_core_cursor_update(nxinput_core_pad *pad, float delta_seconds,
                                float speed, float smoothing,
                                nxinput_cursor_state *state);

#endif
