/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_lifecycle -- see include/nxinput_lifecycle.h. Pure. */
#include "nxinput_lifecycle.h"
#include <string.h>

static int ctx_is_unknown(uint8_t c) { return c == NXINPUT_LC_CTX_UNKNOWN || c == NXINPUT_LC_CTX_LOADING || c == NXINPUT_LC_CTX_UNPROVEN; }

int nxinput_lifecycle_init(nxinput_lifecycle *lc, const nxinput_lc_ops *ops, nxinput_lc_fallback fb, uint64_t initial_owner_digest) {
  if (!lc || !ops || !ops->deliver) return -1;
  memset(lc, 0, sizeof *lc);
  lc->ops = *ops; lc->next_generation = 1; lc->context_epoch = 1; lc->mapping_generation = 1; lc->modality_epoch = 1;
  lc->fallback = (uint8_t)fb; lc->context = NXINPUT_LC_CTX_UNKNOWN; lc->owner = NXINPUT_LC_OWNER_NONE;
  lc->neutral_pending = 1; lc->owner_digest = initial_owner_digest; lc->hotplug_generation = 1;
  return 0;
}

uint32_t nxinput_lifecycle_hotplug_generation(const nxinput_lifecycle *lc) { return lc ? lc->hotplug_generation : 0u; }

static nxinput_lc_device *dev_by_instance(nxinput_lifecycle *lc, int instance_id) {
  unsigned i; for (i = 0; i < NXINPUT_LC_MAX_DEVICES; i++) if (lc->dev[i].used && lc->dev[i].instance_id == instance_id) return &lc->dev[i]; return NULL;
}
static void dev_release_all(nxinput_lifecycle *lc, nxinput_lc_device *d) {
  unsigned e;
  for (e = 0; e < NXINPUT_LC_MAX_EDGES; e++) {
    if (d->held[e / 32] & (1u << (e % 32))) {
      lc->ops.deliver(lc->ops.user, (nxinput_lc_owner)lc->owner, d->instance_id, d->generation, e, 0, lc->context_epoch, lc->mapping_generation);
      d->held[e / 32] &= ~(1u << (e % 32));
    }
  }
}
static void release_all(nxinput_lifecycle *lc) {
  unsigned i; for (i = 0; i < NXINPUT_LC_MAX_DEVICES; i++) if (lc->dev[i].used) dev_release_all(lc, &lc->dev[i]);
  lc->released_all++;
}

uint32_t nxinput_lifecycle_admit(nxinput_lifecycle *lc, int instance_id, uint64_t physical_id) {
  nxinput_lc_device *d; unsigned i;
  if (!lc) return 0;
  d = dev_by_instance(lc, instance_id);
  if (d) { dev_release_all(lc, d); d->used = 0; } /* instance id reused: the old one is gone */
  for (i = 0; i < NXINPUT_LC_MAX_DEVICES; i++) if (lc->dev[i].used && lc->dev[i].physical_id == physical_id) { dev_release_all(lc, &lc->dev[i]); lc->dev[i].used = 0; }
  lc->hotplug_generation++; /* C5: the device set changed */
  for (i = 0; i < NXINPUT_LC_MAX_DEVICES; i++) {
    if (!lc->dev[i].used) {
      d = &lc->dev[i]; memset(d, 0, sizeof *d);
      d->used = 1; d->state = 0; d->instance_id = instance_id; d->physical_id = physical_id; d->generation = lc->next_generation++;
      return d->generation;
    }
  }
  lc->admits_overflow++;
  return 0; /* visible: the caller counts it; nothing truncated silently */
}

int nxinput_lifecycle_open(nxinput_lifecycle *lc, int instance_id, uint32_t generation) {
  nxinput_lc_device *d = lc ? dev_by_instance(lc, instance_id) : NULL;
  if (!d) { if (lc) lc->refused_open_race++; return -1; }
  if (d->state == 2) { lc->refused_quarantined++; return -1; } /* C5: quarantined until re-admitted */
  if (d->generation != generation) { lc->refused_open_race++; dev_release_all(lc, d); d->state = 2; return -1; } /* instance swapped between discovery and open: quarantined (its holds released first) */
  if (d->state == 1) { lc->refused_reopen++; return -1; } /* C5: already open: no second open, no reset of its held state */
  d->state = 1; return 0;
}

