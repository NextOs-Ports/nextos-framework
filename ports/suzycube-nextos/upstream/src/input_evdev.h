#ifndef SUZYCUBE_INPUT_EVDEV_H
#define SUZYCUBE_INPUT_EVDEV_H

#include <linux/input.h>
#include <stddef.h>
#include <stdint.h>

#define SC_EVDEV_WORD_BITS (8U * sizeof(unsigned long))
#define SC_EVDEV_KEY_WORDS \
    ((KEY_MAX + 1U + SC_EVDEV_WORD_BITS - 1U) / SC_EVDEV_WORD_BITS)
#define SC_EVDEV_ABS_WORDS \
    ((ABS_MAX + 1U + SC_EVDEV_WORD_BITS - 1U) / SC_EVDEV_WORD_BITS)

enum sc_evdev_face_mapping {
    SC_EVDEV_FACE_UNKNOWN = 0,
    SC_EVDEV_FACE_XBOX_POSITIONAL,
    SC_EVDEV_FACE_NINTENDO_LABELS,
};

/* Mirror of the first 15 SDL_GameControllerButton values, so the pure evdev
 * helpers stay SDL-free.  input.c static-asserts the equivalence. */
enum {
    SC_PAD_A = 0, SC_PAD_B = 1, SC_PAD_X = 2, SC_PAD_Y = 3,
    SC_PAD_BACK = 4, SC_PAD_GUIDE = 5, SC_PAD_START = 6,
    SC_PAD_LEFTSTICK = 7, SC_PAD_RIGHTSTICK = 8,
    SC_PAD_LEFTSHOULDER = 9, SC_PAD_RIGHTSHOULDER = 10,
    SC_PAD_DPAD_UP = 11, SC_PAD_DPAD_DOWN = 12,
    SC_PAD_DPAD_LEFT = 13, SC_PAD_DPAD_RIGHT = 14,
    SC_PAD_COUNT = 15,
};

int sc_evdev_test_bit(const unsigned long *bits, size_t words, int code);
int sc_evdev_sdl_button_index(const unsigned long *bits, size_t words,
                              int code);
enum sc_evdev_face_mapping sc_evdev_classify_face_mapping(
    const unsigned long *bits, size_t words,
    int bind_a, int bind_b, int bind_x, int bind_y);
int sc_evdev_abs_code_for_sdl_axis(const unsigned long *abs_bits, size_t words,
                                   int axis_index);
int sc_evdev_any_key_pressed(const unsigned long *state,
                             const unsigned long *capabilities, size_t words);
int sc_evdev_axis_is_analog(int minimum, int maximum);

#endif
