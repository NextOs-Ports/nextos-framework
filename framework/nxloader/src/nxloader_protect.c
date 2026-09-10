/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nxloader_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static uint8_t nxloader_elf_flags_to_protection(uint32_t flags) {
  uint8_t protection = 0;
  if (flags & PF_R)
    protection |= PROT_READ;
  if (flags & PF_W)
    protection |= PROT_WRITE;
  if (flags & PF_X)
    protection |= PROT_EXEC;
  return protection;
}

int nxloader_pointer_overlaps_relro(const nxloader_module *module,
                                    uintptr_t address, size_t size) {
  uint64_t relro_end;
  uint64_t first_vma;
  uint64_t final_vma;
  size_t first_offset;
  size_t final_offset;
  uintptr_t mapping;
  size_t address_offset;
  if (!module || !module->mapping || size == 0 ||
      module->dynamic.relro_size == 0)
    return 0;
  if (!nxloader_u64_add(module->dynamic.relro_vma,
                        module->dynamic.relro_size, &relro_end) ||
      !nxloader_align_up_u64(relro_end, module->page_size, &final_vma))
    return 1;
  first_vma = nxloader_align_down_u64(module->dynamic.relro_vma,
                                      module->page_size);
  if (first_vma < module->minimum_vma || final_vma > module->maximum_vma)
    return 1;
  first_offset = (size_t)(first_vma - module->minimum_vma);
  final_offset = (size_t)(final_vma - module->minimum_vma);
  mapping = (uintptr_t)module->mapping;
  if (address < mapping || size > module->mapping_size)
    return 1;
  address_offset = (size_t)(address - mapping);
  if (address_offset > module->mapping_size - size)
    return 1;
  return address_offset < final_offset &&
         first_offset < address_offset + size;
}

nxloader_result nxloader_close_auxiliary_pool(nxloader_module *module,
                                               size_t index) {
  uint8_t *base;
  size_t size;
  size_t used;
  if (!module || index >= module->aux_pool_count)
    return NXLOADER_EINVAL;
  base = module->aux_pool[index].base;
  size = module->aux_pool[index].size;
  used = module->aux_pool[index].used;
  if (!base || size == 0 || used > size)
    return NXLOADER_EBOUNDS;
  if (mprotect(base, size, PROT_READ | PROT_EXEC) != 0) {
    nxloader_result log_result = nxloader_log(
        module, NXLOADER_LOG_ERROR,
        "mprotect failed while closing auxiliary pool %zu: %s", index,
        strerror(errno));
    return log_result == NXLOADER_OK ? NXLOADER_EPROTECT : log_result;
  }
  if (used)
    __builtin___clear_cache((char *)base, (char *)base + used);
  return NXLOADER_OK;
}

static nxloader_result nxloader_build_image_protections(
    nxloader_module *module, uint8_t **out_protections,
    size_t *out_page_count) {
  uint8_t *protections;
  size_t page_count;
  size_t index;
  if (!module || !out_protections || !out_page_count || !module->mapping ||
      module->page_size == 0 ||
      module->mapping_size % module->page_size != 0)
    return NXLOADER_EINVAL;
  *out_protections = NULL;
  *out_page_count = 0;
  page_count = module->mapping_size / module->page_size;
  protections = (uint8_t *)calloc(page_count ? page_count : 1, 1);
  if (!protections)
    return NXLOADER_ENOMEM;

  for (index = 0; index < module->segment_count; ++index) {
    const nxloader_segment *segment = &module->segments[index];
    uint64_t first_vma = nxloader_align_down_u64(segment->vma,
                                                 module->page_size);
    uint64_t final_vma;
    size_t first_page;
    size_t final_page;
    size_t page;
    uint8_t protection = nxloader_elf_flags_to_protection(segment->flags);
    uint64_t segment_end;
    if (!nxloader_u64_add(segment->vma, segment->memory_size, &segment_end) ||
        !nxloader_align_up_u64(segment_end, module->page_size, &final_vma)) {
      free(protections);
      return NXLOADER_EOVERFLOW;
    }
    first_page = (size_t)((first_vma - module->minimum_vma) /
                          module->page_size);
    final_page = (size_t)((final_vma - module->minimum_vma) /
                          module->page_size);
    if (final_page > page_count || first_page > final_page) {
      free(protections);
      return NXLOADER_EBOUNDS;
    }
    for (page = first_page; page < final_page; ++page)
      protections[page] |= protection;
  }

  if (module->dynamic.relro_size) {
    uint64_t relro_end;
    uint64_t first_vma;
    uint64_t final_vma;
    size_t first_page;
    size_t final_page;
    size_t page;
    if (!nxloader_u64_add(module->dynamic.relro_vma,
                          module->dynamic.relro_size, &relro_end) ||
        !nxloader_align_up_u64(relro_end, module->page_size, &final_vma)) {
      free(protections);
      return NXLOADER_EOVERFLOW;
    }
    first_vma = nxloader_align_down_u64(module->dynamic.relro_vma,
                                        module->page_size);
    if (first_vma < module->minimum_vma || final_vma > module->maximum_vma) {
      free(protections);
      return NXLOADER_EBOUNDS;
    }
    first_page = (size_t)((first_vma - module->minimum_vma) /
                          module->page_size);
    final_page = (size_t)((final_vma - module->minimum_vma) /
                          module->page_size);
    for (page = first_page; page < final_page; ++page)
      protections[page] &= (uint8_t)~PROT_WRITE;
  }

  if (module->trampoline_pool_size) {
    size_t first_page = module->image_size / module->page_size;
    for (index = first_page; index < page_count; ++index)
      protections[index] = PROT_READ | PROT_EXEC;
  }

  if (!(module->config.flags & NXLOADER_CONFIG_ALLOW_WX_SEGMENTS)) {
    for (index = 0; index < page_count; ++index) {
      if ((protections[index] & (PROT_WRITE | PROT_EXEC)) ==
          (PROT_WRITE | PROT_EXEC)) {
        free(protections);
        return NXLOADER_EPROTECT;
      }
    }
  }

  *out_protections = protections;
  *out_page_count = page_count;
  return NXLOADER_OK;
}

