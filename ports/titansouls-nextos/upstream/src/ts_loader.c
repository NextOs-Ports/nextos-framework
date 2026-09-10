/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Strict two-module nxloader adapter for Titan Souls (ARMv7 softfp guest). */

#define _GNU_SOURCE
#include "ts_loader.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nxloader_softfp.h>

#define TS_DEFAULT_MAX_FILE_SIZE (16u * 1024u * 1024u)
#define TS_DEFAULT_MAX_IMAGE_SIZE (64u * 1024u * 1024u)
#define TS_DEFAULT_TRAMPOLINE_POOL (64u * 1024u)
#define TS_MAX_ADAPTER_IMPORTS 4096u
#define TS_MAX_DYNAMIC_NAME 4096u

/* Priority is guest policy, not nxloader policy.  Adapter interposition beats
 * guest exports; the shared softfp bridge beats legacy duplicate libm entries;
 * FMOD beats the game for its public API; direct host symbols are last. */
#define TS_PROVIDER_PRIORITY_SOFTFP 400
#define TS_PROVIDER_PRIORITY_ADAPTER 300
#define TS_PROVIDER_PRIORITY_FMOD 200
#define TS_PROVIDER_PRIORITY_GAME 100
#define TS_PROVIDER_PRIORITY_HOST 0

#define TS_FMOD_SONAME "libfmodex.so"
#define TS_GAME_SONAME "libTestSuite.so"

#define TS_HOST_SYMBOL(name) name,
static const char *const ts_host_allowlist[] = {
#include "host_symbols.inc"
};
#undef TS_HOST_SYMBOL

typedef struct ts_loader_slot {
  nxloader_module *module;
  nxloader_module_info info;
  nxloader_resolution_report resolution;
  int registered;
  int resolved;
  int finalized;
  int initialized;
} ts_loader_slot;

struct ts_loader {
  nxloader_registry *registry;
  ts_loader_slot slots[TS_LOADER_MODULE_COUNT];
  ts_loader_stage stage;
  nxloader_log_fn log;
  void *log_userdata;
  size_t max_guest_file_size;
  size_t max_guest_image_size;
  size_t trampoline_pool_size;
  size_t host_resolved_count;
  char last_error[512];
};

static const char *ts_module_name(ts_loader_module_id module_id) {
  switch (module_id) {
  case TS_LOADER_MODULE_FMOD:
    return TS_FMOD_SONAME;
  case TS_LOADER_MODULE_GAME:
    return TS_GAME_SONAME;
  case TS_LOADER_MODULE_NONE:
  case TS_LOADER_MODULE_COUNT:
  default:
    return "(none)";
  }
}

static int ts_module_id_valid(ts_loader_module_id module_id) {
  return module_id == TS_LOADER_MODULE_FMOD ||
         module_id == TS_LOADER_MODULE_GAME;
}

static int ts_name_valid(const char *name) {
  return name && name[0] &&
         memchr(name, '\0', (size_t)TS_MAX_DYNAMIC_NAME + 1u) != NULL;
}

static void ts_emit(ts_loader *loader, nxloader_log_level level,
                    const char *format, ...) {
  char message[512];
  va_list arguments;
  if (!loader || !loader->log || !format)
    return;
  va_start(arguments, format);
  (void)vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  loader->log(loader->log_userdata, level, message);
}

static void ts_core_log(void *userdata, nxloader_log_level level,
                        const char *message) {
  ts_loader *loader = (ts_loader *)userdata;
  if (loader && loader->log)
    loader->log(loader->log_userdata, level,
                message ? message : "(no nxloader diagnostic)");
}

static nxloader_result ts_fail(ts_loader *loader,
                               ts_loader_module_id module_id,
                               const char *phase, nxloader_result result,
                               const char *detail) {
  if (loader) {
    loader->stage = TS_LOADER_STAGE_ERROR;
    (void)snprintf(loader->last_error, sizeof(loader->last_error),
                   "%s/%s: %s%s%s", ts_module_name(module_id),
                   phase ? phase : "unknown", nxloader_result_string(result),
                   detail && detail[0] ? ": " : "",
                   detail && detail[0] ? detail : "");
    ts_emit(loader, NXLOADER_LOG_ERROR, "%s", loader->last_error);
  }
  return result;
}

