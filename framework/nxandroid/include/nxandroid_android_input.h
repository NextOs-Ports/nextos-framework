/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXANDROID_ANDROID_INPUT_H
#define NXANDROID_ANDROID_INPUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Strict Android/JNI/touch consumer boundary for V4-CONTROLLERS-03 / C7.
 *
 * The adapter resolves GPTK V2 exactly once (the optional
 * nxandroid_android_gptk.h bridge does that) and gives this module the frozen
 * authority snapshot. nxandroid then enforces one route per semantic action:
 * ACTION reaches exactly one declared sink, SUPPRESS reaches none, and NATIVE
 * reaches exactly one per-control native route. It never guesses an Android
 * keycode, axis, Java class, JNI symbol or touch coordinate.
 *
 * JavaVM/JNIEnv, jobject lifetimes, JNI exceptions, Android objects and the
 * actual callback invocation remain adapter-owned. The structured callback is
 * the narrow boundary at which a consumer receipt may be advanced. */
#define NXANDROID_ANDROID_INPUT_API_VERSION 1u

#define NXANDROID_ANDROID_CONTROL_COUNT 18u
#define NXANDROID_ANDROID_CONTEXT_COUNT 3u
#define NXANDROID_ANDROID_MAX_ROUTES 64u
#define NXANDROID_ANDROID_MAX_NATIVE_ROUTES 18u
#define NXANDROID_ANDROID_MAX_PADS 4u
#define NXANDROID_ANDROID_ACTION_MAX 65u
#define NXANDROID_ANDROID_ID_MAX 65u
#define NXANDROID_ANDROID_GUID_MAX 65u

typedef enum nxandroid_android_result {
  NXANDROID_ANDROID_OK = 0,
  NXANDROID_ANDROID_EINVAL = -1,
  NXANDROID_ANDROID_EDUPLICATE = -2,
  NXANDROID_ANDROID_ENOTFOUND = -3,
  NXANDROID_ANDROID_EFULL = -4,
  NXANDROID_ANDROID_ESTATE = -5,
  NXANDROID_ANDROID_ECALLBACK = -6
} nxandroid_android_result;

const char *nxandroid_android_result_string(nxandroid_android_result result);

/* Ordinals intentionally mirror nxinput_gptk_control. The bridge contains
 * compile-time assertions, so either component changing independently fails
 * at the integration boundary rather than silently remapping a control. */
typedef enum nxandroid_android_control {
  NXANDROID_ANDROID_A = 0,
  NXANDROID_ANDROID_B,
  NXANDROID_ANDROID_X,
  NXANDROID_ANDROID_Y,
  NXANDROID_ANDROID_L1,
  NXANDROID_ANDROID_R1,
  NXANDROID_ANDROID_L2,
  NXANDROID_ANDROID_R2,
  NXANDROID_ANDROID_L3,
  NXANDROID_ANDROID_R3,
  NXANDROID_ANDROID_START,
  NXANDROID_ANDROID_SELECT,
  NXANDROID_ANDROID_UP,
  NXANDROID_ANDROID_DOWN,
  NXANDROID_ANDROID_LEFT,
  NXANDROID_ANDROID_RIGHT,
  NXANDROID_ANDROID_LEFT_STICK,
  NXANDROID_ANDROID_RIGHT_STICK
} nxandroid_android_control;

typedef enum nxandroid_android_context_kind {
  NXANDROID_ANDROID_MENU = 0,
  NXANDROID_ANDROID_GAMEPLAY,
  NXANDROID_ANDROID_CURSOR
} nxandroid_android_context_kind;

typedef enum nxandroid_android_decision {
  NXANDROID_ANDROID_DECIDE_NONE = 0,
  NXANDROID_ANDROID_DECIDE_ACTION,
  NXANDROID_ANDROID_DECIDE_SUPPRESS,
  NXANDROID_ANDROID_DECIDE_NATIVE
} nxandroid_android_decision;

typedef enum nxandroid_android_sink_kind {
  NXANDROID_ANDROID_SINK_KEY_EVENT = 1,
  NXANDROID_ANDROID_SINK_MOTION_EVENT,
  NXANDROID_ANDROID_SINK_JNI_PUSH,
  NXANDROID_ANDROID_SINK_JNI_PULL,
  NXANDROID_ANDROID_SINK_NATIVE_GAMEPAD,
  NXANDROID_ANDROID_SINK_TOUCH
} nxandroid_android_sink_kind;

