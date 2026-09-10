/*
 * ELF64/AArch64 backend. Relocations are the set proven by Bully2,
 * Sonic 4 Episode II and Horizon Chase: RELATIVE, ABS64, GLOB_DAT and
 * JUMP_SLOT. Instruction relocations remain fail-closed: approved guests do
 * not carry them in their dynamic tables, while engine-specific instruction
 * patching belongs to the separately checked hook API.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "nxloader_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef EM_AARCH64
#define EM_AARCH64 183
#endif
#ifndef R_AARCH64_NONE
#define R_AARCH64_NONE 0
#endif
#ifndef R_AARCH64_ABS64
#define R_AARCH64_ABS64 257
#endif
#ifndef R_AARCH64_GLOB_DAT
#define R_AARCH64_GLOB_DAT 1025
#endif
#ifndef R_AARCH64_JUMP_SLOT
#define R_AARCH64_JUMP_SLOT 1026
#endif
#ifndef R_AARCH64_RELATIVE
#define R_AARCH64_RELATIVE 1027
#endif
#ifndef R_AARCH64_IRELATIVE
#define R_AARCH64_IRELATIVE 1032
#endif
#ifndef STT_GNU_IFUNC
#define STT_GNU_IFUNC 10
#endif

typedef nxloader_result (*nxloader_rela64_visitor)(nxloader_module *module,
                                                   const Elf64_Rela *rela,
                                                   void *userdata);

#define NX64_SEEN_STRTAB       (UINT64_C(1) << 0)
#define NX64_SEEN_STRSZ        (UINT64_C(1) << 1)
#define NX64_SEEN_SYMTAB       (UINT64_C(1) << 2)
#define NX64_SEEN_SYMENT       (UINT64_C(1) << 3)
#define NX64_SEEN_HASH         (UINT64_C(1) << 4)
#define NX64_SEEN_GNU_HASH     (UINT64_C(1) << 5)
#define NX64_SEEN_RELA         (UINT64_C(1) << 6)
#define NX64_SEEN_RELASZ       (UINT64_C(1) << 7)
#define NX64_SEEN_RELAENT      (UINT64_C(1) << 8)
#define NX64_SEEN_REL          (UINT64_C(1) << 9)
#define NX64_SEEN_RELSZ        (UINT64_C(1) << 10)
#define NX64_SEEN_RELENT       (UINT64_C(1) << 11)
#define NX64_SEEN_JMPREL       (UINT64_C(1) << 12)
#define NX64_SEEN_PLTRELSZ     (UINT64_C(1) << 13)
#define NX64_SEEN_PLTREL       (UINT64_C(1) << 14)
#define NX64_SEEN_INIT         (UINT64_C(1) << 15)
#define NX64_SEEN_INIT_ARRAY   (UINT64_C(1) << 16)
#define NX64_SEEN_INIT_ARRAYSZ (UINT64_C(1) << 17)
#define NX64_SEEN_SONAME       (UINT64_C(1) << 18)
#define NX64_SEEN_FLAGS        (UINT64_C(1) << 19)
#define NX64_SEEN_FLAGS_1      (UINT64_C(1) << 20)

static int nxloader_dynamic64_once(uint64_t *seen, uint64_t flag) {
  if (*seen & flag)
    return 0;
  *seen |= flag;
  return 1;
}

static int nxloader_aarch64_tls_relocation(uint32_t type) {
  return (type >= 512 && type <= 573) || (type >= 1028 && type <= 1031);
}

static int nxloader_aarch64_supported_relocation(uint32_t type) {
  return type == R_AARCH64_NONE || type == R_AARCH64_ABS64 ||
         type == R_AARCH64_GLOB_DAT || type == R_AARCH64_JUMP_SLOT ||
         type == R_AARCH64_RELATIVE;
}

static nxloader_result nxloader_aarch64_validate_relocation_target(
    const nxloader_module *module, uint64_t vma) {
  const nxloader_segment *segment =
      nxloader_segment_for_vma_range(module, vma, sizeof(uint64_t));
  uint32_t flags;
  nxloader_result result;
  if ((vma & (sizeof(uint64_t) - 1)) != 0)
    return NXLOADER_EFORMAT;
  if (!segment)
    return NXLOADER_EBOUNDS;
  flags = segment->flags;
  /* Dynamic scalar relocations may update writable data/RELRO while staging,
   * never executable instructions. W^X protections are finalized later. */
  if ((flags & PF_W) == 0 || (flags & PF_X) != 0)
    return NXLOADER_EUNSUPPORTED;
  result = nxloader_validate_relocation_metadata_target(
      module, vma, sizeof(uint64_t));
  return result;
}

static int nxloader_add_signed64(uint64_t base, int64_t addend,
                                 uint64_t *result) {
  if (addend >= 0)
    return nxloader_u64_add(base, (uint64_t)addend, result);
  if (addend == INT64_MIN) {
    uint64_t amount = (uint64_t)INT64_MAX + 1u;
    if (base < amount)
      return 0;
    *result = base - amount;
    return 1;
  }
  if (base < (uint64_t)(-addend))
    return 0;
  *result = base - (uint64_t)(-addend);
  return 1;
}

