#include "input_adapter.h"

#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORDS ((KEY_MAX + 1 + 8 * sizeof(unsigned long) - 1) / \
               (8 * sizeof(unsigned long)))

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static void set_key(unsigned long *bits, int code)
{
    bits[code / (8 * sizeof(unsigned long))] |=
        1UL << (code % (8 * sizeof(unsigned long)));
}

int main(void)
{
    const char *guid = "03000000100800000100000010010000";
    char mapping[1024];
    unsigned long bits[WORDS];
    int select = -1, start = -1;
    struct tr_input_motion_state motion = {0};
    struct tr_input_motion_result result = {0};

    if (!tr_input_build_raw_mapping(mapping, sizeof mapping, guid, 4, 12, 1))
        fail("standard topology rejected");
    if (!strstr(mapping, "back:b8,start:b9") ||
        !strstr(mapping, "rightstick:b11") ||
        !strstr(mapping, "rightx:a2,righty:a3") ||
        !strstr(mapping, "dpup:h0.1"))
        fail("standard topology mapping changed");

    if (!tr_input_build_raw_mapping(mapping, sizeof mapping, guid, 4, 17, 0))
        fail("button-dpad handheld topology rejected");
    if (!strstr(mapping, "dpup:b8,dpdown:b9") ||
        !strstr(mapping, "back:b12,start:b13") ||
        !strstr(mapping, "rightstick:b15"))
        fail("button-dpad topology mapping changed");

    if (tr_input_build_raw_mapping(mapping, sizeof mapping, guid, 3, 17, 0) ||
        tr_input_build_raw_mapping(mapping, sizeof mapping, guid, 4, 11, 0) ||
        tr_input_build_raw_mapping(mapping, sizeof mapping, "bad-guid", 4, 12, 1))
        fail("near-miss topology was guessed");

    memset(bits, 0, sizeof bits);
    set_key(bits, BTN_GAMEPAD);
    set_key(bits, BTN_SELECT);
    set_key(bits, BTN_START);
    set_key(bits, BTN_TRIGGER_HAPPY1);
    set_key(bits, BTN_TRIGGER_HAPPY2);
    if (!tr_input_pick_exit_codes(bits, WORDS, &select, &start) ||
        select != BTN_SELECT || start != BTN_START)
        fail("real SELECT/START did not win");

    memset(bits, 0, sizeof bits);
    set_key(bits, BTN_GAMEPAD);
    set_key(bits, BTN_TRIGGER_HAPPY1);
    set_key(bits, BTN_TRIGGER_HAPPY2);
    if (!tr_input_pick_exit_codes(bits, WORDS, &select, &start) ||
        select != BTN_TRIGGER_HAPPY1 || start != BTN_TRIGGER_HAPPY2)
        fail("trigger-happy exit fallback rejected");

    memset(bits, 0, sizeof bits);
    set_key(bits, BTN_GAMEPAD);
    set_key(bits, BTN_TRIGGER_HAPPY1);
    if (tr_input_pick_exit_codes(bits, WORDS, &select, &start))
        fail("half an exit chord was accepted");

    tr_input_resolve_motion(&motion, TR_LEFT_STICK_ENTER - 1, 0,
                            0, 0, 0, 0, &result);
    if (result.direction || result.jump)
        fail("left stick entered before the less-sensitive threshold");
    tr_input_resolve_motion(&motion, TR_LEFT_STICK_ENTER + 1, 0,
                            0, 0, 0, 0, &result);
    if (result.direction != 1)
        fail("left stick full direction was rejected");
    tr_input_resolve_motion(&motion, TR_LEFT_STICK_LEAVE + 1, 0,
                            0, 0, 0, 0, &result);
    if (result.direction != 1)
        fail("left stick hysteresis did not hold");
    tr_input_resolve_motion(&motion, TR_LEFT_STICK_LEAVE - 1, 0,
                            0, 0, 0, 0, &result);
    if (result.direction)
        fail("left stick did not release near centre");

    tr_input_resolve_motion(&motion, 32767, 0, 1, 0, 1, 0, &result);
    if (result.direction != -1 || !result.jump)
        fail("D-pad diagonal did not override analogue drift");
    tr_input_resolve_motion(&motion, 0, 0, 1, 1, 0, 0, &result);
    if (result.direction)
        fail("opposite D-pad directions did not cancel");
    tr_input_resolve_motion(&motion, 0, 0, 0, 0, 0, 1, &result);
    if (result.direction || !result.jump)
        fail("south-button jump mapping changed");
    tr_input_reset_motion(&motion);
    tr_input_resolve_motion(&motion, TR_LEFT_STICK_LEAVE + 1, 0,
                            0, 0, 0, 0, &result);
    if (result.direction)
        fail("motion reset retained analogue hysteresis");

    if (!tr_input_scene_is_gameplay("Level1") ||
        !tr_input_scene_is_gameplay("Level100 Copy") ||
        tr_input_scene_is_gameplay("Level Select1") ||
        tr_input_scene_is_gameplay("Title Screen") ||
        tr_input_scene_is_gameplay("Intro0") ||
        tr_input_scene_is_gameplay(NULL))
        fail("scene gameplay classifier changed");

    puts("input adapter contract: OK");
    return 0;
}
