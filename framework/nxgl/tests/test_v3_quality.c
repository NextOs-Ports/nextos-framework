/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test for nxgl_quality (V3 quality=low|medium|high lifecycle). */
#include "nxgl_quality.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void check(int cond, const char *msg) {
  if (!cond) {
    (void)fprintf(stderr, "nxgl_quality: %s\n", msg);
    exit(1);
  }
}

int main(void) {
  char buf[128];
  nxgl_quality_state s;

  /* parse: exact allowlist; everything else fails SAFE to AUTO. */
  check(nxgl_quality_parse("low") == NXGL_QUALITY_LOW, "parse low");
  check(nxgl_quality_parse("medium") == NXGL_QUALITY_MEDIUM, "parse medium");
  check(nxgl_quality_parse("high") == NXGL_QUALITY_HIGH, "parse high");
  check(nxgl_quality_parse("auto") == NXGL_QUALITY_AUTO, "parse auto");
  check(nxgl_quality_parse(NULL) == NXGL_QUALITY_AUTO, "parse null");
  check(nxgl_quality_parse("ultra") == NXGL_QUALITY_AUTO, "parse unknown");
  check(nxgl_quality_parse("HIGH") == NXGL_QUALITY_AUTO, "parse case");

  /* name round-trip. */
  check(strcmp(nxgl_quality_name(NXGL_QUALITY_LOW), "low") == 0, "name low");
  check(strcmp(nxgl_quality_name(NXGL_QUALITY_AUTO), "auto") == 0, "name auto");
  check(strcmp(nxgl_quality_name((nxgl_quality_level)999), "auto") == 0,
        "name out of range -> auto");

  /* resolve: concrete passes through; AUTO defers to the adapter recommend;
   * a non-concrete recommend floors to MEDIUM (never a device decision). */
  check(nxgl_quality_resolve(NXGL_QUALITY_HIGH, NXGL_QUALITY_LOW) ==
            NXGL_QUALITY_HIGH, "resolve concrete passthrough");
  check(nxgl_quality_resolve(NXGL_QUALITY_AUTO, NXGL_QUALITY_LOW) ==
            NXGL_QUALITY_LOW, "resolve auto -> recommend");
  check(nxgl_quality_resolve(NXGL_QUALITY_AUTO, NXGL_QUALITY_AUTO) ==
            NXGL_QUALITY_MEDIUM, "resolve auto+auto -> medium floor");

  /* lifecycle: init resolves; ready is refused before apply. */
  check(nxgl_quality_state_init(NULL, NXGL_QUALITY_LOW, NXGL_QUALITY_LOW) == -1,
        "init null rejected");
  check(nxgl_quality_state_init(&s, NXGL_QUALITY_AUTO, NXGL_QUALITY_HIGH) == 0,
        "init ok");
  check(s.resolved == NXGL_QUALITY_HIGH, "init resolved auto->high");
  check(s.applied == 0 && s.ready == 0, "init not applied/ready");
  check(nxgl_quality_state_ready(&s) == -1, "ready before apply refused");
  check(s.ready == 0, "ready flag stays 0 after refusal");
  check(nxgl_quality_state_apply(&s) == 0, "apply ok");
  check(s.applied == 1, "applied flag set");
  check(nxgl_quality_state_ready(&s) == 0, "ready after apply ok");
  check(s.ready == 1, "ready flag set");

  /* receipts: one line per stage, reflecting the real flags. */
  check(nxgl_quality_receipt(&s, NXGL_QUALITY_STAGE_RESOLVE, buf, sizeof buf) > 0,
        "resolve receipt");
  check(strcmp(buf,
               "QUALITY: stage=resolve requested=auto resolved=high "
               "applied=1 ready=1") == 0,
        "resolve receipt text");
  check(nxgl_quality_receipt(&s, NXGL_QUALITY_STAGE_READY, buf, sizeof buf) > 0,
        "ready receipt");
  check(strncmp(buf, "QUALITY: stage=ready ", 21) == 0, "ready receipt prefix");

  /* receipt fails closed on bad args / short buffer. */
  check(nxgl_quality_receipt(&s, NXGL_QUALITY_STAGE_RESOLVE, buf, 4u) == 0 &&
            buf[0] == '\0',
        "short buffer -> empty");
  check(nxgl_quality_receipt(NULL, NXGL_QUALITY_STAGE_RESOLVE, buf,
                             sizeof buf) == 0,
        "null state -> 0");

  (void)puts("nxgl_quality tests: ok (parse/resolve/lifecycle/receipt)");
  return 0;
}
