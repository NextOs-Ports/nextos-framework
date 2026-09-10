#include "input.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "first_accept.h"
#include "util.h"

enum {
  ANDROID_KEY_BACK = 4,
  ANDROID_KEY_DPAD_UP = 19,
  ANDROID_KEY_DPAD_DOWN = 20,
  ANDROID_KEY_DPAD_LEFT = 21,
  ANDROID_KEY_DPAD_RIGHT = 22,
  ANDROID_KEY_ENTER = 66,
  ANDROID_KEY_BUTTON_A = 96,
  ANDROID_KEY_BUTTON_B = 97,
  ANDROID_KEY_BUTTON_X = 99,
  ANDROID_KEY_BUTTON_Y = 100,
  ANDROID_KEY_BUTTON_L1 = 102,
  ANDROID_KEY_BUTTON_R1 = 103,
  ANDROID_KEY_BUTTON_L2 = 104,
  ANDROID_KEY_BUTTON_R2 = 105,
  ANDROID_KEY_BUTTON_THUMBL = 106,
  ANDROID_KEY_BUTTON_THUMBR = 107,
  ANDROID_KEY_BUTTON_START = 108,
  ANDROID_KEY_BUTTON_SELECT = 109,
  ANDROID_KEY_BUTTON_MODE = 110,
};

static void finish_first_run_touch(struct asm2_input *input);

static void send_hid(struct asm2_input *input, int event_id, double value) {
  if (!input || event_id < ASM2_HID_LEFT_TRIGGER ||
      event_id > ASM2_HID_RIGHT_STICK)
    return;

  unsigned int bit = 1u << event_id;
  if ((input->hid_sent & bit) != 0 &&
      fabs(input->hid_values[event_id] - value) < 0.000001)
    return;

  input->hid_values[event_id] = value;
  input->hid_sent |= bit;
  if (input->callbacks.hid)
    input->callbacks.hid(event_id, value, input->callbacks.userdata);
}

static void release_controller_state(struct asm2_input *input) {
  if (!input)
    return;
  finish_first_run_touch(input);
  for (int event_id = ASM2_HID_LEFT_TRIGGER;
       event_id <= ASM2_HID_RIGHT_STICK; ++event_id) {
    unsigned int bit = 1u << event_id;
    if ((input->hid_sent & bit) != 0 && input->hid_values[event_id] != 0.0) {
      input->hid_values[event_id] = 0.0;
      if (input->callbacks.hid)
        input->callbacks.hid(event_id, 0.0, input->callbacks.userdata);
    }
  }
  memset(input->hid_values, 0, sizeof(input->hid_values));
  input->hid_sent = 0;
  input->select_pressed = 0;
  input->start_pressed = 0;
}

static void close_controller(struct asm2_input *input, int notify) {
  if (!input || !input->controller)
    return;

  release_controller_state(input);
  if (notify && input->callbacks.controller_disconnected)
    input->callbacks.controller_disconnected(input->callbacks.userdata);
  SDL_GameControllerClose(input->controller);
  input->controller = NULL;
  input->instance_id = -1;
}

static void open_controller(struct asm2_input *input, int device_index) {
  if (!input || input->controller || device_index < 0 ||
      !SDL_IsGameController(device_index))
    return;

  SDL_GameController *controller = SDL_GameControllerOpen(device_index);
  if (!controller)
    return;
  SDL_Joystick *joystick = SDL_GameControllerGetJoystick(controller);
  SDL_JoystickID instance_id = SDL_JoystickInstanceID(joystick);
  if (instance_id < 0) {
    SDL_GameControllerClose(controller);
    return;
  }

  input->controller = controller;
  input->instance_id = instance_id;
  const char *name = SDL_GameControllerName(controller);
  if (!name || !name[0])
    name = "SDL Game Controller";
  debugPrintf("ASM2_INPUT controller connected name=%s id=%d\n", name,
              input->instance_id);
  if (input->callbacks.controller_connected)
    input->callbacks.controller_connected(name, input->callbacks.userdata);
}

static void open_first_controller(struct asm2_input *input) {
  if (!input || input->controller)
    return;
  int joystick_count = SDL_NumJoysticks();
  for (int index = 0; index < joystick_count; ++index) {
    open_controller(input, index);
    if (input->controller)
      break;
  }
}

