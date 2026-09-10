/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxandroid_android_input.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_EVENTS 512u

static unsigned int checks;

#define CHECK(condition)                                                      \
  do {                                                                        \
    checks++;                                                                 \
    if (!(condition)) {                                                       \
      fprintf(stderr, "android-c7 check failed at %s:%d: %s\n", __FILE__,   \
              __LINE__, #condition);                                          \
      return 1;                                                               \
    }                                                                         \
  } while (0)

typedef struct event_log {
  nxandroid_android_event events[MAX_EVENTS];
  size_t count;
  int fail;
  int malformed_ack;
} event_log;

static int accept_event(void *userdata, const nxandroid_android_event *event,
                        nxandroid_android_ack *ack) {
  event_log *log = (event_log *)userdata;
  if (log->fail)
    return 91;
  if (log->count >= MAX_EVENTS)
    return 92;
  log->events[log->count++] = *event;
  if (!log->malformed_ack) {
    ack->api_version = NXANDROID_ANDROID_INPUT_API_VERSION;
    ack->sequence = event->sequence;
    ack->handled = 1;
    ack->return_value = (int32_t)event->kind;
  }
  return 0;
}

static void set_decision(nxandroid_android_authority *authority, int context,
                         int control, nxandroid_android_decision decision,
                         const char *action) {
  authority->decision[context][control] = (uint8_t)decision;
  if (decision == NXANDROID_ANDROID_DECIDE_ACTION && action != NULL)
    memcpy(authority->action[context][control], action, strlen(action) + 1u);
}

static void fill_authority(nxandroid_android_authority *authority) {
  int context;
  int control;
  memset(authority, 0, sizeof(*authority));
  authority->api_version = NXANDROID_ANDROID_INPUT_API_VERSION;
  authority->schema_version = 2u;
  for (context = 0; context < (int)NXANDROID_ANDROID_CONTEXT_COUNT;
       ++context) {
    authority->context_present[context] = 1u;
    for (control = 0; control < (int)NXANDROID_ANDROID_CONTROL_COUNT;
         ++control)
      set_decision(authority, context, control,
                   NXANDROID_ANDROID_DECIDE_SUPPRESS, NULL);
    set_decision(authority, context, NXANDROID_ANDROID_X,
                 NXANDROID_ANDROID_DECIDE_ACTION, "game.fire");
    set_decision(authority, context, NXANDROID_ANDROID_Y,
                 NXANDROID_ANDROID_DECIDE_ACTION, "game.push");
    set_decision(authority, context, NXANDROID_ANDROID_L1,
                 NXANDROID_ANDROID_DECIDE_ACTION, "game.touch");
    set_decision(authority, context, NXANDROID_ANDROID_R1,
                 NXANDROID_ANDROID_DECIDE_ACTION, "game.touch");
    set_decision(authority, context, NXANDROID_ANDROID_L2,
                 NXANDROID_ANDROID_DECIDE_ACTION, "game.throttle");
    set_decision(authority, context, NXANDROID_ANDROID_R2,
                 NXANDROID_ANDROID_DECIDE_ACTION, "game.pull");
    set_decision(authority, context, NXANDROID_ANDROID_L3,
                 NXANDROID_ANDROID_DECIDE_NATIVE, NULL);
    set_decision(authority, context, NXANDROID_ANDROID_R3,
                 NXANDROID_ANDROID_DECIDE_ACTION, "cursor.click");
    set_decision(authority, context, NXANDROID_ANDROID_START,
                 NXANDROID_ANDROID_DECIDE_ACTION, "menu.pause");
    set_decision(authority, context, NXANDROID_ANDROID_UP,
                 NXANDROID_ANDROID_DECIDE_ACTION, "menu.direction");
    set_decision(authority, context, NXANDROID_ANDROID_DOWN,
                 NXANDROID_ANDROID_DECIDE_ACTION, "menu.direction");
    set_decision(authority, context, NXANDROID_ANDROID_LEFT,
                 NXANDROID_ANDROID_DECIDE_ACTION, "menu.direction");
    set_decision(authority, context, NXANDROID_ANDROID_RIGHT,
                 NXANDROID_ANDROID_DECIDE_ACTION, "menu.direction");
    set_decision(authority, context, NXANDROID_ANDROID_LEFT_STICK,
                 NXANDROID_ANDROID_DECIDE_NATIVE, NULL);
    set_decision(authority, context, NXANDROID_ANDROID_RIGHT_STICK,
                 NXANDROID_ANDROID_DECIDE_ACTION, "cursor.move");
  }
}

static const nxandroid_android_route routes[] = {
    {"game.fire", NXANDROID_ANDROID_SINK_KEY_EVENT,
     NXANDROID_ANDROID_SIGNAL_BUTTON, 99, 0, 0.0f, 0.0f},
    {"game.push", NXANDROID_ANDROID_SINK_JNI_PUSH,
     NXANDROID_ANDROID_SIGNAL_BUTTON, 7, 0, 0.0f, 0.0f},
    {"game.touch", NXANDROID_ANDROID_SINK_TOUCH,
     NXANDROID_ANDROID_SIGNAL_TOUCH_FIXED, 0, 0, 0.20f, 0.80f},
    {"game.throttle", NXANDROID_ANDROID_SINK_MOTION_EVENT,
     NXANDROID_ANDROID_SIGNAL_AXIS, 17, 0, 0.0f, 0.0f},
    {"game.pull", NXANDROID_ANDROID_SINK_JNI_PULL,
     NXANDROID_ANDROID_SIGNAL_AXIS, 18, 0, 0.0f, 0.0f},
    {"cursor.click", NXANDROID_ANDROID_SINK_TOUCH,
     NXANDROID_ANDROID_SIGNAL_TOUCH_CURSOR, 0, 0, 0.0f, 0.0f},
    {"menu.pause", NXANDROID_ANDROID_SINK_KEY_EVENT,
     NXANDROID_ANDROID_SIGNAL_BUTTON, 108, 0, 0.0f, 0.0f},
    {"menu.direction", NXANDROID_ANDROID_SINK_KEY_EVENT,
     NXANDROID_ANDROID_SIGNAL_BUTTON, 19, 0, 0.0f, 0.0f},
    {"cursor.move", NXANDROID_ANDROID_SINK_TOUCH,
     NXANDROID_ANDROID_SIGNAL_CURSOR, 0, 0, 0.0f, 0.0f},
};

static const nxandroid_android_native_route native_routes[] = {
    {NXANDROID_ANDROID_L3, NXANDROID_ANDROID_SINK_NATIVE_GAMEPAD,
     NXANDROID_ANDROID_SIGNAL_BUTTON, 106, 0},
    {NXANDROID_ANDROID_LEFT_STICK, NXANDROID_ANDROID_SINK_NATIVE_GAMEPAD,
     NXANDROID_ANDROID_SIGNAL_VECTOR, 0, 1},
};

static nxandroid_android_profile make_profile(event_log *log) {
  nxandroid_android_profile profile;
  memset(&profile, 0, sizeof(profile));
  profile.consumer_id = "c7-realistic-harness";
  profile.consumer_version = "1";
  profile.routes = routes;
  profile.route_count = sizeof(routes) / sizeof(routes[0]);
  profile.native_routes = native_routes;
  profile.native_route_count =
      sizeof(native_routes) / sizeof(native_routes[0]);
  profile.touch.drawable_width = 1280;
  profile.touch.drawable_height = 720;
  profile.touch.content_left = 80;
  profile.touch.content_top = 20;
  profile.touch.content_width = 1120;
  profile.touch.content_height = 680;
  profile.touch.safe_left = 24;
  profile.touch.safe_top = 12;
  profile.touch.safe_right = 32;
  profile.touch.safe_bottom = 18;
  profile.touch.rotation_degrees = 0;
  profile.cursor.initial_x = 0.5f;
  profile.cursor.initial_y = 0.5f;
  profile.cursor.speed_screen_heights_per_second = 1.25f;
  profile.cursor.deadzone = 0.15f;
  profile.cursor.response_curve = 1.4f;
  profile.cursor.smoothing_seconds = 0.04f;
  profile.event = accept_event;
  profile.userdata = log;
  return profile;
}

static int test_touch_geometry(void) {
  nxandroid_android_touch_geometry geometry = {640, 480, 40, 20, 560, 440,
                                                12,  8,   16, 10, 0};
  int x;
  int y;
  char error[128];
  CHECK(nxandroid_android_touch_resolve(&geometry, 0.0f, 0.0f, &x, &y,
                                        error, sizeof(error)) ==
        NXANDROID_ANDROID_OK);
  CHECK(x == 40 && y == 20);
  CHECK(nxandroid_android_touch_resolve(&geometry, 1.0f, 1.0f, &x, &y,
                                        error, sizeof(error)) ==
        NXANDROID_ANDROID_OK);
  CHECK(x == 599 && y == 459);
  geometry.rotation_degrees = 90;
  CHECK(nxandroid_android_touch_resolve(&geometry, 0.0f, 0.0f, &x, &y,
                                        error, sizeof(error)) ==
        NXANDROID_ANDROID_OK);
  CHECK(x == 599 && y == 20);
  geometry.rotation_degrees = 180;
  CHECK(nxandroid_android_touch_resolve(&geometry, 0.0f, 0.0f, &x, &y,
                                        error, sizeof(error)) ==
        NXANDROID_ANDROID_OK);
  CHECK(x == 599 && y == 459);
  geometry.rotation_degrees = 270;
  CHECK(nxandroid_android_touch_resolve(&geometry, 0.0f, 0.0f, &x, &y,
                                        error, sizeof(error)) ==
        NXANDROID_ANDROID_OK);
  CHECK(x == 40 && y == 459);
  geometry.rotation_degrees = 45;
  CHECK(nxandroid_android_touch_resolve(&geometry, 0.5f, 0.5f, &x, &y,
                                        error, sizeof(error)) ==
        NXANDROID_ANDROID_EINVAL);
  geometry.rotation_degrees = 0;
  geometry.safe_left = 630;
  CHECK(nxandroid_android_touch_resolve(&geometry, 0.5f, 0.5f, &x, &y,
                                        error, sizeof(error)) ==
        NXANDROID_ANDROID_EINVAL);
  return 0;
}

static int test_validation(void) {
  nxandroid_android_context context;
  nxandroid_android_authority authority;
  nxandroid_android_profile profile;
  nxandroid_android_route duplicate[2];
  nxandroid_android_native_route invalid_native[2];
  event_log log;
  char error[256];
  memset(&log, 0, sizeof(log));
  fill_authority(&authority);
  profile = make_profile(&log);

  duplicate[0] = routes[0];
  duplicate[1] = routes[0];
  profile.routes = duplicate;
  profile.route_count = 2u;
  CHECK(nxandroid_android_context_init(&context, &authority, &profile, error,
                                       sizeof(error)) ==
        NXANDROID_ANDROID_EDUPLICATE);

  profile = make_profile(&log);
  profile.native_routes = NULL;
  profile.native_route_count = 0u;
  CHECK(nxandroid_android_context_init(&context, &authority, &profile, error,
                                       sizeof(error)) ==
        NXANDROID_ANDROID_EINVAL);

  invalid_native[0] = native_routes[0];
  invalid_native[1] = native_routes[1];
  invalid_native[0].sink = NXANDROID_ANDROID_SINK_TOUCH;
  invalid_native[0].signal = NXANDROID_ANDROID_SIGNAL_TOUCH_FIXED;
  profile = make_profile(&log);
  profile.native_routes = invalid_native;
  CHECK(nxandroid_android_context_init(&context, &authority, &profile, error,
                                       sizeof(error)) ==
        NXANDROID_ANDROID_EINVAL);

  authority.schema_version = 1u;
  profile = make_profile(&log);
  CHECK(nxandroid_android_context_init(&context, &authority, &profile, error,
                                       sizeof(error)) ==
        NXANDROID_ANDROID_EINVAL);

  fill_authority(&authority);
  authority.decision[NXANDROID_ANDROID_MENU][NXANDROID_ANDROID_A] =
      NXANDROID_ANDROID_DECIDE_NONE;
  CHECK(nxandroid_android_context_init(&context, &authority, &profile, error,
                                       sizeof(error)) ==
        NXANDROID_ANDROID_EINVAL);
  return 0;
}

static int test_dispatch_and_lifecycle(void) {
  nxandroid_android_context context;
  nxandroid_android_authority authority;
  nxandroid_android_profile profile;
  nxandroid_android_pull_state pull;
  nxandroid_android_event event;
  event_log log;
  char error[256];
  char receipt[512];
  size_t before;
  uint32_t first_generation;
  uint32_t second_generation;
  memset(&log, 0, sizeof(log));
  fill_authority(&authority);
  profile = make_profile(&log);
  CHECK(nxandroid_android_context_init(&context, &authority, &profile, error,
                                       sizeof(error)) ==
        NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_control_read_policy(
            &context, NXANDROID_ANDROID_MENU, NXANDROID_ANDROID_A) ==
        NXANDROID_ANDROID_READ_SUPPRESSED);
  CHECK(nxandroid_android_control_read_policy(
            &context, NXANDROID_ANDROID_MENU, NXANDROID_ANDROID_L3) ==
        NXANDROID_ANDROID_READ_NATIVE);

  CHECK(nxandroid_android_pad_connect(
            &context, 10, 110, "same-guid",
            NXANDROID_ANDROID_SOURCE_PORT_BUNDLE, 100u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.count == 1u);
  CHECK(log.events[0].kind == NXANDROID_ANDROID_EVENT_PAD_ADDED);
  CHECK(log.events[0].source == NXANDROID_ANDROID_SOURCE_PORT_BUNDLE);
  CHECK(log.events[0].timestamp_ns == 100u);
  first_generation = log.events[0].generation;
  CHECK(first_generation != 0u);

  before = log.count;
  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_A, 1, 110u) ==
        NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_A, 0, 111u) ==
        NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_B, 1, 112u) ==
        NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_B, 0, 113u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.count == before);
  CHECK(nxandroid_android_pull(&context, 10, NXANDROID_ANDROID_A, 114u,
                               &pull) == NXANDROID_ANDROID_ENOTFOUND);

  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_X, 1, 120u) ==
        NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_X, 1, 121u) ==
        NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_X, 0, 122u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.count == before + 2u);
  event = log.events[log.count - 2u];
  CHECK(event.kind == NXANDROID_ANDROID_EVENT_BUTTON);
  CHECK(event.sink == NXANDROID_ANDROID_SINK_KEY_EVENT);
  CHECK(event.code_x == 99 && event.pressed == 1);
  CHECK(event.instance_id == 10 && event.device_id == 110);
  CHECK(event.generation == first_generation);
  CHECK(event.context == NXANDROID_ANDROID_MENU);
  CHECK(event.sequence != 0u && event.timestamp_ns == 120u);
  CHECK(log.events[log.count - 1u].pressed == 0);

  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_Y, 1, 123u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.events[log.count - 1u].sink == NXANDROID_ANDROID_SINK_JNI_PUSH);
  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_Y, 0, 124u) ==
        NXANDROID_ANDROID_OK);

  before = log.count;
  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_L1, 1, 125u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.count == before + 1u);
  CHECK(log.events[before].kind == NXANDROID_ANDROID_EVENT_TOUCH_DOWN);
  CHECK(log.events[before].sink == NXANDROID_ANDROID_SINK_TOUCH);
  CHECK(log.events[before].pixel_x >= 80 && log.events[before].pixel_x < 1200);
  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_R1, 1, 126u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.count == before + 1u);
  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_L1, 0, 127u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.count == before + 1u);
  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_R1, 0, 128u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.count == before + 2u);
  CHECK(log.events[before + 1u].kind == NXANDROID_ANDROID_EVENT_TOUCH_UP);

  before = log.count;
  CHECK(nxandroid_android_axis(&context, 10, NXANDROID_ANDROID_L2, 0.2f,
                               130u) == NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_axis(&context, 10, NXANDROID_ANDROID_L2, 0.7f,
                               131u) == NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_axis(&context, 10, NXANDROID_ANDROID_L2, 0.8f,
                               132u) == NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_axis(&context, 10, NXANDROID_ANDROID_L2, 0.3f,
                               133u) == NXANDROID_ANDROID_OK);
  CHECK(log.count == before + 4u);
  CHECK(log.events[before].kind == NXANDROID_ANDROID_EVENT_AXIS);
  CHECK(log.events[before].pressed == 0);
  CHECK(log.events[before + 1u].pressed == 1);
  CHECK(log.events[before + 2u].pressed == 1);
  CHECK(log.events[before + 3u].pressed == 0);
  CHECK(fabsf(log.events[before + 3u].value - 0.3f) < 0.0001f);

  before = log.count;
  CHECK(nxandroid_android_axis(&context, 10, NXANDROID_ANDROID_R2, 0.8f,
                               140u) == NXANDROID_ANDROID_OK);
  CHECK(log.count == before);
  CHECK(nxandroid_android_pull(&context, 10, NXANDROID_ANDROID_R2, 141u,
                               &pull) == NXANDROID_ANDROID_OK);
  CHECK(pull.available && pull.pressed);
  CHECK(fabsf(pull.value - 0.8f) < 0.0001f);
  CHECK(pull.generation == first_generation);
  CHECK(pull.source == NXANDROID_ANDROID_SOURCE_PORT_BUNDLE);
  CHECK(pull.context == NXANDROID_ANDROID_MENU);
  CHECK(pull.instance_id == 10 && pull.device_id == 110);
  CHECK(pull.update_timestamp_ns == 140u);
  CHECK(pull.read_timestamp_ns == 141u);
  CHECK(pull.read_sequence > pull.update_sequence);
  CHECK(nxandroid_android_axis(&context, 10, NXANDROID_ANDROID_R2, 0.0f,
                               142u) == NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_pull(&context, 10, NXANDROID_ANDROID_R2, 143u,
                               &pull) == NXANDROID_ANDROID_OK);
  CHECK(!pull.pressed && pull.value == 0.0f);

  before = log.count;
  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_L3, 1, 150u) ==
        NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_L3, 0, 151u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.count == before + 2u);
  CHECK(log.events[before].sink == NXANDROID_ANDROID_SINK_NATIVE_GAMEPAD);
  CHECK(log.events[before].code_x == 106);
  CHECK(context.native_inputs == 2u);

  before = log.count;
  CHECK(nxandroid_android_vector(&context, 10, NXANDROID_ANDROID_LEFT_STICK,
                                 -1.0f, 0.5f, 0.016f, 160u) ==
        NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_vector(&context, 10, NXANDROID_ANDROID_LEFT_STICK,
                                 0.0f, 0.0f, 0.016f, 161u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.count == before + 2u);
  CHECK(log.events[before].kind == NXANDROID_ANDROID_EVENT_VECTOR);
  CHECK(log.events[before].sink == NXANDROID_ANDROID_SINK_NATIVE_GAMEPAD);

  CHECK(nxandroid_android_set_context(&context, NXANDROID_ANDROID_CURSOR,
                                      170u) == NXANDROID_ANDROID_OK);
  before = log.count;
  CHECK(nxandroid_android_vector(&context, 10, NXANDROID_ANDROID_RIGHT_STICK,
                                 1.0f, 0.0f, 0.016f, 171u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.count == before + 1u);
  CHECK(log.events[before].kind == NXANDROID_ANDROID_EVENT_CURSOR_MOVE);
  CHECK(log.events[before].pixel_x >= 80 && log.events[before].pixel_x < 1200);
  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_R3, 1, 172u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.events[log.count - 1u].kind == NXANDROID_ANDROID_EVENT_TOUCH_DOWN);
  before = log.count;
  CHECK(nxandroid_android_vector(&context, 10, NXANDROID_ANDROID_RIGHT_STICK,
                                 0.8f, 0.2f, 0.016f, 173u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.count == before + 2u);
  CHECK(log.events[log.count - 1u].kind == NXANDROID_ANDROID_EVENT_TOUCH_MOVE);
  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_R3, 0, 174u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.events[log.count - 1u].kind == NXANDROID_ANDROID_EVENT_TOUCH_UP);

  CHECK(nxandroid_android_pad_connect(
            &context, 20, 220, "same-guid",
            NXANDROID_ANDROID_SOURCE_CFW_GUID_DB, 180u) ==
        NXANDROID_ANDROID_OK);
  second_generation = log.events[log.count - 1u].generation;
  CHECK(second_generation != first_generation);
  CHECK(nxandroid_android_button(&context, 20, NXANDROID_ANDROID_X, 1, 181u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.events[log.count - 1u].instance_id == 20);
  CHECK(log.events[log.count - 1u].generation == second_generation);
  before = log.count;
  CHECK(nxandroid_android_pad_disconnect(&context, 10, 182u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.events[log.count - 1u].kind == NXANDROID_ANDROID_EVENT_PAD_REMOVED);
  CHECK(log.events[log.count - 1u].instance_id == 10);
  CHECK(log.count >= before + 1u);
  CHECK(nxandroid_android_button(&context, 20, NXANDROID_ANDROID_X, 0, 183u) ==
        NXANDROID_ANDROID_OK);

  CHECK(nxandroid_android_pad_connect(
            &context, 10, 111, "same-guid",
            NXANDROID_ANDROID_SOURCE_RUNTIME_BUILTIN, 184u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.events[log.count - 1u].generation > second_generation);
  CHECK(nxandroid_android_axis(&context, 10, NXANDROID_ANDROID_L2, 1.0f,
                               185u) == NXANDROID_ANDROID_OK);
  before = log.count;
  CHECK(nxandroid_android_pad_cancel(&context, 10, 186u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.count == before + 2u);
  CHECK(log.events[before].kind == NXANDROID_ANDROID_EVENT_AXIS);
  CHECK(log.events[before].value == 0.0f);
  CHECK(log.events[before + 1u].kind == NXANDROID_ANDROID_EVENT_CANCELLED);

  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_X, 1, 190u) ==
        NXANDROID_ANDROID_OK);
  before = log.count;
  CHECK(nxandroid_android_set_focus(&context, 0, 191u) == NXANDROID_ANDROID_OK);
  CHECK(log.count == before + 2u);
  CHECK(log.events[before].pressed == 0);
  CHECK(log.events[before + 1u].kind == NXANDROID_ANDROID_EVENT_FOCUS_LOST);
  before = log.count;
  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_X, 1, 192u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.count == before);
  CHECK(nxandroid_android_set_focus(&context, 1, 193u) == NXANDROID_ANDROID_OK);
  CHECK(log.count == before + 1u);
  CHECK(log.events[log.count - 1u].kind == NXANDROID_ANDROID_EVENT_FOCUS_GAINED);
  CHECK(nxandroid_android_set_resumed(&context, 0, 194u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.events[log.count - 1u].kind == NXANDROID_ANDROID_EVENT_PAUSED);
  CHECK(nxandroid_android_set_resumed(&context, 1, 195u) ==
        NXANDROID_ANDROID_OK);
  CHECK(log.events[log.count - 1u].kind == NXANDROID_ANDROID_EVENT_RESUMED);

  CHECK(nxandroid_android_button(&context, 10, NXANDROID_ANDROID_X, 1, 194u) ==
        NXANDROID_ANDROID_EINVAL);
  CHECK(nxandroid_android_receipt(&context, receipt, sizeof(receipt)) ==
        NXANDROID_ANDROID_OK);
  CHECK(strstr(receipt, "consumer=c7-realistic-harness") != NULL);
  CHECK(strstr(receipt, "pull_reads=2") != NULL);
  CHECK(strstr(receipt, "identifiers=redacted") != NULL);
  CHECK(strstr(receipt, "same-guid") == NULL);
  CHECK(strstr(receipt, "device_id") == NULL);
  return 0;
}

static int test_fail_stop(void) {
  nxandroid_android_context context;
  nxandroid_android_authority authority;
  nxandroid_android_profile profile;
  event_log log;
  char error[128];
  memset(&log, 0, sizeof(log));
  fill_authority(&authority);
  profile = make_profile(&log);
  log.malformed_ack = 1;
  CHECK(nxandroid_android_context_init(&context, &authority, &profile, error,
                                       sizeof(error)) ==
        NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_pad_connect(
            &context, 1, 1, "guid",
            NXANDROID_ANDROID_SOURCE_GET_CONTROLS, 1u) ==
        NXANDROID_ANDROID_ECALLBACK);
  CHECK(context.failed == 1);
  CHECK(context.delivered_events == 0u);
  CHECK(nxandroid_android_button(&context, 1, NXANDROID_ANDROID_X, 1, 2u) ==
        NXANDROID_ANDROID_ESTATE);
  return 0;
}

int main(void) {
  if (test_touch_geometry() != 0 || test_validation() != 0 ||
      test_dispatch_and_lifecycle() != 0 || test_fail_stop() != 0)
    return 1;
  printf("nxandroid_android_c7_tests=PASS checks=%u\n", checks);
  printf("consumer_callbacks_acknowledged=1 pull_reads_measured=1\n");
  printf("android_guest_code_executed=0 android_device_access=0 ");
  printf("android_network_access=0\n");
  return 0;
}
