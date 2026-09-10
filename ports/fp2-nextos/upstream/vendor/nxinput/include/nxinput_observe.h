/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxinput_observe -- V4-CONTROLLERS-03 / C2: complete, bounded input
 * observability.
 *
 * WHY THIS EXISTS
 * ---------------
 * A log line like "button=4" or "keys=17 godot=14 ignored_low=3" proves
 * nothing: it does not say where the mapping came from, which GUID entry was
 * selected, how EACH control was bound, which press/release arrived, or which
 * consumer callback delivered or suppressed the event. This module makes the
 * whole chain explain itself in bounded, legible receipts.
 *
 * INVARIANT (absolute for C2): observability ON or OFF produces exactly the
 * same decisions and the same input sequence. This is guaranteed by
 * construction -- the module is a PURE OBSERVER:
 *   - it never calls SDL, never touches the environment, never opens devices;
 *   - every record_* function is fed with facts the caller already measured;
 *   - no record_* function returns data a caller could branch on (void), so
 *     telemetry cannot leak back into the decision path;
 *   - with a NULL sink every call is a cheap no-op.
 *
 * It does NOT decide which mapping wins, does not rewrite anything and does
 * not correct A/B, L2/R2 or R3 -- those belong to later categories. It only
 * makes today's behavior visible, including the wrong parts.
 *
 * Bounded by design: one LOAD line, one CAPABILITIES line per pad, exactly
 * NXINPUT_OBSERVE_CONTROL_COUNT BINDING lines per pad report, first press and
 * first release per control per run (plus deadzone/threshold firsts and
 * min/max extremes for sticks/triggers), and an optional diagnostic budget
 * that is still a hard cap -- never an unbounded per-frame log.
 *
 * Sanitization: free-text fields pass through nxinput_observe_sanitize();
 * anything that looks like a path, an IP, a hostname or a free-form device
 * name is redacted. Symbolic names (BTN_*, SDL_*, b0..b31, a0.., h0.1) pass.
 */
#ifndef NXINPUT_OBSERVE_H
#define NXINPUT_OBSERVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_OBSERVE_API_VERSION 1u
#define NXINPUT_OBSERVE_SCHEMA "nx-input-observe"
#define NXINPUT_OBSERVE_SCHEMA_VERSION 1

#define NXINPUT_OBSERVE_MAX_PADS 4u
#define NXINPUT_OBSERVE_LINE_MAX 512u
#define NXINPUT_OBSERVE_TEXT_MAX 64u
/* Diagnostic-mode budget: additional event lines beyond the bounded firsts.
 * A hard cap, never an unbounded per-frame log. */
#define NXINPUT_OBSERVE_DIAG_BUDGET 256u

/* The 18 canonical controls. Every binding report emits ALL of them, used or
 * not, in exactly this order. */
typedef enum nxinput_observe_control {
  NXINPUT_OBSERVE_A = 0,
  NXINPUT_OBSERVE_B,
  NXINPUT_OBSERVE_X,
  NXINPUT_OBSERVE_Y,
  NXINPUT_OBSERVE_L1,
  NXINPUT_OBSERVE_R1,
  NXINPUT_OBSERVE_L2,
  NXINPUT_OBSERVE_R2,
  NXINPUT_OBSERVE_L3,
  NXINPUT_OBSERVE_R3,
  NXINPUT_OBSERVE_START,
  NXINPUT_OBSERVE_SELECT,
  NXINPUT_OBSERVE_UP,
  NXINPUT_OBSERVE_DOWN,
  NXINPUT_OBSERVE_LEFT,
  NXINPUT_OBSERVE_RIGHT,
  NXINPUT_OBSERVE_LEFT_STICK,
  NXINPUT_OBSERVE_RIGHT_STICK,
  NXINPUT_OBSERVE_CONTROL_COUNT
} nxinput_observe_control;

