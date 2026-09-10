/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxandroid_unity_input.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define UNITY_EPSILON 0.0001f

_Static_assert(NXANDROID_ANDROID_CONTROL_COUNT == 18u,
               "Unity C8 requires the exact GPTK V2 control vocabulary");

static nxandroid_unity_result fail_with(char *error, size_t error_size,
                                        const char *message) {
  if (error != NULL && error_size > 0u)
    snprintf(error, error_size, "%s", message);
  return NXANDROID_UNITY_EINVAL;
}

static nxandroid_unity_result fail_context(nxandroid_unity_context *context,
                                           nxandroid_unity_result result) {
  if (context != NULL)
    context->failed = 1;
  return result;
}

const char *nxandroid_unity_result_string(nxandroid_unity_result result) {
  switch (result) {
  case NXANDROID_UNITY_OK:
    return "ok";
  case NXANDROID_UNITY_EINVAL:
    return "invalid Unity input contract";
  case NXANDROID_UNITY_ESTATE:
    return "Unity input context is fail-stopped or out of order";
  case NXANDROID_UNITY_ECALLBACK:
    return "Unity adapter callback failed";
  case NXANDROID_UNITY_ENOTFOUND:
    return "Unity pad or receipt not found";
  case NXANDROID_UNITY_EDUPLICATE:
    return "duplicate Unity pad or receipt";
  case NXANDROID_UNITY_EUNPROVEN:
    return "Unity profile is classified but not deployable";
  default:
    return "unknown Unity input result";
  }
}

const char *nxandroid_unity_profile_name(nxandroid_unity_profile_kind kind) {
  switch (kind) {
  case NXANDROID_UNITY_LEGACY_INPUT:
    return "unity-legacy-input";
  case NXANDROID_UNITY_NEW_INPUT_SYSTEM:
    return "unity-new-input-system";
  case NXANDROID_UNITY_REWIRED:
    return "rewired";
  case NXANDROID_UNITY_INCONTROL:
    return "incontrol";
  case NXANDROID_UNITY_RAW_ANDROID:
    return "raw-android-inputdevice";
  default:
    return "unknown";
  }
}

static int enum_in_range(int value, int minimum, int maximum) {
  return value >= minimum && value <= maximum;
}

static int bounded_string(const char *value) {
  size_t index;
  if (value == NULL)
    return 0;
  for (index = 0u; index < NXANDROID_UNITY_TEXT_MAX; ++index) {
    if (value[index] == '\0')
      return index != 0u;
  }
  return 0;
}

static int optional_empty(const char *value) {
  return value == NULL || value[0] == '\0';
}

static int exact_text(const char *value) {
  if (!bounded_string(value))
    return 0;
  if (strchr(value, '*') != NULL || strchr(value, '?') != NULL)
    return 0;
  if (strcmp(value, "unknown") == 0 || strcmp(value, "latest") == 0 ||
      strcmp(value, "pending") == 0 || strcmp(value, "N/A") == 0)
    return 0;
  return 1;
}

static int lower_hex(const char *value, size_t length) {
  size_t index;
  if (value == NULL)
    return 0;
  for (index = 0u; index < length; ++index) {
    char c = value[index];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return 0;
  }
  return value[length] == '\0';
}

static int full_signature(const char *value) {
  const char *open;
  const char *close;
  if (!exact_text(value) || strstr(value, "::") == NULL)
    return 0;
  open = strchr(value, '(');
  close = strrchr(value, ')');
  return open != NULL && close != NULL && close > open;
}

static int has(const char *value, const char *needle) {
  return value != NULL && strstr(value, needle) != NULL;
}

static int has_two_dots(const char *value) {
  const char *first;
  if (value == NULL)
    return 0;
  first = strchr(value, '.');
  return first != NULL && strchr(first + 1, '.') != NULL;
}

static int signal_valid_for_control(int control, int signal) {
  int stick = control == NXANDROID_ANDROID_LEFT_STICK ||
              control == NXANDROID_ANDROID_RIGHT_STICK;
  int trigger = control == NXANDROID_ANDROID_L2 ||
                control == NXANDROID_ANDROID_R2;
  if (stick)
    return signal == NXANDROID_ANDROID_SIGNAL_VECTOR;
  if (trigger)
    return signal == NXANDROID_ANDROID_SIGNAL_AXIS ||
           signal == NXANDROID_ANDROID_SIGNAL_BUTTON;
  return signal == NXANDROID_ANDROID_SIGNAL_BUTTON;
}

