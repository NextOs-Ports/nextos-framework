/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxinput_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fail(const char *expression, const char *file, int line) {
  (void)fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
  exit(1);
}

#define CHECK(expression)                                                       \
  do {                                                                          \
    if (!(expression))                                                          \
      fail(#expression, __FILE__, __LINE__);                                    \
  } while (0)

static int nearf(float left, float right, float tolerance) {
  return fabsf(left - right) <= tolerance;
}

static int16_t positive_axis(float value) {
  return (int16_t)(value * 32767.0f);
}

static void test_axis_and_trigger(void) {
  CHECK(nearf(nxinput_core_axis(INT16_MIN), -1.0f, 0.00001f));
  CHECK(nearf(nxinput_core_axis(INT16_MAX), 1.0f, 0.00001f));
  CHECK(nxinput_core_axis(0) == 0.0f);

  CHECK(nxinput_core_trigger(-1, 0.05f) == 0.0f);
  CHECK(nxinput_core_trigger(positive_axis(0.04f), 0.05f) == 0.0f);
  CHECK(nearf(nxinput_core_trigger(INT16_MAX, 0.05f), 1.0f, 0.00001f));
}

static void test_radial_deadzone_hysteresis(void) {
  nxinput_stick_filter filter = {0};
  float x = 9.0f;
  float y = 9.0f;
  float magnitude;

  nxinput_core_filter_stick(&filter, positive_axis(0.18f), 0, 0.20f,
                            0.15f, &x, &y);
  CHECK(!filter.active);
  CHECK(x == 0.0f && y == 0.0f);

  nxinput_core_filter_stick(&filter, positive_axis(0.50f), 0, 0.20f,
                            0.15f, &x, &y);
  CHECK(filter.active);
  CHECK(x > 0.40f && y == 0.0f);

  /* Between the thresholds the already-active stick remains active. */
  nxinput_core_filter_stick(&filter, positive_axis(0.18f), 0, 0.20f,
                            0.15f, &x, &y);
  CHECK(filter.active);
  CHECK(x > 0.0f);

  nxinput_core_filter_stick(&filter, positive_axis(0.14f), 0, 0.20f,
                            0.15f, &x, &y);
  CHECK(!filter.active);
  CHECK(x == 0.0f && y == 0.0f);

  /* The same intermediate value cannot wake a neutral stick. */
  nxinput_core_filter_stick(&filter, positive_axis(0.18f), 0, 0.20f,
                            0.15f, &x, &y);
  CHECK(!filter.active);
  CHECK(x == 0.0f && y == 0.0f);

  nxinput_core_filter_stick(&filter, INT16_MAX, INT16_MAX, 0.20f, 0.15f,
                            &x, &y);
  magnitude = sqrtf(x * x + y * y);
  CHECK(nearf(magnitude, 1.0f, 0.0001f));
  CHECK(nearf(x, y, 0.0001f));
}

static void test_tap_latch_and_release(void) {
  nxinput_core_pad pad;
  int quit_requested = 0;
  uint32_t a = NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_A);
  uint32_t b = NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_B);

  nxinput_core_pad_init(&pad);
  nxinput_core_set_button(&pad, NXINPUT_BUTTON_A, 1, 0, &quit_requested);
  nxinput_core_set_button(&pad, NXINPUT_BUTTON_A, 0, 0, &quit_requested);
  CHECK((pad.buttons & a) == 0u);
  CHECK((pad.pressed_latch & a) != 0u);
  CHECK((pad.released_latch & a) != 0u);

  nxinput_core_set_button(&pad, NXINPUT_BUTTON_B, 1, 0, &quit_requested);
  CHECK((pad.buttons & b) != 0u);
  nxinput_core_release_all(&pad);
  CHECK(pad.buttons == 0u);
  CHECK(pad.pressed_latch == 0u);
  CHECK((pad.released_latch & b) != 0u);
  CHECK(pad.left_x == 0.0f && pad.right_trigger == 0.0f);
}

static void test_quit_chord_is_edge_latched(void) {
  nxinput_core_pad pad;
  int quit_requested = 0;

  nxinput_core_pad_init(&pad);
  nxinput_core_set_button(&pad, NXINPUT_BUTTON_BACK, 1, 0,
                          &quit_requested);
  CHECK(!quit_requested);
  nxinput_core_set_button(&pad, NXINPUT_BUTTON_START, 1, 0,
                          &quit_requested);
  CHECK(quit_requested);

  quit_requested = 0;
  nxinput_core_set_button(&pad, NXINPUT_BUTTON_START, 1, 0,
                          &quit_requested);
  CHECK(!quit_requested);
  nxinput_core_set_button(&pad, NXINPUT_BUTTON_START, 0, 0,
                          &quit_requested);
  nxinput_core_set_button(&pad, NXINPUT_BUTTON_START, 1, 0,
                          &quit_requested);
  CHECK(quit_requested);
}