static void ts_clear_error(ts_loader *loader) {
  if (loader)
    (void)snprintf(loader->last_error, sizeof(loader->last_error), "success");
}

/* libfmodex.so imports this Android log entry point strongly.  ArkOS has no
 * liblog, so resolving it through RTLD_DEFAULT would either fail or depend on
 * an accidental process export.  Keep the compatibility local, finite and
 * non-variadic; __android_log_print/vprint remain owned by imports.c. */
NXLOADER_ARM_SOFTFP static int ts_android_log_write(int priority,
                                                    const char *tag,
                                                    const char *message) {
  char line[768];
  int written = snprintf(line, sizeof(line), "[android/%d] %s: %s\n",
                         priority, tag ? tag : "?",
                         message ? message : "(null)");
  if (written > 0) {
    size_t length = (size_t)written;
    if (length >= sizeof(line))
      length = sizeof(line) - 1u;
    (void)fwrite(line, 1u, length, stderr);
  }
  return 1;
}

static nxloader_result ts_add_adapter_provider(
    ts_loader *loader, const DynLibFunction *imports, size_t import_count) {
  nxloader_symbol *symbols;
  nxloader_provider provider;
  nxloader_registry_report report;
  nxloader_result result;
  size_t symbol_count;
  size_t index;
  int has_android_log_write = 0;

  if (!loader || !imports || import_count == 0u ||
      import_count > TS_MAX_ADAPTER_IMPORTS ||
      import_count >= TS_MAX_ADAPTER_IMPORTS ||
      import_count >= SIZE_MAX / sizeof(*symbols))
    return NXLOADER_EINVAL;

  symbol_count = import_count;
  for (index = 0u; index < import_count; ++index) {
    if (!ts_name_valid(imports[index].symbol) || imports[index].func == 0u)
      return NXLOADER_EINVAL;
    if (strcmp(imports[index].symbol, "__android_log_write") == 0)
      has_android_log_write = 1;
  }
  if (!has_android_log_write)
    ++symbol_count;
  symbols = (nxloader_symbol *)calloc(symbol_count, sizeof(*symbols));
  if (!symbols)
    return NXLOADER_ENOMEM;
  for (index = 0u; index < import_count; ++index) {
    symbols[index].name = imports[index].symbol;
    symbols[index].address = imports[index].func;
  }
  if (!has_android_log_write) {
    __typeof__(&ts_android_log_write) function_pointer =
        &ts_android_log_write;
    uintptr_t function_address = 0u;
    if (sizeof(function_pointer) != sizeof(function_address)) {
      free(symbols);
      return NXLOADER_EUNSUPPORTED;
    }
    memcpy(&function_address, &function_pointer, sizeof(function_address));
    symbols[import_count].name = "__android_log_write";
    symbols[import_count].address = function_address;
  }

  memset(&provider, 0, sizeof(provider));
  provider.struct_size = sizeof(provider);
  provider.name = "titansouls-adapter-v1";
  provider.symbols = symbols;
  provider.symbol_count = symbol_count;
  provider.priority = TS_PROVIDER_PRIORITY_ADAPTER;
  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  result = nxloader_registry_add_provider(loader->registry, &provider, &report);
  free(symbols);
  if (result == NXLOADER_OK)
    ts_emit(loader, NXLOADER_LOG_INFO,
            "adapter provider: %zu symbols, %zu equivalent%s", report.added,
            report.equivalent,
            has_android_log_write ? "" : ", builtin=__android_log_write");
  return result;
}

