/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxinput_gptk_live.h"

#include <stdio.h>
#include <string.h>

static void live_error(char *error, size_t error_size, const char *message) {
  if (error != 0 && error_size > 0u) {
    (void)snprintf(error, error_size, "NXI1010: %s", message);
  }
}

static int live_action_valid(const char *action) {
  size_t i;
  size_t length;

  if (action == 0) {
    return 0;
  }
  length = strlen(action);
  if (length == 0u || length > NXINPUT_GPTK_ACTION_MAX) {
    return 0;
  }
  for (i = 0u; i < length; i++) {
    unsigned char c = (unsigned char)action[i];
    if (!((c >= (unsigned char)'a' && c <= (unsigned char)'z') ||
          (c >= (unsigned char)'A' && c <= (unsigned char)'Z') ||
          (c >= (unsigned char)'0' && c <= (unsigned char)'9') ||
          c == (unsigned char)'_' || c == (unsigned char)'.' ||
          c == (unsigned char)'-')) {
      return 0;
    }
  }
  return 1;
}

static int live_context_source_valid(const char *source) {
  size_t i;
  size_t length;

  if (source == 0) {
    return 0;
  }
  length = strlen(source);
  if (length == 0u || length > NXINPUT_GPTK_LIVE_CONTEXT_SOURCE_MAX) {
    return 0;
  }
  for (i = 0u; i < length; i++) {
    unsigned char c = (unsigned char)source[i];
    if (!((c >= (unsigned char)'a' && c <= (unsigned char)'z') ||
          (c >= (unsigned char)'A' && c <= (unsigned char)'Z') ||
          (c >= (unsigned char)'0' && c <= (unsigned char)'9') ||
          c == (unsigned char)'_' || c == (unsigned char)'.' ||
          c == (unsigned char)':' || c == (unsigned char)'/' ||
          c == (unsigned char)'-')) {
      return 0;
    }
  }
  return 1;
}

static int live_is_vector_control(int control) {
  return control == (int)NXINPUT_GPTK_LEFT_STICK ||
         control == (int)NXINPUT_GPTK_RIGHT_STICK;
}

static size_t live_scalar_matches(const nxinput_gptk_live *live,
                                  const char *action) {
  size_t i;
  size_t count = 0u;

  for (i = 0u; live != 0 && i < live->sink_count; i++) {
    if (strcmp(live->sinks[i].action, action) == 0) {
      count++;
    }
  }
  return count;
}

static size_t live_vector_matches(const nxinput_gptk_live *live,
                                  const char *action) {
  size_t i;
  size_t count = 0u;

  for (i = 0u; live != 0 && i < live->vector_sink_count; i++) {
    if (strcmp(live->vector_sinks[i].action, action) == 0) {
      count++;
    }
  }
  return count;
}

void nxinput_gptk_live_init(nxinput_gptk_live *live,
                            const nxinput_gptk *map) {
  if (live == 0) {
    return;
  }
  memset(live, 0, sizeof *live);
  live->map = map;
  /* The numeric value is irrelevant until context_proven becomes true. */
  live->context = NXINPUT_GPTK_CONTEXT_MENU;
}

int nxinput_gptk_live_register(nxinput_gptk_live *live, const char *action,
                               nxinput_gptk_live_sink_fn fn, void *user) {
  size_t length;

  if (live == 0 || live->sealed || live->fatal || fn == 0 ||
      !live_action_valid(action) ||
      live->sink_count >= (size_t)NXINPUT_GPTK_MAX_SINKS) {
    return -1;
  }
  length = strlen(action);
  memcpy(live->sinks[live->sink_count].action, action, length + 1u);
  live->sinks[live->sink_count].fn = fn;
  live->sinks[live->sink_count].user = user;
  live->sink_count++;
  return 0;
}

int nxinput_gptk_live_register_vector(
    nxinput_gptk_live *live, const char *action,
    nxinput_gptk_live_vector_sink_fn fn, void *user) {
  size_t length;

  if (live == 0 || live->sealed || live->fatal || fn == 0 ||
      !live_action_valid(action) ||
      live->vector_sink_count >= (size_t)NXINPUT_GPTK_MAX_SINKS) {
    return -1;
  }
  length = strlen(action);
  memcpy(live->vector_sinks[live->vector_sink_count].action, action,
         length + 1u);
  live->vector_sinks[live->vector_sink_count].fn = fn;
  live->vector_sinks[live->vector_sink_count].user = user;
  live->vector_sink_count++;
  return 0;
}

