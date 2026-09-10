/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxgl_retry_contract -- the single-clean-retry contract as a pure state
 * machine (V3).
 *
 * The effectful pieces already enforce "one attempt per process" each in
 * their own way (the NXGL_SDL_PROVIDER_RECOVERY_V2_APPLIED marker, the
 * one-shot rollback, the pre-context plan in nxgl.h). This module states the
 * shared contract once, purely, so an adapter can drive its KMSDRM/EGL retry
 * loop against an auditable machine instead of ad-hoc flags:
 *
 *   - the first REAL failure (a measured KMSDRM/EGL failure, never a guess)
 *     authorizes AT MOST ONE clean retry;
 *   - a second failure is TERMINAL: no third attempt, ever -- the adapter
 *     must surface the real error and exit;
 *   - success at any point ends the machine in SUCCEEDED;
 *   - every event on a finished machine (SUCCEEDED or TERMINAL) is refused.
 *
 * Nothing here performs the retry, touches EGL, the environment or the
 * process: the machine only answers "may I?" and remembers what happened.
 */
#ifndef NXGL_RETRY_CONTRACT_H
#define NXGL_RETRY_CONTRACT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXGL_RETRY_CONTRACT_API_VERSION 1u

typedef enum nxgl_retry_state {
  /* No attempt has been reported yet. */
  NXGL_RETRY_STATE_FRESH = 0,
  /* The first attempt failed; exactly one clean retry is authorized. */
  NXGL_RETRY_STATE_RETRY_AUTHORIZED,
  /* The attempt (first or retry) succeeded. Final. */
  NXGL_RETRY_STATE_SUCCEEDED,
  /* The retry also failed. Final: no further attempt is ever permitted. */
  NXGL_RETRY_STATE_TERMINAL
} nxgl_retry_state;

typedef enum nxgl_retry_decision {
  /* Proceed: nothing to retry (success recorded). */
  NXGL_RETRY_DECISION_PROCEED = 0,
  /* One clean retry is authorized -- tear down fully, then try once more. */
  NXGL_RETRY_DECISION_RETRY_ONCE,
  /* Terminal: report the real failure; do not attempt again. */
  NXGL_RETRY_DECISION_FAIL_TERMINAL,
  /* The call was invalid (finished machine, malformed struct). State was
   * not changed except that a finished machine stays finished. */
  NXGL_RETRY_DECISION_REFUSED
} nxgl_retry_decision;

typedef struct nxgl_retry_contract {
  uint32_t api_version; /* NXGL_RETRY_CONTRACT_API_VERSION */
  size_t struct_size;   /* sizeof(nxgl_retry_contract) */
  nxgl_retry_state state;
  unsigned failures_reported;
  unsigned retries_authorized; /* can only ever reach 1 */
} nxgl_retry_contract;

/* Start (or restart nothing: a machine is single-use) in FRESH. */
void nxgl_retry_contract_init(nxgl_retry_contract *contract);

/* Report the outcome of one real attempt: failed=0 for success, nonzero for
 * a real measured failure. Returns the decision; REFUSED on a NULL/malformed
 * contract or on any report after SUCCEEDED/TERMINAL (finished machines
 * never change state). At most one RETRY_ONCE is ever returned per machine;
 * the failure after it is always FAIL_TERMINAL. */
nxgl_retry_decision nxgl_retry_contract_report(nxgl_retry_contract *contract,
                                               int failed);

/* Whether one more attempt is currently permitted (FRESH counts: the first
 * attempt is always permitted; RETRY_AUTHORIZED permits the single retry). */
int nxgl_retry_contract_attempt_permitted(const nxgl_retry_contract *contract);

const char *nxgl_retry_state_name(nxgl_retry_state state);

#ifdef __cplusplus
}
#endif

#endif /* NXGL_RETRY_CONTRACT_H */
