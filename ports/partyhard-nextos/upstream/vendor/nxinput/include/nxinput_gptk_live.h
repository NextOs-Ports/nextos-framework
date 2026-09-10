/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_GPTK_LIVE_H
#define NXINPUT_GPTK_LIVE_H

/* Runtime boundary for an editable NEXTOSCONTROLLERS.gptk.
 *
 * The parser/dispatcher alone cannot know whether the engine is in a menu,
 * gameplay or a cursor overlay, nor whether a declared action reaches a real
 * engine sink.  This boundary therefore starts UNPROVEN.  It may consume an
 * input only after every ACTION has an ACK-capable sink and the adapter has
 * supplied a current, evidenced context.  Until then every event is explicit
 * PASSTHROUGH and the native engine path remains authoritative.
 */

#include "nxinput_gptk.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_GPTK_LIVE_API_VERSION 1u
/* /3: the 0.10.0 runtime understands NEXTOS_CONTROLLERS/3 (FACE_LAYOUT).
 * A V3-capable live runtime must never present the /2 marker (sealed
 * V4-CTRL-01 oracle, case N28); release gates match this exact string
 * inside the ELF. */
#define NXINPUT_GPTK_RUNTIME_MARKER "nxinput-gptk-runtime/3"
#define NXINPUT_GPTK_EVENT_EVIDENCE_SCHEMA "nxinput-gptk-event-evidence/1"
#define NXINPUT_GPTK_LIVE_CONTEXT_SOURCE_MAX 95u
/* 0.11.1 (V5-M1a item 2): the NEUTRAL FLOOR of the vector gesture edge.
 * A stick vector arrives every frame; the evidence of a gesture is an EDGE:
 * one "opened" when the vector leaves the neutral disc and one "closed"
 * when it returns. `x != 0 || y != 0` is NOT neutral detection: a pad whose
 * axis range is 0..255 normalizes its rest to +0.0039 (asymmetric halves
 * around the 127.5 midpoint), so with a zero floor the gesture opened at
 * boot and never closed (Nameless Cat menu on the primary CFW, 2026-09-03; FP2
 * and Blossom Tales carried the same `!= 0.0f`). The floor is universal:
 * default 1/64 (covers 0..255 -> 1/255 and 0..65535 -> 1.5e-5 rests), the
 * adapter may raise it per control to its own deadzone (cursor), never
 * above 0.9. Radius compare: x*x + y*y <= floor*floor. */
#define NXINPUT_GPTK_LIVE_VECTOR_NEUTRAL_FLOOR_DEFAULT 0.015625f
#define NXINPUT_GPTK_LIVE_VECTOR_NEUTRAL_FLOOR_MAX 0.9f

typedef enum nxinput_gptk_live_result {
  /* Native input must receive the event exactly once. */
  NXINPUT_GPTK_LIVE_PASSTHROUGH = 0,
  /* Every registered adapter sink ACKed this semantic event enqueue. */
  NXINPUT_GPTK_LIVE_DELIVERED = 1,
  /* A proven context explicitly mapped this control to `null`. */
  NXINPUT_GPTK_LIVE_SUPPRESSED = 2,
  /* A sink failed after invocation: never replay natively; invalidate run. */
  NXINPUT_GPTK_LIVE_FATAL = -1
} nxinput_gptk_live_result;

/* Return 0 only after the adapter boundary accepted/enqueued the action.
 * Evidence that C#/GDScript consumed the semantic action remains external. */
typedef int (*nxinput_gptk_live_sink_fn)(void *user, const char *action,
                                         int pressed, float value);
typedef int (*nxinput_gptk_live_vector_sink_fn)(void *user,
                                                const char *action,
                                                float x, float y);

typedef enum nxinput_gptk_live_vector_edge {
  NXINPUT_GPTK_VECTOR_EDGE_NONE = 0,
  NXINPUT_GPTK_VECTOR_EDGE_OPENED = 1,  /* left the neutral disc (gesture start) */
  NXINPUT_GPTK_VECTOR_EDGE_CLOSED = -1  /* returned to neutral (gesture end) */
} nxinput_gptk_live_vector_edge;

typedef struct nxinput_gptk_live_sink {
  char action[NXINPUT_GPTK_ACTION_MAX + 1u];
  nxinput_gptk_live_sink_fn fn;
  void *user;
} nxinput_gptk_live_sink;

typedef struct nxinput_gptk_live_vector_sink {
  char action[NXINPUT_GPTK_ACTION_MAX + 1u];
  nxinput_gptk_live_vector_sink_fn fn;
  void *user;
} nxinput_gptk_live_vector_sink;

