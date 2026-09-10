/*
 * nxloader.h - versioned, context-based Android ELF loader core.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The relocation set intentionally follows the AArch64 and ARMv7 loaders
 * proven by the NextOS Bully2, Sonic 4 Episode II, KOTOR and TASM2 ports.
 */
#ifndef NXLOADER_H
#define NXLOADER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXLOADER_API_VERSION_MAJOR 1u
#define NXLOADER_API_VERSION_MINOR 3u
#define NXLOADER_API_VERSION \
  ((NXLOADER_API_VERSION_MAJOR << 16) | NXLOADER_API_VERSION_MINOR)
#define NXLOADER_VERSION_STRING "0.5.0"

typedef struct nxloader_module nxloader_module;
typedef struct nxloader_registry nxloader_registry;

typedef enum nxloader_result {
  NXLOADER_OK = 0,
  NXLOADER_EINVAL = -1,
  NXLOADER_ENOMEM = -2,
  NXLOADER_EIO = -3,
  NXLOADER_EFORMAT = -4,
  NXLOADER_EARCH = -5,
  NXLOADER_EBOUNDS = -6,
  NXLOADER_ESTATE = -7,
  NXLOADER_EPROTECT = -8,
  NXLOADER_ERELOC = -9,
  NXLOADER_EUNRESOLVED = -10,
  NXLOADER_ECOLLISION = -11,
  NXLOADER_EOVERFLOW = -12,
  NXLOADER_EUNSUPPORTED = -13,
  NXLOADER_ECALLBACK = -14,
  NXLOADER_EREENTRANT = -15
} nxloader_result;

typedef enum nxloader_arch {
  NXLOADER_ARCH_AUTO = 0,
  NXLOADER_ARCH_ARMV7 = 1,
  NXLOADER_ARCH_AARCH64 = 2
} nxloader_arch;

/* ARM EABI float calling convention as declared by e_flags. Android guests
 * frequently leave the bits unspecified; that state must never be guessed as
 * hard- or soft-float by device/firmware name. A port adapter may combine the
 * reported value with its audited guest contract. */
typedef enum nxloader_arm_float_abi {
  NXLOADER_ARM_FLOAT_ABI_NOT_APPLICABLE = 0,
  NXLOADER_ARM_FLOAT_ABI_UNSPECIFIED = 1,
  NXLOADER_ARM_FLOAT_ABI_SOFT = 2,
  NXLOADER_ARM_FLOAT_ABI_HARD = 3
} nxloader_arm_float_abi;

typedef enum nxloader_state {
  NXLOADER_STATE_EMPTY = 0,
  NXLOADER_STATE_LOADED = 1,
  NXLOADER_STATE_RELOCATED = 2,
  NXLOADER_STATE_RESOLVED = 3,
  NXLOADER_STATE_FINALIZED = 4,
  NXLOADER_STATE_INITIALIZED = 5,
  NXLOADER_STATE_ERROR = 6,
  /* Values above ERROR are additive API 1.3 lifecycle states. Existing state
   * values remain stable for source and log compatibility. */
  NXLOADER_STATE_INITIALIZING = 7,
  NXLOADER_STATE_JNI_LOADING = 8,
  NXLOADER_STATE_READY = 9
} nxloader_state;

typedef enum nxloader_log_level {
  NXLOADER_LOG_ERROR = 0,
  NXLOADER_LOG_WARNING = 1,
  NXLOADER_LOG_INFO = 2,
  NXLOADER_LOG_DEBUG = 3
} nxloader_log_level;

typedef void (*nxloader_log_fn)(void *userdata, nxloader_log_level level,
                                const char *message);

typedef enum nxloader_reloc_phase {
  NXLOADER_RELOC_PHASE_LOCAL = 1,
  NXLOADER_RELOC_PHASE_IMPORT = 2
} nxloader_reloc_phase;

typedef struct nxloader_reloc_info {
  size_t struct_size;
  nxloader_reloc_phase phase;
  nxloader_arch arch;
  uint32_t type;
  uint64_t target_vma;
  int64_t addend;
  const char *symbol;
  uint8_t symbol_defined;
  uint8_t symbol_weak;
  uint8_t reserved[6];
} nxloader_reloc_info;

/*
 * Relocation hooks are deliberately opt-in. USE_DEFAULT retains the loader's
 * checked ABI implementation. WRITE stores *value, SKIP leaves the target
 * untouched, and REJECT fails the phase before any relocation is committed.
 * ARM CALL/JUMP24/THM_CALL use only the checked default codec and do not invoke
 * this callback; use nxloader_module_install_hook() after resolve for a
 * deliberate function-entry patch.
 */
typedef enum nxloader_reloc_action {
  NXLOADER_RELOC_USE_DEFAULT = 0,
  NXLOADER_RELOC_WRITE = 1,
  NXLOADER_RELOC_SKIP = 2,
  NXLOADER_RELOC_REJECT = 3
} nxloader_reloc_action;

typedef nxloader_reloc_action (*nxloader_reloc_hook_fn)(
    void *userdata, const nxloader_module *module,
    const nxloader_reloc_info *relocation, uint64_t *value);

