/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_observe -- see include/nxinput_observe.h. Pure observer: no SDL,
 * no environment, no devices, no I/O beyond the caller-provided sink, and no
 * return value a decision path could branch on. */
#include "nxinput_observe.h"

#include <stdio.h>
#include <string.h>

static const char *const control_names[NXINPUT_OBSERVE_CONTROL_COUNT] = {
    "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "L3", "R3",
    "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT",
    "LEFT_STICK", "RIGHT_STICK",
};

const char *nxinput_observe_control_name(nxinput_observe_control control) {
  if ((unsigned int)control >= NXINPUT_OBSERVE_CONTROL_COUNT) {
    return "invalid";
  }
  return control_names[control];
}

const char *nxinput_observe_source_name(nxinput_observe_source source) {
  switch (source) {
    case NXINPUT_OBSERVE_SOURCE_PORTMASTER_ENV:
      return "portmaster-env";
    case NXINPUT_OBSERVE_SOURCE_CFW_FILE:
      return "cfw-file";
    case NXINPUT_OBSERVE_SOURCE_PORT_BUNDLE:
      return "port-bundle";
    case NXINPUT_OBSERVE_SOURCE_SDL_BUILTIN:
    default:
      return "sdl-builtin";
  }
}

const char *nxinput_observe_semantic_name(nxinput_observe_semantic semantic) {
  switch (semantic) {
    case NXINPUT_OBSERVE_SEMANTIC_ACTION:
      return "action";
    case NXINPUT_OBSERVE_SEMANTIC_NULL:
      return "null";
    case NXINPUT_OBSERVE_SEMANTIC_NATIVE:
      return "native";
    case NXINPUT_OBSERVE_SEMANTIC_LEGACY_UNMANAGED:
    default:
      return "legacy-unmanaged";
  }
}

const char *nxinput_observe_delivery_name(nxinput_observe_delivery delivery) {
  switch (delivery) {
    case NXINPUT_OBSERVE_DELIVERED:
      return "delivered";
    case NXINPUT_OBSERVE_SUPPRESSED:
      return "suppressed";
    case NXINPUT_OBSERVE_PENDING_NOT_INSTRUMENTED:
    default:
      return "pending/not-instrumented";
  }
}

static const char *bind_kind_name(nxinput_observe_bind_kind kind) {
  switch (kind) {
    case NXINPUT_OBSERVE_BIND_BUTTON:
      return "button";
    case NXINPUT_OBSERVE_BIND_AXIS:
      return "axis";
    case NXINPUT_OBSERVE_BIND_HAT:
      return "hat";
    case NXINPUT_OBSERVE_BIND_NONE:
    default:
      return "none";
  }
}

static const char *trigger_kind_name(nxinput_observe_trigger_kind kind) {
  switch (kind) {
    case NXINPUT_OBSERVE_TRIGGER_AXIS:
      return "axis";
    case NXINPUT_OBSERVE_TRIGGER_BUTTON:
      return "button";
    case NXINPUT_OBSERVE_TRIGGER_BOTH:
      return "both";
    case NXINPUT_OBSERVE_TRIGGER_NA:
    default:
      return "na";
  }
}

static const char *chord_state_name(nxinput_observe_chord_state state) {
  switch (state) {
    case NXINPUT_OBSERVE_CHORD_ARMED:
      return "armed";
    case NXINPUT_OBSERVE_CHORD_FIRED:
      return "fired";
    case NXINPUT_OBSERVE_CHORD_IDLE:
    default:
      return "idle";
  }
}

static const char *phase_name(nxinput_observe_event_phase phase) {
  switch (phase) {
    case NXINPUT_OBSERVE_PHASE_PRESS:
      return "press";
    case NXINPUT_OBSERVE_PHASE_RELEASE:
      return "release";
    case NXINPUT_OBSERVE_PHASE_DEADZONE_EXIT:
      return "dz-exit";
    case NXINPUT_OBSERVE_PHASE_DEADZONE_ENTER:
      return "dz-enter";
    case NXINPUT_OBSERVE_PHASE_THRESHOLD_ENTER:
      return "thr-enter";
    case NXINPUT_OBSERVE_PHASE_THRESHOLD_EXIT:
    default:
      return "thr-exit";
  }
}

