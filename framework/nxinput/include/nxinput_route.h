/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_ROUTE_H
#define NXINPUT_ROUTE_H
/*
 * nxinput_route -- V5 (mission 5, 6.6, F1-F6): the OUTPUT ROUTER.
 *
 * One physical edge -> exactly ONE primary typed route -> one sink. Multiple
 * legitimate sources may hold the same output: the router refcounts per
 * (route, output, player, context_epoch) and the sink sees ONE press and ONE
 * release. Companions are explicit, ordered, same-transport members of the
 * same transaction; nothing is broadcast. Release-all on generation change,
 * focus loss, unplug, context switch, reload or fatal drops every held
 * output exactly once. A cooperative LEASE token guards the emitter: a second
 * cooperative emitter must lose the lease before it may emit.
 *
 * Pure: sinks are callbacks; no threads, no I/O.
 */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum nxinput_route_kind {
  NXINPUT_ROUTE_ENGINE_DIRECT = 0, NXINPUT_ROUTE_GODOT_INPUTMAP, NXINPUT_ROUTE_ANDROID_KEYEVENT,
  NXINPUT_ROUTE_ANDROID_MOTION, NXINPUT_ROUTE_ENGINE_TOUCH, NXINPUT_ROUTE_SDL_POINTER,
  NXINPUT_ROUTE_SDL_GAMEPAD, NXINPUT_ROUTE_SDL_KEYBOARD, NXINPUT_ROUTE_UINPUT_KEYBOARD,
  NXINPUT_ROUTE_KIND_COUNT
} nxinput_route_kind;

#define NXINPUT_ROUTE_MAX_HELD 128u
#define NXINPUT_ROUTE_MAX_COMPANIONS 4u

typedef struct nxinput_route_output {
  uint8_t kind;             /* nxinput_route_kind */
  uint32_t code;            /* keycode / action id hash / pointer id */
  uint8_t player;
} nxinput_route_output;

/* Sink: pressed=1/0 for digital; the receipt carries the edge ids. */
typedef void (*nxinput_route_sink_fn)(void *user, const nxinput_route_output *out, int pressed,
                                      uint32_t mapping_generation, uint32_t context_epoch, uint64_t source_edge_id);

typedef struct nxinput_route_held {
  nxinput_route_output out; uint32_t context_epoch; unsigned refs;
  uint64_t source_ids[8]; unsigned sources; /* who holds it (source_edge_id per source) */
} nxinput_route_held;

typedef struct nxinput_router {
  nxinput_route_sink_fn sink; void *user;
  uint32_t mapping_generation, context_epoch;
  nxinput_route_held held[NXINPUT_ROUTE_MAX_HELD]; unsigned held_count;
  uint64_t lease_token; uint8_t lease_owned;
  uint64_t identity_token;  /* our own virtual output identity: never a source */
  unsigned refused_no_lease, refused_self_source, refused_cross_transport;
  unsigned refused_sources_full; /* 0.11.1: a 9th source on one output is refused, never silently counted */
} nxinput_router;

int nxinput_router_init(nxinput_router *r, nxinput_route_sink_fn sink, void *user, uint64_t identity_token);
/* Cooperative lease: the emitter emits only while it owns `token`. */
void nxinput_router_lease_acquire(nxinput_router *r, uint64_t token);
void nxinput_router_lease_lost(nxinput_router *r);
/* A press from source_edge_id (unique per physical edge) on the primary
 * output plus optional companions (same transport kind). Returns the number
 * of sink presses actually emitted (0 when already held by another source or
 * refused), -1 when refused (no lease, self-source, cross-transport). */
int nxinput_router_press(nxinput_router *r, uint64_t source_edge_id, uint64_t source_identity,
                         const nxinput_route_output *primary, const nxinput_route_output *companions, unsigned ncomp);
int nxinput_router_release(nxinput_router *r, uint64_t source_edge_id, const nxinput_route_output *primary,
                           const nxinput_route_output *companions, unsigned ncomp);
/* Release EVERYTHING held, once each, then bump the generation/epoch. */
unsigned nxinput_router_release_all(nxinput_router *r, int new_mapping_generation, int new_context_epoch);
int nxinput_router_is_held(const nxinput_router *r, const nxinput_route_output *out);
#ifdef __cplusplus
}
#endif
#endif