/* Where the active mapping was loaded from. Classes only -- never a path. */
typedef enum nxinput_observe_source {
  NXINPUT_OBSERVE_SOURCE_PORTMASTER_ENV = 0, /* SDL_GAMECONTROLLERCONFIG */
  NXINPUT_OBSERVE_SOURCE_CFW_FILE,           /* SDL_GAMECONTROLLERCONFIG_FILE */
  NXINPUT_OBSERVE_SOURCE_PORT_BUNDLE,        /* file pinned inside the port */
  NXINPUT_OBSERVE_SOURCE_SDL_BUILTIN         /* SDL's own database */
} nxinput_observe_source;

/* How the physical control reaches the canonical one. */
typedef enum nxinput_observe_bind_kind {
  NXINPUT_OBSERVE_BIND_NONE = 0,
  NXINPUT_OBSERVE_BIND_BUTTON,
  NXINPUT_OBSERVE_BIND_AXIS,
  NXINPUT_OBSERVE_BIND_HAT
} nxinput_observe_bind_kind;

/* Trigger duality: L2/R2 must always say whether they are axis, button or
 * both. NA for everything that is not a trigger. */
typedef enum nxinput_observe_trigger_kind {
  NXINPUT_OBSERVE_TRIGGER_NA = 0,
  NXINPUT_OBSERVE_TRIGGER_AXIS,
  NXINPUT_OBSERVE_TRIGGER_BUTTON,
  NXINPUT_OBSERVE_TRIGGER_BOTH
} nxinput_observe_trigger_kind;

/* Semantic state of a binding. C2 reports only what EXISTS today: a control
 * absent from a GPTK V1 file is legacy-unmanaged -- never an anticipated
 * `null`/`native` of the future V2 format. */
typedef enum nxinput_observe_semantic {
  NXINPUT_OBSERVE_SEMANTIC_ACTION = 0,
  NXINPUT_OBSERVE_SEMANTIC_NULL,
  NXINPUT_OBSERVE_SEMANTIC_NATIVE,
  NXINPUT_OBSERVE_SEMANTIC_LEGACY_UNMANAGED
} nxinput_observe_semantic;

/* Consumer delivery. An adapter that is not instrumented yet MUST report
 * pending/not-instrumented; only the real callback/readback that hands the
 * state to the engine may say delivered or suppressed. */
typedef enum nxinput_observe_delivery {
  NXINPUT_OBSERVE_PENDING_NOT_INSTRUMENTED = 0,
  NXINPUT_OBSERVE_DELIVERED,
  NXINPUT_OBSERVE_SUPPRESSED
} nxinput_observe_delivery;

typedef enum nxinput_observe_event_phase {
  NXINPUT_OBSERVE_PHASE_PRESS = 0,
  NXINPUT_OBSERVE_PHASE_RELEASE,
  NXINPUT_OBSERVE_PHASE_DEADZONE_EXIT,  /* stick left the deadzone */
  NXINPUT_OBSERVE_PHASE_DEADZONE_ENTER, /* stick back to neutral */
  NXINPUT_OBSERVE_PHASE_THRESHOLD_ENTER, /* analog trigger crossed in */
  NXINPUT_OBSERVE_PHASE_THRESHOLD_EXIT   /* analog trigger crossed out */
} nxinput_observe_event_phase;

typedef enum nxinput_observe_chord_state {
  NXINPUT_OBSERVE_CHORD_IDLE = 0,
  NXINPUT_OBSERVE_CHORD_ARMED,
  NXINPUT_OBSERVE_CHORD_FIRED
} nxinput_observe_chord_state;

/* Negative chord attestations: pairs that must NEVER leave the game. */
typedef enum nxinput_observe_chord_negative {
  NXINPUT_OBSERVE_CHORD_NEG_L2_R2 = 0,
  NXINPUT_OBSERVE_CHORD_NEG_GUIDE_START,
  NXINPUT_OBSERVE_CHORD_NEG_CROSS_PAD
} nxinput_observe_chord_negative;