/* Allowed characters for sanitized free text. Everything else -- and any
 * token carrying '/', or shaped like an IPv4 -- is redacted so no personal
 * path, IP, hostname or free-form device name survives into a receipt. */
static int allowed_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '+' || c == ':' ||
         c == '.' || c == ',' || c == '=' || c == '-';
}

static int looks_like_ipv4(const char *s) {
  unsigned int dots = 0u, digits = 0u;
  const char *p;
  for (p = s; *p != '\0'; p++) {
    if (*p == '.') {
      dots++;
    } else if (*p >= '0' && *p <= '9') {
      digits++;
    } else {
      return 0;
    }
  }
  return dots == 3u && digits >= 4u;
}

const char *nxinput_observe_sanitize(char *dst, size_t cap, const char *src) {
  size_t out = 0u;
  size_t i;
  if (dst == NULL || cap == 0u) {
    return "-";
  }
  if (src == NULL || src[0] == '\0') {
    (void)snprintf(dst, cap, "-");
    return dst;
  }
  if (strchr(src, '/') != NULL || looks_like_ipv4(src)) {
    (void)snprintf(dst, cap, "redacted");
    return dst;
  }
  for (i = 0u; src[i] != '\0' && out + 1u < cap; i++) {
    dst[out++] = allowed_char(src[i]) ? src[i] : '_';
  }
  dst[out] = '\0';
  if (out == 0u) {
    (void)snprintf(dst, cap, "-");
  }
  return dst;
}

/* FNV-1a over the capability numbers: a stable digest that never encodes a
 * device name. */
static uint32_t caps_digest(const unsigned int *values, size_t count) {
  uint32_t hash = UINT32_C(2166136261);
  size_t i;
  for (i = 0u; i < count; i++) {
    hash ^= (uint32_t)values[i];
    hash *= UINT32_C(16777619);
  }
  return hash;
}

static int observing(const nxinput_observe *obs) {
  return obs != NULL && obs->api_version == NXINPUT_OBSERVE_API_VERSION &&
         obs->struct_size == sizeof(*obs) && obs->sink != NULL;
}

static void emit(nxinput_observe *obs, const char *marker, const char *body) {
  char line[NXINPUT_OBSERVE_LINE_MAX];
  int written;
  obs->seq++;
  written = snprintf(line, sizeof line,
                     "%s: schema=%s/%d run=%s gen=%s consumer=%s seq=%u %s",
                     marker, NXINPUT_OBSERVE_SCHEMA,
                     NXINPUT_OBSERVE_SCHEMA_VERSION, obs->run_id,
                     obs->generation, obs->consumer, (unsigned int)obs->seq,
                     body);
  if (written < 0 || (size_t)written >= sizeof line) {
    /* A receipt that does not fit is truncated by snprintf; still emitted so
     * the drop is visible rather than silent. */
  }
  obs->sink(obs->userdata, line);
}

int nxinput_observe_init(nxinput_observe *obs, nxinput_observe_sink sink,
                         void *userdata, const char *run_id,
                         const char *generation, const char *consumer) {
  unsigned int pad;
  if (obs == NULL) {
    return -1;
  }
  memset(obs, 0, sizeof(*obs));
  obs->api_version = NXINPUT_OBSERVE_API_VERSION;
  obs->struct_size = sizeof(*obs);
  obs->sink = sink;
  obs->userdata = userdata;
  (void)nxinput_observe_sanitize(obs->run_id, sizeof obs->run_id, run_id);
  (void)nxinput_observe_sanitize(obs->generation, sizeof obs->generation,
                                 generation);
  (void)nxinput_observe_sanitize(obs->consumer, sizeof obs->consumer,
                                 consumer);
  for (pad = 0u; pad < NXINPUT_OBSERVE_MAX_PADS; pad++) {
    nxinput_observe_pad_reset(obs, pad);
  }
  return 0;
}