static nxloader_result ts_add_host_provider(ts_loader *loader) {
  const size_t allowlist_count =
      sizeof(ts_host_allowlist) / sizeof(ts_host_allowlist[0]);
  nxloader_symbol *symbols;
  nxloader_provider provider;
  nxloader_registry_report report;
  nxloader_result result;
  size_t found = 0u;
  size_t index;

  if (!loader || allowlist_count > SIZE_MAX / sizeof(*symbols))
    return NXLOADER_EINVAL;
  symbols = (nxloader_symbol *)calloc(allowlist_count, sizeof(*symbols));
  if (!symbols)
    return NXLOADER_ENOMEM;

  for (index = 0u; index < allowlist_count; ++index) {
    void *pointer;
    uintptr_t address = 0u;
    (void)dlerror();
    pointer = dlsym(RTLD_DEFAULT, ts_host_allowlist[index]);
    if (!pointer) {
      ts_emit(loader, NXLOADER_LOG_DEBUG,
              "allowlisted host symbol unavailable: %s",
              ts_host_allowlist[index]);
      continue;
    }
    memcpy(&address, &pointer, sizeof(address));
    if (address == 0u)
      continue;
    symbols[found].name = ts_host_allowlist[index];
    symbols[found].address = address;
    ++found;
  }

  memset(&provider, 0, sizeof(provider));
  provider.struct_size = sizeof(provider);
  provider.name = "titansouls-explicit-host-v1";
  provider.symbols = symbols;
  provider.symbol_count = found;
  provider.priority = TS_PROVIDER_PRIORITY_HOST;
  memset(&report, 0, sizeof(report));
  report.struct_size = sizeof(report);
  result = nxloader_registry_add_provider(loader->registry, &provider, &report);
  free(symbols);
  if (result == NXLOADER_OK) {
    loader->host_resolved_count = found;
    ts_emit(loader, NXLOADER_LOG_INFO,
            "host provider: %zu/%zu allowlisted symbols visible", found,
            allowlist_count);
  }
  return result;
}

static nxloader_result ts_create_guest_module(ts_loader *loader,
                                               nxloader_module **out_module) {
  nxloader_config config;
  if (!loader || !out_module)
    return NXLOADER_EINVAL;
  *out_module = NULL;
  nxloader_config_init(&config);
  config.expected_arch = NXLOADER_ARCH_ARMV7;
  /* File-backed RX pages are an optional residency optimization.  No
   * foreign-architecture, W+X or ARM text-relocation opt-in is enabled. */
  config.flags = NXLOADER_CONFIG_FILE_BACKED_TEXT;
  config.max_file_size = loader->max_guest_file_size;
  config.max_image_size = loader->max_guest_image_size;
  config.trampoline_pool_size = loader->trampoline_pool_size;
  config.log = ts_core_log;
  config.userdata = loader;
  return nxloader_module_create(&config, out_module);
}

static nxloader_result ts_validate_guest(ts_loader *loader,
                                         ts_loader_module_id module_id,
                                         nxloader_module *module,
                                         const char *expected_soname) {
  nxloader_module_info info;
  const char *soname;
  nxloader_result result;

  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  result = nxloader_module_get_info(module, &info);
  if (result != NXLOADER_OK)
    return result;
  if (info.arch != NXLOADER_ARCH_ARMV7)
    return NXLOADER_EARCH;
  /* Both owner-verified 1.0.3 guests omit EF_ARM_ABI_FLOAT_* and were audited
   * as softfp.  A different declaration is a different guest contract. */
  if (info.arm_float_abi != NXLOADER_ARM_FLOAT_ABI_UNSPECIFIED)
    return NXLOADER_EFORMAT;
  if (info.state != NXLOADER_STATE_LOADED || !info.mapping_base ||
      info.mapping_size == 0u || info.image_size == 0u ||
      info.segment_count == 0u)
    return NXLOADER_EFORMAT;
  soname = nxloader_module_soname(module);
  if (!soname || strcmp(soname, expected_soname) != 0)
    return NXLOADER_EFORMAT;
  loader->slots[module_id].info = info;
  return NXLOADER_OK;
}