static void test_cursor_is_right_stick_r3_menu_only(void) {
  nxinput_core_pad pad;
  nxinput_cursor_state state;
  int16_t axes[NXINPUT_CORE_AXIS_COUNT] = {0, 0, INT16_MAX, 0, 0, 0};
  int quit_requested = 0;
  unsigned int frame;

  nxinput_core_pad_init(&pad);

  /* A and D-pad never become a cursor click, even in a menu. */
  nxinput_core_set_button(&pad, NXINPUT_BUTTON_A, 1, 1, &quit_requested);
  nxinput_core_set_button(&pad, NXINPUT_BUTTON_DPAD_RIGHT, 1, 1,
                          &quit_requested);
  CHECK(!pad.cursor_click_latch);

  /* R3 in gameplay remains a normal pad button, with no cursor side effect. */
  nxinput_core_set_button(&pad, NXINPUT_BUTTON_RIGHT_STICK, 1, 0,
                          &quit_requested);
  CHECK(!pad.cursor_click_latch);
  nxinput_core_set_button(&pad, NXINPUT_BUTTON_RIGHT_STICK, 0, 0,
                          &quit_requested);

  nxinput_core_set_button(&pad, NXINPUT_BUTTON_RIGHT_STICK, 1, 1,
                          &quit_requested);
  CHECK(pad.cursor_click_latch);
  CHECK((pad.buttons & NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_RIGHT_STICK)) != 0u);

  nxinput_core_set_axes(&pad, axes, 0.20f, 0.15f, 0.05f);
  for (frame = 0u; frame < 10u; frame++)
    nxinput_core_cursor_update(&pad, 1.0f / 60.0f, 1.25f, 0.045f,
                               &state);
  CHECK(state.active);
  CHECK(state.x > 0.5f);
  CHECK(nearf(state.y, 0.5f, 0.00001f));
  CHECK(state.click_pending);

  nxinput_core_release_all(&pad);
  CHECK(!pad.cursor_click_latch);
  CHECK(pad.cursor_velocity_x == 0.0f);
}

static void test_cursor_large_dt_is_clamped(void) {
  nxinput_core_pad pad_a;
  nxinput_core_pad pad_b;
  nxinput_cursor_state state_a;
  nxinput_cursor_state state_b;
  int16_t axes[NXINPUT_CORE_AXIS_COUNT] = {0, 0, INT16_MAX, 0, 0, 0};

  /* A debugger pause or a stalled frame (huge dt) must not fling the cursor
   * across the screen; dt beyond the clamp must behave exactly like dt=0.1s. */
  nxinput_core_pad_init(&pad_a);
  nxinput_core_set_axes(&pad_a, axes, 0.20f, 0.15f, 0.05f);
  nxinput_core_cursor_update(&pad_a, 0.1f, 1.25f, 0.045f, &state_a);

  nxinput_core_pad_init(&pad_b);
  nxinput_core_set_axes(&pad_b, axes, 0.20f, 0.15f, 0.05f);
  nxinput_core_cursor_update(&pad_b, 0.2f, 1.25f, 0.045f, &state_b);

  CHECK(nearf(state_a.x, state_b.x, 0.00001f));
  CHECK(state_b.x > 0.5f);
  CHECK(state_b.x < 0.5f + 0.2f);
}

static void test_public_lifecycle(void) {
  nxinput_config config;
  nxinput_context *input;
  nxinput_pad_state state;
  unsigned int slot;

  CHECK(SDL_setenv("NXINPUT_ANALOG_STICKS_HINT", "invalid", 1) == 0);
  nxinput_config_init(&config);
  config.rescan_interval_ms = 0u;
  input = nxinput_create(&config);
  CHECK(input != NULL);
  CHECK(nxinput_connected_count(input) <= NXINPUT_MAX_PADS);
  CHECK(nxinput_get_cursor_context(input) == NXINPUT_CURSOR_OFF);
  CHECK(nxinput_host_analog_sticks_hint(input) ==
        NXINPUT_ANALOG_STICKS_HINT_UNKNOWN);
  CHECK(nxinput_host_analog_sticks_hint(NULL) ==
        NXINPUT_ANALOG_STICKS_HINT_UNKNOWN);

  nxinput_set_cursor_context(input, NXINPUT_CURSOR_GAMEPLAY);
  CHECK(nxinput_get_cursor_context(input) == NXINPUT_CURSOR_GAMEPLAY);
  nxinput_set_focus(input, 0);
  for (slot = 0u; slot < NXINPUT_MAX_PADS; slot++) {
    CHECK(nxinput_get_pad(input, slot, &state));
    CHECK(!state.focused);
    CHECK(state.buttons == 0u);
    CHECK(state.left_x == 0.0f && state.right_y == 0.0f);
  }
  nxinput_set_focus(input, 1);
  nxinput_poll(input);
  nxinput_destroy(input);
  CHECK(SDL_setenv("NXINPUT_ANALOG_STICKS_HINT", "", 1) == 0);
}

static void test_mapping_file_hint(void) {
  static const char *const guid_text =
      "03000000deadbeef0000000000000000";
  static const char *const mapping_line =
      "03000000deadbeef0000000000000000,nxinput file fixture,"
      "a:b0,platform:Linux,\n";
  char path[] = "/tmp/nxinput-mapping-XXXXXX";
  nxinput_config config;
  nxinput_context *input;
  SDL_JoystickGUID guid;
  char *loaded;
  FILE *stream;
  int descriptor;

  descriptor = mkstemp(path);
  CHECK(descriptor >= 0);
  stream = fdopen(descriptor, "w");
  CHECK(stream != NULL);
  CHECK(fputs(mapping_line, stream) >= 0);
  CHECK(fclose(stream) == 0);
  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG", "", 1) == 0);
  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG_FILE", path, 1) == 0);

  nxinput_config_init(&config);
  config.rescan_interval_ms = 0u;
  input = nxinput_create(&config);
  CHECK(input != NULL);
  guid = SDL_JoystickGetGUIDFromString(guid_text);
  loaded = SDL_GameControllerMappingForGUID(guid);
  CHECK(loaded != NULL);
  CHECK(strstr(loaded, "nxinput file fixture") != NULL);
  SDL_free(loaded);
  nxinput_destroy(input);

  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG_FILE", "", 1) == 0);
  CHECK(unlink(path) == 0);
}

/* The inherited PortMaster mapping arrives as a single multi-line
 * SDL_GAMECONTROLLERCONFIG value. The parser must walk every line, skip blanks,
 * comments and surrounding whitespace/CR, and still load each valid entry. */
