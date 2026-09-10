/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_AUTHORITY_V5_H
#define NXINPUT_AUTHORITY_V5_H
/*
 * nxinput_authority_v5 -- 0.11.1 (E3): PORT_AUTHORITY_MODE as an EXECUTABLE
 * machine, not a header word.
 *
 * The owner file declares AUTHORITY = nextos | engine | synchronized (schema
 * 4). Until 0.11.0 that word was parsed and stored; nothing acted on it.
 * This module ELECTS the mode once per mapping generation and then answers,
 * per edge and per artefact, what the mode allows:
 *
 *   NEXTOS        the GPTK owner is the authority. Every edge resolves to
 *                 exactly one owner (nxinput_decision_edge_owner): action ->
 *                 NEXTOS_ACTION, native -> ENGINE_NATIVE (that edge only,
 *                 the NextOS delivery for it is suppressed), null ->
 *                 SUPPRESSED. The owner file is EDITABLE and materialized.
 *   ENGINE        the engine's own config/UI governs. There is NO owner GPTK:
 *                 the module refuses to present an editable owner (it names a
 *                 READBACK MIRROR instead), every edge is ENGINE_NATIVE unless
 *                 the mirror says null, and the registry is fed from the
 *                 engine readback, never from a GPTK the user could edit.
 *   SYNCHRONIZED  both sides exist and are kept equal by the CAS transaction
 *                 (nxinput_sync). Electing it REQUIRES the engine hooks
 *                 (apply/readback/rollback/owner CAS): without them the
 *                 election fails closed to... nothing: the port stays native
 *                 (unproven), never a silent NEXTOS.
 *
 * Exactly ONE mode is active; changing it is a new mapping generation with
 * release-all (the caller's lifecycle). The sovereign chord pre-router is
 * outside the mode (nxinput_prerouter): while it retains an edge the owner
 * is SUPPRESSED whatever the mode says.
 *
 * Pure: no I/O.
 */
#include "nxinput_decision.h"
#include "nxinput_gptk4.h"
#include "nxinput_lifecycle.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum nxinput_authority_owner_file {
  NXINPUT_OWNER_FILE_NONE = 0,       /* not elected / native */
  NXINPUT_OWNER_FILE_EDITABLE,       /* NEXTOSCONTROLLERS.gptk is the owner's */
  NXINPUT_OWNER_FILE_MIRROR_READBACK,/* ENGINE: a named readback mirror, never presented as editable */
  NXINPUT_OWNER_FILE_SYNCHRONIZED    /* both, kept equal by CAS */
} nxinput_authority_owner_file;

typedef struct nxinput_authority_v5 {
  uint8_t elected;                 /* 1 after a successful election */
  uint8_t mode;                    /* nxinput_authority_mode */
  uint8_t owner_file;              /* nxinput_authority_owner_file */
  uint32_t mapping_generation;     /* the generation the election belongs to */
  const nxinput_gptk4 *map;        /* not owned */
  const nxinput_sync_ops *sync;    /* required for SYNCHRONIZED */
  char mirror_name[64];            /* ENGINE: "<port>.engine-readback.mirror" */
  unsigned refused_sync_without_engine, refused_stale_generation, edges_resolved, edges_suppressed_by_chord;
} nxinput_authority_v5;

/* Elect the mode of `map` (its AUTHORITY word) for `mapping_generation`.
 * `sync_ops` may be NULL except for SYNCHRONIZED. Returns 0 on election,
 * -1 when refused (NULL map, SYNCHRONIZED without engine hooks): the module
 * stays un-elected and every resolution answers SUPPRESSED. */
int nxinput_authority_v5_elect(nxinput_authority_v5 *a, const nxinput_gptk4 *map,
                               uint32_t mapping_generation, const nxinput_sync_ops *sync_ops);
/* The one owner of an edge in the elected mode. `binding` is the gptk4 kind
 * of the resolved slot (NATIVE/NULL/ACTION); `chord_hold` from the
 * pre-router; `generation` must equal the elected one (a stale generation
 * is SUPPRESSED and counted). */
nxinput_edge_owner nxinput_authority_v5_edge_owner(nxinput_authority_v5 *a, uint32_t generation,
                                                   nxinput_gptk4_kind binding, int chord_hold);
/* Resolve one slot of the elected map in `context` straight to its owner. */
nxinput_edge_owner nxinput_authority_v5_resolve(nxinput_authority_v5 *a, uint32_t generation,
                                                const char *context, nxinput_gptk4_control slot, int chord_hold);
/* What artefact the owner may see/edit in this mode. */
nxinput_authority_owner_file nxinput_authority_v5_owner_file(const nxinput_authority_v5 *a);
/* 1 when the registry/prompts must be fed from the ENGINE READBACK (ENGINE
 * mode), 0 when from the GPTK generation. */
int nxinput_authority_v5_registry_from_readback(const nxinput_authority_v5 *a);
const char *nxinput_authority_v5_mode_name(uint8_t mode);
#ifdef __cplusplus
}
#endif
#endif
