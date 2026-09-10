#ifndef ASM2_INPUT_H
#define ASM2_INPUT_H

#include <SDL2/SDL.h>

/*
 * NativeBridgeHIDControllers event identifiers used by the 1.2.7d APK.
 * Axis/trigger values are continuous doubles; buttons use 0.0/1.0.
 */
enum asm2_hid_event_id {
  ASM2_HID_LEFT_TRIGGER = 1,
  ASM2_HID_RIGHT_TRIGGER = 2,
  ASM2_HID_LEFT_X = 3,
  ASM2_HID_LEFT_Y = 4,
  ASM2_HID_RIGHT_X = 5,
  ASM2_HID_RIGHT_Y = 6,
  ASM2_HID_DPAD_UP = 7,
  ASM2_HID_DPAD_DOWN = 8,
  ASM2_HID_DPAD_LEFT = 9,
  ASM2_HID_DPAD_RIGHT = 10,
  ASM2_HID_LEFT_SHOULDER = 11,
  ASM2_HID_RIGHT_SHOULDER = 12,
  ASM2_HID_Y = 13,
  ASM2_HID_A = 14,
  ASM2_HID_X = 15,
  ASM2_HID_B = 16,
  ASM2_HID_START = 17,
  ASM2_HID_SELECT = 18,
  ASM2_HID_BACK = 19,
  ASM2_HID_LEFT_STICK = 20,
  ASM2_HID_RIGHT_STICK = 21,
};

enum asm2_touch_action {
  ASM2_TOUCH_UP = 0,
  ASM2_TOUCH_DOWN = 1,
  ASM2_TOUCH_MOVE = 2,
};

struct asm2_input_callbacks {
  void (*controller_connected)(const char *name, void *userdata);
  void (*controller_disconnected)(void *userdata);
  void (*hid)(int event_id, double value, void *userdata);
  void (*key)(int android_keycode, int pressed, void *userdata);
  void (*touch)(int action, int x, int y, int pointer_id, void *userdata);
  void *userdata;
};

#define ASM2_INPUT_MAX_FINGERS 10

struct asm2_touch_slot {
  SDL_FingerID finger_id;
  int active;
  float x;
  float y;
};

struct asm2_input {
  SDL_GameController *controller;
  SDL_JoystickID instance_id;
  struct asm2_input_callbacks callbacks;
  const char *first_run_touch_state_path;
  double hid_values[ASM2_HID_RIGHT_STICK + 1];
  unsigned int hid_sent;
  int drawable_width;
  int drawable_height;
  int first_run_touch_stage;
  int first_run_touch_active;
  int first_run_touch_button;
  int first_run_touch_x;
  int first_run_touch_y;
  int select_pressed;
  int start_pressed;
  int mouse_pressed;
  int mouse_x;
  int mouse_y;
  unsigned char key_down[128];
  struct asm2_touch_slot fingers[ASM2_INPUT_MAX_FINGERS];
};

void asm2_input_init(struct asm2_input *input,
                     const struct asm2_input_callbacks *callbacks,
                     int drawable_width, int drawable_height,
                     const char *first_run_touch_state_path,
                     int first_run_touch_stage);

/* Returns one while the game should continue, zero on quit/Select+Start. */
int asm2_input_pump(struct asm2_input *input);

/* Releases active inputs, reports controller removal, then closes SDL handles. */
void asm2_input_shutdown(struct asm2_input *input);

#endif
