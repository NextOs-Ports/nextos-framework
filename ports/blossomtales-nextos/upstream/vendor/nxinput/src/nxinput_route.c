/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxinput_route.h"
#include <string.h>

static int out_eq(const nxinput_route_output *a, const nxinput_route_output *b) { return a->kind == b->kind && a->code == b->code && a->player == b->player; }

int nxinput_router_init(nxinput_router *r, nxinput_route_sink_fn sink, void *user, uint64_t identity_token) {
  if (!r || !sink) return -1;
  memset(r, 0, sizeof *r); r->sink = sink; r->user = user; r->identity_token = identity_token; r->mapping_generation = 1; r->context_epoch = 1; return 0;
}
void nxinput_router_lease_acquire(nxinput_router *r, uint64_t token) { if (r) { r->lease_token = token; r->lease_owned = 1; } }
void nxinput_router_lease_lost(nxinput_router *r) { if (r) { r->lease_owned = 0; nxinput_router_release_all(r, 1, 0); } }

static nxinput_route_held *find(nxinput_router *r, const nxinput_route_output *o) { unsigned i; for (i = 0; i < r->held_count; i++) if (out_eq(&r->held[i].out, o) && r->held[i].context_epoch == r->context_epoch) return &r->held[i]; return NULL; }

static int press_one(nxinput_router *r, uint64_t src, const nxinput_route_output *o) {
  nxinput_route_held *h = find(r, o); unsigned i;
  if (h) {
    for (i = 0; i < h->sources; i++) if (h->source_ids[i] == src) return 0; /* same edge twice: idempotent, no double delivery */
    /* 0.11.1 (review): a source the table cannot record must NOT count a
     * reference it can never release -- that latched the output forever. */
    if (h->sources >= 8) { r->refused_sources_full++; return -1; }
    h->source_ids[h->sources++] = src;
    h->refs++; return 0; /* OR/refcount: sink already sees it pressed */
  }
  if (r->held_count >= NXINPUT_ROUTE_MAX_HELD) return -1;
  h = &r->held[r->held_count++]; memset(h, 0, sizeof *h); h->out = *o; h->context_epoch = r->context_epoch; h->refs = 1; h->source_ids[0] = src; h->sources = 1;
  r->sink(r->user, o, 1, r->mapping_generation, r->context_epoch, src); return 1;
}

int nxinput_router_press(nxinput_router *r, uint64_t src, uint64_t source_identity, const nxinput_route_output *primary, const nxinput_route_output *comp, unsigned ncomp) {
  unsigned i; int n = 0;
  if (!r || !primary) return -1;
  if (!r->lease_owned) { r->refused_no_lease++; return -1; }
  if (source_identity == r->identity_token) { r->refused_self_source++; return -1; } /* our own virtual output re-entering as a source */
  if (ncomp > NXINPUT_ROUTE_MAX_COMPANIONS) return -1;
  for (i = 0; i < ncomp; i++) if (comp[i].kind != primary->kind) { r->refused_cross_transport++; return -1; } /* companions never cross gamepad/keyboard */
  { int k = press_one(r, src, primary); if (k < 0) return -1; n += k; }
  for (i = 0; i < ncomp; i++) { int k = press_one(r, src, &comp[i]); if (k < 0) { /* partial failure: undo the whole transaction */ nxinput_router_release(r, src, primary, comp, i); return -1; } n += k; }
  return n;
}

static int release_one(nxinput_router *r, uint64_t src, const nxinput_route_output *o) {
  nxinput_route_held *h = find(r, o); unsigned i, j;
  if (!h) return 0;
  for (i = 0; i < h->sources; i++) if (h->source_ids[i] == src) break;
  if (i == h->sources) return 0; /* this source never held it: another source's edge stays */
  for (j = i; j + 1 < h->sources; j++) h->source_ids[j] = h->source_ids[j + 1];
  h->sources--; h->refs--;
  if (h->refs > 0) return 0;
  r->sink(r->user, o, 0, r->mapping_generation, r->context_epoch, src);
  { unsigned idx = (unsigned)(h - r->held); for (j = idx; j + 1 < r->held_count; j++) r->held[j] = r->held[j + 1]; r->held_count--; }
  return 1;
}

int nxinput_router_release(nxinput_router *r, uint64_t src, const nxinput_route_output *primary, const nxinput_route_output *comp, unsigned ncomp) {
  unsigned i; int n = 0;
  if (!r || !primary) return -1;
  for (i = ncomp; i > 0; i--) n += release_one(r, src, &comp[i - 1]); /* companions release in reverse order */
  n += release_one(r, src, primary);
  return n;
}

unsigned nxinput_router_release_all(nxinput_router *r, int new_mapping_generation, int new_context_epoch) {
  unsigned n = 0;
  if (!r) return 0;
  while (r->held_count > 0) { nxinput_route_held h = r->held[r->held_count - 1]; r->held_count--; r->sink(r->user, &h.out, 0, r->mapping_generation, h.context_epoch, 0); n++; }
  if (new_mapping_generation) r->mapping_generation++;
  if (new_context_epoch) r->context_epoch++;
  return n;
}
int nxinput_router_is_held(const nxinput_router *r, const nxinput_route_output *o) { unsigned i; if (!r) return 0; for (i = 0; i < r->held_count; i++) if (out_eq(&r->held[i].out, o)) return 1; return 0; }
