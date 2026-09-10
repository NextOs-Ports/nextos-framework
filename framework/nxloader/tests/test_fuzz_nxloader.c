/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nxloader.h"

#include <stddef.h>
#include <stdint.h>

#define FUZZ_MAX_FILE (2u * 1024u * 1024u)
#define FUZZ_MAX_IMAGE (8u * 1024u * 1024u)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  nxloader_config config;
  nxloader_module *module = NULL;
  nxloader_registry *registry = NULL;
  nxloader_result result;
  if (!data || size < 1 || size > FUZZ_MAX_FILE)
    return 0;
  nxloader_config_init(&config);
  config.flags = NXLOADER_CONFIG_ALLOW_FOREIGN_ARCH;
  config.max_file_size = FUZZ_MAX_FILE;
  config.max_image_size = FUZZ_MAX_IMAGE;
  config.trampoline_pool_size = 4096;
  if (nxloader_module_create(&config, &module) != NXLOADER_OK)
    return 0;
  result = nxloader_module_load_memory(module, data, size, "fuzz-input");
  if (result == NXLOADER_OK)
    result = nxloader_module_relocate(module);
  if (result == NXLOADER_OK &&
      nxloader_registry_create(&registry) == NXLOADER_OK) {
    result = nxloader_module_resolve(
        module, registry, NXLOADER_RESOLVE_ALLOW_UNRESOLVED, NULL);
    if (result == NXLOADER_OK)
      (void)nxloader_module_finalize(module);
  }
  /* Fuzzing must never execute guest initializers. */
  nxloader_registry_destroy(registry);
  nxloader_module_destroy(module);
  return 0;
}