/* Return an alternate name, or NULL to retain name. The returned string must
 * remain valid until the enclosing nxloader API call returns. */
typedef const char *(*nxloader_alias_fn)(void *userdata,
                                         const nxloader_module *module,
                                         const char *name);

typedef enum nxloader_initializer_kind {
  NXLOADER_INITIALIZER_DT_INIT = 1,
  NXLOADER_INITIALIZER_INIT_ARRAY = 2
} nxloader_initializer_kind;

typedef struct nxloader_initializer_info {
  size_t struct_size;
  nxloader_initializer_kind kind;
  size_t index;
  uintptr_t address;
} nxloader_initializer_info;

typedef enum nxloader_initializer_action {
  NXLOADER_INITIALIZER_RUN = 0,
  NXLOADER_INITIALIZER_SKIP = 1,
  NXLOADER_INITIALIZER_REJECT = 2
} nxloader_initializer_action;

typedef nxloader_initializer_action (*nxloader_initializer_filter_fn)(
    void *userdata, const nxloader_module *module,
    const nxloader_initializer_info *initializer);

/* Log, alias, relocation and initializer callbacks are trusted adapter code.
 * Every nxloader API for the same module, including read-only queries and
 * destroy, is rejected defensively while one of these callbacks is active.
 * The violation is sticky until that callback returns: the enclosing API fails
 * with NXLOADER_EREENTRANT before publishing its transaction. Destroy is a
 * safe no-op. Calling APIs for another module remains valid. */

enum nxloader_config_flags {
  /* Inspection/tests only: permit loading an ABI different from the process. */
  NXLOADER_CONFIG_ALLOW_FOREIGN_ARCH = 1u << 0,
  /* Must be explicit because writable+executable guest segments weaken W^X. */
  NXLOADER_CONFIG_ALLOW_WX_SEGMENTS = 1u << 1,
  /* ARM branch dynamic relocations modify executable guest instructions.
   * Default-off and valid only for the narrowly validated CALL/JUMP codecs;
   * it never permits arbitrary text writes or permanent RWX pages. */
  NXLOADER_CONFIG_ALLOW_ARM_TEXT_RELOCS = 1u << 2
};

typedef struct nxloader_config {
  size_t struct_size;
  uint32_t api_version;
  nxloader_arch expected_arch;
  uint32_t flags;
  size_t max_file_size;
  size_t max_image_size;
  /* AArch64 entry hooks use 16 bytes each; ARM branch veneers use aligned
   * 8-byte slots. The pool is RW only before finalize and RX afterwards.
   * Zero disables trampoline/veneer allocation. */
  size_t trampoline_pool_size;
  nxloader_log_fn log;
  nxloader_alias_fn alias;
  nxloader_reloc_hook_fn relocation_hook;
  nxloader_initializer_filter_fn initializer_filter;
  void *userdata;
} nxloader_config;

typedef struct nxloader_module_info {
  size_t struct_size;
  nxloader_arch arch;
  nxloader_state state;
  uint32_t elf_flags;
  nxloader_arm_float_abi arm_float_abi;
  void *mapping_base;
  size_t mapping_size;
  size_t image_size;
  size_t load_alignment;
  uint64_t minimum_vma;
  uint64_t maximum_vma;
  size_t segment_count;
  size_t symbol_count;
  size_t relocation_count;
  size_t needed_count;
} nxloader_module_info;

typedef struct nxloader_resolution_report {
  size_t struct_size;
  size_t imports_resolved;
  size_t weak_imports_zeroed;
  size_t unresolved_strong;
  /* Borrowed from the module; valid until module destruction. */
  const char *first_unresolved;
} nxloader_resolution_report;

typedef struct nxloader_symbol {
  /* Non-empty byte string, NUL-terminated within 4,096 bytes. */
  const char *name;
  uintptr_t address;
  uint32_t flags;
} nxloader_symbol;

enum nxloader_symbol_flags {
  NXLOADER_SYMBOL_WEAK = 1u << 0
};

typedef struct nxloader_provider {
  size_t struct_size;
  /* Non-empty byte string, NUL-terminated within 4,096 bytes. */
  const char *name;
  const nxloader_symbol *symbols;
  size_t symbol_count;
  int priority;
} nxloader_provider;

typedef struct nxloader_registry_report {
  size_t struct_size;
  size_t added;
  size_t replaced_lower_priority;
  size_t ignored_lower_priority;
  size_t equivalent;
} nxloader_registry_report;

typedef struct nxloader_registry_match {
  size_t struct_size;
  uintptr_t address;
  int priority;
  uint32_t flags;
  /* Borrowed from the registry; invalidated by mutation or destruction. */
  const char *provider;
} nxloader_registry_match;

enum nxloader_resolve_flags {
  /* Keep unresolved strong slots untouched and permit finalize. Intended only
   * for controlled bring-up; production ports should use the default (zero). */
  NXLOADER_RESOLVE_ALLOW_UNRESOLVED = 1u << 0
};

#define NXLOADER_JNI_ONLOAD_MAX_ACCEPTED_VERSIONS 16u

