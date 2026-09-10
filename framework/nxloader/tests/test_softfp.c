/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nxloader_softfp.h"

#include <stdio.h>
#include <string.h>

int main(void) {
  nxloader_registry *registry = NULL;
  nxloader_registry_report report;
  nxloader_registry_match match;
  nxloader_result result;
  if (nxloader_process_arch() != NXLOADER_ARCH_ARMV7)
    return 77;
  result = nxloader_registry_create(&registry);
  if (result != NXLOADER_OK)
    return 1;
  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  result = nxloader_softfp_add_libm(registry, "armv7-softfp-libm", 100,
                                    &report);
  if (result != NXLOADER_OK || report.added < 60) {
    nxloader_registry_destroy(registry);
    return 1;
  }
  memset(&match, 0, sizeof(match));
  match.struct_size = sizeof(match);
  result = nxloader_registry_lookup(registry, "sinf", &match);
  if (result != NXLOADER_OK || match.address == 0 ||
      strcmp(match.provider, "armv7-softfp-libm") != 0) {
    nxloader_registry_destroy(registry);
    return 1;
  }
  nxloader_registry_destroy(registry);
  puts("nxloader: ARMv7 softfp provider test passed");
  return 0;
}
