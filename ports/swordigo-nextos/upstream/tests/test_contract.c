/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "contract.h"

#include <stdio.h>
#include <stdlib.h>

static void require(int condition, const char *message) {
  if (!condition) {
    (void)fprintf(stderr, "Swordigo contract test: %s\n", message);
    exit(1);
  }
}

int main(void) {
  static const char list[] =
      "adapter.gl-provider-reexec-preload\n"
      "adapter.gl-provider-probe-init-reexec\r\n"
      "game.swordigo.present-alpha-one";

  require(swordigo_contract_has_token(
              list, "adapter.gl-provider-reexec-preload"),
          "LF token not found");
  require(swordigo_contract_has_token(
              list, "adapter.gl-provider-probe-init-reexec"),
          "CRLF token not found");
  require(swordigo_contract_has_token(
              list, "game.swordigo.present-alpha-one"),
          "final token not found");
  require(!swordigo_contract_has_token(
              list, "adapter.gl-provider-reexec"),
          "substring was accepted");
  require(!swordigo_contract_has_token(
              "alpha,beta", "alpha"),
          "comma-delimited input was accepted");
  require(!swordigo_contract_has_token(NULL, "alpha") &&
              !swordigo_contract_has_token(list, ""),
          "empty input was accepted");

  (void)unsetenv("NXCOMPAT_ENABLED_QUIRKS");
  require(!swordigo_contract_quirk_enabled(
              "game.swordigo.present-alpha-one"),
          "missing contract enabled a quirk");
  require(setenv("NXCOMPAT_ENABLED_QUIRKS", list, 1) == 0,
          "setenv failed");
  require(swordigo_contract_quirk_enabled(
              "game.swordigo.present-alpha-one"),
          "declared quirk was not enabled");
  require(!swordigo_contract_quirk_enabled(
              "game.swordigo.present-alpha-one-extra"),
          "near-match enabled a quirk");

  (void)puts("Swordigo declarative contract tests passed");
  return 0;
}
