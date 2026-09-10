/* SPDX-License-Identifier: GPL-3.0-only */
/* Pure single-clean-retry state machine. See nxgl_retry_contract.h. */
#include "nxgl_retry_contract.h"

static int contract_valid(const nxgl_retry_contract *contract) {
  return contract != NULL &&
         contract->api_version == NXGL_RETRY_CONTRACT_API_VERSION &&
         contract->struct_size == sizeof(*contract);
}

void nxgl_retry_contract_init(nxgl_retry_contract *contract) {
  if (contract == NULL) {
    return;
  }
  contract->api_version = NXGL_RETRY_CONTRACT_API_VERSION;
  contract->struct_size = sizeof(*contract);
  contract->state = NXGL_RETRY_STATE_FRESH;
  contract->failures_reported = 0;
  contract->retries_authorized = 0;
}

nxgl_retry_decision nxgl_retry_contract_report(nxgl_retry_contract *contract,
                                               int failed) {
  if (!contract_valid(contract)) {
    return NXGL_RETRY_DECISION_REFUSED;
  }
  switch (contract->state) {
  case NXGL_RETRY_STATE_FRESH:
    if (failed == 0) {
      contract->state = NXGL_RETRY_STATE_SUCCEEDED;
      return NXGL_RETRY_DECISION_PROCEED;
    }
    contract->failures_reported++;
    contract->retries_authorized = 1;
    contract->state = NXGL_RETRY_STATE_RETRY_AUTHORIZED;
    return NXGL_RETRY_DECISION_RETRY_ONCE;
  case NXGL_RETRY_STATE_RETRY_AUTHORIZED:
    if (failed == 0) {
      contract->state = NXGL_RETRY_STATE_SUCCEEDED;
      return NXGL_RETRY_DECISION_PROCEED;
    }
    contract->failures_reported++;
    contract->state = NXGL_RETRY_STATE_TERMINAL;
    return NXGL_RETRY_DECISION_FAIL_TERMINAL;
  case NXGL_RETRY_STATE_SUCCEEDED:
  case NXGL_RETRY_STATE_TERMINAL:
  default:
    /* Finished machines never move: a report here is a caller bug. */
    return NXGL_RETRY_DECISION_REFUSED;
  }
}

int nxgl_retry_contract_attempt_permitted(
    const nxgl_retry_contract *contract) {
  if (!contract_valid(contract)) {
    return 0;
  }
  return contract->state == NXGL_RETRY_STATE_FRESH ||
         contract->state == NXGL_RETRY_STATE_RETRY_AUTHORIZED;
}

const char *nxgl_retry_state_name(nxgl_retry_state state) {
  switch (state) {
  case NXGL_RETRY_STATE_FRESH:
    return "fresh";
  case NXGL_RETRY_STATE_RETRY_AUTHORIZED:
    return "retry-authorized";
  case NXGL_RETRY_STATE_SUCCEEDED:
    return "succeeded";
  case NXGL_RETRY_STATE_TERMINAL:
    return "terminal";
  default:
    return "invalid";
  }
}
