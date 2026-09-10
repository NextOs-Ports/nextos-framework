/* SPDX-License-Identifier: GPL-3.0-only */
#include "input_guard.h"

int ph_input_test_bit(const unsigned long *bits, size_t words, int code)
{
    if (!bits || code < 0)
        return 0;
    size_t word = (size_t)code / PH_INPUT_WORD_BITS;
    if (word >= words)
        return 0;
    return (int)((bits[word] >> ((size_t)code % PH_INPUT_WORD_BITS)) & 1UL);
}

int ph_input_any_key_pressed(const unsigned long *state,
                             const unsigned long *capabilities,
                             size_t words)
{
    if (!state || !capabilities)
        return 1;
    for (size_t i = 0; i < words; i++)
        if (state[i] & capabilities[i])
            return 1;
    return 0;
}

int ph_input_abs_code_for_sdl_axis(const unsigned long *abs_bits, size_t words,
                                   int axis_index)
{
    if (!abs_bits || axis_index < 0)
        return -1;
    int index = 0;
    for (int code = 0; code <= ABS_MAX; code++) {
        if (code >= ABS_HAT0X && code <= ABS_HAT3Y)
            continue;
        if (!ph_input_test_bit(abs_bits, words, code))
            continue;
        if (index == axis_index)
            return code;
        index++;
    }
    return -1;
}

int ph_input_axis_is_centered(int value, int minimum, int maximum,
                              int flat, int fuzz)
{
    long long span = (long long)maximum - (long long)minimum;
    if (span < 16 || value < minimum || value > maximum)
        return 0;
    long long center = (long long)minimum + span / 2;
    long long tolerance = 1;
    if (flat > 0 && flat > tolerance)
        tolerance = flat;
    if (fuzz > 0 && fuzz > tolerance)
        tolerance = fuzz;
    /* A nonsensical kernel tolerance cannot be used as proof of neutral. */
    if (tolerance > span / 4)
        return 0;
    long long distance = (long long)value - center;
    if (distance < 0)
        distance = -distance;
    return distance <= tolerance;
}

int ph_input_hat_axis_is_centered(int value, int minimum, int maximum)
{
    long long span = (long long)maximum - (long long)minimum;
    if (span < 2 || (span & 1) || value < minimum || value > maximum)
        return 0;
    return (long long)value == (long long)minimum + span / 2;
}

int ph_input_guard_button_value(int raw, int snapshot_valid,
                                int physically_neutral, int binding_supported)
{
    raw = !!raw;
    if (raw && snapshot_valid && physically_neutral && binding_supported)
        return 0;
    return raw;
}

int ph_input_guard_axis_value(int raw, int snapshot_valid,
                              int physically_centered)
{
    if (raw && snapshot_valid && physically_centered)
        return 0;
    return raw;
}

unsigned ph_input_resolve_dpad(int *up, int *down, int *left, int *right)
{
    unsigned conflicts = PH_INPUT_DPAD_CONFLICT_NONE;
    if (!up || !down || !left || !right)
        return conflicts;
    if (*up && *down) {
        *up = 0;
        *down = 0;
        conflicts |= PH_INPUT_DPAD_CONFLICT_VERTICAL;
    }
    if (*left && *right) {
        *left = 0;
        *right = 0;
        conflicts |= PH_INPUT_DPAD_CONFLICT_HORIZONTAL;
    }
    return conflicts;
}