static nxloader_result nxloader_elf64_gnu_symbol_count(nxloader_module *module,
                                                       uint64_t hash_vma,
                                                       size_t *out_count,
                                                       size_t *out_size) {
  const uint8_t *header =
      (const uint8_t *)nxloader_vma_pointer(module, hash_vma, 16);
  uint32_t bucket_count;
  uint32_t symbol_offset;
  uint32_t bloom_count;
  uint64_t bloom_bytes;
  uint64_t bucket_bytes;
  uint64_t buckets_vma;
  uint64_t chains_vma;
  size_t maximum = 0;
  size_t bucket_index;
  uint32_t maximum_bucket = 0;
  size_t chain_entries_scanned = 0;
  size_t scan_budget;
  size_t chain_scan_budget;
  uint64_t chain_bytes;
  uint64_t hash_end;
  uint64_t hash_size;
  if (!out_count || !out_size)
    return NXLOADER_EINVAL;
  if (!header)
    return NXLOADER_EBOUNDS;
  bucket_count = nxloader_read_u32(header + 0);
  symbol_offset = nxloader_read_u32(header + 4);
  bloom_count = nxloader_read_u32(header + 8);
  if (!bucket_count || !nxloader_is_power_of_two(bloom_count) ||
      !nxloader_u64_mul(bloom_count, sizeof(uint64_t), &bloom_bytes) ||
      !nxloader_u64_add(hash_vma, 16, &buckets_vma) ||
      !nxloader_u64_add(buckets_vma, bloom_bytes, &buckets_vma) ||
      !nxloader_u64_mul(bucket_count, sizeof(uint32_t), &bucket_bytes) ||
      bucket_bytes > SIZE_MAX ||
      !nxloader_u64_add(buckets_vma, bucket_bytes, &chains_vma) ||
      !nxloader_vma_pointer(module, buckets_vma, (size_t)bucket_bytes))
    return NXLOADER_EBOUNDS;
  scan_budget = module->image_size / sizeof(uint32_t);
  if ((uint64_t)bucket_count > (uint64_t)scan_budget)
    return NXLOADER_EBOUNDS;
  chain_scan_budget = scan_budget - bucket_count;
  for (bucket_index = 0; bucket_index < bucket_count; ++bucket_index) {
    const uint8_t *bucket_pointer = (const uint8_t *)nxloader_vma_pointer(
        module, buckets_vma + bucket_index * sizeof(uint32_t),
        sizeof(uint32_t));
    uint32_t symbol_index = nxloader_read_u32(bucket_pointer);
    if (symbol_index == 0)
      continue;
    if (symbol_index < symbol_offset)
      return NXLOADER_EFORMAT;
    if (symbol_index > maximum_bucket)
      maximum_bucket = symbol_index;
  }
  maximum = symbol_offset;
  if (maximum_bucket != 0) {
    uint32_t symbol_index = maximum_bucket;
    for (;;) {
      uint64_t chain_offset;
      uint64_t chain_vma;
      const void *chain_pointer;
      uint32_t chain;
      if (chain_entries_scanned >= chain_scan_budget ||
          !nxloader_u64_mul(symbol_index - symbol_offset,
                            sizeof(uint32_t), &chain_offset) ||
          !nxloader_u64_add(chains_vma, chain_offset, &chain_vma))
        return NXLOADER_EBOUNDS;
      chain_pointer = nxloader_vma_pointer(module, chain_vma, sizeof(uint32_t));
      if (!chain_pointer)
        return NXLOADER_EBOUNDS;
      chain_entries_scanned++;
      chain = nxloader_read_u32(chain_pointer);
      if (symbol_index == UINT32_MAX)
        return NXLOADER_EOVERFLOW;
      maximum = (size_t)symbol_index + 1;
      if (chain & 1u)
        break;
      symbol_index++;
    }
  }
  if (maximum < symbol_offset ||
      !nxloader_u64_mul(maximum - symbol_offset, sizeof(uint32_t),
                        &chain_bytes) ||
      !nxloader_u64_add(chains_vma, chain_bytes, &hash_end) ||
      hash_end < hash_vma)
    return NXLOADER_EOVERFLOW;
  hash_size = hash_end - hash_vma;
  if (hash_size > SIZE_MAX ||
      !nxloader_vma_pointer(module, hash_vma, (size_t)hash_size))
    return NXLOADER_EBOUNDS;
  *out_count = maximum;
  *out_size = (size_t)hash_size;
  return nxloader_log(module, NXLOADER_LOG_DEBUG,
                      "AArch64 GNU hash scan: buckets=%u chain_entries=%zu "
                      "work_budget=%zu",
                      bucket_count, chain_entries_scanned, scan_budget);
}

static int nxloader_needed64_compare(const char *left, const char *right) {
  return strcmp(left, right);
}

static void nxloader_needed64_swap(const char **left, const char **right) {
  const char *temporary = *left;
  *left = *right;
  *right = temporary;
}

static void nxloader_needed64_sift_down(const char **names, size_t root,
                                        size_t count) {
  while (root < count / 2) {
    size_t child = root * 2 + 1;
    if (child + 1 < count &&
        nxloader_needed64_compare(names[child], names[child + 1]) < 0)
      child++;
    if (nxloader_needed64_compare(names[root], names[child]) >= 0)
      return;
    nxloader_needed64_swap(&names[root], &names[child]);
    root = child;
  }
}

static nxloader_result nxloader_validate_needed64(nxloader_module *module) {
  const nxloader_dynamic_info *dynamic;
  const char **names;
  size_t index;
  size_t start;
  size_t end;
  if (!module)
    return NXLOADER_EINVAL;
  dynamic = &module->dynamic;
  if (!dynamic->needed_count)
    return NXLOADER_OK;
  if (dynamic->needed_count > SIZE_MAX / sizeof(*names))
    return NXLOADER_EOVERFLOW;
  names = (const char **)malloc(dynamic->needed_count * sizeof(*names));
  if (!names)
    return NXLOADER_ENOMEM;
  for (index = 0; index < dynamic->needed_count; ++index) {
    names[index] =
        nxloader_checked_string(module, dynamic->needed_offsets[index]);
    if (!nxloader_dynamic_name_valid(names[index])) {
      free(names);
      return NXLOADER_EFORMAT;
    }
  }
  for (start = dynamic->needed_count / 2; start > 0; --start)
    nxloader_needed64_sift_down(names, start - 1, dynamic->needed_count);
  for (end = dynamic->needed_count; end > 1; --end) {
    nxloader_needed64_swap(&names[0], &names[end - 1]);
    nxloader_needed64_sift_down(names, 0, end - 1);
  }
  for (index = 1; index < dynamic->needed_count; ++index) {
    if (nxloader_needed64_compare(names[index - 1], names[index]) == 0) {
      free(names);
      return NXLOADER_EFORMAT;
    }
  }
  free(names);
  return NXLOADER_OK;
}