static int producer_sink_valid(nxandroid_android_sink_kind sink,
                               int button) {
  if (button)
    return sink == NXANDROID_ANDROID_SINK_KEY_EVENT ||
           sink == NXANDROID_ANDROID_SINK_JNI_PUSH ||
           sink == NXANDROID_ANDROID_SINK_NATIVE_GAMEPAD;
  return sink == NXANDROID_ANDROID_SINK_MOTION_EVENT ||
         sink == NXANDROID_ANDROID_SINK_JNI_PUSH ||
         sink == NXANDROID_ANDROID_SINK_NATIVE_GAMEPAD;
}

static int profile_api_family_valid(const nxandroid_unity_profile *profile) {
  switch (profile->kind) {
  case NXANDROID_UNITY_LEGACY_INPUT:
    return has(profile->enumerate_api,
               "UnityEngine.Input::GetJoystickNames") &&
           has(profile->consumer_button_api, "UnityEngine.Input::GetButton") &&
           has(profile->consumer_axis_api, "UnityEngine.Input::GetAxis");
  case NXANDROID_UNITY_NEW_INPUT_SYSTEM:
    return has(profile->register_api,
               "UnityEngine.InputSystem.InputSystem::AddDevice") &&
           has(profile->producer_button_api, "QueueEvent") &&
           has(profile->producer_axis_api, "QueueEvent") &&
           has(profile->enumerate_api, "UnityEngine.InputSystem.Gamepad") &&
           has(profile->consumer_button_api, "ButtonControl") &&
           has(profile->consumer_axis_api, "ReadValue");
  case NXANDROID_UNITY_REWIRED:
    return has(profile->register_api, "Rewired") &&
           has(profile->enumerate_api, "Rewired") &&
           has(profile->consumer_button_api, "Rewired.Player::GetButton") &&
           has(profile->consumer_axis_api, "Rewired.Player::GetAxis");
  case NXANDROID_UNITY_INCONTROL:
    return has(profile->register_api, "InControl") &&
           has(profile->enumerate_api, "InControl") &&
           has(profile->consumer_button_api, "ReadRawButtonState") &&
           has(profile->consumer_axis_api, "ReadRawAnalogValue");
  case NXANDROID_UNITY_RAW_ANDROID:
    return has(profile->register_api, "android.view.InputDevice") &&
           has(profile->enumerate_api, "android.view.InputDevice") &&
           has(profile->consumer_button_api, "android.view.KeyEvent") &&
           has(profile->consumer_axis_api, "android.view.MotionEvent");
  default:
    return 0;
  }
}

