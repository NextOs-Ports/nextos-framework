/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_EXIT_CHORD_H
#define NXINPUT_EXIT_CHORD_H

/* SDL-version-neutral SELECT+START chord.
 *
 * An SDL2 wrapper reads SDL_GameController state; an SDL3 wrapper reads
 * SDL_Gamepad state; another adapter may use any already-normalized primary
 * controller API.  This state machine never reads the GPTK action map and never
 * depends on the game receiving SELECT or START.  Raw evdev remains the narrow
 * no-primary fallback in nxinput_evdev_chord.h.
 */

#include "nxinput_gptk.h"

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_EXIT_CHORD_API_VERSION 1u
#define NXINPUT_EXIT_CHORD_DEFAULT_HOLD_POLLS 3u

typedef int (*nxinput_exit_chord_state_fn)(void *user, size_t pad,
                                           int logical_control);

typedef struct nxinput_exit_chord {
  uint32_t api_version;
  uint32_t hold_polls;
  uint32_t consecutive_polls;
  uint8_t fired_for_hold;
  uint8_t request_pending;
  uint16_t reserved;
} nxinput_exit_chord;

/* hold_polls=0 selects the canonical three-poll (~50 ms at 60 Hz) debounce. */
void nxinput_exit_chord_init(nxinput_exit_chord *chord, uint32_t hold_polls);

/* Feed already-normalized SELECT/START state for one authoritative pad. Returns
 * 1 only on the firing edge.  The request remains sticky until consumed and
 * cannot fire again during the same hold even if consumed early.
 */
int nxinput_exit_chord_update(nxinput_exit_chord *chord, int select_down,
                              int start_down);

/* Poll all authoritative pads through a version-neutral callback. SELECT and
 * START must be down on the same pad. The callback receives
 * NXINPUT_GPTK_SELECT or NXINPUT_GPTK_START only. */
int nxinput_exit_chord_poll(nxinput_exit_chord *chord,
                            nxinput_exit_chord_state_fn state, void *user,
                            size_t pad_count);

int nxinput_exit_chord_requested(const nxinput_exit_chord *chord);
int nxinput_exit_chord_consume(nxinput_exit_chord *chord);

/* Focus loss, hot-unplug or primary-source replacement: drop the in-progress
 * hold without manufacturing a request. An already pending request remains. */
void nxinput_exit_chord_reset_hold(nxinput_exit_chord *chord);

/* SIGTERM MUST CONVERGE ON THE CHORD'S PATH (C5B / audit 116B).
 *
 * A port that saves and cleans up when SELECT+START fires, but is simply
 * killed when the system asks it to stop, has two different endings -- and
 * the one nobody tests is the one that loses the save. The C5B contract is
 * that a termination signal raises the SAME sticky request the chord raises,
 * so there is exactly one finalisation whichever way the run ends.
 *
 * It must NEVER be answered by manufacturing a keyboard event. Escape and
 * Enter are the game's, not ours.
 *
 * A signal handler may do almost nothing safely, so it writes only to a
 * `volatile sig_atomic_t` slot:
 *
 *     static volatile sig_atomic_t g_term;
 *     static void on_term(int s) { (void)s; g_term = 1; }
 *     ...
 *     signal(SIGTERM, on_term);
 *     while (running) {
 *       nxinput_exit_chord_poll(&chord, read_pad, ctx, pads);
 *       nxinput_exit_chord_fold_signal(&chord, &g_term);
 *       if (nxinput_exit_chord_consume(&chord)) { save(); break; }
 *     }
 *
 * Returns 1 when a signal was folded in on this call. Folding is idempotent:
 * the slot is cleared, so a signal cannot produce two finalisations. */
int nxinput_exit_chord_fold_signal(nxinput_exit_chord *chord,
                                   volatile sig_atomic_t *slot);

#ifdef __cplusplus
}
#endif

#endif