static void test_mapping_multiline_env_parser(void) {
  static const char *const guid_text = "03000000feedface0000000000000000";
  static const char *const env =
      "# leading comment line\r\n"
      "   \r\n"
      "  03000000feedface0000000000000000,nxinput first fixture,a:b0,"
      "platform:Linux,  \r\n"
      "\t03000000feedface0000000000000000,nxinput REPLACES via second line,"
      "a:b0,platform:Linux,\n";
  nxinput_config config;
  nxinput_context *input;
  SDL_JoystickGUID guid;
  char *loaded;

  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG", env, 1) == 0);
  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG_FILE", "", 1) == 0);

  nxinput_config_init(&config);
  config.rescan_interval_ms = 0u;
  input = nxinput_create(&config);
  CHECK(input != NULL);
  guid = SDL_JoystickGetGUIDFromString(guid_text);
  loaded = SDL_GameControllerMappingForGUID(guid);
  CHECK(loaded != NULL);
  /* Both valid lines targeted the same GUID; SDL replaces on the second add, so
   * the comment/blank/whitespace lines were skipped without aborting the walk
   * and the final replacement landed. */
  CHECK(strstr(loaded, "REPLACES via second line") != NULL);
  SDL_free(loaded);
  nxinput_destroy(input);

  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG", "", 1) == 0);
}

#if SDL_VERSION_ATLEAST(2, 0, 14)
static void observe_all_events(nxinput_context *input) {
  SDL_Event event;
  while (SDL_PollEvent(&event))
    nxinput_observe_event(input, &event);
}

/* A mapped digital controller is a valid SDL_GameController even with no
 * axes. The raw/default view must preserve that topology, while the explicit
 * adapter option may duplicate its complete D-pad into the missing left
 * stick without consuming the D-pad buttons. */
static void test_zero_stick_capabilities_and_opt_in(void) {
  const Uint32 subsystems =
      SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER;
  nxinput_config config;
  nxinput_context *input;
  nxinput_pad_state raw_state;
  nxinput_pad_state adapted_state;
  SDL_Joystick *joystick;
  SDL_JoystickGUID guid;
  SDL_JoystickID instance_id;
  char guid_text[NXINPUT_GUID_MAX];
  char mapping[512];
  uint32_t capabilities = UINT32_MAX;
  int device_index;
  int slot;
  uint32_t right = NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_RIGHT);
  uint32_t up = NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_UP);

  CHECK(SDL_InitSubSystem(subsystems) == 0);
  device_index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                           0, 15, 0);
  CHECK(device_index >= 0);
  guid = SDL_JoystickGetDeviceGUID(device_index);
  SDL_JoystickGetGUIDString(guid, guid_text, (int)sizeof(guid_text));
  CHECK(snprintf(mapping, sizeof(mapping),
                 "%s,nxinput digital,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,"
                 "start:b6,leftshoulder:b9,rightshoulder:b10,"
                 "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,platform:Linux,",
                 guid_text) > 0);
  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG", mapping, 1) == 0);
  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG_FILE", "", 1) == 0);
  CHECK(SDL_setenv("NXINPUT_ANALOG_STICKS_HINT", "0", 1) == 0);

  joystick = SDL_JoystickOpen(device_index);
  CHECK(joystick != NULL);
  instance_id = SDL_JoystickInstanceID(joystick);
  CHECK(instance_id >= 0);

  nxinput_config_init(&config);
  config.initialize_sdl = 0;
  config.rescan_interval_ms = 0u;
  input = nxinput_create(&config);
  CHECK(input != NULL);
  CHECK(nxinput_host_analog_sticks_hint(input) == 0);
  slot = nxinput_find_instance(input, (int32_t)instance_id);
  CHECK(slot >= 0);
  CHECK(nxinput_get_pad_capabilities(input, (unsigned int)slot,
                                     &capabilities));
  CHECK(capabilities == NXINPUT_PAD_CAP_DPAD);

  CHECK(SDL_JoystickSetVirtualButton(joystick, 14, SDL_PRESSED) == 0);
  nxinput_poll(input);
  CHECK(nxinput_get_pad(input, (unsigned int)slot, &raw_state));
  CHECK((raw_state.buttons & right) != 0u);
  CHECK(raw_state.left_x == 0.0f && raw_state.left_y == 0.0f);
  CHECK(nxinput_get_pad_with_options(
      input, (unsigned int)slot,
      NXINPUT_PAD_OPTION_DPAD_LEFT_STICK_IF_MISSING, &adapted_state));
  CHECK((adapted_state.buttons & right) != 0u);
  CHECK(adapted_state.left_x == 1.0f && adapted_state.left_y == 0.0f);

  CHECK(SDL_JoystickSetVirtualButton(joystick, 11, SDL_PRESSED) == 0);
  nxinput_poll(input);
  CHECK(nxinput_get_pad_with_options(
      input, (unsigned int)slot,
      NXINPUT_PAD_OPTION_DPAD_LEFT_STICK_IF_MISSING, &adapted_state));
  CHECK((adapted_state.buttons & (right | up)) == (right | up));
  CHECK(nearf(adapted_state.left_x, 0.70710678f, 0.00001f));
  CHECK(nearf(adapted_state.left_y, -0.70710678f, 0.00001f));

  nxinput_set_focus(input, 0);
  CHECK(nxinput_get_pad_with_options(
      input, (unsigned int)slot,
      NXINPUT_PAD_OPTION_DPAD_LEFT_STICK_IF_MISSING, &adapted_state));
  CHECK(adapted_state.buttons == 0u);
  CHECK(adapted_state.left_x == 0.0f && adapted_state.left_y == 0.0f);

  nxinput_destroy(input);
  SDL_JoystickClose(joystick);
  CHECK(SDL_JoystickDetachVirtual(device_index) == 0);
  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG", "", 1) == 0);
  CHECK(SDL_setenv("NXINPUT_ANALOG_STICKS_HINT", "", 1) == 0);
  SDL_QuitSubSystem(subsystems);
}