static nxloader_result ts_prepare_module(ts_loader *loader,
                                         ts_loader_module_id module_id,
                                         const char *path,
                                         const char *expected_soname,
                                         int provider_priority) {
  ts_loader_slot *slot;
  nxloader_registry_report registry_report;
  nxloader_result result;

  if (!loader || !ts_module_id_valid(module_id) || !path || !path[0])
    return NXLOADER_EINVAL;
  slot = &loader->slots[module_id];
  result = ts_create_guest_module(loader, &slot->module);
  if (result != NXLOADER_OK)
    return ts_fail(loader, module_id, "create", result, NULL);
  result = nxloader_module_load_file(slot->module, path);
  if (result != NXLOADER_OK)
    return ts_fail(loader, module_id, "load", result, path);
  result = ts_validate_guest(loader, module_id, slot->module,
                             expected_soname);
  if (result != NXLOADER_OK)
    return ts_fail(loader, module_id, "guest-contract", result,
                   "expected ARMv7, float ABI unspecified and exact SONAME");
  result = nxloader_module_relocate(slot->module);
  if (result != NXLOADER_OK)
    return ts_fail(loader, module_id, "relocate", result, NULL);

  memset(&registry_report, 0, sizeof(registry_report));
  registry_report.struct_size = sizeof(registry_report);
  result = nxloader_registry_add_module(loader->registry, slot->module,
                                        expected_soname, provider_priority,
                                        &registry_report);
  if (result != NXLOADER_OK)
    return ts_fail(loader, module_id, "register-exports", result, NULL);
  slot->registered = 1;

  memset(&slot->resolution, 0, sizeof(slot->resolution));
  slot->resolution.struct_size = sizeof(slot->resolution);
  result = nxloader_module_resolve(slot->module, loader->registry, 0u,
                                   &slot->resolution);
  if (result != NXLOADER_OK) {
    const char *first = slot->resolution.first_unresolved;
    return ts_fail(loader, module_id, "strict-resolve", result,
                   first ? first : "no symbol reported");
  }
  slot->resolved = 1;
  ts_emit(loader, NXLOADER_LOG_INFO,
          "%s prepared: exports=%zu imports=%zu weak-zero=%zu",
          expected_soname, registry_report.added,
          slot->resolution.imports_resolved,
          slot->resolution.weak_imports_zeroed);
  return NXLOADER_OK;
}

void ts_loader_config_init(ts_loader_config *config) {
  if (!config)
    return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->api_version = TS_LOADER_API_VERSION;
  config->max_guest_file_size = TS_DEFAULT_MAX_FILE_SIZE;
  config->max_guest_image_size = TS_DEFAULT_MAX_IMAGE_SIZE;
  config->trampoline_pool_size = TS_DEFAULT_TRAMPOLINE_POOL;
}

nxloader_result ts_loader_create(const ts_loader_config *config,
                                 ts_loader **out_loader) {
  ts_loader *loader;
  nxloader_registry_report softfp_report;
  nxloader_result result;

  if (!out_loader)
    return NXLOADER_EINVAL;
  *out_loader = NULL;
  if (!config || config->struct_size < sizeof(*config) ||
      config->api_version != TS_LOADER_API_VERSION ||
      !config->adapter_imports || config->adapter_import_count == 0u ||
      config->max_guest_file_size == 0u ||
      config->max_guest_image_size == 0u ||
      config->trampoline_pool_size == 0u)
    return NXLOADER_EINVAL;
  if (nxloader_process_arch() != NXLOADER_ARCH_ARMV7)
    return NXLOADER_EARCH;

  loader = (ts_loader *)calloc(1, sizeof(*loader));
  if (!loader)
    return NXLOADER_ENOMEM;
  loader->stage = TS_LOADER_STAGE_EMPTY;
  loader->log = config->log;
  loader->log_userdata = config->log_userdata;
  loader->max_guest_file_size = config->max_guest_file_size;
  loader->max_guest_image_size = config->max_guest_image_size;
  loader->trampoline_pool_size = config->trampoline_pool_size;
  ts_clear_error(loader);

  result = nxloader_registry_create(&loader->registry);
  if (result != NXLOADER_OK)
    goto fail;
  result = ts_add_host_provider(loader);
  if (result != NXLOADER_OK)
    goto fail;
  result = ts_add_adapter_provider(loader, config->adapter_imports,
                                   config->adapter_import_count);
  if (result != NXLOADER_OK)
    goto fail;
  memset(&softfp_report, 0, sizeof(softfp_report));
  softfp_report.struct_size = sizeof(softfp_report);
  result = nxloader_softfp_add_libm(loader->registry,
                                    "titansouls-nxloader-softfp-v1",
                                    TS_PROVIDER_PRIORITY_SOFTFP,
                                    &softfp_report);
  if (result != NXLOADER_OK)
    goto fail;

  loader->stage = TS_LOADER_STAGE_REGISTRY_READY;
  ts_emit(loader, NXLOADER_LOG_INFO,
          "softfp provider: added=%zu replaced=%zu", softfp_report.added,
          softfp_report.replaced_lower_priority);
  *out_loader = loader;
  return NXLOADER_OK;

fail:
  (void)ts_fail(loader, TS_LOADER_MODULE_NONE, "registry", result, NULL);
  ts_loader_destroy(loader);
  return result;
}

