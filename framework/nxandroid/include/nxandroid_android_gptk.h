/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXANDROID_ANDROID_GPTK_H
#define NXANDROID_ANDROID_GPTK_H

/* Optional compile-time bridge to the accepted nxinput GPTK V2 authority.
 * This header is not included by nxandroid itself: an Android adapter opts in,
 * links the pinned nxinput-gptk component and calls exactly this conversion. */
#include "nxandroid_android_input.h"
#include <nxinput_gptk.h>

#include <stdio.h>
#include <string.h>

#if defined(__cplusplus)
#define NXANDROID_GPTK_STATIC_ASSERT(c, m) static_assert((c), m)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define NXANDROID_GPTK_STATIC_ASSERT(c, m) _Static_assert((c), m)
#else
#define NXANDROID_GPTK_JOIN_INNER(a, b) a##b
#define NXANDROID_GPTK_JOIN(a, b) NXANDROID_GPTK_JOIN_INNER(a, b)
#define NXANDROID_GPTK_STATIC_ASSERT(c, m) \
  typedef char NXANDROID_GPTK_JOIN(nxandroid_gptk_assert_, __LINE__)[(c) ? 1 : -1]
#endif

NXANDROID_GPTK_STATIC_ASSERT((int)NXANDROID_ANDROID_CONTROL_COUNT ==
                                 (int)NXINPUT_GPTK_CONTROL_COUNT,
                             "GPTK control count changed");
NXANDROID_GPTK_STATIC_ASSERT((int)NXANDROID_ANDROID_A == (int)NXINPUT_GPTK_A,
                             "GPTK A ordinal changed");
NXANDROID_GPTK_STATIC_ASSERT((int)NXANDROID_ANDROID_R2 ==
                                 (int)NXINPUT_GPTK_R2,
                             "GPTK R2 ordinal changed");
NXANDROID_GPTK_STATIC_ASSERT((int)NXANDROID_ANDROID_SELECT ==
                                 (int)NXINPUT_GPTK_SELECT,
                             "GPTK SELECT ordinal changed");
NXANDROID_GPTK_STATIC_ASSERT((int)NXANDROID_ANDROID_RIGHT_STICK ==
                                 (int)NXINPUT_GPTK_RIGHT_STICK,
                             "GPTK right-stick ordinal changed");
NXANDROID_GPTK_STATIC_ASSERT((int)NXANDROID_ANDROID_CONTEXT_COUNT ==
                                 (int)NXINPUT_GPTK_CONTEXT_COUNT,
                             "GPTK context count changed");

/* Calls nxinput_gptk_decide exactly once for each (context, control) pair.
 * V1 is rejected: Android C7 requires explicit complete tri-state authority.
 * The output is cleared on every failure. */
static inline nxandroid_android_result nxandroid_android_authority_from_gptk(
    const nxinput_gptk *map, nxandroid_android_authority *out, char *error,
    size_t error_size) {
  int context;
  int control;

  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (map == NULL || out == NULL ||
      map->api_version != NXINPUT_GPTK_API_VERSION ||
      map->schema_version != NXINPUT_GPTK_SCHEMA_V2) {
    if (error != NULL && error_size > 0u)
      snprintf(error, error_size,
               "Android GPTK bridge requires one parsed V2 authority");
    return NXANDROID_ANDROID_EINVAL;
  }

  out->api_version = NXANDROID_ANDROID_INPUT_API_VERSION;
  out->schema_version = map->schema_version;
  for (context = 0; context < (int)NXANDROID_ANDROID_CONTEXT_COUNT;
       ++context) {
    out->context_present[context] =
        (uint8_t)(map->context_present[context] != 0);
    for (control = 0; control < (int)NXANDROID_ANDROID_CONTROL_COUNT;
         ++control) {
      const char *action = NULL;
      nxinput_gptk_decision decision = nxinput_gptk_decide(
          map, (nxinput_gptk_context)context, control, &action);

      switch (decision) {
      case NXINPUT_GPTK_DECIDE_NONE:
        out->decision[context][control] = NXANDROID_ANDROID_DECIDE_NONE;
        break;
      case NXINPUT_GPTK_DECIDE_ACTION:
        if (action == NULL || action[0] == '\0' ||
            strlen(action) >= NXANDROID_ANDROID_ACTION_MAX) {
          memset(out, 0, sizeof(*out));
          if (error != NULL && error_size > 0u)
            snprintf(error, error_size,
                     "Android GPTK action is empty or oversized");
          return NXANDROID_ANDROID_EINVAL;
        }
        out->decision[context][control] = NXANDROID_ANDROID_DECIDE_ACTION;
        memcpy(out->action[context][control], action, strlen(action) + 1u);
        break;
      case NXINPUT_GPTK_DECIDE_SUPPRESS:
        out->decision[context][control] = NXANDROID_ANDROID_DECIDE_SUPPRESS;
        break;
      case NXINPUT_GPTK_DECIDE_NATIVE:
        out->decision[context][control] = NXANDROID_ANDROID_DECIDE_NATIVE;
        break;
      default:
        memset(out, 0, sizeof(*out));
        if (error != NULL && error_size > 0u)
          snprintf(error, error_size, "Android GPTK decision is unknown");
        return NXANDROID_ANDROID_EINVAL;
      }
    }
  }
  return NXANDROID_ANDROID_OK;
}

#undef NXANDROID_GPTK_STATIC_ASSERT
#undef NXANDROID_GPTK_JOIN
#undef NXANDROID_GPTK_JOIN_INNER

#endif
