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

nxloader_result nxloader_protect_image(nxloader_module *module) {
  uint8_t *protections;
  size_t page_count;
  size_t index;
  size_t start;
  if (!module || !module->mapping || module->page_size == 0 ||
      module->mapping_size % module->page_size != 0)
    return NXLOADER_EINVAL;
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

  start = 0;
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
      free(protections);
      return log_result == NXLOADER_OK ? NXLOADER_EPROTECT : log_result;
    }
    start = end;
  }
  free(protections);
  return NXLOADER_OK;
}
