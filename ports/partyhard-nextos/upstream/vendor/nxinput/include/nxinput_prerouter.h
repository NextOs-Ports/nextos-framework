/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_PREROUTER_H
#define NXINPUT_PREROUTER_H
/*
 * nxinput_prerouter -- V5 (0.11.0), mission 5.6 / D7: the SOVEREIGN
 * START/SELECT PRE-ROUTER, outside PORT_AUTHORITY_MODE.
 *
 * START and SELECT edges of ONE physical instance are RETAINED for a short,
 * bounded, documented window. If the same instance completes SELECT+START
 * inside the window, both presses (and their releases) are CONSUMED and
 * exactly ONE `system.exit` request is emitted -- START never pauses first.
 * If the window ends without the chord, the individual press is FORWARDED
 * exactly once, in order, and its release follows exactly once. Focus loss,
 * unplug, timeout of a released tap, reload and error release any retention
 * without latch: a press that was never forwarded is dropped, a press that
 * was forwarded gets its release. Pads never mix: SELECT on one instance and
 * START on another do not chord (ever), and a reconnected pad that reuses an
 * instance id carries a new device_instance_generation, so a press from the
 * previous generation cannot complete a chord with the new one. Auto-repeat
 * (value 2) is ignored. L2+R2 is not this router's business: it never exits.
 *
 * 0.11.1 (review finding 8): a chord needs BOTH edges physically down --
 * a SELECT tap followed by a START press inside the window is two
 * individual presses, never an exit; a double tap of one edge forwards
 * both taps; expired edges flush in press-time order; an instance beyond
 * the slot table is forwarded individually and COUNTED (overflow_refused).
 *
 * While an edge is retained the owner resolution says SUPPRESSED
 * (nxinput_decision_edge_owner with chord_hold=1).
 *
 * Pure: time is a monotonic value the caller passes; outputs are callbacks.
 */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_PREROUTER_WINDOW_NS_DEFAULT 180000000ull /* 180 ms, documented */
#define NXINPUT_PREROUTER_MAX_INSTANCES 8u

typedef enum nxinput_prerouter_edge { NXINPUT_PREROUTER_SELECT = 0, NXINPUT_PREROUTER_START = 1 } nxinput_prerouter_edge;

typedef struct nxinput_prerouter_ops {
  void *user;
  /* Forward an individual edge to the owner router (exactly once per press/release). */
  void (*forward)(void *user, int instance_id, uint32_t device_instance_generation, nxinput_prerouter_edge edge, int pressed, uint64_t physical_edge_id);
  /* Exactly one exit request per completed chord. */
  void (*system_exit)(void *user, int instance_id, uint32_t device_instance_generation, uint64_t physical_edge_id);
} nxinput_prerouter_ops;

/* 0.11.1 (D5): the PHYSICAL GRAPH behind the chord. Raw evdev and SDL do
 * not share an event id: an instance is a TRANSPORT PATH to a physical pad.
 * Before any edge is delivered, ONE ingestion owner path is elected per
 * physical_device_id (declared priority, first bound wins a tie); the other
 * paths are OBSERVERS: their SELECT/START edges are counted and dropped, so
 * two routes can never produce two exits or two pauses. The chord also
 * needs the SELECT edge CERTIFIED by the independent physical profile: an
 * unknown provider never guesses which button is SELECT -- an uncertified
 * SELECT is forwarded as an individual press and never chords. */
typedef enum nxinput_prerouter_path {
  NXINPUT_PREROUTER_PATH_SDL2 = 0, NXINPUT_PREROUTER_PATH_SDL3, NXINPUT_PREROUTER_PATH_RAW_EVDEV,
  NXINPUT_PREROUTER_PATH_ENGINE_DIRECT
} nxinput_prerouter_path;
#define NXINPUT_PREROUTER_MAX_PHYSICAL 8u
typedef struct nxinput_prerouter_physical {
  uint8_t used; uint64_t physical_id; int owner_instance; uint32_t owner_generation; uint8_t owner_path; int owner_priority; uint8_t select_certified;
} nxinput_prerouter_physical;