nxandroid_unity_result nxandroid_unity_profile_validate(
    const nxandroid_unity_profile *profile, char *error, size_t error_size) {
  const char *const text_fields[] = {
      profile != NULL ? profile->profile_id : NULL,
      profile != NULL ? profile->unity_version : NULL,
      profile != NULL ? profile->plugin_name : NULL,
      profile != NULL ? profile->plugin_version : NULL,
      profile != NULL ? profile->consumer_name : NULL,
      profile != NULL ? profile->consumer_version : NULL,
      profile != NULL ? profile->abi : NULL,
      profile != NULL ? profile->assembly_name : NULL,
      profile != NULL ? profile->license_spdx : NULL};
  const char *const signatures[] = {
      profile != NULL ? profile->register_api : NULL,
      profile != NULL ? profile->unregister_api : NULL,
      profile != NULL ? profile->enumerate_api : NULL,
      profile != NULL ? profile->producer_button_api : NULL,
      profile != NULL ? profile->producer_axis_api : NULL,
      profile != NULL ? profile->consumer_button_api : NULL,
      profile != NULL ? profile->consumer_axis_api : NULL,
      profile != NULL ? profile->consumer_action_api : NULL};
  size_t index;
  int control;

  if (profile == NULL ||
      profile->api_version != NXANDROID_UNITY_INPUT_API_VERSION)
    return fail_with(error, error_size,
                     "Unity profile: NULL or wrong API version");
  if (!enum_in_range((int)profile->kind, NXANDROID_UNITY_LEGACY_INPUT,
                     NXANDROID_UNITY_RAW_ANDROID) ||
      !enum_in_range((int)profile->runtime, NXANDROID_UNITY_RUNTIME_IL2CPP,
                     NXANDROID_UNITY_RUNTIME_MONO) ||
      !enum_in_range((int)profile->evidence,
                     NXANDROID_UNITY_EVIDENCE_FIXTURE,
                     NXANDROID_UNITY_EVIDENCE_NA) ||
      !enum_in_range((int)profile->identity_policy,
                     NXANDROID_UNITY_IDENTITY_PHYSICAL,
                     NXANDROID_UNITY_IDENTITY_PROVEN_OVERRIDE) ||
      !enum_in_range((int)profile->registration_thread,
                     NXANDROID_UNITY_THREAD_PLAYER,
                     NXANDROID_UNITY_THREAD_ANDROID_INPUT) ||
      !enum_in_range((int)profile->producer_thread,
                     NXANDROID_UNITY_THREAD_PLAYER,
                     NXANDROID_UNITY_THREAD_ANDROID_INPUT) ||
      !enum_in_range((int)profile->consumer_thread,
                     NXANDROID_UNITY_THREAD_PLAYER,
                     NXANDROID_UNITY_THREAD_ANDROID_INPUT) ||
      !producer_sink_valid(profile->producer_button_sink, 1) ||
      !producer_sink_valid(profile->producer_axis_sink, 0))
    return fail_with(error, error_size,
                     "Unity profile: invalid enum or thread contract");

  for (index = 0u; index < sizeof(text_fields) / sizeof(text_fields[0]);
       ++index) {
    if (!exact_text(text_fields[index]))
      return fail_with(error, error_size,
                       "Unity profile: identity is not exact and bounded");
  }
  for (index = 0u; index < sizeof(signatures) / sizeof(signatures[0]);
       ++index) {
    if (!full_signature(signatures[index]))
      return fail_with(error, error_size,
                       "Unity profile: API must be a full signature");
  }
  if (!has_two_dots(profile->unity_version) ||
      !has_two_dots(profile->plugin_version))
    return fail_with(error, error_size,
                     "Unity profile: runtime/plugin versions are not exact");
  if (!lower_hex(profile->metadata_sha256, NXANDROID_UNITY_SHA256_LENGTH) ||
      !lower_hex(profile->runtime_sha256, NXANDROID_UNITY_SHA256_LENGTH) ||
      !lower_hex(profile->artifact_sha256, NXANDROID_UNITY_SHA256_LENGTH) ||
      !lower_hex(profile->source_commit, NXANDROID_UNITY_GIT_ID_LENGTH) ||
      !lower_hex(profile->source_tree, NXANDROID_UNITY_GIT_ID_LENGTH))
    return fail_with(error, error_size,
                     "Unity profile: malformed provenance identity");
  if (profile->identity_policy == NXANDROID_UNITY_IDENTITY_PHYSICAL) {
    if (!optional_empty(profile->identity_evidence_sha256) ||
        !optional_empty(profile->identity_name) ||
        !optional_empty(profile->identity_vendor) ||
        !optional_empty(profile->identity_product))
      return fail_with(error, error_size,
                       "Unity profile: physical identity cannot carry override proof");
  } else {
    if (!lower_hex(profile->identity_evidence_sha256,
                   NXANDROID_UNITY_SHA256_LENGTH) ||
        !exact_text(profile->identity_name) ||
        !exact_text(profile->identity_vendor) ||
        !exact_text(profile->identity_product))
      return fail_with(error, error_size,
                       "Unity profile: identity override lacks exact A/B proof or identity");
  }
  if (!profile_api_family_valid(profile))
    return fail_with(error, error_size,
                     "Unity profile: APIs do not match the declared consumer family");

  for (control = 0; control < (int)NXANDROID_ANDROID_CONTROL_COUNT;
       ++control) {
    const nxandroid_unity_control_contract *item =
        &profile->controls[control];
    if (item->reachable != 0 && item->reachable != 1)
      return fail_with(error, error_size,
                       "Unity profile: reachable must be zero or one");
    if (item->reachable) {
      if (!signal_valid_for_control(control, (int)item->signal) ||
          !exact_text(item->consumer_control))
        return fail_with(error, error_size,
                         "Unity profile: invalid reachable control contract");
    } else if (item->signal != 0 || !optional_empty(item->consumer_control)) {
      return fail_with(error, error_size,
                       "Unity profile: unreachable control carries a route");
    }
  }
  return NXANDROID_UNITY_OK;
}

static int context_ready(const nxandroid_unity_context *context) {
  return context != NULL && context->initialized != 0 &&
         context->api_version == NXANDROID_UNITY_INPUT_API_VERSION &&
         context->failed == 0;
}