static nxloader_result nxloader_parse_dynamic64(nxloader_module *module,
                                                uint64_t dynamic_vma,
                                                size_t dynamic_size) {
  const Elf64_Dyn *entries;
  size_t entry_count;
  size_t index;
  size_t needed_count = 0;
  size_t needed_index = 0;
  uint64_t seen = 0;
  int saw_null = 0;
  nxloader_dynamic_info *dynamic = &module->dynamic;
  uint64_t relro_vma = dynamic->relro_vma;
  size_t relro_size = dynamic->relro_size;
  uint64_t exidx_vma = dynamic->arm_exidx_vma;
  size_t exidx_size = dynamic->arm_exidx_size;
  memset(dynamic, 0, sizeof(*dynamic));
  dynamic->dynamic_vma = dynamic_vma;
  dynamic->dynamic_size = dynamic_size;
  dynamic->relro_vma = relro_vma;
  dynamic->relro_size = relro_size;
  dynamic->arm_exidx_vma = exidx_vma;
  dynamic->arm_exidx_size = exidx_size;
  if (dynamic_size < sizeof(Elf64_Dyn) ||
      dynamic_size % sizeof(Elf64_Dyn) != 0 ||
      dynamic_vma % sizeof(uint64_t) != 0)
    return NXLOADER_EFORMAT;
  entries = (const Elf64_Dyn *)nxloader_vma_pointer(module, dynamic_vma,
                                                    dynamic_size);
  if (!entries)
    return NXLOADER_EBOUNDS;
  entry_count = dynamic_size / sizeof(*entries);
  for (index = 0; index < entry_count; ++index) {
    if (entries[index].d_tag == DT_NEEDED)
      needed_count++;
    if (entries[index].d_tag == DT_NULL) {
      saw_null = 1;
      break;
    }
  }
  if (!saw_null)
    return NXLOADER_EFORMAT;
  if (needed_count) {
    if (needed_count > SIZE_MAX / sizeof(uint32_t))
      return NXLOADER_EOVERFLOW;
    dynamic->needed_offsets =
        (uint32_t *)calloc(needed_count, sizeof(uint32_t));
    if (!dynamic->needed_offsets)
      return NXLOADER_ENOMEM;
  }
  dynamic->needed_count = needed_count;
  for (index = 0; index < entry_count && entries[index].d_tag != DT_NULL;
       ++index) {
    uint64_t value = entries[index].d_un.d_val;
    switch (entries[index].d_tag) {
    case DT_STRTAB:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_STRTAB))
        return NXLOADER_EFORMAT;
      dynamic->string_table_vma = value;
      break;
    case DT_STRSZ:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_STRSZ))
        return NXLOADER_EFORMAT;
      if (value > SIZE_MAX) return NXLOADER_EOVERFLOW;
      dynamic->string_table_size = (size_t)value;
      break;
    case DT_SYMTAB:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_SYMTAB))
        return NXLOADER_EFORMAT;
      dynamic->symbol_table_vma = value;
      break;
    case DT_SYMENT:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_SYMENT))
        return NXLOADER_EFORMAT;
      if (value > SIZE_MAX) return NXLOADER_EOVERFLOW;
      dynamic->symbol_entry_size = (size_t)value;
      break;
    case DT_HASH:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_HASH))
        return NXLOADER_EFORMAT;
      dynamic->sysv_hash_vma = value;
      break;
    case DT_GNU_HASH:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_GNU_HASH))
        return NXLOADER_EFORMAT;
      dynamic->gnu_hash_vma = value;
      break;
    case DT_RELA:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_RELA))
        return NXLOADER_EFORMAT;
      dynamic->rela_vma = value;
      break;
    case DT_RELASZ:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_RELASZ))
        return NXLOADER_EFORMAT;
      if (value > SIZE_MAX) return NXLOADER_EOVERFLOW;
      dynamic->rela_size = (size_t)value;
      break;
    case DT_RELAENT:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_RELAENT))
        return NXLOADER_EFORMAT;
      if (value > SIZE_MAX) return NXLOADER_EOVERFLOW;
      dynamic->rela_entry_size = (size_t)value;
      break;
    case DT_REL:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_REL))
        return NXLOADER_EFORMAT;
      dynamic->rel_vma = value;
      break;
    case DT_RELSZ:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_RELSZ))
        return NXLOADER_EFORMAT;
      if (value > SIZE_MAX) return NXLOADER_EOVERFLOW;
      dynamic->rel_size = (size_t)value;
      break;
    case DT_RELENT:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_RELENT))
        return NXLOADER_EFORMAT;
      if (value > SIZE_MAX) return NXLOADER_EOVERFLOW;
      dynamic->rel_entry_size = (size_t)value;
      break;
    case DT_JMPREL:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_JMPREL))
        return NXLOADER_EFORMAT;
      dynamic->plt_relocation_vma = value;
      break;
    case DT_PLTRELSZ:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_PLTRELSZ))
        return NXLOADER_EFORMAT;
      if (value > SIZE_MAX) return NXLOADER_EOVERFLOW;
      dynamic->plt_relocation_size = (size_t)value;
      break;
    case DT_PLTREL:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_PLTREL))
        return NXLOADER_EFORMAT;
      dynamic->plt_relocation_kind = value;
      break;
    case DT_INIT:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_INIT))
        return NXLOADER_EFORMAT;
      dynamic->init_vma = value;
      break;
    case DT_INIT_ARRAY:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_INIT_ARRAY))
        return NXLOADER_EFORMAT;
      dynamic->init_array_vma = value;
      break;
    case DT_INIT_ARRAYSZ:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_INIT_ARRAYSZ))
        return NXLOADER_EFORMAT;
      if (value > SIZE_MAX) return NXLOADER_EOVERFLOW;
      dynamic->init_array_size = (size_t)value;
      break;
    case DT_SONAME:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_SONAME) ||
          value > UINT32_MAX)
        return NXLOADER_EFORMAT;
      dynamic->soname_offset = (uint32_t)value;
      dynamic->has_soname = 1;
      break;
    case DT_NEEDED:
      if (value > UINT32_MAX || needed_index >= needed_count)
        return NXLOADER_EOVERFLOW;
      dynamic->needed_offsets[needed_index++] = (uint32_t)value;
      break;
    case DT_FLAGS:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_FLAGS))
        return NXLOADER_EFORMAT;
      if (value & DF_TEXTREL)
        return NXLOADER_EUNSUPPORTED;
      break;
    case DT_FLAGS_1:
      if (!nxloader_dynamic64_once(&seen, NX64_SEEN_FLAGS_1))
        return NXLOADER_EFORMAT;
      break;
    case DT_RPATH:
    case DT_RUNPATH:
    case DT_TEXTREL:
    case DT_TLSDESC_PLT:
    case DT_TLSDESC_GOT:
      return NXLOADER_EUNSUPPORTED;
    case DT_ANDROID_REL:
    case DT_ANDROID_RELSZ:
    case DT_ANDROID_RELA:
    case DT_ANDROID_RELASZ:
    case DT_RELR:
    case DT_RELRSZ:
    case DT_RELRENT:
      return NXLOADER_EUNSUPPORTED;
    default: break;
    }
  }
  if ((seen & (NX64_SEEN_STRTAB | NX64_SEEN_STRSZ | NX64_SEEN_SYMTAB |
               NX64_SEEN_SYMENT)) !=
          (NX64_SEEN_STRTAB | NX64_SEEN_STRSZ | NX64_SEEN_SYMTAB |
           NX64_SEEN_SYMENT) ||
      !(seen & (NX64_SEEN_HASH | NX64_SEEN_GNU_HASH)) ||
      !dynamic->string_table_vma || !dynamic->string_table_size ||
      !dynamic->symbol_table_vma ||
      dynamic->symbol_table_vma % sizeof(uint64_t) != 0 ||
      dynamic->symbol_entry_size != sizeof(Elf64_Sym))
    return NXLOADER_EFORMAT;
  if (seen & (NX64_SEEN_REL | NX64_SEEN_RELSZ | NX64_SEEN_RELENT))
    return NXLOADER_EUNSUPPORTED;
  if ((seen & (NX64_SEEN_RELA | NX64_SEEN_RELASZ | NX64_SEEN_RELAENT)) &&
      (seen & (NX64_SEEN_RELA | NX64_SEEN_RELASZ |
               NX64_SEEN_RELAENT)) !=
          (NX64_SEEN_RELA | NX64_SEEN_RELASZ | NX64_SEEN_RELAENT))
    return NXLOADER_EFORMAT;
  if ((seen & (NX64_SEEN_JMPREL | NX64_SEEN_PLTRELSZ |
               NX64_SEEN_PLTREL)) &&
      (seen & (NX64_SEEN_JMPREL | NX64_SEEN_PLTRELSZ |
               NX64_SEEN_PLTREL)) !=
          (NX64_SEEN_JMPREL | NX64_SEEN_PLTRELSZ | NX64_SEEN_PLTREL))
    return NXLOADER_EFORMAT;
  if ((seen & (NX64_SEEN_INIT_ARRAY | NX64_SEEN_INIT_ARRAYSZ)) &&
      (seen & (NX64_SEEN_INIT_ARRAY | NX64_SEEN_INIT_ARRAYSZ)) !=
          (NX64_SEEN_INIT_ARRAY | NX64_SEEN_INIT_ARRAYSZ))
    return NXLOADER_EFORMAT;
  module->string_table = (const char *)nxloader_vma_pointer(
      module, dynamic->string_table_vma, dynamic->string_table_size);
  if (!module->string_table)
    return NXLOADER_EBOUNDS;

  if (dynamic->sysv_hash_vma) {
    const uint8_t *hash = (const uint8_t *)nxloader_vma_pointer(
        module, dynamic->sysv_hash_vma, 8);
    uint32_t bucket_count;
    uint32_t chain_count;
    uint64_t words;
    if (!hash)
      return NXLOADER_EBOUNDS;
    bucket_count = nxloader_read_u32(hash);
    chain_count = nxloader_read_u32(hash + 4);
    if (!nxloader_u64_add(2u, (uint64_t)bucket_count + chain_count, &words) ||
        !nxloader_u64_mul(words, sizeof(uint32_t), &words) ||
        words > SIZE_MAX ||
        !nxloader_vma_pointer(module, dynamic->sysv_hash_vma, (size_t)words))
      return NXLOADER_EBOUNDS;
    module->symbol_count = chain_count;
    dynamic->sysv_hash_size = (size_t)words;
  }
  if (dynamic->gnu_hash_vma) {
    size_t gnu_symbol_count;
    nxloader_result result = nxloader_elf64_gnu_symbol_count(
        module, dynamic->gnu_hash_vma, &gnu_symbol_count,
        &dynamic->gnu_hash_size);
    if (result != NXLOADER_OK)
      return result;
    if (!dynamic->sysv_hash_vma)
      module->symbol_count = gnu_symbol_count;
    else if (gnu_symbol_count > module->symbol_count)
      return NXLOADER_EFORMAT;
  }
  if (!module->symbol_count ||
      module->symbol_count > SIZE_MAX / sizeof(Elf64_Sym))
    return NXLOADER_EFORMAT;
  module->symbol_table = nxloader_vma_pointer(
      module, dynamic->symbol_table_vma,
      module->symbol_count * sizeof(Elf64_Sym));
  if (!module->symbol_table)
    return NXLOADER_EBOUNDS;
  for (index = 0; index < module->symbol_count; ++index) {
    const Elf64_Sym *symbols = (const Elf64_Sym *)module->symbol_table;
    if (ELF64_ST_TYPE(symbols[index].st_info) == STT_GNU_IFUNC)
      return nxloader_fail(module, NXLOADER_EUNSUPPORTED,
                           "AArch64 GNU IFUNC symbols are unsupported");
  }

  if (dynamic->rela_size) {
    if (!dynamic->rela_vma || dynamic->rela_entry_size != sizeof(Elf64_Rela) ||
        dynamic->rela_vma % sizeof(uint64_t) != 0 ||
        dynamic->rela_size % sizeof(Elf64_Rela) != 0 ||
        !nxloader_vma_pointer(module, dynamic->rela_vma, dynamic->rela_size))
      return NXLOADER_EFORMAT;
    module->relocation_count = dynamic->rela_size / sizeof(Elf64_Rela);
  }
  if (dynamic->plt_relocation_size) {
    uint64_t rela_end;
    uint64_t plt_end;
    int same;
    if (!dynamic->plt_relocation_vma ||
        dynamic->plt_relocation_kind != DT_RELA ||
        dynamic->plt_relocation_vma % sizeof(uint64_t) != 0 ||
        dynamic->plt_relocation_size % sizeof(Elf64_Rela) != 0 ||
        !nxloader_vma_pointer(module, dynamic->plt_relocation_vma,
                              dynamic->plt_relocation_size))
      return NXLOADER_EFORMAT;
    same = dynamic->rela_vma == dynamic->plt_relocation_vma &&
           dynamic->rela_size == dynamic->plt_relocation_size;
    if (!same && dynamic->rela_size) {
      if (!nxloader_u64_add(dynamic->rela_vma, dynamic->rela_size, &rela_end) ||
          !nxloader_u64_add(dynamic->plt_relocation_vma,
                            dynamic->plt_relocation_size, &plt_end))
        return NXLOADER_EOVERFLOW;
      if (dynamic->rela_vma < plt_end &&
          dynamic->plt_relocation_vma < rela_end)
        return NXLOADER_EFORMAT;
    }
    if (!same) {
      size_t plt_count =
          dynamic->plt_relocation_size / sizeof(Elf64_Rela);
      if (plt_count > SIZE_MAX - module->relocation_count)
        return NXLOADER_EOVERFLOW;
      module->relocation_count += plt_count;
    }
  }
  if (dynamic->init_array_size &&
      (!dynamic->init_array_vma ||
       dynamic->init_array_vma % sizeof(uint64_t) != 0 ||
       dynamic->init_array_size % sizeof(uint64_t) != 0 ||
       !nxloader_vma_pointer(module, dynamic->init_array_vma,
                             dynamic->init_array_size)))
    return NXLOADER_EFORMAT;
  if (dynamic->has_soname &&
      !nxloader_dynamic_name_valid(
          nxloader_checked_string(module, dynamic->soname_offset)))
    return NXLOADER_EFORMAT;
  {
    nxloader_result result = nxloader_validate_needed64(module);
    if (result != NXLOADER_OK)
      return result;
  }
  return NXLOADER_OK;
}

