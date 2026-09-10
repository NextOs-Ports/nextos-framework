/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_GODOT_RUNTIME_H
#define NXINPUT_GODOT_RUNTIME_H

/* Reusable, dependency-free policy for a Godot adapter that turns physical
 * controls into InputEventAction calls.  Engine/game policy (scene names,
 * action names, co-op ownership and actual sinks) stays in the port adapter.
 * These helpers never claim semantic consumption: a callback ACK proves only
 * that the adapter enqueued the event; external, engine-backed evidence must
 * bind each semantic action to its real consumer before release. */

#include <stdbool.h>
#include <stdint.h>

#define NXINPUT_GODOT_RUNTIME_API_VERSION 1u
#define NXINPUT_GODOT_RUNTIME_MARKER "nxinput-godot-runtime/1"

typedef struct nxinput_godot_action_latch {
  uint32_t held_count;
} nxinput_godot_action_latch;

typedef enum nxinput_godot_action_effect {
  NXINPUT_GODOT_ACTION_INVALID = -1,
  NXINPUT_GODOT_ACTION_ACK_ONLY = 0,
  NXINPUT_GODOT_ACTION_DELIVER = 1
} nxinput_godot_action_effect;

static inline nxinput_godot_action_effect nxinput_godot_action_preview(
    const nxinput_godot_action_latch *latch, int pressed) {
  if (latch == 0)
    return NXINPUT_GODOT_ACTION_INVALID;
  if (pressed) {
    if (latch->held_count == UINT32_MAX)
      return NXINPUT_GODOT_ACTION_INVALID;
    return latch->held_count == 0u ? NXINPUT_GODOT_ACTION_DELIVER
                                  : NXINPUT_GODOT_ACTION_ACK_ONLY;
  }
  if (latch->held_count == 0u)
    return NXINPUT_GODOT_ACTION_INVALID;
  return latch->held_count == 1u ? NXINPUT_GODOT_ACTION_DELIVER
                                : NXINPUT_GODOT_ACTION_ACK_ONLY;
}

static inline int nxinput_godot_action_commit(
    nxinput_godot_action_latch *latch, int pressed) {
  if (nxinput_godot_action_preview(latch, pressed) ==
      NXINPUT_GODOT_ACTION_INVALID)
    return -1;
  if (pressed)
    latch->held_count++;
  else
    latch->held_count--;
  return 0;
}

/* Preserve raw component strengths. Godot/Input.GetVector remains the sole
 * owner of the game's configurable radial deadzone. */
static inline void nxinput_godot_split_vector(float x, float y,
                                               float strengths[4]) {
  if (strengths == 0)
    return;
  strengths[0] = y < 0.0f ? -y : 0.0f;
  strengths[1] = y > 0.0f ? y : 0.0f;
  strengths[2] = x < 0.0f ? -x : 0.0f;
  strengths[3] = x > 0.0f ? x : 0.0f;
}

#define NXINPUT_GODOT_VECTOR_ALIAS_SOURCES 2u

typedef struct nxinput_godot_vector_alias {
  float source[NXINPUT_GODOT_VECTOR_ALIAS_SOURCES][4];
} nxinput_godot_vector_alias;

/* Keep the two physical sticks independent when an owner maps both to one
 * semantic vector action. The sink sees the per-direction maximum, so a
 * neutral source cannot release or weaken the other held source. */
static inline int nxinput_godot_vector_alias_update(
    nxinput_godot_vector_alias *alias, unsigned int source, float x, float y,
    float aggregate[4]) {
  unsigned int direction;
  if (alias == 0 || aggregate == 0 ||
      source >= NXINPUT_GODOT_VECTOR_ALIAS_SOURCES)
    return -1;
  nxinput_godot_split_vector(x, y, alias->source[source]);
  for (direction = 0u; direction < 4u; direction++) {
    float first = alias->source[0][direction];
    float second = alias->source[1][direction];
    aggregate[direction] = first > second ? first : second;
  }
  return 0;
}

static inline void nxinput_godot_vector_alias_clear(
    nxinput_godot_vector_alias *alias) {
  unsigned int source;
  unsigned int direction;
  if (alias == 0)
    return;
  for (source = 0u; source < NXINPUT_GODOT_VECTOR_ALIAS_SOURCES; source++)
    for (direction = 0u; direction < 4u; direction++)
      alias->source[source][direction] = 0.0f;
}

typedef struct nxinput_godot_neutral_handoff {
  uint32_t controls;
} nxinput_godot_neutral_handoff;

/* Handoff only: raw vectors delivered to Godot are never shaped here. This
 * radial release floor absorbs ordinary SDL stick drift so an authority mask
 * cannot remain armed forever after the player lets go. */
#define NXINPUT_GODOT_HANDOFF_NEUTRAL 0.20f

