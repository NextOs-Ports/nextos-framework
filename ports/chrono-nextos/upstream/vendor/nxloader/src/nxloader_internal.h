/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NXLOADER_INTERNAL_H
#define NXLOADER_INTERNAL_H

#include "nxloader.h"

#include <elf.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PT_GNU_RELRO
#define PT_GNU_RELRO 0x6474e552
#endif
#ifndef PT_ARM_EXIDX
#define PT_ARM_EXIDX 0x70000001
#endif
#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif
#ifndef DT_ANDROID_REL
#define DT_ANDROID_REL 0x6000000f
#endif
#ifndef DT_ANDROID_RELSZ
#define DT_ANDROID_RELSZ 0x60000010
#endif
#ifndef DT_ANDROID_RELA
#define DT_ANDROID_RELA 0x60000011
#endif
#ifndef DT_ANDROID_RELASZ
#define DT_ANDROID_RELASZ 0x60000012
#endif
#ifndef DT_RELR
#define DT_RELR 36
#endif
#ifndef DT_RELRSZ
#define DT_RELRSZ 35
#endif
#ifndef DT_RELRENT
#define DT_RELRENT 37
#endif
#ifndef DT_TLSDESC_PLT
#define DT_TLSDESC_PLT 0x6ffffef6
#endif
#ifndef DT_TLSDESC_GOT
#define DT_TLSDESC_GOT 0x6ffffef7
#endif
#ifndef DT_RPATH
#define DT_RPATH 15
#endif
#ifndef DT_TEXTREL
#define DT_TEXTREL 22
#endif
#ifndef DT_RUNPATH
#define DT_RUNPATH 29
#endif
#ifndef DT_FLAGS
#define DT_FLAGS 30
#endif
#ifndef DT_FLAGS_1
#define DT_FLAGS_1 0x6ffffffb
#endif
#ifndef DF_TEXTREL
#define DF_TEXTREL 0x00000004
#endif

/* Dynamic names are attacker-controlled input and are consulted from several
 * hot paths (relocations, exports and DT_NEEDED validation).  Keeping one
 * explicit ceiling makes every lookup bounded instead of repeatedly scanning
 * the remainder of a potentially very large dynstr table. */
#define NXLOADER_MAX_DYNAMIC_NAME_LENGTH 4096u

typedef struct nxloader_segment {
  uint64_t vma;
  uint64_t memory_size;
  uint64_t file_offset;
  uint64_t file_size;
  uint32_t flags;
} nxloader_segment;

typedef struct nxloader_dynamic_info {
  uint64_t dynamic_vma;
  size_t dynamic_size;

  uint64_t string_table_vma;
  size_t string_table_size;
  uint64_t symbol_table_vma;
  size_t symbol_entry_size;
  uint64_t sysv_hash_vma;
  size_t sysv_hash_size;
  uint64_t gnu_hash_vma;
  size_t gnu_hash_size;

  uint64_t rel_vma;
  size_t rel_size;
  size_t rel_entry_size;
  uint64_t rela_vma;
  size_t rela_size;
  size_t rela_entry_size;
  uint64_t plt_relocation_vma;
  size_t plt_relocation_size;
  uint64_t plt_relocation_kind;

  uint64_t init_vma;
  uint64_t init_array_vma;
  size_t init_array_size;

  uint64_t relro_vma;
  size_t relro_size;
  uint64_t arm_exidx_vma;
  size_t arm_exidx_size;

  uint32_t soname_offset;
  uint8_t has_soname;
  uint8_t textrel_declared;
  uint32_t *needed_offsets;
  size_t needed_count;
} nxloader_dynamic_info;

typedef struct nxloader_pending_write {
  void *target;
  uint64_t value;
  uint8_t width;
} nxloader_pending_write;

typedef struct nxloader_pending_list {
  nxloader_pending_write *items;
  size_t count;
  size_t capacity;
} nxloader_pending_list;

struct nxloader_module {
  nxloader_config config;
  nxloader_state state;
  uint8_t callback_active;
  uint8_t callback_violation;
  nxloader_arch arch;
  uint32_t elf_flags;
  nxloader_arm_float_abi arm_float_abi;
  char *debug_name;

  void *mapping;
  size_t mapping_size;
  size_t image_size;
  size_t page_size;
  size_t load_alignment;
  uint64_t minimum_vma;
  uint64_t maximum_vma;
  uintptr_t runtime_bias;

  nxloader_segment *segments;
  size_t segment_count;
  nxloader_dynamic_info dynamic;

  void *symbol_table;
  const char *string_table;
  size_t symbol_count;
  size_t relocation_count;

  uint8_t *trampoline_pool;
  size_t trampoline_pool_size;
  size_t trampoline_pool_used;
};

typedef struct nxloader_registry_entry {
  char *symbol;
  char *provider;
  uintptr_t address;
  int priority;
  uint32_t flags;
} nxloader_registry_entry;

struct nxloader_registry {
  /* Kept in strict bytewise symbol-name order.  This is both the canonical
   * ownership store and the worst-case-bounded lookup index. */
  nxloader_registry_entry *entries;
  size_t count;
  size_t capacity;
};

