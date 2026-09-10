/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxandroid_unity_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks;

#define CHECK(condition)                                                       \
  do {                                                                         \
    checks++;                                                                  \
    if (!(condition)) {                                                        \
      fprintf(stderr, "unity-c8: CHECK failed at %s:%d: %s\n", __FILE__,     \
              __LINE__, #condition);                                           \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

static const char *const control_names[NXANDROID_ANDROID_CONTROL_COUNT] = {
    "A",          "B",          "X",          "Y",
    "L1",         "R1",         "L2",         "R2",
    "L3",         "R3",         "START",      "SELECT",
    "UP",         "DOWN",       "LEFT",       "RIGHT",
    "LEFT_STICK", "RIGHT_STICK"};

typedef struct fixture_ops {
  uint64_t thread_token;
  uint64_t device_calls;
  uint64_t producer_calls;
  uint64_t last_sequence;
  nxandroid_unity_device_operation last_operation;
  nxandroid_android_control last_control;
  const char *last_api;
  int bad_device_ack;
} fixture_ops;

static uint64_t current_thread_token(void *userdata) {
  fixture_ops *fixture = (fixture_ops *)userdata;
  return fixture->thread_token;
}

static int device_callback(void *userdata,
                           const nxandroid_unity_device_request *request,
                           nxandroid_unity_api_ack *ack) {
  fixture_ops *fixture = (fixture_ops *)userdata;
  fixture->device_calls++;
  fixture->last_sequence = request->sequence;
  fixture->last_operation = request->operation;
  fixture->last_api = request->api_signature;
  memset(ack, 0, sizeof(*ack));
  ack->api_version = NXANDROID_UNITY_INPUT_API_VERSION;
  ack->sequence = request->sequence + (fixture->bad_device_ack ? 1u : 0u);
  ack->returned = 1;
  ack->handled = 1;
  ack->return_value = request->operation == NXANDROID_UNITY_DEVICE_ADD ? 1 : 0;
  return 0;
}

static int producer_callback(void *userdata,
                             const nxandroid_unity_producer_request *request,
                             nxandroid_unity_api_ack *ack) {
  fixture_ops *fixture = (fixture_ops *)userdata;
  fixture->producer_calls++;
  fixture->last_sequence = request->sequence;
  fixture->last_control = request->control;
  fixture->last_api = request->api_signature;
  memset(ack, 0, sizeof(*ack));
  ack->api_version = NXANDROID_UNITY_INPUT_API_VERSION;
  ack->sequence = request->sequence;
  ack->returned = 1;
  ack->handled = 1;
  ack->return_value = 1;
  return 0;
}

static void set_family(nxandroid_unity_profile *profile,
                       nxandroid_unity_profile_kind kind) {
  profile->kind = kind;
  switch (kind) {
  case NXANDROID_UNITY_LEGACY_INPUT:
    profile->profile_id = "fixture-unity-legacy";
    profile->plugin_name = "UnityEngine.Input";
    profile->plugin_version = "2019.4.40f1";
    profile->assembly_name = "UnityEngine.InputLegacyModule.dll";
    profile->register_api =
        "android.view.InputDevice::getDevice(int):InputDevice";
    profile->unregister_api =
        "android.view.InputDevice::onRemoved(int):void";
    profile->enumerate_api =
        "UnityEngine.Input::GetJoystickNames():String[]";
    profile->producer_button_api =
        "UnityEngine.UnityPlayer::nativeInjectEvent(KeyEvent):boolean";
    profile->producer_axis_api =
        "UnityEngine.UnityPlayer::nativeInjectEvent(MotionEvent):boolean";
    profile->consumer_button_api =
        "UnityEngine.Input::GetButton(string):boolean";
    profile->consumer_axis_api =
        "UnityEngine.Input::GetAxisRaw(string):float";
    profile->consumer_action_api =
        "Fixture.LegacyActions::Read(string):boolean";
    break;
  case NXANDROID_UNITY_NEW_INPUT_SYSTEM:
    profile->profile_id = "fixture-unity-new-input";
    profile->plugin_name = "com.unity.inputsystem";
    profile->plugin_version = "1.14.2";
    profile->assembly_name = "Unity.InputSystem.dll";
    profile->register_api =
        "UnityEngine.InputSystem.InputSystem::AddDevice(string):InputDevice";
    profile->unregister_api =
        "UnityEngine.InputSystem.InputSystem::RemoveDevice(InputDevice):void";
    profile->enumerate_api =
        "UnityEngine.InputSystem.Gamepad::get_all():ReadOnlyArray";
    profile->producer_button_api =
        "UnityEngine.InputSystem.InputSystem::QueueEvent(ButtonEvent):void";
    profile->producer_axis_api =
        "UnityEngine.InputSystem.InputSystem::QueueEvent(StateEvent):void";
    profile->consumer_button_api =
        "UnityEngine.InputSystem.Controls.ButtonControl::get_isPressed():boolean";
    profile->consumer_axis_api =
        "UnityEngine.InputSystem.InputControl::ReadValue():float";
    profile->consumer_action_api =
        "UnityEngine.InputSystem.InputAction::ReadValue():float";
    profile->producer_button_sink = NXANDROID_ANDROID_SINK_JNI_PUSH;
    profile->producer_axis_sink = NXANDROID_ANDROID_SINK_JNI_PUSH;
    break;
  case NXANDROID_UNITY_REWIRED:
    profile->profile_id = "fixture-rewired";
    profile->plugin_name = "Rewired";
    profile->plugin_version = "1.1.9";
    profile->assembly_name = "Rewired_Core.dll";
    profile->register_api =
        "Rewired.ControllerHelper::GetJoysticks():IList";
    profile->unregister_api =
        "Rewired.ControllerHelper::RemoveController(int):boolean";
    profile->enumerate_api =
        "Rewired.ControllerHelper::GetJoysticks():IList";
    profile->producer_button_api =
        "UnityEngine.UnityPlayer::nativeInjectEvent(KeyEvent):boolean";
    profile->producer_axis_api =
        "UnityEngine.UnityPlayer::nativeInjectEvent(MotionEvent):boolean";
    profile->consumer_button_api =
        "Rewired.Player::GetButton(string):boolean";
    profile->consumer_axis_api =
        "Rewired.Player::GetAxis(string):float";
    profile->consumer_action_api =
        "Fixture.RewiredActions::Consume(string):boolean";
    break;
  case NXANDROID_UNITY_INCONTROL:
    profile->profile_id = "fixture-incontrol";
    profile->plugin_name = "InControl";
    profile->plugin_version = "1.7.4";
    profile->assembly_name = "Assembly-CSharp.dll";
    profile->register_api =
        "InControl.InputManager::AttachDevice(InputDevice):void";
    profile->unregister_api =
        "InControl.InputManager::DetachDevice(InputDevice):void";
    profile->enumerate_api =
        "InControl.InputManager::get_Devices():ReadOnlyCollection";
    profile->producer_button_api =
        "UnityEngine.UnityPlayer::nativeInjectEvent(KeyEvent):boolean";
    profile->producer_axis_api =
        "UnityEngine.UnityPlayer::nativeInjectEvent(MotionEvent):boolean";
    profile->consumer_button_api =
        "InControl.UnityInputDevice::ReadRawButtonState(int):boolean";
    profile->consumer_axis_api =
        "InControl.UnityInputDevice::ReadRawAnalogValue(int):float";
    profile->consumer_action_api =
        "InControl.PlayerAction::get_WasPressed():boolean";
    break;
  case NXANDROID_UNITY_RAW_ANDROID:
    profile->profile_id = "fixture-raw-android";
    profile->plugin_name = "android.view.InputDevice";
    profile->plugin_version = "34.0.0";
    profile->assembly_name = "UnityPlayerActivity.java";
    profile->register_api =
        "android.view.InputDevice::getDevice(int):InputDevice";
    profile->unregister_api =
        "android.hardware.input.InputManager::unregisterInputDeviceListener(Listener):void";
    profile->enumerate_api =
        "android.view.InputDevice::getDeviceIds():int[]";
    profile->producer_button_api =
        "android.app.Activity::dispatchKeyEvent(KeyEvent):boolean";
    profile->producer_axis_api =
        "android.app.Activity::dispatchGenericMotionEvent(MotionEvent):boolean";
    profile->consumer_button_api =
        "android.view.KeyEvent::getKeyCode():int";
    profile->consumer_axis_api =
        "android.view.MotionEvent::getAxisValue(int):float";
    profile->consumer_action_api =
        "Fixture.RawPlayer::consume(InputEvent):boolean";
    profile->registration_thread = NXANDROID_UNITY_THREAD_ANDROID_INPUT;
    profile->producer_thread = NXANDROID_UNITY_THREAD_ANDROID_INPUT;
    profile->consumer_thread = NXANDROID_UNITY_THREAD_ANDROID_INPUT;
    break;
  default:
    exit(2);
  }
}

static void build_profile(nxandroid_unity_profile *profile,
                          nxandroid_unity_profile_kind kind) {
  int control;
  memset(profile, 0, sizeof(*profile));
  profile->api_version = NXANDROID_UNITY_INPUT_API_VERSION;
  profile->runtime = NXANDROID_UNITY_RUNTIME_IL2CPP;
  profile->evidence = NXANDROID_UNITY_EVIDENCE_FIXTURE;
  profile->identity_policy = NXANDROID_UNITY_IDENTITY_PHYSICAL;
  profile->registration_thread = NXANDROID_UNITY_THREAD_PLAYER;
  profile->producer_thread = NXANDROID_UNITY_THREAD_PLAYER;
  profile->consumer_thread = NXANDROID_UNITY_THREAD_PLAYER;
  profile->producer_button_sink = NXANDROID_ANDROID_SINK_KEY_EVENT;
  profile->producer_axis_sink = NXANDROID_ANDROID_SINK_MOTION_EVENT;
  profile->unity_version = "2019.4.40f1";
  profile->consumer_name = "c8-hermetic-consumer";
  profile->consumer_version = "1.0.0";
  profile->abi = "arm64-v8a";
  profile->metadata_sha256 =
      "1111111111111111111111111111111111111111111111111111111111111111";
  profile->runtime_sha256 =
      "2222222222222222222222222222222222222222222222222222222222222222";
  profile->source_commit = "3333333333333333333333333333333333333333";
  profile->source_tree = "4444444444444444444444444444444444444444";
  profile->artifact_sha256 =
      "5555555555555555555555555555555555555555555555555555555555555555";
  profile->license_spdx = "GPL-3.0-only";
  set_family(profile, kind);
  for (control = 0; control < (int)NXANDROID_ANDROID_CONTROL_COUNT;
       ++control) {
    profile->controls[control].reachable = 1;
    profile->controls[control].signal =
        control == NXANDROID_ANDROID_LEFT_STICK ||
                control == NXANDROID_ANDROID_RIGHT_STICK
            ? NXANDROID_ANDROID_SIGNAL_VECTOR
            : control == NXANDROID_ANDROID_L2 ||
                      control == NXANDROID_ANDROID_R2
                  ? NXANDROID_ANDROID_SIGNAL_AXIS
                  : NXANDROID_ANDROID_SIGNAL_BUTTON;
    profile->controls[control].consumer_control = control_names[control];
  }
}

static void run_initial_lifecycle(nxandroid_unity_context *unity,
                                  fixture_ops *fixture) {
  static const nxandroid_unity_lifecycle_phase phases[] = {
      NXANDROID_UNITY_PHASE_INIT_ARRAY,
      NXANDROID_UNITY_PHASE_JNI_ONLOAD_MAIN,
      NXANDROID_UNITY_PHASE_JNI_ONLOAD_RUNTIME,
      NXANDROID_UNITY_PHASE_JNI_ONLOAD_UNITY,
      NXANDROID_UNITY_PHASE_PLAYER_INIT,
      NXANDROID_UNITY_PHASE_SURFACE_CREATE,
      NXANDROID_UNITY_PHASE_SURFACE_CHANGE,
      NXANDROID_UNITY_PHASE_RESUME,
      NXANDROID_UNITY_PHASE_FOCUS_GAIN,
      NXANDROID_UNITY_PHASE_FRAME_LOOP};
  size_t index;
  fixture->thread_token = 100u;
  for (index = 0u; index < sizeof(phases) / sizeof(phases[0]); ++index)
    CHECK(nxandroid_unity_lifecycle(unity, phases[index], 1u) ==
          NXANDROID_UNITY_OK);
  CHECK(unity->live == 1);
}

static void build_android_contract(
    nxandroid_android_authority *authority, nxandroid_android_route *routes,
    size_t *route_count, nxandroid_android_native_route *native_routes,
    size_t *native_count, const nxandroid_unity_profile *unity_profile,
    int null_native_mode) {
  int context;
  int control;
  size_t route = 0u;
  memset(authority, 0, sizeof(*authority));
  memset(routes, 0, NXANDROID_ANDROID_CONTROL_COUNT * sizeof(*routes));
  memset(native_routes, 0,
         NXANDROID_ANDROID_CONTROL_COUNT * sizeof(*native_routes));
  authority->api_version = NXANDROID_ANDROID_INPUT_API_VERSION;
  authority->schema_version = 2u;
  authority->context_present[NXANDROID_ANDROID_MENU] = 1u;
  authority->context_present[NXANDROID_ANDROID_GAMEPLAY] = 1u;
  for (context = NXANDROID_ANDROID_MENU;
       context <= NXANDROID_ANDROID_GAMEPLAY; ++context) {
    for (control = 0; control < (int)NXANDROID_ANDROID_CONTROL_COUNT;
         ++control) {
      int suppress = null_native_mode &&
                     (control == NXANDROID_ANDROID_A ||
                      control == NXANDROID_ANDROID_B);
      int native = null_native_mode && control == NXANDROID_ANDROID_L3;
      authority->decision[context][control] =
          suppress ? NXANDROID_ANDROID_DECIDE_SUPPRESS
                   : native ? NXANDROID_ANDROID_DECIDE_NATIVE
                            : NXANDROID_ANDROID_DECIDE_ACTION;
      if (!suppress && !native)
        snprintf(authority->action[context][control],
                 NXANDROID_ANDROID_ACTION_MAX, "unity.%s",
                 control_names[control]);
    }
  }
  for (control = 0; control < (int)NXANDROID_ANDROID_CONTROL_COUNT;
       ++control) {
    if (null_native_mode &&
        (control == NXANDROID_ANDROID_A ||
         control == NXANDROID_ANDROID_B ||
         control == NXANDROID_ANDROID_L3))
      continue;
    routes[route].action = authority->action[NXANDROID_ANDROID_MENU][control];
    routes[route].signal =
        control == NXANDROID_ANDROID_LEFT_STICK ||
                control == NXANDROID_ANDROID_RIGHT_STICK
            ? NXANDROID_ANDROID_SIGNAL_VECTOR
            : control == NXANDROID_ANDROID_L2 ||
                      control == NXANDROID_ANDROID_R2
                  ? NXANDROID_ANDROID_SIGNAL_AXIS
                  : NXANDROID_ANDROID_SIGNAL_BUTTON;
    routes[route].sink =
        routes[route].signal == NXANDROID_ANDROID_SIGNAL_BUTTON
            ? unity_profile->producer_button_sink
            : unity_profile->producer_axis_sink;
    route++;
  }
  if (null_native_mode) {
    native_routes[0].control = NXANDROID_ANDROID_L3;
    native_routes[0].sink = unity_profile->producer_button_sink;
    native_routes[0].signal = NXANDROID_ANDROID_SIGNAL_BUTTON;
    *native_count = 1u;
  } else {
    *native_count = 0u;
  }
  *route_count = route;
}

static void init_android(nxandroid_android_context *android,
                         nxandroid_unity_context *unity,
                         int null_native_mode) {
  nxandroid_android_authority authority;
  nxandroid_android_route routes[NXANDROID_ANDROID_CONTROL_COUNT];
  nxandroid_android_native_route
      native_routes[NXANDROID_ANDROID_CONTROL_COUNT];
  nxandroid_android_profile profile;
  size_t route_count;
  size_t native_count;
  char error[256];
  build_android_contract(&authority, routes, &route_count, native_routes,
                         &native_count, unity->profile, null_native_mode);
  memset(&profile, 0, sizeof(profile));
  profile.consumer_id = "unity-c8-fixture";
  profile.consumer_version = "1.0.0";
  profile.routes = routes;
  profile.route_count = route_count;
  profile.native_routes = native_routes;
  profile.native_route_count = native_count;
  profile.event = nxandroid_unity_android_event;
  profile.userdata = unity;
  memset(error, 0, sizeof(error));
  CHECK(nxandroid_android_context_init(android, &authority, &profile, error,
                                       sizeof(error)) == NXANDROID_ANDROID_OK);
}

static nxandroid_unity_pad_state *find_unity_pad(
    nxandroid_unity_context *unity, int32_t instance_id) {
  size_t index;
  for (index = 0u; index < NXANDROID_UNITY_MAX_PADS; ++index) {
    if (unity->pads[index].connected &&
        unity->pads[index].instance_id == instance_id)
      return &unity->pads[index];
  }
  return NULL;
}

static void complete_return(nxandroid_unity_context *unity,
                            fixture_ops *fixture, int32_t instance_id,
                            nxandroid_android_control control, int pressed,
                            float value, float x, float y) {
  nxandroid_unity_consumer_return returned;
  nxandroid_unity_pad_state *pad = find_unity_pad(unity, instance_id);
  const char *low_api;
  CHECK(pad != NULL);
  low_api = unity->profile->controls[control].signal ==
                    NXANDROID_ANDROID_SIGNAL_BUTTON
                ? unity->profile->consumer_button_api
                : unity->profile->consumer_axis_api;
  memset(&returned, 0, sizeof(returned));
  returned.api_version = NXANDROID_UNITY_INPUT_API_VERSION;
  returned.stage = NXANDROID_UNITY_API_LOW_LEVEL_RETURN;
  returned.event_sequence = fixture->last_sequence;
  returned.instance_id = instance_id;
  returned.generation = pad->generation;
  returned.control = control;
  returned.signal = unity->profile->controls[control].signal;
  returned.api_returned = 1;
  returned.handled = 1;
  returned.pressed = pressed;
  returned.value = value;
  returned.x = x;
  returned.y = y;
  returned.api_signature = low_api;
  CHECK(nxandroid_unity_note_consumer_return(unity, &returned) ==
        NXANDROID_UNITY_OK);
  returned.stage = NXANDROID_UNITY_API_ACTION_RETURN;
  returned.api_signature = unity->profile->consumer_action_api;
  CHECK(nxandroid_unity_note_consumer_return(unity, &returned) ==
        NXANDROID_UNITY_OK);
}

static void exercise_control(nxandroid_android_context *android,
                             nxandroid_unity_context *unity,
                             fixture_ops *fixture, int32_t instance_id,
                             nxandroid_android_control control,
                             uint64_t *timestamp) {
  uint64_t before = fixture->producer_calls;
  if (control == NXANDROID_ANDROID_L2 || control == NXANDROID_ANDROID_R2) {
    CHECK(nxandroid_android_axis(android, instance_id, control, 1.0f,
                                 ++*timestamp) == NXANDROID_ANDROID_OK);
    CHECK(fixture->producer_calls == before + 1u);
    complete_return(unity, fixture, instance_id, control, 1, 1.0f, 0.0f,
                    0.0f);
    CHECK(nxandroid_android_axis(android, instance_id, control, 0.0f,
                                 ++*timestamp) == NXANDROID_ANDROID_OK);
    CHECK(fixture->producer_calls == before + 2u);
    complete_return(unity, fixture, instance_id, control, 0, 0.0f, 0.0f,
                    0.0f);
  } else if (control == NXANDROID_ANDROID_LEFT_STICK ||
             control == NXANDROID_ANDROID_RIGHT_STICK) {
    CHECK(nxandroid_android_vector(android, instance_id, control, 1.0f, -1.0f,
                                   0.0f, ++*timestamp) ==
          NXANDROID_ANDROID_OK);
    CHECK(fixture->producer_calls == before + 1u);
    complete_return(unity, fixture, instance_id, control, 1, 0.0f, 1.0f,
                    -1.0f);
    CHECK(nxandroid_android_vector(android, instance_id, control, 0.0f, 0.0f,
                                   0.0f, ++*timestamp) ==
          NXANDROID_ANDROID_OK);
    CHECK(fixture->producer_calls == before + 2u);
    complete_return(unity, fixture, instance_id, control, 0, 0.0f, 0.0f,
                    0.0f);
  } else {
    CHECK(nxandroid_android_button(android, instance_id, control, 1,
                                   ++*timestamp) == NXANDROID_ANDROID_OK);
    CHECK(fixture->producer_calls == before + 1u);
    complete_return(unity, fixture, instance_id, control, 1, 1.0f, 0.0f,
                    0.0f);
    CHECK(nxandroid_android_button(android, instance_id, control, 0,
                                   ++*timestamp) == NXANDROID_ANDROID_OK);
    CHECK(fixture->producer_calls == before + 2u);
    complete_return(unity, fixture, instance_id, control, 0, 0.0f, 0.0f,
                    0.0f);
  }
}

static void exercise_null_native(nxandroid_unity_context *unity,
                                 fixture_ops *fixture, uint64_t *timestamp) {
  nxandroid_android_context android;
  uint64_t before;
  init_android(&android, unity, 1);
  fixture->thread_token = unity->profile->registration_thread ==
                                  NXANDROID_UNITY_THREAD_ANDROID_INPUT
                              ? 200u
                              : 100u;
  CHECK(nxandroid_android_pad_connect(
            &android, 30, 300, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            NXANDROID_ANDROID_SOURCE_CFW_GUID_DB, ++*timestamp) ==
        NXANDROID_ANDROID_OK);
  before = fixture->producer_calls;
  CHECK(nxandroid_android_button(&android, 30, NXANDROID_ANDROID_A, 1,
                                 ++*timestamp) == NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_button(&android, 30, NXANDROID_ANDROID_A, 0,
                                 ++*timestamp) == NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_button(&android, 30, NXANDROID_ANDROID_B, 1,
                                 ++*timestamp) == NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_button(&android, 30, NXANDROID_ANDROID_B, 0,
                                 ++*timestamp) == NXANDROID_ANDROID_OK);
  CHECK(fixture->producer_calls == before);
  exercise_control(&android, unity, fixture, 30, NXANDROID_ANDROID_L2,
                   timestamp);
  exercise_control(&android, unity, fixture, 30, NXANDROID_ANDROID_R2,
                   timestamp);
  exercise_control(&android, unity, fixture, 30, NXANDROID_ANDROID_L3,
                   timestamp);
  CHECK(android.suppressed_inputs == 4u);
  CHECK(android.native_inputs == 2u);
  CHECK(nxandroid_android_pad_disconnect(&android, 30, ++*timestamp) ==
        NXANDROID_ANDROID_OK);
  nxandroid_android_context_reset(&android);
}

static void exercise_profile(nxandroid_unity_profile_kind kind) {
  nxandroid_unity_profile profile;
  nxandroid_unity_context unity;
  nxandroid_unity_ops ops;
  nxandroid_android_context android;
  fixture_ops fixture;
  uint64_t timestamp = 1000u + (uint64_t)kind * 10000u;
  uint32_t old_generation;
  int control;
  char error[256];
  char receipt[512];

  memset(&fixture, 0, sizeof(fixture));
  memset(&ops, 0, sizeof(ops));
  build_profile(&profile, kind);
  CHECK(nxandroid_unity_profile_validate(&profile, error, sizeof(error)) ==
        NXANDROID_UNITY_OK);
  ops.current_thread_token = current_thread_token;
  ops.device = device_callback;
  ops.producer = producer_callback;
  ops.userdata = &fixture;
  CHECK(nxandroid_unity_context_init(&unity, &profile, &ops, error,
                                     sizeof(error)) == NXANDROID_UNITY_OK);
  run_initial_lifecycle(&unity, &fixture);
  init_android(&android, &unity, 0);

  fixture.thread_token = profile.registration_thread ==
                                 NXANDROID_UNITY_THREAD_ANDROID_INPUT
                             ? 200u
                             : 100u;
  CHECK(nxandroid_android_pad_connect(
            &android, 10, 100, "11111111111111111111111111111111",
            NXANDROID_ANDROID_SOURCE_GET_CONTROLS, ++timestamp) ==
        NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_pad_connect(
            &android, 11, 101, "11111111111111111111111111111111",
            NXANDROID_ANDROID_SOURCE_GET_CONTROLS, ++timestamp) ==
        NXANDROID_ANDROID_OK);
  CHECK(find_unity_pad(&unity, 10) != NULL);
  CHECK(find_unity_pad(&unity, 11) != NULL);
  CHECK(find_unity_pad(&unity, 10)->generation !=
        find_unity_pad(&unity, 11)->generation);

  fixture.thread_token = profile.producer_thread ==
                                 NXANDROID_UNITY_THREAD_ANDROID_INPUT
                             ? 200u
                             : 100u;
  for (control = 0; control < (int)NXANDROID_ANDROID_CONTROL_COUNT;
       ++control)
    exercise_control(&android, &unity, &fixture, 10,
                     (nxandroid_android_control)control, &timestamp);
  exercise_control(&android, &unity, &fixture, 11, NXANDROID_ANDROID_A,
                   &timestamp);
  CHECK(unity.complete_receipts == 38u);

  /* Unplug while held: the release reaches the producer, is never called a
   * consumer receipt, and is invalidated only for this generation. */
  exercise_control(&android, &unity, &fixture, 11, NXANDROID_ANDROID_X,
                   &timestamp);
  CHECK(nxandroid_android_button(&android, 11, NXANDROID_ANDROID_X, 1,
                                 ++timestamp) == NXANDROID_ANDROID_OK);
  complete_return(&unity, &fixture, 11, NXANDROID_ANDROID_X, 1, 1.0f, 0.0f,
                  0.0f);
  CHECK(unity.complete_receipts == 41u);
  fixture.thread_token = profile.registration_thread ==
                                 NXANDROID_UNITY_THREAD_ANDROID_INPUT
                             ? 200u
                             : 100u;
  old_generation = find_unity_pad(&unity, 11)->generation;
  CHECK(nxandroid_android_pad_disconnect(&android, 11, ++timestamp) ==
        NXANDROID_ANDROID_OK);
  CHECK(find_unity_pad(&unity, 11) == NULL);
  CHECK(unity.invalidated_pending == 1u);
  CHECK(nxandroid_android_pad_connect(
            &android, 11, 102, "11111111111111111111111111111111",
            NXANDROID_ANDROID_SOURCE_GET_CONTROLS, ++timestamp) ==
        NXANDROID_ANDROID_OK);
  CHECK(find_unity_pad(&unity, 11)->generation != old_generation);
  fixture.thread_token = profile.producer_thread ==
                                 NXANDROID_UNITY_THREAD_ANDROID_INPUT
                             ? 200u
                             : 100u;
  exercise_control(&android, &unity, &fixture, 11, NXANDROID_ANDROID_B,
                   &timestamp);
  CHECK(unity.complete_receipts == 43u);

  /* Deactivation lets C7 release owned state before the native lifecycle;
   * activation restores the native lifecycle before C7 admits new input. */
  fixture.thread_token = 100u;
  CHECK(nxandroid_android_set_focus(&android, 0, ++timestamp) ==
        NXANDROID_ANDROID_OK);
  CHECK(nxandroid_unity_lifecycle(&unity, NXANDROID_UNITY_PHASE_FOCUS_LOSS,
                                  2u) == NXANDROID_UNITY_OK);
  CHECK(nxandroid_android_set_resumed(&android, 0, ++timestamp) ==
        NXANDROID_ANDROID_OK);
  CHECK(nxandroid_unity_lifecycle(&unity, NXANDROID_UNITY_PHASE_PAUSE, 2u) ==
        NXANDROID_UNITY_OK);
  CHECK(nxandroid_unity_lifecycle(&unity, NXANDROID_UNITY_PHASE_RESUME, 2u) ==
        NXANDROID_UNITY_OK);
  CHECK(nxandroid_unity_lifecycle(&unity, NXANDROID_UNITY_PHASE_FOCUS_GAIN,
                                  2u) == NXANDROID_UNITY_OK);
  CHECK(nxandroid_unity_lifecycle(&unity, NXANDROID_UNITY_PHASE_FRAME_LOOP,
                                  2u) == NXANDROID_UNITY_OK);
  CHECK(nxandroid_android_set_resumed(&android, 1, ++timestamp) ==
        NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_set_focus(&android, 1, ++timestamp) ==
        NXANDROID_ANDROID_OK);

  fixture.thread_token = profile.registration_thread ==
                                 NXANDROID_UNITY_THREAD_ANDROID_INPUT
                             ? 200u
                             : 100u;
  CHECK(nxandroid_android_pad_disconnect(&android, 10, ++timestamp) ==
        NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_pad_disconnect(&android, 11, ++timestamp) ==
        NXANDROID_ANDROID_OK);
  nxandroid_android_context_reset(&android);

  fixture.thread_token = profile.producer_thread ==
                                 NXANDROID_UNITY_THREAD_ANDROID_INPUT
                             ? 200u
                             : 100u;
  exercise_null_native(&unity, &fixture, &timestamp);

  fixture.thread_token = 100u;
  CHECK(nxandroid_unity_receipt(&unity, receipt, sizeof(receipt)) ==
        NXANDROID_UNITY_OK);
  CHECK(strstr(receipt, "evidence=FIXTURE") != NULL);
  CHECK(strstr(receipt, "failed=0") != NULL);
  CHECK(strstr(receipt, "complete_receipts=49") != NULL);
  CHECK(strstr(receipt, "invalidated_pending=1") != NULL);

  CHECK(nxandroid_unity_lifecycle(&unity, NXANDROID_UNITY_PHASE_FOCUS_LOSS,
                                  3u) == NXANDROID_UNITY_OK);
  CHECK(nxandroid_unity_lifecycle(&unity, NXANDROID_UNITY_PHASE_PAUSE, 3u) ==
        NXANDROID_UNITY_OK);
  CHECK(nxandroid_unity_lifecycle(
            &unity, NXANDROID_UNITY_PHASE_SURFACE_DESTROY, 3u) ==
        NXANDROID_UNITY_OK);
  CHECK(nxandroid_unity_lifecycle(&unity, NXANDROID_UNITY_PHASE_SHUTDOWN, 3u) ==
        NXANDROID_UNITY_OK);
  CHECK(unity.shutdown == 1);
  nxandroid_unity_context_reset(&unity);
}

static void negative_contracts(void) {
  nxandroid_unity_profile profile;
  nxandroid_unity_context context;
  nxandroid_unity_ops ops;
  fixture_ops fixture;
  nxandroid_android_context android;
  nxandroid_android_event event;
  nxandroid_android_ack android_ack;
  nxandroid_unity_consumer_return returned;
  nxandroid_unity_pad_state *pad;
  uint64_t timestamp = 90000u;
  char error[256];

  memset(&fixture, 0, sizeof(fixture));
  memset(&ops, 0, sizeof(ops));
  build_profile(&profile, NXANDROID_UNITY_REWIRED);
  profile.consumer_button_api = "Rewired.Player.GetButton";
  CHECK(nxandroid_unity_profile_validate(&profile, error, sizeof(error)) ==
        NXANDROID_UNITY_EINVAL);

  build_profile(&profile, NXANDROID_UNITY_RAW_ANDROID);
  profile.producer_button_sink = NXANDROID_ANDROID_SINK_TOUCH;
  CHECK(nxandroid_unity_profile_validate(&profile, error, sizeof(error)) ==
        NXANDROID_UNITY_EINVAL);

  build_profile(&profile, NXANDROID_UNITY_LEGACY_INPUT);
  profile.identity_name = "invented-pad";
  CHECK(nxandroid_unity_profile_validate(&profile, error, sizeof(error)) ==
        NXANDROID_UNITY_EINVAL);

  build_profile(&profile, NXANDROID_UNITY_INCONTROL);
  profile.identity_policy = NXANDROID_UNITY_IDENTITY_PROVEN_OVERRIDE;
  profile.identity_evidence_sha256 = NULL;
  CHECK(nxandroid_unity_profile_validate(&profile, error, sizeof(error)) ==
        NXANDROID_UNITY_EINVAL);

  build_profile(&profile, NXANDROID_UNITY_NEW_INPUT_SYSTEM);
  profile.plugin_version = "1.14";
  CHECK(nxandroid_unity_profile_validate(&profile, error, sizeof(error)) ==
        NXANDROID_UNITY_EINVAL);

  build_profile(&profile, NXANDROID_UNITY_NEW_INPUT_SYSTEM);
  profile.evidence = NXANDROID_UNITY_EVIDENCE_PENDING;
  ops.current_thread_token = current_thread_token;
  ops.device = device_callback;
  ops.producer = producer_callback;
  ops.userdata = &fixture;
  CHECK(nxandroid_unity_context_init(&context, &profile, &ops, error,
                                     sizeof(error)) ==
        NXANDROID_UNITY_EUNPROVEN);

  build_profile(&profile, NXANDROID_UNITY_LEGACY_INPUT);
  CHECK(nxandroid_unity_context_init(&context, &profile, &ops, error,
                                     sizeof(error)) == NXANDROID_UNITY_OK);
  fixture.thread_token = 100u;
  CHECK(nxandroid_unity_lifecycle(
            &context, NXANDROID_UNITY_PHASE_JNI_ONLOAD_MAIN, 1u) ==
        NXANDROID_UNITY_ESTATE);
  CHECK(context.failed == 1);

  memset(&fixture, 0, sizeof(fixture));
  build_profile(&profile, NXANDROID_UNITY_REWIRED);
  CHECK(nxandroid_unity_context_init(&context, &profile, &ops, error,
                                     sizeof(error)) == NXANDROID_UNITY_OK);
  run_initial_lifecycle(&context, &fixture);
  fixture.bad_device_ack = 1;
  memset(&event, 0, sizeof(event));
  memset(&android_ack, 0, sizeof(android_ack));
  event.api_version = NXANDROID_ANDROID_INPUT_API_VERSION;
  event.kind = NXANDROID_ANDROID_EVENT_PAD_ADDED;
  event.source = NXANDROID_ANDROID_SOURCE_GET_CONTROLS;
  event.sequence = 1u;
  event.instance_id = 1;
  event.device_id = 2;
  event.generation = 1u;
  CHECK(nxandroid_unity_android_event(&context, &event, &android_ack) != 0);
  CHECK(context.failed == 1);
  CHECK(context.device_calls == 0u);

  memset(&fixture, 0, sizeof(fixture));
  build_profile(&profile, NXANDROID_UNITY_LEGACY_INPUT);
  CHECK(nxandroid_unity_context_init(&context, &profile, &ops, error,
                                     sizeof(error)) == NXANDROID_UNITY_OK);
  run_initial_lifecycle(&context, &fixture);
  init_android(&android, &context, 0);
  CHECK(nxandroid_android_pad_connect(
            &android, 5, 50, "55555555555555555555555555555555",
            NXANDROID_ANDROID_SOURCE_GET_CONTROLS, ++timestamp) ==
        NXANDROID_ANDROID_OK);
  CHECK(nxandroid_android_button(&android, 5, NXANDROID_ANDROID_A, 1,
                                 ++timestamp) == NXANDROID_ANDROID_OK);
  pad = find_unity_pad(&context, 5);
  CHECK(pad != NULL);
  memset(&returned, 0, sizeof(returned));
  returned.api_version = NXANDROID_UNITY_INPUT_API_VERSION;
  returned.stage = NXANDROID_UNITY_API_ACTION_RETURN;
  returned.event_sequence = fixture.last_sequence;
  returned.instance_id = 5;
  returned.generation = pad->generation;
  returned.control = NXANDROID_ANDROID_A;
  returned.signal = NXANDROID_ANDROID_SIGNAL_BUTTON;
  returned.api_returned = 1;
  returned.handled = 1;
  returned.pressed = 1;
  returned.value = 1.0f;
  returned.api_signature = profile.consumer_action_api;
  CHECK(nxandroid_unity_note_consumer_return(&context, &returned) ==
        NXANDROID_UNITY_ESTATE);
  CHECK(context.complete_receipts == 0u);
  CHECK(context.failed == 1);
}

int main(void) {
  int kind;
  negative_contracts();
  for (kind = NXANDROID_UNITY_LEGACY_INPUT;
       kind <= NXANDROID_UNITY_RAW_ANDROID; ++kind)
    exercise_profile((nxandroid_unity_profile_kind)kind);
  printf("nxandroid unity C8: PASS checks=%u profiles=5 controls=90 "
         "evidence=FIXTURE physical=PENDING_PHYSICAL\n",
         checks);
  return 0;
}
