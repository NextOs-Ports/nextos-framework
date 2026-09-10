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

const char *nxinput_gptk_live_result_name(nxinput_gptk_live_result result);
const char *nxinput_gptk_runtime_marker(void);
const char *nxinput_gptk_event_evidence_schema(void);

#ifdef __cplusplus
}
#endif

#endif
