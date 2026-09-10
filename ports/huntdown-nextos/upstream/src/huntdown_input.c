#define _GNU_SOURCE
#include "huntdown_input.h"
#include "huntdown_input_bindings.h"
#include "huntdown_build.h"
#include "nxinput_pad_ordinal_fix.h"

#include <SDL2/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>

#define HD_PLAYERS 2
#define HD_BUTTONS SDL_CONTROLLER_BUTTON_MAX
#define HD_AXES SDL_CONTROLLER_AXIS_MAX
#define HD_TRIGGER_DOWN 0.35f

struct hd_pad {
  SDL_GameController *controller;
  SDL_JoystickID instance;
  unsigned char button[HD_BUTTONS];
  unsigned char previous_button[HD_BUTTONS];
  /* A tap shorter than one poll interval never shows up in the polled state.
   * SDL's own button-down events latch it for exactly one frame. */
  unsigned char latched_button[HD_BUTTONS];
  float axis[HD_AXES];
  float previous_axis[HD_AXES];
  /* Worn sticks rest above a single threshold and jam a direction.  Each
   * direction engages high and only releases low. */
  unsigned char direction_held[4];
  unsigned char previous_direction_held[4];
};

/* Left, Right, Up, Down. */
enum { HD_DIR_LEFT, HD_DIR_RIGHT, HD_DIR_UP, HD_DIR_DOWN, HD_DIR_COUNT };
#define HD_DIRECTION_ENGAGE 0.45f
#define HD_DIRECTION_RELEASE 0.30f

static struct hd_pad g_pad[HD_PLAYERS];
static uintptr_t g_il2cpp_base;
static int g_installed;
static int g_sdl_ready;
static int g_exit_requested;
static unsigned g_poll_count;

#if HD_DEV_DIAGNOSTICS
/* Private controller pulses used to prove native menu transitions
 * without patching managed state.  It enters through the same GamePad methods
 * as a physical A button and is compiled out of public builds. */
static void hd_diagnostic_action_name(const char *entry, const char *name) {
  enum { HD_SEEN_ACTIONS = 32 };
  static char seen[HD_SEEN_ACTIONS][112];
  static int seen_count;
  if (!getenv("HD_ACTION_TRACE") || !name || !*name) return;
  for (int i = 0; i < seen_count; i++)
    if (!strcmp(seen[i], name)) return;
  if (seen_count < HD_SEEN_ACTIONS) {
    snprintf(seen[seen_count], sizeof seen[seen_count], "%s", name);
    seen_count++;
  }
  fprintf(stderr, "[HDINPUT] acao atribuida via %s: \"%s\"\n",
          entry ? entry : "?", name);
}

static int hd_diagnostic_a(int previous) {
  enum { HD_TEST_PULSES = 16 };
  static long targets[HD_TEST_PULSES];
  static unsigned char logged[HD_TEST_PULSES];
  static int target_count = -1;
  static unsigned live_checked = (unsigned)-1;
  static long live_target = -1;
  static int live_logged;
  if (target_count < 0) {
    const char *value = getenv("HD_TEST_A_FRAMES");
    if (!value || !*value) value = getenv("HD_TEST_A_FRAME");
    target_count = 0;
    while (value && *value && target_count < HD_TEST_PULSES) {
      char *end = NULL;
      long target = strtol(value, &end, 10);
      if (end == value) {
        value++;
        continue;
      }
      if (target >= 0) targets[target_count++] = target;
      value = end;
      while (*value == ',' || *value == ';' || *value == ' ' ||
             *value == '\t') value++;
    }
  }
  if (!previous && getenv("HD_TEST_A_LIVE") &&
      live_checked != g_poll_count) {
    live_checked = g_poll_count;
    if (access("/tmp/huntdown-test-a", F_OK) == 0) {
      unlink("/tmp/huntdown-test-a");
      live_target = (long)g_poll_count;
      live_logged = 0;
    }
  }
  long sample = (long)g_poll_count - (previous ? 1L : 0L);
  for (int i = 0; i < target_count; i++) {
    int active = sample >= targets[i] && sample < targets[i] + 2;
    if (active && !previous && !logged[i]) {
      logged[i] = 1;
      fprintf(stderr, "[HDINPUT] pulso A de diagnostico no frame %ld\n",
              sample);
    }
    if (active) return 1;
  }
  if (live_target >= 0 && sample >= live_target &&
      sample < live_target + 2) {
    if (!previous && !live_logged) {
      live_logged = 1;
      fprintf(stderr, "[HDINPUT] pulso A de diagnostico ao vivo no frame %ld\n",
              sample);
    }
    return 1;
  }
  return 0;
}
#else
static inline void hd_diagnostic_action_name(const char *entry,
                                             const char *name) {
  (void)entry;
  (void)name;
}

static inline int hd_diagnostic_a(int previous) {
  (void)previous;
  return 0;
}
#endif

static float hd_axis_normalize(Sint16 value) {
  float result = value < 0 ? (float)value / 32768.0f
                           : (float)value / 32767.0f;
  if (result > 1.0f) result = 1.0f;
  if (result < -1.0f) result = -1.0f;
  return result;
}

static float hd_trigger_normalize(Sint16 value) {
  if (value < 0) return ((float)value + 32768.0f) / 65535.0f;
  return (float)value / 32767.0f;
}

static int hd_button(const struct hd_pad *pad, SDL_GameControllerButton button,
                     int previous) {
  if (!pad || button < 0 || button >= HD_BUTTONS) return 0;
  return previous ? pad->previous_button[button] : pad->button[button];
}

static float hd_axis(const struct hd_pad *pad, SDL_GameControllerAxis axis,
                     int previous) {
  if (!pad || axis < 0 || axis >= HD_AXES) return 0.0f;
  return previous ? pad->previous_axis[axis] : pad->axis[axis];
}