nxandroid_unity_result nxandroid_unity_context_init(
    nxandroid_unity_context *context,
    const nxandroid_unity_profile *profile, const nxandroid_unity_ops *ops,
    char *error, size_t error_size) {
  nxandroid_unity_result result;
  if (context == NULL || ops == NULL || ops->current_thread_token == NULL ||
      ops->device == NULL || ops->producer == NULL)
    return fail_with(error, error_size,
                     "Unity context: missing context or callback");
  memset(context, 0, sizeof(*context));
  result = nxandroid_unity_profile_validate(profile, error, error_size);
  if (result != NXANDROID_UNITY_OK)
    return result;
  if (profile->evidence != NXANDROID_UNITY_EVIDENCE_FIXTURE &&
      profile->evidence != NXANDROID_UNITY_EVIDENCE_REAL_API_HOST &&
      profile->evidence != NXANDROID_UNITY_EVIDENCE_PHYSICAL)
    return NXANDROID_UNITY_EUNPROVEN;
  context->api_version = NXANDROID_UNITY_INPUT_API_VERSION;
  context->profile = profile;
  context->ops = *ops;
  context->initialized = 1;
  return NXANDROID_UNITY_OK;
}

void nxandroid_unity_context_reset(nxandroid_unity_context *context) {
  if (context != NULL)
    memset(context, 0, sizeof(*context));
}

static uint64_t current_thread(nxandroid_unity_context *context) {
  uint64_t token;
  if (!context_ready(context))
    return 0u;
  token = context->ops.current_thread_token(context->ops.userdata);
  return token;
}

static int thread_matches(nxandroid_unity_context *context,
                          nxandroid_unity_thread_kind kind) {
  uint64_t token = current_thread(context);
  if (token == 0u)
    return 0;
  if (kind == NXANDROID_UNITY_THREAD_PLAYER)
    return token == context->lifecycle_thread_token;
  if (context->input_thread_token == 0u)
    context->input_thread_token = token;
  return token == context->input_thread_token;
}