typedef void (*nxinput_observe_sink)(void *userdata, const char *line);

/* Per-pad bounded state. Public only so the caller can allocate the context;
 * never poke the fields directly. */
typedef struct nxinput_observe_pad_state {
  uint32_t press_seen;   /* one bit per canonical control */
  uint32_t release_seen;
  uint32_t binding_reported;
  uint8_t stick_out[2];  /* left/right: currently outside the deadzone */
  uint8_t stick_dz_exit_seen[2];
  uint8_t stick_dz_enter_seen[2];
  int16_t stick_min[2][2]; /* [side][x=0,y=1] */
  int16_t stick_max[2][2];
  uint8_t trigger_in[2]; /* left/right: currently past the threshold */
  uint8_t trigger_enter_seen[2];
  uint8_t trigger_exit_seen[2];
  int32_t trigger_min[2];
  int32_t trigger_max[2];
  uint8_t in_use;
} nxinput_observe_pad_state;

typedef struct nxinput_observe {
  uint32_t api_version; /* NXINPUT_OBSERVE_API_VERSION */
  size_t struct_size;   /* sizeof(nxinput_observe) */
  nxinput_observe_sink sink; /* NULL = observability OFF (all no-ops) */
  void *userdata;
  int diagnostic_mode;  /* extra bounded events up to DIAG_BUDGET */
  uint32_t diag_used;
  uint32_t seq;         /* monotonic receipt sequence */
  uint32_t dropped;     /* events beyond every budget (counted, not logged) */
  char run_id[96];
  char generation[72];
  char consumer[NXINPUT_OBSERVE_TEXT_MAX]; /* consumer runtime tag */
  nxinput_observe_pad_state pads[NXINPUT_OBSERVE_MAX_PADS];
} nxinput_observe;

/* Initialise. `sink` NULL turns every later call into a no-op. run_id,
 * generation and consumer are sanitized copies ("" allowed -> "-"). Returns
 * 0, -1 on NULL obs. */
int nxinput_observe_init(nxinput_observe *obs, nxinput_observe_sink sink,
                         void *userdata, const char *run_id,
                         const char *generation, const char *consumer);

/* Reset one pad slot (hotplug remove/re-add). Receipts of other pads are
 * never mixed or lost. */
void nxinput_observe_pad_reset(nxinput_observe *obs, unsigned int pad);

/* Copy `src` into dst applying the redaction rules: tokens containing '/',
 * anything shaped like an IPv4, and characters outside [A-Za-z0-9_+:.,=-]
 * are replaced so no personal path, IP, hostname or free-form device name
 * survives. Empty/NULL becomes "-". Returns dst. */
const char *nxinput_observe_sanitize(char *dst, size_t cap, const char *src);

/* NXINPUT-LOAD: provenance of the active mapping. `map_sha256` is the hash of
 * the mapping content (the content itself never enters the receipt),
 * `entries` how many mapping lines the source carried, `guid_requested` and
 * `guid_selected` the GUID asked for / actually chosen, `priority` the
 * loading order slot, `result` a stable token such as "selected",
 * "fallback-builtin", "empty-source". */
void nxinput_observe_load(nxinput_observe *obs, nxinput_observe_source source,
                          unsigned int entries, const char *map_sha256,
                          const char *guid_requested,
                          const char *guid_selected, unsigned int priority,
                          const char *result);

/* NXINPUT-CAPABILITIES: measured physical facts of one pad. `ordinal_max` is
 * the highest ordinal observed, `low_keys` how many sub-gamepad key codes
 * exist, `gamepad_lo`/`gamepad_hi` the observed gamepad ordinal range,
 * `analog_axes` how many analog axes. The digest printed is derived from the
 * numbers alone -- a device NAME is never a criterion and never logged. */
void nxinput_observe_capabilities(nxinput_observe *obs, unsigned int pad,
                                  unsigned int buttons, unsigned int axes,
                                  unsigned int hats, unsigned int ordinal_max,
                                  unsigned int low_keys, unsigned int gamepad_lo,
                                  unsigned int gamepad_hi,
                                  unsigned int analog_axes);