nxloader_result nxloader_parse_elf64(nxloader_module *module,
                                     const uint8_t *data, size_t size) {
  const Elf64_Ehdr *header;
  const Elf64_Phdr *programs;
  uint64_t program_bytes;
  size_t index;
  size_t segment_index = 0;
  size_t segment_count = 0;
  int executable_segment = 0;
  long page_size;
  uint64_t dynamic_vma = 0;
  uint64_t dynamic_offset = 0;
  size_t dynamic_file_size = 0;
  size_t dynamic_size = 0;
  int saw_dynamic = 0;
  int saw_relro = 0;
  nxloader_result result;
  if (!module || !data || size < sizeof(Elf64_Ehdr))
    return NXLOADER_EINVAL;
  header = (const Elf64_Ehdr *)data;
  if (header->e_ident[EI_CLASS] != ELFCLASS64 ||
      header->e_machine != EM_AARCH64 || header->e_type != ET_DYN ||
      header->e_version != EV_CURRENT ||
      header->e_flags != 0 ||
      header->e_ehsize != sizeof(Elf64_Ehdr) || !header->e_phnum ||
      header->e_phentsize != sizeof(Elf64_Phdr) ||
      header->e_phoff % sizeof(uint64_t) != 0)
    return nxloader_fail(module, NXLOADER_EFORMAT,
                         "invalid AArch64 shared-object header");
  if (!nxloader_u64_mul(header->e_phnum, sizeof(Elf64_Phdr), &program_bytes) ||
      !nxloader_range_valid(size, header->e_phoff, program_bytes))
    return NXLOADER_EBOUNDS;
  programs = (const Elf64_Phdr *)(data + header->e_phoff);
  module->elf_flags = header->e_flags;
  page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0 || !nxloader_is_power_of_two((uint64_t)page_size))
    page_size = 4096;
  module->page_size = (size_t)page_size;
  module->load_alignment = module->page_size;
  module->minimum_vma = UINT64_MAX;
  module->maximum_vma = 0;
  for (index = 0; index < header->e_phnum; ++index) {
    const Elf64_Phdr *program = &programs[index];
    if (program->p_type == PT_LOAD) {
      uint64_t memory_end;
      uint64_t page_end;
      uint64_t page_start;
      if (!program->p_memsz || program->p_filesz > program->p_memsz ||
          !nxloader_range_valid(size, program->p_offset, program->p_filesz) ||
          !nxloader_u64_add(program->p_vaddr, program->p_memsz, &memory_end) ||
          !nxloader_align_up_u64(memory_end, module->page_size, &page_end))
        return NXLOADER_EBOUNDS;
      if (program->p_flags & ~(uint32_t)(PF_R | PF_W | PF_X))
        return NXLOADER_EFORMAT;
      if (program->p_align > 1 &&
          (!nxloader_is_power_of_two(program->p_align) ||
           (program->p_vaddr & (program->p_align - 1)) !=
               (program->p_offset & (program->p_align - 1))))
        return NXLOADER_EFORMAT;
      if ((program->p_vaddr & (module->page_size - 1)) !=
          (program->p_offset & (module->page_size - 1)))
        return NXLOADER_EFORMAT;
      if (program->p_align > module->config.max_image_size)
        return NXLOADER_EBOUNDS;
      if (program->p_align > module->load_alignment)
        module->load_alignment = (size_t)program->p_align;
      if ((program->p_flags & (PF_W | PF_X)) == (PF_W | PF_X) &&
          !(module->config.flags & NXLOADER_CONFIG_ALLOW_WX_SEGMENTS))
        return nxloader_fail(module, NXLOADER_EPROTECT,
                             "writable+executable PT_LOAD requires opt-in");
      if (program->p_flags & PF_X)
        executable_segment = 1;
      page_start = nxloader_align_down_u64(program->p_vaddr,
                                           module->page_size);
      if (page_start < module->minimum_vma)
        module->minimum_vma = page_start;
      if (page_end > module->maximum_vma)
        module->maximum_vma = page_end;
      segment_count++;
    } else if (program->p_type == PT_DYNAMIC) {
      if (saw_dynamic || !program->p_memsz ||
          program->p_filesz != program->p_memsz ||
          program->p_memsz > SIZE_MAX ||
          !nxloader_range_valid(size, program->p_offset, program->p_filesz) ||
          (program->p_flags & ~(uint32_t)(PF_R | PF_W | PF_X)) ||
          (program->p_flags & PF_X) ||
          (program->p_align > 1 &&
           (!nxloader_is_power_of_two(program->p_align) ||
            (program->p_vaddr & (program->p_align - 1)) !=
                (program->p_offset & (program->p_align - 1)))))
        return NXLOADER_EFORMAT;
      saw_dynamic = 1;
      dynamic_vma = program->p_vaddr;
      dynamic_offset = program->p_offset;
      dynamic_file_size = (size_t)program->p_filesz;
      dynamic_size = (size_t)program->p_memsz;
    } else if (program->p_type == PT_GNU_RELRO) {
      if (saw_relro)
        return NXLOADER_EFORMAT;
      if (program->p_memsz > SIZE_MAX)
        return NXLOADER_EOVERFLOW;
      if (!program->p_memsz || program->p_filesz > program->p_memsz ||
          (program->p_flags & ~(uint32_t)(PF_R | PF_W | PF_X)) ||
          (program->p_flags & PF_X) ||
          (program->p_align > 1 &&
           (!nxloader_is_power_of_two(program->p_align) ||
            (program->p_vaddr & (program->p_align - 1)) !=
                (program->p_offset & (program->p_align - 1)))))
        return NXLOADER_EFORMAT;
      saw_relro = 1;
      module->dynamic.relro_vma = program->p_vaddr;
      module->dynamic.relro_size = (size_t)program->p_memsz;
    } else if (program->p_type == PT_TLS &&
               (program->p_memsz != 0 || program->p_filesz != 0)) {
      return NXLOADER_EUNSUPPORTED;
    } else if (program->p_type == PT_INTERP) {
      /* Android guests are loaded by nxloader and must not select a Linux or
       * Android process interpreter. PT_INTERP belongs only to the host ELF. */
      return NXLOADER_EUNSUPPORTED;
    }
  }
  if (!segment_count || !executable_segment ||
      module->minimum_vma == UINT64_MAX ||
      module->maximum_vma <= module->minimum_vma || !saw_dynamic ||
      !dynamic_size)
    return NXLOADER_EFORMAT;
  if (segment_count > SIZE_MAX / sizeof(*module->segments))
    return NXLOADER_EOVERFLOW;
  module->segments = (nxloader_segment *)calloc(segment_count,
                                                sizeof(*module->segments));
  if (!module->segments)
    return NXLOADER_ENOMEM;
  module->segment_count = segment_count;
  for (index = 0; index < header->e_phnum; ++index) {
    if (programs[index].p_type != PT_LOAD)
      continue;
    module->segments[segment_index].vma = programs[index].p_vaddr;
    module->segments[segment_index].memory_size = programs[index].p_memsz;
    module->segments[segment_index].file_offset = programs[index].p_offset;
    module->segments[segment_index].file_size = programs[index].p_filesz;
    module->segments[segment_index].flags = programs[index].p_flags;
    segment_index++;
  }
  result = nxloader_validate_segments(module);
  if (result != NXLOADER_OK)
    return result;
  if (!nxloader_file_mapping_matches(module, dynamic_vma, dynamic_offset,
                                     dynamic_file_size))
    return nxloader_fail(module, NXLOADER_EFORMAT,
                         "PT_DYNAMIC does not match a file-backed PT_LOAD");
  result = nxloader_validate_relro(module);
  if (result != NXLOADER_OK)
    return result;
  result = nxloader_allocate_image(module, data, size);
  if (result != NXLOADER_OK)
    return result;
  result = nxloader_parse_dynamic64(module, dynamic_vma, dynamic_size);
  if (result != NXLOADER_OK)
    return result;
  return NXLOADER_OK;
}