void ts_loader_destroy(ts_loader *loader) {
  if (!loader)
    return;
  /* Registry first, then consumer before provider: registered module exports
   * are borrowed addresses and nxloader deliberately performs no guest fini. */
  nxloader_registry_destroy(loader->registry);
  loader->registry = NULL;
  nxloader_module_destroy(loader->slots[TS_LOADER_MODULE_GAME].module);
  loader->slots[TS_LOADER_MODULE_GAME].module = NULL;
  nxloader_module_destroy(loader->slots[TS_LOADER_MODULE_FMOD].module);
  loader->slots[TS_LOADER_MODULE_FMOD].module = NULL;
  free(loader);
}

nxloader_result ts_loader_prepare(ts_loader *loader, const char *fmod_path,
                                  const char *game_path) {
  nxloader_result result;
  if (!loader || !fmod_path || !game_path)
    return NXLOADER_EINVAL;
  if (loader->stage != TS_LOADER_STAGE_REGISTRY_READY)
    return NXLOADER_ESTATE;
  ts_clear_error(loader);

  result = ts_prepare_module(loader, TS_LOADER_MODULE_FMOD, fmod_path,
                             TS_FMOD_SONAME, TS_PROVIDER_PRIORITY_FMOD);
  if (result != NXLOADER_OK)
    return result;
  result = ts_prepare_module(loader, TS_LOADER_MODULE_GAME, game_path,
                             TS_GAME_SONAME, TS_PROVIDER_PRIORITY_GAME);
  if (result != NXLOADER_OK)
    return result;
  loader->stage = TS_LOADER_STAGE_PREPARED;
  return NXLOADER_OK;
}

nxloader_result ts_loader_install_hook(ts_loader *loader,
                                       ts_loader_module_id module_id,
                                       uintptr_t target,
                                       uintptr_t replacement,
                                       size_t available_bytes) {
  nxloader_result result;
  if (!loader || !ts_module_id_valid(module_id) || target == 0u ||
      replacement == 0u || available_bytes != 8u)
    return NXLOADER_EINVAL;
  if (loader->stage != TS_LOADER_STAGE_PREPARED ||
      !loader->slots[module_id].resolved)
    return NXLOADER_ESTATE;
  result = nxloader_module_install_hook(loader->slots[module_id].module,
                                        target, replacement, available_bytes);
  if (result != NXLOADER_OK)
    return ts_fail(loader, module_id, "install-hook", result, NULL);
  return NXLOADER_OK;
}

nxloader_result ts_loader_install_export_hook(
    ts_loader *loader, ts_loader_module_id module_id, const char *export_name,
    uintptr_t replacement, size_t available_bytes) {
  uintptr_t target = 0u;
  nxloader_result result;
  if (!loader || !ts_name_valid(export_name))
    return NXLOADER_EINVAL;
  result = ts_loader_find_export(loader, module_id, export_name, &target);
  if (result != NXLOADER_OK)
    return result;
  return ts_loader_install_hook(loader, module_id, target, replacement,
                                available_bytes);
}