enum nxloader_jni_onload_flags {
  /* Succeed and enter READY when this exact module has no JNI_OnLoad export.
   * If the export exists, it is always invoked and its returned version must
   * appear in accepted_versions. */
  NXLOADER_JNI_ONLOAD_OPTIONAL = 1u << 0
};

typedef struct nxloader_jni_onload_options {
  size_t struct_size;
  /* Opaque adapter-owned JavaVM. NULL is rejected. */
  void *java_vm;
  /* JNI reserves this argument and requires NULL. */
  void *reserved;
  /* Exact positive-version allowlist supplied by the engine adapter. */
  const int32_t *accepted_versions;
  size_t accepted_version_count;
  uint32_t flags;
} nxloader_jni_onload_options;

/* Configuration and lifetime. */
void nxloader_config_init(nxloader_config *config);
nxloader_result nxloader_module_create(const nxloader_config *config,
                                       nxloader_module **out_module);
/* Safe no-op during a host callback for this module, INITIALIZING or
 * JNI_LOADING. It never invokes guest teardown. */
void nxloader_module_destroy(nxloader_module *module);

/* Explicit native-loader lifecycle. No phase is run implicitly. */
nxloader_result nxloader_module_load_file(nxloader_module *module,
                                          const char *path);
nxloader_result nxloader_module_load_memory(nxloader_module *module,
                                            const void *data, size_t size,
                                            const char *debug_name);
nxloader_result nxloader_module_relocate(nxloader_module *module);
nxloader_result nxloader_module_resolve(
    nxloader_module *module, const nxloader_registry *registry, uint32_t flags,
    nxloader_resolution_report *report);
nxloader_result nxloader_module_finalize(nxloader_module *module);
/* Preflights the complete DT_INIT then INIT_ARRAY plan, including every filter
 * decision, before entering INITIALIZING or executing any guest address. */
nxloader_result nxloader_module_call_initializers(nxloader_module *module);
/* Explicit Android ordering boundary. Valid only after INITIALIZED. The core
 * performs a literal, non-aliased JNI_OnLoad lookup and never invokes it
 * implicitly. READY is published only for an allowed returned version or an
 * explicitly OPTIONAL absent export. */
nxloader_result nxloader_module_call_jni_onload(
    nxloader_module *module, const nxloader_jni_onload_options *options,
    int32_t *out_version);

nxloader_result nxloader_module_get_info(const nxloader_module *module,
                                         nxloader_module_info *info);
nxloader_state nxloader_module_get_state(const nxloader_module *module);
nxloader_arch nxloader_module_get_arch(const nxloader_module *module);

/* Guest image introspection. */
void *nxloader_module_vma_to_pointer(const nxloader_module *module,
                                     uint64_t vma, size_t size);
nxloader_result nxloader_module_find_export(const nxloader_module *module,
                                            const char *name,
                                            uintptr_t *address);
nxloader_result nxloader_module_find_relocation(const nxloader_module *module,
                                                const char *name,
                                                uintptr_t *slot_address);
size_t nxloader_module_needed_count(const nxloader_module *module);
const char *nxloader_module_needed(const nxloader_module *module, size_t index);
/* Borrowed from the module; NULL means the ELF declared no DT_SONAME. */
const char *nxloader_module_soname(const nxloader_module *module);
/* ARM EHABI support for a host __gnu_Unwind_Find_exidx bridge. */
nxloader_result nxloader_module_find_arm_exidx(
    const nxloader_module *module, uintptr_t program_counter,
    uintptr_t *table_address, size_t *entry_count);

/* Optional function-entry patching. Must run before finalize. `available_bytes`
 * makes the overwrite contract explicit (4 for AArch64, 8 for ARM/Thumb). */
nxloader_result nxloader_module_install_hook(nxloader_module *module,
                                             uintptr_t target,
                                             uintptr_t destination,
                                             size_t available_bytes);

/* Explicit import providers and auxiliary guest modules. Registration is
 * atomic: a same-priority, same-strength, different-address collision changes
 * nothing. A module registered as a provider must outlive the registry and all
 * consumers resolved from its addresses; destruction does not invalidate or
 * remove its registry entries automatically. */
nxloader_result nxloader_registry_create(nxloader_registry **out_registry);
void nxloader_registry_destroy(nxloader_registry *registry);
nxloader_result nxloader_registry_add_provider(
    nxloader_registry *registry, const nxloader_provider *provider,
    nxloader_registry_report *report);
nxloader_result nxloader_registry_add_module(nxloader_registry *registry,
                                             const nxloader_module *module,
                                             const char *provider_name,
                                             int priority,
                                             nxloader_registry_report *report);
nxloader_result nxloader_registry_lookup(const nxloader_registry *registry,
                                         const char *symbol,
                                         nxloader_registry_match *match);

nxloader_arch nxloader_process_arch(void);
const char *nxloader_result_string(nxloader_result result);
const char *nxloader_arch_string(nxloader_arch arch);
const char *nxloader_arm_float_abi_string(nxloader_arm_float_abi abi);
const char *nxloader_state_string(nxloader_state state);

#ifdef __cplusplus
}
#endif

#endif /* NXLOADER_H */