/* A real left stick always wins, even when the host hint is contradictory and
 * the adapter asks for the digital fallback. This is the one-stick boundary. */
static void test_one_stick_is_never_overridden(void) {
  const Uint32 subsystems =
      SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER;
  nxinput_config config;
  nxinput_context *input;
  nxinput_pad_state state;
  SDL_Joystick *joystick;
  SDL_JoystickGUID guid;
  SDL_JoystickID instance_id;
  SDL_Event remap_event;
  char guid_text[NXINPUT_GUID_MAX];
  char mapping[576];
  char partial_mapping[576];
  uint32_t capabilities = 0u;
  uint32_t generation;
  int device_index;
  int slot;

  CHECK(SDL_InitSubSystem(subsystems) == 0);
  device_index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                           2, 15, 0);
  CHECK(device_index >= 0);
  guid = SDL_JoystickGetDeviceGUID(device_index);
  SDL_JoystickGetGUIDString(guid, guid_text, (int)sizeof(guid_text));
  CHECK(snprintf(mapping, sizeof(mapping),
                 "%s,nxinput one stick,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,"
                 "start:b6,leftshoulder:b9,rightshoulder:b10,"
                 "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
                 "leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux,",
                 guid_text) > 0);
  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG", mapping, 1) == 0);
  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG_FILE", "", 1) == 0);
  CHECK(SDL_setenv("NXINPUT_ANALOG_STICKS_HINT", "2", 1) == 0);

  joystick = SDL_JoystickOpen(device_index);
  CHECK(joystick != NULL);
  instance_id = SDL_JoystickInstanceID(joystick);
  CHECK(instance_id >= 0);

  nxinput_config_init(&config);
  config.initialize_sdl = 0;
  config.rescan_interval_ms = 0u;
  input = nxinput_create(&config);
  CHECK(input != NULL);
  slot = nxinput_find_instance(input, (int32_t)instance_id);
  CHECK(slot >= 0);
  CHECK(nxinput_get_pad_capabilities(input, (unsigned int)slot,
                                     &capabilities));
  CHECK((capabilities & NXINPUT_PAD_CAP_DPAD) != 0u);
  CHECK((capabilities & NXINPUT_PAD_CAP_LEFT_STICK) != 0u);
  CHECK((capabilities & NXINPUT_PAD_CAP_RIGHT_STICK) == 0u);

  /* The mapping and host hint both advertise a second stick, but this
   * controller physically has only axes 0/1. Unreachable a2/a3 bindings must
   * not manufacture a capability. Default cursor input stays on the missing
   * right stick; an explicit MENU-only option may use the real left stick. */
  {
    nxinput_cursor_state default_cursor;
    nxinput_cursor_state fallback_cursor;
    CHECK(SDL_JoystickSetVirtualAxis(joystick, 0, INT16_MAX) == 0);
    nxinput_poll(input);
    nxinput_set_cursor_context(input, NXINPUT_CURSOR_MENU);
    CHECK(nxinput_cursor_update(input, (unsigned int)slot, 0.1f,
                                &default_cursor));
    CHECK(nearf(default_cursor.x, 0.5f, 0.00001f));
    CHECK(nxinput_cursor_update_with_options(
        input, (unsigned int)slot, 0.1f,
        NXINPUT_CURSOR_OPTION_LEFT_STICK_IF_RIGHT_MISSING,
        &fallback_cursor));
    CHECK(fallback_cursor.x > 0.5f);
    CHECK(!nxinput_cursor_update_with_options(
        input, (unsigned int)slot, 0.1f, UINT32_C(0x80000000),
        &fallback_cursor));
    nxinput_set_cursor_context(input, NXINPUT_CURSOR_GAMEPLAY);
  }

  CHECK(SDL_JoystickSetVirtualAxis(joystick, 0, INT16_MAX) == 0);
  CHECK(SDL_JoystickSetVirtualButton(joystick, 13, SDL_PRESSED) == 0);
  nxinput_poll(input);
  CHECK(nxinput_get_pad_with_options(
      input, (unsigned int)slot,
      NXINPUT_PAD_OPTION_DPAD_LEFT_STICK_IF_MISSING, &state));
  CHECK(state.left_x > 0.95f);
  CHECK((state.buttons & NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_LEFT)) != 0u);

  /* A remap that exposes only one left-stick axis is not a complete stick,
   * but that real axis must still block the digital fallback. The topology
   * generation changes once and the centered/missing Y axis remains raw. */
  generation = state.generation;
  CHECK(snprintf(partial_mapping, sizeof(partial_mapping),
                 "%s,nxinput partial stick,a:b0,b:b1,x:b2,y:b3,back:b4,"
                 "guide:b5,start:b6,leftshoulder:b9,rightshoulder:b10,"
                 "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
                 "leftx:a0,platform:Linux,",
                 guid_text) > 0);
  CHECK(SDL_GameControllerAddMapping(partial_mapping) >= 0);
  memset(&remap_event, 0, sizeof(remap_event));
  remap_event.type = SDL_CONTROLLERDEVICEREMAPPED;
  remap_event.cdevice.which = instance_id;
  nxinput_observe_event(input, &remap_event);
  CHECK(nxinput_get_pad_capabilities(input, (unsigned int)slot,
                                     &capabilities));
  CHECK((capabilities & NXINPUT_PAD_CAP_DPAD) != 0u);
  CHECK((capabilities & NXINPUT_PAD_CAP_LEFT_STICK) == 0u);
  CHECK(SDL_JoystickSetVirtualAxis(joystick, 0, INT16_MAX) == 0);
  nxinput_poll(input);
  CHECK(nxinput_get_pad_with_options(
      input, (unsigned int)slot,
      NXINPUT_PAD_OPTION_DPAD_LEFT_STICK_IF_MISSING, &state));
  CHECK(state.generation > generation);
  CHECK(state.left_x > 0.95f && state.left_y == 0.0f);
  CHECK((state.buttons & NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_DPAD_LEFT)) != 0u);

  nxinput_destroy(input);
  SDL_JoystickClose(joystick);
  CHECK(SDL_JoystickDetachVirtual(device_index) == 0);
  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG", "", 1) == 0);
  CHECK(SDL_setenv("NXINPUT_ANALOG_STICKS_HINT", "", 1) == 0);
  SDL_QuitSubSystem(subsystems);
}