static int hd_trigger(const struct hd_pad *pad, SDL_GameControllerAxis axis,
                      int previous) {
  return hd_axis(pad, axis, previous) >= HD_TRIGGER_DOWN;
}

static void hd_update_direction(struct hd_pad *pad, int direction,
                                float magnitude) {
  if (direction < 0 || direction >= HD_DIR_COUNT) return;
  if (pad->direction_held[direction])
    pad->direction_held[direction] = magnitude > HD_DIRECTION_RELEASE;
  else
    pad->direction_held[direction] = magnitude > HD_DIRECTION_ENGAGE;
}

static int hd_direction(const struct hd_pad *pad, int direction,
                        int previous) {
  if (!pad || direction < 0 || direction >= HD_DIR_COUNT) return 0;
  return previous ? pad->previous_direction_held[direction]
                  : pad->direction_held[direction];
}

static int hd_stick_direction(const struct hd_pad *pad, const char *name,
                              int previous) {
  if (!strcasecmp(name, "Left"))
    return hd_button(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT, previous) ||
           hd_direction(pad, HD_DIR_LEFT, previous);
  if (!strcasecmp(name, "Right"))
    return hd_button(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, previous) ||
           hd_direction(pad, HD_DIR_RIGHT, previous);
  if (!strcasecmp(name, "Up"))
    return hd_button(pad, SDL_CONTROLLER_BUTTON_DPAD_UP, previous) ||
           hd_direction(pad, HD_DIR_UP, previous);
  if (!strcasecmp(name, "Down"))
    return hd_button(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN, previous) ||
           hd_direction(pad, HD_DIR_DOWN, previous);
  return 0;
}

static int hd_binding_pressed(const struct hd_pad *pad, unsigned binding,
                              int previous) {
  if ((binding & HD_SIGNAL_A) &&
      hd_button(pad, SDL_CONTROLLER_BUTTON_A, previous)) return 1;
  if ((binding & HD_SIGNAL_B) &&
      hd_button(pad, SDL_CONTROLLER_BUTTON_B, previous)) return 1;
  if ((binding & HD_SIGNAL_X) &&
      hd_button(pad, SDL_CONTROLLER_BUTTON_X, previous)) return 1;
  if ((binding & HD_SIGNAL_Y) &&
      hd_button(pad, SDL_CONTROLLER_BUTTON_Y, previous)) return 1;
  if ((binding & HD_SIGNAL_LB) &&
      hd_button(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, previous)) return 1;
  if ((binding & HD_SIGNAL_RB) &&
      hd_button(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, previous)) return 1;
  if ((binding & HD_SIGNAL_L2) &&
      hd_trigger(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, previous)) return 1;
  if ((binding & HD_SIGNAL_R2) &&
      hd_trigger(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, previous)) return 1;
  if ((binding & HD_SIGNAL_L3) &&
      hd_button(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK, previous)) return 1;
  if ((binding & HD_SIGNAL_R3) &&
      hd_button(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK, previous)) return 1;
  if ((binding & HD_SIGNAL_START) &&
      hd_button(pad, SDL_CONTROLLER_BUTTON_START, previous)) return 1;
  if ((binding & HD_SIGNAL_BACK) &&
      hd_button(pad, SDL_CONTROLLER_BUTTON_BACK, previous)) return 1;
  if ((binding & HD_SIGNAL_GUIDE) &&
      hd_button(pad, SDL_CONTROLLER_BUTTON_GUIDE, previous)) return 1;
  return 0;
}

static int hd_action(const struct hd_pad *pad, const char *name, int previous) {
  if (!name) return 0;
  if (!strcasecmp(name, "Jump") && hd_diagnostic_a(previous)) return 1;
  if (!pad || !pad->controller) return 0;
  unsigned binding = hd_gameplay_binding(name);
  if (binding) return hd_binding_pressed(pad, binding, previous);
  return hd_stick_direction(pad, name, previous);
}

static float hd_action_axis(const struct hd_pad *pad, const char *name,
                            int previous) {
  if (!pad || !pad->controller || !name) return 0.0f;
  if (!strcasecmp(name, "Move Horizontal") ||
      !strcasecmp(name, "MoveHorizontal") ||
      !strcasecmp(name, "Horizontal")) {
    if (hd_button(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT, previous)) return -1.0f;
    if (hd_button(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, previous)) return 1.0f;
    float value = hd_axis(pad, SDL_CONTROLLER_AXIS_LEFTX, previous);
    return value > -0.16f && value < 0.16f ? 0.0f : value;
  }
  if (!strcasecmp(name, "Move Vertical") ||
      !strcasecmp(name, "MoveVertical") ||
      !strcasecmp(name, "Vertical")) {
    if (hd_button(pad, SDL_CONTROLLER_BUTTON_DPAD_UP, previous)) return 1.0f;
    if (hd_button(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN, previous)) return -1.0f;
    float value = -hd_axis(pad, SDL_CONTROLLER_AXIS_LEFTY, previous);
    return value > -0.16f && value < 0.16f ? 0.0f : value;
  }
  return 0.0f;
}

static void hd_il2cpp_string(void *string, char *output, size_t capacity) {
  if (!output || capacity == 0) return;
  output[0] = '\0';
  if (!string) return;
  int32_t length = *(int32_t *)((char *)string + 0x10);
  if (length < 0 || length > 96) return;
  const uint16_t *chars = (const uint16_t *)((char *)string + 0x14);
  size_t count = (size_t)length < capacity - 1 ? (size_t)length : capacity - 1;
  for (size_t i = 0; i < count; i++)
    output[i] = chars[i] < 0x80 ? (char)chars[i] : '?';
  output[count] = '\0';
}

