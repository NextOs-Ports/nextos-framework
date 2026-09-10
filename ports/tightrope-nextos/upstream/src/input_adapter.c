#include "input_adapter.h"

#include <ctype.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>

static int guid_is_hex32(const char *guid)
{
    if (!guid || strlen(guid) != 32)
        return 0;
    for (size_t i = 0; i < 32; i++)
        if (!isxdigit((unsigned char)guid[i]))
            return 0;
    return 1;
}

int tr_input_build_raw_mapping(char *output, size_t output_size,
                               const char *guid, int axes, int buttons,
                               int hats)
{
    const char *layout;
    int written;

    if (!output || output_size == 0 || !guid_is_hex32(guid) || axes < 4)
        return 0;

    /* Layout A: conventional USB/handheld pad, four axes, twelve buttons and
     * a hat.  Layout B: the proven RK3326/GO-Super topology where the D-pad is
     * b8..b11 and SELECT/START/L3/R3 follow it.  Near misses stay unmapped. */
    if (hats > 0 && buttons >= 12) {
        layout =
            "a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,"
            "back:b8,start:b9,leftstick:b10,rightstick:b11,"
            "leftx:a0,lefty:a1,rightx:a2,righty:a3,"
            "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,";
    } else if (hats == 0 && buttons >= 16) {
        layout =
            "a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,"
            "dpup:b8,dpdown:b9,dpleft:b10,dpright:b11,"
            "back:b12,start:b13,leftstick:b14,rightstick:b15,"
            "leftx:a0,lefty:a1,rightx:a2,righty:a3,";
    } else {
        return 0;
    }

    written = snprintf(output, output_size,
                       "%s,Tightrope raw fallback,platform:Linux,%s",
                       guid, layout);
    return written > 0 && (size_t)written < output_size;
}

static int key_is_set(const unsigned long *keybits, size_t keybit_words,
                      int code)
{
    const size_t bits_per_word = 8u * sizeof(unsigned long);
    size_t word;

    if (!keybits || code < 0)
        return 0;
    word = (size_t)code / bits_per_word;
    if (word >= keybit_words)
        return 0;
    return !!(keybits[word] &
              (1UL << ((size_t)code % bits_per_word)));
}

int tr_input_pick_exit_codes(const unsigned long *keybits,
                             size_t keybit_words, int *select_code,
                             int *start_code)
{
    int select = -1, start = -1;

    if (!select_code || !start_code ||
        (!key_is_set(keybits, keybit_words, BTN_GAMEPAD) &&
         !key_is_set(keybits, keybit_words, BTN_JOYSTICK)))
        return 0;

    if (key_is_set(keybits, keybit_words, BTN_SELECT) &&
        key_is_set(keybits, keybit_words, BTN_START)) {
        select = BTN_SELECT;
        start = BTN_START;
    } else if (key_is_set(keybits, keybit_words, BTN_TRIGGER_HAPPY1) &&
               key_is_set(keybits, keybit_words, BTN_TRIGGER_HAPPY2)) {
        select = BTN_TRIGGER_HAPPY1;
        start = BTN_TRIGGER_HAPPY2;
    } else {
        return 0;
    }

    *select_code = select;
    *start_code = start;
    return 1;
}

void tr_input_resolve_motion(struct tr_input_motion_state *state,
                             int axis_x, int axis_y,
                             int dpad_left, int dpad_right, int dpad_up,
                             int jump_button,
                             struct tr_input_motion_result *result)
{
    int left, right;

    if (!state || !result)
        return;

    state->analog_left = state->analog_left
        ? axis_x < -TR_LEFT_STICK_LEAVE
        : axis_x < -TR_LEFT_STICK_ENTER;
    state->analog_right = state->analog_right
        ? axis_x > TR_LEFT_STICK_LEAVE
        : axis_x > TR_LEFT_STICK_ENTER;
    state->analog_jump = state->analog_jump
        ? axis_y < -TR_LEFT_STICK_LEAVE
        : axis_y < -TR_LEFT_STICK_ENTER;

    /* A deliberate D-pad direction owns this sample.  Opposite directions
     * cancel cleanly; when neither is held the analogue hysteresis applies. */
    if (dpad_left || dpad_right) {
        left = !!dpad_left;
        right = !!dpad_right;
    } else {
        left = state->analog_left;
        right = state->analog_right;
    }
    result->direction = left == right ? 0 : left ? -1 : 1;
    result->jump = !!(jump_button || dpad_up || state->analog_jump);
}

void tr_input_reset_motion(struct tr_input_motion_state *state)
{
    if (state)
        memset(state, 0, sizeof *state);
}

int tr_input_scene_is_gameplay(const char *scene_name)
{
    return scene_name && strncmp(scene_name, "Level", 5) == 0 &&
           isdigit((unsigned char)scene_name[5]);
}
