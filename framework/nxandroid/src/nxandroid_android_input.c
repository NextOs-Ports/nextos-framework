/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxandroid_android_input.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define NXANDROID_TRIGGER_ENTER 0.60f
#define NXANDROID_TRIGGER_EXIT 0.40f
#define NXANDROID_FLOAT_EPSILON 0.00001f

static nxandroid_android_result fail_with(char *error, size_t error_size,
                                          const char *message) {
  if (error != NULL && error_size > 0u)
    snprintf(error, error_size, "%s", message);
  return NXANDROID_ANDROID_EINVAL;
}

const char *nxandroid_android_result_string(nxandroid_android_result result) {
  switch (result) {
  case NXANDROID_ANDROID_OK:
    return "ok";
  case NXANDROID_ANDROID_EINVAL:
    return "invalid Android input contract";
  case NXANDROID_ANDROID_EDUPLICATE:
    return "duplicate Android input identity or route";
  case NXANDROID_ANDROID_ENOTFOUND:
    return "Android input identity or route not found";
  case NXANDROID_ANDROID_EFULL:
    return "Android input capacity exhausted";
  case NXANDROID_ANDROID_ESTATE:
    return "Android input context is fail-stopped";
  case NXANDROID_ANDROID_ECALLBACK:
    return "Android consumer callback failed";
  default:
    return "unknown Android input result";
  }
}

const char *nxandroid_android_control_name(nxandroid_android_control control) {
  static const char *const names[NXANDROID_ANDROID_CONTROL_COUNT] = {
      "A",          "B",          "X",          "Y",
      "L1",         "R1",         "L2",         "R2",
      "L3",         "R3",         "START",      "SELECT",
      "UP",         "DOWN",       "LEFT",       "RIGHT",
      "LEFT_STICK", "RIGHT_STICK"};
  if ((int)control < 0 || (unsigned int)control >=
                              NXANDROID_ANDROID_CONTROL_COUNT)
    return "UNKNOWN";
  return names[(unsigned int)control];
}

const char *nxandroid_android_sink_name(nxandroid_android_sink_kind sink) {
  switch (sink) {
  case NXANDROID_ANDROID_SINK_KEY_EVENT:
    return "keyevent";
  case NXANDROID_ANDROID_SINK_MOTION_EVENT:
    return "motionevent";
  case NXANDROID_ANDROID_SINK_JNI_PUSH:
    return "jni-push";
  case NXANDROID_ANDROID_SINK_JNI_PULL:
    return "jni-pull";
  case NXANDROID_ANDROID_SINK_NATIVE_GAMEPAD:
    return "native-gamepad";
  case NXANDROID_ANDROID_SINK_TOUCH:
    return "touch";
  default:
    return "none";
  }
}

static float clamp_unit(float value) {
  if (!(value > 0.0f))
    return 0.0f;
  if (value > 1.0f)
    return 1.0f;
  return value;
}

static float clamp_signed(float value) {
  if (value < -1.0f)
    return -1.0f;
  if (value > 1.0f)
    return 1.0f;
  return value;
}

nxandroid_android_result nxandroid_android_touch_resolve(
    const nxandroid_android_touch_geometry *geometry, float norm_x,
    float norm_y, int *out_x, int *out_y, char *error, size_t error_size) {
  int safe_right;
  int safe_bottom;
  int content_right;
  int content_bottom;
  int left;
  int top;
  int right;
  int bottom;
  float rotated_x;
  float rotated_y;
  int pixel_x;
  int pixel_y;

  if (geometry == NULL || out_x == NULL || out_y == NULL)
    return fail_with(error, error_size,
                     "Android touch: NULL geometry or output");
  if (geometry->drawable_width <= 0 || geometry->drawable_height <= 0 ||
      geometry->content_width <= 0 || geometry->content_height <= 0)
    return fail_with(error, error_size,
                     "Android touch: non-positive drawable/content rect");
  if (geometry->content_left < 0 || geometry->content_top < 0 ||
      geometry->safe_left < 0 || geometry->safe_top < 0 ||
      geometry->safe_right < 0 || geometry->safe_bottom < 0)
    return fail_with(error, error_size,
                     "Android touch: negative rectangle or inset");
  if (geometry->content_left >
          geometry->drawable_width - geometry->content_width ||
      geometry->content_top >
          geometry->drawable_height - geometry->content_height)
    return fail_with(error, error_size,
                     "Android touch: content rect leaves drawable");
  if (geometry->safe_left >= geometry->drawable_width ||
      geometry->safe_right >=
          geometry->drawable_width - geometry->safe_left ||
      geometry->safe_top >= geometry->drawable_height ||
      geometry->safe_bottom >=
          geometry->drawable_height - geometry->safe_top)
    return fail_with(error, error_size,
                     "Android touch: safe area consumes drawable");

  safe_right = geometry->drawable_width - geometry->safe_right;
  safe_bottom = geometry->drawable_height - geometry->safe_bottom;
  content_right = geometry->content_left + geometry->content_width;
  content_bottom = geometry->content_top + geometry->content_height;
  left = geometry->content_left > geometry->safe_left
             ? geometry->content_left
             : geometry->safe_left;
  top = geometry->content_top > geometry->safe_top ? geometry->content_top
                                                    : geometry->safe_top;
  right = content_right < safe_right ? content_right : safe_right;
  bottom = content_bottom < safe_bottom ? content_bottom : safe_bottom;
  if (left >= right || top >= bottom)
    return fail_with(error, error_size,
                     "Android touch: content and safe area do not intersect");

  norm_x = clamp_unit(norm_x);
  norm_y = clamp_unit(norm_y);
  switch (geometry->rotation_degrees) {
  case 0:
    rotated_x = norm_x;
    rotated_y = norm_y;
    break;
  case 90:
    rotated_x = 1.0f - norm_y;
    rotated_y = norm_x;
    break;
  case 180:
    rotated_x = 1.0f - norm_x;
    rotated_y = 1.0f - norm_y;
    break;
  case 270:
    rotated_x = norm_y;
    rotated_y = 1.0f - norm_x;
    break;
  default:
    return fail_with(error, error_size,
                     "Android touch: rotation must be 0/90/180/270");
  }

  pixel_x = left + (int)(rotated_x * (float)(right - left));
  pixel_y = top + (int)(rotated_y * (float)(bottom - top));
  if (pixel_x >= right)
    pixel_x = right - 1;
  if (pixel_y >= bottom)
    pixel_y = bottom - 1;
  if (pixel_x < left)
    pixel_x = left;
  if (pixel_y < top)
    pixel_y = top;
  *out_x = pixel_x;
  *out_y = pixel_y;
  return NXANDROID_ANDROID_OK;
}

static int control_valid(int control) {
  return control >= 0 &&
         (unsigned int)control < NXANDROID_ANDROID_CONTROL_COUNT;
}

static int context_valid(int context) {
  return context >= 0 &&
         (unsigned int)context < NXANDROID_ANDROID_CONTEXT_COUNT;
}

static int source_valid(int source) {
  return source >= (int)NXANDROID_ANDROID_SOURCE_GET_CONTROLS &&
         source <= (int)NXANDROID_ANDROID_SOURCE_RAW_DECLARED;
}

static int sink_valid(int sink) {
  return sink >= (int)NXANDROID_ANDROID_SINK_KEY_EVENT &&
         sink <= (int)NXANDROID_ANDROID_SINK_TOUCH;
}

static int signal_valid(int signal) {
  return signal >= (int)NXANDROID_ANDROID_SIGNAL_BUTTON &&
         signal <= (int)NXANDROID_ANDROID_SIGNAL_CURSOR;
}

static int is_trigger(int control) {
  return control == NXANDROID_ANDROID_L2 ||
         control == NXANDROID_ANDROID_R2;
}