static struct hd_pad *hd_player(int player) {
  return player >= 0 && player < HD_PLAYERS ? &g_pad[player] : NULL;
}

/* Exact ABI of Huntdown.GamePad in Assembly-CSharp.dll. The final MethodInfo
 * parameter is supplied by IL2CPP after the declared managed parameters. */
static int hd_gamepad_any(void *self, int player, void *method_info) {
  (void)self;
  (void)method_info;
  if (hd_diagnostic_a(0)) return 1;
  struct hd_pad *pad = hd_player(player);
  if (!pad || !pad->controller) return 0;
  for (int i = 0; i < HD_BUTTONS; i++) if (pad->button[i]) return 1;
  return hd_trigger(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 0) ||
         hd_trigger(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 0);
}

static int hd_gamepad_button(void *self, int player, void *action,
                             void *method_info) {
  (void)self;
  (void)method_info;
  char name[112];
  hd_il2cpp_string(action, name, sizeof name);
  hd_diagnostic_action_name("GetButton", name);
  return hd_action(hd_player(player), name, 0);
}

static int hd_gamepad_button_down(void *self, int player, void *action,
                                  void *method_info) {
  (void)self;
  (void)method_info;
  char name[112];
  hd_il2cpp_string(action, name, sizeof name);
  hd_diagnostic_action_name("GetButtonDown", name);
  struct hd_pad *pad = hd_player(player);
  return hd_action(pad, name, 0) && !hd_action(pad, name, 1);
}

static int hd_gamepad_button_up(void *self, int player, void *action,
                                void *method_info) {
  (void)self;
  (void)method_info;
  char name[112];
  hd_il2cpp_string(action, name, sizeof name);
  hd_diagnostic_action_name("GetButtonUp", name);
  struct hd_pad *pad = hd_player(player);
  return !hd_action(pad, name, 0) && hd_action(pad, name, 1);
}

static float hd_gamepad_axis(void *self, int player, void *action,
                             void *method_info) {
  (void)self;
  (void)method_info;
  char name[112];
  hd_il2cpp_string(action, name, sizeof name);
  hd_diagnostic_action_name("GetAxis", name);
  return hd_action_axis(hd_player(player), name, 0);
}

static int hd_gamepad_has_joystick(void *self, void *method_info) {
  (void)self;
  (void)method_info;
  return g_pad[0].controller || g_pad[1].controller;
}

static int hd_gamepad_player_has_controller(void *self, int player,
                                             void *method_info) {
  (void)self;
  (void)method_info;
  struct hd_pad *pad = hd_player(player);
  return pad && pad->controller;
}

static int hd_gamepad_search_controller(void *self, int player, int steal,
                                        void *method_info) {
  (void)self;
  (void)steal;
  (void)method_info;
  struct hd_pad *pad = hd_player(player);
  return pad && pad->controller;
}

static int hd_gamepad_controller_count(void *self, int ignore_assigned,
                                       void *method_info) {
  (void)self;
  (void)ignore_assigned;
  (void)method_info;
  return (g_pad[0].controller ? 1 : 0) + (g_pad[1].controller ? 1 : 0);
}

/* MainMenu.InteractGamepad uses these high-level GamePad methods instead of
 * GetButton/GetAxis. Android normally fills their backing arrays through
 * UnityEngine.Input; on the native host, return the same normalized SDL state
 * while leaving MainMenu's own edge detection and Click* dispatch untouched. */
static struct hd_pad *hd_player_bool(int player_one) {
  return hd_player(player_one ? 0 : 1);
}

#define HD_DO_DIRECTION(function_name, action_name)                         \
  static int function_name(void *self, int player_one, void *method_info) { \
    (void)self;                                                              \
    (void)method_info;                                                       \
    return hd_stick_direction(hd_player_bool(player_one), action_name, 0);   \
  }

HD_DO_DIRECTION(hd_gamepad_do_left, "Left")
HD_DO_DIRECTION(hd_gamepad_do_right, "Right")
HD_DO_DIRECTION(hd_gamepad_do_up, "Up")
HD_DO_DIRECTION(hd_gamepad_do_down, "Down")

#define HD_DO_MENU(function_name, menu_slot)                                \
  static int function_name(void *self, int player_one, void *method_info) { \
    (void)self;                                                              \
    (void)method_info;                                                       \
    return hd_binding_pressed(hd_player_bool(player_one),                    \
                              hd_menu_binding(menu_slot), 0);                \
  }

/* These are menu-level slots, not gameplay action names.  Binding them to
 * Fire/Fire2 made X cancel and B act as the secondary choice.  Keep the
 * authored menu convention while the lower-level gameplay map remains
 * unchanged: A confirms, B cancels, X/Y are the two extra actions. */
HD_DO_MENU(hd_gamepad_do_start, HD_MENU_START)
HD_DO_MENU(hd_gamepad_do_action1, HD_MENU_ACTION1)
HD_DO_MENU(hd_gamepad_do_action2, HD_MENU_ACTION2)
HD_DO_MENU(hd_gamepad_do_back, HD_MENU_BACK)
HD_DO_MENU(hd_gamepad_do_enter, HD_MENU_ENTER)
HD_DO_MENU(hd_gamepad_do_exit, HD_MENU_EXIT)

struct hd_hook {
  const char *name;
  uintptr_t rva;
  unsigned char signature[16];
  void *replacement;
};

#define HD_SIG(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p) \
  {a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p}