void nxinput_observe_pad_reset(nxinput_observe *obs, unsigned int pad) {
  nxinput_observe_pad_state *state;
  int side;
  if (obs == NULL || pad >= NXINPUT_OBSERVE_MAX_PADS) {
    return;
  }
  state = &obs->pads[pad];
  memset(state, 0, sizeof(*state));
  for (side = 0; side < 2; side++) {
    state->stick_min[side][0] = INT16_MAX;
    state->stick_min[side][1] = INT16_MAX;
    state->stick_max[side][0] = INT16_MIN;
    state->stick_max[side][1] = INT16_MIN;
    state->trigger_min[side] = INT32_MAX;
    state->trigger_max[side] = INT32_MIN;
  }
}

void nxinput_observe_load(nxinput_observe *obs, nxinput_observe_source source,
                          unsigned int entries, const char *map_sha256,
                          const char *guid_requested,
                          const char *guid_selected, unsigned int priority,
                          const char *result) {
  char body[NXINPUT_OBSERVE_LINE_MAX];
  char hash[72], req[40], sel[40], res[48];
  if (!observing(obs)) {
    return;
  }
  (void)nxinput_observe_sanitize(hash, sizeof hash, map_sha256);
  (void)nxinput_observe_sanitize(req, sizeof req, guid_requested);
  (void)nxinput_observe_sanitize(sel, sizeof sel, guid_selected);
  (void)nxinput_observe_sanitize(res, sizeof res, result);
  (void)snprintf(body, sizeof body,
                 "source=%s entries=%u map_sha256=%s guid_requested=%s "
                 "guid_selected=%s priority=%u result=%s",
                 nxinput_observe_source_name(source), entries, hash, req, sel,
                 priority, res);
  emit(obs, "NXINPUT-LOAD", body);
}

void nxinput_observe_capabilities(nxinput_observe *obs, unsigned int pad,
                                  unsigned int buttons, unsigned int axes,
                                  unsigned int hats, unsigned int ordinal_max,
                                  unsigned int low_keys,
                                  unsigned int gamepad_lo,
                                  unsigned int gamepad_hi,
                                  unsigned int analog_axes) {
  char body[NXINPUT_OBSERVE_LINE_MAX];
  unsigned int values[8];
  if (!observing(obs) || pad >= NXINPUT_OBSERVE_MAX_PADS) {
    return;
  }
  obs->pads[pad].in_use = 1u;
  values[0] = buttons; values[1] = axes; values[2] = hats;
  values[3] = ordinal_max; values[4] = low_keys; values[5] = gamepad_lo;
  values[6] = gamepad_hi; values[7] = analog_axes;
  (void)snprintf(body, sizeof body,
                 "pad=%u buttons=%u axes=%u hats=%u ordinal_max=%u "
                 "low_keys=%u gamepad_range=%u-%u analog_axes=%u "
                 "digest=%08x",
                 pad, buttons, axes, hats, ordinal_max, low_keys, gamepad_lo,
                 gamepad_hi, analog_axes,
                 (unsigned int)caps_digest(values, 8u));
  emit(obs, "NXINPUT-CAPABILITIES", body);
}

void nxinput_observe_binding(nxinput_observe *obs, unsigned int pad,
                             nxinput_observe_control control,
                             const char *physical,
                             nxinput_observe_bind_kind kind, int ordinal,
                             nxinput_observe_semantic semantic,
                             const char *sink_label, int reachable,
                             nxinput_observe_trigger_kind trigger_kind) {
  char body[NXINPUT_OBSERVE_LINE_MAX];
  char phys[48], sink_text[48];
  if (!observing(obs) || pad >= NXINPUT_OBSERVE_MAX_PADS ||
      (unsigned int)control >= NXINPUT_OBSERVE_CONTROL_COUNT) {
    return;
  }
  obs->pads[pad].binding_reported |= UINT32_C(1) << (unsigned int)control;
  (void)nxinput_observe_sanitize(phys, sizeof phys, physical);
  (void)nxinput_observe_sanitize(sink_text, sizeof sink_text, sink_label);
  (void)snprintf(body, sizeof body,
                 "pad=%u control=%s physical=%s kind=%s ordinal=%d "
                 "semantic=%s sink=%s reachable=%d trigger_kind=%s",
                 pad, nxinput_observe_control_name(control), phys,
                 bind_kind_name(kind), ordinal,
                 nxinput_observe_semantic_name(semantic), sink_text,
                 reachable ? 1 : 0, trigger_kind_name(trigger_kind));
  emit(obs, "NXINPUT-BINDING", body);
}

