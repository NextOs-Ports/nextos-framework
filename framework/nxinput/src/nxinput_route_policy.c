/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_route_policy -- see include/nxinput_route_policy.h. Pure. */
#include "nxinput_route_policy.h"
#include <stdio.h>
#include <string.h>

const char *nxinput_route_policy_route_name(nxinput_route_id route) {
  switch (route) {
    case NXINPUT_ROUTE_V5_SEAM: return "v5-seam";
    case NXINPUT_ROUTE_SDL3_PORTMASTER: return "sdl3-portmaster";
    case NXINPUT_ROUTE_PAD_ORDINAL_FIX: return "pad-ordinal-fix";
    case NXINPUT_ROUTE_GODOT_NATIVE_SEAM: return "godot-native-seam";
    default: return "?";
  }
}

int nxinput_route_policy_decide(nxinput_route_id route, const nxinput_provider_descriptor *provider,
                                nxinput_trust source_trust, int legacy_opt_in, nxinput_route_verdict *out) {
  nxinput_decision_input in; nxinput_decision d;
  if (out == NULL) return -1;
  memset(out, 0, sizeof *out);
  out->decision = NXINPUT_DECIDE_DO_NOT_MUTATE_STORE;
  if ((int)route < 0 || route >= NXINPUT_ROUTE_ID_COUNT) { snprintf(out->reason, sizeof out->reason, "unknown-route"); return -1; }
  memset(&in, 0, sizeof in);
  in.source_trust = (uint8_t)source_trust;
  switch (route) {
    case NXINPUT_ROUTE_GODOT_NATIVE_SEAM:
      in.consumer_kind = NXINPUT_CONSUMER_GODOT; in.consumer_table = NXINPUT_CTABLE_NOT_APPLICABLE;
      break;
    case NXINPUT_ROUTE_SDL3_PORTMASTER:
      in.consumer_kind = NXINPUT_CONSUMER_SDL3;
      in.consumer_table = provider != NULL && nxinput_provider_allows_rewrite(provider) && provider->evidence.api == NXINPUT_SDL_API_3 ? NXINPUT_CTABLE_PROVED : NXINPUT_CTABLE_UNKNOWN;
      out->legacy = 1;
      break;
    case NXINPUT_ROUTE_PAD_ORDINAL_FIX:
      in.consumer_kind = NXINPUT_CONSUMER_SDL2;
      in.consumer_table = provider != NULL && nxinput_provider_allows_rewrite(provider) && provider->evidence.api == NXINPUT_SDL_API_2 ? NXINPUT_CTABLE_PROVED : NXINPUT_CTABLE_UNKNOWN;
      out->legacy = 1;
      break;
    default:
      in.consumer_kind = provider != NULL && provider->evidence.api == NXINPUT_SDL_API_3 ? NXINPUT_CONSUMER_SDL3 : NXINPUT_CONSUMER_SDL2;
      in.consumer_table = provider != NULL && nxinput_provider_allows_rewrite(provider) ? NXINPUT_CTABLE_PROVED : NXINPUT_CTABLE_UNKNOWN;
      break;
  }
  if (in.consumer_table == NXINPUT_CTABLE_PROVED) {
    /* a legacy route has no integral-equivalence evidence: the machine can
     * only answer TRANSLATE_TYPED at best, never BYTE_INTACT; the legacy
     * converters ARE the typed translation of their own source. */
    in.source.domain = 1; in.consumer.domain = 2; in.source.table_digest = 1; in.consumer.table_digest = 2;
  }
  if (nxinput_decision_decide(&in, &d) != 0) { snprintf(out->reason, sizeof out->reason, "machine-refused-input"); return 0; }
  out->decision = d.decision;
  if (out->legacy && !legacy_opt_in) { out->allowed = 0; snprintf(out->reason, sizeof out->reason, "legacy-route-without-opt-in"); return 0; }
  if (route == NXINPUT_ROUTE_GODOT_NATIVE_SEAM) {
    out->allowed = (uint8_t)(d.decision == NXINPUT_DECIDE_ROUTE_TYPED_DIRECT);
    snprintf(out->reason, sizeof out->reason, out->allowed ? "typed-direct" : "source-unproven");
    return 0;
  }
  out->allowed = d.ordinal_setter_allowed;
  snprintf(out->reason, sizeof out->reason, out->allowed ? "machine-allows-setter" : d.passthrough ? "provider-unknown-stock" : "provider-or-source-unknown");
  return 0;
}

int nxinput_route_policy_receipt(nxinput_route_id route, const nxinput_route_verdict *v, char *out, size_t cap) {
  int n;
  if (v == NULL || out == NULL || cap == 0) return -1;
  n = snprintf(out, cap, "NXC6-ROUTE route=%s allowed=%u legacy=%u decision=%s reason=%s",
               nxinput_route_policy_route_name(route), (unsigned)v->allowed, (unsigned)v->legacy,
               nxinput_decision_name((nxinput_mapping_decision)v->decision), v->reason);
  return n < 0 || (size_t)n >= cap ? -1 : n;
}