typedef enum nxandroid_android_signal_kind {
  NXANDROID_ANDROID_SIGNAL_BUTTON = 1,
  NXANDROID_ANDROID_SIGNAL_AXIS,
  NXANDROID_ANDROID_SIGNAL_VECTOR,
  NXANDROID_ANDROID_SIGNAL_TOUCH_FIXED,
  NXANDROID_ANDROID_SIGNAL_TOUCH_CURSOR,
  NXANDROID_ANDROID_SIGNAL_CURSOR
} nxandroid_android_signal_kind;

typedef enum nxandroid_android_event_kind {
  NXANDROID_ANDROID_EVENT_BUTTON = 1,
  NXANDROID_ANDROID_EVENT_AXIS,
  NXANDROID_ANDROID_EVENT_VECTOR,
  NXANDROID_ANDROID_EVENT_TOUCH_DOWN,
  NXANDROID_ANDROID_EVENT_TOUCH_UP,
  NXANDROID_ANDROID_EVENT_TOUCH_MOVE,
  NXANDROID_ANDROID_EVENT_CURSOR_MOVE,
  NXANDROID_ANDROID_EVENT_PAD_ADDED,
  NXANDROID_ANDROID_EVENT_PAD_REMOVED,
  NXANDROID_ANDROID_EVENT_FOCUS_GAINED,
  NXANDROID_ANDROID_EVENT_FOCUS_LOST,
  NXANDROID_ANDROID_EVENT_RESUMED,
  NXANDROID_ANDROID_EVENT_PAUSED,
  NXANDROID_ANDROID_EVENT_CANCELLED
} nxandroid_android_event_kind;

typedef enum nxandroid_android_source_kind {
  NXANDROID_ANDROID_SOURCE_GET_CONTROLS = 1,
  NXANDROID_ANDROID_SOURCE_CFW_GUID_DB,
  NXANDROID_ANDROID_SOURCE_PORT_BUNDLE,
  NXANDROID_ANDROID_SOURCE_RUNTIME_BUILTIN,
  NXANDROID_ANDROID_SOURCE_RAW_DECLARED,
  NXANDROID_ANDROID_SOURCE_LIFECYCLE
} nxandroid_android_source_kind;

typedef enum nxandroid_android_read_policy {
  NXANDROID_ANDROID_READ_NONE = 0,
  NXANDROID_ANDROID_READ_SYNTHESIZED,
  NXANDROID_ANDROID_READ_SUPPRESSED,
  NXANDROID_ANDROID_READ_NATIVE
} nxandroid_android_read_policy;

/* Frozen result of the GPTK authority lookup. action is non-empty only for
 * DECIDE_ACTION. Callers normally populate this with the real nxinput bridge;
 * direct construction exists for hermetic consumers and tests. */
typedef struct nxandroid_android_authority {
  uint32_t api_version;
  uint32_t schema_version;
  uint8_t context_present[NXANDROID_ANDROID_CONTEXT_COUNT];
  uint8_t decision[NXANDROID_ANDROID_CONTEXT_COUNT]
                  [NXANDROID_ANDROID_CONTROL_COUNT];
  char action[NXANDROID_ANDROID_CONTEXT_COUNT]
             [NXANDROID_ANDROID_CONTROL_COUNT]
             [NXANDROID_ANDROID_ACTION_MAX];
} nxandroid_android_authority;

/* One semantic action, one sink, one signal shape. code_x/code_y are values
 * from the consumer's observed contract (for example AKEYCODE or AXIS_*), not
 * a framework-wide Android table. norm_x/norm_y are used only by TOUCH_FIXED;
 * TOUCH_CURSOR clicks the per-pad cursor maintained by a CURSOR route. */
typedef struct nxandroid_android_route {
  const char *action;
  nxandroid_android_sink_kind sink;
  nxandroid_android_signal_kind signal;
  int32_t code_x;
  int32_t code_y;
  float norm_x;
  float norm_y;
} nxandroid_android_route;

/* NATIVE is per physical control and has exactly one declared route. */
typedef struct nxandroid_android_native_route {
  nxandroid_android_control control;
  nxandroid_android_sink_kind sink;
  nxandroid_android_signal_kind signal;
  int32_t code_x;
  int32_t code_y;
} nxandroid_android_native_route;

