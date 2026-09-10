/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>

#include "../src/ab_exit_chord.h"

int main(void) {
  ab_exit_chord_state state = {0};

  assert(!ab_exit_chord_update(&state, 0, 0));
  assert(!ab_exit_chord_update(&state, 1, 0));
  assert(ab_exit_chord_update(&state, 1, 1));
  assert(!ab_exit_chord_update(&state, 1, 1));
  assert(!ab_exit_chord_update(&state, 0, 1));
  assert(ab_exit_chord_update(&state, 1, 1));
  assert(!ab_exit_chord_update(&state, 0, 0));
  puts("angrybirds exit chord: PASS edge=1 held-repeat=0");
  return 0;
}