static int hid_button(SDL_GameControllerButton button) {
  switch (button) {
  case SDL_CONTROLLER_BUTTON_A: return ASM2_HID_A;
  case SDL_CONTROLLER_BUTTON_B: return ASM2_HID_B;
  case SDL_CONTROLLER_BUTTON_X: return ASM2_HID_X;
  case SDL_CONTROLLER_BUTTON_Y: return ASM2_HID_Y;
  case SDL_CONTROLLER_BUTTON_BACK: return ASM2_HID_SELECT;
  case SDL_CONTROLLER_BUTTON_GUIDE: return ASM2_HID_BACK;
  case SDL_CONTROLLER_BUTTON_START: return ASM2_HID_START;
  case SDL_CONTROLLER_BUTTON_LEFTSTICK: return ASM2_HID_LEFT_STICK;
  case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return ASM2_HID_RIGHT_STICK;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
    return ASM2_HID_LEFT_SHOULDER;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
    return ASM2_HID_RIGHT_SHOULDER;
  case SDL_CONTROLLER_BUTTON_DPAD_UP: return ASM2_HID_DPAD_UP;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return ASM2_HID_DPAD_DOWN;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return ASM2_HID_DPAD_LEFT;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return ASM2_HID_DPAD_RIGHT;
  default: return 0;
  }
}

static double normalized_stick_axis(Sint16 raw) {
  const double deadzone = 0.12;
  double value = raw < 0 ? (double)raw / 32768.0 : (double)raw / 32767.0;
  if (fabs(value) <= deadzone)
    return 0.0;
  /* StandardHIDController forwards the raw axis outside flat+fuzz. */
  return value;
}

static double normalized_trigger_axis(Sint16 raw) {
  if (raw <= 0)
    return 0.0;
  return (double)raw / 32767.0;
}

static void send_key(struct asm2_input *input, int key, int pressed) {
  if (!input || key <= 0 || key >= (int)sizeof(input->key_down))
    return;
  pressed = pressed != 0;
  if (input->key_down[key] == pressed)
    return;
  input->key_down[key] = (unsigned char)pressed;
  if (input->callbacks.key)
    input->callbacks.key(key, pressed, input->callbacks.userdata);
}

static void release_keyboard_state(struct asm2_input *input) {
  if (!input)
    return;
  for (int key = 1; key < (int)sizeof(input->key_down); ++key) {
    if (!input->key_down[key])
      continue;
    input->key_down[key] = 0;
    if (input->callbacks.key)
      input->callbacks.key(key, 0, input->callbacks.userdata);
  }
}

static int keyboard_android_key(SDL_Keycode key) {
  switch (key) {
  case SDLK_RETURN: return ANDROID_KEY_ENTER;
  case SDLK_ESCAPE:
  case SDLK_BACKSPACE: return ANDROID_KEY_BACK;
  case SDLK_UP: return ANDROID_KEY_DPAD_UP;
  case SDLK_DOWN: return ANDROID_KEY_DPAD_DOWN;
  case SDLK_LEFT: return ANDROID_KEY_DPAD_LEFT;
  case SDLK_RIGHT: return ANDROID_KEY_DPAD_RIGHT;
  case SDLK_z: return ANDROID_KEY_BUTTON_A;
  case SDLK_x: return ANDROID_KEY_BUTTON_B;
  case SDLK_a: return ANDROID_KEY_BUTTON_X;
  case SDLK_s: return ANDROID_KEY_BUTTON_Y;
  case SDLK_q: return ANDROID_KEY_BUTTON_L1;
  case SDLK_w: return ANDROID_KEY_BUTTON_R1;
  case SDLK_1: return ANDROID_KEY_BUTTON_L2;
  case SDLK_2: return ANDROID_KEY_BUTTON_R2;
  case SDLK_LSHIFT: return ANDROID_KEY_BUTTON_THUMBL;
  case SDLK_RSHIFT: return ANDROID_KEY_BUTTON_THUMBR;
  case SDLK_TAB: return ANDROID_KEY_BUTTON_SELECT;
  case SDLK_SPACE: return ANDROID_KEY_BUTTON_START;
  case SDLK_HOME: return ANDROID_KEY_BUTTON_MODE;
  default: return 0;
  }
}