int nxinput_lifecycle_open_checked(nxinput_lifecycle *lc, int instance_id, uint32_t generation, uint32_t hotplug_seen) {
  nxinput_lc_device *d = lc ? dev_by_instance(lc, instance_id) : NULL;
  if (!d) { if (lc) lc->refused_open_race++; return -1; }
  if (hotplug_seen != lc->hotplug_generation) { lc->refused_hotplug_race++; dev_release_all(lc, d); d->state = 2; return -1; } /* the list changed under the caller: quarantined, holds released */
  return nxinput_lifecycle_open(lc, instance_id, generation);
}

void nxinput_lifecycle_remove(nxinput_lifecycle *lc, int instance_id) {
  nxinput_lc_device *d = lc ? dev_by_instance(lc, instance_id) : NULL;
  if (!d) return;
  lc->hotplug_generation++; /* C5: the device set changed */
  dev_release_all(lc, d); memset(d->down, 0, sizeof d->down); d->used = 0;
  if (lc->neutral_pending) nxinput_lifecycle_neutral_gate(lc);
}

int nxinput_lifecycle_edge(nxinput_lifecycle *lc, int instance_id, uint32_t generation, unsigned edge, int pressed) {
  nxinput_lc_device *d; uint32_t bit;
  if (!lc || edge >= NXINPUT_LC_MAX_EDGES) return -1;
  d = dev_by_instance(lc, instance_id);
  if (!d || d->state != 1) { lc->refused_not_open++; return -1; }
  if (d->generation != generation) { lc->refused_stale_generation++; return -1; }
  bit = 1u << (edge % 32);
  if (!pressed) {
    d->down[edge / 32] &= ~bit;
    if (!(d->held[edge / 32] & bit)) { if (lc->neutral_pending) nxinput_lifecycle_neutral_gate(lc); return 0; } /* never delivered: nothing to release */
    d->held[edge / 32] &= ~bit;
    lc->ops.deliver(lc->ops.user, (nxinput_lc_owner)lc->owner, instance_id, generation, edge, 0, lc->context_epoch, lc->mapping_generation);
    if (lc->neutral_pending) nxinput_lifecycle_neutral_gate(lc);
    return 1;
  }
  d->down[edge / 32] |= bit; /* the source is not neutral, whoever owns it */
  if (lc->neutral_pending || lc->owner == NXINPUT_LC_OWNER_NONE) { lc->refused_gate++; return -2; } /* gate closed: not delivered to any owner, not latched */
  if (d->held[edge / 32] & bit) return 0;
  d->held[edge / 32] |= bit;
  lc->ops.deliver(lc->ops.user, (nxinput_lc_owner)lc->owner, instance_id, generation, edge, 1, lc->context_epoch, lc->mapping_generation);
  return 1;
}

int nxinput_lifecycle_any_held(const nxinput_lifecycle *lc) {
  unsigned i, w;
  if (!lc) return 0;
  for (i = 0; i < NXINPUT_LC_MAX_DEVICES; i++) if (lc->dev[i].used) for (w = 0; w < NXINPUT_LC_MAX_EDGES / 32; w++) if (lc->dev[i].down[w]) return 1;
  return 0;
}

static void begin_transition(nxinput_lifecycle *lc) {
  release_all(lc);             /* 1. release-all of the previous epoch */
  lc->context_epoch++;         /* 2. new epoch */
  lc->owner = NXINPUT_LC_OWNER_NONE; lc->neutral_pending = 1; /* 3. wait for neutral */
}

int nxinput_lifecycle_neutral_gate(nxinput_lifecycle *lc) {
  if (!lc || !lc->neutral_pending) return 0;
  if (nxinput_lifecycle_any_held(lc)) return 0;
  lc->neutral_pending = 0;
  /* 4. exactly one owner: actions for a known context; the DECLARED fallback otherwise */
  if (ctx_is_unknown(lc->context)) lc->owner = lc->fallback == NXINPUT_LC_FALLBACK_ENGINE_NATIVE ? NXINPUT_LC_OWNER_ENGINE_NATIVE : NXINPUT_LC_OWNER_NONE;
  else lc->owner = NXINPUT_LC_OWNER_ACTIONS;
  return 1;
}

void nxinput_lifecycle_focus_lost(nxinput_lifecycle *lc) { if (!lc) return; begin_transition(lc); nxinput_lifecycle_neutral_gate(lc); }

void nxinput_lifecycle_set_context(nxinput_lifecycle *lc, nxinput_lc_context ctx) {
  if (!lc) return;
  begin_transition(lc);
  lc->context = (uint8_t)ctx;
  nxinput_lifecycle_neutral_gate(lc);
}

