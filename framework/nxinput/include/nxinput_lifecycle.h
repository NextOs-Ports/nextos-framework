/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_LIFECYCLE_H
#define NXINPUT_LIFECYCLE_H
/*
 * nxinput_lifecycle -- V5 (0.11.0), mission 5.4 / D4, E5a, E7, E13:
 * ONE central lifecycle for every input source of a port.
 *
 *   admit -> open (instance re-checked) -> events -> remove/unplug
 *   reconnect reusing an instance id  => new device_instance_generation
 *   focus lost / fatal / reload / context change => release-all of the
 *   previous epoch, then the epoch is incremented, then a NEUTRAL GATE:
 *   no edge is delivered to ANY owner until every involved source is
 *   neutral again. `unknown/loading/unproven` contexts never switch to a
 *   passthrough during a hold: only the adapter's DECLARED fallback
 *   activates, after the gate.
 *
 *   hot reload (E7): release-all -> parse the new owner text -> on success
 *   publish mapping_generation+1 in the safe frame; on failure keep the
 *   last valid generation (the owner's bytes are never touched here).
 *
 *   synchronized (E13): a CAS transaction with a lease and two sides:
 *   owner_reload  = freeze edges, release holds, apply to the engine,
 *                   READBACK, publish only if readback == requested; else
 *                   rollback the engine, keep the generation, report;
 *   engine_rebind = the user's explicit UI gesture; owner file CAS on its
 *                   digest: if the owner changed meanwhile => CONFLICT,
 *                   nothing written, nothing published.
 *   Neither side is ever published alone.
 *
 * Pure: all effects are callbacks; storage bounded.
 */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_LC_MAX_DEVICES 8u
#define NXINPUT_LC_MAX_EDGES 32u

typedef enum nxinput_lc_context {
  NXINPUT_LC_CTX_UNKNOWN = 0, NXINPUT_LC_CTX_LOADING, NXINPUT_LC_CTX_UNPROVEN,
  NXINPUT_LC_CTX_TITLE, NXINPUT_LC_CTX_MENU, NXINPUT_LC_CTX_GAMEPLAY, NXINPUT_LC_CTX_PAUSE
} nxinput_lc_context;
/* what the adapter DECLARES for the unknown/loading/unproven contexts */
typedef enum nxinput_lc_fallback { NXINPUT_LC_FALLBACK_SUPPRESS = 0, NXINPUT_LC_FALLBACK_ENGINE_NATIVE = 1 } nxinput_lc_fallback;
typedef enum nxinput_lc_owner { NXINPUT_LC_OWNER_NONE = 0, NXINPUT_LC_OWNER_ACTIONS, NXINPUT_LC_OWNER_ENGINE_NATIVE } nxinput_lc_owner;

typedef struct nxinput_lc_device {
  uint8_t used, state;             /* state: 0 admitted, 1 open, 2 quarantined */
  int instance_id; uint64_t physical_id; uint32_t generation;
  uint32_t held[NXINPUT_LC_MAX_EDGES / 32]; /* edges DELIVERED (pressed) to an owner */
  uint32_t down[NXINPUT_LC_MAX_EDGES / 32]; /* edges PHYSICALLY down (source not neutral) */
} nxinput_lc_device;

typedef struct nxinput_lc_ops {
  void *user;
  /* deliver an edge press/release to the active owner (exactly once) */
  void (*deliver)(void *user, nxinput_lc_owner owner, int instance_id, uint32_t generation, unsigned edge, int pressed, uint32_t context_epoch, uint32_t mapping_generation);
  /* owner text parser for hot reload: 0 ok (digest out), -1 error */
  int (*parse_owner)(void *user, const char *text, uint64_t *digest);
} nxinput_lc_ops;

typedef struct nxinput_lifecycle {
  nxinput_lc_ops ops;
  nxinput_lc_device dev[NXINPUT_LC_MAX_DEVICES];
  uint32_t next_generation, context_epoch, mapping_generation, modality_epoch;
  uint8_t context, fallback, owner, neutral_pending;
  uint64_t owner_digest;
  unsigned refused_stale_generation, refused_gate, refused_not_open, refused_open_race, released_all, reloads_ok, reloads_failed;
  /* 0.11.1 (C5): the HOTPLUG generation -- bumped on every admit/remove so
   * a caller that enumerated the device list can prove the list did not
   * change between discovery and open (nxinput_lifecycle_open_checked). */
  uint32_t hotplug_generation;
  unsigned refused_reopen, refused_quarantined, refused_hotplug_race, admits_overflow;
} nxinput_lifecycle;