/* NXINPUT-BINDING: one line for ONE canonical control. A complete report
 * emits all NXINPUT_OBSERVE_CONTROL_COUNT controls for the pad; use
 * nxinput_observe_binding_missing() to prove completeness in gates.
 * `physical` is the symbolic physical source ("b0", "a2", "h0.1", "BTN_TL2",
 * "none"), `sink_label` names the guest sink class ("sdl-gamecontroller",
 * "gptokeyb-keyboard", "uinput", "none"). `trigger_kind` is mandatory
 * (non-NA) for L2/R2 and NA for everything else. */
void nxinput_observe_binding(nxinput_observe *obs, unsigned int pad,
                             nxinput_observe_control control,
                             const char *physical,
                             nxinput_observe_bind_kind kind, int ordinal,
                             nxinput_observe_semantic semantic,
                             const char *sink_label, int reachable,
                             nxinput_observe_trigger_kind trigger_kind);

/* Bitmask of canonical controls NOT yet reported for `pad` (0 == complete). */
uint32_t nxinput_observe_binding_missing(const nxinput_observe *obs,
                                         unsigned int pad);

/* NXINPUT-CHORD: the exact pair governing SELECT+START on ONE pad/instance. */
void nxinput_observe_chord(nxinput_observe *obs, unsigned int pad,
                           const char *select_physical,
                           const char *start_physical, int same_instance,
                           nxinput_observe_chord_state state);

/* Negative attestation: this pair was seen and did NOT trigger the exit. */
void nxinput_observe_chord_denied(nxinput_observe *obs,
                                  nxinput_observe_chord_negative kind);

/* NXINPUT-EVENT (digital): bounded first press and first release per control
 * per run. In diagnostic mode later events also print until the hard budget;
 * beyond every budget they are counted in `dropped`, never logged. */
void nxinput_observe_event(nxinput_observe *obs, unsigned int pad,
                           nxinput_observe_control control,
                           nxinput_observe_event_phase phase,
                           const char *physical, const char *context,
                           const char *sink_label);

/* NXINPUT-EVENT (stick): feed raw positions; emits bounded deadzone-exit /
 * back-to-neutral firsts and tracks min/max per axis. */
void nxinput_observe_stick(nxinput_observe *obs, unsigned int pad, int right,
                           int16_t x, int16_t y, int16_t deadzone);

/* NXINPUT-EVENT (analog trigger): feed raw value; emits bounded
 * threshold-enter/exit firsts and tracks min/max. */
void nxinput_observe_trigger(nxinput_observe *obs, unsigned int pad, int right,
                             int32_t value, int32_t threshold);

/* NXINPUT-EVENT summary lines for one pad: stick extremes and trigger
 * min/max, plus which controls were never pressed. One-shot per call site;
 * intended at teardown or on demand (doctor). */
void nxinput_observe_summary(nxinput_observe *obs, unsigned int pad);

/* NXINPUT-CONSUMER: ONLY the callback/readback that hands the state to the
 * engine may report delivered/suppressed. A dispatcher or parser must not
 * pretend to be the consumer; an uninstrumented adapter reports
 * NXINPUT_OBSERVE_PENDING_NOT_INSTRUMENTED. */
void nxinput_observe_consumer(nxinput_observe *obs,
                              nxinput_observe_control control,
                              const char *action, const char *state,
                              const char *context,
                              nxinput_observe_delivery delivery);

const char *nxinput_observe_control_name(nxinput_observe_control control);
const char *nxinput_observe_source_name(nxinput_observe_source source);
const char *nxinput_observe_semantic_name(nxinput_observe_semantic semantic);
const char *nxinput_observe_delivery_name(nxinput_observe_delivery delivery);

#ifdef __cplusplus
}
#endif

#endif /* NXINPUT_OBSERVE_H */