uint32_t nxinput_observe_binding_missing(const nxinput_observe *obs,
                                         unsigned int pad) {
  uint32_t all =
      (UINT32_C(1) << (unsigned int)NXINPUT_OBSERVE_CONTROL_COUNT) - 1u;
  if (obs == NULL || pad >= NXINPUT_OBSERVE_MAX_PADS) {
    return all;
  }
  return all & ~obs->pads[pad].binding_reported;
}

void nxinput_observe_chord(nxinput_observe *obs, unsigned int pad,
                           const char *select_physical,
                           const char *start_physical, int same_instance,
                           nxinput_observe_chord_state state) {
  char body[NXINPUT_OBSERVE_LINE_MAX];
  char sel[48], sta[48];
  if (!observing(obs) || pad >= NXINPUT_OBSERVE_MAX_PADS) {
    return;
  }
  (void)nxinput_observe_sanitize(sel, sizeof sel, select_physical);
  (void)nxinput_observe_sanitize(sta, sizeof sta, start_physical);
  (void)snprintf(body, sizeof body,
                 "pad=%u select=%s start=%s same_instance=%d state=%s",
                 pad, sel, sta, same_instance ? 1 : 0,
                 chord_state_name(state));
  emit(obs, "NXINPUT-CHORD", body);
}

void nxinput_observe_chord_denied(nxinput_observe *obs,
                                  nxinput_observe_chord_negative kind) {
  const char *pair;
  char body[NXINPUT_OBSERVE_LINE_MAX];
  if (!observing(obs)) {
    return;
  }
  switch (kind) {
    case NXINPUT_OBSERVE_CHORD_NEG_L2_R2:
      pair = "L2+R2";
      break;
    case NXINPUT_OBSERVE_CHORD_NEG_GUIDE_START:
      pair = "GUIDE+START";
      break;
    case NXINPUT_OBSERVE_CHORD_NEG_CROSS_PAD:
    default:
      pair = "SELECT+START-cross-pad";
      break;
  }
  (void)snprintf(body, sizeof body, "pair=%s exit=denied", pair);
  emit(obs, "NXINPUT-CHORD", body);
}

/* Bounded event admission: firsts always pass; diagnostic mode admits more
 * until the hard budget; everything else only bumps the drop counter. */
static int admit_extra(nxinput_observe *obs) {
  if (obs->diagnostic_mode && obs->diag_used < NXINPUT_OBSERVE_DIAG_BUDGET) {
    obs->diag_used++;
    return 1;
  }
  obs->dropped++;
  return 0;
}

static void emit_event(nxinput_observe *obs, unsigned int pad,
                       const char *control_name,
                       nxinput_observe_event_phase phase, const char *physical,
                       const char *context, const char *sink_label,
                       const char *extra) {
  char body[NXINPUT_OBSERVE_LINE_MAX];
  char phys[48], ctx[48], sink_text[48];
  (void)nxinput_observe_sanitize(phys, sizeof phys, physical);
  (void)nxinput_observe_sanitize(ctx, sizeof ctx, context);
  (void)nxinput_observe_sanitize(sink_text, sizeof sink_text, sink_label);
  (void)snprintf(body, sizeof body,
                 "pad=%u control=%s phase=%s physical=%s context=%s "
                 "sink=%s%s%s",
                 pad, control_name, phase_name(phase), phys, ctx, sink_text,
                 extra != NULL ? " " : "", extra != NULL ? extra : "");
  emit(obs, "NXINPUT-EVENT", body);
}