/* Drawable and content rect are in final display coordinates. Safe insets are
 * relative to the drawable. The usable touch rectangle is their intersection;
 * normalized coordinates are rotated inside it. */
typedef struct nxandroid_android_touch_geometry {
  int drawable_width;
  int drawable_height;
  int content_left;
  int content_top;
  int content_width;
  int content_height;
  int safe_left;
  int safe_top;
  int safe_right;
  int safe_bottom;
  int rotation_degrees;
} nxandroid_android_touch_geometry;

typedef struct nxandroid_android_cursor_tuning {
  float initial_x;
  float initial_y;
  float speed_screen_heights_per_second;
  float deadzone;
  float response_curve;
  float smoothing_seconds;
} nxandroid_android_cursor_tuning;

typedef struct nxandroid_android_event {
  uint32_t api_version;
  nxandroid_android_event_kind kind;
  nxandroid_android_source_kind source;
  nxandroid_android_context_kind context;
  nxandroid_android_sink_kind sink;
  nxandroid_android_signal_kind signal;
  nxandroid_android_control control;
  uint32_t generation;
  uint64_t sequence;
  uint64_t timestamp_ns;
  int32_t instance_id;
  int32_t device_id;
  int32_t code_x;
  int32_t code_y;
  int32_t pointer_id;
  int pressed;
  float value;
  float x;
  float y;
  int pixel_x;
  int pixel_y;
  const char *action;
} nxandroid_android_event;

typedef struct nxandroid_android_ack {
  uint32_t api_version;
  uint64_t sequence;
  int handled;
  int32_t return_value;
} nxandroid_android_ack;

/* Return zero only after the real consumer call returned, and fill ack with
 * its handled/return result and the exact event sequence. A callback error or
 * malformed acknowledgement fail-stops the context. */
typedef int (*nxandroid_android_event_fn)(void *userdata,
                                         const nxandroid_android_event *event,
                                         nxandroid_android_ack *ack);

typedef struct nxandroid_android_profile {
  const char *consumer_id;
  const char *consumer_version;
  const nxandroid_android_route *routes;
  size_t route_count;
  const nxandroid_android_native_route *native_routes;
  size_t native_route_count;
  nxandroid_android_touch_geometry touch;
  nxandroid_android_cursor_tuning cursor;
  nxandroid_android_event_fn event;
  void *userdata;
} nxandroid_android_profile;

/* Public fixed storage keeps the API allocation-free. Applications must treat
 * every member below as private and initialize only through context_init. */
typedef struct nxandroid_android_stored_route {
  char action[NXANDROID_ANDROID_ACTION_MAX];
  uint8_t sink;
  uint8_t signal;
  int32_t code_x;
  int32_t code_y;
  float norm_x;
  float norm_y;
} nxandroid_android_stored_route;

typedef struct nxandroid_android_pad_state {
  int connected;
  int32_t instance_id;
  int32_t device_id;
  uint32_t generation;
  uint8_t source;
  char guid[NXANDROID_ANDROID_GUID_MAX];
  uint8_t button_down[NXANDROID_ANDROID_CONTROL_COUNT];
  uint8_t trigger_down[NXANDROID_ANDROID_CONTROL_COUNT];
  float value[NXANDROID_ANDROID_CONTROL_COUNT];
  float vector_x[NXANDROID_ANDROID_CONTROL_COUNT];
  float vector_y[NXANDROID_ANDROID_CONTROL_COUNT];
  float cursor_x;
  float cursor_y;
  float cursor_velocity_x;
  float cursor_velocity_y;
  uint8_t route_hold_count[NXANDROID_ANDROID_MAX_ROUTES];
  uint64_t pull_sequence[NXANDROID_ANDROID_CONTROL_COUNT];
  uint64_t pull_timestamp_ns[NXANDROID_ANDROID_CONTROL_COUNT];
} nxandroid_android_pad_state;

