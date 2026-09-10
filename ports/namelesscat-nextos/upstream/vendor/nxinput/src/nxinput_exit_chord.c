/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxinput_exit_chord.h"

#include <string.h>

void nxinput_exit_chord_init(nxinput_exit_chord *chord, uint32_t hold_polls) {
  if (chord == 0) {
    return;
  }
  memset(chord, 0, sizeof *chord);
  chord->api_version = NXINPUT_EXIT_CHORD_API_VERSION;
  chord->hold_polls = hold_polls == 0u
                          ? (uint32_t)NXINPUT_EXIT_CHORD_DEFAULT_HOLD_POLLS
                          : hold_polls;
}

int nxinput_exit_chord_update(nxinput_exit_chord *chord, int select_down,
                              int start_down) {
  if (chord == 0 || chord->api_version != NXINPUT_EXIT_CHORD_API_VERSION ||
      chord->hold_polls == 0u) {
    return 0;
  }
  if (!select_down || !start_down) {
    chord->consecutive_polls = 0u;
    chord->fired_for_hold = 0u;
    return 0;
  }
  if (chord->consecutive_polls < chord->hold_polls) {
    chord->consecutive_polls++;
  }
  if (chord->consecutive_polls >= chord->hold_polls &&
      !chord->fired_for_hold) {
    chord->fired_for_hold = 1u;
    chord->request_pending = 1u;
    return 1;
  }
  return 0;
}

int nxinput_exit_chord_poll(nxinput_exit_chord *chord,
                            nxinput_exit_chord_state_fn state, void *user,
                            size_t pad_count) {
  size_t pad;
  int chord_down = 0;

  if (state != 0) {
    for (pad = 0u; pad < pad_count; pad++) {
      int select_down = state(user, pad, (int)NXINPUT_GPTK_SELECT);
      int start_down = state(user, pad, (int)NXINPUT_GPTK_START);

      if (select_down && start_down) {
        chord_down = 1;
        break;
      }
    }
  }
  return nxinput_exit_chord_update(chord, chord_down, chord_down);
}

int nxinput_exit_chord_requested(const nxinput_exit_chord *chord) {
  return chord != 0 &&
                 chord->api_version == NXINPUT_EXIT_CHORD_API_VERSION &&
                 chord->request_pending
             ? 1
             : 0;
}

int nxinput_exit_chord_consume(nxinput_exit_chord *chord) {
  int pending;

  if (chord == 0 || chord->api_version != NXINPUT_EXIT_CHORD_API_VERSION) {
    return 0;
  }
  pending = chord->request_pending ? 1 : 0;
  chord->request_pending = 0u;
  return pending;
}

void nxinput_exit_chord_reset_hold(nxinput_exit_chord *chord) {
  if (chord == 0 || chord->api_version != NXINPUT_EXIT_CHORD_API_VERSION) {
    return;
  }
  chord->consecutive_polls = 0u;
  chord->fired_for_hold = 0u;
}

/* The signal and the chord raise the SAME request, so a port has exactly one
 * ending. Clearing the slot here is what keeps a single SIGTERM from being
 * folded twice; nothing keyboard-shaped is manufactured anywhere. */
int nxinput_exit_chord_fold_signal(nxinput_exit_chord *chord,
                                   volatile sig_atomic_t *slot) {
  if (chord == 0 || slot == 0 || *slot == 0) {
    return 0;
  }
  *slot = 0;
  chord->request_pending = 1u;
  chord->fired_for_hold = 1u;
  return 1;
}