static int is_stick(int control) {
  return control == NXANDROID_ANDROID_LEFT_STICK ||
         control == NXANDROID_ANDROID_RIGHT_STICK;
}

static int route_shape_valid(int sink, int signal) {
  if (!sink_valid(sink) || !signal_valid(signal))
    return 0;
  switch ((nxandroid_android_sink_kind)sink) {
  case NXANDROID_ANDROID_SINK_KEY_EVENT:
    return signal == NXANDROID_ANDROID_SIGNAL_BUTTON;
  case NXANDROID_ANDROID_SINK_MOTION_EVENT:
    return signal == NXANDROID_ANDROID_SIGNAL_AXIS ||
           signal == NXANDROID_ANDROID_SIGNAL_VECTOR;
  case NXANDROID_ANDROID_SINK_JNI_PUSH:
  case NXANDROID_ANDROID_SINK_JNI_PULL:
  case NXANDROID_ANDROID_SINK_NATIVE_GAMEPAD:
    return signal == NXANDROID_ANDROID_SIGNAL_BUTTON ||
           signal == NXANDROID_ANDROID_SIGNAL_AXIS ||
           signal == NXANDROID_ANDROID_SIGNAL_VECTOR;
  case NXANDROID_ANDROID_SINK_TOUCH:
    return signal == NXANDROID_ANDROID_SIGNAL_TOUCH_FIXED ||
           signal == NXANDROID_ANDROID_SIGNAL_TOUCH_CURSOR ||
           signal == NXANDROID_ANDROID_SIGNAL_CURSOR;
  default:
    return 0;
  }
}

static int native_route_shape_valid(int sink, int signal) {
  if (sink == NXANDROID_ANDROID_SINK_TOUCH)
    return 0;
  return route_shape_valid(sink, signal);
}

static int signal_accepts_control(int signal, int control) {
  if (!control_valid(control))
    return 0;
  if (is_stick(control))
    return signal == NXANDROID_ANDROID_SIGNAL_VECTOR ||
           signal == NXANDROID_ANDROID_SIGNAL_CURSOR;
  if (is_trigger(control))
    return signal == NXANDROID_ANDROID_SIGNAL_AXIS ||
           signal == NXANDROID_ANDROID_SIGNAL_BUTTON ||
           signal == NXANDROID_ANDROID_SIGNAL_TOUCH_FIXED ||
           signal == NXANDROID_ANDROID_SIGNAL_TOUCH_CURSOR;
  return signal == NXANDROID_ANDROID_SIGNAL_BUTTON ||
         signal == NXANDROID_ANDROID_SIGNAL_TOUCH_FIXED ||
         signal == NXANDROID_ANDROID_SIGNAL_TOUCH_CURSOR;
}

static int bounded_string(const char *value, size_t capacity) {
  size_t length;
  if (value == NULL || capacity == 0u)
    return 0;
  for (length = 0u; length < capacity; ++length) {
    if (value[length] == '\0')
      return length != 0u;
  }
  return 0;
}

static int route_index(const nxandroid_android_context *context,
                       const char *action) {
  size_t index;
  if (context == NULL || action == NULL)
    return -1;
  for (index = 0u; index < context->route_count; ++index) {
    if (strcmp(context->routes[index].action, action) == 0)
      return (int)index;
  }
  return -1;
}

static int native_index(const nxandroid_android_context *context,
                        int control) {
  size_t index;
  if (context == NULL)
    return -1;
  for (index = 0u; index < context->native_route_count; ++index) {
    if ((int)context->native_routes[index].control == control)
      return (int)index;
  }
  return -1;
}

static nxandroid_android_result validate_authority_and_routes(
    nxandroid_android_context *context, char *error, size_t error_size) {
  int ctx;
  int control;
  size_t route;
  size_t native_route;

  if (context->authority.api_version != NXANDROID_ANDROID_INPUT_API_VERSION ||
      context->authority.schema_version != 2u ||
      context->authority.context_present[NXANDROID_ANDROID_MENU] == 0u ||
      context->authority.context_present[NXANDROID_ANDROID_GAMEPLAY] == 0u)
    return fail_with(error, error_size,
                     "Android input requires complete GPTK V2 menu/gameplay");

  for (route = 0u; route < context->route_count; ++route) {
    size_t other;
    int used = 0;
    if (!bounded_string(context->routes[route].action,
                        NXANDROID_ANDROID_ACTION_MAX) ||
        !route_shape_valid(context->routes[route].sink,
                           context->routes[route].signal))
      return fail_with(error, error_size,
                       "Android action route has invalid action/sink/signal");
    for (other = route + 1u; other < context->route_count; ++other) {
      if (strcmp(context->routes[route].action,
                 context->routes[other].action) == 0)
        return NXANDROID_ANDROID_EDUPLICATE;
    }
    for (ctx = 0; ctx < (int)NXANDROID_ANDROID_CONTEXT_COUNT; ++ctx) {
      for (control = 0; control < (int)NXANDROID_ANDROID_CONTROL_COUNT;
           ++control) {
        if (context->authority.decision[ctx][control] ==
                NXANDROID_ANDROID_DECIDE_ACTION &&
            bounded_string(context->authority.action[ctx][control],
                           NXANDROID_ANDROID_ACTION_MAX) &&
            strcmp(context->authority.action[ctx][control],
                   context->routes[route].action) == 0) {
          if (!signal_accepts_control(context->routes[route].signal, control))
            return fail_with(error, error_size,
                             "Android action route mismatches control shape");
          used = 1;
        }
      }
    }
    if (!used)
      return fail_with(error, error_size,
                       "Android action route is not governed by GPTK V2");
  }

  for (native_route = 0u; native_route < context->native_route_count;
       ++native_route) {
    size_t other;
    int used = 0;
    int native_control = (int)context->native_routes[native_route].control;
    if (!control_valid(native_control) ||
        !native_route_shape_valid(context->native_routes[native_route].sink,
                                  context->native_routes[native_route].signal) ||
        !signal_accepts_control(context->native_routes[native_route].signal,
                                native_control))
      return fail_with(error, error_size,
                       "Android native route has invalid control/sink/signal");
    for (other = native_route + 1u; other < context->native_route_count;
         ++other) {
      if (context->native_routes[other].control ==
          context->native_routes[native_route].control)
        return NXANDROID_ANDROID_EDUPLICATE;
    }
    for (ctx = 0; ctx < (int)NXANDROID_ANDROID_CONTEXT_COUNT; ++ctx) {
      if (context->authority.decision[ctx][native_control] ==
          NXANDROID_ANDROID_DECIDE_NATIVE)
        used = 1;
    }
    if (!used)
      return fail_with(error, error_size,
                       "Android native route lacks a GPTK native decision");
  }

  for (ctx = 0; ctx < (int)NXANDROID_ANDROID_CONTEXT_COUNT; ++ctx) {
    for (control = 0; control < (int)NXANDROID_ANDROID_CONTROL_COUNT;
         ++control) {
      int decision = context->authority.decision[ctx][control];
      const char *action = context->authority.action[ctx][control];
      if (decision < NXANDROID_ANDROID_DECIDE_NONE ||
          decision > NXANDROID_ANDROID_DECIDE_NATIVE)
        return fail_with(error, error_size,
                         "Android authority contains unknown decision");
      if (context->authority.context_present[ctx] == 0u &&
          decision != NXANDROID_ANDROID_DECIDE_NONE)
        return fail_with(error, error_size,
                         "Android absent context carries a decision");
      if (context->authority.context_present[ctx] != 0u &&
          decision == NXANDROID_ANDROID_DECIDE_NONE)
        return fail_with(error, error_size,
                         "Android GPTK V2 present context is incomplete");
      if (decision == NXANDROID_ANDROID_DECIDE_ACTION) {
        if (!bounded_string(action, NXANDROID_ANDROID_ACTION_MAX) ||
            route_index(context, action) < 0)
          return fail_with(error, error_size,
                           "Android GPTK action lacks its single route");
      } else if (action[0] != '\0') {
        return fail_with(error, error_size,
                         "Android non-action authority carries action bytes");
      }
      if (decision == NXANDROID_ANDROID_DECIDE_NATIVE &&
          native_index(context, control) < 0)
        return fail_with(error, error_size,
                         "Android GPTK native lacks its single route");
    }
  }
  return NXANDROID_ANDROID_OK;
}