static const struct hd_hook g_hooks_200023[] = {
  {"GamePad.DoLeft", 0x14AF420,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0x08,0x24,0x40,0xf9,0xe8,0x01,0x00,0xb4,0x0a,0x19,0x40,0xb9),
   (void *)hd_gamepad_do_left},
  {"GamePad.DoRight", 0x14AF46C,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0x08,0x24,0x40,0xf9,0xe8,0x01,0x00,0xb4,0x0a,0x19,0x40,0xb9),
   (void *)hd_gamepad_do_right},
  {"GamePad.DoUp", 0x14AF4B8,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0x08,0x28,0x40,0xf9,0xe8,0x01,0x00,0xb4,0x0a,0x19,0x40,0xb9),
   (void *)hd_gamepad_do_up},
  {"GamePad.DoDown", 0x14AF504,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0x08,0x28,0x40,0xf9,0xe8,0x01,0x00,0xb4,0x0a,0x19,0x40,0xb9),
   (void *)hd_gamepad_do_down},
  {"GamePad.DoStart", 0x14B09F0,
   HD_SIG(0xfe,0x57,0xbe,0xa9,0xf4,0x4f,0x01,0xa9,0xb5,0xd4,0x00,0xd0,0xa8,0x26,0x4f,0x39),
   (void *)hd_gamepad_do_start},
  {"GamePad.DoAction1", 0x14B0B04,
   HD_SIG(0xfe,0x57,0xbe,0xa9,0xf4,0x4f,0x01,0xa9,0xb5,0xd4,0x00,0xd0,0xa8,0x2a,0x4f,0x39),
   (void *)hd_gamepad_do_action1},
  {"GamePad.DoAction2", 0x14B0BEC,
   HD_SIG(0xfe,0x57,0xbe,0xa9,0xf4,0x4f,0x01,0xa9,0xb5,0xd4,0x00,0xd0,0xa8,0x2e,0x4f,0x39),
   (void *)hd_gamepad_do_action2},
  {"GamePad.DoBack", 0x14B0CD4,
   HD_SIG(0xfe,0x57,0xbe,0xa9,0xf4,0x4f,0x01,0xa9,0xb5,0xd4,0x00,0xd0,0xa8,0x32,0x4f,0x39),
   (void *)hd_gamepad_do_back},
  {"GamePad.DoEnter", 0x14B0DBC,
   HD_SIG(0xfe,0x57,0xbe,0xa9,0xf4,0x4f,0x01,0xa9,0xb5,0xd4,0x00,0xd0,0xa8,0x36,0x4f,0x39),
   (void *)hd_gamepad_do_enter},
  {"GamePad.DoExit", 0x14B0EA4,
   HD_SIG(0xfe,0x57,0xbe,0xa9,0xf4,0x4f,0x01,0xa9,0xb5,0xd4,0x00,0xd0,0xa8,0x3a,0x4f,0x39),
   (void *)hd_gamepad_do_exit},
  {"GamePad.GetAnyButton", 0x14B3624,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0xc1,0x00,0x00,0x34,0x00,0x14,0x40,0xf9,0xc0,0x00,0x00,0xb4),
   (void *)hd_gamepad_any},
  {"GamePad.GetButton", 0x14B0AD8,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0xe1,0x00,0x00,0x34,0x00,0x14,0x40,0xf9,0xe0,0x00,0x00,0xb4),
   (void *)hd_gamepad_button},
  {"GamePad.GetButtonUp", 0x14B1C14,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0xe1,0x00,0x00,0x34,0x00,0x14,0x40,0xf9,0xe0,0x00,0x00,0xb4),
   (void *)hd_gamepad_button_up},
  {"GamePad.GetButtonDown", 0x14B2208,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0xe1,0x00,0x00,0x34,0x00,0x14,0x40,0xf9,0xe0,0x00,0x00,0xb4),
   (void *)hd_gamepad_button_down},
  {"GamePad.GetAxis", 0x14B21DC,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0xe1,0x00,0x00,0x34,0x00,0x14,0x40,0xf9,0xe0,0x00,0x00,0xb4),
   (void *)hd_gamepad_axis},
  {"GamePad.HasJoystick", 0x14B28B4,
   HD_SIG(0xfe,0x0f,0x1e,0xf8,0xf4,0x4f,0x01,0xa9,0xb4,0xd4,0x00,0x90,0x73,0xc4,0x00,0xd0),
   (void *)hd_gamepad_has_joystick},
  {"GamePad.GetPlayerHasController", 0x14B2874,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0x81,0x00,0x00,0x34,0x08,0x14,0x40,0xf9,0x88,0x00,0x00,0xb5),
   (void *)hd_gamepad_player_has_controller},
  {"GamePad.SearchForPlayerController", 0x14B297C,
   HD_SIG(0xfe,0x67,0xbc,0xa9,0xf8,0x5f,0x01,0xa9,0xf6,0x57,0x02,0xa9,0xf4,0x4f,0x03,0xa9),
   (void *)hd_gamepad_search_controller},
  {"GamePad.AvailableControllerCount", 0x14B32B0,
   HD_SIG(0xfe,0x0f,0x1b,0xf8,0xfa,0x67,0x01,0xa9,0xf8,0x5f,0x02,0xa9,0xf6,0x57,0x03,0xa9),
   (void *)hd_gamepad_controller_count},
};

/* Huntdown versionCode 200036, Unity 6000.2.6f2.  These addresses and entry
 * bytes come from this build's own IL2CPP metadata/image pair. */