static void test_capability_mapping_repair(void) {
  const Uint32 subsystems =
      SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER;
  nxinput_config config;
  nxinput_context *input;
  nxinput_pad_state state;
  SDL_Joystick *joystick;
  SDL_JoystickGUID guid;
  SDL_JoystickID instance_id;
  char guid_text[NXINPUT_GUID_MAX];
  char mapping[768];
  char *loaded;
  int device_index;
  int slot;
  uint32_t a = NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_A);
  uint32_t b = NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_B);
  uint32_t guide = NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_GUIDE);
  uint32_t r3 = NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_RIGHT_STICK);

  CHECK(SDL_InitSubSystem(subsystems) == 0);
  device_index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                           4, 12, 1);
  CHECK(device_index >= 0);
  guid = SDL_JoystickGetDeviceGUID(device_index);
  SDL_JoystickGetGUIDString(guid, guid_text, (int)sizeof(guid_text));
  CHECK(snprintf(
            mapping, sizeof(mapping),
            "%s,nxinput capability fixture,a:b0,b:b1,x:b2,y:b3,"
            "leftshoulder:b4,rightshoulder:b5,start:b6,back:b7,"
            "leftstick:b8,guide:b9,lefttrigger:b10,righttrigger:b11,"
            "leftx:a0~,lefty:a1~,rightx:a2,righty:a3,"
            "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
            "platform:Linux,",
            guid_text) > 0);
  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG", mapping, 1) == 0);
  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG_FILE", "", 1) == 0);

  joystick = SDL_JoystickOpen(device_index);
  CHECK(joystick != NULL);
  instance_id = SDL_JoystickInstanceID(joystick);
  CHECK(instance_id >= 0);

  nxinput_config_init(&config);
  config.initialize_sdl = 0;
  config.rescan_interval_ms = 0u;
  input = nxinput_create(&config);
  CHECK(input != NULL);
  slot = nxinput_find_instance(input, (int32_t)instance_id);
  CHECK(slot >= 0);

  loaded = SDL_GameControllerMappingForGUID(guid);
  CHECK(loaded != NULL);
  CHECK(strstr(loaded, ",a:b1,") != NULL);
  CHECK(strstr(loaded, ",b:b0,") != NULL);
  CHECK(strstr(loaded, ",x:b3,") != NULL);
  CHECK(strstr(loaded, ",y:b2,") != NULL);
  CHECK(strstr(loaded, ",rightstick:b9,") != NULL);
  CHECK(strstr(loaded, ",guide:b9,") == NULL);
  CHECK(strstr(loaded, ",leftx:a0~,") != NULL);
  SDL_free(loaded);

  CHECK(SDL_JoystickSetVirtualButton(joystick, 9, SDL_PRESSED) == 0);
  nxinput_poll(input);
  CHECK(nxinput_get_pad(input, (unsigned int)slot, &state));
  CHECK((state.buttons & r3) != 0u);
  CHECK((state.buttons & guide) == 0u);
  CHECK(SDL_JoystickSetVirtualButton(joystick, 9, SDL_RELEASED) == 0);

  CHECK(SDL_JoystickSetVirtualButton(joystick, 1, SDL_PRESSED) == 0);
  nxinput_poll(input);
  CHECK(nxinput_get_pad(input, (unsigned int)slot, &state));
  CHECK((state.buttons & a) != 0u);
  CHECK((state.buttons & b) == 0u);
  CHECK(SDL_JoystickSetVirtualButton(joystick, 1, SDL_RELEASED) == 0);
  CHECK(SDL_JoystickSetVirtualButton(joystick, 0, SDL_PRESSED) == 0);
  nxinput_poll(input);
  CHECK(nxinput_get_pad(input, (unsigned int)slot, &state));
  CHECK((state.buttons & a) == 0u);
  CHECK((state.buttons & b) != 0u);

  nxinput_destroy(input);
  SDL_JoystickClose(joystick);
  CHECK(SDL_JoystickDetachVirtual(device_index) == 0);
  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG", "", 1) == 0);
  SDL_QuitSubSystem(subsystems);
}

static void test_capability_mapping_repair_rejects_near_miss(void) {
  const Uint32 subsystems =
      SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER;
  nxinput_config config;
  nxinput_context *input;
  SDL_JoystickGUID guid;
  char guid_text[NXINPUT_GUID_MAX];
  char mapping[640];
  char *loaded;
  int device_index;

  CHECK(SDL_InitSubSystem(subsystems) == 0);
  device_index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                           4, 12, 0);
  CHECK(device_index >= 0);
  guid = SDL_JoystickGetDeviceGUID(device_index);
  SDL_JoystickGetGUIDString(guid, guid_text, (int)sizeof(guid_text));
  CHECK(snprintf(
            mapping, sizeof(mapping),
            "%s,nxinput near miss,a:b0,b:b1,x:b2,y:b3,"
            "leftshoulder:b4,rightshoulder:b5,back:b6,start:b7,"
            "leftstick:b8,guide:b9,lefttrigger:b10,righttrigger:b11,"
            "leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux,",
            guid_text) > 0);
  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG", mapping, 1) == 0);

  nxinput_config_init(&config);
  config.initialize_sdl = 0;
  config.rescan_interval_ms = 0u;
  input = nxinput_create(&config);
  CHECK(input != NULL);
  loaded = SDL_GameControllerMappingForGUID(guid);
  CHECK(loaded != NULL);
  CHECK(strstr(loaded, ",a:b0,") != NULL);
  CHECK(strstr(loaded, ",guide:b9,") != NULL);
  CHECK(strstr(loaded, ",rightstick:b9,") == NULL);
  SDL_free(loaded);

  nxinput_destroy(input);
  CHECK(SDL_JoystickDetachVirtual(device_index) == 0);
  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG", "", 1) == 0);
  SDL_QuitSubSystem(subsystems);
}

