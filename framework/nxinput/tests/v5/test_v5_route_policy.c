/* SPDX-License-Identifier: GPL-3.0-only */
/* 0.11.1 C6: one policy in front of every setter. Mutants: legacy route
 * calling its setter without opt-in; SDL3 route rewriting under an unknown
 * provider; Godot native seam forced onto an ordinal table; the V5 seam
 * under UNKNOWN never allowed a translated setter. */
#include "../../include/nxinput_route_policy.h"
#include <stdio.h>
#include <string.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
int main(void) {
  nxinput_provider_descriptor p2, p3, un; nxinput_route_verdict v; char line[200];
  memset(&p2, 0, sizeof p2); p2.evidence.api = NXINPUT_SDL_API_2; p2.method = NXINPUT_PROVIDER_METHOD_MEASURED_INPROCESS; p2.domain = NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED;
  memset(&p3, 0, sizeof p3); p3.evidence.api = NXINPUT_SDL_API_3; p3.method = NXINPUT_PROVIDER_METHOD_PINNED_ELF; p3.domain = NXINPUT_SDL_DOMAIN_SDL3_EVDEV;
  memset(&un, 0, sizeof un); un.evidence.api = NXINPUT_SDL_API_2; un.method = NXINPUT_PROVIDER_METHOD_UNKNOWN; un.domain = NXINPUT_SDL_DOMAIN_UNDECLARED;
  CHECK(nxinput_route_policy_decide(NXINPUT_ROUTE_V5_SEAM, &p2, NXINPUT_TRUST_PROVED, 0, &v) == 0 && v.allowed && !v.legacy, "V5 seam, proved source + measured provider: setter allowed");
  CHECK(nxinput_route_policy_decide(NXINPUT_ROUTE_V5_SEAM, &un, NXINPUT_TRUST_PROVED, 0, &v) == 0 && !v.allowed && v.decision == NXINPUT_DECIDE_DO_NOT_MUTATE_STORE, "V5 seam under an UNKNOWN provider: no translated setter (stock mode elsewhere)");
  CHECK(nxinput_route_policy_decide(NXINPUT_ROUTE_V5_SEAM, NULL, NXINPUT_TRUST_PROVED, 0, &v) == 0 && !v.allowed, "V5 seam with no descriptor at all: refused");
  CHECK(nxinput_route_policy_decide(NXINPUT_ROUTE_SDL3_PORTMASTER, &p3, NXINPUT_TRUST_PROVED, 0, &v) == 0 && !v.allowed && v.legacy && !strcmp(v.reason, "legacy-route-without-opt-in"), "MUTANT killed: legacy SDL3/PortMaster route calling SDL_SetGamepadMapping without the explicit opt-in");
  CHECK(nxinput_route_policy_decide(NXINPUT_ROUTE_SDL3_PORTMASTER, &p3, NXINPUT_TRUST_PROVED, 1, &v) == 0 && v.allowed && v.legacy, "opt-in + pinned SDL3 provider + proved source: legacy route allowed (typed)");
  CHECK(nxinput_route_policy_decide(NXINPUT_ROUTE_SDL3_PORTMASTER, &un, NXINPUT_TRUST_PROVED, 1, &v) == 0 && !v.allowed, "MUTANT killed: SDL3 route rewriting under an unknown provider even with opt-in");
  CHECK(nxinput_route_policy_decide(NXINPUT_ROUTE_SDL3_PORTMASTER, &p2, NXINPUT_TRUST_PROVED, 1, &v) == 0 && !v.allowed, "an SDL2 provider never authorizes the SDL3 route (5.7)");
  CHECK(nxinput_route_policy_decide(NXINPUT_ROUTE_PAD_ORDINAL_FIX, &p2, NXINPUT_TRUST_PROVED, 0, &v) == 0 && !v.allowed && v.legacy, "MUTANT killed: V3 ordinal fix calling AddMapping without opt-in");
  CHECK(nxinput_route_policy_decide(NXINPUT_ROUTE_PAD_ORDINAL_FIX, &p2, NXINPUT_TRUST_UNKNOWN, 1, &v) == 0 && !v.allowed, "ordinal fix with an unproven source: refused even with opt-in");
  CHECK(nxinput_route_policy_decide(NXINPUT_ROUTE_GODOT_NATIVE_SEAM, NULL, NXINPUT_TRUST_PROVED, 0, &v) == 0 && v.allowed && v.decision == NXINPUT_DECIDE_ROUTE_TYPED_DIRECT, "Godot native seam: typed direct route, no ordinal table");
  CHECK(nxinput_route_policy_decide(NXINPUT_ROUTE_GODOT_NATIVE_SEAM, NULL, NXINPUT_TRUST_UNKNOWN, 0, &v) == 0 && !v.allowed, "Godot native seam with an unproven line: refused");
  CHECK(nxinput_route_policy_decide((nxinput_route_id)99, NULL, NXINPUT_TRUST_PROVED, 1, &v) == -1 && !v.allowed, "unknown route: refused");
  CHECK(nxinput_route_policy_receipt(NXINPUT_ROUTE_SDL3_PORTMASTER, &v, line, sizeof line) > 0 && strstr(line, "NXC6-ROUTE route=sdl3-portmaster"), "receipt line");
  printf(fails ? "v5-route-policy: FAIL\n" : "v5-route-policy: OK\n"); return fails ? 1 : 0;
}