nxandroid_android_result nxandroid_android_context_init(
    nxandroid_android_context *context,
    const nxandroid_android_authority *authority,
    const nxandroid_android_profile *profile, char *error,
    size_t error_size) {
  size_t index;
  int needs_touch = 0;
  int needs_cursor = 0;
  int touch_x;
  int touch_y;
  nxandroid_android_result checked;

  if (context == NULL || authority == NULL || profile == NULL ||
      profile->event == NULL ||
      !bounded_string(profile->consumer_id, NXANDROID_ANDROID_ID_MAX) ||
      !bounded_string(profile->consumer_version, NXANDROID_ANDROID_ID_MAX) ||
      profile->route_count > NXANDROID_ANDROID_MAX_ROUTES ||
      profile->native_route_count > NXANDROID_ANDROID_MAX_NATIVE_ROUTES ||
      (profile->route_count > 0u && profile->routes == NULL) ||
      (profile->native_route_count > 0u && profile->native_routes == NULL))
    return fail_with(error, error_size,
                     "Android input profile is incomplete or oversized");

  memset(context, 0, sizeof(*context));
  context->api_version = NXANDROID_ANDROID_INPUT_API_VERSION;
  memcpy(&context->authority, authority, sizeof(*authority));
  memcpy(context->consumer_id, profile->consumer_id,
         strlen(profile->consumer_id) + 1u);
  memcpy(context->consumer_version, profile->consumer_version,
         strlen(profile->consumer_version) + 1u);
  context->route_count = profile->route_count;
  for (index = 0u; index < profile->route_count; ++index) {
    const nxandroid_android_route *source = &profile->routes[index];
    nxandroid_android_stored_route *destination = &context->routes[index];
    if (!bounded_string(source->action, NXANDROID_ANDROID_ACTION_MAX)) {
      memset(context, 0, sizeof(*context));
      return fail_with(error, error_size,
                       "Android action route has invalid action name");
    }
    memcpy(destination->action, source->action, strlen(source->action) + 1u);
    destination->sink = (uint8_t)source->sink;
    destination->signal = (uint8_t)source->signal;
    destination->code_x = source->code_x;
    destination->code_y = source->code_y;
    destination->norm_x = source->norm_x;
    destination->norm_y = source->norm_y;
    if (source->signal == NXANDROID_ANDROID_SIGNAL_TOUCH_FIXED &&
        (!isfinite(source->norm_x) || !isfinite(source->norm_y) ||
         source->norm_x < 0.0f || source->norm_x > 1.0f ||
         source->norm_y < 0.0f || source->norm_y > 1.0f)) {
      memset(context, 0, sizeof(*context));
      return fail_with(error, error_size,
                       "Android fixed touch coordinate is outside [0,1]");
    }
    if (source->sink == NXANDROID_ANDROID_SINK_TOUCH)
      needs_touch = 1;
    if (source->signal == NXANDROID_ANDROID_SIGNAL_CURSOR)
      needs_cursor = 1;
  }
  context->native_route_count = profile->native_route_count;
  if (profile->native_route_count > 0u)
    memcpy(context->native_routes, profile->native_routes,
           profile->native_route_count * sizeof(profile->native_routes[0]));
  context->touch = profile->touch;
  context->cursor = profile->cursor;
  context->event = profile->event;
  context->userdata = profile->userdata;
  context->focused = 1;
  context->resumed = 1;
  context->active_context = NXANDROID_ANDROID_MENU;

  checked = validate_authority_and_routes(context, error, error_size);
  if (checked != NXANDROID_ANDROID_OK) {
    memset(context, 0, sizeof(*context));
    return checked;
  }
  if (needs_touch && nxandroid_android_touch_resolve(
                         &context->touch, 0.5f, 0.5f, &touch_x, &touch_y,
                         error, error_size) != NXANDROID_ANDROID_OK) {
    memset(context, 0, sizeof(*context));
    return NXANDROID_ANDROID_EINVAL;
  }
  if (needs_cursor &&
      (!(context->cursor.initial_x >= 0.0f) ||
       context->cursor.initial_x > 1.0f ||
       !(context->cursor.initial_y >= 0.0f) ||
       context->cursor.initial_y > 1.0f ||
       !(context->cursor.speed_screen_heights_per_second > 0.0f) ||
       !isfinite(context->cursor.speed_screen_heights_per_second) ||
       context->cursor.speed_screen_heights_per_second > 8.0f ||
       !(context->cursor.deadzone >= 0.0f) ||
       context->cursor.deadzone >= 0.9f ||
       !(context->cursor.response_curve > 0.0f) ||
       !isfinite(context->cursor.response_curve) ||
       context->cursor.response_curve > 4.0f ||
       !(context->cursor.smoothing_seconds >= 0.0f) ||
       !isfinite(context->cursor.smoothing_seconds) ||
       context->cursor.smoothing_seconds > 0.5f)) {
    memset(context, 0, sizeof(*context));
    return fail_with(error, error_size,
                     "Android cursor tuning is outside its contract");
  }
  context->initialized = 1;
  return NXANDROID_ANDROID_OK;
}

void nxandroid_android_context_reset(nxandroid_android_context *context) {
  if (context != NULL)
    memset(context, 0, sizeof(*context));
}

static int context_ready(const nxandroid_android_context *context) {
  return context != NULL && context->initialized != 0 &&
         context->api_version == NXANDROID_ANDROID_INPUT_API_VERSION &&
         context->failed == 0;
}

static nxandroid_android_result begin_timestamp(
    nxandroid_android_context *context, uint64_t timestamp_ns) {
  if (!context_ready(context))
    return context != NULL && context->failed
               ? NXANDROID_ANDROID_ESTATE
               : NXANDROID_ANDROID_EINVAL;
  if (timestamp_ns == 0u || timestamp_ns < context->last_timestamp_ns)
    return NXANDROID_ANDROID_EINVAL;
  context->current_timestamp_ns = timestamp_ns;
  context->last_timestamp_ns = timestamp_ns;
  return NXANDROID_ANDROID_OK;
}

static int pad_index(const nxandroid_android_context *context,
                     int32_t instance_id) {
  size_t index;
  if (context == NULL)
    return -1;
  for (index = 0u; index < NXANDROID_ANDROID_MAX_PADS; ++index) {
    if (context->pads[index].connected &&
        context->pads[index].instance_id == instance_id)
      return (int)index;
  }
  return -1;
}

static nxandroid_android_result emit_event(nxandroid_android_context *context,
                                           nxandroid_android_event *event) {
  nxandroid_android_ack ack;
  if (!context_ready(context))
    return NXANDROID_ANDROID_ESTATE;
  if (context->next_sequence == UINT64_MAX) {
    context->failed = 1;
    return NXANDROID_ANDROID_ESTATE;
  }
  event->api_version = NXANDROID_ANDROID_INPUT_API_VERSION;
  event->context =
      (nxandroid_android_context_kind)context->active_context;
  event->sequence = ++context->next_sequence;
  event->timestamp_ns = context->current_timestamp_ns;
  if (event->source == 0)
    event->source = NXANDROID_ANDROID_SOURCE_LIFECYCLE;
  memset(&ack, 0, sizeof(ack));
  if (context->event(context->userdata, event, &ack) != 0 ||
      ack.api_version != NXANDROID_ANDROID_INPUT_API_VERSION ||
      ack.sequence != event->sequence ||
      (ack.handled != 0 && ack.handled != 1)) {
    context->failed = 1;
    return NXANDROID_ANDROID_ECALLBACK;
  }
  context->delivered_events++;
  context->last_ack_sequence = ack.sequence;
  context->last_handled = ack.handled;
  context->last_return_value = ack.return_value;
  return NXANDROID_ANDROID_OK;
}

