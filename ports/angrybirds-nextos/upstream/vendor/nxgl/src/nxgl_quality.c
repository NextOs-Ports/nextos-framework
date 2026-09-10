/* SPDX-License-Identifier: GPL-3.0-only */
/* nxgl_quality -- see include/nxgl_quality.h. Pure: no EGL, no I/O, no clock. */
#include "nxgl_quality.h"

#include <stdio.h>
#include <string.h>

static int nxq_concrete(nxgl_quality_level level) {
  return level == NXGL_QUALITY_LOW || level == NXGL_QUALITY_MEDIUM ||
         level == NXGL_QUALITY_HIGH;
}

nxgl_quality_level nxgl_quality_parse(const char *value) {
  if (value == NULL) {
    return NXGL_QUALITY_AUTO;
  }
  if (strcmp(value, "low") == 0) {
    return NXGL_QUALITY_LOW;
  }
  if (strcmp(value, "medium") == 0) {
    return NXGL_QUALITY_MEDIUM;
  }
  if (strcmp(value, "high") == 0) {
    return NXGL_QUALITY_HIGH;
  }
  /* "auto" and every unknown value fail safe to AUTO. */
  return NXGL_QUALITY_AUTO;
}

const char *nxgl_quality_name(nxgl_quality_level level) {
  switch (level) {
    case NXGL_QUALITY_LOW:
      return "low";
    case NXGL_QUALITY_MEDIUM:
      return "medium";
    case NXGL_QUALITY_HIGH:
      return "high";
    case NXGL_QUALITY_AUTO:
    case NXGL_QUALITY_LEVEL_COUNT:
    default:
      return "auto";
  }
}

nxgl_quality_level nxgl_quality_resolve(nxgl_quality_level requested,
                                        nxgl_quality_level recommended) {
  if (nxq_concrete(requested)) {
    return requested;
  }
  /* requested is AUTO (or out of range): defer to the adapter's measured
   * recommendation; if that is not concrete either, floor to MEDIUM. */
  if (nxq_concrete(recommended)) {
    return recommended;
  }
  return NXGL_QUALITY_MEDIUM;
}

int nxgl_quality_state_init(nxgl_quality_state *s,
                            nxgl_quality_level requested,
                            nxgl_quality_level recommended) {
  if (s == NULL) {
    return -1;
  }
  s->api_version = NXGL_QUALITY_API_VERSION;
  s->struct_size = sizeof(*s);
  s->requested = requested;
  s->resolved = nxgl_quality_resolve(requested, recommended);
  s->applied = 0;
  s->ready = 0;
  return 0;
}

int nxgl_quality_state_apply(nxgl_quality_state *s) {
  if (s == NULL || !nxq_concrete(s->resolved)) {
    return -1;
  }
  s->applied = 1;
  return 0;
}

int nxgl_quality_state_ready(nxgl_quality_state *s) {
  if (s == NULL || !s->applied) {
    return -1; /* order is enforced: apply before ready */
  }
  s->ready = 1;
  return 0;
}

size_t nxgl_quality_receipt(const nxgl_quality_state *s,
                            nxgl_quality_stage stage,
                            char *buf, size_t cap) {
  const char *stage_name;
  int written;

  if (buf == NULL || cap == 0u) {
    return 0u;
  }
  buf[0] = '\0';
  if (s == NULL || stage >= NXGL_QUALITY_STAGE_COUNT) {
    return 0u;
  }
  switch (stage) {
    case NXGL_QUALITY_STAGE_RESOLVE:
      stage_name = "resolve";
      break;
    case NXGL_QUALITY_STAGE_APPLY:
      stage_name = "apply";
      break;
    case NXGL_QUALITY_STAGE_READY:
      stage_name = "ready";
      break;
    case NXGL_QUALITY_STAGE_COUNT:
    default:
      return 0u;
  }
  written = snprintf(buf, cap,
                     "QUALITY: stage=%s requested=%s resolved=%s applied=%d "
                     "ready=%d",
                     stage_name, nxgl_quality_name(s->requested),
                     nxgl_quality_name(s->resolved), s->applied ? 1 : 0,
                     s->ready ? 1 : 0);
  if (written < 0 || (size_t)written >= cap) {
    buf[0] = '\0';
    return 0u;
  }
  return (size_t)written;
}
