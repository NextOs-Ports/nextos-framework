/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PARTYHARD_INPUT_GUARD_H
#define PARTYHARD_INPUT_GUARD_H

#include <linux/input.h>
#include <stddef.h>

#define PH_INPUT_WORD_BITS (8U * sizeof(unsigned long))
#define PH_INPUT_KEY_WORDS \
    ((KEY_MAX + 1U + PH_INPUT_WORD_BITS - 1U) / PH_INPUT_WORD_BITS)
#define PH_INPUT_ABS_WORDS \
    ((ABS_MAX + 1U + PH_INPUT_WORD_BITS - 1U) / PH_INPUT_WORD_BITS)

enum ph_input_dpad_conflict {
    PH_INPUT_DPAD_CONFLICT_NONE = 0,
    PH_INPUT_DPAD_CONFLICT_VERTICAL = 1,
    PH_INPUT_DPAD_CONFLICT_HORIZONTAL = 2,
};

int ph_input_test_bit(const unsigned long *bits, size_t words, int code);

/* This deliberately answers only the mapping-independent question needed by
 * the lost-release guard: is any EV_KEY capability still physically down? */
int ph_input_any_key_pressed(const unsigned long *state,
                             const unsigned long *capabilities,
                             size_t words);

/* Linux SDL's evdev joystick axes are the declared EV_ABS codes in ascending
 * order, excluding hats. The SDL binding remains the mapping authority; this
 * helper only finds the kernel axis backing that already-resolved binding. */
int ph_input_abs_code_for_sdl_axis(const unsigned long *abs_bits, size_t words,
                                   int axis_index);

/* A centered physical stick proves that a non-zero cached SDL value is stale.
 * The kernel-reported flat/fuzz are honoured; malformed/short ranges fail
 * open by returning false. */
int ph_input_axis_is_centered(int value, int minimum, int maximum,
                              int flat, int fuzz);

/* Hats are discrete and may be reported as -1..1 or 0..2. A range without a
 * unique midpoint is not sufficient evidence of release. */
int ph_input_hat_axis_is_centered(int value, int minimum, int maximum);

/* Neutral-only filters: they can erase a stale non-zero value only when the
 * exact physical snapshot is conclusive. They can never manufacture input. */
int ph_input_guard_button_value(int raw, int snapshot_valid,
                                int physically_neutral, int binding_supported);
int ph_input_guard_axis_value(int raw, int snapshot_valid,
                              int physically_centered);

/* Opposite directions are an impossible intent. Cancel the contradictory
 * pair before the native Android KeyEvent route sees it. */
unsigned ph_input_resolve_dpad(int *up, int *down, int *left, int *right);

#endif
