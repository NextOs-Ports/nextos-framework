/* Proven Angry Birds global-pointer A/R3 ownership state machine.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AB_CURSOR_BUTTONS_H
#define AB_CURSOR_BUTTONS_H

typedef struct ab_cursor_button_state {
  int a_down;
  int r3_down;
  int touch_down;
} ab_cursor_button_state;

enum {
  AB_CURSOR_TOUCH_NONE = 0,
  AB_CURSOR_TOUCH_DOWN = 1,
  AB_CURSOR_TOUCH_UP = 2
};

/* Press and release latches may both be present when a complete short tap
 * happened between frames. A and R3 are aliases for one logical finger: the
 * first source lowers it and only the last released source raises it. */
static inline unsigned int
ab_cursor_button_update(ab_cursor_button_state *state, int a_pressed,
                        int a_released, int r3_pressed, int r3_released,
                        int allow_touch_down) {
  unsigned int actions = AB_CURSOR_TOUCH_NONE;

  if (a_pressed)
    state->a_down = 1;
  if (r3_pressed)
    state->r3_down = 1;
  /* The approved Mali-450 adapter suppresses a new pointer finger while the
   * slingshot or the two-finger camera gesture owns touch. A later fresh A/R3
   * edge may start it after that owner releases. */
  if ((a_pressed || r3_pressed) && !state->touch_down && allow_touch_down &&
      (state->a_down || state->r3_down)) {
    state->touch_down = 1;
    actions |= AB_CURSOR_TOUCH_DOWN;
  }

  if (a_released)
    state->a_down = 0;
  if (r3_released)
    state->r3_down = 0;
  if (state->touch_down && !state->a_down && !state->r3_down) {
    state->touch_down = 0;
    actions |= AB_CURSOR_TOUCH_UP;
  }
  return actions;
}

#endif