static nxloader_result nxloader_visit_rela64(nxloader_module *module,
                                             nxloader_rela64_visitor visitor,
                                             void *userdata) {
  const nxloader_dynamic_info *dynamic = &module->dynamic;
  size_t table_index;
  struct {
    uint64_t vma;
    size_t size;
  } tables[2];
  size_t table_count = 0;
  if (dynamic->rela_size) {
    tables[table_count].vma = dynamic->rela_vma;
    tables[table_count++].size = dynamic->rela_size;
  }
  if (dynamic->plt_relocation_size &&
      !(dynamic->plt_relocation_vma == dynamic->rela_vma &&
        dynamic->plt_relocation_size == dynamic->rela_size)) {
    tables[table_count].vma = dynamic->plt_relocation_vma;
    tables[table_count++].size = dynamic->plt_relocation_size;
  }
  for (table_index = 0; table_index < table_count; ++table_index) {
    const Elf64_Rela *relocations = (const Elf64_Rela *)nxloader_vma_pointer(
        module, tables[table_index].vma, tables[table_index].size);
    size_t count = tables[table_index].size / sizeof(*relocations);
    size_t index;
    if (!relocations)
      return NXLOADER_EBOUNDS;
    for (index = 0; index < count; ++index) {
      nxloader_result result = visitor(module, &relocations[index], userdata);
      if (result != NXLOADER_OK)
        return result;
    }
  }
  return NXLOADER_OK;
}

typedef struct nxloader_local64_context {
  nxloader_pending_list pending;
} nxloader_local64_context;

