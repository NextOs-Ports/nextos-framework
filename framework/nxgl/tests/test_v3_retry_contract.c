/* SPDX-License-Identifier: GPL-3.0-only */
/* V3-GRAPHICS-01: single-clean-retry contract. At most ONE retry after a real
 * KMSDRM/EGL failure; a second failure is terminal, forever. */
#include "nxgl_retry_contract.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      g_failures++;                                                      \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
    }                                                                    \
  } while (0)

int main(void) {
  /* 1. Happy path: first attempt succeeds; the machine finishes and refuses
   * anything further. */
  {
    nxgl_retry_contract contract;

    nxgl_retry_contract_init(&contract);
    CHECK(contract.state == NXGL_RETRY_STATE_FRESH);
    CHECK(nxgl_retry_contract_attempt_permitted(&contract) == 1);
    CHECK(nxgl_retry_contract_report(&contract, 0) ==
          NXGL_RETRY_DECISION_PROCEED);
    CHECK(contract.state == NXGL_RETRY_STATE_SUCCEEDED);
    CHECK(nxgl_retry_contract_attempt_permitted(&contract) == 0);
    CHECK(nxgl_retry_contract_report(&contract, 1) ==
          NXGL_RETRY_DECISION_REFUSED);
    CHECK(contract.state == NXGL_RETRY_STATE_SUCCEEDED);
  }

  /* 2. Fail then succeed: exactly one retry authorized and consumed. */
  {
    nxgl_retry_contract contract;

    nxgl_retry_contract_init(&contract);
    CHECK(nxgl_retry_contract_report(&contract, 1) ==
          NXGL_RETRY_DECISION_RETRY_ONCE);
    CHECK(contract.state == NXGL_RETRY_STATE_RETRY_AUTHORIZED);
    CHECK(contract.retries_authorized == 1);
    CHECK(nxgl_retry_contract_attempt_permitted(&contract) == 1);
    CHECK(nxgl_retry_contract_report(&contract, 0) ==
          NXGL_RETRY_DECISION_PROCEED);
    CHECK(contract.state == NXGL_RETRY_STATE_SUCCEEDED);
    CHECK(contract.failures_reported == 1);
  }

  /* 3. Fail then fail: TERMINAL. No third attempt is ever authorized, and
   * further reports (of either outcome) are refused without a state change --
   * a "success" after terminal cannot resurrect the machine. */
  {
    nxgl_retry_contract contract;

    nxgl_retry_contract_init(&contract);
    CHECK(nxgl_retry_contract_report(&contract, 1) ==
          NXGL_RETRY_DECISION_RETRY_ONCE);
    CHECK(nxgl_retry_contract_report(&contract, 1) ==
          NXGL_RETRY_DECISION_FAIL_TERMINAL);
    CHECK(contract.state == NXGL_RETRY_STATE_TERMINAL);
    CHECK(contract.failures_reported == 2);
    CHECK(contract.retries_authorized == 1);
    CHECK(nxgl_retry_contract_attempt_permitted(&contract) == 0);
    CHECK(nxgl_retry_contract_report(&contract, 1) ==
          NXGL_RETRY_DECISION_REFUSED);
    CHECK(nxgl_retry_contract_report(&contract, 0) ==
          NXGL_RETRY_DECISION_REFUSED);
    CHECK(contract.state == NXGL_RETRY_STATE_TERMINAL);
    CHECK(contract.failures_reported == 2);
  }

  /* 4. RETRY_ONCE can only ever be returned once per machine: exhaustive
   * walk of every report sequence up to length 4. */
  {
    int sequence;

    for (sequence = 0; sequence < 16; sequence++) {
      nxgl_retry_contract contract;
      int step;
      int retries_granted = 0;

      nxgl_retry_contract_init(&contract);
      for (step = 0; step < 4; step++) {
        int failed = (sequence >> step) & 1;

        if (nxgl_retry_contract_report(&contract, failed) ==
            NXGL_RETRY_DECISION_RETRY_ONCE) {
          retries_granted++;
        }
      }
      CHECK(retries_granted <= 1);
    }
  }

  /* 5. Malformed calls are refused and never move a machine. */
  {
    nxgl_retry_contract contract;

    nxgl_retry_contract_init(&contract);
    contract.api_version = 99;
    CHECK(nxgl_retry_contract_report(&contract, 1) ==
          NXGL_RETRY_DECISION_REFUSED);
    CHECK(nxgl_retry_contract_attempt_permitted(&contract) == 0);
    CHECK(nxgl_retry_contract_report(NULL, 1) == NXGL_RETRY_DECISION_REFUSED);
    CHECK(nxgl_retry_contract_attempt_permitted(NULL) == 0);
    nxgl_retry_contract_init(NULL); /* must not crash */
  }

  /* 6. State names are stable strings for receipts. */
  {
    CHECK(strcmp(nxgl_retry_state_name(NXGL_RETRY_STATE_FRESH), "fresh") == 0);
    CHECK(strcmp(nxgl_retry_state_name(NXGL_RETRY_STATE_RETRY_AUTHORIZED),
                 "retry-authorized") == 0);
    CHECK(strcmp(nxgl_retry_state_name(NXGL_RETRY_STATE_SUCCEEDED),
                 "succeeded") == 0);
    CHECK(strcmp(nxgl_retry_state_name(NXGL_RETRY_STATE_TERMINAL),
                 "terminal") == 0);
    CHECK(strcmp(nxgl_retry_state_name((nxgl_retry_state)77), "invalid") == 0);
  }

  if (g_failures != 0) {
    fprintf(stderr, "nxgl_v3_retry_contract=FAIL failures=%d\n", g_failures);
    return 1;
  }
  printf("nxgl_v3_retry_contract=PASS cases=6\n");
  return 0;
}
