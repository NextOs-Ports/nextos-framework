/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_ROUTE_POLICY_H
#define NXINPUT_ROUTE_POLICY_H
/*
 * nxinput_route_policy -- 0.11.1 (C6): ONE policy in front of EVERY setter.
 *
 * Before 0.11.1 three routes could hand a mapping line to an SDL setter on
 * their own terms: the V5 seam (nxinput_sdl_seam + nxc6_glue, through the
 * 1.4 machine), the SDL3/PortMaster coordinator (nxinput_sdl3_portmaster,
 * its own joydev->SDL3 conversion) and the V3-era ordinal fix
 * (nxinput_pad_ordinal_fix.h, VID/PID + bus signature). Two authorities
 * for the same store is exactly what mission 1.4 forbids. This module is
 * the single gate: a setter call is allowed ONLY when the 1.4 machine says
 * so for the route's consumer kind and the resolved provider descriptor.
 * Routes that cannot feed the machine (no provider descriptor) are LEGACY:
 * they run only under an explicit opt-in and every refusal is counted and
 * receipted. `tests/v5/legacy_routes_gate.py` proves that every file that
 * names a setter goes through this module or carries the LEGACY marker.
 *
 * Pure: no I/O; the opt-in is a value the adapter passes, never getenv here.
 */
#include "nxinput_decision.h"
#include "nxinput_provider.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_ROUTE_LEGACY_MARKER "NXINPUT-LEGACY-ROUTE"

typedef enum nxinput_route_id {
  NXINPUT_ROUTE_V5_SEAM = 0,        /* nxinput_sdl_seam via nxc6_glue: the machine route */
  NXINPUT_ROUTE_SDL3_PORTMASTER,    /* nxinput_sdl3_portmaster: LEGACY (opt-in) */
  NXINPUT_ROUTE_PAD_ORDINAL_FIX,    /* nxinput_pad_ordinal_fix.h: LEGACY (opt-in) */
  NXINPUT_ROUTE_GODOT_NATIVE_SEAM,  /* nxinput_godot_seam: consumer GODOT, typed direct */
  NXINPUT_ROUTE_ID_COUNT
} nxinput_route_id;

typedef struct nxinput_route_verdict {
  uint8_t allowed;          /* may the setter be called with this line? */
  uint8_t legacy;           /* the route is a legacy route */
  uint8_t decision;         /* nxinput_mapping_decision that decided */
  char reason[48];
} nxinput_route_verdict;

/* Decide whether `route` may call its setter. `provider` may be NULL
 * (unknown); `source_trust` is the route's own proof of the line's domain;
 * `legacy_opt_in` is the adapter's explicit opt-in for legacy routes (the
 * launcher's NXINPUT_LEGACY_ROUTES=allow, passed as a value). Returns 0. */
int nxinput_route_policy_decide(nxinput_route_id route, const nxinput_provider_descriptor *provider,
                                nxinput_trust source_trust, int legacy_opt_in, nxinput_route_verdict *out);
const char *nxinput_route_policy_route_name(nxinput_route_id route);
/* Bounded receipt line "NXC6-ROUTE route=... allowed=... legacy=... decision=... reason=..." */
int nxinput_route_policy_receipt(nxinput_route_id route, const nxinput_route_verdict *v, char *out, size_t cap);
#ifdef __cplusplus
}
#endif
#endif