static const struct hd_hook g_hooks_200036[] = {
  {"GamePad.DoLeft", 0x1A6AA6C,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0x08,0x24,0x40,0xf9,0xe8,0x01,0x00,0xb4,0x29,0x00,0x00,0x52),
   (void *)hd_gamepad_do_left},
  {"GamePad.DoRight", 0x1A6AAB8,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0x08,0x24,0x40,0xf9,0xe8,0x01,0x00,0xb4,0x29,0x00,0x00,0x52),
   (void *)hd_gamepad_do_right},
  {"GamePad.DoUp", 0x1A6AB04,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0x08,0x28,0x40,0xf9,0xe8,0x01,0x00,0xb4,0x29,0x00,0x00,0x52),
   (void *)hd_gamepad_do_up},
  {"GamePad.DoDown", 0x1A6AB50,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0x08,0x28,0x40,0xf9,0xe8,0x01,0x00,0xb4,0x29,0x00,0x00,0x52),
   (void *)hd_gamepad_do_down},
  {"GamePad.DoStart", 0x1A6C00C,
   HD_SIG(0xfe,0x57,0xbe,0xa9,0xf4,0x4f,0x01,0xa9,0x15,0x11,0x01,0xf0,0xf4,0x03,0x01,0x2a),
   (void *)hd_gamepad_do_start},
  {"GamePad.DoAction1", 0x1A6C120,
   HD_SIG(0xfe,0x57,0xbe,0xa9,0xf4,0x4f,0x01,0xa9,0x15,0x11,0x01,0xf0,0xf4,0x03,0x01,0x2a),
   (void *)hd_gamepad_do_action1},
  {"GamePad.DoAction2", 0x1A6C208,
   HD_SIG(0xfe,0x57,0xbe,0xa9,0xf4,0x4f,0x01,0xa9,0x15,0x11,0x01,0xf0,0xf4,0x03,0x01,0x2a),
   (void *)hd_gamepad_do_action2},
  {"GamePad.DoBack", 0x1A6C2F0,
   HD_SIG(0xfe,0x57,0xbe,0xa9,0xf4,0x4f,0x01,0xa9,0x15,0x11,0x01,0xf0,0xf4,0x03,0x01,0x2a),
   (void *)hd_gamepad_do_back},
  {"GamePad.DoEnter", 0x1A6C3D8,
   HD_SIG(0xfe,0x57,0xbe,0xa9,0xf4,0x4f,0x01,0xa9,0x15,0x11,0x01,0xf0,0xf4,0x03,0x01,0x2a),
   (void *)hd_gamepad_do_enter},
  {"GamePad.DoExit", 0x1A6C4C0,
   HD_SIG(0xfe,0x57,0xbe,0xa9,0xf4,0x4f,0x01,0xa9,0x15,0x11,0x01,0xf0,0xf4,0x03,0x01,0x2a),
   (void *)hd_gamepad_do_exit},
  {"GamePad.GetAnyButton", 0x1A6EDCC,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0xc1,0x00,0x00,0x34,0x00,0x14,0x40,0xf9,0xc0,0x00,0x00,0xb4),
   (void *)hd_gamepad_any},
  {"GamePad.GetButton", 0x1A6C0F4,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0xe1,0x00,0x00,0x34,0x00,0x14,0x40,0xf9,0xe0,0x00,0x00,0xb4),
   (void *)hd_gamepad_button},
  {"GamePad.GetButtonUp", 0x1A6D3D8,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0xe1,0x00,0x00,0x34,0x00,0x14,0x40,0xf9,0xe0,0x00,0x00,0xb4),
   (void *)hd_gamepad_button_up},
  {"GamePad.GetButtonDown", 0x1A6D9CC,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0xe1,0x00,0x00,0x34,0x00,0x14,0x40,0xf9,0xe0,0x00,0x00,0xb4),
   (void *)hd_gamepad_button_down},
  {"GamePad.GetAxis", 0x1A6D9A0,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0xe1,0x00,0x00,0x34,0x00,0x14,0x40,0xf9,0xe0,0x00,0x00,0xb4),
   (void *)hd_gamepad_axis},
  {"GamePad.HasJoystick", 0x1A6E078,
   HD_SIG(0xfe,0x0f,0x1e,0xf8,0xf4,0x4f,0x01,0xa9,0x14,0x11,0x01,0xb0,0x53,0xfd,0x00,0x90),
   (void *)hd_gamepad_has_joystick},
  {"GamePad.GetPlayerHasController", 0x1A6E038,
   HD_SIG(0xfe,0x0f,0x1f,0xf8,0x81,0x00,0x00,0x34,0x08,0x14,0x40,0xf9,0x88,0x00,0x00,0xb5),
   (void *)hd_gamepad_player_has_controller},
  {"GamePad.SearchForPlayerController", 0x1A6E140,
   HD_SIG(0xfe,0x67,0xbc,0xa9,0xf8,0x5f,0x01,0xa9,0xf6,0x57,0x02,0xa9,0xf4,0x4f,0x03,0xa9),
   (void *)hd_gamepad_search_controller},
  {"GamePad.AvailableControllerCount", 0x1A6EA4C,
   HD_SIG(0xfe,0x0f,0x1b,0xf8,0xfa,0x67,0x01,0xa9,0xf8,0x5f,0x02,0xa9,0xf6,0x57,0x03,0xa9),
   (void *)hd_gamepad_controller_count},
};

static void hd_write_hook(const struct hd_hook *hook) {
  uintptr_t address = g_il2cpp_base + hook->rva;
  uint32_t *code = (uint32_t *)address;
  code[0] = 0x58000050u; /* ldr x16, [pc, #8] */
  code[1] = 0xd61f0200u; /* br x16 */
  *(uint64_t *)(code + 2) = (uint64_t)(uintptr_t)hook->replacement;
  __builtin___clear_cache((char *)address, (char *)address + 16);
}