static void test_virtual_controller_hotplug(void) {
  const Uint32 subsystems =
      SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER;
  nxinput_config config;
  nxinput_context *input;
  nxinput_pad_state state;
  SDL_Joystick *joystick;
  SDL_JoystickGUID guid;
  SDL_JoystickID instance_id;
  char guid_text[NXINPUT_GUID_MAX];
  char mapping[512];
  int device_index;
  int slot;
  uint32_t capabilities = 0u;
  uint32_t a = NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_A);

  CHECK(SDL_InitSubSystem(subsystems) == 0);
  device_index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                           (int)NXINPUT_CORE_AXIS_COUNT,
                                           (int)NXINPUT_BUTTON_COUNT, 0);
  CHECK(device_index >= 0);

  guid = SDL_JoystickGetDeviceGUID(device_index);
  SDL_JoystickGetGUIDString(guid, guid_text, (int)sizeof(guid_text));
  CHECK(snprintf(mapping, sizeof(mapping),
                 "%s,nxinput virtual,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,"
                 "start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,"
                 "rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,"
                 "dpright:b14,leftx:a0,lefty:a1,rightx:a2,righty:a3,"
                 "lefttrigger:a4,righttrigger:a5,platform:Linux,",
                 guid_text) > 0);
  /* SDL was initialized before the PortMaster-style value appeared. nxinput
   * must still inherit and apply it before its initial controller scan. */
  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG", mapping, 1) == 0);
  CHECK(SDL_setenv("NXINPUT_ANALOG_STICKS_HINT", "2", 1) == 0);

  joystick = SDL_JoystickOpen(device_index);
  CHECK(joystick != NULL);
  instance_id = SDL_JoystickInstanceID(joystick);
  CHECK(instance_id >= 0);

  nxinput_config_init(&config);
  config.initialize_sdl = 0;
  config.rescan_interval_ms = 0u;
  input = nxinput_create(&config);
  CHECK(input != NULL);
  slot = nxinput_find_instance(input, (int32_t)instance_id);
  CHECK(slot >= 0);
  CHECK(nxinput_host_analog_sticks_hint(input) == 2);
  CHECK(nxinput_get_pad_capabilities(input, (unsigned int)slot,
                                     &capabilities));
  CHECK(capabilities == NXINPUT_PAD_CAP_MASK_ALL);

  CHECK(SDL_JoystickSetVirtualButton(joystick, 0, SDL_PRESSED) == 0);
  nxinput_poll(input);
  CHECK(nxinput_get_pad(input, (unsigned int)slot, &state));
  CHECK((state.buttons & a) != 0u);

  CHECK(SDL_JoystickSetVirtualButton(joystick, 0, SDL_RELEASED) == 0);
  nxinput_poll(input);
  CHECK(nxinput_get_pad(input, (unsigned int)slot, &state));
  CHECK((state.buttons & a) == 0u);
  (void)nxinput_consume_pressed(input, (unsigned int)slot,
                                NXINPUT_BUTTON_MASK_ALL);
  (void)nxinput_consume_released(input, (unsigned int)slot,
                                 NXINPUT_BUTTON_MASK_ALL);

  /* Both edges are observed before the next nxinput_poll: the short tap still
   * reaches the consumable press latch. */
  CHECK(SDL_JoystickSetVirtualButton(joystick, 0, SDL_PRESSED) == 0);
  SDL_PumpEvents();
  observe_all_events(input);
  CHECK(SDL_JoystickSetVirtualButton(joystick, 0, SDL_RELEASED) == 0);
  SDL_PumpEvents();
  observe_all_events(input);
  CHECK(nxinput_get_pad(input, (unsigned int)slot, &state));
  CHECK((state.buttons & a) == 0u);
  CHECK((nxinput_consume_pressed(input, (unsigned int)slot, a) & a) != 0u);

  CHECK(SDL_JoystickSetVirtualAxis(joystick, 2, INT16_MAX) == 0);
  nxinput_poll(input);
  CHECK(nxinput_get_pad(input, (unsigned int)slot, &state));
  CHECK(state.right_x > 0.95f);

  CHECK(SDL_JoystickSetVirtualButton(joystick, 0, SDL_PRESSED) == 0);
  nxinput_poll(input);
  nxinput_set_focus(input, 0);
  CHECK(nxinput_get_pad(input, (unsigned int)slot, &state));
  CHECK(state.buttons == 0u);
  CHECK(state.right_x == 0.0f);
  CHECK((state.released_latch & a) != 0u);

  SDL_JoystickClose(joystick);
  CHECK(SDL_JoystickDetachVirtual(device_index) == 0);
  SDL_PumpEvents();
  observe_all_events(input);
  nxinput_poll(input);
  CHECK(nxinput_find_instance(input, (int32_t)instance_id) < 0);

  nxinput_destroy(input);
  CHECK(SDL_setenv("NXINPUT_ANALOG_STICKS_HINT", "", 1) == 0);
  SDL_QuitSubSystem(subsystems);
}

/* Every virtual pad shares SDL's default virtual GUID, so a single mapping line
 * built from that GUID covers all attachments while SDL still assigns distinct
 * instance IDs. Read it from a throwaway probe. */