static nxandroid_android_result emit_lifecycle(
    nxandroid_android_context *context, nxandroid_android_pad_state *pad,
    nxandroid_android_event_kind kind) {
  nxandroid_android_event event;
  memset(&event, 0, sizeof(event));
  event.kind = kind;
  event.control = NXANDROID_ANDROID_A;
  if (pad != NULL) {
    event.source = (nxandroid_android_source_kind)pad->source;
    event.generation = pad->generation;
    event.instance_id = pad->instance_id;
    event.device_id = pad->device_id;
  } else
    event.source = NXANDROID_ANDROID_SOURCE_LIFECYCLE;
  return emit_event(context, &event);
}

nxandroid_android_result nxandroid_android_pad_connect(
    nxandroid_android_context *context, int32_t instance_id, int32_t device_id,
    const char *guid, nxandroid_android_source_kind source,
    uint64_t timestamp_ns) {
  size_t index;
  nxandroid_android_pad_state *pad = NULL;
  if (!context_ready(context) || instance_id < 0 || !source_valid((int)source) ||
      !bounded_string(guid, NXANDROID_ANDROID_GUID_MAX))
    return context != NULL && context->failed
               ? NXANDROID_ANDROID_ESTATE
               : NXANDROID_ANDROID_EINVAL;
  if (pad_index(context, instance_id) >= 0)
    return NXANDROID_ANDROID_EDUPLICATE;
  if (begin_timestamp(context, timestamp_ns) != NXANDROID_ANDROID_OK)
    return NXANDROID_ANDROID_EINVAL;
  for (index = 0u; index < NXANDROID_ANDROID_MAX_PADS; ++index) {
    if (!context->pads[index].connected) {
      pad = &context->pads[index];
      break;
    }
  }
  if (pad == NULL)
    return NXANDROID_ANDROID_EFULL;
  memset(pad, 0, sizeof(*pad));
  pad->connected = 1;
  pad->instance_id = instance_id;
  pad->device_id = device_id;
  pad->source = (uint8_t)source;
  if (context->next_generation == UINT32_MAX) {
    memset(pad, 0, sizeof(*pad));
    context->failed = 1;
    return NXANDROID_ANDROID_ESTATE;
  }
  context->next_generation++;
  pad->generation = context->next_generation;
  memcpy(pad->guid, guid, strlen(guid) + 1u);
  pad->cursor_x = context->cursor.initial_x;
  pad->cursor_y = context->cursor.initial_y;
  return emit_lifecycle(context, pad, NXANDROID_ANDROID_EVENT_PAD_ADDED);
}

static nxandroid_android_decision current_decision(
    const nxandroid_android_context *context, int control,
    const char **action) {
  int ctx = context->active_context;
  if (action != NULL)
    *action = NULL;
  if (!context_valid(ctx) || !control_valid(control))
    return NXANDROID_ANDROID_DECIDE_NONE;
  if (context->authority.decision[ctx][control] ==
      NXANDROID_ANDROID_DECIDE_ACTION) {
    if (action != NULL)
      *action = context->authority.action[ctx][control];
  }
  return (nxandroid_android_decision)context->authority.decision[ctx][control];
}

static nxandroid_android_result pull_update(nxandroid_android_context *context,
                                            nxandroid_android_pad_state *pad,
                                            int control, int pressed,
                                            float value, float x, float y) {
  pad->value[control] = value;
  pad->vector_x[control] = x;
  pad->vector_y[control] = y;
  if (is_trigger(control))
    pad->trigger_down[control] = (uint8_t)(pressed != 0);
  else if (!is_stick(control))
    pad->button_down[control] = (uint8_t)(pressed != 0);
  if (context->next_sequence == UINT64_MAX) {
    context->failed = 1;
    return NXANDROID_ANDROID_ESTATE;
  }
  pad->pull_sequence[control] = ++context->next_sequence;
  pad->pull_timestamp_ns[control] = context->current_timestamp_ns;
  context->pull_writes++;
  return NXANDROID_ANDROID_OK;
}

static nxandroid_android_result resolve_touch(
    const nxandroid_android_context *context,
    const nxandroid_android_pad_state *pad,
    const nxandroid_android_stored_route *route, int *pixel_x, int *pixel_y) {
  float x = route->norm_x;
  float y = route->norm_y;
  if (route->signal == NXANDROID_ANDROID_SIGNAL_TOUCH_CURSOR) {
    x = pad->cursor_x;
    y = pad->cursor_y;
  }
  return nxandroid_android_touch_resolve(&context->touch, x, y, pixel_x,
                                         pixel_y, NULL, 0u);
}

static nxandroid_android_result dispatch_action_button(
    nxandroid_android_context *context, nxandroid_android_pad_state *pad,
    int slot, int control, int route_slot, int pressed, float value) {
  nxandroid_android_stored_route *route = &context->routes[route_slot];
  nxandroid_android_event event;
  uint8_t *holds = &pad->route_hold_count[route_slot];

  if (pressed) {
    if (*holds == 255u)
      return NXANDROID_ANDROID_ESTATE;
    (*holds)++;
    if (*holds != 1u)
      return NXANDROID_ANDROID_OK;
  } else {
    if (*holds == 0u)
      return NXANDROID_ANDROID_OK;
    (*holds)--;
    if (*holds != 0u)
      return NXANDROID_ANDROID_OK;
  }
  if (route->sink == NXANDROID_ANDROID_SINK_JNI_PULL)
    return pull_update(context, pad, control, pressed, value, 0.0f, 0.0f);

  memset(&event, 0, sizeof(event));
  event.sink = (nxandroid_android_sink_kind)route->sink;
  event.signal = (nxandroid_android_signal_kind)route->signal;
  event.control = (nxandroid_android_control)control;
  event.source = (nxandroid_android_source_kind)pad->source;
  event.generation = pad->generation;
  event.instance_id = pad->instance_id;
  event.device_id = pad->device_id;
  event.code_x = route->code_x;
  event.code_y = route->code_y;
  event.pressed = pressed != 0;
  event.value = value;
  event.action = route->action;
  if (route->signal == NXANDROID_ANDROID_SIGNAL_TOUCH_FIXED ||
      route->signal == NXANDROID_ANDROID_SIGNAL_TOUCH_CURSOR) {
    if (resolve_touch(context, pad, route, &event.pixel_x, &event.pixel_y) !=
        NXANDROID_ANDROID_OK)
      return NXANDROID_ANDROID_EINVAL;
    event.pointer_id = slot * (int)NXANDROID_ANDROID_MAX_ROUTES + route_slot + 1;
    event.x = route->signal == NXANDROID_ANDROID_SIGNAL_TOUCH_CURSOR
                  ? pad->cursor_x
                  : clamp_unit(route->norm_x);
    event.y = route->signal == NXANDROID_ANDROID_SIGNAL_TOUCH_CURSOR
                  ? pad->cursor_y
                  : clamp_unit(route->norm_y);
    event.kind = pressed ? NXANDROID_ANDROID_EVENT_TOUCH_DOWN
                         : NXANDROID_ANDROID_EVENT_TOUCH_UP;
  } else {
    event.kind = NXANDROID_ANDROID_EVENT_BUTTON;
  }
  return emit_event(context, &event);
}

