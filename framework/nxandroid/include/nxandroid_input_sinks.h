/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXANDROID_INPUT_SINKS_H
#define NXANDROID_INPUT_SINKS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Versioned input-sink interface for semantic controller actions.
 *
 * A port adapter maps NEXTOSCONTROLLERS.gptk semantic actions (resolved by
 * nxinput) onto one or more delivery backends. nxandroid only routes: it owns
 * no JavaVM, JNIEnv, jobject reference, Activity, Surface or input device.
 * Every Android/JNI interaction happens inside adapter-provided callbacks;
 * this module never includes a JNI header and never performs a JNI call.
 *
 * Multiple sinks per action are a supported, load-bearing feature (Action
 * Squad lesson: one logical "ui.confirm" must reach both the UI manager and
 * the gameplay channel from a single physical press). Delivery invokes every
 * sink registered for the action, in registration order, exactly once per
 * call.
 *
 * Double-read guard: the exclusive flag marks that synthesized delivery owns
 * the guest-visible input. The SDL virtual-pad path in the adapter must check
 * it so the guest never reads the physical device AND receives synthesized
 * events for the same press. The flag is only transported here; enforcement
 * lives in the adapter, which owns both event paths.
 *
 * This is a new, independent ABI counter, additive to the existing nxandroid
 * contract. It does not alter NXANDROID_API_VERSION. */
#define NXANDROID_INPUT_SINK_API_VERSION 1u

/* Storage sizes include the NUL terminator. */
#define NXANDROID_INPUT_SINK_ACTION_MAX 65u
#define NXANDROID_INPUT_SINK_DESCRIPTION_MAX 64u
#define NXANDROID_INPUT_SINK_MAX_ENTRIES 64u

typedef enum nxandroid_input_result {
  NXANDROID_INPUT_OK = 0,
  NXANDROID_INPUT_EINVAL = -1,
  NXANDROID_INPUT_EFULL = -2,
  NXANDROID_INPUT_EDUPLICATE = -3
} nxandroid_input_result;

const char *nxandroid_input_result_string(nxandroid_input_result result);

typedef enum nxandroid_input_sink_kind {
  /* The adapter delivers an Android KeyEvent/AKeyEvent equivalent. */
  NXANDROID_SINK_ANDROID_KEY = 1,
  /* The adapter delivers an Android MotionEvent/AMotionEvent equivalent. */
  NXANDROID_SINK_ANDROID_MOTION,
  /* Adapter-provided function that performs the JNI call itself. The
   * function, not this module, owns the JNIEnv discipline. */
  NXANDROID_SINK_JNI_CALLBACK,
  /* Engine-internal C hook (for example a Unity Rewired/InControl/Input
   * System adapter shim living in the port adapter). */
  NXANDROID_SINK_INTERNAL_API,
  /* The adapter computes coordinates from drawable/orientation/safe-area via
   * nxandroid_touch_resolve; the sink receives normalized 0..1 coordinates
   * plus the resolved pixel point. No fixed pixel coordinate ever comes from
   * a user-provided mapping file — callers pass normalized targets only. */
  NXANDROID_SINK_TOUCH
} nxandroid_input_sink_kind;

/* pressed is 1 on press and 0 on release; value carries the analog magnitude
 * (0.0/1.0 for digital actions). The action string is the registry-owned
 * copy and is valid only for the duration of the call. */
typedef void (*nxandroid_input_sink_fn)(void *userdata, const char *action,
                                        int pressed, float value);

typedef struct nxandroid_input_sink_entry {
  char action[NXANDROID_INPUT_SINK_ACTION_MAX];
  nxandroid_input_sink_kind kind;
  nxandroid_input_sink_fn callback;
  void *userdata;
  char description[NXANDROID_INPUT_SINK_DESCRIPTION_MAX];
} nxandroid_input_sink_entry;

/* Fixed-capacity, caller-owned storage. No dynamic allocation anywhere in
 * this module. Zero-initialization via nxandroid_input_sinks_init (or a
 * memset to zero) yields a valid empty registry. Not thread-safe; the
 * adapter serializes access exactly as it serializes its input pump. */
typedef struct nxandroid_input_sink_registry {
  nxandroid_input_sink_entry entries[NXANDROID_INPUT_SINK_MAX_ENTRIES];
  size_t entry_count;
  int exclusive;
} nxandroid_input_sink_registry;

void nxandroid_input_sinks_init(nxandroid_input_sink_registry *registry);

/* description may be NULL (stored empty) and is truncated to fit. Rejects a
 * NULL/empty/oversized action, an unknown kind or a NULL callback with
 * EINVAL; a full registry with EFULL; and the same action+kind+callback
 * registered twice with EDUPLICATE (userdata intentionally does not
 * disambiguate — two registrations differing only in userdata are one
 * logical sink registered twice). */
nxandroid_input_result nxandroid_input_sinks_register(
    nxandroid_input_sink_registry *registry, const char *action,
    nxandroid_input_sink_kind kind, nxandroid_input_sink_fn callback,
    void *userdata, const char *description);

/* Invokes every sink registered for the action, in registration order,
 * exactly once per call. Returns the number of sinks invoked; 0 means the
 * action is unmapped and the caller logs it. Returns NXANDROID_INPUT_EINVAL
 * (negative) on a NULL registry or NULL/empty action. Callbacks must not
 * mutate the registry during delivery. */
int nxandroid_input_sinks_deliver(nxandroid_input_sink_registry *registry,
                                  const char *action, int pressed,
                                  float value);

/* Double-read guard flag (see the header comment). set_exclusive stores any
 * non-zero value as 1; exclusive() returns 0 for a NULL registry. */
nxandroid_input_result nxandroid_input_sinks_set_exclusive(
    nxandroid_input_sink_registry *registry, int exclusive);
int nxandroid_input_sinks_exclusive(
    const nxandroid_input_sink_registry *registry);

/* width/height describe the drawable in pixels. rotation_degrees is the
 * guest-visible rotation (0, 90, 180 or 270). safe_* are non-negative pixel
 * insets applied in the drawable's own frame (after rotation). */
typedef struct nxandroid_touch_geometry {
  int width;
  int height;
  int rotation_degrees;
  int safe_left;
  int safe_top;
  int safe_right;
  int safe_bottom;
} nxandroid_touch_geometry;

/* Maps a normalized (0..1, top-left origin, pre-rotation) action target into
 * the rotated, safe-area-inset drawable and returns the pixel point. Inputs
 * outside 0..1 are clamped. Fails closed (nonzero result plus a message in
 * error when provided) on a NULL argument, zero/negative dimensions, a
 * negative inset, a safe area that consumes the whole drawable, or a
 * rotation outside {0, 90, 180, 270}. On failure the outputs are untouched.
 * Results always land inside the safe rectangle. */
nxandroid_input_result nxandroid_touch_resolve(
    const nxandroid_touch_geometry *geometry, float norm_x, float norm_y,
    int *out_px, int *out_py, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
