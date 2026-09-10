/* SPDX-License-Identifier: GPL-3.0-only */
/* V3-GRAPHICS-01 harness: prints the GLES1 selection receipt INCLUDING the
 * per-candidate trace, so the runner can assert (a) the LIVE candidate wins in
 * both mirrored orders, (b) every losing candidate's failure reason is named,
 * and (c) a healthy first candidate means the later rung is never probed. */
#include "nxgl_gles1.h"

#include <stdio.h>

int main(void) {
  nxgl_gles1_receipt receipt;
  int rc = nxgl_gles1_init(&receipt);

  printf("rc=%d provider=%s liveness=%s trace=[%s]\n", rc, receipt.provider,
         nxgl_gles1_liveness(), receipt.candidates);
  return 0;
}