static nxandroid_android_result dispatch_native_button(
    nxandroid_android_context *context, nxandroid_android_pad_state *pad,
    int control, int native_slot, int pressed, float value) {
  const nxandroid_android_native_route *route =
      &context->native_routes[native_slot];
  nxandroid_android_event event;
  if (route->sink == NXANDROID_ANDROID_SINK_JNI_PULL)
    return pull_update(context, pad, control, pressed, value, 0.0f, 0.0f);
  memset(&event, 0, sizeof(event));
  event.kind = NXANDROID_ANDROID_EVENT_BUTTON;
  event.sink = route->sink;
  event.signal = route->signal;
  event.control = (nxandroid_android_control)control;
  event.source = (nxandroid_android_source_kind)pad->source;
  event.generation = pad->generation;
  event.instance_id = pad->instance_id;
  event.device_id = pad->device_id;
  event.code_x = route->code_x;
  event.code_y = route->code_y;
  event.pressed = pressed != 0;
  event.value = value;
  return emit_event(context, &event);
}

static nxandroid_android_result dispatch_button(
    nxandroid_android_context *context, nxandroid_android_pad_state *pad,
    int slot, int control, int pressed, float value) {
  const char *action = NULL;
  nxandroid_android_decision decision =
      current_decision(context, control, &action);
  if (decision == NXANDROID_ANDROID_DECIDE_NONE)
    return NXANDROID_ANDROID_OK;
  if (decision == NXANDROID_ANDROID_DECIDE_SUPPRESS) {
    context->suppressed_inputs++;
    return NXANDROID_ANDROID_OK;
  }
  if (decision == NXANDROID_ANDROID_DECIDE_ACTION) {
    int route_slot = route_index(context, action);
    if (route_slot < 0)
      return NXANDROID_ANDROID_ESTATE;
    return dispatch_action_button(context, pad, slot, control, route_slot,
                                  pressed, value);
  }
  if (decision == NXANDROID_ANDROID_DECIDE_NATIVE) {
    int native_slot = native_index(context, control);
    if (native_slot < 0)
      return NXANDROID_ANDROID_ESTATE;
    context->native_inputs++;
    return dispatch_native_button(context, pad, control, native_slot, pressed,
                                  value);
  }
  return NXANDROID_ANDROID_ESTATE;
}

nxandroid_android_result nxandroid_android_button(
    nxandroid_android_context *context, int32_t instance_id,
    nxandroid_android_control control, int pressed, uint64_t timestamp_ns) {
  int slot;
  nxandroid_android_pad_state *pad;
  int normalized = pressed != 0;
  if (!context_ready(context) || !control_valid((int)control) ||
      is_trigger((int)control) || is_stick((int)control))
    return context != NULL && context->failed
               ? NXANDROID_ANDROID_ESTATE
               : NXANDROID_ANDROID_EINVAL;
  slot = pad_index(context, instance_id);
  if (slot < 0)
    return NXANDROID_ANDROID_ENOTFOUND;
  if (begin_timestamp(context, timestamp_ns) != NXANDROID_ANDROID_OK)
    return NXANDROID_ANDROID_EINVAL;
  pad = &context->pads[slot];
  if (!context->focused || !context->resumed) {
    pad->button_down[control] = 0u;
    return NXANDROID_ANDROID_OK;
  }
  if ((int)pad->button_down[control] == normalized)
    return NXANDROID_ANDROID_OK;
  pad->button_down[control] = (uint8_t)normalized;
  pad->value[control] = normalized ? 1.0f : 0.0f;
  return dispatch_button(context, pad, slot, control, normalized,
                         normalized ? 1.0f : 0.0f);
}

static nxandroid_android_result dispatch_axis_route(
    nxandroid_android_context *context, nxandroid_android_pad_state *pad,
    int slot, int control, int route_slot, float value, int pressed,
    int edge) {
  nxandroid_android_stored_route *route = &context->routes[route_slot];
  nxandroid_android_event event;
  if (route->signal == NXANDROID_ANDROID_SIGNAL_BUTTON ||
      route->signal == NXANDROID_ANDROID_SIGNAL_TOUCH_FIXED ||
      route->signal == NXANDROID_ANDROID_SIGNAL_TOUCH_CURSOR) {
    if (!edge)
      return NXANDROID_ANDROID_OK;
    return dispatch_action_button(context, pad, slot, control, route_slot,
                                  pressed, value);
  }
  if (route->signal != NXANDROID_ANDROID_SIGNAL_AXIS)
    return NXANDROID_ANDROID_ESTATE;
  if (route->sink == NXANDROID_ANDROID_SINK_JNI_PULL)
    return pull_update(context, pad, control, pressed, value, 0.0f, 0.0f);
  memset(&event, 0, sizeof(event));
  event.kind = NXANDROID_ANDROID_EVENT_AXIS;
  event.sink = (nxandroid_android_sink_kind)route->sink;
  event.signal = NXANDROID_ANDROID_SIGNAL_AXIS;
  event.control = (nxandroid_android_control)control;
  event.source = (nxandroid_android_source_kind)pad->source;
  event.generation = pad->generation;
  event.instance_id = pad->instance_id;
  event.device_id = pad->device_id;
  event.code_x = route->code_x;
  event.code_y = route->code_y;
  event.pressed = pressed;
  event.value = value;
  event.action = route->action;
  return emit_event(context, &event);
}

static nxandroid_android_result dispatch_native_axis(
    nxandroid_android_context *context, nxandroid_android_pad_state *pad,
    int control, int native_slot, float value, int pressed, int edge) {
  const nxandroid_android_native_route *route =
      &context->native_routes[native_slot];
  nxandroid_android_event event;
  if (route->signal == NXANDROID_ANDROID_SIGNAL_BUTTON) {
    if (!edge)
      return NXANDROID_ANDROID_OK;
    return dispatch_native_button(context, pad, control, native_slot, pressed,
                                  value);
  }
  if (route->signal != NXANDROID_ANDROID_SIGNAL_AXIS)
    return NXANDROID_ANDROID_ESTATE;
  if (route->sink == NXANDROID_ANDROID_SINK_JNI_PULL)
    return pull_update(context, pad, control, pressed, value, 0.0f, 0.0f);
  memset(&event, 0, sizeof(event));
  event.kind = NXANDROID_ANDROID_EVENT_AXIS;
  event.sink = route->sink;
  event.signal = route->signal;
  event.control = (nxandroid_android_control)control;
  event.source = (nxandroid_android_source_kind)pad->source;
  event.generation = pad->generation;
  event.instance_id = pad->instance_id;
  event.device_id = pad->device_id;
  event.code_x = route->code_x;
  event.code_y = route->code_y;
  event.pressed = pressed;
  event.value = value;
  return emit_event(context, &event);
}