static int touch_slot_for_finger(const struct asm2_input *input,
                                 SDL_FingerID finger_id) {
  for (int slot = 0; slot < ASM2_INPUT_MAX_FINGERS; ++slot) {
    if (input->fingers[slot].active &&
        input->fingers[slot].finger_id == finger_id)
      return slot;
  }
  return -1;
}

static int allocate_touch_slot(struct asm2_input *input,
                               SDL_FingerID finger_id) {
  int existing = touch_slot_for_finger(input, finger_id);
  if (existing >= 0)
    return existing;
  for (int slot = 0; slot < ASM2_INPUT_MAX_FINGERS; ++slot) {
    if (!input->fingers[slot].active) {
      input->fingers[slot].finger_id = finger_id;
      input->fingers[slot].active = 1;
      return slot;
    }
  }
  return -1;
}

static int active_touch_count(const struct asm2_input *input) {
  int count = 0;
  for (int slot = 0; slot < ASM2_INPUT_MAX_FINGERS; ++slot)
    count += input->fingers[slot].active != 0;
  return count;
}

static int pixel_coordinate(float normalized, int extent) {
  if (extent <= 0)
    return 0;
  if (normalized < 0.0f)
    normalized = 0.0f;
  if (normalized > 1.0f)
    normalized = 1.0f;
  int pixel = (int)(normalized * (float)extent);
  return pixel < extent ? pixel : extent - 1;
}

static void send_touch(struct asm2_input *input, int action, float x, float y,
                       int pointer_id) {
  if (!input->callbacks.touch)
    return;
  input->callbacks.touch(action,
                         pixel_coordinate(x, input->drawable_width),
                         pixel_coordinate(y, input->drawable_height),
                         pointer_id, input->callbacks.userdata);
}

static int clamp_pixel_coordinate(int coordinate, int extent) {
  if (extent <= 0 || coordinate < 0)
    return 0;
  return coordinate < extent ? coordinate : extent - 1;
}

static void send_touch_pixels(struct asm2_input *input, int action, int x,
                              int y, int pointer_id) {
  if (!input->callbacks.touch)
    return;
  input->callbacks.touch(
      action, clamp_pixel_coordinate(x, input->drawable_width),
      clamp_pixel_coordinate(y, input->drawable_height), pointer_id,
      input->callbacks.userdata);
}

static void send_touch_end_sentinel(struct asm2_input *input) {
  if (input->callbacks.touch)
    input->callbacks.touch(ASM2_TOUCH_UP, -2000, -2000, 0,
                           input->callbacks.userdata);
}

static int is_first_run_touch_button(SDL_GameControllerButton button) {
  return button == SDL_CONTROLLER_BUTTON_A ||
         button == SDL_CONTROLLER_BUTTON_B ||
         button == SDL_CONTROLLER_BUTTON_X ||
         button == SDL_CONTROLLER_BUTTON_Y;
}