nxandroid_unity_result nxandroid_unity_lifecycle(
    nxandroid_unity_context *context, nxandroid_unity_lifecycle_phase phase,
    uint64_t cycle_id) {
  static const nxandroid_unity_lifecycle_phase initial[] = {
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
  uint64_t token;

  if (!context_ready(context) || cycle_id == 0u)
    return context != NULL && context->failed ? NXANDROID_UNITY_ESTATE
                                               : NXANDROID_UNITY_EINVAL;
  token = current_thread(context);
  if (token == 0u)
    return fail_context(context, NXANDROID_UNITY_ESTATE);
  if (context->lifecycle_thread_token == 0u)
    context->lifecycle_thread_token = token;
  if (token != context->lifecycle_thread_token)
    return fail_context(context, NXANDROID_UNITY_ESTATE);

  if (context->initial_phase_index <
      sizeof(initial) / sizeof(initial[0])) {
    if (phase != initial[context->initial_phase_index])
      return fail_context(context, NXANDROID_UNITY_ESTATE);
    if (context->initial_phase_index == 0u)
      context->active_cycle = cycle_id;
    if (cycle_id != context->active_cycle)
      return fail_context(context, NXANDROID_UNITY_ESTATE);
    context->initial_phase_index++;
    if (phase == NXANDROID_UNITY_PHASE_SURFACE_CREATE)
      context->surface = 1;
    else if (phase == NXANDROID_UNITY_PHASE_RESUME)
      context->resumed = 1;
    else if (phase == NXANDROID_UNITY_PHASE_FOCUS_GAIN)
      context->focused = 1;
    else if (phase == NXANDROID_UNITY_PHASE_FRAME_LOOP)
      context->live = 1;
    context->lifecycle_events++;
    return NXANDROID_UNITY_OK;
  }

  switch (phase) {
  case NXANDROID_UNITY_PHASE_FOCUS_LOSS:
    if (!context->live || !context->focused)
      return fail_context(context, NXANDROID_UNITY_ESTATE);
    context->focused = 0;
    context->live = 0;
    context->active_cycle = cycle_id;
    break;
  case NXANDROID_UNITY_PHASE_PAUSE:
    if (context->focused || !context->resumed ||
        cycle_id != context->active_cycle)
      return fail_context(context, NXANDROID_UNITY_ESTATE);
    context->resumed = 0;
    break;
  case NXANDROID_UNITY_PHASE_SURFACE_DESTROY:
    if (context->resumed || !context->surface ||
        cycle_id != context->active_cycle)
      return fail_context(context, NXANDROID_UNITY_ESTATE);
    context->surface = 0;
    break;
  case NXANDROID_UNITY_PHASE_SURFACE_CREATE:
    if (context->surface || context->resumed || context->live)
      return fail_context(context, NXANDROID_UNITY_ESTATE);
    context->surface = 1;
    context->active_cycle = cycle_id;
    break;
  case NXANDROID_UNITY_PHASE_SURFACE_CHANGE:
    if (!context->surface || context->resumed ||
        cycle_id != context->active_cycle)
      return fail_context(context, NXANDROID_UNITY_ESTATE);
    break;
  case NXANDROID_UNITY_PHASE_RESUME:
    if (!context->surface || context->resumed ||
        cycle_id != context->active_cycle)
      return fail_context(context, NXANDROID_UNITY_ESTATE);
    context->resumed = 1;
    break;
  case NXANDROID_UNITY_PHASE_FOCUS_GAIN:
    if (!context->resumed || context->focused ||
        cycle_id != context->active_cycle)
      return fail_context(context, NXANDROID_UNITY_ESTATE);
    context->focused = 1;
    break;
  case NXANDROID_UNITY_PHASE_FRAME_LOOP:
    if (!context->surface || !context->resumed || !context->focused ||
        context->live || cycle_id != context->active_cycle)
      return fail_context(context, NXANDROID_UNITY_ESTATE);
    context->live = 1;
    break;
  case NXANDROID_UNITY_PHASE_SHUTDOWN:
    {
      size_t pad_index_value;
      for (pad_index_value = 0u; pad_index_value < NXANDROID_UNITY_MAX_PADS;
           ++pad_index_value) {
        if (context->pads[pad_index_value].connected)
          return fail_context(context, NXANDROID_UNITY_ESTATE);
      }
    }
    if (context->live || context->focused || context->resumed ||
        context->surface || context->shutdown ||
        cycle_id != context->active_cycle)
      return fail_context(context, NXANDROID_UNITY_ESTATE);
    context->shutdown = 1;
    break;
  default:
    return fail_context(context, NXANDROID_UNITY_ESTATE);
  }
  context->lifecycle_events++;
  return NXANDROID_UNITY_OK;
}

static int pad_index(const nxandroid_unity_context *context,
                     int32_t instance_id) {
  size_t index;
  if (context == NULL)
    return -1;
  for (index = 0u; index < NXANDROID_UNITY_MAX_PADS; ++index) {
    if (context->pads[index].connected &&
        context->pads[index].instance_id == instance_id)
      return (int)index;
  }
  return -1;
}

static int free_pad_index(const nxandroid_unity_context *context) {
  size_t index;
  for (index = 0u; index < NXANDROID_UNITY_MAX_PADS; ++index) {
    if (!context->pads[index].connected)
      return (int)index;
  }
  return -1;
}

static int pending_complete(const nxandroid_unity_pending_state *pending) {
  return pending->valid && pending->low_level_returned &&
         pending->action_returned;
}

static int callback_ack_valid(const nxandroid_unity_api_ack *ack,
                              uint64_t sequence) {
  return ack->api_version == NXANDROID_UNITY_INPUT_API_VERSION &&
         ack->sequence == sequence && ack->returned == 1 &&
         (ack->handled == 0 || ack->handled == 1);
}

static int c7_ack(nxandroid_android_ack *ack, uint64_t sequence, int handled,
                  int32_t return_value) {
  ack->api_version = NXANDROID_ANDROID_INPUT_API_VERSION;
  ack->sequence = sequence;
  ack->handled = handled;
  ack->return_value = return_value;
  return 0;
}

static int device_event(nxandroid_unity_context *context,
                        const nxandroid_android_event *event,
                        nxandroid_android_ack *ack) {
  nxandroid_unity_device_request request;
  nxandroid_unity_api_ack api_ack;
  nxandroid_unity_device_operation operation;
  int slot;

  if (!context->live || !context->focused || !context->resumed ||
      !thread_matches(context, context->profile->registration_thread))
    return (int)fail_context(context, NXANDROID_UNITY_ESTATE);
  operation = event->kind == NXANDROID_ANDROID_EVENT_PAD_ADDED
                  ? NXANDROID_UNITY_DEVICE_ADD
                  : NXANDROID_UNITY_DEVICE_REMOVE;
  slot = pad_index(context, event->instance_id);
  if (operation == NXANDROID_UNITY_DEVICE_ADD) {
    if (slot >= 0)
      return (int)fail_context(context, NXANDROID_UNITY_EDUPLICATE);
    slot = free_pad_index(context);
    if (slot < 0)
      return (int)fail_context(context, NXANDROID_UNITY_ESTATE);
  } else {
    if (slot < 0 || context->pads[slot].generation != event->generation)
      return (int)fail_context(context, NXANDROID_UNITY_ENOTFOUND);
  }

  memset(&request, 0, sizeof(request));
  memset(&api_ack, 0, sizeof(api_ack));
  request.api_version = NXANDROID_UNITY_INPUT_API_VERSION;
  request.operation = operation;
  request.profile_kind = context->profile->kind;
  request.sequence = event->sequence;
  request.instance_id = event->instance_id;
  request.device_id = event->device_id;
  request.generation = event->generation;
  request.source = event->source;
  request.identity_policy = context->profile->identity_policy;
  request.identity_name = context->profile->identity_name;
  request.identity_vendor = context->profile->identity_vendor;
  request.identity_product = context->profile->identity_product;
  request.api_signature = operation == NXANDROID_UNITY_DEVICE_ADD
                              ? context->profile->register_api
                              : context->profile->unregister_api;
  if (context->ops.device(context->ops.userdata, &request, &api_ack) != 0 ||
      !callback_ack_valid(&api_ack, event->sequence))
    return (int)fail_context(context, NXANDROID_UNITY_ECALLBACK);

  if (operation == NXANDROID_UNITY_DEVICE_ADD) {
    memset(&context->pads[slot], 0, sizeof(context->pads[slot]));
    context->pads[slot].connected = 1;
    context->pads[slot].instance_id = event->instance_id;
    context->pads[slot].device_id = event->device_id;
    context->pads[slot].generation = event->generation;
  } else {
    int control;
    for (control = 0; control < (int)NXANDROID_ANDROID_CONTROL_COUNT;
         ++control) {
      nxandroid_unity_pending_state *pending =
          &context->pads[slot].pending[control];
      if (pending->valid && !pending_complete(pending))
        context->invalidated_pending++;
    }
    memset(&context->pads[slot], 0, sizeof(context->pads[slot]));
  }
  context->device_calls++;
  return c7_ack(ack, event->sequence, api_ack.handled,
                api_ack.return_value);
}

static int lifecycle_event(nxandroid_unity_context *context,
                           const nxandroid_android_event *event,
                           nxandroid_android_ack *ack) {
  switch (event->kind) {
  case NXANDROID_ANDROID_EVENT_FOCUS_GAINED:
    if (!context->focused || !context->live)
      return (int)fail_context(context, NXANDROID_UNITY_ESTATE);
    break;
  case NXANDROID_ANDROID_EVENT_FOCUS_LOST:
    if (!context->focused || !context->live)
      return (int)fail_context(context, NXANDROID_UNITY_ESTATE);
    break;
  case NXANDROID_ANDROID_EVENT_RESUMED:
    if (!context->resumed || !context->live)
      return (int)fail_context(context, NXANDROID_UNITY_ESTATE);
    break;
  case NXANDROID_ANDROID_EVENT_PAUSED:
    if (!context->resumed)
      return (int)fail_context(context, NXANDROID_UNITY_ESTATE);
    break;
  case NXANDROID_ANDROID_EVENT_CANCELLED:
    break;
  default:
    return (int)fail_context(context, NXANDROID_UNITY_EINVAL);
  }
  return c7_ack(ack, event->sequence, 1, 1);
}

static int producer_event(nxandroid_unity_context *context,
                          const nxandroid_android_event *event,
                          nxandroid_android_ack *ack) {
  nxandroid_unity_producer_request request;
  nxandroid_unity_api_ack api_ack;
  nxandroid_unity_pad_state *pad;
  nxandroid_unity_pending_state *pending;
  const nxandroid_unity_control_contract *contract;
  const char *signature;
  int slot;

  if (!context->live || !context->focused || !context->resumed ||
      event->control < NXANDROID_ANDROID_A ||
      event->control > NXANDROID_ANDROID_RIGHT_STICK ||
      !thread_matches(context, context->profile->producer_thread))
    return (int)fail_context(context, NXANDROID_UNITY_ESTATE);
  slot = pad_index(context, event->instance_id);
  if (slot < 0)
    return (int)fail_context(context, NXANDROID_UNITY_ENOTFOUND);
  pad = &context->pads[slot];
  if (pad->generation != event->generation ||
      pad->device_id != event->device_id)
    return (int)fail_context(context, NXANDROID_UNITY_ESTATE);
  contract = &context->profile->controls[event->control];
  if (!contract->reachable || contract->signal != event->signal ||
      (event->signal == NXANDROID_ANDROID_SIGNAL_BUTTON
           ? event->sink != context->profile->producer_button_sink
           : event->sink != context->profile->producer_axis_sink))
    return (int)fail_context(context, NXANDROID_UNITY_ESTATE);
  pending = &pad->pending[event->control];
  if (pending->valid && !pending_complete(pending))
    return (int)fail_context(context, NXANDROID_UNITY_ESTATE);
  signature = event->signal == NXANDROID_ANDROID_SIGNAL_BUTTON
                  ? context->profile->producer_button_api
                  : context->profile->producer_axis_api;

  memset(&request, 0, sizeof(request));
  memset(&api_ack, 0, sizeof(api_ack));
  request.api_version = NXANDROID_UNITY_INPUT_API_VERSION;
  request.profile_kind = context->profile->kind;
  request.sequence = event->sequence;
  request.instance_id = event->instance_id;
  request.device_id = event->device_id;
  request.generation = event->generation;
  request.control = event->control;
  request.event_kind = event->kind;
  request.signal = event->signal;
  request.pressed = event->pressed;
  request.value = event->value;
  request.x = event->x;
  request.y = event->y;
  request.action = event->action;
  request.api_signature = signature;
  if (context->ops.producer(context->ops.userdata, &request, &api_ack) != 0 ||
      !callback_ack_valid(&api_ack, event->sequence))
    return (int)fail_context(context, NXANDROID_UNITY_ECALLBACK);

  memset(pending, 0, sizeof(*pending));
  pending->sequence = event->sequence;
  pending->valid = 1u;
  pending->signal = (uint8_t)event->signal;
  pending->pressed = event->pressed;
  pending->value = event->value;
  pending->x = event->x;
  pending->y = event->y;
  context->producer_returns++;
  return c7_ack(ack, event->sequence, api_ack.handled,
                api_ack.return_value);
}

int nxandroid_unity_android_event(void *userdata,
                                  const nxandroid_android_event *event,
                                  nxandroid_android_ack *ack) {
  nxandroid_unity_context *context = (nxandroid_unity_context *)userdata;
  if (!context_ready(context) || event == NULL || ack == NULL ||
      event->api_version != NXANDROID_ANDROID_INPUT_API_VERSION)
    return -1;
  switch (event->kind) {
  case NXANDROID_ANDROID_EVENT_PAD_ADDED:
  case NXANDROID_ANDROID_EVENT_PAD_REMOVED:
    return device_event(context, event, ack);
  case NXANDROID_ANDROID_EVENT_FOCUS_GAINED:
  case NXANDROID_ANDROID_EVENT_FOCUS_LOST:
  case NXANDROID_ANDROID_EVENT_RESUMED:
  case NXANDROID_ANDROID_EVENT_PAUSED:
  case NXANDROID_ANDROID_EVENT_CANCELLED:
    return lifecycle_event(context, event, ack);
  case NXANDROID_ANDROID_EVENT_BUTTON:
  case NXANDROID_ANDROID_EVENT_AXIS:
  case NXANDROID_ANDROID_EVENT_VECTOR:
  case NXANDROID_ANDROID_EVENT_TOUCH_DOWN:
  case NXANDROID_ANDROID_EVENT_TOUCH_UP:
  case NXANDROID_ANDROID_EVENT_TOUCH_MOVE:
  case NXANDROID_ANDROID_EVENT_CURSOR_MOVE:
    return producer_event(context, event, ack);
  default:
    return -1;
  }
}

static int float_matches(float actual, float expected) {
  return isfinite(actual) && isfinite(expected) &&
         fabsf(actual - expected) <= UNITY_EPSILON;
}

nxandroid_unity_result nxandroid_unity_note_consumer_return(
    nxandroid_unity_context *context,
    const nxandroid_unity_consumer_return *returned) {
  nxandroid_unity_pad_state *pad;
  nxandroid_unity_pending_state *pending;
  const char *expected_signature;
  int slot;

  if (!context_ready(context) || returned == NULL ||
      returned->api_version != NXANDROID_UNITY_INPUT_API_VERSION ||
      returned->api_returned != 1 ||
      (returned->handled != 0 && returned->handled != 1) ||
      returned->control < NXANDROID_ANDROID_A ||
      returned->control > NXANDROID_ANDROID_RIGHT_STICK ||
      !full_signature(returned->api_signature) ||
      !enum_in_range((int)returned->stage,
                     NXANDROID_UNITY_API_LOW_LEVEL_RETURN,
                     NXANDROID_UNITY_API_ACTION_RETURN) ||
      !thread_matches(context, context->profile->consumer_thread))
    return fail_context(context, NXANDROID_UNITY_EINVAL);
  slot = pad_index(context, returned->instance_id);
  if (slot < 0)
    return fail_context(context, NXANDROID_UNITY_ENOTFOUND);
  pad = &context->pads[slot];
  if (pad->generation != returned->generation)
    return fail_context(context, NXANDROID_UNITY_ESTATE);
  pending = &pad->pending[returned->control];
  if (!pending->valid || pending->sequence != returned->event_sequence ||
      pending->signal != returned->signal ||
      pending->pressed != returned->pressed ||
      !float_matches(pending->value, returned->value) ||
      !float_matches(pending->x, returned->x) ||
      !float_matches(pending->y, returned->y))
    return fail_context(context, NXANDROID_UNITY_ESTATE);

  if (returned->stage == NXANDROID_UNITY_API_LOW_LEVEL_RETURN) {
    if (pending->low_level_returned)
      return fail_context(context, NXANDROID_UNITY_EDUPLICATE);
    expected_signature =
        pending->signal == NXANDROID_ANDROID_SIGNAL_BUTTON
            ? context->profile->consumer_button_api
            : context->profile->consumer_axis_api;
    if (strcmp(returned->api_signature, expected_signature) != 0)
      return fail_context(context, NXANDROID_UNITY_EINVAL);
    pending->low_level_returned = 1u;
    context->low_level_returns++;
  } else {
    if (!pending->low_level_returned || pending->action_returned ||
        strcmp(returned->api_signature,
               context->profile->consumer_action_api) != 0)
      return fail_context(context, NXANDROID_UNITY_ESTATE);
    pending->action_returned = 1u;
    context->action_returns++;
  }
  if (pending_complete(pending))
    context->complete_receipts++;
  return NXANDROID_UNITY_OK;
}

static const char *evidence_name(nxandroid_unity_evidence_class evidence) {
  switch (evidence) {
  case NXANDROID_UNITY_EVIDENCE_FIXTURE:
    return "FIXTURE";
  case NXANDROID_UNITY_EVIDENCE_REAL_API_HOST:
    return "REAL_API_HOST";
  case NXANDROID_UNITY_EVIDENCE_PHYSICAL:
    return "PHYSICAL";
  case NXANDROID_UNITY_EVIDENCE_PENDING:
    return "PENDING";
  case NXANDROID_UNITY_EVIDENCE_UNPROVEN:
    return "UNPROVEN";
  case NXANDROID_UNITY_EVIDENCE_NA:
    return "N/A";
  default:
    return "INVALID";
  }
}

nxandroid_unity_result nxandroid_unity_receipt(
    const nxandroid_unity_context *context, char *output, size_t output_size) {
  int written;
  if (!context_ready(context) || output == NULL || output_size == 0u)
    return NXANDROID_UNITY_EINVAL;
  written = snprintf(
      output, output_size,
      "profile=%s evidence=%s device_calls=%llu producer_returns=%llu "
      "low_level_returns=%llu action_returns=%llu complete_receipts=%llu "
      "invalidated_pending=%llu lifecycle_events=%llu failed=0",
      nxandroid_unity_profile_name(context->profile->kind),
      evidence_name(context->profile->evidence),
      (unsigned long long)context->device_calls,
      (unsigned long long)context->producer_returns,
      (unsigned long long)context->low_level_returns,
      (unsigned long long)context->action_returns,
      (unsigned long long)context->complete_receipts,
      (unsigned long long)context->invalidated_pending,
      (unsigned long long)context->lifecycle_events);
  if (written < 0 || (size_t)written >= output_size)
    return NXANDROID_UNITY_EINVAL;
  return NXANDROID_UNITY_OK;
}