nxandroid_android_result nxandroid_android_axis(
    nxandroid_android_context *context, int32_t instance_id,
    nxandroid_android_control control, float value, uint64_t timestamp_ns) {
  int slot;
  nxandroid_android_pad_state *pad;
  float previous;
  int was_pressed;
  int pressed;
  int edge;
  const char *action = NULL;
  nxandroid_android_decision decision;
  if (!context_ready(context) || !is_trigger((int)control) || !isfinite(value))
    return context != NULL && context->failed
               ? NXANDROID_ANDROID_ESTATE
               : NXANDROID_ANDROID_EINVAL;
  slot = pad_index(context, instance_id);
  if (slot < 0)
    return NXANDROID_ANDROID_ENOTFOUND;
  if (begin_timestamp(context, timestamp_ns) != NXANDROID_ANDROID_OK)
    return NXANDROID_ANDROID_EINVAL;
  pad = &context->pads[slot];
  if (!context->focused || !context->resumed) {
    pad->value[control] = 0.0f;
    pad->trigger_down[control] = 0u;
    return NXANDROID_ANDROID_OK;
  }
  value = clamp_unit(value);
  previous = pad->value[control];
  was_pressed = pad->trigger_down[control] != 0u;
  pressed = was_pressed;
  if (!was_pressed && value >= NXANDROID_TRIGGER_ENTER)
    pressed = 1;
  else if (was_pressed && value <= NXANDROID_TRIGGER_EXIT)
    pressed = 0;
  edge = pressed != was_pressed;
  if (!edge && fabsf(previous - value) <= NXANDROID_FLOAT_EPSILON)
    return NXANDROID_ANDROID_OK;
  pad->value[control] = value;
  pad->trigger_down[control] = (uint8_t)pressed;
  decision = current_decision(context, control, &action);
  if (decision == NXANDROID_ANDROID_DECIDE_NONE)
    return NXANDROID_ANDROID_OK;
  if (decision == NXANDROID_ANDROID_DECIDE_SUPPRESS) {
    context->suppressed_inputs++;
    return NXANDROID_ANDROID_OK;
  }
  if (decision == NXANDROID_ANDROID_DECIDE_ACTION) {
    int route_slot = route_index(context, action);
    if (route_slot < 0)
      return NXANDROID_ANDROID_ESTATE;
    return dispatch_axis_route(context, pad, slot, control, route_slot, value,
                               pressed, edge);
  }
  if (decision == NXANDROID_ANDROID_DECIDE_NATIVE) {
    int native_slot = native_index(context, control);
    if (native_slot < 0)
      return NXANDROID_ANDROID_ESTATE;
    context->native_inputs++;
    return dispatch_native_axis(context, pad, control, native_slot, value,
                                pressed, edge);
  }
  return NXANDROID_ANDROID_ESTATE;
}

static nxandroid_android_result emit_vector(
    nxandroid_android_context *context, nxandroid_android_pad_state *pad,
    int control, nxandroid_android_sink_kind sink,
    nxandroid_android_signal_kind signal, int32_t code_x, int32_t code_y,
    const char *action, float x, float y) {
  nxandroid_android_event event;
  if (sink == NXANDROID_ANDROID_SINK_JNI_PULL)
    return pull_update(context, pad, control,
                       fabsf(x) > NXANDROID_FLOAT_EPSILON ||
                           fabsf(y) > NXANDROID_FLOAT_EPSILON,
                       0.0f, x, y);
  memset(&event, 0, sizeof(event));
  event.kind = NXANDROID_ANDROID_EVENT_VECTOR;
  event.sink = sink;
  event.signal = signal;
  event.control = (nxandroid_android_control)control;
  event.source = (nxandroid_android_source_kind)pad->source;
  event.generation = pad->generation;
  event.instance_id = pad->instance_id;
  event.device_id = pad->device_id;
  event.code_x = code_x;
  event.code_y = code_y;
  event.pressed = fabsf(x) > NXANDROID_FLOAT_EPSILON ||
                  fabsf(y) > NXANDROID_FLOAT_EPSILON;
  event.x = x;
  event.y = y;
  event.action = action;
  return emit_event(context, &event);
}

static int active_cursor_touch_route(const nxandroid_android_context *context,
                                     const nxandroid_android_pad_state *pad) {
  size_t index;
  for (index = 0u; index < context->route_count; ++index) {
    if (pad->route_hold_count[index] != 0u &&
        context->routes[index].signal ==
            NXANDROID_ANDROID_SIGNAL_TOUCH_CURSOR)
      return (int)index;
  }
  return -1;
}

static nxandroid_android_result move_cursor(
    nxandroid_android_context *context, nxandroid_android_pad_state *pad,
    int slot, int control, int route_slot, float raw_x, float raw_y,
    float dt_seconds) {
  nxandroid_android_stored_route *route = &context->routes[route_slot];
  float magnitude = sqrtf(raw_x * raw_x + raw_y * raw_y);
  float target_x = 0.0f;
  float target_y = 0.0f;
  float alpha = 1.0f;
  float old_x = pad->cursor_x;
  float old_y = pad->cursor_y;
  float aspect_scale;
  nxandroid_android_event event;
  int touch_route;

  if (dt_seconds < 0.0f || dt_seconds > 0.25f || !isfinite(dt_seconds))
    return NXANDROID_ANDROID_EINVAL;
  if (magnitude > context->cursor.deadzone && magnitude > 0.0f) {
    float scaled = (magnitude - context->cursor.deadzone) /
                   (1.0f - context->cursor.deadzone);
    float shaped = powf(clamp_unit(scaled), context->cursor.response_curve);
    target_x = raw_x / magnitude * shaped;
    target_y = raw_y / magnitude * shaped;
  }
  if (context->cursor.smoothing_seconds > 0.0f && dt_seconds > 0.0f)
    alpha = dt_seconds / (context->cursor.smoothing_seconds + dt_seconds);
  pad->cursor_velocity_x +=
      (target_x - pad->cursor_velocity_x) * alpha;
  pad->cursor_velocity_y +=
      (target_y - pad->cursor_velocity_y) * alpha;
  aspect_scale = (float)context->touch.drawable_height /
                 (float)context->touch.drawable_width;
  pad->cursor_x = clamp_unit(
      pad->cursor_x + pad->cursor_velocity_x *
                          context->cursor.speed_screen_heights_per_second *
                          dt_seconds * aspect_scale);
  pad->cursor_y = clamp_unit(
      pad->cursor_y + pad->cursor_velocity_y *
                          context->cursor.speed_screen_heights_per_second *
                          dt_seconds);
  if (fabsf(old_x - pad->cursor_x) <= NXANDROID_FLOAT_EPSILON &&
      fabsf(old_y - pad->cursor_y) <= NXANDROID_FLOAT_EPSILON)
    return NXANDROID_ANDROID_OK;

  memset(&event, 0, sizeof(event));
  event.kind = NXANDROID_ANDROID_EVENT_CURSOR_MOVE;
  event.sink = (nxandroid_android_sink_kind)route->sink;
  event.signal = NXANDROID_ANDROID_SIGNAL_CURSOR;
  event.control = (nxandroid_android_control)control;
  event.source = (nxandroid_android_source_kind)pad->source;
  event.generation = pad->generation;
  event.instance_id = pad->instance_id;
  event.device_id = pad->device_id;
  event.code_x = route->code_x;
  event.code_y = route->code_y;
  event.x = pad->cursor_x;
  event.y = pad->cursor_y;
  event.action = route->action;
  if (nxandroid_android_touch_resolve(&context->touch, event.x, event.y,
                                      &event.pixel_x, &event.pixel_y, NULL,
                                      0u) != NXANDROID_ANDROID_OK)
    return NXANDROID_ANDROID_EINVAL;
  if (emit_event(context, &event) != NXANDROID_ANDROID_OK)
    return NXANDROID_ANDROID_ECALLBACK;

  touch_route = active_cursor_touch_route(context, pad);
  if (touch_route >= 0) {
    nxandroid_android_stored_route *touch = &context->routes[touch_route];
    event.kind = NXANDROID_ANDROID_EVENT_TOUCH_MOVE;
    event.signal = NXANDROID_ANDROID_SIGNAL_TOUCH_CURSOR;
    event.code_x = touch->code_x;
    event.code_y = touch->code_y;
    event.pointer_id = slot * (int)NXANDROID_ANDROID_MAX_ROUTES +
                       touch_route + 1;
    event.action = touch->action;
    return emit_event(context, &event);
  }
  return NXANDROID_ANDROID_OK;
}