static void nxinput_read_virtual_guid(char *out, size_t out_size) {
  int device_index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                               (int)NXINPUT_CORE_AXIS_COUNT,
                                               (int)NXINPUT_BUTTON_COUNT, 0);
  SDL_JoystickGUID guid;
  CHECK(device_index >= 0);
  guid = SDL_JoystickGetDeviceGUID(device_index);
  SDL_JoystickGetGUIDString(guid, out, (int)out_size);
  CHECK(SDL_JoystickDetachVirtual(device_index) == 0);
  /* Drop the probe's hotplug events so they cannot perturb the real test. */
  SDL_PumpEvents();
  for (;;) {
    SDL_Event event;
    if (SDL_PollEvent(&event) == 0)
      break;
  }
}

static void nxinput_build_virtual_mapping(char *buf, size_t buf_size,
                                          const char *guid_text) {
  snprintf(buf, buf_size,
           "%s,nxinput virtual,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,"
           "start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,"
           "rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,"
           "dpright:b14,leftx:a0,lefty:a1,rightx:a2,righty:a3,"
           "lefttrigger:a4,righttrigger:a5,platform:Linux,", guid_text);
}

static int nxinput_attach_virtual(SDL_Joystick **out_joystick,
                                  SDL_JoystickID *out_instance,
                                  int *out_device_index) {
  int device_index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                               (int)NXINPUT_CORE_AXIS_COUNT,
                                               (int)NXINPUT_BUTTON_COUNT, 0);
  SDL_Joystick *joystick;
  if (device_index < 0)
    return 0;
  joystick = SDL_JoystickOpen(device_index);
  if (!joystick)
    return 0;
  *out_joystick = joystick;
  *out_instance = SDL_JoystickInstanceID(joystick);
  if (out_device_index)
    *out_device_index = device_index;
  return 1;
}

static void drain_controller_added_events(void) {
  for (;;) {
    SDL_Event event;
    int n = SDL_PeepEvents(&event, 1, SDL_GETEVENT,
                           SDL_CONTROLLERDEVICEADDED,
                           SDL_CONTROLLERDEVICEADDED);
    if (n <= 0)
      break;
  }
}

/* Two independent pads must land in distinct slots with distinct instance IDs,
 * keep independent button state, and a pad beyond NXINPUT_MAX_PADS is rejected
 * rather than overwriting another player. */
static void test_two_controllers_distinct_and_beyond_limit(void) {
  const Uint32 subsystems =
      SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER;
  char mapping[512];
  char guid_text[NXINPUT_GUID_MAX];
  nxinput_config config;
  nxinput_context *input;
  SDL_Joystick *joy_a = NULL;
  SDL_Joystick *joy_b = NULL;
  SDL_JoystickID id_a = 0;
  SDL_JoystickID id_b = 0;
  nxinput_pad_state state;
  int slot_a;
  int slot_b;
  unsigned int extra;

  CHECK(SDL_InitSubSystem(subsystems) == 0);
  nxinput_read_virtual_guid(guid_text, sizeof(guid_text));
  nxinput_build_virtual_mapping(mapping, sizeof(mapping), guid_text);
  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG", mapping, 1) == 0);

  CHECK(nxinput_attach_virtual(&joy_a, &id_a, NULL));
  CHECK(nxinput_attach_virtual(&joy_b, &id_b, NULL));
  CHECK(id_a != id_b);

  nxinput_config_init(&config);
  config.initialize_sdl = 0;
  config.rescan_interval_ms = 0u;
  input = nxinput_create(&config);
  CHECK(input != NULL);
  CHECK(nxinput_connected_count(input) >= 2u);
  slot_a = nxinput_find_instance(input, (int32_t)id_a);
  slot_b = nxinput_find_instance(input, (int32_t)id_b);
  CHECK(slot_a >= 0 && slot_b >= 0 && slot_a != slot_b);

  /* Pressing A on pad A must not light A on pad B. */
  CHECK(SDL_JoystickSetVirtualButton(joy_a, 0, SDL_PRESSED) == 0);
  nxinput_poll(input);
  CHECK(nxinput_get_pad(input, (unsigned int)slot_a, &state));
  CHECK((state.buttons & NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_A)) != 0u);
  CHECK(nxinput_get_pad(input, (unsigned int)slot_b, &state));
  CHECK((state.buttons & NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_A)) == 0u);

  /* Fill every remaining slot; NXINPUT_MAX_PADS is the hard ceiling. With
   * rescan disabled, each new pad is opened only when nxinput observes its
   * CONTROLLERDEVICEADDED event. */
  for (extra = nxinput_connected_count(input); extra < NXINPUT_MAX_PADS;
       extra++) {
    SDL_Joystick *filler = NULL;
    SDL_JoystickID fill_id = 0;
    CHECK(nxinput_attach_virtual(&filler, &fill_id, NULL));
  }
  SDL_PumpEvents();
  observe_all_events(input);
  nxinput_poll(input);
  CHECK(nxinput_connected_count(input) == NXINPUT_MAX_PADS);

  /* One more attachment beyond the ceiling is refused, not folded into P1. */
  {
    SDL_Joystick *overflow = NULL;
    SDL_JoystickID overflow_id = 0;
    CHECK(nxinput_attach_virtual(&overflow, &overflow_id, NULL));
    SDL_PumpEvents();
    observe_all_events(input);
    nxinput_poll(input);
    CHECK(nxinput_connected_count(input) == NXINPUT_MAX_PADS);
    SDL_JoystickClose(overflow);
  }

  SDL_JoystickClose(joy_a);
  SDL_JoystickClose(joy_b);
  nxinput_destroy(input);
  SDL_QuitSubSystem(subsystems);
}

/* A hotplug SDL_CONTROLLERDEVICEADDED that nxinput never observes must still be
 * recovered: a subsequent button event for the unknown instance triggers the
 * conservative rescan path, which opens the now-known device. */