static nxloader_result nxloader_local_rela64(nxloader_module *module,
                                             const Elf64_Rela *rela,
                                             void *userdata) {
  nxloader_local64_context *context = (nxloader_local64_context *)userdata;
  const Elf64_Sym *symbols = (const Elf64_Sym *)module->symbol_table;
  uint32_t type = (uint32_t)ELF64_R_TYPE(rela->r_info);
  uint32_t symbol_index = (uint32_t)ELF64_R_SYM(rela->r_info);
  const Elf64_Sym *symbol = NULL;
  const char *name = NULL;
  nxloader_reloc_info info;
  void *target;
  uint64_t value = 0;
  uint64_t hooked_value = 0;
  int has_default = 0;
  int should_write = 0;
  unsigned symbol_type;
  nxloader_result result;
  if (type == R_AARCH64_NONE)
    return NXLOADER_OK;
  if (nxloader_aarch64_tls_relocation(type) ||
      type == R_AARCH64_IRELATIVE)
    return NXLOADER_EUNSUPPORTED;
  if (!nxloader_aarch64_supported_relocation(type))
    return nxloader_fail(module, NXLOADER_ERELOC,
                         "unsupported AArch64 relocation %u at VMA 0x%llx",
                         type, (unsigned long long)rela->r_offset);
  result = nxloader_aarch64_validate_relocation_target(module,
                                                       rela->r_offset);
  if (result != NXLOADER_OK)
    return result;
  target = nxloader_vma_pointer(module, rela->r_offset, sizeof(uint64_t));
  if (!target)
    return NXLOADER_EBOUNDS;
  if (symbol_index >= module->symbol_count)
    return NXLOADER_EFORMAT;
  symbol = &symbols[symbol_index];
  symbol_type = ELF64_ST_TYPE(symbol->st_info);
  if (symbol_type != STT_NOTYPE && symbol_type != STT_OBJECT &&
      symbol_type != STT_FUNC)
    return NXLOADER_EUNSUPPORTED;
  if (type == R_AARCH64_RELATIVE && symbol_index != 0)
    return NXLOADER_EFORMAT;
  if (symbol->st_name) {
    name = nxloader_checked_string(module, symbol->st_name);
    if (!name)
      return NXLOADER_EFORMAT;
  }
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  info.phase = NXLOADER_RELOC_PHASE_LOCAL;
  info.arch = NXLOADER_ARCH_AARCH64;
  info.type = type;
  info.target_vma = rela->r_offset;
  info.addend = rela->r_addend;
  info.symbol = name;
  info.symbol_defined = symbol->st_shndx != SHN_UNDEF;
  info.symbol_weak = ELF64_ST_BIND(symbol->st_info) == STB_WEAK;
  switch (type) {
  case R_AARCH64_RELATIVE:
    if (rela->r_addend < 0 ||
        !nxloader_vma_pointer(module, (uint64_t)rela->r_addend, 0))
      return NXLOADER_EBOUNDS;
    has_default = nxloader_add_signed64(module->runtime_bias,
                                        rela->r_addend, &value);
    if (!has_default)
      return NXLOADER_EOVERFLOW;
    break;
  case R_AARCH64_ABS64:
  case R_AARCH64_GLOB_DAT:
  case R_AARCH64_JUMP_SLOT:
    if (symbol->st_shndx == SHN_UNDEF)
      return NXLOADER_OK;
    if (symbol->st_shndx == SHN_ABS)
      value = symbol->st_value;
    else {
      if (!nxloader_vma_pointer(module, symbol->st_value, 0))
        return NXLOADER_EBOUNDS;
      if (!nxloader_u64_add(module->runtime_bias, symbol->st_value, &value))
        return NXLOADER_EOVERFLOW;
    }
    if (!nxloader_add_signed64(value, rela->r_addend, &value))
      return NXLOADER_EOVERFLOW;
    has_default = 1;
    break;
  default: return NXLOADER_ERELOC;
  }
  result = nxloader_apply_relocation_hook(module, &info, value, has_default,
                                          &should_write, &hooked_value);
  if (result != NXLOADER_OK) {
    nxloader_result log_result = nxloader_log(
        module, NXLOADER_LOG_ERROR,
        "AArch64 relocation %u at VMA 0x%llx was rejected", type,
        (unsigned long long)rela->r_offset);
    if (log_result != NXLOADER_OK)
      return log_result;
    return result;
  }
  if (!should_write)
    return NXLOADER_OK;
  return nxloader_pending_add(&context->pending, target, hooked_value, 8);
}

nxloader_result nxloader_relocate_elf64(nxloader_module *module) {
  nxloader_local64_context context;
  nxloader_result result;
  memset(&context, 0, sizeof(context));
  result = nxloader_visit_rela64(module, nxloader_local_rela64, &context);
  if (result == NXLOADER_OK)
    result = nxloader_pending_commit(&context.pending);
  nxloader_pending_dispose(&context.pending);
  return result;
}

typedef struct nxloader_resolve64_context {
  const nxloader_registry *registry;
  nxloader_pending_list pending;
  nxloader_resolution_report local_report;
} nxloader_resolve64_context;

static nxloader_result nxloader_import_rela64(nxloader_module *module,
                                              const Elf64_Rela *rela,
                                              void *userdata) {
  nxloader_resolve64_context *context =
      (nxloader_resolve64_context *)userdata;
  const Elf64_Sym *symbols = (const Elf64_Sym *)module->symbol_table;
  uint32_t type = (uint32_t)ELF64_R_TYPE(rela->r_info);
  uint32_t symbol_index = (uint32_t)ELF64_R_SYM(rela->r_info);
  const Elf64_Sym *symbol;
  const char *name;
  const char *query;
  nxloader_registry_match match;
  nxloader_reloc_info info;
  uint64_t value = 0;
  uint64_t hooked_value = 0;
  int has_default = 0;
  int should_write = 0;
  int is_weak;
  int registry_found = 0;
  nxloader_result result;
  void *target;
  if (type == R_AARCH64_NONE || type == R_AARCH64_RELATIVE)
    return NXLOADER_OK;
  if (nxloader_aarch64_tls_relocation(type) ||
      type == R_AARCH64_IRELATIVE)
    return NXLOADER_EUNSUPPORTED;
  if (!nxloader_aarch64_supported_relocation(type))
    return NXLOADER_ERELOC;
  if (type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT &&
      type != R_AARCH64_JUMP_SLOT)
    return NXLOADER_ERELOC;
  result = nxloader_aarch64_validate_relocation_target(module,
                                                       rela->r_offset);
  if (result != NXLOADER_OK)
    return result;
  if (symbol_index >= module->symbol_count)
    return NXLOADER_EFORMAT;
  symbol = &symbols[symbol_index];
  if (ELF64_ST_TYPE(symbol->st_info) != STT_NOTYPE &&
      ELF64_ST_TYPE(symbol->st_info) != STT_OBJECT &&
      ELF64_ST_TYPE(symbol->st_info) != STT_FUNC)
    return NXLOADER_EUNSUPPORTED;
  if (symbol->st_shndx != SHN_UNDEF)
    return NXLOADER_OK;
  if (ELF64_ST_BIND(symbol->st_info) != STB_GLOBAL &&
      ELF64_ST_BIND(symbol->st_info) != STB_WEAK)
    return NXLOADER_EFORMAT;
  name = nxloader_checked_string(module, symbol->st_name);
  if (!name || !*name)
    return NXLOADER_EFORMAT;
  result = nxloader_apply_alias(module, name, &query);
  if (result != NXLOADER_OK)
    return result;
  memset(&match, 0, sizeof(match));
  match.struct_size = sizeof(match);
  result = nxloader_registry_lookup(context->registry, query, &match);
  is_weak = ELF64_ST_BIND(symbol->st_info) == STB_WEAK;
  if (result == NXLOADER_OK) {
    if (!nxloader_add_signed64((uint64_t)match.address, rela->r_addend,
                               &value))
      return NXLOADER_EOVERFLOW;
    has_default = 1;
    registry_found = 1;
  } else if (result == NXLOADER_EUNRESOLVED && is_weak) {
    value = (uint64_t)rela->r_addend;
    has_default = 1;
  } else if (result != NXLOADER_EUNRESOLVED) {
    return result;
  }
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  info.phase = NXLOADER_RELOC_PHASE_IMPORT;
  info.arch = NXLOADER_ARCH_AARCH64;
  info.type = type;
  info.target_vma = rela->r_offset;
  info.addend = rela->r_addend;
  info.symbol = name;
  info.symbol_weak = (uint8_t)is_weak;
  if (has_default || module->config.relocation_hook) {
    result = nxloader_apply_relocation_hook(module, &info, value, has_default,
                                            &should_write, &hooked_value);
    if (result != NXLOADER_OK && !(result == NXLOADER_ERELOC && !has_default))
      return result;
    if (result == NXLOADER_OK && should_write) {
      target = nxloader_vma_pointer(module, rela->r_offset, sizeof(uint64_t));
      if (!target)
        return NXLOADER_EBOUNDS;
      result = nxloader_pending_add(&context->pending, target, hooked_value, 8);
      if (result != NXLOADER_OK)
        return result;
      if (!registry_found && is_weak && hooked_value == value)
        context->local_report.weak_imports_zeroed++;
      else
        context->local_report.imports_resolved++;
      return NXLOADER_OK;
    }
    if (result == NXLOADER_OK && !should_write)
      return NXLOADER_OK;
  }
  if (!has_default) {
    context->local_report.unresolved_strong++;
    if (!context->local_report.first_unresolved)
      context->local_report.first_unresolved = name;
    return NXLOADER_OK;
  }
  return NXLOADER_OK;
}