nxandroid_android_result nxandroid_android_vector(
    nxandroid_android_context *context, int32_t instance_id,
    nxandroid_android_control control, float x, float y, float dt_seconds,
    uint64_t timestamp_ns) {
  int slot;
  nxandroid_android_pad_state *pad;
  const char *action = NULL;
  nxandroid_android_decision decision;
  if (!context_ready(context) || !is_stick((int)control) || !isfinite(x) ||
      !isfinite(y) || !isfinite(dt_seconds) || dt_seconds < 0.0f ||
      dt_seconds > 0.25f)
    return context != NULL && context->failed
               ? NXANDROID_ANDROID_ESTATE
               : NXANDROID_ANDROID_EINVAL;
  slot = pad_index(context, instance_id);
  if (slot < 0)
    return NXANDROID_ANDROID_ENOTFOUND;
  if (begin_timestamp(context, timestamp_ns) != NXANDROID_ANDROID_OK)
    return NXANDROID_ANDROID_EINVAL;
  pad = &context->pads[slot];
  if (!context->focused || !context->resumed) {
    pad->vector_x[control] = 0.0f;
    pad->vector_y[control] = 0.0f;
    return NXANDROID_ANDROID_OK;
  }
  x = clamp_signed(x);
  y = clamp_signed(y);
  if (fabsf(pad->vector_x[control] - x) <= NXANDROID_FLOAT_EPSILON &&
      fabsf(pad->vector_y[control] - y) <= NXANDROID_FLOAT_EPSILON &&
      dt_seconds == 0.0f)
    return NXANDROID_ANDROID_OK;
  pad->vector_x[control] = x;
  pad->vector_y[control] = y;
  decision = current_decision(context, control, &action);
  if (decision == NXANDROID_ANDROID_DECIDE_NONE)
    return NXANDROID_ANDROID_OK;
  if (decision == NXANDROID_ANDROID_DECIDE_SUPPRESS) {
    context->suppressed_inputs++;
    return NXANDROID_ANDROID_OK;
  }
  if (decision == NXANDROID_ANDROID_DECIDE_ACTION) {
    int route_slot = route_index(context, action);
    nxandroid_android_stored_route *route;
    if (route_slot < 0)
      return NXANDROID_ANDROID_ESTATE;
    route = &context->routes[route_slot];
    if (route->signal == NXANDROID_ANDROID_SIGNAL_CURSOR)
      return move_cursor(context, pad, slot, control, route_slot, x, y,
                         dt_seconds);
    return emit_vector(context, pad, control,
                       (nxandroid_android_sink_kind)route->sink,
                       (nxandroid_android_signal_kind)route->signal,
                       route->code_x, route->code_y, route->action, x, y);
  }
  if (decision == NXANDROID_ANDROID_DECIDE_NATIVE) {
    int native_slot = native_index(context, control);
    const nxandroid_android_native_route *route;
    if (native_slot < 0)
      return NXANDROID_ANDROID_ESTATE;
    route = &context->native_routes[native_slot];
    context->native_inputs++;
    return emit_vector(context, pad, control, route->sink, route->signal,
                       route->code_x, route->code_y, NULL, x, y);
  }
  return NXANDROID_ANDROID_ESTATE;
}

static nxandroid_android_result release_pad(nxandroid_android_context *context,
                                            int slot) {
  nxandroid_android_pad_state *pad = &context->pads[slot];
  int control;
  nxandroid_android_result first = NXANDROID_ANDROID_OK;
  for (control = 0; control < (int)NXANDROID_ANDROID_CONTROL_COUNT;
       ++control) {
    nxandroid_android_result result = NXANDROID_ANDROID_OK;
    if (is_trigger(control) &&
        (pad->trigger_down[control] || pad->value[control] != 0.0f)) {
      result = nxandroid_android_axis(
          context, pad->instance_id, (nxandroid_android_control)control,
          0.0f, context->current_timestamp_ns);
    } else if (is_stick(control) &&
               (pad->vector_x[control] != 0.0f ||
                pad->vector_y[control] != 0.0f)) {
      result = nxandroid_android_vector(context, pad->instance_id,
                                        (nxandroid_android_control)control,
                                        0.0f, 0.0f, 0.0f,
                                        context->current_timestamp_ns);
    } else if (!is_trigger(control) && !is_stick(control) &&
               pad->button_down[control]) {
      result = nxandroid_android_button(
          context, pad->instance_id, (nxandroid_android_control)control, 0,
          context->current_timestamp_ns);
    }
    if (first == NXANDROID_ANDROID_OK && result != NXANDROID_ANDROID_OK)
      first = result;
  }
  memset(pad->route_hold_count, 0, sizeof(pad->route_hold_count));
  memset(pad->button_down, 0, sizeof(pad->button_down));
  memset(pad->trigger_down, 0, sizeof(pad->trigger_down));
  memset(pad->value, 0, sizeof(pad->value));
  memset(pad->vector_x, 0, sizeof(pad->vector_x));
  memset(pad->vector_y, 0, sizeof(pad->vector_y));
  pad->cursor_velocity_x = 0.0f;
  pad->cursor_velocity_y = 0.0f;
  return first;
}

nxandroid_android_result nxandroid_android_pad_disconnect(
    nxandroid_android_context *context, int32_t instance_id,
    uint64_t timestamp_ns) {
  int slot;
  nxandroid_android_pad_state snapshot;
  nxandroid_android_result result;
  if (!context_ready(context))
    return context != NULL && context->failed
               ? NXANDROID_ANDROID_ESTATE
               : NXANDROID_ANDROID_EINVAL;
  slot = pad_index(context, instance_id);
  if (slot < 0)
    return NXANDROID_ANDROID_ENOTFOUND;
  if (begin_timestamp(context, timestamp_ns) != NXANDROID_ANDROID_OK)
    return NXANDROID_ANDROID_EINVAL;
  result = release_pad(context, slot);
  if (result != NXANDROID_ANDROID_OK)
    return result;
  snapshot = context->pads[slot];
  memset(&context->pads[slot], 0, sizeof(context->pads[slot]));
  return emit_lifecycle(context, &snapshot,
                        NXANDROID_ANDROID_EVENT_PAD_REMOVED);
}

nxandroid_android_result nxandroid_android_pad_cancel(
    nxandroid_android_context *context, int32_t instance_id,
    uint64_t timestamp_ns) {
  int slot;
  nxandroid_android_result result;
  if (!context_ready(context))
    return context != NULL && context->failed
               ? NXANDROID_ANDROID_ESTATE
               : NXANDROID_ANDROID_EINVAL;
  slot = pad_index(context, instance_id);
  if (slot < 0)
    return NXANDROID_ANDROID_ENOTFOUND;
  if (begin_timestamp(context, timestamp_ns) != NXANDROID_ANDROID_OK)
    return NXANDROID_ANDROID_EINVAL;
  result = release_pad(context, slot);
  if (result != NXANDROID_ANDROID_OK)
    return result;
  return emit_lifecycle(context, &context->pads[slot],
                        NXANDROID_ANDROID_EVENT_CANCELLED);
}

nxandroid_android_result nxandroid_android_set_context(
    nxandroid_android_context *context, nxandroid_android_context_kind kind,
    uint64_t timestamp_ns) {
  size_t slot;
  if (!context_ready(context) || !context_valid((int)kind) ||
      context->authority.context_present[kind] == 0u)
    return context != NULL && context->failed
               ? NXANDROID_ANDROID_ESTATE
               : NXANDROID_ANDROID_EINVAL;
  if (begin_timestamp(context, timestamp_ns) != NXANDROID_ANDROID_OK)
    return NXANDROID_ANDROID_EINVAL;
  if (context->active_context == (uint8_t)kind)
    return NXANDROID_ANDROID_OK;
  for (slot = 0u; slot < NXANDROID_ANDROID_MAX_PADS; ++slot) {
    if (context->pads[slot].connected) {
      nxandroid_android_result result = release_pad(context, (int)slot);
      if (result != NXANDROID_ANDROID_OK)
        return result;
    }
  }
  context->active_context = (uint8_t)kind;
  return NXANDROID_ANDROID_OK;
}

