/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxinput_exit_chord.h"

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

typedef struct fake_pads {
  int down[2][NXINPUT_GPTK_CONTROL_COUNT];
} fake_pads;

static int normalized_state(void *user, size_t pad, int control) {
  fake_pads *pads = (fake_pads *)user;

  if (pad >= 2u || (control != (int)NXINPUT_GPTK_SELECT &&
                    control != (int)NXINPUT_GPTK_START)) {
    return 0;
  }
  return pads->down[pad][control];
}

int main(void) {
  nxinput_exit_chord chord;
  fake_pads pads;
  int i;

  memset(&pads, 0, sizeof pads);
  nxinput_exit_chord_init(&chord, 0u);
  CHECK(chord.hold_polls == NXINPUT_EXIT_CHORD_DEFAULT_HOLD_POLLS);

  /* SELECT and START on different pads never form a chord. */
  pads.down[0][NXINPUT_GPTK_SELECT] = 1;
  pads.down[1][NXINPUT_GPTK_START] = 1;
  for (i = 0; i < 5; i++) {
    CHECK(nxinput_exit_chord_poll(&chord, normalized_state, &pads, 2u) == 0);
  }

  /* One authoritative pad, debounce edge, sticky consume and no repeat while
   * held. The callback is equally usable by SDL2 GameController and SDL3
   * Gamepad wrappers because the state machine contains no SDL types. */
  pads.down[0][NXINPUT_GPTK_START] = 1;
  CHECK(nxinput_exit_chord_poll(&chord, normalized_state, &pads, 2u) == 0);
  CHECK(nxinput_exit_chord_poll(&chord, normalized_state, &pads, 2u) == 0);
  CHECK(nxinput_exit_chord_poll(&chord, normalized_state, &pads, 2u) == 1);
  CHECK(nxinput_exit_chord_requested(&chord) == 1);
  CHECK(nxinput_exit_chord_consume(&chord) == 1);
  CHECK(nxinput_exit_chord_consume(&chord) == 0);
  for (i = 0; i < 8; i++) {
    CHECK(nxinput_exit_chord_poll(&chord, normalized_state, &pads, 2u) == 0);
  }

  /* Release rearms. A focus/hotplug reset drops a partial hold, but never an
   * already-pending request. */
  memset(&pads, 0, sizeof pads);
  CHECK(nxinput_exit_chord_poll(&chord, normalized_state, &pads, 2u) == 0);
  pads.down[1][NXINPUT_GPTK_SELECT] = 1;
  pads.down[1][NXINPUT_GPTK_START] = 1;
  CHECK(nxinput_exit_chord_poll(&chord, normalized_state, &pads, 2u) == 0);
  CHECK(nxinput_exit_chord_poll(&chord, normalized_state, &pads, 2u) == 0);
  nxinput_exit_chord_reset_hold(&chord);
  CHECK(nxinput_exit_chord_poll(&chord, normalized_state, &pads, 2u) == 0);
  CHECK(nxinput_exit_chord_poll(&chord, normalized_state, &pads, 2u) == 0);
  CHECK(nxinput_exit_chord_poll(&chord, normalized_state, &pads, 2u) == 1);
  nxinput_exit_chord_reset_hold(&chord);
  CHECK(nxinput_exit_chord_requested(&chord) == 1);
  CHECK(nxinput_exit_chord_consume(&chord) == 1);

  /* C5B: a termination signal must land on the CHORD's request, so a port
   * has one ending and one save whichever way it stops. */
  {
    static volatile sig_atomic_t slot;
    nxinput_exit_chord term;

    nxinput_exit_chord_init(&term, 0u);
    slot = 0;
    CHECK(nxinput_exit_chord_fold_signal(&term, &slot) == 0);
    CHECK(nxinput_exit_chord_requested(&term) == 0);
    slot = 1;
    CHECK(nxinput_exit_chord_fold_signal(&term, &slot) == 1);
    CHECK(slot == 0);
    CHECK(nxinput_exit_chord_requested(&term) == 1);
    /* folding again must not manufacture a second ending */
    CHECK(nxinput_exit_chord_fold_signal(&term, &slot) == 0);
    CHECK(nxinput_exit_chord_consume(&term) == 1);
    CHECK(nxinput_exit_chord_consume(&term) == 0);
    CHECK(nxinput_exit_chord_fold_signal(&term, NULL) == 0);
    CHECK(nxinput_exit_chord_fold_signal(NULL, &slot) == 0);
  }

  (void)puts("exit chord tests: ok (version-neutral primary, sticky request, "
             "SIGTERM converges on the chord)");
  return 0;
}
