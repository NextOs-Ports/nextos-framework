/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_prerouter -- see include/nxinput_prerouter.h. Pure. */
#include "nxinput_prerouter.h"
#include <string.h>

enum { IDLE = 0, RETAINED = 1, FORWARDED = 2, CONSUMED = 3 };

int nxinput_prerouter_init(nxinput_prerouter *p, const nxinput_prerouter_ops *ops, uint64_t window_ns) {
  if (p == NULL || ops == NULL || ops->forward == NULL || ops->system_exit == NULL) return -1;
  memset(p, 0, sizeof *p);
  p->ops = *ops;
  p->window_ns = window_ns ? window_ns : NXINPUT_PREROUTER_WINDOW_NS_DEFAULT;
  if (p->window_ns > 1000000000ull) return -1; /* bounded: never above 1 s */
  p->next_edge_id = 1;
  return 0;
}

static nxinput_prerouter_slot *find_slot(nxinput_prerouter *p, int instance_id) {
  unsigned i;
  for (i = 0; i < NXINPUT_PREROUTER_MAX_INSTANCES; i++)
    if (p->slot[i].used && p->slot[i].instance_id == instance_id) return &p->slot[i];
  return NULL;
}

static nxinput_prerouter_slot *get_slot(nxinput_prerouter *p, int instance_id, uint32_t generation) {
  nxinput_prerouter_slot *s = find_slot(p, instance_id); unsigned i;
  if (s != NULL) return s;
  for (i = 0; i < NXINPUT_PREROUTER_MAX_INSTANCES; i++) {
    if (!p->slot[i].used) {
      s = &p->slot[i]; memset(s, 0, sizeof *s);
      s->used = 1; s->instance_id = instance_id; s->generation = generation;
      return s;
    }
  }
  return NULL;
}

/* Deliver the release owed by a completed tap (one tick after its press). */
static void flush_owed_release(nxinput_prerouter *p, nxinput_prerouter_slot *s, int e) {
  if (!s->release_deferred[e]) return;
  p->ops.forward(p->ops.user, s->instance_id, s->generation, (nxinput_prerouter_edge)e, 0, s->owed_release_id[e]);
  p->forwarded_release++;
  s->release_deferred[e] = 0; s->owed_release_id[e] = 0;
}

/* Flush one edge of one slot: retained press -> forward press (and owe its
 * release if it was released while retained). Exactly once. */
static void flush_edge(nxinput_prerouter *p, nxinput_prerouter_slot *s, int e) {
  if (s->state[e] != RETAINED) return;
  /* order: a release still owed by the previous tap of this edge goes out
   * before the next press of the same edge (never two presses in flight) */
  flush_owed_release(p, s, e);
  p->ops.forward(p->ops.user, s->instance_id, s->generation, (nxinput_prerouter_edge)e, 1, s->edge_id[e]);
  p->forwarded_press++;
  if (s->released_while_retained[e]) {
    /* 0.11.5 (field, FP2 03/09): a tap shorter than the window used to be
     * forwarded press+release back to back inside ONE tick; a consumer that
     * samples per frame (the padset union) saw no edge at all and START
     * never paused. The press goes out now and the release is OWED to a
     * later tick, so at least one sample observes the button down.
     * 0.11.6: "later" means a tick that starts AFTER this call -- when the
     * press is flushed from an event (double tap, SELECT tap + START), the
     * tick of the same sample must not pay the debt (2 -> 1 -> flush). */
    s->state[e] = IDLE;
    s->release_deferred[e] = (uint8_t)(p->in_tick ? 1u : 2u);
    s->owed_release_id[e] = s->edge_id[e];
    s->released_while_retained[e] = 0;
  } else {
    s->state[e] = FORWARDED;
  }
}

/* Drop one edge without forwarding (focus/unplug/reload): a forwarded press
 * gets its release; a retained press is dropped; a consumed press is
 * forgotten (its release was already swallowed by the chord). */