nxloader_result nxloader_resolve_elf64(
    nxloader_module *module, const nxloader_registry *registry, uint32_t flags,
    nxloader_resolution_report *report) {
  nxloader_resolve64_context context;
  nxloader_result result;
  memset(&context, 0, sizeof(context));
  context.registry = registry;
  context.local_report.struct_size = sizeof(context.local_report);
  result = nxloader_visit_rela64(module, nxloader_import_rela64, &context);
  if (result == NXLOADER_OK && context.local_report.unresolved_strong &&
      !(flags & NXLOADER_RESOLVE_ALLOW_UNRESOLVED))
    result = NXLOADER_EUNRESOLVED;
  if (result == NXLOADER_EUNRESOLVED) {
    nxloader_result log_result = nxloader_log(
        module, NXLOADER_LOG_ERROR,
        "%zu unresolved strong import(s); first: %s",
        context.local_report.unresolved_strong,
        context.local_report.first_unresolved
            ? context.local_report.first_unresolved
            : "<unknown>");
    if (log_result != NXLOADER_OK) {
      nxloader_pending_dispose(&context.pending);
      return log_result;
    }
  }
  if (result == NXLOADER_OK)
    result = nxloader_pending_commit(&context.pending);
  if (report) {
    size_t struct_size = report->struct_size;
    *report = context.local_report;
    report->struct_size = struct_size;
  }
  nxloader_pending_dispose(&context.pending);
  return result;
}

nxloader_result nxloader_find_export_elf64(const nxloader_module *module,
                                           const char *name,
                                           uintptr_t *address,
                                           uint32_t *symbol_flags,
                                           uint32_t *symbol_type) {
  const Elf64_Sym *symbols = (const Elf64_Sym *)module->symbol_table;
  size_t index;
  for (index = 0; index < module->symbol_count; ++index) {
    const char *candidate;
    uint64_t value;
    if (symbols[index].st_shndx == SHN_UNDEF || !symbols[index].st_name)
      continue;
    candidate = nxloader_checked_string(module, symbols[index].st_name);
    if (!candidate)
      return NXLOADER_EFORMAT;
    if (strcmp(candidate, name) != 0)
      continue;
    if (ELF64_ST_TYPE(symbols[index].st_info) != STT_NOTYPE &&
        ELF64_ST_TYPE(symbols[index].st_info) != STT_OBJECT &&
        ELF64_ST_TYPE(symbols[index].st_info) != STT_FUNC)
      return NXLOADER_EUNSUPPORTED;
    if (symbols[index].st_shndx == SHN_ABS)
      value = symbols[index].st_value;
    else {
      if (!nxloader_vma_pointer(module, symbols[index].st_value, 0))
        return NXLOADER_EBOUNDS;
      if (!nxloader_u64_add(module->runtime_bias, symbols[index].st_value,
                            &value) || value > UINTPTR_MAX)
        return NXLOADER_EOVERFLOW;
    }
    *address = (uintptr_t)value;
    if (symbol_flags)
      *symbol_flags = ELF64_ST_BIND(symbols[index].st_info) == STB_WEAK
                          ? NXLOADER_SYMBOL_WEAK
                          : 0;
    if (symbol_type)
      *symbol_type = ELF64_ST_TYPE(symbols[index].st_info);
    return NXLOADER_OK;
  }
  return NXLOADER_EUNRESOLVED;
}

typedef struct nxloader_find_rela64_context {
  const char *name;
  uintptr_t slot;
} nxloader_find_rela64_context;

static nxloader_result nxloader_find_rela64(nxloader_module *module,
                                            const Elf64_Rela *rela,
                                            void *userdata) {
  nxloader_find_rela64_context *context =
      (nxloader_find_rela64_context *)userdata;
  const Elf64_Sym *symbols = (const Elf64_Sym *)module->symbol_table;
  uint32_t type = (uint32_t)ELF64_R_TYPE(rela->r_info);
  uint32_t symbol_index = (uint32_t)ELF64_R_SYM(rela->r_info);
  const char *name;
  void *slot;
  if (context->slot ||
      (type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT &&
       type != R_AARCH64_JUMP_SLOT))
    return NXLOADER_OK;
  if (symbol_index >= module->symbol_count)
    return NXLOADER_EFORMAT;
  name = nxloader_checked_string(module, symbols[symbol_index].st_name);
  if (!name)
    return NXLOADER_EFORMAT;
  if (strcmp(name, context->name) != 0)
    return NXLOADER_OK;
  slot = nxloader_vma_pointer(module, rela->r_offset, sizeof(uint64_t));
  if (!slot)
    return NXLOADER_EBOUNDS;
  context->slot = (uintptr_t)slot;
  return NXLOADER_OK;
}

nxloader_result nxloader_find_relocation_elf64(const nxloader_module *module,
                                               const char *name,
                                               uintptr_t *slot_address) {
  nxloader_find_rela64_context context;
  nxloader_result result;
  memset(&context, 0, sizeof(context));
  context.name = name;
  result = nxloader_visit_rela64((nxloader_module *)module,
                                 nxloader_find_rela64, &context);
  if (result != NXLOADER_OK)
    return result;
  if (!context.slot)
    return NXLOADER_EUNRESOLVED;
  *slot_address = context.slot;
  return NXLOADER_OK;
}

