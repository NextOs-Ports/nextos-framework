/* SPDX-License-Identifier: GPL-3.0-only */
/* V5 / F1,F4-F6, E8: one primary route, refcount, companions, lease, release-all. */
#include "../../include/nxinput_route.h"
#include <stdio.h>
#include <string.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static char trace[512];
static void sink(void *u, const nxinput_route_output *o, int p, uint32_t g, uint32_t e, uint64_t src) { char b[48]; (void)u; snprintf(b, sizeof b, "%s%u/%u%c ", o->kind == NXINPUT_ROUTE_SDL_KEYBOARD ? "K" : "G", (unsigned)o->code, (unsigned)e, p ? '+' : '-'); (void)g; (void)src; strcat(trace, b); }
int main(void) {
  nxinput_router r; nxinput_route_output A = {NXINPUT_ROUTE_ANDROID_KEYEVENT, 96, 0}, K = {NXINPUT_ROUTE_SDL_KEYBOARD, 44, 0}, B = {NXINPUT_ROUTE_ANDROID_KEYEVENT, 97, 0};
  nxinput_router_init(&r, sink, NULL, 0xFEEDu);
  CHECK(nxinput_router_press(&r, 1, 1, &A, NULL, 0) == -1 && r.refused_no_lease == 1, "no lease: nothing emitted");
  nxinput_router_lease_acquire(&r, 0x1234u);
  trace[0] = 0; CHECK(nxinput_router_press(&r, 1, 1, &A, NULL, 0) == 1 && !strcmp(trace, "G96/1+ "), "press delivers exactly one sink press");
  CHECK(nxinput_router_press(&r, 1, 1, &A, NULL, 0) == 0, "same edge pressed twice: idempotent, no double delivery");
  CHECK(nxinput_router_press(&r, 2, 2, &A, NULL, 0) == 0, "second source (keyboard) holding the same output: refcount, no second press");
  trace[0] = 0; nxinput_router_release(&r, 1, &A, NULL, 0); CHECK(!strcmp(trace, "") && nxinput_router_is_held(&r, &A), "releasing one source keeps the other's hold");
  trace[0] = 0; nxinput_router_release(&r, 2, &A, NULL, 0); CHECK(!strcmp(trace, "G96/1- "), "last source releases: exactly one sink release");
  /* one edge -> gamepad + keyboard without a companion: cross-transport refused */
  CHECK(nxinput_router_press(&r, 3, 3, &A, &K, 1) == -1 && r.refused_cross_transport == 1 && !nxinput_router_is_held(&r, &A), "gamepad + keyboard companion refused (no cross-transport, nothing left held)");
  trace[0] = 0; CHECK(nxinput_router_press(&r, 4, 4, &A, &B, 1) == 2 && !strcmp(trace, "G96/1+ G97/1+ "), "explicit same-transport companion: ordered presses");
  trace[0] = 0; nxinput_router_release(&r, 4, &A, &B, 1); CHECK(!strcmp(trace, "G97/1- G96/1- "), "companions release in reverse order");
  CHECK(nxinput_router_press(&r, 5, 0xFEEDu, &A, NULL, 0) == -1 && r.refused_self_source == 1, "our own virtual output re-entering as a source is refused");
  nxinput_router_press(&r, 6, 6, &A, NULL, 0); nxinput_router_press(&r, 7, 7, &K, NULL, 0);
  trace[0] = 0; CHECK(nxinput_router_release_all(&r, 1, 1) == 2 && strstr(trace, "K44/1-") && strstr(trace, "G96/1-") && r.mapping_generation == 2 && r.context_epoch == 2, "release-all drops every held output once and bumps generation/epoch");
  nxinput_router_press(&r, 8, 8, &A, NULL, 0); trace[0] = 0; nxinput_router_lease_lost(&r); CHECK(!strcmp(trace, "G96/2- ") && nxinput_router_press(&r, 9, 9, &A, NULL, 0) == -1, "lease lost: held outputs released, further emission refused");
  /* 0.11.1 (review): 9 sources on one output -- the 9th is refused, and the
   * output releases when the 8 recorded sources release (never latched). */
  { nxinput_router q; uint64_t k; nxinput_router_init(&q, sink, NULL, 0xFEEDu); nxinput_router_lease_acquire(&q, 1);
    for (k = 1; k <= 8; k++) nxinput_router_press(&q, 100 + k, 100 + k, &A, NULL, 0);
    CHECK(nxinput_router_press(&q, 200, 200, &A, NULL, 0) == -1 && q.refused_sources_full == 1, "MUTANT killed: 9th source counted without being recorded (refused instead)");
    trace[0] = 0; for (k = 1; k <= 8; k++) nxinput_router_release(&q, 100 + k, &A, NULL, 0);
    CHECK(!strcmp(trace, "G96/1- ") && !nxinput_router_is_held(&q, &A), "output released once when the recorded sources release (no latch)"); }
  printf(fails ? "v5-route: FAIL\n" : "v5-route: OK\n"); return fails ? 1 : 0;
}
