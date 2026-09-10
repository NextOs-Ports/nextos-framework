#include "input_policy.h"
#include "nxinput_core.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static int16_t positive_axis(float value)
{
    return (int16_t)(value * (float)INT16_MAX);
}

static int near(float actual, float expected, float tolerance)
{
    return fabsf(actual - expected) <= tolerance;
}

static int16_t signed_axis(int sign)
{
    return sign < 0 ? INT16_MIN : INT16_MAX;
}

int main(void)
{
    /* SDL controller triggers use 0 for rest even when the physical mapping
     * is a digital button. This is the ROCKNIX b6/b7 regression case: rest
     * must remain exactly neutral instead of becoming 0.5. */
    assert(nxinput_core_trigger(INT16_MIN,
                                SC_INPUT_TRIGGER_DEADZONE) == 0.0f);
    assert(nxinput_core_trigger(0, SC_INPUT_TRIGGER_DEADZONE) == 0.0f);
    assert(nxinput_core_trigger(positive_axis(0.04f),
                                SC_INPUT_TRIGGER_DEADZONE) == 0.0f);
    assert(near(nxinput_core_trigger(INT16_MAX,
                                     SC_INPUT_TRIGGER_DEADZONE),
                1.0f, 0.00001f));

    nxinput_stick_filter filter = {0};
    float x = 1.0f;
    float y = 1.0f;

    /* A worn stick resting around 0.25 stays neutral. */
    nxinput_core_filter_stick(&filter, positive_axis(0.25f), 0,
                              SC_INPUT_STICK_ENTER_DEADZONE,
                              SC_INPUT_STICK_EXIT_DEADZONE, &x, &y);
    assert(filter.active == 0);
    assert(x == 0.0f && y == 0.0f);

    /* Deliberate movement engages; hysteresis keeps it stable until the stick
     * really returns below the 0.30 release threshold. */
    nxinput_core_filter_stick(&filter, positive_axis(0.41f), 0,
                              SC_INPUT_STICK_ENTER_DEADZONE,
                              SC_INPUT_STICK_EXIT_DEADZONE, &x, &y);
    assert(filter.active == 1 && x > 0.0f && y == 0.0f);
    nxinput_core_filter_stick(&filter, positive_axis(0.31f), 0,
                              SC_INPUT_STICK_ENTER_DEADZONE,
                              SC_INPUT_STICK_EXIT_DEADZONE, &x, &y);
    assert(filter.active == 1 && x > 0.0f);
    nxinput_core_filter_stick(&filter, positive_axis(0.29f), 0,
                              SC_INPUT_STICK_ENTER_DEADZONE,
                              SC_INPUT_STICK_EXIT_DEADZONE, &x, &y);
    assert(filter.active == 0);
    assert(x == 0.0f && y == 0.0f);

    /* The deadzone is radial: a diagonal whose components are each below the
     * engage threshold can still be an intentional movement by magnitude. */
    nxinput_core_filter_stick(&filter, positive_axis(0.30f),
                              positive_axis(0.30f),
                              SC_INPUT_STICK_ENTER_DEADZONE,
                              SC_INPUT_STICK_EXIT_DEADZONE, &x, &y);
    assert(filter.active == 1 && x > 0.0f && y > 0.0f);

    /* Full travel is quadrant-symmetric. In particular lower-left cannot be
     * reduced to a half press by the shared radial filter. */
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            filter = (nxinput_stick_filter){0};
            nxinput_core_filter_stick(&filter, signed_axis(sx),
                                      signed_axis(sy),
                                      SC_INPUT_STICK_ENTER_DEADZONE,
                                      SC_INPUT_STICK_EXIT_DEADZONE, &x, &y);
            assert(filter.active == 1);
            assert((x < 0.0f ? -1 : 1) == sx);
            assert((y < 0.0f ? -1 : 1) == sy);
            assert(near(sqrtf(x * x + y * y), 1.0f, 0.00001f));
            assert(near(fabsf(x), 0.70710678f, 0.00002f));
            assert(near(fabsf(y), 0.70710678f, 0.00002f));
        }
    }

    /* AXIS_HAT is an absolute level, not an edge. Every cardinal direction
     * is exactly full scale and every diagonal carries both full components.
     * This covers D-pad-only handhelds as well as controller hats. */
    static const struct {
        int up, down, left, right;
        float expected_x, expected_y;
    } hats[] = {
        { 1, 0, 0, 0,  0.0f, -1.0f },
        { 0, 1, 0, 0,  0.0f,  1.0f },
        { 0, 0, 1, 0, -1.0f,  0.0f },
        { 0, 0, 0, 1,  1.0f,  0.0f },
        { 1, 0, 1, 0, -1.0f, -1.0f },
        { 1, 0, 0, 1,  1.0f, -1.0f },
        { 0, 1, 1, 0, -1.0f,  1.0f }, /* lower-left report */
        { 0, 1, 0, 1,  1.0f,  1.0f },
    };
    for (size_t i = 0; i < sizeof hats / sizeof *hats; i++) {
        sc_input_dpad_axes(hats[i].up, hats[i].down,
                           hats[i].left, hats[i].right, &x, &y);
        assert(x == hats[i].expected_x);
        assert(y == hats[i].expected_y);
    }

    puts("SUZY CUBE INPUT POLICY UNIT: PASS");
    return 0;
}