typedef struct nxandroid_android_context {
  uint32_t api_version;
  int initialized;
  int failed;
  int focused;
  int resumed;
  uint8_t active_context;
  char consumer_id[NXANDROID_ANDROID_ID_MAX];
  char consumer_version[NXANDROID_ANDROID_ID_MAX];
  nxandroid_android_authority authority;
  nxandroid_android_stored_route routes[NXANDROID_ANDROID_MAX_ROUTES];
  size_t route_count;
  nxandroid_android_native_route
      native_routes[NXANDROID_ANDROID_MAX_NATIVE_ROUTES];
  size_t native_route_count;
  nxandroid_android_touch_geometry touch;
  nxandroid_android_cursor_tuning cursor;
  nxandroid_android_event_fn event;
  void *userdata;
  nxandroid_android_pad_state pads[NXANDROID_ANDROID_MAX_PADS];
  uint32_t next_generation;
  uint64_t next_sequence;
  uint64_t current_timestamp_ns;
  uint64_t last_timestamp_ns;
  uint64_t delivered_events;
  uint64_t suppressed_inputs;
  uint64_t native_inputs;
  uint64_t pull_writes;
  uint64_t pull_reads;
  uint64_t last_ack_sequence;
  int last_handled;
  int32_t last_return_value;
  uint64_t last_pull_read_sequence;
  uint64_t last_pull_read_timestamp_ns;
} nxandroid_android_context;

typedef struct nxandroid_android_pull_state {
  int available;
  int pressed;
  float value;
  float x;
  float y;
  nxandroid_android_source_kind source;
  nxandroid_android_context_kind context;
  int32_t instance_id;
  int32_t device_id;
  uint32_t generation;
  uint64_t update_sequence;
  uint64_t update_timestamp_ns;
  uint64_t read_sequence;
  uint64_t read_timestamp_ns;
} nxandroid_android_pull_state;

nxandroid_android_result nxandroid_android_touch_resolve(
    const nxandroid_android_touch_geometry *geometry, float norm_x,
    float norm_y, int *out_x, int *out_y, char *error, size_t error_size);

nxandroid_android_result nxandroid_android_context_init(
    nxandroid_android_context *context,
    const nxandroid_android_authority *authority,
    const nxandroid_android_profile *profile, char *error,
    size_t error_size);

void nxandroid_android_context_reset(nxandroid_android_context *context);

nxandroid_android_result nxandroid_android_pad_connect(
    nxandroid_android_context *context, int32_t instance_id, int32_t device_id,
    const char *guid, nxandroid_android_source_kind source,
    uint64_t timestamp_ns);
nxandroid_android_result nxandroid_android_pad_disconnect(
    nxandroid_android_context *context, int32_t instance_id,
    uint64_t timestamp_ns);
nxandroid_android_result nxandroid_android_pad_cancel(
    nxandroid_android_context *context, int32_t instance_id,
    uint64_t timestamp_ns);

nxandroid_android_result nxandroid_android_set_context(
    nxandroid_android_context *context, nxandroid_android_context_kind kind,
    uint64_t timestamp_ns);
nxandroid_android_result nxandroid_android_set_focus(
    nxandroid_android_context *context, int focused, uint64_t timestamp_ns);
nxandroid_android_result nxandroid_android_set_resumed(
    nxandroid_android_context *context, int resumed, uint64_t timestamp_ns);

nxandroid_android_result nxandroid_android_button(
    nxandroid_android_context *context, int32_t instance_id,
    nxandroid_android_control control, int pressed, uint64_t timestamp_ns);
nxandroid_android_result nxandroid_android_axis(
    nxandroid_android_context *context, int32_t instance_id,
    nxandroid_android_control control, float value, uint64_t timestamp_ns);
nxandroid_android_result nxandroid_android_vector(
    nxandroid_android_context *context, int32_t instance_id,
    nxandroid_android_control control, float x, float y, float dt_seconds,
    uint64_t timestamp_ns);

nxandroid_android_read_policy nxandroid_android_control_read_policy(
    const nxandroid_android_context *context,
    nxandroid_android_context_kind kind, nxandroid_android_control control);

/* JNI pull never calls the push callback. It exposes only a control whose
 * selected ACTION or NATIVE route explicitly declared JNI_PULL. */
nxandroid_android_result nxandroid_android_pull(
    nxandroid_android_context *context, int32_t instance_id,
    nxandroid_android_control control, uint64_t timestamp_ns,
    nxandroid_android_pull_state *out);

/* Redacted, bounded consumer receipt. Device IDs and GUIDs are deliberately
 * excluded. Returns OK on success or EINVAL when the buffer is too small. */
nxandroid_android_result nxandroid_android_receipt(
    const nxandroid_android_context *context, char *output,
    size_t output_size);

const char *nxandroid_android_control_name(nxandroid_android_control control);
const char *nxandroid_android_sink_name(nxandroid_android_sink_kind sink);

#ifdef __cplusplus
}
#endif

#endif