typedef struct nxinput_gptk_live {
  const nxinput_gptk *map; /* not owned; must outlive this object */
  nxinput_gptk_context context;
  uint32_t context_epoch;
  uint32_t latched;
  int context_proven;
  int sealed;
  int fatal;
  size_t sink_count;
  size_t vector_sink_count;
  nxinput_gptk_live_sink sinks[NXINPUT_GPTK_MAX_SINKS];
  nxinput_gptk_live_vector_sink vector_sinks[NXINPUT_GPTK_MAX_SINKS];
  char context_source[NXINPUT_GPTK_LIVE_CONTEXT_SOURCE_MAX + 1u];
  /* 0.11.1: vector gesture edge state (per control bit). */
  float vector_neutral_floor[NXINPUT_GPTK_CONTROL_COUNT];
  uint32_t vector_active;          /* controls whose gesture is open */
  uint32_t vector_released_mask;   /* gestures closed by the last release-all */
  int last_vector_edge;            /* nxinput_gptk_live_vector_edge of the last feed_vector */
  unsigned long vector_gestures_opened, vector_gestures_closed;
} nxinput_gptk_live;

void nxinput_gptk_live_init(nxinput_gptk_live *live,
                            const nxinput_gptk *map);
int nxinput_gptk_live_register(nxinput_gptk_live *live, const char *action,
                               nxinput_gptk_live_sink_fn fn, void *user);
int nxinput_gptk_live_register_vector(
    nxinput_gptk_live *live, const char *action,
    nxinput_gptk_live_vector_sink_fn fn, void *user);

/* Freeze registration only when every ACTION in every present context has a
 * sink of the correct scalar/vector kind.  Failure leaves the runtime
 * unsealed and unable to suppress anything. */
int nxinput_gptk_live_seal(nxinput_gptk_live *live, char *error,
                           size_t error_size);

/* Prove a current context from real engine state. `source` is a bounded,
 * path-free evidence label such as "scene:main_menu". */
int nxinput_gptk_live_set_context(nxinput_gptk_live *live,
                                  nxinput_gptk_context context,
                                  const char *source);
void nxinput_gptk_live_clear_context(nxinput_gptk_live *live);
/* Additive checked form for adapters that must turn a release ACK failure into
 * terminal lifecycle failure instead of silently falling back to native. */
int nxinput_gptk_live_clear_context_checked(nxinput_gptk_live *live);
int nxinput_gptk_live_is_fatal(const nxinput_gptk_live *live);
int nxinput_gptk_live_context_proven(const nxinput_gptk_live *live);
int nxinput_gptk_live_ready(const nxinput_gptk_live *live);
uint32_t nxinput_gptk_live_context_epoch(const nxinput_gptk_live *live);
const char *nxinput_gptk_live_context_source(const nxinput_gptk_live *live);

/* Query before suppressing the native path.  It returns true only after seal
 * + proven context and only for ACTION/SUPPRESS. */
int nxinput_gptk_live_should_consume(const nxinput_gptk_live *live,
                                     int control);

nxinput_gptk_live_result nxinput_gptk_live_feed(
    nxinput_gptk_live *live, int control, int pressed, float value);
nxinput_gptk_live_result nxinput_gptk_live_feed_vector(
    nxinput_gptk_live *live, int control, float x, float y);

/* 0.11.1 vector gesture edge (item 2). The floor is per control, clamped to
 * [0, NXINPUT_GPTK_LIVE_VECTOR_NEUTRAL_FLOOR_MAX]; NaN/negative -> 0; a
 * non-stick control or NULL -> -1. init() sets the DEFAULT floor on both
 * sticks. */
int nxinput_gptk_live_set_vector_neutral_floor(nxinput_gptk_live *live,
                                               int control, float floor_r);
float nxinput_gptk_live_vector_neutral_floor(const nxinput_gptk_live *live,
                                             int control);
/* 1 when (x,y) is inside the neutral disc of `control` (radius = floor). */
int nxinput_gptk_live_vector_is_neutral(const nxinput_gptk_live *live,
                                        int control, float x, float y);
/* The edge produced by the LAST nxinput_gptk_live_feed_vector() call:
 * OPENED when a delivered vector left neutral while no gesture was open,
 * CLOSED when the gesture returned to neutral (or the decision stopped being
 * ACTION while a gesture was open), NONE otherwise. */
nxinput_gptk_live_vector_edge nxinput_gptk_live_last_vector_edge(
    const nxinput_gptk_live *live);
int nxinput_gptk_live_vector_active(const nxinput_gptk_live *live, int control);
/* Bitmask (1 << control) of the gestures that the last clear_context /
 * release-all closed; the adapter records their "closed" evidence lines. */
uint32_t nxinput_gptk_live_vector_released_mask(const nxinput_gptk_live *live);

const char *nxinput_gptk_live_result_name(nxinput_gptk_live_result result);
const char *nxinput_gptk_runtime_marker(void);
const char *nxinput_gptk_event_evidence_schema(void);

#ifdef __cplusplus
}
#endif

#endif