nxandroid_android_result nxandroid_android_set_focus(
    nxandroid_android_context *context, int focused, uint64_t timestamp_ns) {
  size_t slot;
  int normalized = focused != 0;
  if (!context_ready(context))
    return context != NULL && context->failed
               ? NXANDROID_ANDROID_ESTATE
               : NXANDROID_ANDROID_EINVAL;
  if (begin_timestamp(context, timestamp_ns) != NXANDROID_ANDROID_OK)
    return NXANDROID_ANDROID_EINVAL;
  if (context->focused == normalized)
    return NXANDROID_ANDROID_OK;
  if (!normalized) {
    for (slot = 0u; slot < NXANDROID_ANDROID_MAX_PADS; ++slot) {
      if (context->pads[slot].connected) {
        nxandroid_android_result result = release_pad(context, (int)slot);
        if (result != NXANDROID_ANDROID_OK)
          return result;
      }
    }
  }
  context->focused = normalized;
  return emit_lifecycle(context, NULL,
                        normalized ? NXANDROID_ANDROID_EVENT_FOCUS_GAINED
                                   : NXANDROID_ANDROID_EVENT_FOCUS_LOST);
}

nxandroid_android_result nxandroid_android_set_resumed(
    nxandroid_android_context *context, int resumed,
    uint64_t timestamp_ns) {
  size_t slot;
  int normalized = resumed != 0;
  if (!context_ready(context))
    return context != NULL && context->failed
               ? NXANDROID_ANDROID_ESTATE
               : NXANDROID_ANDROID_EINVAL;
  if (begin_timestamp(context, timestamp_ns) != NXANDROID_ANDROID_OK)
    return NXANDROID_ANDROID_EINVAL;
  if (context->resumed == normalized)
    return NXANDROID_ANDROID_OK;
  if (!normalized) {
    for (slot = 0u; slot < NXANDROID_ANDROID_MAX_PADS; ++slot) {
      if (context->pads[slot].connected) {
        nxandroid_android_result result = release_pad(context, (int)slot);
        if (result != NXANDROID_ANDROID_OK)
          return result;
      }
    }
  }
  context->resumed = normalized;
  return emit_lifecycle(context, NULL,
                        normalized ? NXANDROID_ANDROID_EVENT_RESUMED
                                   : NXANDROID_ANDROID_EVENT_PAUSED);
}

nxandroid_android_read_policy nxandroid_android_control_read_policy(
    const nxandroid_android_context *context,
    nxandroid_android_context_kind kind, nxandroid_android_control control) {
  int decision;
  if (context == NULL || !context->initialized || !context_valid((int)kind) ||
      !control_valid((int)control))
    return NXANDROID_ANDROID_READ_NONE;
  decision = context->authority.decision[kind][control];
  switch (decision) {
  case NXANDROID_ANDROID_DECIDE_ACTION:
    return NXANDROID_ANDROID_READ_SYNTHESIZED;
  case NXANDROID_ANDROID_DECIDE_SUPPRESS:
    return NXANDROID_ANDROID_READ_SUPPRESSED;
  case NXANDROID_ANDROID_DECIDE_NATIVE:
    return NXANDROID_ANDROID_READ_NATIVE;
  default:
    return NXANDROID_ANDROID_READ_NONE;
  }
}

nxandroid_android_result nxandroid_android_pull(
    nxandroid_android_context *context, int32_t instance_id,
    nxandroid_android_control control, uint64_t timestamp_ns,
    nxandroid_android_pull_state *out) {
  int slot;
  const char *action = NULL;
  nxandroid_android_decision decision;
  int selected = -1;
  if (!context_ready(context) || out == NULL || !control_valid((int)control))
    return context != NULL && context->failed
               ? NXANDROID_ANDROID_ESTATE
               : NXANDROID_ANDROID_EINVAL;
  memset(out, 0, sizeof(*out));
  slot = pad_index(context, instance_id);
  if (slot < 0)
    return NXANDROID_ANDROID_ENOTFOUND;
  if (begin_timestamp(context, timestamp_ns) != NXANDROID_ANDROID_OK)
    return NXANDROID_ANDROID_EINVAL;
  decision = current_decision(context, control, &action);
  if (decision == NXANDROID_ANDROID_DECIDE_ACTION) {
    selected = route_index(context, action);
    if (selected < 0 || context->routes[selected].sink !=
                            NXANDROID_ANDROID_SINK_JNI_PULL)
      return NXANDROID_ANDROID_ENOTFOUND;
  } else if (decision == NXANDROID_ANDROID_DECIDE_NATIVE) {
    selected = native_index(context, control);
    if (selected < 0 || context->native_routes[selected].sink !=
                            NXANDROID_ANDROID_SINK_JNI_PULL)
      return NXANDROID_ANDROID_ENOTFOUND;
  } else {
    return NXANDROID_ANDROID_ENOTFOUND;
  }
  out->available = 1;
  out->pressed = is_trigger(control)
                     ? context->pads[slot].trigger_down[control] != 0u
                     : (!is_stick(control) &&
                        context->pads[slot].button_down[control] != 0u);
  out->value = context->pads[slot].value[control];
  out->x = context->pads[slot].vector_x[control];
  out->y = context->pads[slot].vector_y[control];
  out->source =
      (nxandroid_android_source_kind)context->pads[slot].source;
  out->context =
      (nxandroid_android_context_kind)context->active_context;
  out->instance_id = context->pads[slot].instance_id;
  out->device_id = context->pads[slot].device_id;
  out->generation = context->pads[slot].generation;
  out->update_sequence = context->pads[slot].pull_sequence[control];
  out->update_timestamp_ns =
      context->pads[slot].pull_timestamp_ns[control];
  if (context->next_sequence == UINT64_MAX) {
    context->failed = 1;
    return NXANDROID_ANDROID_ESTATE;
  }
  out->read_sequence = ++context->next_sequence;
  out->read_timestamp_ns = timestamp_ns;
  context->pull_reads++;
  context->last_pull_read_sequence = out->read_sequence;
  context->last_pull_read_timestamp_ns = timestamp_ns;
  return NXANDROID_ANDROID_OK;
}

nxandroid_android_result nxandroid_android_receipt(
    const nxandroid_android_context *context, char *output,
    size_t output_size) {
  int written;
  if (context == NULL || !context->initialized || output == NULL ||
      output_size == 0u)
    return NXANDROID_ANDROID_EINVAL;
  written = snprintf(
      output, output_size,
      "NXANDROID-C7 consumer=%s version=%s routes=%lu native_routes=%lu "
      "events=%llu suppressed=%llu native=%llu pull_reads=%llu "
      "last_sequence=%llu last_handled=%d last_return=%d "
      "last_pull_sequence=%llu last_pull_timestamp_ns=%llu "
      "identifiers=redacted status=%s",
      context->consumer_id, context->consumer_version,
      (unsigned long)context->route_count,
      (unsigned long)context->native_route_count,
      (unsigned long long)context->delivered_events,
      (unsigned long long)context->suppressed_inputs,
      (unsigned long long)context->native_inputs,
      (unsigned long long)context->pull_reads,
      (unsigned long long)context->last_ack_sequence,
      context->last_handled, context->last_return_value,
      (unsigned long long)context->last_pull_read_sequence,
      (unsigned long long)context->last_pull_read_timestamp_ns,
      context->failed ? "fail-stopped" : "ok");
  if (written < 0 || (size_t)written >= output_size) {
    output[0] = '\0';
    return NXANDROID_ANDROID_EINVAL;
  }
  return NXANDROID_ANDROID_OK;
}