static void persist_first_run_touch_state(struct asm2_input *input) {
  if (!input || !input->first_run_touch_state_path ||
      !input->first_run_touch_state_path[0])
    return;

  char temporary_path[4096];
  int path_length = snprintf(temporary_path, sizeof(temporary_path), "%s.tmp",
                             input->first_run_touch_state_path);
  if (path_length < 0 || (size_t)path_length >= sizeof(temporary_path)) {
    debugPrintf("ASM2_INPUT first-run state path too long\n");
    return;
  }

  int descriptor = open(temporary_path,
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (descriptor < 0) {
    debugPrintf("ASM2_INPUT first-run state failed path=%s errno=%d\n",
                input->first_run_touch_state_path, errno);
    return;
  }

  char value[32];
  int value_length = snprintf(value, sizeof(value), "next=%d\n",
                              input->first_run_touch_stage);
  ssize_t written = write(descriptor, value, (size_t)value_length);
  int saved_errno = 0;
  if (written != value_length)
    saved_errno = written < 0 ? errno : EIO;
  if (saved_errno == 0 && fsync(descriptor) != 0)
    saved_errno = errno;
  if (close(descriptor) != 0 && saved_errno == 0)
    saved_errno = errno;
  if (saved_errno == 0 &&
      rename(temporary_path, input->first_run_touch_state_path) != 0)
    saved_errno = errno;
  if (saved_errno != 0) {
    unlink(temporary_path);
    debugPrintf("ASM2_INPUT first-run state failed path=%s errno=%d\n",
                input->first_run_touch_state_path, saved_errno);
    return;
  }
  debugPrintf("ASM2_INPUT first-run state saved next=%d\n",
              input->first_run_touch_stage);
}

static void begin_first_run_touch(struct asm2_input *input,
                                  SDL_GameControllerButton button) {
  input->first_run_touch_active = 1;
  input->first_run_touch_button = (int)button;
  if (input->first_run_touch_stage == ASM2_FIRST_RUN_TOUCH_UPDATE_LOG) {
    /* The plastic face-button labels and SDL's logical A/B mapping differ on
     * Nintendo-style handhelds. This modal expects Android's logical A, so
     * keep accepting any face button as the recovery trigger but emit the
     * same deterministic confirmation action on every controller layout. */
    debugPrintf(
        "ASM2_INPUT first-run hid down stage=%d button=%d logical=%d\n",
        input->first_run_touch_stage, (int)button, ASM2_HID_A);
    send_hid(input, ASM2_HID_A, 1.0);
    return;
  }
  asm2_first_run_touch_position(
      input->first_run_touch_stage, input->drawable_width,
      input->drawable_height, &input->first_run_touch_x,
      &input->first_run_touch_y);
  debugPrintf("ASM2_INPUT first-run touch down stage=%d button=%d x=%d y=%d\n",
              input->first_run_touch_stage, (int)button,
              input->first_run_touch_x, input->first_run_touch_y);
  send_touch_pixels(input, ASM2_TOUCH_DOWN, input->first_run_touch_x,
                    input->first_run_touch_y, 0);
}

static void finish_first_run_touch(struct asm2_input *input) {
  if (!input || !input->first_run_touch_active)
    return;
  if (input->first_run_touch_stage == ASM2_FIRST_RUN_TOUCH_UPDATE_LOG) {
    send_hid(input, ASM2_HID_A, 0.0);
  } else {
    send_touch_pixels(input, ASM2_TOUCH_UP, input->first_run_touch_x,
                      input->first_run_touch_y, 0);
    send_touch_end_sentinel(input);
  }
  input->first_run_touch_active = 0;
  if (input->first_run_touch_stage < ASM2_FIRST_RUN_TOUCH_COMPLETE)
    ++input->first_run_touch_stage;
  input->first_run_touch_button = SDL_CONTROLLER_BUTTON_INVALID;
  persist_first_run_touch_state(input);
  debugPrintf("ASM2_INPUT first-run touch up next=%d\n",
              input->first_run_touch_stage);
}

static int mouse_drawable_coordinate(struct asm2_input *input,
                                     Uint32 window_id, int coordinate,
                                     int horizontal) {
  int drawable_extent =
      horizontal ? input->drawable_width : input->drawable_height;
  SDL_Window *window = SDL_GetWindowFromID(window_id);
  int window_width = input->drawable_width;
  int window_height = input->drawable_height;
  if (window)
    SDL_GetWindowSize(window, &window_width, &window_height);
  int window_extent = horizontal ? window_width : window_height;
  if (window_extent > 0 && window_extent != drawable_extent) {
    double scaled =
        (double)coordinate * (double)drawable_extent / (double)window_extent;
    coordinate = (int)scaled;
  }
  return clamp_pixel_coordinate(coordinate, drawable_extent);
}

static void handle_mouse_button(struct asm2_input *input,
                                const SDL_MouseButtonEvent *button,
                                Uint32 event_type) {
  if (button->which == SDL_TOUCH_MOUSEID || button->button != SDL_BUTTON_LEFT)
    return;

  int x = mouse_drawable_coordinate(input, button->windowID, button->x, 1);
  int y = mouse_drawable_coordinate(input, button->windowID, button->y, 0);
  input->mouse_x = x;
  input->mouse_y = y;

  if (event_type == SDL_MOUSEBUTTONDOWN) {
    if (input->mouse_pressed)
      return;
    input->mouse_pressed = 1;
    send_touch_pixels(input, ASM2_TOUCH_DOWN, x, y, 0);
  } else if (input->mouse_pressed) {
    send_touch_pixels(input, ASM2_TOUCH_UP, x, y, 0);
    input->mouse_pressed = 0;
    send_touch_end_sentinel(input);
  }
}

static void handle_mouse_motion(struct asm2_input *input,
                                const SDL_MouseMotionEvent *motion) {
  if (motion->which == SDL_TOUCH_MOUSEID || !input->mouse_pressed)
    return;
  int x = mouse_drawable_coordinate(input, motion->windowID, motion->x, 1);
  int y = mouse_drawable_coordinate(input, motion->windowID, motion->y, 0);
  input->mouse_x = x;
  input->mouse_y = y;
  send_touch_pixels(input, ASM2_TOUCH_MOVE, x, y, 0);
}

static void release_mouse_touch(struct asm2_input *input) {
  if (!input->mouse_pressed)
    return;
  send_touch_pixels(input, ASM2_TOUCH_UP, input->mouse_x, input->mouse_y, 0);
  input->mouse_pressed = 0;
  send_touch_end_sentinel(input);
}

static void release_all_touches(struct asm2_input *input) {
  int had_touches = active_touch_count(input) != 0;
  for (int slot = 0; slot < ASM2_INPUT_MAX_FINGERS; ++slot) {
    if (!input->fingers[slot].active)
      continue;
    send_touch(input, ASM2_TOUCH_UP, input->fingers[slot].x,
               input->fingers[slot].y, slot);
    input->fingers[slot].active = 0;
  }
  if (had_touches)
    send_touch_end_sentinel(input);
}

static void handle_finger_event(struct asm2_input *input,
                                const SDL_TouchFingerEvent *finger,
                                Uint32 event_type) {
  int slot = touch_slot_for_finger(input, finger->fingerId);
  if (event_type == SDL_FINGERDOWN)
    slot = allocate_touch_slot(input, finger->fingerId);
  if (slot < 0)
    return;

  input->fingers[slot].x = finger->x;
  input->fingers[slot].y = finger->y;

  int action = ASM2_TOUCH_MOVE;
  if (event_type == SDL_FINGERDOWN)
    action = ASM2_TOUCH_DOWN;
  else if (event_type == SDL_FINGERUP)
    action = ASM2_TOUCH_UP;
  send_touch(input, action, finger->x, finger->y, slot);

  if (event_type == SDL_FINGERUP) {
    input->fingers[slot].active = 0;
    /* The Java 1.2.7d path clears its last pointer with this sentinel. */
    if (active_touch_count(input) == 0)
      send_touch_end_sentinel(input);
  }
}

void asm2_input_init(struct asm2_input *input,
                     const struct asm2_input_callbacks *callbacks,
                     int drawable_width, int drawable_height,
                     const char *first_run_touch_state_path,
                     int first_run_touch_stage) {
  if (!input)
    return;
  memset(input, 0, sizeof(*input));
  input->instance_id = -1;
  input->first_run_touch_button = SDL_CONTROLLER_BUTTON_INVALID;
  input->first_run_touch_state_path = first_run_touch_state_path;
  input->first_run_touch_stage = first_run_touch_stage;
  if (!first_run_touch_state_path || !first_run_touch_state_path[0] ||
      first_run_touch_stage < ASM2_FIRST_RUN_TOUCH_LEGAL ||
      first_run_touch_stage > ASM2_FIRST_RUN_TOUCH_COMPLETE)
    input->first_run_touch_stage = ASM2_FIRST_RUN_TOUCH_COMPLETE;
  input->drawable_width = drawable_width > 0 ? drawable_width : 1;
  input->drawable_height = drawable_height > 0 ? drawable_height : 1;
  if (callbacks)
    input->callbacks = *callbacks;
  if (input->first_run_touch_stage < ASM2_FIRST_RUN_TOUCH_COMPLETE)
    debugPrintf("ASM2_INPUT first-run touch armed next=%d\n",
                input->first_run_touch_stage);

  SDL_GameControllerEventState(SDL_ENABLE);
  SDL_JoystickEventState(SDL_ENABLE);
  open_first_controller(input);
}

int asm2_input_pump(struct asm2_input *input) {
  if (!input)
    return 0;

  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
    case SDL_QUIT:
      return 0;

    case SDL_CONTROLLERDEVICEADDED:
      open_controller(input, event.cdevice.which);
      break;

    case SDL_CONTROLLERDEVICEREMOVED:
      if (event.cdevice.which == input->instance_id) {
        debugPrintf("ASM2_INPUT controller disconnected id=%d\n",
                    input->instance_id);
        close_controller(input, 1);
        open_first_controller(input);
      }
      break;

    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
      if (!input->controller || event.cbutton.which != input->instance_id)
        break;
      int pressed = event.type == SDL_CONTROLLERBUTTONDOWN;
      SDL_GameControllerButton button = event.cbutton.button;
      int event_id = hid_button(button);
      if (input->first_run_touch_active &&
          (int)button == input->first_run_touch_button) {
        if (!pressed)
          finish_first_run_touch(input);
        break;
      }
      if (pressed &&
          input->first_run_touch_stage < ASM2_FIRST_RUN_TOUCH_COMPLETE &&
          is_first_run_touch_button(button)) {
        begin_first_run_touch(input, button);
        break;
      }
      send_hid(input, event_id, pressed ? 1.0 : 0.0);
      if (event_id == ASM2_HID_SELECT)
        input->select_pressed = pressed;
      else if (event_id == ASM2_HID_START)
        input->start_pressed = pressed;
      if (input->select_pressed && input->start_pressed)
        return 0;
      break;
    }

    case SDL_CONTROLLERAXISMOTION: {
      if (!input->controller || event.caxis.which != input->instance_id)
        break;
      int event_id = 0;
      double value = 0.0;
      switch (event.caxis.axis) {
      case SDL_CONTROLLER_AXIS_LEFTX:
        event_id = ASM2_HID_LEFT_X;
        value = normalized_stick_axis(event.caxis.value);
        break;
      case SDL_CONTROLLER_AXIS_LEFTY:
        event_id = ASM2_HID_LEFT_Y;
        value = normalized_stick_axis(event.caxis.value);
        break;
      case SDL_CONTROLLER_AXIS_RIGHTX:
        event_id = ASM2_HID_RIGHT_X;
        value = normalized_stick_axis(event.caxis.value);
        break;
      case SDL_CONTROLLER_AXIS_RIGHTY:
        event_id = ASM2_HID_RIGHT_Y;
        value = normalized_stick_axis(event.caxis.value);
        break;
      case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
        event_id = ASM2_HID_LEFT_TRIGGER;
        value = normalized_trigger_axis(event.caxis.value);
        break;
      case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
        event_id = ASM2_HID_RIGHT_TRIGGER;
        value = normalized_trigger_axis(event.caxis.value);
        break;
      default:
        break;
      }
      send_hid(input, event_id, value);
      break;
    }

    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION:
    case SDL_FINGERUP:
      handle_finger_event(input, &event.tfinger, event.type);
      break;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
      handle_mouse_button(input, &event.button, event.type);
      break;

    case SDL_MOUSEMOTION:
      handle_mouse_motion(input, &event.motion);
      break;

    case SDL_KEYDOWN:
    case SDL_KEYUP:
      if (!event.key.repeat)
        send_key(input, keyboard_android_key(event.key.keysym.sym),
                 event.type == SDL_KEYDOWN);
      break;

    case SDL_WINDOWEVENT:
      if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
          event.window.event == SDL_WINDOWEVENT_RESIZED) {
        SDL_Window *window = SDL_GetWindowFromID(event.window.windowID);
        int width = event.window.data1;
        int height = event.window.data2;
        if (window)
          SDL_GL_GetDrawableSize(window, &width, &height);
        if (width > 0)
          input->drawable_width = width;
        if (height > 0)
          input->drawable_height = height;
      } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
        release_controller_state(input);
        release_keyboard_state(input);
        release_mouse_touch(input);
        release_all_touches(input);
      }
      break;

    default:
      break;
    }
  }
  return 1;
}

void asm2_input_shutdown(struct asm2_input *input) {
  if (!input)
    return;
  release_keyboard_state(input);
  release_mouse_touch(input);
  release_all_touches(input);
  close_controller(input, 1);
}