nxloader_result nxloader_add_exports_elf64(nxloader_registry *registry,
                                           const nxloader_module *module,
                                           const char *provider_name,
                                           int priority,
                                           nxloader_registry_report *report) {
  const Elf64_Sym *symbols = (const Elf64_Sym *)module->symbol_table;
  nxloader_symbol *exports;
  size_t count = 0;
  size_t index;
  nxloader_provider provider;
  nxloader_result result;
  for (index = 0; index < module->symbol_count; ++index) {
    unsigned binding = ELF64_ST_BIND(symbols[index].st_info);
    unsigned type = ELF64_ST_TYPE(symbols[index].st_info);
    unsigned visibility = ELF64_ST_VISIBILITY(symbols[index].st_other);
    if (symbols[index].st_shndx != SHN_UNDEF && symbols[index].st_name &&
        symbols[index].st_value &&
        (binding == STB_GLOBAL || binding == STB_WEAK) &&
        (type == STT_NOTYPE || type == STT_OBJECT || type == STT_FUNC) &&
        visibility != STV_HIDDEN && visibility != STV_INTERNAL)
      count++;
  }
  exports = (nxloader_symbol *)calloc(count ? count : 1, sizeof(*exports));
  if (!exports)
    return NXLOADER_ENOMEM;
  count = 0;
  for (index = 0; index < module->symbol_count; ++index) {
    unsigned binding = ELF64_ST_BIND(symbols[index].st_info);
    unsigned type = ELF64_ST_TYPE(symbols[index].st_info);
    unsigned visibility = ELF64_ST_VISIBILITY(symbols[index].st_other);
    uintptr_t address;
    const char *name;
    if (symbols[index].st_shndx == SHN_UNDEF || !symbols[index].st_name ||
        !symbols[index].st_value ||
        (binding != STB_GLOBAL && binding != STB_WEAK) ||
        (type != STT_NOTYPE && type != STT_OBJECT && type != STT_FUNC) ||
        visibility == STV_HIDDEN || visibility == STV_INTERNAL)
      continue;
    name = nxloader_checked_string(module, symbols[index].st_name);
    if (!name) {
      free(exports);
      return NXLOADER_EFORMAT;
    }
    if (symbols[index].st_shndx == SHN_ABS) {
      if (symbols[index].st_value > UINTPTR_MAX) {
        free(exports);
        return NXLOADER_EOVERFLOW;
      }
      address = (uintptr_t)symbols[index].st_value;
    } else {
      uint64_t value;
      if (!nxloader_vma_pointer(module, symbols[index].st_value, 0) ||
          !nxloader_u64_add(module->runtime_bias, symbols[index].st_value,
                            &value) || value > UINTPTR_MAX) {
        free(exports);
        return NXLOADER_EOVERFLOW;
      }
      address = (uintptr_t)value;
    }
    exports[count].name = name;
    exports[count].address = address;
    exports[count].flags = binding == STB_WEAK ? NXLOADER_SYMBOL_WEAK : 0;
    count++;
  }
  memset(&provider, 0, sizeof(provider));
  provider.struct_size = sizeof(provider);
  provider.name = provider_name;
  provider.symbols = exports;
  provider.symbol_count = count;
  provider.priority = priority;
  result = nxloader_registry_add_provider(registry, &provider, report);
  free(exports);
  return result;
}

typedef struct nxloader_initializer_plan64 {
  uintptr_t address;
} nxloader_initializer_plan64;

static nxloader_result nxloader_plan_initializer64(
    nxloader_module *module, nxloader_initializer_kind kind, size_t index,
    uintptr_t address, int *should_run) {
  nxloader_initializer_info info;
  nxloader_initializer_action action;
  nxloader_result result;
  if (!should_run)
    return NXLOADER_EINVAL;
  *should_run = 0;
  if ((address & 3u) != 0 ||
      !nxloader_pointer_is_executable(module, address, 1))
    return NXLOADER_EBOUNDS;
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  info.kind = kind;
  info.index = index;
  info.address = address;
  result = nxloader_apply_initializer_filter(module, &info, &action);
  if (result != NXLOADER_OK)
    return result;
  *should_run = action == NXLOADER_INITIALIZER_RUN;
  return NXLOADER_OK;
}

nxloader_result nxloader_call_initializers_elf64(nxloader_module *module) {
  nxloader_initializer_plan64 *plan = NULL;
  size_t capacity = module->dynamic.init_array_size / sizeof(uint64_t);
  size_t count = 0;
  size_t index;
  nxloader_result result;
  void (*function)(void);
  if (module->dynamic.init_vma) {
    if (capacity == SIZE_MAX)
      return NXLOADER_EOVERFLOW;
    capacity++;
  }
  if (capacity > SIZE_MAX / sizeof(*plan))
    return NXLOADER_EOVERFLOW;
  if (capacity) {
    plan = (nxloader_initializer_plan64 *)calloc(capacity, sizeof(*plan));
    if (!plan)
      return NXLOADER_ENOMEM;
  }
  if (module->dynamic.init_vma) {
    uint64_t address64;
    int should_run;
    if (!nxloader_u64_add(module->runtime_bias, module->dynamic.init_vma,
                          &address64) || address64 > UINTPTR_MAX) {
      result = NXLOADER_EOVERFLOW;
      goto done;
    }
    result = nxloader_plan_initializer64(
        module, NXLOADER_INITIALIZER_DT_INIT, SIZE_MAX,
        (uintptr_t)address64, &should_run);
    if (result != NXLOADER_OK)
      goto done;
    if (should_run)
      plan[count++].address = (uintptr_t)address64;
  }
  for (index = 0;
       index < module->dynamic.init_array_size / sizeof(uint64_t); ++index) {
    const void *entry = nxloader_vma_pointer(
        module, module->dynamic.init_array_vma + index * sizeof(uint64_t),
        sizeof(uint64_t));
    uint64_t address64;
    int should_run;
    if (!entry) {
      result = NXLOADER_EBOUNDS;
      goto done;
    }
    address64 = nxloader_read_u64(entry);
    if (address64 == 0 || address64 == UINT64_MAX)
      continue;
    if (address64 > UINTPTR_MAX) {
      result = NXLOADER_EOVERFLOW;
      goto done;
    }
    result = nxloader_plan_initializer64(
        module, NXLOADER_INITIALIZER_INIT_ARRAY, index,
        (uintptr_t)address64, &should_run);
    if (result != NXLOADER_OK)
      goto done;
    if (should_run)
      plan[count++].address = (uintptr_t)address64;
  }
  if (count && nxloader_process_arch() != NXLOADER_ARCH_AARCH64) {
    result = NXLOADER_EARCH;
    goto done;
  }
  if (count && sizeof(function) != sizeof(uintptr_t)) {
    result = NXLOADER_EUNSUPPORTED;
    goto done;
  }
  module->state = NXLOADER_STATE_INITIALIZING;
  for (index = 0; index < count; ++index) {
    memcpy(&function, &plan[index].address, sizeof(function));
    function();
  }
  result = NXLOADER_OK;

done:
  free(plan);
  return result;
}
