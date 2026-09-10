/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>

#include "../src/ab_cursor_buttons.h"

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "cursor button test failed at line %d: %s\n", __LINE__, \
              #expr);                                                          \
      return 1;                                                                \
    }                                                                          \
  } while (0)

int main(void) {
  ab_cursor_button_state state = {0, 0, 0};
  unsigned int action;

  action = ab_cursor_button_update(&state, 1, 0, 0, 0, 1);
  CHECK(action == AB_CURSOR_TOUCH_DOWN && state.touch_down);
  action = ab_cursor_button_update(&state, 0, 1, 0, 0, 1);
  CHECK(action == AB_CURSOR_TOUCH_UP && !state.touch_down);

  action = ab_cursor_button_update(&state, 0, 0, 1, 0, 1);
  CHECK(action == AB_CURSOR_TOUCH_DOWN && state.touch_down);
  action = ab_cursor_button_update(&state, 0, 0, 0, 1, 1);
  CHECK(action == AB_CURSOR_TOUCH_UP && !state.touch_down);

  action = ab_cursor_button_update(&state, 1, 0, 0, 0, 1);
  CHECK(action == AB_CURSOR_TOUCH_DOWN);
  action = ab_cursor_button_update(&state, 0, 0, 1, 0, 1);
  CHECK(action == AB_CURSOR_TOUCH_NONE && state.touch_down);
  action = ab_cursor_button_update(&state, 0, 1, 0, 0, 1);
  CHECK(action == AB_CURSOR_TOUCH_NONE && state.touch_down);
  action = ab_cursor_button_update(&state, 0, 0, 0, 1, 1);
  CHECK(action == AB_CURSOR_TOUCH_UP && !state.touch_down);

  action = ab_cursor_button_update(&state, 1, 1, 0, 0, 1);
  CHECK(action == (AB_CURSOR_TOUCH_DOWN | AB_CURSOR_TOUCH_UP));
  CHECK(!state.a_down && !state.r3_down && !state.touch_down);

  action = ab_cursor_button_update(&state, 1, 0, 0, 0, 0);
  CHECK(action == AB_CURSOR_TOUCH_NONE && state.a_down && !state.touch_down);
  action = ab_cursor_button_update(&state, 0, 0, 1, 0, 1);
  CHECK(action == AB_CURSOR_TOUCH_DOWN && state.touch_down);
  action = ab_cursor_button_update(&state, 0, 1, 0, 0, 1);
  CHECK(action == AB_CURSOR_TOUCH_NONE && state.touch_down);
  action = ab_cursor_button_update(&state, 0, 0, 0, 1, 1);
  CHECK(action == AB_CURSOR_TOUCH_UP && !state.touch_down);

  puts("angrybirds cursor buttons: PASS A=1 R3=1 combined-release=1 tap=1 ownership-gate=1");
  return 0;
}
