/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_authority_v5 -- see include/nxinput_authority_v5.h. Pure. */
#include "nxinput_authority_v5.h"
#include <stdio.h>
#include <string.h>

const char *nxinput_authority_v5_mode_name(uint8_t mode) {
  switch (mode) {
    case NXINPUT_AUTHORITY_NEXTOS: return "nextos";
    case NXINPUT_AUTHORITY_ENGINE: return "engine";
    case NXINPUT_AUTHORITY_SYNCHRONIZED: return "synchronized";
    default: return "none";
  }
}

int nxinput_authority_v5_elect(nxinput_authority_v5 *a, const nxinput_gptk4 *map,
                               uint32_t mapping_generation, const nxinput_sync_ops *sync_ops) {
  uint8_t mode;
  if (a == NULL) return -1;
  {
    unsigned keep_sync = a->refused_sync_without_engine, keep_stale = a->refused_stale_generation;
    memset(a, 0, sizeof *a);
    a->refused_sync_without_engine = keep_sync; a->refused_stale_generation = keep_stale;
  }
  if (map == NULL) return -1;
  switch (map->authority) {
    case NXINPUT_GPTK4_AUTH_ENGINE: mode = NXINPUT_AUTHORITY_ENGINE; break;
    case NXINPUT_GPTK4_AUTH_SYNCHRONIZED: mode = NXINPUT_AUTHORITY_SYNCHRONIZED; break;
    default: mode = NXINPUT_AUTHORITY_NEXTOS; break;
  }
  if (mode == NXINPUT_AUTHORITY_SYNCHRONIZED &&
      (sync_ops == NULL || sync_ops->engine_apply == NULL || sync_ops->engine_readback == NULL ||
       sync_ops->engine_rollback == NULL || sync_ops->owner_write_cas == NULL)) {
    a->refused_sync_without_engine++;
    return -1; /* fail closed: no silent fallback to NEXTOS */
  }
  a->elected = 1; a->mode = mode; a->map = map; a->sync = sync_ops; a->mapping_generation = mapping_generation;
  switch (mode) {
    case NXINPUT_AUTHORITY_ENGINE:
      a->owner_file = NXINPUT_OWNER_FILE_MIRROR_READBACK;
      snprintf(a->mirror_name, sizeof a->mirror_name, "%.40s.engine-readback.mirror", map->port[0] ? map->port : "port");
      break;
    case NXINPUT_AUTHORITY_SYNCHRONIZED: a->owner_file = NXINPUT_OWNER_FILE_SYNCHRONIZED; break;
    default: a->owner_file = NXINPUT_OWNER_FILE_EDITABLE; break;
  }
  return 0;
}

static nxinput_edge_binding binding_of(nxinput_gptk4_kind k) {
  switch (k) {
    case NXINPUT_GPTK4_ACTION: return NXINPUT_EDGE_ACTION;
    case NXINPUT_GPTK4_NULL: return NXINPUT_EDGE_NULL;
    default: return NXINPUT_EDGE_NATIVE;
  }
}

nxinput_edge_owner nxinput_authority_v5_edge_owner(nxinput_authority_v5 *a, uint32_t generation,
                                                   nxinput_gptk4_kind binding, int chord_hold) {
  nxinput_edge_owner o;
  if (a == NULL || !a->elected) return NXINPUT_OWNER_SUPPRESSED;
  if (generation != a->mapping_generation) { a->refused_stale_generation++; return NXINPUT_OWNER_SUPPRESSED; }
  if (chord_hold) { a->edges_suppressed_by_chord++; return NXINPUT_OWNER_SUPPRESSED; }
  o = nxinput_decision_edge_owner((nxinput_authority_mode)a->mode, binding_of(binding), 0);
  a->edges_resolved++;
  return o;
}

nxinput_edge_owner nxinput_authority_v5_resolve(nxinput_authority_v5 *a, uint32_t generation,
                                                const char *context, nxinput_gptk4_control slot, int chord_hold) {
  const nxinput_gptk4_binding *b;
  if (a == NULL || !a->elected || a->map == NULL) return NXINPUT_OWNER_SUPPRESSED;
  b = nxinput_gptk4_resolve(a->map, context, slot);
  return nxinput_authority_v5_edge_owner(a, generation, b ? (nxinput_gptk4_kind)b->kind : NXINPUT_GPTK4_NATIVE, chord_hold);
}

nxinput_authority_owner_file nxinput_authority_v5_owner_file(const nxinput_authority_v5 *a) {
  return a != NULL && a->elected ? (nxinput_authority_owner_file)a->owner_file : NXINPUT_OWNER_FILE_NONE;
}

int nxinput_authority_v5_registry_from_readback(const nxinput_authority_v5 *a) {
  return a != NULL && a->elected && a->mode == NXINPUT_AUTHORITY_ENGINE;
}