static void test_lost_added_event_rescan_recovery(void) {
  const Uint32 subsystems =
      SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER;
  char mapping[512];
  char guid_text[NXINPUT_GUID_MAX];
  nxinput_config config;
  nxinput_context *input;
  SDL_Joystick *joystick = NULL;
  SDL_GameController *foreign_controller = NULL;
  SDL_JoystickID instance_id = 0;
  int device_index = 0;

  CHECK(SDL_InitSubSystem(subsystems) == 0);
  nxinput_read_virtual_guid(guid_text, sizeof(guid_text));
  nxinput_build_virtual_mapping(mapping, sizeof(mapping), guid_text);
  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG", mapping, 1) == 0);

  nxinput_config_init(&config);
  config.initialize_sdl = 0;
  config.rescan_interval_ms = 0u;
  input = nxinput_create(&config);
  CHECK(input != NULL);
  CHECK(nxinput_connected_count(input) == 0u);

  /* Attach after creation so the initial scan saw nothing, then hide the ADDED
   * event from nxinput. A foreign component has still opened the device as a
   * game controller, so controller button events keep flowing. */
  CHECK(nxinput_attach_virtual(&joystick, &instance_id, &device_index));
  foreign_controller = SDL_GameControllerOpen(device_index);
  CHECK(foreign_controller != NULL);
  SDL_PumpEvents();
  drain_controller_added_events();
  CHECK(nxinput_find_instance(input, (int32_t)instance_id) < 0);

  /* A button edge for an unknown instance must provoke the fallback rescan. */
  CHECK(SDL_JoystickSetVirtualButton(joystick, 0, SDL_PRESSED) == 0);
  SDL_PumpEvents();
  observe_all_events(input);
  CHECK(nxinput_find_instance(input, (int32_t)instance_id) >= 0);
  CHECK(nxinput_connected_count(input) == 1u);

  SDL_GameControllerClose(foreign_controller);
  SDL_JoystickClose(joystick);
  (void)SDL_JoystickDetachVirtual(device_index);
  SDL_PumpEvents();
  observe_all_events(input);
  nxinput_destroy(input);
  SDL_QuitSubSystem(subsystems);
}

/* A button held down when the device vanishes must not resurface as a ghost
 * press on the next pad that reuses the slot; only the release is observable. */
static void test_disconnect_mid_press_no_ghost(void) {
  const Uint32 subsystems =
      SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER;
  char mapping[512];
  char guid_text[NXINPUT_GUID_MAX];
  nxinput_config config;
  nxinput_context *input;
  SDL_Joystick *joy_a = NULL;
  SDL_JoystickID id_a = 0;
  int device_index_a = 0;
  int slot_a;
  nxinput_pad_state state;

  CHECK(SDL_InitSubSystem(subsystems) == 0);
  nxinput_read_virtual_guid(guid_text, sizeof(guid_text));
  nxinput_build_virtual_mapping(mapping, sizeof(mapping), guid_text);
  CHECK(SDL_setenv("SDL_GAMECONTROLLERCONFIG", mapping, 1) == 0);

  CHECK(nxinput_attach_virtual(&joy_a, &id_a, &device_index_a));
  nxinput_config_init(&config);
  config.initialize_sdl = 0;
  config.rescan_interval_ms = 0u;
  input = nxinput_create(&config);
  CHECK(input != NULL);
  slot_a = nxinput_find_instance(input, (int32_t)id_a);
  CHECK(slot_a >= 0);

  CHECK(SDL_JoystickSetVirtualButton(joy_a, 0, SDL_PRESSED) == 0);
  SDL_PumpEvents();
  observe_all_events(input);
  nxinput_poll(input);
  CHECK(nxinput_get_pad(input, (unsigned int)slot_a, &state));
  CHECK((state.pressed_latch & NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_A)) != 0u);

  /* Device disappears while A is still held. */
  SDL_JoystickClose(joy_a);
  CHECK(SDL_JoystickDetachVirtual(device_index_a) == 0);
  SDL_PumpEvents();
  observe_all_events(input);
  nxinput_poll(input);
  CHECK(nxinput_find_instance(input, (int32_t)id_a) < 0);
  /* The pending press was discarded; only a release remains observable. */
  CHECK(nxinput_consume_pressed(input, (unsigned int)slot_a,
                                NXINPUT_BUTTON_BIT(NXINPUT_BUTTON_A)) == 0u);
  CHECK(nxinput_get_pad(input, (unsigned int)slot_a, &state));
  CHECK(state.buttons == 0u);

  /* A fresh pad reusing the slot starts clean. */
  {
    SDL_Joystick *joy_b = NULL;
    SDL_JoystickID id_b = 0;
    CHECK(nxinput_attach_virtual(&joy_b, &id_b, NULL));
    SDL_PumpEvents();
    observe_all_events(input);
    nxinput_poll(input);
    CHECK(nxinput_find_instance(input, (int32_t)id_b) >= 0);
    SDL_JoystickClose(joy_b);
  }

  nxinput_destroy(input);
  SDL_QuitSubSystem(subsystems);
}
#endif

int main(void) {
  test_axis_and_trigger();
  test_radial_deadzone_hysteresis();
  test_tap_latch_and_release();
  test_quit_chord_is_edge_latched();
  test_cursor_is_right_stick_r3_menu_only();
  test_cursor_large_dt_is_clamped();
  test_public_lifecycle();
  test_mapping_file_hint();
  test_mapping_multiline_env_parser();
#if SDL_VERSION_ATLEAST(2, 0, 14)
  test_zero_stick_capabilities_and_opt_in();
  test_one_stick_is_never_overridden();
  test_capability_mapping_repair();
  test_capability_mapping_repair_rejects_near_miss();
  test_virtual_controller_hotplug();
  test_two_controllers_distinct_and_beyond_limit();
  test_lost_added_event_rescan_recovery();
  test_disconnect_mid_press_no_ghost();
#endif
  (void)puts("nxinput tests: ok");
  return 0;
}