static inline uint32_t nxinput_godot_control_bit(int control) {
  return control >= 0 && control < 32
             ? UINT32_C(1) << (unsigned int)control
             : 0u;
}

static inline int nxinput_godot_vector_neutral(float x, float y) {
  return x * x + y * y <=
         NXINPUT_GODOT_HANDOFF_NEUTRAL * NXINPUT_GODOT_HANDOFF_NEUTRAL;
}

static inline void nxinput_godot_handoff_snapshot(
    nxinput_godot_neutral_handoff *handoff, uint32_t down_controls,
    int left_control, float left_x, float left_y,
    int right_control, float right_x, float right_y) {
  if (handoff == 0)
    return;
  handoff->controls = down_controls;
  if (!nxinput_godot_vector_neutral(left_x, left_y))
    handoff->controls |= nxinput_godot_control_bit(left_control);
  if (!nxinput_godot_vector_neutral(right_x, right_y))
    handoff->controls |= nxinput_godot_control_bit(right_control);
}

/* Rebuild both sides of an authority transition from one physical snapshot.
 * A control already crossing an earlier boundary keeps that ownership until
 * release/neutral, even when a second scene transition happens first.  This
 * prevents a native press from receiving a swallowed release and prevents a
 * governed press from becoming a new action in the next context.  If a stale
 * caller ever presents overlap, native passthrough wins: releasing a native
 * press is the only choice that cannot leave the engine's physical latch
 * stuck. `governed_controls` identifies ACTION/SUPPRESS in the OLD proved
 * context; NONE/NATIVE and an unproved old context stay native. */
static inline void nxinput_godot_handoff_partition(
    nxinput_godot_neutral_handoff *native_handoff,
    nxinput_godot_neutral_handoff *suppressed_handoff,
    uint32_t down_controls, uint32_t governed_controls,
    int left_control, float left_x, float left_y,
    int right_control, float right_x, float right_y) {
  uint32_t active = down_controls;
  uint32_t old_native;
  uint32_t old_suppressed;
  uint32_t unowned;

  if (native_handoff == 0 || suppressed_handoff == 0 ||
      native_handoff == suppressed_handoff)
    return;
  if (!nxinput_godot_vector_neutral(left_x, left_y))
    active |= nxinput_godot_control_bit(left_control);
  if (!nxinput_godot_vector_neutral(right_x, right_y))
    active |= nxinput_godot_control_bit(right_control);

  old_native = native_handoff->controls & active;
  old_suppressed = suppressed_handoff->controls & active & ~old_native;
  unowned = active & ~(old_native | old_suppressed);
  native_handoff->controls = old_native | (unowned & ~governed_controls);
  suppressed_handoff->controls =
      old_suppressed | (unowned & governed_controls);
}

static inline int nxinput_godot_handoff_button(
    nxinput_godot_neutral_handoff *handoff, int control, int pressed) {
  uint32_t bit = nxinput_godot_control_bit(control);
  if (handoff == 0 || bit == 0u || (handoff->controls & bit) == 0u)
    return 0;
  if (!pressed)
    handoff->controls &= ~bit;
  return 1;
}

static inline int nxinput_godot_handoff_vector(
    nxinput_godot_neutral_handoff *handoff, int control, float x, float y) {
  uint32_t bit = nxinput_godot_control_bit(control);
  if (handoff == 0 || bit == 0u || (handoff->controls & bit) == 0u)
    return 0;
  if (nxinput_godot_vector_neutral(x, y))
    handoff->controls &= ~bit;
  return 1;
}

/* Fatal delivery/release is a lifecycle failure, never a clean mapped quit.
 * Consuming the close request does not clear `fatal`, so health remains
 * forbidden and the process exit status remains nonzero. */
typedef struct nxinput_godot_lifecycle {
  int fatal;
  int close_pending;
} nxinput_godot_lifecycle;

static inline void nxinput_godot_lifecycle_fail(
    nxinput_godot_lifecycle *lifecycle) {
  if (lifecycle != 0) {
    lifecycle->fatal = 1;
    lifecycle->close_pending = 1;
  }
}

static inline int nxinput_godot_lifecycle_consume_close(
    nxinput_godot_lifecycle *lifecycle) {
  int pending;
  if (lifecycle == 0)
    return 0;
  pending = lifecycle->close_pending;
  lifecycle->close_pending = 0;
  return pending;
}

static inline int nxinput_godot_lifecycle_health_allowed(
    const nxinput_godot_lifecycle *lifecycle) {
  return lifecycle != 0 && !lifecycle->fatal;
}

static inline int nxinput_godot_lifecycle_exit_status(
    const nxinput_godot_lifecycle *lifecycle) {
  return lifecycle != 0 && lifecycle->fatal ? 1 : 0;
}

static inline const char *nxinput_godot_runtime_marker(void) {
  return NXINPUT_GODOT_RUNTIME_MARKER;
}

#endif