typedef struct nxinput_prerouter_slot {
  int instance_id; uint32_t generation; uint8_t used;
  /* per edge: 0 idle, 1 retained (pressed, not forwarded), 2 forwarded (pressed), 3 consumed by chord (pressed) */
  uint8_t state[2]; uint64_t t_press[2]; uint64_t edge_id[2]; uint8_t released_while_retained[2];
  /* 0.11.5/0.11.6: a completed tap flushes its press now and OWES its release
   * to a LATER tick, so at least one per-frame sample sees the button down.
   * release_deferred: 0 none, 1 flush on the next tick, 2 armed inside an
   * event (the tick of the same sample only demotes it to 1). The owed
   * release is independent of state[] (the edge may already be retaining a
   * new press) and carries the id of the press it closes. */
  uint8_t release_deferred[2]; uint64_t owed_release_id[2];
} nxinput_prerouter_slot;

typedef struct nxinput_prerouter {
  nxinput_prerouter_ops ops; uint64_t window_ns; uint64_t next_edge_id;
  nxinput_prerouter_slot slot[NXINPUT_PREROUTER_MAX_INSTANCES];
  nxinput_prerouter_physical physical[NXINPUT_PREROUTER_MAX_PHYSICAL];
  /* per instance slot: the physical binding (0 = unbound: legacy per-instance graph) */
  uint64_t slot_physical[NXINPUT_PREROUTER_MAX_INSTANCES];
  unsigned exits, forwarded_press, forwarded_release, consumed, dropped, cross_pad_refused, stale_generation_refused, repeats_ignored;
  unsigned observer_dropped, uncertified_select_refused, election_ambiguous; /* 0.11.1 D5 */
  unsigned overflow_refused; /* 0.11.1: edges of instances beyond the slot table (forwarded individually, never chord) */
  uint8_t in_tick; /* 0.11.6: a release owed from inside tick() needs one tick; from an event, a full tick must pass first */
} nxinput_prerouter;

int nxinput_prerouter_init(nxinput_prerouter *p, const nxinput_prerouter_ops *ops, uint64_t window_ns);
/* One evdev-like event. value: 1 press, 0 release, 2 auto-repeat. */
void nxinput_prerouter_event(nxinput_prerouter *p, int instance_id, uint32_t device_instance_generation, nxinput_prerouter_edge edge, int value, uint64_t now_ns);
/* Advance time: flushes retained edges whose window ended. */
void nxinput_prerouter_tick(nxinput_prerouter *p, uint64_t now_ns);
/* Focus lost / reload / error: release everything (forward releases for
 * forwarded presses, drop retained ones). */
void nxinput_prerouter_release_all(nxinput_prerouter *p, uint64_t now_ns);
/* Unplug of one instance: same as above for that instance only, slot freed. */
void nxinput_prerouter_unplug(nxinput_prerouter *p, int instance_id, uint64_t now_ns);
/* 0.11.1 (D5): bind a transport instance to a physical device and elect the
 * ingestion owner for that physical id. `priority` is the adapter's declared
 * provider priority (higher wins; equal = the first bound path stays owner).
 * `select_certified` = the independent physical profile certifies which
 * edge is SELECT on this path (0 => that path never chords). Returns 1 when
 * this instance is (now) the owner, 0 when it is an observer, -1 on
 * overflow. Rebinding an instance under a NEW generation re-runs the
 * election (a reconnected pad may change owner). */
int nxinput_prerouter_bind_physical(nxinput_prerouter *p, int instance_id, uint32_t device_instance_generation,
                                    uint64_t physical_id, nxinput_prerouter_path path, int priority, int select_certified);
/* The owner instance of a physical id (-1 when none bound). */
int nxinput_prerouter_owner_of(const nxinput_prerouter *p, uint64_t physical_id);
/* Is this instance currently retaining or consuming an edge? (owner => SUPPRESSED) */
int nxinput_prerouter_holding(const nxinput_prerouter *p, int instance_id, nxinput_prerouter_edge edge);
#ifdef __cplusplus
}
#endif
#endif