int nxinput_gptk_live_seal(nxinput_gptk_live *live, char *error,
                           size_t error_size) {
  int context;
  int control;

  if (live == 0 || live->map == 0 || live->fatal || live->sealed) {
    live_error(error, error_size, "runtime cannot be sealed");
    return -1;
  }
  for (context = 0; context < (int)NXINPUT_GPTK_CONTEXT_COUNT; context++) {
    if (!live->map->context_present[context]) {
      continue;
    }
    for (control = 0; control < (int)NXINPUT_GPTK_CONTROL_COUNT; control++) {
      const char *action = 0;
      nxinput_gptk_decision decision = nxinput_gptk_decide(
          live->map, (nxinput_gptk_context)context, control, &action);
      size_t matches;

      if (decision != NXINPUT_GPTK_DECIDE_ACTION) {
        continue;
      }
      matches = live_is_vector_control(control)
                    ? live_vector_matches(live, action)
                    : live_scalar_matches(live, action);
      if (matches == 0u) {
        live_error(error, error_size,
                   live_is_vector_control(control)
                       ? "mapped vector action has no ACK sink"
                       : "mapped action has no ACK sink");
        /* Explicitly retain the native-safe state. */
        live->sealed = 0;
        live->context_proven = 0;
        live->context_source[0] = '\0';
        return -1;
      }
    }
  }
  live->sealed = 1;
  return 0;
}

static int live_emit_release(nxinput_gptk_live *live, int control) {
  const char *action = 0;
  size_t i;

  if (nxinput_gptk_decide(live->map, live->context, control, &action) !=
      NXINPUT_GPTK_DECIDE_ACTION) {
    return 0;
  }
  for (i = 0u; i < live->sink_count; i++) {
    if (strcmp(live->sinks[i].action, action) == 0 &&
        live->sinks[i].fn(live->sinks[i].user, action, 0, 0.0f) != 0) {
      return -1;
    }
  }
  return 0;
}

void nxinput_gptk_live_clear_context(nxinput_gptk_live *live) {
  int control;

  if (live == 0) {
    return;
  }
  if (live->context_proven) {
    for (control = 0; control < (int)NXINPUT_GPTK_CONTROL_COUNT; control++) {
      uint32_t bit = UINT32_C(1) << (unsigned int)control;
      if ((live->latched & bit) != 0u &&
          live_emit_release(live, control) != 0) {
        live->fatal = 1;
      }
    }
  }
  live->latched = 0u;
  live->context_proven = 0;
  live->context_source[0] = '\0';
}

int nxinput_gptk_live_clear_context_checked(nxinput_gptk_live *live) {
  if (live == 0) {
    return -1;
  }
  nxinput_gptk_live_clear_context(live);
  return live->fatal ? -1 : 0;
}

int nxinput_gptk_live_is_fatal(const nxinput_gptk_live *live) {
  return live != 0 && live->fatal ? 1 : 0;
}

int nxinput_gptk_live_set_context(nxinput_gptk_live *live,
                                  nxinput_gptk_context context,
                                  const char *source) {
  size_t length;

  if (live == 0 || !live->sealed || live->fatal || live->map == 0 ||
      (int)context < 0 || (int)context >= (int)NXINPUT_GPTK_CONTEXT_COUNT ||
      !live->map->context_present[context] ||
      !live_context_source_valid(source)) {
    if (live != 0) {
      nxinput_gptk_live_clear_context(live);
    }
    return -1;
  }
  if (live->context_proven && live->context == context &&
      strcmp(live->context_source, source) == 0) {
    return 0;
  }
  nxinput_gptk_live_clear_context(live);
  if (live->fatal) {
    return -1;
  }
  length = strlen(source);
  live->context = context;
  memcpy(live->context_source, source, length + 1u);
  live->context_epoch++;
  if (live->context_epoch == 0u) {
    live->context_epoch = 1u;
  }
  live->context_proven = 1;
  return 0;
}

int nxinput_gptk_live_context_proven(const nxinput_gptk_live *live) {
  return live != 0 && live->context_proven && !live->fatal ? 1 : 0;
}

int nxinput_gptk_live_ready(const nxinput_gptk_live *live) {
  return live != 0 && live->sealed && live->context_proven && !live->fatal
             ? 1
             : 0;
}

uint32_t nxinput_gptk_live_context_epoch(const nxinput_gptk_live *live) {
  return live != 0 ? live->context_epoch : 0u;
}

const char *nxinput_gptk_live_context_source(const nxinput_gptk_live *live) {
  return (live != 0 && live->context_proven) ? live->context_source : 0;
}

