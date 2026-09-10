#ifndef TIGHTROPE_INPUT_ADAPTER_H
#define TIGHTROPE_INPUT_ADAPTER_H

#include <stddef.h>

/* Build a conservative SDL mapping only for the two raw topologies already
 * exercised by the published Hitman GO/Tightrope family.  Device names, GUID
 * identities and CFW names never select a layout; the GUID is used solely as
 * SDL's mapping key. */
int tr_input_build_raw_mapping(char *output, size_t output_size,
                               const char *guid, int axes, int buttons,
                               int hats);

/* Pick the physical SELECT/START pair for the emergency read-only evdev
 * chord.  Real BTN_SELECT/BTN_START wins; the proven handheld fallback is
 * BTN_TRIGGER_HAPPY1/2. */
int tr_input_pick_exit_codes(const unsigned long *keybits,
                             size_t keybit_words, int *select_code,
                             int *start_code);

#define TR_LEFT_STICK_ENTER 23000
#define TR_LEFT_STICK_LEAVE 14000

struct tr_input_motion_state {
    int analog_left;
    int analog_right;
    int analog_jump;
};

struct tr_input_motion_result {
    int direction;
    int jump;
};

/* Resolve the touch-first gameplay controls.  D-pad horizontal input wins
 * over the analogue stick so drift cannot fight a deliberate precise step;
 * D-pad up and the south button both jump. */
void tr_input_resolve_motion(struct tr_input_motion_state *state,
                             int axis_x, int axis_y,
                             int dpad_left, int dpad_right, int dpad_up,
                             int jump_button,
                             struct tr_input_motion_result *result);

void tr_input_reset_motion(struct tr_input_motion_state *state);

/* MBS scene names are part of the accepted owner payload.  Only real level
 * scenes expose the fixed touch controls; menu and selector names must never
 * arm the D-pad finger path. */
int tr_input_scene_is_gameplay(const char *scene_name);

#endif
