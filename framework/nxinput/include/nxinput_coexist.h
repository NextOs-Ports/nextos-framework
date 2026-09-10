/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_COEXIST_H
#define NXINPUT_COEXIST_H
/*
 * nxinput_coexist -- 0.11.1 (B8, mission 5.7): the PRE-INIT SEQUENCER for a
 * process that maps SDL2 and SDL3 at the same time.
 *
 * `SDL_GAMECONTROLLERCONFIG` has one global name and both majors import it
 * at init. The sequencer stages the ambient value out of the environment
 * ONCE, then serves each major's init under ITS OWN corpus (begin -> the
 * major's SDL_Init -> end restores the staged state); a second begin while
 * one is open is refused (never two majors consuming one ambiguous
 * configuration). When the process is done, restore() puts the ambient
 * value back. Everything above the init (descriptors, stores, instance ids,
 * event ids, generations) is per major by construction: this module only
 * guards the one shared resource. Physical arbitration between the two
 * majors is nxinput_prerouter_bind_physical().
 *
 * Impure on purpose: it is the environment that is being sequenced.
 */
#include "nxinput_sdl.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NXINPUT_COEXIST_STAGE_MAX 65536u
typedef struct nxinput_coexist {
  char *staged; size_t staged_len; uint8_t had_ambient, staged_done, open; uint8_t open_api;
  unsigned sequenced, concurrent_refused;
} nxinput_coexist;
int nxinput_coexist_init(nxinput_coexist *c);
/* Stage the ambient variable out (0 ok, also when absent). */
int nxinput_coexist_stage(nxinput_coexist *c);
/* Serve `api`'s init: export `corpus` (may be NULL for none) as the ONLY
 * SDL_GAMECONTROLLERCONFIG until end(). -1 when another begin is open. */
int nxinput_coexist_begin(nxinput_coexist *c, uint8_t api, const char *corpus);
/* Provider-LOCAL corpus: `sethint` is the provider's own SDL_SetHint,
 * resolved from ITS handle (never a global symbol), so a major that
 * snapshots the environment at first use (SDL3) still receives ITS corpus.
 * The environment is exported too for majors that read it at init. */
typedef int (*nxinput_coexist_sethint_fn)(const char *name, const char *value);
int nxinput_coexist_begin_with_hint(nxinput_coexist *c, uint8_t api, const char *corpus, nxinput_coexist_sethint_fn sethint);
int nxinput_coexist_end(nxinput_coexist *c);
/* Put the ambient value back (end of the process' sequencing). */
void nxinput_coexist_restore(nxinput_coexist *c);
int nxinput_coexist_receipt(const nxinput_coexist *c, char *out, size_t cap);

/* 0.11.1 (B8): the ARBITRATION of two majors in one process, from their
 * provider descriptors. SEPARATE = two providers (isolated stores, one
 * physical owner elected by the pre-router); SHARED_CORE = sdl2-compat
 * over the SDL3 core: ONE provider behind two ABIs, stores shared by
 * construction, every isolation claim refused; AMBIGUOUS = descriptors
 * cannot be correlated (fail closed before any edge). */
#include "nxinput_provider.h"
typedef enum nxinput_coexist_verdict { NXINPUT_COEXIST_SEPARATE = 0, NXINPUT_COEXIST_SHARED_CORE, NXINPUT_COEXIST_AMBIGUOUS } nxinput_coexist_verdict;
nxinput_coexist_verdict nxinput_coexist_arbitrate(const nxinput_provider_descriptor *sdl2, const nxinput_provider_descriptor *sdl3);
const char *nxinput_coexist_verdict_name(nxinput_coexist_verdict v);
#ifdef __cplusplus
}
#endif
#endif