int nxinput_gptk_live_should_consume(const nxinput_gptk_live *live,
                                     int control) {
  nxinput_gptk_decision decision;

  if (!nxinput_gptk_live_ready(live)) {
    return 0;
  }
  decision = nxinput_gptk_decide(live->map, live->context, control, 0);
  return decision == NXINPUT_GPTK_DECIDE_ACTION ||
                 decision == NXINPUT_GPTK_DECIDE_SUPPRESS
             ? 1
             : 0;
}

nxinput_gptk_live_result nxinput_gptk_live_feed(
    nxinput_gptk_live *live, int control, int pressed, float value) {
  const char *action = 0;
  nxinput_gptk_decision decision;
  uint32_t bit;
  int down;
  size_t i;
  size_t delivered = 0u;

  if (!nxinput_gptk_live_ready(live) || control < 0 ||
      control >= (int)NXINPUT_GPTK_CONTROL_COUNT ||
      live_is_vector_control(control)) {
    return NXINPUT_GPTK_LIVE_PASSTHROUGH;
  }
  decision = nxinput_gptk_decide(live->map, live->context, control, &action);
  if (decision == NXINPUT_GPTK_DECIDE_NONE ||
      decision == NXINPUT_GPTK_DECIDE_NATIVE) {
    return NXINPUT_GPTK_LIVE_PASSTHROUGH;
  }
  if (decision == NXINPUT_GPTK_DECIDE_SUPPRESS) {
    return NXINPUT_GPTK_LIVE_SUPPRESSED;
  }
  bit = UINT32_C(1) << (unsigned int)control;
  down = pressed != 0;
  if (((live->latched & bit) != 0u) == down) {
    return NXINPUT_GPTK_LIVE_DELIVERED;
  }
  for (i = 0u; i < live->sink_count; i++) {
    if (strcmp(live->sinks[i].action, action) == 0) {
      delivered++;
      if (live->sinks[i].fn(live->sinks[i].user, action, down,
                            down ? value : 0.0f) != 0) {
        live->fatal = 1;
        live->context_proven = 0;
        live->latched = 0u;
        return NXINPUT_GPTK_LIVE_FATAL;
      }
    }
  }
  if (delivered == 0u) {
    live->fatal = 1;
    live->context_proven = 0;
    live->latched = 0u;
    return NXINPUT_GPTK_LIVE_FATAL;
  }
  if (down) {
    live->latched |= bit;
  } else {
    live->latched &= ~bit;
  }
  return NXINPUT_GPTK_LIVE_DELIVERED;
}

nxinput_gptk_live_result nxinput_gptk_live_feed_vector(
    nxinput_gptk_live *live, int control, float x, float y) {
  const char *action = 0;
  nxinput_gptk_decision decision;
  size_t i;
  size_t delivered = 0u;

  if (!nxinput_gptk_live_ready(live) || !live_is_vector_control(control)) {
    return NXINPUT_GPTK_LIVE_PASSTHROUGH;
  }
  decision = nxinput_gptk_decide(live->map, live->context, control, &action);
  if (decision == NXINPUT_GPTK_DECIDE_NONE ||
      decision == NXINPUT_GPTK_DECIDE_NATIVE) {
    return NXINPUT_GPTK_LIVE_PASSTHROUGH;
  }
  if (decision == NXINPUT_GPTK_DECIDE_SUPPRESS) {
    return NXINPUT_GPTK_LIVE_SUPPRESSED;
  }
  for (i = 0u; i < live->vector_sink_count; i++) {
    if (strcmp(live->vector_sinks[i].action, action) == 0) {
      delivered++;
      if (live->vector_sinks[i].fn(live->vector_sinks[i].user, action, x, y) !=
          0) {
        live->fatal = 1;
        live->context_proven = 0;
        return NXINPUT_GPTK_LIVE_FATAL;
      }
    }
  }
  if (delivered == 0u) {
    live->fatal = 1;
    live->context_proven = 0;
    return NXINPUT_GPTK_LIVE_FATAL;
  }
  return NXINPUT_GPTK_LIVE_DELIVERED;
}

const char *nxinput_gptk_live_result_name(nxinput_gptk_live_result result) {
  switch (result) {
    case NXINPUT_GPTK_LIVE_DELIVERED:
      return "DELIVERED";
    case NXINPUT_GPTK_LIVE_SUPPRESSED:
      return "SUPPRESSED";
    case NXINPUT_GPTK_LIVE_FATAL:
      return "FATAL";
    case NXINPUT_GPTK_LIVE_PASSTHROUGH:
    default:
      return "PASSTHROUGH";
  }
}

const char *nxinput_gptk_runtime_marker(void) {
  return NXINPUT_GPTK_RUNTIME_MARKER;
}

const char *nxinput_gptk_event_evidence_schema(void) {
  return NXINPUT_GPTK_EVENT_EVIDENCE_SCHEMA;
}