int hd_input_install(uintptr_t il2cpp_base) {
  if (g_installed) return 1;
  if (!il2cpp_base) return 0;
  g_il2cpp_base = il2cpp_base;

  const struct hd_hook *hooks = g_hooks_200023;
  size_t hook_count = sizeof g_hooks_200023 / sizeof g_hooks_200023[0];
  if (hd_build_current() == HD_BUILD_200036) {
    hooks = g_hooks_200036;
    hook_count = sizeof g_hooks_200036 / sizeof g_hooks_200036[0];
  } else if (hd_build_current() != HD_BUILD_200023) {
    fprintf(stderr, "[HDINPUT] perfil de build desconhecido; bridge recusada\n");
    return 0;
  }

  for (size_t i = 0; i < hook_count; i++) {
    const unsigned char *code = (const unsigned char *)(g_il2cpp_base + hooks[i].rva);
    if (memcmp(code, hooks[i].signature, sizeof hooks[i].signature) != 0) {
      fprintf(stderr, "[HDINPUT] assinatura divergente em %s (RVA 0x%lx); nenhum hook aplicado\n",
              hooks[i].name, (unsigned long)hooks[i].rva);
      return 0;
    }
  }

  /* Make every target page writable before changing the first instruction.
   * A protection failure therefore cannot leave a partially installed bridge. */
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) return 0;
  uintptr_t pages[32];
  size_t page_count = 0;
  for (size_t i = 0; i < hook_count; i++) {
    uintptr_t address = g_il2cpp_base + hooks[i].rva;
    uintptr_t page = address & ~((uintptr_t)page_size - 1);
    size_t p = 0;
    while (p < page_count && pages[p] != page) p++;
    if (p == page_count) pages[page_count++] = page;
  }
  size_t writable = 0;
  for (; writable < page_count; writable++) {
    if (mprotect((void *)pages[writable], (size_t)page_size,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
      fprintf(stderr, "[HDINPUT] mprotect página %p: %s\n",
              (void *)pages[writable], strerror(errno));
      for (size_t p = 0; p < writable; p++)
        mprotect((void *)pages[p], (size_t)page_size,
                 PROT_READ | PROT_EXEC);
      return 0;
    }
  }
  for (size_t i = 0; i < hook_count; i++)
    hd_write_hook(&hooks[i]);
  for (size_t p = 0; p < page_count; p++)
    if (mprotect((void *)pages[p], (size_t)page_size,
                 PROT_READ | PROT_EXEC) != 0)
      fprintf(stderr, "[HDINPUT] aviso: não restaurou RX em %p: %s\n",
              (void *)pages[p], strerror(errno));

  g_installed = 1;
  fprintf(stderr,
          "[HDINPUT] %zu hooks Huntdown %s instalados (%s)\n",
          hook_count, hd_build_version_name(), hd_build_label());
  return 1;
}

static void hd_load_mapping_file(const char *path) {
  if (!path || !*path || access(path, R_OK) != 0) return;
  int count = SDL_GameControllerAddMappingsFromFile(path);
  if (count >= 0) fprintf(stderr, "[HDINPUT] %d mapeamentos carregados de %s\n", count, path);
}

#define HD_MAPPING_GUIDS 32
static char g_mapping_guid[HD_MAPPING_GUIDS][64];
static int g_mapping_guid_count;

static int hd_mapping_seen(const char *guid) {
  for (int i = 0; i < g_mapping_guid_count; i++)
    if (!strcmp(g_mapping_guid[i], guid)) return 1;
  return 0;
}

static void hd_mapping_remember(const char *guid) {
  if (g_mapping_guid_count >= HD_MAPPING_GUIDS || hd_mapping_seen(guid)) return;
  snprintf(g_mapping_guid[g_mapping_guid_count++],
           sizeof g_mapping_guid[0], "%s", guid);
}

static void hd_add_mapping(int index) {
  SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(index);
  char guid_text[64];
  SDL_JoystickGetGUIDString(guid, guid_text, sizeof guid_text);
  if (hd_mapping_seen(guid_text)) return;
  if (!strcmp(guid_text, "0300605b100800000100000010010000")) {
    const char *mapping =
        "0300605b100800000100000010010000,USB Gamepad,"
        "a:b2,b:b1,x:b3,y:b0,leftshoulder:b4,rightshoulder:b5,"
        "lefttrigger:b6,righttrigger:b7,back:b8,start:b9,"
        "leftstick:b10,rightstick:b11,leftx:a0,lefty:a1,rightx:a3,righty:a2,"
        "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,platform:Linux,";
    SDL_GameControllerAddMapping(mapping);
  }
  nxinput_pad_ordinal_fix_apply(index, "HUNTDOWN",
                                NXINPUT_PAD_ORDINAL_LAYOUT_HID);
  if (SDL_IsGameController(index)) {
    hd_mapping_remember(guid_text);
    return;
  }

  SDL_Joystick *probe = SDL_JoystickOpen(index);
  int buttons = probe ? SDL_JoystickNumButtons(probe) : 0;
  int axes = probe ? SDL_JoystickNumAxes(probe) : 0;
  int hats = probe ? SDL_JoystickNumHats(probe) : 0;
  if (probe) SDL_JoystickClose(probe);
  if (buttons < 8 || axes < 2) return;

  char mapping[1024];
  snprintf(mapping, sizeof mapping,
           "%s,Huntdown Xbox Profile,a:b0,b:b1,x:b2,y:b3,"
           "leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,"
           "back:b8,start:b9,leftstick:b10,rightstick:b11,"
           "leftx:a0,lefty:a1,rightx:a2,righty:a3,"
           "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,platform:Linux,",
           guid_text);
  if (SDL_GameControllerAddMapping(mapping) >= 0) {
    hd_mapping_remember(guid_text);
    fprintf(stderr, "[HDINPUT] perfil genérico js%d (%d botões, %d eixos, %d hats)\n",
            index, buttons, axes, hats);
  }
}