static void drop_edge(nxinput_prerouter *p, nxinput_prerouter_slot *s, int e) {
  if (s->state[e] == FORWARDED) {
    p->ops.forward(p->ops.user, s->instance_id, s->generation, (nxinput_prerouter_edge)e, 0, s->edge_id[e]);
    p->forwarded_release++;
  } else if (s->state[e] == RETAINED) {
    p->dropped++;
  }
  s->state[e] = IDLE; s->released_while_retained[e] = 0;
  flush_owed_release(p, s, e); /* a forwarded press always gets its release, even on focus/unplug */
}

static void slot_maybe_free(nxinput_prerouter *p, nxinput_prerouter_slot *s) {
  if (s->state[0] == IDLE && s->state[1] == IDLE && !s->release_deferred[0] && !s->release_deferred[1] && p->slot_physical[s - p->slot] == 0) s->used = 0;
}

/* ---------------- 0.11.1 D5: the physical graph ------------------------- */
static nxinput_prerouter_physical *phys_find(nxinput_prerouter *p, uint64_t physical_id) {
  unsigned i;
  for (i = 0; i < NXINPUT_PREROUTER_MAX_PHYSICAL; i++)
    if (p->physical[i].used && p->physical[i].physical_id == physical_id) return &p->physical[i];
  return NULL;
}

int nxinput_prerouter_bind_physical(nxinput_prerouter *p, int instance_id, uint32_t generation,
                                    uint64_t physical_id, nxinput_prerouter_path path, int priority, int select_certified) {
  nxinput_prerouter_physical *ph; nxinput_prerouter_slot *s; unsigned i; int slot_index = -1;
  if (p == NULL || physical_id == 0) return -1;
  ph = phys_find(p, physical_id);
  if (ph == NULL) {
    for (i = 0; i < NXINPUT_PREROUTER_MAX_PHYSICAL; i++) if (!p->physical[i].used) { ph = &p->physical[i]; break; }
    if (ph == NULL) return -1;
    memset(ph, 0, sizeof *ph); ph->used = 1; ph->physical_id = physical_id; ph->owner_instance = -1;
  }
  /* the instance keeps a slot so the binding survives idle periods */
  s = find_slot(p, instance_id);
  if (s == NULL) s = get_slot(p, instance_id, generation);
  if (s == NULL) return -1;
  if (s->generation != generation) { drop_edge(p, s, 0); drop_edge(p, s, 1); s->generation = generation; p->stale_generation_refused++; }
  slot_index = (int)(s - p->slot);
  p->slot_physical[slot_index] = physical_id;
  if (ph->owner_instance < 0 || ph->owner_instance == instance_id || priority > ph->owner_priority) {
    if (ph->owner_instance >= 0 && ph->owner_instance != instance_id) {
      /* the previous owner becomes an observer: its retained edges are dropped, never delivered twice */
      nxinput_prerouter_slot *old = find_slot(p, ph->owner_instance);
      if (old != NULL) { drop_edge(p, old, 0); drop_edge(p, old, 1); }
    }
    ph->owner_instance = instance_id; ph->owner_generation = generation; ph->owner_path = (uint8_t)path; ph->owner_priority = priority;
    ph->select_certified = (uint8_t)(select_certified ? 1u : 0u);
    return 1;
  }
  if (priority == ph->owner_priority && ph->owner_path != (uint8_t)path) p->election_ambiguous++; /* tie: first bound stays owner, counted */
  return 0;
}

int nxinput_prerouter_owner_of(const nxinput_prerouter *p, uint64_t physical_id) {
  unsigned i;
  if (p == NULL) return -1;
  for (i = 0; i < NXINPUT_PREROUTER_MAX_PHYSICAL; i++)
    if (p->physical[i].used && p->physical[i].physical_id == physical_id) return p->physical[i].owner_instance;
  return -1;
}

