/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Titan Souls' narrow ARMv7 adapter for the NextOS nxloader framework.
 *
 * nxloader owns ELF validation, relocation, symbol resolution, W^X, ARM
 * entry hooks and initializer ordering.  This adapter owns only the audited
 * two-module graph (FMOD Ex -> libTestSuite), its explicit providers and the
 * guest-specific ARM softfp contract.
 */

#ifndef TITANSOULS_TS_LOADER_H
#define TITANSOULS_TS_LOADER_H

#include <stddef.h>
#include <stdint.h>

#include <nxloader.h>

/* DynLibFunction is the Titan Souls adapter's narrow name/address table. */
#include "guest_import.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TS_LOADER_API_VERSION 1u

typedef struct ts_loader ts_loader;

typedef enum ts_loader_module_id {
  TS_LOADER_MODULE_NONE = -1,
  TS_LOADER_MODULE_FMOD = 0,
  TS_LOADER_MODULE_GAME = 1,
  TS_LOADER_MODULE_COUNT = 2
} ts_loader_module_id;

typedef enum ts_loader_stage {
  TS_LOADER_STAGE_EMPTY = 0,
  TS_LOADER_STAGE_REGISTRY_READY = 1,
  /* Both modules are loaded, locally relocated, registered as providers and
   * strictly resolved.  This is the only stage where hooks may be added. */
  TS_LOADER_STAGE_PREPARED = 2,
  TS_LOADER_STAGE_FINALIZED = 3,
  TS_LOADER_STAGE_INITIALIZED = 4,
  TS_LOADER_STAGE_ERROR = 5
} ts_loader_stage;

typedef struct ts_loader_config {
  size_t struct_size;
  uint32_t api_version;

  /* Borrowed only for ts_loader_create().  Every name/address is copied by
   * nxloader's registry before create returns. */
  const DynLibFunction *adapter_imports;
  size_t adapter_import_count;

  size_t max_guest_file_size;
  size_t max_guest_image_size;
  size_t trampoline_pool_size;

  nxloader_log_fn log;
  void *log_userdata;
} ts_loader_config;

void ts_loader_config_init(ts_loader_config *config);

/* Creates the explicit adapter/softfp/host registry.  The host provider is
 * constructed exclusively from host_symbols.inc; there is no generic dlsym
 * fallback.  Libraries supplying allowlisted host symbols must already be in
 * the process global scope and remain loaded for the loader lifetime. */
nxloader_result ts_loader_create(const ts_loader_config *config,
                                 ts_loader **out_loader);
void ts_loader_destroy(ts_loader *loader);

/* Fixed dependency order:
 *   libfmodex.so: load -> relocate -> register -> strict resolve
 *   libTestSuite.so: load -> relocate -> register -> strict resolve
 * Successful return leaves the loader in TS_LOADER_STAGE_PREPARED. */
nxloader_result ts_loader_prepare(ts_loader *loader, const char *fmod_path,
                                  const char *game_path);

/* ARM/Thumb entry patches are intentionally explicit.  nxloader's ARMv7
 * contract requires exactly eight audited bytes and accepts hooks only after
 * resolve and before finalize.  No callable-original trampoline is returned. */
nxloader_result ts_loader_install_hook(ts_loader *loader,
                                       ts_loader_module_id module_id,
                                       uintptr_t target,
                                       uintptr_t replacement,
                                       size_t available_bytes);
nxloader_result ts_loader_install_export_hook(
    ts_loader *loader, ts_loader_module_id module_id, const char *export_name,
    uintptr_t replacement, size_t available_bytes);

/* finalize applies segment permissions/GNU RELRO and closes the hook window.
 * Initializers run separately and in Android dependency order: FMOD, then the
 * game.  Neither function dispatches JNI_OnLoad or android_main implicitly. */
nxloader_result ts_loader_finalize(ts_loader *loader);
nxloader_result ts_loader_call_initializers(ts_loader *loader);

nxloader_result ts_loader_find_export(const ts_loader *loader,
                                      ts_loader_module_id module_id,
                                      const char *name,
                                      uintptr_t *address);
nxloader_result ts_loader_get_info(const ts_loader *loader,
                                   ts_loader_module_id module_id,
                                   nxloader_module_info *info);

/* EHABI lookup for a specific module, or for whichever of the two executable
 * mappings contains program_counter. */
nxloader_result ts_loader_find_arm_exidx(
    const ts_loader *loader, ts_loader_module_id module_id,
    uintptr_t program_counter, uintptr_t *table_address, size_t *entry_count);
nxloader_result ts_loader_find_arm_exidx_any(
    const ts_loader *loader, uintptr_t program_counter,
    ts_loader_module_id *module_id, uintptr_t *table_address,
    size_t *entry_count);

ts_loader_stage ts_loader_get_stage(const ts_loader *loader);
const char *ts_loader_last_error(const ts_loader *loader);
size_t ts_loader_host_allowlist_count(void);
size_t ts_loader_host_resolved_count(const ts_loader *loader);

#ifdef __cplusplus
}
#endif

#endif /* TITANSOULS_TS_LOADER_H */