static int hd_instance_open(SDL_JoystickID instance) {
  for (int player = 0; player < HD_PLAYERS; player++)
    if (g_pad[player].controller && g_pad[player].instance == instance) return 1;
  return 0;
}

static void hd_rescan(void) {
  for (int player = 0; player < HD_PLAYERS; player++) {
    if (g_pad[player].controller &&
        !SDL_GameControllerGetAttached(g_pad[player].controller)) {
      SDL_GameControllerClose(g_pad[player].controller);
      memset(&g_pad[player], 0, sizeof g_pad[player]);
      g_pad[player].instance = -1;
      fprintf(stderr, "[HDINPUT] controle P%d removido\n", player + 1);
    }
  }
  int count = SDL_NumJoysticks();
  for (int index = 0; index < count; index++) {
    hd_add_mapping(index);
    if (!SDL_IsGameController(index)) continue;
    SDL_GameController *controller = SDL_GameControllerOpen(index);
    if (!controller) continue;
    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(controller);
    SDL_JoystickID instance = joystick ? SDL_JoystickInstanceID(joystick) : -1;
    if (instance < 0 || hd_instance_open(instance)) {
      SDL_GameControllerClose(controller);
      continue;
    }
    int player;
    for (player = 0; player < HD_PLAYERS; player++)
      if (!g_pad[player].controller) break;
    if (player == HD_PLAYERS) {
      SDL_GameControllerClose(controller);
      break;
    }
    g_pad[player].controller = controller;
    g_pad[player].instance = instance;
    fprintf(stderr, "[HDINPUT] P%d = %s (instance=%d)\n", player + 1,
            SDL_GameControllerName(controller), (int)instance);
    char *mapping = SDL_GameControllerMapping(controller);
    if (mapping) {
      fprintf(stderr, "[HDINPUT] P%d mapping SDL ativo: %s\n", player + 1,
              mapping);
      SDL_free(mapping);
    }
  }
}

/* ---------- raw evdev exit chord ----------
 * Several handhelds wire SELECT/START as HD_KEY_TRIGGER_HAPPY1/2 instead of
 * BTN_SELECT/BTN_START, so a firmware without a controller mapping leaves the
 * SDL chord with nothing to read.  Watching evdev directly makes the exit
 * combination independent of any mapping database, and both paths converge on
 * the same in-process shutdown request. */
#define HD_EVDEV_DEVICES 8

/* Named BTN_* in current headers and KEY_* in older ones; the codes are
 * stable ABI, so pin them and stop depending on the build host's uapi. */
#define HD_KEY_TRIGGER_HAPPY1 0x2c0
#define HD_KEY_TRIGGER_HAPPY2 0x2c1

static int g_evdev_fd[HD_EVDEV_DEVICES];
static int g_evdev_count = -1;
static unsigned char g_evdev_select;
static unsigned char g_evdev_start;

static int hd_evdev_is_gamepad(int fd) {
  unsigned long keys[(KEY_MAX / (8 * sizeof(unsigned long))) + 1];
  memset(keys, 0, sizeof keys);
  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0) return 0;
#define HD_BIT_SET(code)                                                    \
  ((keys[(code) / (8 * sizeof(unsigned long))] >>                           \
    ((code) % (8 * sizeof(unsigned long)))) & 1UL)
  return HD_BIT_SET(BTN_SOUTH) || HD_BIT_SET(BTN_A) ||
         HD_BIT_SET(HD_KEY_TRIGGER_HAPPY1);
#undef HD_BIT_SET
}

static void hd_evdev_open(void) {
  if (g_evdev_count >= 0) return;
  g_evdev_count = 0;
  for (int index = 0; index < 32 && g_evdev_count < HD_EVDEV_DEVICES; ++index) {
    char path[64];
    snprintf(path, sizeof path, "/dev/input/event%d", index);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    if (!hd_evdev_is_gamepad(fd)) {
      close(fd);
      continue;
    }
    char name[128] = "?";
    (void)ioctl(fd, EVIOCGNAME(sizeof name), name);
    fprintf(stderr, "[HDINPUT] combo de saida tambem por evdev: %s (%s)\n",
            path, name);
    g_evdev_fd[g_evdev_count++] = fd;
  }
  if (!g_evdev_count)
    fprintf(stderr,
            "[HDINPUT] nenhum evdev de controle legivel; combo de saida "
            "depende do mapeamento SDL\n");
}

static int hd_evdev_exit_chord(void) {
  if (g_evdev_count <= 0) return 0;
  struct input_event event;
  for (int i = 0; i < g_evdev_count; ++i) {
    while (read(g_evdev_fd[i], &event, sizeof event) == (ssize_t)sizeof event) {
      if (event.type != EV_KEY) continue;
      unsigned char down = event.value != 0; /* 1 press, 2 autorepeat */
      switch (event.code) {
        case BTN_SELECT:
        case HD_KEY_TRIGGER_HAPPY1:
          g_evdev_select = down;
          break;
        case BTN_START:
        case HD_KEY_TRIGGER_HAPPY2:
          g_evdev_start = down;
          break;
        default:
          break;
      }
    }
  }
  return g_evdev_select && g_evdev_start;
}

static void hd_evdev_close(void) {
  for (int i = 0; i < g_evdev_count && i < HD_EVDEV_DEVICES; ++i)
    close(g_evdev_fd[i]);
  g_evdev_count = -1;
  g_evdev_select = 0;
  g_evdev_start = 0;
}

/* Drain SDL's own button-down events so a press shorter than one poll still
 * reaches the engine on the next frame. */