static nxloader_result nxloader_apply_image_protections(
    nxloader_module *module, const uint8_t *protections, size_t page_count) {
  size_t start = 0;
  if (!module || !module->mapping || !protections || page_count == 0)
    return NXLOADER_EINVAL;
  while (start < page_count) {
    size_t end = start + 1;
    int protection = protections[start];
    while (end < page_count && protections[end] == protections[start])
      ++end;
    if (mprotect((uint8_t *)module->mapping + start * module->page_size,
                 (end - start) * module->page_size, protection) != 0) {
      nxloader_result log_result = nxloader_log(
          module, NXLOADER_LOG_ERROR,
          "mprotect failed for pages %zu..%zu: %s", start, end,
          strerror(errno));
      return log_result == NXLOADER_OK ? NXLOADER_EPROTECT : log_result;
    }
    start = end;
  }
  return NXLOADER_OK;
}

nxloader_result nxloader_protect_image(nxloader_module *module) {
  uint8_t *protections = NULL;
  size_t page_count = 0;
  size_t index;
  nxloader_result result = nxloader_build_image_protections(
      module, &protections, &page_count);
  if (result != NXLOADER_OK)
    return result;

  for (index = 0; index < module->segment_count; ++index) {
    const nxloader_segment *segment = &module->segments[index];
    if (segment->flags & PF_X) {
      char *first = (char *)nxloader_vma_pointer(
          module, segment->vma, (size_t)segment->memory_size);
      if (!first) {
        free(protections);
        return NXLOADER_EBOUNDS;
      }
      __builtin___clear_cache(first, first + segment->memory_size);
    }
  }
  if (module->trampoline_pool_used)
    __builtin___clear_cache((char *)module->trampoline_pool,
                            (char *)module->trampoline_pool +
                                module->trampoline_pool_used);
  for (index = 0; index < module->aux_pool_count; ++index) {
    result = nxloader_close_auxiliary_pool(module, index);
    if (result != NXLOADER_OK) {
      free(protections);
      return result;
    }
  }
  result = nxloader_apply_image_protections(module, protections, page_count);
  free(protections);
  return result;
}

nxloader_result nxloader_prepare_image_patch(nxloader_module *module,
                                             int *final_protections_restored) {
  uint8_t *final_protections = NULL;
  uint8_t *patch_protections = NULL;
  size_t page_count = 0;
  size_t index;
  nxloader_result result;
  nxloader_result rollback;
  if (!final_protections_restored)
    return NXLOADER_EINVAL;
  *final_protections_restored = 1;
  result = nxloader_build_image_protections(
      module, &final_protections, &page_count);
  if (result != NXLOADER_OK)
    return result;
  patch_protections = (uint8_t *)malloc(page_count);
  if (!patch_protections) {
    free(final_protections);
    return NXLOADER_ENOMEM;
  }
  memcpy(patch_protections, final_protections, page_count);
  for (index = 0; index < page_count; ++index) {
    uintptr_t page_address =
        (uintptr_t)module->mapping + index * module->page_size;
    if ((patch_protections[index] & PROT_EXEC) &&
        !nxloader_pointer_overlaps_relro(module, page_address,
                                         module->page_size))
      patch_protections[index] = PROT_READ | PROT_WRITE;
    if ((patch_protections[index] & (PROT_WRITE | PROT_EXEC)) ==
        (PROT_WRITE | PROT_EXEC)) {
      free(patch_protections);
      free(final_protections);
      return NXLOADER_EPROTECT;
    }
  }
  result = nxloader_apply_image_protections(
      module, patch_protections, page_count);
  if (result != NXLOADER_OK) {
    rollback = nxloader_apply_image_protections(
        module, final_protections, page_count);
    if (rollback != NXLOADER_OK) {
      *final_protections_restored = 0;
      result = rollback;
    }
  }
  free(patch_protections);
  free(final_protections);
  return result;
}