int nxloader_range_valid(size_t total, uint64_t offset, uint64_t length);
int nxloader_u64_add(uint64_t left, uint64_t right, uint64_t *result);
int nxloader_u64_mul(uint64_t left, uint64_t right, uint64_t *result);
int nxloader_is_power_of_two(uint64_t value);
uint64_t nxloader_align_down_u64(uint64_t value, uint64_t alignment);
int nxloader_align_up_u64(uint64_t value, uint64_t alignment,
                          uint64_t *result);
char *nxloader_strdup(const char *text);

nxloader_result nxloader_log(const nxloader_module *module,
                             nxloader_log_level level, const char *format,
                             ...);
nxloader_result nxloader_fail(nxloader_module *module, nxloader_result result,
                              const char *format, ...);
nxloader_result nxloader_module_callback_guard(
    const nxloader_module *module);

nxloader_result nxloader_allocate_image(nxloader_module *module,
                                        const uint8_t *file_data,
                                        size_t file_size);
nxloader_result nxloader_validate_segments(nxloader_module *module);
int nxloader_file_mapping_matches(const nxloader_module *module, uint64_t vma,
                                  uint64_t file_offset, uint64_t size);
nxloader_result nxloader_validate_relro(const nxloader_module *module);
void nxloader_release_image(nxloader_module *module);
const nxloader_segment *nxloader_segment_for_vma_range(
    const nxloader_module *module, uint64_t vma, size_t size);
void *nxloader_vma_pointer(const nxloader_module *module, uint64_t vma,
                           size_t size);
int nxloader_pointer_is_executable(const nxloader_module *module,
                                   uintptr_t address, size_t size);
nxloader_result nxloader_validate_relocation_metadata_target(
    const nxloader_module *module, uint64_t target_vma, size_t width);
nxloader_result nxloader_protect_image(nxloader_module *module);

nxloader_result nxloader_pending_add(nxloader_pending_list *pending,
                                     void *target, uint64_t value,
                                     uint8_t width);
nxloader_result nxloader_pending_validate(nxloader_pending_list *pending);
nxloader_result nxloader_pending_commit(nxloader_pending_list *pending);
void nxloader_pending_dispose(nxloader_pending_list *pending);

uint16_t nxloader_read_u16(const void *pointer);
uint32_t nxloader_read_u32(const void *pointer);
uint64_t nxloader_read_u64(const void *pointer);
void nxloader_write_u16(void *pointer, uint16_t value);
void nxloader_write_u32(void *pointer, uint32_t value);
void nxloader_write_u64(void *pointer, uint64_t value);

nxloader_result nxloader_parse_elf32(nxloader_module *module,
                                     const uint8_t *data, size_t size);
nxloader_result nxloader_parse_elf64(nxloader_module *module,
                                     const uint8_t *data, size_t size);
nxloader_result nxloader_relocate_elf32(nxloader_module *module);
nxloader_result nxloader_relocate_elf64(nxloader_module *module);
nxloader_result nxloader_resolve_elf32(
    nxloader_module *module, const nxloader_registry *registry, uint32_t flags,
    nxloader_resolution_report *report);
nxloader_result nxloader_resolve_elf64(
    nxloader_module *module, const nxloader_registry *registry, uint32_t flags,
    nxloader_resolution_report *report);
nxloader_result nxloader_find_export_elf32(const nxloader_module *module,
                                           const char *name,
                                           uintptr_t *address,
                                           uint32_t *symbol_flags,
                                           uint32_t *symbol_type);
nxloader_result nxloader_find_export_elf64(const nxloader_module *module,
                                           const char *name,
                                           uintptr_t *address,
                                           uint32_t *symbol_flags,
                                           uint32_t *symbol_type);
nxloader_result nxloader_find_relocation_elf32(const nxloader_module *module,
                                               const char *name,
                                               uintptr_t *slot_address);
nxloader_result nxloader_find_relocation_elf64(const nxloader_module *module,
                                               const char *name,
                                               uintptr_t *slot_address);
nxloader_result nxloader_add_exports_elf32(nxloader_registry *registry,
                                           const nxloader_module *module,
                                           const char *provider_name,
                                           int priority,
                                           nxloader_registry_report *report);
nxloader_result nxloader_add_exports_elf64(nxloader_registry *registry,
                                           const nxloader_module *module,
                                           const char *provider_name,
                                           int priority,
                                           nxloader_registry_report *report);
nxloader_result nxloader_call_initializers_elf32(nxloader_module *module);
nxloader_result nxloader_call_initializers_elf64(nxloader_module *module);

nxloader_result nxloader_apply_relocation_hook(
    nxloader_module *module, const nxloader_reloc_info *info,
    uint64_t default_value, int has_default, int *should_write,
    uint64_t *write_value);
nxloader_result nxloader_apply_alias(const nxloader_module *module,
                                     const char *name,
                                     const char **out_alias);
nxloader_result nxloader_apply_initializer_filter(
    nxloader_module *module, const nxloader_initializer_info *info,
    nxloader_initializer_action *out_action);
const char *nxloader_checked_string(const nxloader_module *module,
                                    uint32_t offset);
int nxloader_dynamic_name_valid(const char *name);

#endif /* NXLOADER_INTERNAL_H */