/* the physical record of an instance, NULL when unbound (legacy graph) */
static const nxinput_prerouter_physical *phys_of_instance(nxinput_prerouter *p, int instance_id) {
  nxinput_prerouter_slot *s = find_slot(p, instance_id); unsigned i; uint64_t pid;
  if (s == NULL) return NULL;
  pid = p->slot_physical[s - p->slot];
  if (pid == 0) return NULL;
  for (i = 0; i < NXINPUT_PREROUTER_MAX_PHYSICAL; i++)
    if (p->physical[i].used && p->physical[i].physical_id == pid) return &p->physical[i];
  return NULL;
}

void nxinput_prerouter_event(nxinput_prerouter *p, int instance_id, uint32_t generation, nxinput_prerouter_edge edge, int value, uint64_t now_ns) {
  nxinput_prerouter_slot *s; int e = (edge == NXINPUT_PREROUTER_START) ? 1 : 0, o = 1 - e;
  const nxinput_prerouter_physical *ph;
  if (p == NULL) return;
  if (value == 2) { p->repeats_ignored++; return; }
  ph = phys_of_instance(p, instance_id);
  if (ph != NULL && ph->owner_instance != instance_id) {
    /* D5: an OBSERVER path of a physical pad whose owner is another route:
     * its edges are evidence only, never delivered, never chorded. */
    p->observer_dropped++;
    return;
  }
  s = find_slot(p, instance_id);
  if (s != NULL && s->generation != generation) {
    /* the instance id was reused by a reconnected pad: the previous
     * generation's presses can never complete a chord with this one. */
    p->stale_generation_refused++;
    drop_edge(p, s, 0); drop_edge(p, s, 1);
    if (p->slot_physical[s - p->slot] != 0) { s->generation = generation; } else { s->used = 0; s = NULL; }
  }
  if (s == NULL) s = get_slot(p, instance_id, generation);
  if (s == NULL) {
    /* more instances than slots: nothing is retained, nothing is faked --
     * and the refusal is VISIBLE (5.4: never truncated in silence). The
     * edge is forwarded as an individual press/release so the pad is not
     * mute; it simply cannot chord. */
    p->overflow_refused++;
    if (value == 1) { p->ops.forward(p->ops.user, instance_id, generation, edge, 1, p->next_edge_id++); p->forwarded_press++; }
    else { p->ops.forward(p->ops.user, instance_id, generation, edge, 0, p->next_edge_id++); p->forwarded_release++; }
    return;
  }
  /* a NEW press while the previous tap's release is still owed: the new
   * press is retained (its window is at least one tick long), the owed
   * release lands on the next tick, and flush_edge keeps the order -- a fast
   * double tap loses nothing and every tap gets its own sample. */
  if (value == 1) {
    if (s->state[e] == RETAINED && s->released_while_retained[e]) {
      /* 0.11.1 (review finding 8): a DOUBLE TAP inside the window. The
       * first tap is complete (press+release retained); its press is
       * forwarded now, in order, its release is owed to a later tick
       * (0.11.6: never the tick of this same sample), and the second press
       * starts its own retention. */
      flush_edge(p, s, e);
    } else if (s->state[e] != IDLE) {
      return; /* duplicate press without release: ignored */
    }
    s->edge_id[e] = p->next_edge_id++;
    s->t_press[e] = now_ns; s->released_while_retained[e] = 0;
    if (s->state[o] == RETAINED) {
      /* 0.11.1 (review finding 8): a chord needs the other edge still
       * PHYSICALLY DOWN. A tap that was already released (retained only
       * to keep its order) never forms a chord: "SELECT tap, then START"
       * is two individual presses, not an exit. D5: and the path's SELECT
       * must be CERTIFIED by the physical profile (never guessed). */
      if (ph != NULL && !ph->select_certified) {
        p->uncertified_select_refused++;
      } else if (!s->released_while_retained[o] && now_ns - s->t_press[o] <= p->window_ns) {
        s->state[e] = CONSUMED; s->state[o] = CONSUMED;
        p->consumed += 2; p->exits++;
        p->ops.system_exit(p->ops.user, s->instance_id, s->generation, s->edge_id[o]);
        return;
      }
      flush_edge(p, s, o); /* the other edge's window is over (or it was a tap): forward it first, in order */
    }
    s->state[e] = RETAINED;
    return;
  }
  /* release */
  switch (s->state[e]) {
    case RETAINED:
      /* a tap inside the window: keep the press retained until the window
       * ends (the other edge may still arrive), then forward press+release
       * exactly once, in order. */
      s->released_while_retained[e] = 1;
      break;
    case FORWARDED:
      p->ops.forward(p->ops.user, s->instance_id, s->generation, edge, 0, s->edge_id[e]);
      p->forwarded_release++;
      s->state[e] = IDLE;
      break;
    case CONSUMED:
      s->state[e] = IDLE; /* release swallowed by the chord */
      break;
    default:
      break; /* release without press: nothing */
  }
  slot_maybe_free(p, s);
}