static void hd_latch_taps(void) {
  SDL_Event events[32];
  int count = SDL_PeepEvents(events, 32, SDL_GETEVENT,
                             SDL_CONTROLLERBUTTONDOWN,
                             SDL_CONTROLLERBUTTONDOWN);
  for (int i = 0; i < count; ++i) {
    int button = events[i].cbutton.button;
    if (button < 0 || button >= HD_BUTTONS) continue;
    for (int player = 0; player < HD_PLAYERS; ++player) {
      if (g_pad[player].instance != events[i].cbutton.which) continue;
      g_pad[player].latched_button[button] = 1;
      break;
    }
  }
}

void hd_input_poll(void) {
  if (!g_sdl_ready) {
    for (int i = 0; i < HD_PLAYERS; i++) g_pad[i].instance = -1;
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER |
                          SDL_INIT_EVENTS) != 0) {
      fprintf(stderr, "[HDINPUT] SDL input indisponível: %s\n", SDL_GetError());
      return;
    }
    SDL_GameControllerEventState(SDL_ENABLE);
    const char *manual_mapping = getenv("HUNTDOWN_PAD_MAP");
    if (manual_mapping && *manual_mapping) {
      int result = SDL_GameControllerAddMapping(manual_mapping);
      fprintf(stderr, "[HDINPUT] mapping manual result=%d\n", result);
    } else if (!(getenv("SDL_GAMECONTROLLERCONFIG") &&
                 *getenv("SDL_GAMECONTROLLERCONFIG"))) {
      const char *mapping_file = getenv("SDL_GAMECONTROLLERCONFIG_FILE");
      if (mapping_file && *mapping_file) {
        hd_load_mapping_file(mapping_file);
      } else {
        static const char *const mapping_paths[] = {
            "/storage/.config/SDL-GameControllerDB/gamecontrollerdb.txt",
            "/usr/lib/gamecontrollerdb.txt",
            "/opt/system/Tools/PortMaster/gamecontrollerdb.txt",
            "/opt/tools/PortMaster/gamecontrollerdb.txt",
            "/roms/ports/PortMaster/gamecontrollerdb.txt",
            "/storage/roms/ports/PortMaster/gamecontrollerdb.txt",
        };
        for (size_t index = 0;
             index < sizeof mapping_paths / sizeof mapping_paths[0]; ++index)
          hd_load_mapping_file(mapping_paths[index]);
      }
    } else {
      fprintf(stderr,
              "[HDINPUT] mapping explícito SDL_GAMECONTROLLERCONFIG preservado\n");
    }
    g_sdl_ready = 1;
    hd_rescan();
  }

  SDL_PumpEvents();
  hd_latch_taps();
  if ((g_poll_count++ % 120u) == 0) hd_rescan();
  for (int player = 0; player < HD_PLAYERS; player++) {
    struct hd_pad *pad = &g_pad[player];
    memcpy(pad->previous_button, pad->button, sizeof pad->button);
    memcpy(pad->previous_axis, pad->axis, sizeof pad->axis);
    memcpy(pad->previous_direction_held, pad->direction_held,
           sizeof pad->direction_held);
    if (!pad->controller) {
      memset(pad->button, 0, sizeof pad->button);
      memset(pad->latched_button, 0, sizeof pad->latched_button);
      memset(pad->axis, 0, sizeof pad->axis);
      memset(pad->direction_held, 0, sizeof pad->direction_held);
      continue;
    }
    for (int button = 0; button < HD_BUTTONS; button++) {
      pad->button[button] = SDL_GameControllerGetButton(
          pad->controller, (SDL_GameControllerButton)button) ? 1 : 0;
      if (pad->latched_button[button]) {
        pad->button[button] = 1;
        pad->latched_button[button] = 0;
      }
    }
    for (int axis = 0; axis < HD_AXES; axis++) {
      Sint16 raw = SDL_GameControllerGetAxis(
          pad->controller, (SDL_GameControllerAxis)axis);
      pad->axis[axis] = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
                                axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT
                            ? hd_trigger_normalize(raw)
                            : hd_axis_normalize(raw);
    }
    hd_update_direction(pad, HD_DIR_LEFT,
                        -pad->axis[SDL_CONTROLLER_AXIS_LEFTX]);
    hd_update_direction(pad, HD_DIR_RIGHT,
                        pad->axis[SDL_CONTROLLER_AXIS_LEFTX]);
    hd_update_direction(pad, HD_DIR_UP,
                        -pad->axis[SDL_CONTROLLER_AXIS_LEFTY]);
    hd_update_direction(pad, HD_DIR_DOWN,
                        pad->axis[SDL_CONTROLLER_AXIS_LEFTY]);
  }

  hd_evdev_open();
  int exit_chord = (hd_button(&g_pad[0], SDL_CONTROLLER_BUTTON_BACK, 0) &&
                    hd_button(&g_pad[0], SDL_CONTROLLER_BUTTON_START, 0)) ||
                   hd_evdev_exit_chord();
  /* The loop checks this flag immediately after polling and before nativeRender,
   * so the chord must be accepted in this frame.  Waiting 700 ms let Huntdown
   * consume either button as Pause first, making the intended chord appear
   * broken on handhelds.  Individual Back/Start presses remain untouched. */
  if (exit_chord && !g_exit_requested) {
    g_exit_requested = 1;
    fprintf(stderr, "[HDINPUT] SELECT+START: saida solicitada\n");
  }
}

int hd_input_exit_requested(void) {
  return g_exit_requested;
}

void hd_input_shutdown(void) {
  hd_evdev_close();
  for (int player = 0; player < HD_PLAYERS; player++) {
    if (g_pad[player].controller) SDL_GameControllerClose(g_pad[player].controller);
    memset(&g_pad[player], 0, sizeof g_pad[player]);
    g_pad[player].instance = -1;
  }
  if (g_sdl_ready)
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
  g_sdl_ready = 0;
}