int nxinput_lifecycle_init(nxinput_lifecycle *lc, const nxinput_lc_ops *ops, nxinput_lc_fallback declared_fallback, uint64_t initial_owner_digest);
/* admit a physical device under an instance id; reconnect (same physical,
 * same or reused instance) yields a NEW generation. Returns generation, 0 on overflow. */
uint32_t nxinput_lifecycle_admit(nxinput_lifecycle *lc, int instance_id, uint64_t physical_id);
/* open must name the instance that was admitted under this generation.
 * 0.11.1 (C5): a device already OPEN is not reopened (-1, refused_reopen:
 * remove/re-admit first); a QUARANTINED device never opens again under any
 * generation (-1, refused_quarantined). */
int nxinput_lifecycle_open(nxinput_lifecycle *lc, int instance_id, uint32_t generation);
/* 0.11.1 (C5): open that also proves the device LIST did not change
 * between discovery and open: `hotplug_seen` is the hotplug generation the
 * caller read when it enumerated. A different value => the instance may be
 * another device (instance race): refused (-1, refused_hotplug_race) and the
 * device quarantined until re-admitted. */
int nxinput_lifecycle_open_checked(nxinput_lifecycle *lc, int instance_id, uint32_t generation, uint32_t hotplug_seen);
uint32_t nxinput_lifecycle_hotplug_generation(const nxinput_lifecycle *lc);
void nxinput_lifecycle_remove(nxinput_lifecycle *lc, int instance_id);
/* an edge event from a device */
int nxinput_lifecycle_edge(nxinput_lifecycle *lc, int instance_id, uint32_t generation, unsigned edge, int pressed);
void nxinput_lifecycle_focus_lost(nxinput_lifecycle *lc);
void nxinput_lifecycle_set_context(nxinput_lifecycle *lc, nxinput_lc_context ctx);
/* the neutral gate: returns 1 when it opened (owner activated) */
int nxinput_lifecycle_neutral_gate(nxinput_lifecycle *lc);
/* E7: hot reload from owner text; 0 published, -1 kept last generation */
int nxinput_lifecycle_hot_reload(nxinput_lifecycle *lc, const char *owner_text);
int nxinput_lifecycle_any_held(const nxinput_lifecycle *lc);

/* ---------------- E13: synchronized CAS transaction --------------------- */
typedef struct nxinput_sync_ops {
  void *user;
  int (*engine_apply)(void *user, uint64_t mapping_digest);            /* 0 ok */
  int (*engine_readback)(void *user, uint64_t *effective_digest);      /* 0 ok */
  int (*engine_rollback)(void *user, uint64_t previous_digest);        /* 0 ok */
  int (*owner_write_cas)(void *user, uint64_t expected_digest, uint64_t new_digest); /* 0 ok, -2 conflict */
} nxinput_sync_ops;
typedef enum nxinput_sync_result {
  NXINPUT_SYNC_PUBLISHED = 0, NXINPUT_SYNC_REFUSED_READBACK, NXINPUT_SYNC_CONFLICT,
  NXINPUT_SYNC_ROLLBACK_FAILED_RESTART, NXINPUT_SYNC_REFUSED_LEASE, NXINPUT_SYNC_REFUSED_GENERATION
} nxinput_sync_result;
typedef struct nxinput_sync {
  nxinput_sync_ops ops; nxinput_lifecycle *lc; uint64_t engine_digest, owner_digest; uint8_t lease_held;
  unsigned published, refused, conflicts, restarts;
} nxinput_sync;
int nxinput_sync_init(nxinput_sync *s, const nxinput_sync_ops *ops, nxinput_lifecycle *lc, uint64_t initial_digest);
/* owner_reload: the owner file changed to `new_digest` under `expected_generation` */
nxinput_sync_result nxinput_sync_owner_reload(nxinput_sync *s, uint32_t expected_generation, uint64_t new_digest);
/* engine_rebind_ui: the engine UI produced `new_digest`; owner CAS on the digest it was read at */
nxinput_sync_result nxinput_sync_engine_rebind(nxinput_sync *s, uint64_t owner_digest_seen, uint64_t new_digest);
#ifdef __cplusplus
}
#endif
#endif