void nxinput_prerouter_tick(nxinput_prerouter *p, uint64_t now_ns) {
  unsigned i;
  if (p == NULL) return;
  p->in_tick = 1;
  for (i = 0; i < NXINPUT_PREROUTER_MAX_INSTANCES; i++) {
    nxinput_prerouter_slot *s = &p->slot[i];
    int first, second;
    if (!s->used) continue;
    /* releases owed by an earlier tick go out first (they belong to the
     * sample that already saw the press); a release armed inside an event
     * of THIS sample is only demoted here and paid on the next tick */
    { int e; for (e = 0; e < 2; e++) {
        if (s->release_deferred[e] == 2) s->release_deferred[e] = 1;
        else if (s->release_deferred[e] == 1) flush_owed_release(p, s, e);
    } }
    /* 0.11.1 (review): expired edges are flushed in PRESS-TIME order, not in
     * SELECT-then-START order, so "START then SELECT" both expiring in one
     * tick reach the owner in the order the user pressed them. */
    first = (s->state[0] == RETAINED && s->state[1] == RETAINED && s->t_press[1] < s->t_press[0]) ? 1 : 0;
    second = 1 - first;
    if (s->state[first] == RETAINED && now_ns - s->t_press[first] > p->window_ns) flush_edge(p, s, first);
    if (s->state[second] == RETAINED && now_ns - s->t_press[second] > p->window_ns) flush_edge(p, s, second);
    slot_maybe_free(p, s);
  }
  p->in_tick = 0;
}

void nxinput_prerouter_release_all(nxinput_prerouter *p, uint64_t now_ns) {
  unsigned i;
  (void)now_ns;
  if (p == NULL) return;
  for (i = 0; i < NXINPUT_PREROUTER_MAX_INSTANCES; i++) {
    nxinput_prerouter_slot *s = &p->slot[i];
    if (!s->used) continue;
    drop_edge(p, s, 0); drop_edge(p, s, 1);
    if (p->slot_physical[i] == 0) s->used = 0; /* bound instances keep their binding across focus loss */
  }
}

void nxinput_prerouter_unplug(nxinput_prerouter *p, int instance_id, uint64_t now_ns) {
  nxinput_prerouter_slot *s; unsigned i;
  (void)now_ns;
  if (p == NULL) return;
  s = find_slot(p, instance_id);
  if (s == NULL) return;
  drop_edge(p, s, 0); drop_edge(p, s, 1); s->used = 0;
  p->slot_physical[s - p->slot] = 0;
  for (i = 0; i < NXINPUT_PREROUTER_MAX_PHYSICAL; i++)
    if (p->physical[i].used && p->physical[i].owner_instance == instance_id) { p->physical[i].owner_instance = -1; p->physical[i].owner_priority = 0; }
}

int nxinput_prerouter_holding(const nxinput_prerouter *p, int instance_id, nxinput_prerouter_edge edge) {
  unsigned i; int e = (edge == NXINPUT_PREROUTER_START) ? 1 : 0;
  if (p == NULL) return 0;
  for (i = 0; i < NXINPUT_PREROUTER_MAX_INSTANCES; i++)
    if (p->slot[i].used && p->slot[i].instance_id == instance_id)
      return p->slot[i].state[e] == RETAINED || p->slot[i].state[e] == CONSUMED;
  return 0;
}