void nxinput_observe_event(nxinput_observe *obs, unsigned int pad,
                           nxinput_observe_control control,
                           nxinput_observe_event_phase phase,
                           const char *physical, const char *context,
                           const char *sink_label) {
  nxinput_observe_pad_state *state;
  uint32_t bit;
  int first;
  if (!observing(obs) || pad >= NXINPUT_OBSERVE_MAX_PADS ||
      (unsigned int)control >= NXINPUT_OBSERVE_CONTROL_COUNT) {
    return;
  }
  state = &obs->pads[pad];
  bit = UINT32_C(1) << (unsigned int)control;
  if (phase == NXINPUT_OBSERVE_PHASE_PRESS) {
    first = (state->press_seen & bit) == 0u;
    state->press_seen |= bit;
  } else if (phase == NXINPUT_OBSERVE_PHASE_RELEASE) {
    first = (state->release_seen & bit) == 0u;
    state->release_seen |= bit;
  } else {
    first = 0;
  }
  if (!first && !admit_extra(obs)) {
    return;
  }
  emit_event(obs, pad, nxinput_observe_control_name(control), phase, physical,
             context, sink_label, first ? "first=1" : "first=0");
}

void nxinput_observe_stick(nxinput_observe *obs, unsigned int pad, int right,
                           int16_t x, int16_t y, int16_t deadzone) {
  nxinput_observe_pad_state *state;
  int side = right ? 1 : 0;
  int outside;
  const char *name;
  if (!observing(obs) || pad >= NXINPUT_OBSERVE_MAX_PADS) {
    return;
  }
  state = &obs->pads[pad];
  if (x < state->stick_min[side][0]) state->stick_min[side][0] = x;
  if (x > state->stick_max[side][0]) state->stick_max[side][0] = x;
  if (y < state->stick_min[side][1]) state->stick_min[side][1] = y;
  if (y > state->stick_max[side][1]) state->stick_max[side][1] = y;
  outside = (x > deadzone || x < (int16_t)-deadzone ||
             y > deadzone || y < (int16_t)-deadzone);
  name = right ? "RIGHT_STICK" : "LEFT_STICK";
  if (outside && !state->stick_out[side]) {
    state->stick_out[side] = 1u;
    if (!state->stick_dz_exit_seen[side]) {
      char extra[64];
      state->stick_dz_exit_seen[side] = 1u;
      (void)snprintf(extra, sizeof extra, "first=1 x=%d y=%d dz=%d",
                     (int)x, (int)y, (int)deadzone);
      emit_event(obs, pad, name, NXINPUT_OBSERVE_PHASE_DEADZONE_EXIT,
                 "analog", "-", "-", extra);
    } else if (admit_extra(obs)) {
      emit_event(obs, pad, name, NXINPUT_OBSERVE_PHASE_DEADZONE_EXIT,
                 "analog", "-", "-", "first=0");
    }
  } else if (!outside && state->stick_out[side]) {
    state->stick_out[side] = 0u;
    if (!state->stick_dz_enter_seen[side]) {
      state->stick_dz_enter_seen[side] = 1u;
      emit_event(obs, pad, name, NXINPUT_OBSERVE_PHASE_DEADZONE_ENTER,
                 "analog", "-", "-", "first=1");
    } else if (admit_extra(obs)) {
      emit_event(obs, pad, name, NXINPUT_OBSERVE_PHASE_DEADZONE_ENTER,
                 "analog", "-", "-", "first=0");
    }
  }
}

void nxinput_observe_trigger(nxinput_observe *obs, unsigned int pad, int right,
                             int32_t value, int32_t threshold) {
  nxinput_observe_pad_state *state;
  int side = right ? 1 : 0;
  int in;
  const char *name = right ? "R2" : "L2";
  if (!observing(obs) || pad >= NXINPUT_OBSERVE_MAX_PADS) {
    return;
  }
  state = &obs->pads[pad];
  if (value < state->trigger_min[side]) state->trigger_min[side] = value;
  if (value > state->trigger_max[side]) state->trigger_max[side] = value;
  in = value >= threshold;
  if (in && !state->trigger_in[side]) {
    state->trigger_in[side] = 1u;
    if (!state->trigger_enter_seen[side]) {
      char extra[64];
      state->trigger_enter_seen[side] = 1u;
      (void)snprintf(extra, sizeof extra, "first=1 value=%d thr=%d",
                     (int)value, (int)threshold);
      emit_event(obs, pad, name, NXINPUT_OBSERVE_PHASE_THRESHOLD_ENTER,
                 "analog", "-", "-", extra);
    } else if (admit_extra(obs)) {
      emit_event(obs, pad, name, NXINPUT_OBSERVE_PHASE_THRESHOLD_ENTER,
                 "analog", "-", "-", "first=0");
    }
  } else if (!in && state->trigger_in[side]) {
    state->trigger_in[side] = 0u;
    if (!state->trigger_exit_seen[side]) {
      state->trigger_exit_seen[side] = 1u;
      emit_event(obs, pad, name, NXINPUT_OBSERVE_PHASE_THRESHOLD_EXIT,
                 "analog", "-", "-", "first=1");
    } else if (admit_extra(obs)) {
      emit_event(obs, pad, name, NXINPUT_OBSERVE_PHASE_THRESHOLD_EXIT,
                 "analog", "-", "-", "first=0");
    }
  }
}