int nxinput_lifecycle_hot_reload(nxinput_lifecycle *lc, const char *owner_text) {
  uint64_t digest = 0;
  if (!lc || !lc->ops.parse_owner) return -1;
  if (lc->ops.parse_owner(lc->ops.user, owner_text, &digest) != 0) { lc->reloads_failed++; return -1; } /* last valid generation kept */
  begin_transition(lc);                 /* release old holds BEFORE publishing */
  lc->mapping_generation++; lc->owner_digest = digest; lc->reloads_ok++;
  nxinput_lifecycle_neutral_gate(lc);
  return 0;
}

/* ---------------- E13 ---------------------------------------------------- */
int nxinput_sync_init(nxinput_sync *s, const nxinput_sync_ops *ops, nxinput_lifecycle *lc, uint64_t initial_digest) {
  if (!s || !ops || !lc || !ops->engine_apply || !ops->engine_readback || !ops->engine_rollback || !ops->owner_write_cas) return -1;
  memset(s, 0, sizeof *s); s->ops = *ops; s->lc = lc; s->engine_digest = initial_digest; s->owner_digest = initial_digest; s->lease_held = 1;
  return 0;
}

static nxinput_sync_result sync_apply(nxinput_sync *s, uint64_t new_digest) {
  uint64_t eff = 0, prev = s->engine_digest;
  s->lc->owner = NXINPUT_LC_OWNER_NONE; s->lc->neutral_pending = 1; /* freeze new edges */
  nxinput_lifecycle_focus_lost(s->lc);                               /* release holds of the previous generation */
  if (s->ops.engine_apply(s->ops.user, new_digest) != 0 || s->ops.engine_readback(s->ops.user, &eff) != 0 || eff != new_digest) {
    if (s->ops.engine_rollback(s->ops.user, prev) != 0) { s->restarts++; s->refused++; return NXINPUT_SYNC_ROLLBACK_FAILED_RESTART; }
    s->refused++; nxinput_lifecycle_neutral_gate(s->lc);
    return NXINPUT_SYNC_REFUSED_READBACK;                            /* generation unchanged, owner text untouched */
  }
  return NXINPUT_SYNC_PUBLISHED;
}

nxinput_sync_result nxinput_sync_owner_reload(nxinput_sync *s, uint32_t expected_generation, uint64_t new_digest) {
  nxinput_sync_result r;
  if (!s) return NXINPUT_SYNC_REFUSED_LEASE;
  if (!s->lease_held) { s->refused++; return NXINPUT_SYNC_REFUSED_LEASE; }
  if (expected_generation != s->lc->mapping_generation) { s->refused++; return NXINPUT_SYNC_REFUSED_GENERATION; }
  r = sync_apply(s, new_digest);
  if (r != NXINPUT_SYNC_PUBLISHED) return r;
  s->engine_digest = new_digest; s->owner_digest = new_digest;
  s->lc->mapping_generation++; s->lc->owner_digest = new_digest; s->published++;
  nxinput_lifecycle_neutral_gate(s->lc);
  return NXINPUT_SYNC_PUBLISHED;
}

nxinput_sync_result nxinput_sync_engine_rebind(nxinput_sync *s, uint64_t owner_digest_seen, uint64_t new_digest) {
  nxinput_sync_result r; int w;
  if (!s) return NXINPUT_SYNC_REFUSED_LEASE;
  if (!s->lease_held) { s->refused++; return NXINPUT_SYNC_REFUSED_LEASE; }
  if (owner_digest_seen != s->owner_digest) { s->conflicts++; return NXINPUT_SYNC_CONFLICT; } /* owner changed in parallel: nothing written */
  r = sync_apply(s, new_digest);
  if (r != NXINPUT_SYNC_PUBLISHED) return r;
  w = s->ops.owner_write_cas(s->ops.user, s->owner_digest, new_digest);
  if (w != 0) { /* CAS lost or write failed: roll the engine back too, never half */
    if (s->ops.engine_rollback(s->ops.user, s->engine_digest) != 0) { s->restarts++; return NXINPUT_SYNC_ROLLBACK_FAILED_RESTART; }
    s->conflicts++; nxinput_lifecycle_neutral_gate(s->lc); return NXINPUT_SYNC_CONFLICT;
  }
  s->engine_digest = new_digest; s->owner_digest = new_digest;
  s->lc->mapping_generation++; s->lc->owner_digest = new_digest; s->published++;
  nxinput_lifecycle_neutral_gate(s->lc);
  return NXINPUT_SYNC_PUBLISHED;
}