nxloader_result ts_loader_finalize(ts_loader *loader) {
  size_t index;
  nxloader_result result;
  if (!loader)
    return NXLOADER_EINVAL;
  if (loader->stage != TS_LOADER_STAGE_PREPARED)
    return NXLOADER_ESTATE;
  for (index = 0u; index < TS_LOADER_MODULE_COUNT; ++index) {
    result = nxloader_module_finalize(loader->slots[index].module);
    if (result != NXLOADER_OK)
      return ts_fail(loader, (ts_loader_module_id)index, "finalize", result,
                     NULL);
    loader->slots[index].finalized = 1;
  }
  loader->stage = TS_LOADER_STAGE_FINALIZED;
  return NXLOADER_OK;
}

nxloader_result ts_loader_call_initializers(ts_loader *loader) {
  size_t index;
  nxloader_result result;
  if (!loader)
    return NXLOADER_EINVAL;
  if (loader->stage != TS_LOADER_STAGE_FINALIZED)
    return NXLOADER_ESTATE;
  for (index = 0u; index < TS_LOADER_MODULE_COUNT; ++index) {
    result = nxloader_module_call_initializers(loader->slots[index].module);
    if (result != NXLOADER_OK)
      return ts_fail(loader, (ts_loader_module_id)index, "initializers",
                     result, NULL);
    loader->slots[index].initialized = 1;
  }
  loader->stage = TS_LOADER_STAGE_INITIALIZED;
  return NXLOADER_OK;
}

nxloader_result ts_loader_find_export(const ts_loader *loader,
                                      ts_loader_module_id module_id,
                                      const char *name,
                                      uintptr_t *address) {
  if (!loader || !ts_module_id_valid(module_id) || !ts_name_valid(name) ||
      !address || !loader->slots[module_id].module)
    return NXLOADER_EINVAL;
  return nxloader_module_find_export(loader->slots[module_id].module, name,
                                     address);
}

nxloader_result ts_loader_get_info(const ts_loader *loader,
                                   ts_loader_module_id module_id,
                                   nxloader_module_info *info) {
  if (!loader || !ts_module_id_valid(module_id) || !info ||
      !loader->slots[module_id].module)
    return NXLOADER_EINVAL;
  return nxloader_module_get_info(loader->slots[module_id].module, info);
}

nxloader_result ts_loader_find_arm_exidx(
    const ts_loader *loader, ts_loader_module_id module_id,
    uintptr_t program_counter, uintptr_t *table_address, size_t *entry_count) {
  if (!loader || !ts_module_id_valid(module_id) || !table_address ||
      !entry_count || !loader->slots[module_id].module)
    return NXLOADER_EINVAL;
  return nxloader_module_find_arm_exidx(loader->slots[module_id].module,
                                        program_counter, table_address,
                                        entry_count);
}

nxloader_result ts_loader_find_arm_exidx_any(
    const ts_loader *loader, uintptr_t program_counter,
    ts_loader_module_id *module_id, uintptr_t *table_address,
    size_t *entry_count) {
  size_t index;
  nxloader_result result = NXLOADER_EUNRESOLVED;
  if (!loader || !module_id || !table_address || !entry_count)
    return NXLOADER_EINVAL;
  *module_id = TS_LOADER_MODULE_NONE;
  *table_address = 0u;
  *entry_count = 0u;
  for (index = 0u; index < TS_LOADER_MODULE_COUNT; ++index) {
    if (!loader->slots[index].module)
      continue;
    result = nxloader_module_find_arm_exidx(loader->slots[index].module,
                                            program_counter, table_address,
                                            entry_count);
    if (result == NXLOADER_OK) {
      *module_id = (ts_loader_module_id)index;
      return NXLOADER_OK;
    }
    if (result != NXLOADER_EUNRESOLVED)
      return result;
  }
  return NXLOADER_EUNRESOLVED;
}

ts_loader_stage ts_loader_get_stage(const ts_loader *loader) {
  return loader ? loader->stage : TS_LOADER_STAGE_ERROR;
}

const char *ts_loader_last_error(const ts_loader *loader) {
  return loader ? loader->last_error : "invalid loader";
}

size_t ts_loader_host_allowlist_count(void) {
  return sizeof(ts_host_allowlist) / sizeof(ts_host_allowlist[0]);
}

size_t ts_loader_host_resolved_count(const ts_loader *loader) {
  return loader ? loader->host_resolved_count : 0u;
}
