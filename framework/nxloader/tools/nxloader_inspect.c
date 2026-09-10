/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nxloader.h"

#include <stdio.h>
#include <string.h>

static void print_log(void *userdata, nxloader_log_level level,
                      const char *message) {
  static const char *const names[] = {"error", "warning", "info", "debug"};
  (void)userdata;
  fprintf(stderr, "nxloader[%s]: %s\n",
          level >= NXLOADER_LOG_ERROR && level <= NXLOADER_LOG_DEBUG
              ? names[level]
              : "unknown",
          message);
}

int main(int argc, char **argv) {
  nxloader_config config;
  nxloader_module *module = NULL;
  nxloader_module_info info;
  nxloader_result result;
  nxloader_registry *exports = NULL;
  nxloader_registry_report export_report;
  size_t index;
  int relocate = 0;
  int register_exports = 0;
  if (argc < 2 || argc > 3 ||
      (argc == 3 && strcmp(argv[2], "--relocate") != 0 &&
       strcmp(argv[2], "--exports") != 0)) {
    fprintf(stderr, "usage: %s ELF [--relocate|--exports]\n", argv[0]);
    return 2;
  }
  relocate = argc == 3;
  register_exports = argc == 3 && strcmp(argv[2], "--exports") == 0;
  nxloader_config_init(&config);
  config.flags = NXLOADER_CONFIG_ALLOW_FOREIGN_ARCH;
  config.log = print_log;
  result = nxloader_module_create(&config, &module);
  if (result == NXLOADER_OK)
    result = nxloader_module_load_file(module, argv[1]);
  if (result != NXLOADER_OK) {
    fprintf(stderr, "inspect failed: %s\n", nxloader_result_string(result));
    nxloader_module_destroy(module);
    return 1;
  }
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  result = nxloader_module_get_info(module, &info);
  if (result != NXLOADER_OK) {
    nxloader_module_destroy(module);
    return 1;
  }
  printf("arch=%s flags=0x%x arm_float_abi=%s image=%zu segments=%zu symbols=%zu "
         "relocations=%zu needed=%zu\n",
         nxloader_arch_string(info.arch), info.elf_flags,
         nxloader_arm_float_abi_string(info.arm_float_abi), info.image_size,
         info.segment_count, info.symbol_count, info.relocation_count,
         info.needed_count);
  printf("soname=%s\n", nxloader_module_soname(module)
                            ? nxloader_module_soname(module)
                            : "<none>");
  for (index = 0; index < nxloader_module_needed_count(module); ++index) {
    const char *needed = nxloader_module_needed(module, index);
    printf("needed[%zu]=%s\n", index, needed ? needed : "<invalid>");
  }
  if (relocate) {
    result = nxloader_module_relocate(module);
    printf("relocate=%s\n", nxloader_result_string(result));
  }
  if (result == NXLOADER_OK && register_exports) {
    memset(&export_report, 0, sizeof(export_report));
    export_report.struct_size = sizeof(export_report);
    result = nxloader_registry_create(&exports);
    if (result == NXLOADER_OK)
      result = nxloader_registry_add_module(exports, module, "guest-exports", 50,
                                            &export_report);
    printf("exports=%s added=%zu equivalent=%zu\n",
           nxloader_result_string(result), export_report.added,
           export_report.equivalent);
  }
  nxloader_registry_destroy(exports);
  nxloader_module_destroy(module);
  return result == NXLOADER_OK ? 0 : 1;
}