void nxinput_observe_summary(nxinput_observe *obs, unsigned int pad) {
  nxinput_observe_pad_state *state;
  char body[NXINPUT_OBSERVE_LINE_MAX];
  char never[192];
  size_t off = 0u;
  unsigned int control;
  int side;
  if (!observing(obs) || pad >= NXINPUT_OBSERVE_MAX_PADS) {
    return;
  }
  state = &obs->pads[pad];
  never[0] = '\0';
  for (control = 0u; control < NXINPUT_OBSERVE_CONTROL_COUNT; control++) {
    if ((state->press_seen & (UINT32_C(1) << control)) == 0u) {
      int written = snprintf(never + off, sizeof never - off, "%s%s",
                             off > 0u ? "," : "", control_names[control]);
      if (written < 0 || (size_t)written >= sizeof never - off) {
        break;
      }
      off += (size_t)written;
    }
  }
  (void)snprintf(body, sizeof body,
                 "pad=%u kind=summary never_pressed=%s dropped=%u",
                 pad, never[0] != '\0' ? never : "none",
                 (unsigned int)obs->dropped);
  emit(obs, "NXINPUT-EVENT", body);
  for (side = 0; side < 2; side++) {
    if (state->stick_min[side][0] == INT16_MAX) {
      continue; /* never fed */
    }
    (void)snprintf(body, sizeof body,
                   "pad=%u kind=stick-summary side=%s min_x=%d max_x=%d "
                   "min_y=%d max_y=%d",
                   pad, side ? "right" : "left",
                   (int)state->stick_min[side][0],
                   (int)state->stick_max[side][0],
                   (int)state->stick_min[side][1],
                   (int)state->stick_max[side][1]);
    emit(obs, "NXINPUT-EVENT", body);
  }
  for (side = 0; side < 2; side++) {
    if (state->trigger_min[side] == INT32_MAX) {
      continue;
    }
    (void)snprintf(body, sizeof body,
                   "pad=%u kind=trigger-summary side=%s min=%d max=%d",
                   pad, side ? "right" : "left",
                   (int)state->trigger_min[side],
                   (int)state->trigger_max[side]);
    emit(obs, "NXINPUT-EVENT", body);
  }
}

void nxinput_observe_consumer(nxinput_observe *obs,
                              nxinput_observe_control control,
                              const char *action, const char *state,
                              const char *context,
                              nxinput_observe_delivery delivery) {
  char body[NXINPUT_OBSERVE_LINE_MAX];
  char act[48], sta[32], ctx[48];
  if (!observing(obs) ||
      (unsigned int)control >= NXINPUT_OBSERVE_CONTROL_COUNT) {
    return;
  }
  (void)nxinput_observe_sanitize(act, sizeof act, action);
  (void)nxinput_observe_sanitize(sta, sizeof sta, state);
  (void)nxinput_observe_sanitize(ctx, sizeof ctx, context);
  (void)snprintf(body, sizeof body,
                 "control=%s action=%s state=%s context=%s delivery=%s",
                 nxinput_observe_control_name(control), act, sta, ctx,
                 nxinput_observe_delivery_name(delivery));
  emit(obs, "NXINPUT-CONSUMER", body);
}
