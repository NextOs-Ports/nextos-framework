#!/usr/bin/env python3
"""Compile and exercise the real Party Hard lost-release guard helpers."""
from pathlib import Path
import os
import subprocess
import tempfile

port = Path(__file__).resolve().parents[1]
fixture = r'''
#include "input_guard.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void set_bit(unsigned long *bits, int code) {
    bits[(unsigned)code / PH_INPUT_WORD_BITS] |=
        1UL << ((unsigned)code % PH_INPUT_WORD_BITS);
}

int main(void) {
    unsigned long caps[PH_INPUT_KEY_WORDS], state[PH_INPUT_KEY_WORDS];
    memset(caps, 0, sizeof caps);
    memset(state, 0, sizeof state);
    set_bit(caps, BTN_SOUTH);
    set_bit(caps, BTN_DPAD_DOWN);
    assert(ph_input_any_key_pressed(state, caps, PH_INPUT_KEY_WORDS) == 0);
    set_bit(state, BTN_DPAD_DOWN);
    assert(ph_input_any_key_pressed(state, caps, PH_INPUT_KEY_WORDS) == 1);
    memset(state, 0, sizeof state);
    set_bit(state, KEY_POWER); /* not a declared capability: still idle */
    assert(ph_input_any_key_pressed(state, caps, PH_INPUT_KEY_WORDS) == 0);
    assert(ph_input_any_key_pressed(NULL, caps, PH_INPUT_KEY_WORDS) == 1);

    unsigned long abs_bits[PH_INPUT_ABS_WORDS];
    memset(abs_bits, 0, sizeof abs_bits);
    set_bit(abs_bits, ABS_X);
    set_bit(abs_bits, ABS_Y);
    set_bit(abs_bits, ABS_HAT0X); /* hats never consume an SDL axis ordinal */
    set_bit(abs_bits, ABS_RX);
    assert(ph_input_abs_code_for_sdl_axis(abs_bits, PH_INPUT_ABS_WORDS, 0) == ABS_X);
    assert(ph_input_abs_code_for_sdl_axis(abs_bits, PH_INPUT_ABS_WORDS, 1) == ABS_Y);
    assert(ph_input_abs_code_for_sdl_axis(abs_bits, PH_INPUT_ABS_WORDS, 2) == ABS_RX);
    assert(ph_input_abs_code_for_sdl_axis(abs_bits, PH_INPUT_ABS_WORDS, 3) == -1);

    assert(ph_input_axis_is_centered(0, -1800, 1800, 32, 32));
    assert(ph_input_axis_is_centered(31, -1800, 1800, 32, 0));
    assert(!ph_input_axis_is_centered(200, -1800, 1800, 32, 32));
    assert(ph_input_axis_is_centered(128, 0, 255, 1, 0));
    assert(!ph_input_axis_is_centered(1, -1, 1, 0, 0));
    assert(!ph_input_axis_is_centered(0, -100, 100, 90, 0));
    assert(ph_input_hat_axis_is_centered(0, -1, 1));
    assert(ph_input_hat_axis_is_centered(1, 0, 2));
    assert(!ph_input_hat_axis_is_centered(-1, -1, 1));
    assert(!ph_input_hat_axis_is_centered(0, 0, 1));
    assert(!ph_input_hat_axis_is_centered(0, 1, 0));

    /* The guard only erases a stale non-zero value under conclusive neutral
     * evidence. Missing evidence is fail-open; zero can never become input. */
    assert(ph_input_guard_button_value(1, 1, 1, 1) == 0);
    assert(ph_input_guard_button_value(1, 0, 1, 1) == 1);
    assert(ph_input_guard_button_value(1, 1, 0, 1) == 1);
    assert(ph_input_guard_button_value(1, 1, 1, 0) == 1);
    assert(ph_input_guard_button_value(0, 0, 0, 0) == 0);
    assert(ph_input_guard_button_value(0, 1, 1, 1) == 0);
    assert(ph_input_guard_axis_value(-32768, 1, 1) == 0);
    assert(ph_input_guard_axis_value(32767, 0, 1) == 32767);
    assert(ph_input_guard_axis_value(32767, 1, 0) == 32767);
    assert(ph_input_guard_axis_value(0, 0, 0) == 0);

    int up = 0, down = 1, left = 1, right = 1;
    assert(ph_input_resolve_dpad(&up, &down, &left, &right) ==
           PH_INPUT_DPAD_CONFLICT_HORIZONTAL);
    assert(!up && down && !left && !right); /* exact field report */
    up = down = left = right = 1;
    assert(ph_input_resolve_dpad(&up, &down, &left, &right) ==
           (PH_INPUT_DPAD_CONFLICT_VERTICAL | PH_INPUT_DPAD_CONFLICT_HORIZONTAL));
    assert(!up && !down && !left && !right);
    puts("PASS: kernel-neutral verdict, exact SDL-axis identity and D-pad limiter");
}
'''

with tempfile.TemporaryDirectory(prefix="partyhard-input-guard-",
                                 dir=os.getenv("TMPDIR")) as temp:
    source = Path(temp) / "test.c"
    binary = Path(temp) / "test"
    source.write_text(fixture)
    subprocess.run([
        os.getenv("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
        "-I", str(port / "src"), str(source), str(port / "src/input_guard.c"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
