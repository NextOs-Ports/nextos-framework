/* Edge detector shared by the SDL and evdev exit routes.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AB_EXIT_CHORD_H
#define AB_EXIT_CHORD_H

typedef struct ab_exit_chord_state {
  unsigned char down;
} ab_exit_chord_state;

static inline int ab_exit_chord_update(ab_exit_chord_state *state,
                                       int select_down, int start_down) {
  int down;
  int pressed;

  if (!state)
    return 0;
  down = select_down != 0 && start_down != 0;
  pressed = down && !state->down;
  state->down = (unsigned char)down;
  return pressed;
}

#endif
