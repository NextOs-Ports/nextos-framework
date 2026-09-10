/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "nxloader_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define NXLOADER_DEFAULT_MAX_FILE (512u * 1024u * 1024u)
#define NXLOADER_DEFAULT_MAX_IMAGE (1024u * 1024u * 1024u)

int nxloader_range_valid(size_t total, uint64_t offset, uint64_t length) {
  return offset <= (uint64_t)total && length <= (uint64_t)total - offset;
}

int nxloader_u64_add(uint64_t left, uint64_t right, uint64_t *result) {
  if (UINT64_MAX - left < right)
    return 0;
  if (result)
    *result = left + right;
  return 1;
}

int nxloader_u64_mul(uint64_t left, uint64_t right, uint64_t *result) {
  if (left != 0 && right > UINT64_MAX / left)
    return 0;
  if (result)
    *result = left * right;
  return 1;
}

int nxloader_is_power_of_two(uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

uint64_t nxloader_align_down_u64(uint64_t value, uint64_t alignment) {
  return value & ~(alignment - 1);
}

int nxloader_align_up_u64(uint64_t value, uint64_t alignment,
                          uint64_t *result) {
  uint64_t rounded;
  if (!nxloader_u64_add(value, alignment - 1, &rounded))
    return 0;
  if (result)
    *result = rounded & ~(alignment - 1);
  return 1;
}

char *nxloader_strdup(const char *text) {
  size_t length;
  char *copy;
  if (!text)
    return NULL;
  length = strlen(text);
  if (length == SIZE_MAX)
    return NULL;
  copy = (char *)malloc(length + 1);
  if (copy)
    memcpy(copy, text, length + 1);
  return copy;
}

nxloader_result nxloader_module_callback_guard(
    const nxloader_module *module) {
  nxloader_module *mutable_module;
  if (!module || !module->callback_active)
    return NXLOADER_OK;
  mutable_module = (nxloader_module *)module;
  mutable_module->callback_violation = 1;
  return NXLOADER_EREENTRANT;
}

static nxloader_result nxloader_callback_begin(nxloader_module *module) {
  if (!module)
    return NXLOADER_EINVAL;
  if (module->callback_active) {
    module->callback_violation = 1;
    return NXLOADER_EREENTRANT;
  }
  module->callback_active = 1;
  module->callback_violation = 0;
  return NXLOADER_OK;
}

static nxloader_result nxloader_callback_end(nxloader_module *module) {
  int violated;
  if (!module || !module->callback_active)
    return NXLOADER_ESTATE;
  violated = module->callback_violation != 0;
  module->callback_active = 0;
  module->callback_violation = 0;
  return violated ? NXLOADER_EREENTRANT : NXLOADER_OK;
}

static nxloader_result nxloader_vlog(const nxloader_module *module,
                                     nxloader_log_level level,
                                     const char *format,
                                     va_list arguments) {
  char buffer[768];
  int count;
  if (!module || !module->config.log)
    return NXLOADER_OK;
  count = vsnprintf(buffer, sizeof(buffer), format, arguments);
  if (count < 0)
    return NXLOADER_OK;
  buffer[sizeof(buffer) - 1] = '\0';
  if (nxloader_callback_begin((nxloader_module *)module) != NXLOADER_OK)
    return NXLOADER_EREENTRANT;
  module->config.log(module->config.userdata, level, buffer);
  return nxloader_callback_end((nxloader_module *)module);
}

nxloader_result nxloader_log(const nxloader_module *module,
                             nxloader_log_level level, const char *format,
                             ...) {
  nxloader_result result;
  va_list arguments;
  va_start(arguments, format);
  result = nxloader_vlog(module, level, format, arguments);
  va_end(arguments);
  return result;
}

nxloader_result nxloader_fail(nxloader_module *module, nxloader_result result,
                              const char *format, ...) {
  va_list arguments;
  nxloader_result log_result;
  va_start(arguments, format);
  log_result = nxloader_vlog(module, NXLOADER_LOG_ERROR, format, arguments);
  va_end(arguments);
  return log_result == NXLOADER_OK ? result : log_result;
}

void nxloader_config_init(nxloader_config *config) {
  if (!config)
    return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->api_version = NXLOADER_API_VERSION;
  config->expected_arch = NXLOADER_ARCH_AUTO;
  config->max_file_size = NXLOADER_DEFAULT_MAX_FILE;
  config->max_image_size = NXLOADER_DEFAULT_MAX_IMAGE;
}

nxloader_arch nxloader_process_arch(void) {
#if defined(__aarch64__)
  return NXLOADER_ARCH_AARCH64;
#elif defined(__arm__)
  return NXLOADER_ARCH_ARMV7;
#else
  return NXLOADER_ARCH_AUTO;
#endif
}

nxloader_result nxloader_module_create(const nxloader_config *config,
                                       nxloader_module **out_module) {
  nxloader_module *module;
  if (!out_module)
    return NXLOADER_EINVAL;
  *out_module = NULL;
  if (!config || config->struct_size < sizeof(*config) ||
      config->api_version != NXLOADER_API_VERSION)
    return NXLOADER_EINVAL;
  if (config->expected_arch != NXLOADER_ARCH_AUTO &&
      config->expected_arch != NXLOADER_ARCH_ARMV7 &&
      config->expected_arch != NXLOADER_ARCH_AARCH64)
    return NXLOADER_EINVAL;
  if (config->max_file_size == 0 || config->max_image_size == 0 ||
      (config->flags & ~(NXLOADER_CONFIG_ALLOW_FOREIGN_ARCH |
                         NXLOADER_CONFIG_ALLOW_WX_SEGMENTS |
                         NXLOADER_CONFIG_ALLOW_ARM_TEXT_RELOCS)))
    return NXLOADER_EINVAL;
  module = (nxloader_module *)calloc(1, sizeof(*module));
  if (!module)
    return NXLOADER_ENOMEM;
  module->config = *config;
  module->state = NXLOADER_STATE_EMPTY;
  *out_module = module;
  return NXLOADER_OK;
}

void nxloader_release_image(nxloader_module *module) {
  if (!module)
    return;
  if (module->mapping && module->mapping_size)
    (void)munmap(module->mapping, module->mapping_size);
  free(module->segments);
  free(module->dynamic.needed_offsets);
  module->mapping = NULL;
  module->mapping_size = 0;
  module->image_size = 0;
  module->page_size = 0;
  module->load_alignment = 0;
  module->minimum_vma = 0;
  module->maximum_vma = 0;
  module->runtime_bias = 0;
  module->arm_float_abi = NXLOADER_ARM_FLOAT_ABI_NOT_APPLICABLE;
  module->segments = NULL;
  module->segment_count = 0;
  memset(&module->dynamic, 0, sizeof(module->dynamic));
  module->symbol_table = NULL;
  module->string_table = NULL;
  module->symbol_count = 0;
  module->relocation_count = 0;
  module->trampoline_pool = NULL;
  module->trampoline_pool_size = 0;
  module->trampoline_pool_used = 0;
}

void nxloader_module_destroy(nxloader_module *module) {
  if (!module)
    return;
  if (nxloader_module_callback_guard(module) != NXLOADER_OK ||
      module->state == NXLOADER_STATE_INITIALIZING ||
      module->state == NXLOADER_STATE_JNI_LOADING)
    return;
  nxloader_release_image(module);
  free(module->debug_name);
  memset(module, 0, sizeof(*module));
  free(module);
}

static nxloader_result nxloader_validate_arch(nxloader_module *module,
                                              nxloader_arch arch) {
  nxloader_arch process_arch = nxloader_process_arch();
  if (module->config.expected_arch != NXLOADER_ARCH_AUTO &&
      module->config.expected_arch != arch)
    return nxloader_fail(module, NXLOADER_EARCH,
                         "ELF ABI does not match expected ABI");
  if (!(module->config.flags & NXLOADER_CONFIG_ALLOW_FOREIGN_ARCH) &&
      process_arch != arch)
    return nxloader_fail(module, NXLOADER_EARCH,
                         "ELF ABI does not match loader process ABI");
  return NXLOADER_OK;
}

nxloader_result nxloader_module_load_memory(nxloader_module *module,
                                            const void *data, size_t size,
                                            const char *debug_name) {
  const unsigned char *ident = (const unsigned char *)data;
  const uint8_t *parse_data = (const uint8_t *)data;
  uint8_t *aligned_copy = NULL;
  nxloader_result result;
  nxloader_arch arch;
  if (!module)
    return NXLOADER_EINVAL;
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return NXLOADER_EREENTRANT;
  if (!data || size < EI_NIDENT)
    return NXLOADER_EINVAL;
  if (module->state != NXLOADER_STATE_EMPTY)
    return NXLOADER_ESTATE;
  if (size > module->config.max_file_size)
    return nxloader_fail(module, NXLOADER_EBOUNDS,
                         "ELF exceeds configured file-size limit");
  if (memcmp(ident, ELFMAG, SELFMAG) != 0 ||
      ident[EI_DATA] != ELFDATA2LSB || ident[EI_VERSION] != EV_CURRENT ||
      ident[EI_OSABI] != ELFOSABI_NONE || ident[EI_ABIVERSION] != 0 ||
      memcmp(ident + EI_PAD, "\0\0\0\0\0\0\0", EI_NIDENT - EI_PAD) != 0) {
    module->state = NXLOADER_STATE_ERROR;
    return nxloader_fail(module, NXLOADER_EFORMAT,
                         "invalid or non-little-endian ELF header");
  }
  if (ident[EI_CLASS] == ELFCLASS32)
    arch = NXLOADER_ARCH_ARMV7;
  else if (ident[EI_CLASS] == ELFCLASS64)
    arch = NXLOADER_ARCH_AARCH64;
  else {
    module->state = NXLOADER_STATE_ERROR;
    return nxloader_fail(module, NXLOADER_EARCH, "unsupported ELF class");
  }
  result = nxloader_validate_arch(module, arch);
  if (result != NXLOADER_OK) {
    module->state = NXLOADER_STATE_ERROR;
    return result;
  }
  module->arch = arch;
  module->debug_name = nxloader_strdup(debug_name ? debug_name : "<memory>");
  if (!module->debug_name) {
    module->state = NXLOADER_STATE_ERROR;
    return NXLOADER_ENOMEM;
  }
  if (((uintptr_t)parse_data & (sizeof(uint64_t) - 1u)) != 0) {
    aligned_copy = (uint8_t *)malloc(size);
    if (!aligned_copy) {
      module->state = NXLOADER_STATE_ERROR;
      return NXLOADER_ENOMEM;
    }
    memcpy(aligned_copy, parse_data, size);
    parse_data = aligned_copy;
  }
  if (arch == NXLOADER_ARCH_ARMV7)
    result = nxloader_parse_elf32(module, parse_data, size);
  else
    result = nxloader_parse_elf64(module, parse_data, size);
  free(aligned_copy);
  if (result != NXLOADER_OK) {
    nxloader_release_image(module);
    module->state = NXLOADER_STATE_ERROR;
    return result;
  }
  module->state = NXLOADER_STATE_LOADED;
  result = nxloader_log(module, NXLOADER_LOG_INFO,
                        "loaded %s (%s, %zu bytes mapped)",
                        module->debug_name,
                        nxloader_arch_string(module->arch),
                        module->image_size);
  if (result != NXLOADER_OK) {
    nxloader_release_image(module);
    module->state = NXLOADER_STATE_ERROR;
    return result;
  }
  return NXLOADER_OK;
}

nxloader_result nxloader_module_load_file(nxloader_module *module,
                                          const char *path) {
  struct stat status;
  unsigned char *data = NULL;
  size_t size;
  size_t done = 0;
  int descriptor;
  nxloader_result result;
  if (!module)
    return NXLOADER_EINVAL;
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return NXLOADER_EREENTRANT;
  if (!path || !*path)
    return NXLOADER_EINVAL;
  if (module->state != NXLOADER_STATE_EMPTY)
    return NXLOADER_ESTATE;
  descriptor = open(path, O_RDONLY | O_CLOEXEC);
  if (descriptor < 0)
    return nxloader_fail(module, NXLOADER_EIO, "cannot open %s: %s", path,
                         strerror(errno));
  if (fstat(descriptor, &status) != 0 || status.st_size <= 0 ||
      (uintmax_t)status.st_size > SIZE_MAX ||
      (uintmax_t)status.st_size > module->config.max_file_size) {
    close(descriptor);
    return nxloader_fail(module, NXLOADER_EBOUNDS,
                         "invalid or oversized ELF file: %s", path);
  }
  size = (size_t)status.st_size;
  data = (unsigned char *)malloc(size);
  if (!data) {
    close(descriptor);
    return NXLOADER_ENOMEM;
  }
  while (done < size) {
    ssize_t count = read(descriptor, data + done, size - done);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      free(data);
      close(descriptor);
      return nxloader_fail(module, NXLOADER_EIO,
                           "short read while loading %s", path);
    }
    done += (size_t)count;
  }
  close(descriptor);
  result = nxloader_module_load_memory(module, data, size, path);
  free(data);
  return result;
}

nxloader_result nxloader_validate_segments(nxloader_module *module) {
  uint64_t previous_end = 0;
  uint64_t tracked_page = UINT64_MAX;
  uint32_t tracked_page_flags = 0;
  size_t index;
  int have_previous = 0;
  if (!module || !module->segments || !module->segment_count ||
      !module->page_size || !nxloader_is_power_of_two(module->page_size))
    return NXLOADER_EINVAL;
  for (index = 0; index < module->segment_count; ++index) {
    const nxloader_segment *segment = &module->segments[index];
    uint64_t segment_end;
    uint64_t first_page;
    uint64_t last_page;
    uint32_t first_page_flags = segment->flags;
    if (!segment->memory_size || segment->file_size > segment->memory_size ||
        (segment->flags & ~(uint32_t)(PF_R | PF_W | PF_X)) ||
        !nxloader_u64_add(segment->vma, segment->memory_size, &segment_end))
      return nxloader_fail(module, NXLOADER_EFORMAT,
                           "incoherent PT_LOAD segment");
    if (have_previous && segment->vma < previous_end)
      return nxloader_fail(module, NXLOADER_EFORMAT,
                           "PT_LOAD segments overlap or are not VMA-ordered");
    first_page = nxloader_align_down_u64(segment->vma, module->page_size);
    last_page = nxloader_align_down_u64(segment_end - 1, module->page_size);
    if (first_page == tracked_page)
      first_page_flags |= tracked_page_flags;
    if (!(module->config.flags & NXLOADER_CONFIG_ALLOW_WX_SEGMENTS) &&
        (first_page_flags & (PF_W | PF_X)) == (PF_W | PF_X))
      return nxloader_fail(module, NXLOADER_EPROTECT,
                           "PT_LOAD page would require write+execute access");
    tracked_page = last_page;
    tracked_page_flags = last_page == first_page
                             ? first_page_flags
                             : segment->flags;
    previous_end = segment_end;
    have_previous = 1;
  }
  return NXLOADER_OK;
}

int nxloader_file_mapping_matches(const nxloader_module *module, uint64_t vma,
                                  uint64_t file_offset, uint64_t size) {
  const nxloader_segment *segment;
  uint64_t requested_vma_end;
  uint64_t requested_file_end;
  uint64_t segment_vma_end;
  uint64_t segment_file_end;
  uint64_t expected_offset;
  if (!module || !nxloader_u64_add(vma, size, &requested_vma_end) ||
      !nxloader_u64_add(file_offset, size, &requested_file_end) ||
      size > SIZE_MAX)
    return 0;
  segment = nxloader_segment_for_vma_range(module, vma, (size_t)size);
  if (!segment ||
      !nxloader_u64_add(segment->vma, segment->file_size, &segment_vma_end) ||
      !nxloader_u64_add(segment->file_offset, segment->file_size,
                        &segment_file_end) ||
      vma < segment->vma || requested_vma_end > segment_vma_end ||
      file_offset < segment->file_offset || requested_file_end > segment_file_end ||
      !nxloader_u64_add(segment->file_offset, vma - segment->vma,
                        &expected_offset))
    return 0;
  return expected_offset == file_offset;
}

nxloader_result nxloader_validate_relro(const nxloader_module *module) {
  uint64_t relro_end;
  uint64_t first_page;
  uint64_t final_page;
  uint64_t covered_until;
  size_t index;
  if (!module)
    return NXLOADER_EINVAL;
  if (!module->dynamic.relro_size)
    return NXLOADER_OK;
  if (!nxloader_u64_add(module->dynamic.relro_vma,
                        module->dynamic.relro_size, &relro_end) ||
      !nxloader_align_up_u64(relro_end, module->page_size, &final_page))
    return NXLOADER_EOVERFLOW;
  first_page = nxloader_align_down_u64(module->dynamic.relro_vma,
                                      module->page_size);
  if (first_page < module->minimum_vma || final_page > module->maximum_vma ||
      final_page <= first_page)
    return NXLOADER_EBOUNDS;
  /* PT_LOAD entries are already VMA-ordered and non-overlapping.  Sweep their
   * page intervals once: the previous page-by-page nested scan was
   * O(RELRO-pages * PT_LOAD-count) for a hostile program-header table. */
  covered_until = first_page;
  for (index = 0; index < module->segment_count; ++index) {
    const nxloader_segment *segment = &module->segments[index];
    uint64_t segment_end;
    uint64_t segment_first_page;
    uint64_t segment_final_page;
    if (!nxloader_u64_add(segment->vma, segment->memory_size, &segment_end) ||
        !nxloader_align_up_u64(segment_end, module->page_size,
                               &segment_final_page))
      return NXLOADER_EOVERFLOW;
    segment_first_page =
        nxloader_align_down_u64(segment->vma, module->page_size);
    if (segment_final_page <= covered_until)
      continue;
    if (segment_first_page > covered_until)
      return NXLOADER_EFORMAT;
    covered_until = segment_final_page;
    if (covered_until >= final_page)
      return NXLOADER_OK;
  }
  return NXLOADER_EFORMAT;
}

nxloader_result nxloader_allocate_image(nxloader_module *module,
                                        const uint8_t *file_data,
                                        size_t file_size) {
  uint64_t total64;
  size_t trampoline_size = 0;
  size_t reserve_size;
  size_t alignment;
  size_t index;
  int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
  void *mapping_hint = NULL;
  void *reservation;
  void *mapping;
  if (!module || !module->segments || module->segment_count == 0 ||
      module->maximum_vma <= module->minimum_vma)
    return NXLOADER_EINVAL;
  total64 = module->maximum_vma - module->minimum_vma;
  if (total64 > SIZE_MAX || total64 > module->config.max_image_size)
    return nxloader_fail(module, NXLOADER_EBOUNDS,
                         "ELF image exceeds configured image-size limit");
  module->image_size = (size_t)total64;
  if (module->config.trampoline_pool_size) {
    uint64_t rounded;
    if (!nxloader_align_up_u64(module->config.trampoline_pool_size,
                               module->page_size, &rounded) ||
        rounded > SIZE_MAX)
      return NXLOADER_EOVERFLOW;
    trampoline_size = (size_t)rounded;
  }
  if (SIZE_MAX - module->image_size < trampoline_size)
    return NXLOADER_EOVERFLOW;
  module->mapping_size = module->image_size + trampoline_size;
  alignment = module->load_alignment > module->page_size
                  ? module->load_alignment
                  : module->page_size;
  if (!nxloader_is_power_of_two(alignment) ||
      SIZE_MAX - module->mapping_size < alignment) {
    module->mapping_size = 0;
    return NXLOADER_EOVERFLOW;
  }
  reserve_size = module->mapping_size + alignment;
#if defined(MAP_32BIT) && defined(__x86_64__)
  if (module->arch == NXLOADER_ARCH_ARMV7)
    mmap_flags |= MAP_32BIT;
#elif UINTPTR_MAX > UINT32_MAX
  /* Foreign-ABI validation on a 64-bit host still needs representable ARMv7
   * relocation values. This is a non-fixed hint; a real ARMv7 loader process
   * naturally has a 32-bit address space. */
  if (module->arch == NXLOADER_ARCH_ARMV7)
    mapping_hint = (void *)(uintptr_t)UINT32_C(0x10000000);
#endif
  reservation = mmap(mapping_hint, reserve_size, PROT_READ | PROT_WRITE,
                     mmap_flags, -1, 0);
  if (reservation == MAP_FAILED) {
    module->mapping_size = 0;
    return nxloader_fail(module, NXLOADER_ENOMEM, "mmap failed: %s",
                         strerror(errno));
  }
  {
    uintptr_t raw = (uintptr_t)reservation;
    uintptr_t wanted_remainder = (uintptr_t)module->minimum_vma &
                                 (uintptr_t)(alignment - 1);
    uintptr_t adjustment =
        (wanted_remainder - (raw & (uintptr_t)(alignment - 1))) &
        (uintptr_t)(alignment - 1);
    size_t prefix = (size_t)adjustment;
    size_t suffix;
    mapping = (void *)(raw + adjustment);
    if (prefix > reserve_size || module->mapping_size > reserve_size - prefix) {
      (void)munmap(reservation, reserve_size);
      module->mapping_size = 0;
      return NXLOADER_EOVERFLOW;
    }
    suffix = reserve_size - prefix - module->mapping_size;
    if (prefix && munmap(reservation, prefix) != 0) {
      (void)munmap(reservation, reserve_size);
      module->mapping_size = 0;
      return nxloader_fail(module, NXLOADER_ENOMEM,
                           "could not trim aligned mapping prefix");
    }
    if (suffix &&
        munmap((uint8_t *)mapping + module->mapping_size, suffix) != 0) {
      (void)munmap(mapping, reserve_size - prefix);
      module->mapping_size = 0;
      return nxloader_fail(module, NXLOADER_ENOMEM,
                           "could not trim aligned mapping suffix");
    }
  }
  module->mapping = mapping;
  if ((uint64_t)(uintptr_t)mapping < module->minimum_vma) {
    nxloader_release_image(module);
    return nxloader_fail(module, NXLOADER_EOVERFLOW,
                         "mapping cannot represent ELF load bias");
  }
  module->runtime_bias = (uintptr_t)mapping - (uintptr_t)module->minimum_vma;
  if ((module->runtime_bias & (uintptr_t)(alignment - 1)) != 0) {
    nxloader_release_image(module);
    return nxloader_fail(module, NXLOADER_EOVERFLOW,
                         "mapping does not satisfy PT_LOAD alignment");
  }
  if (module->arch == NXLOADER_ARCH_ARMV7) {
    uint64_t limit = UINT64_C(0x100000000);
    uint64_t base = (uint64_t)(uintptr_t)module->mapping;
    if ((uint64_t)module->mapping_size > limit ||
        base > limit - (uint64_t)module->mapping_size) {
      nxloader_release_image(module);
      return nxloader_fail(
          module, NXLOADER_EOVERFLOW,
          "ARMv7 image and veneer pool were not mapped below 4 GiB");
    }
  }
  for (index = 0; index < module->segment_count; ++index) {
    const nxloader_segment *segment = &module->segments[index];
    void *destination;
    if (!nxloader_range_valid(file_size, segment->file_offset,
                              segment->file_size)) {
      nxloader_release_image(module);
      return NXLOADER_EBOUNDS;
    }
    destination = nxloader_vma_pointer(module, segment->vma,
                                       (size_t)segment->file_size);
    if (!destination) {
      nxloader_release_image(module);
      return NXLOADER_EBOUNDS;
    }
    memcpy(destination, file_data + segment->file_offset,
           (size_t)segment->file_size);
  }
  if (trampoline_size) {
    module->trampoline_pool =
        (uint8_t *)module->mapping + module->image_size;
    module->trampoline_pool_size = module->config.trampoline_pool_size;
  }
  return NXLOADER_OK;
}

const nxloader_segment *nxloader_segment_for_vma_range(
    const nxloader_module *module, uint64_t vma, size_t size) {
  uint64_t requested_end;
  size_t low;
  size_t high;
  const nxloader_segment *segment;
  uint64_t segment_end;
  if (!module || !module->segments || module->segment_count == 0 ||
      !nxloader_u64_add(vma, size, &requested_end))
    return NULL;
  /* upper_bound(segment.vma, vma), then validate the single candidate.  The
   * parser has already rejected unordered or overlapping PT_LOAD entries. */
  low = 0;
  high = module->segment_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (module->segments[middle].vma <= vma)
      low = middle + 1;
    else
      high = middle;
  }
  if (low == 0)
    return NULL;
  segment = &module->segments[low - 1];
  if (!nxloader_u64_add(segment->vma, segment->memory_size, &segment_end) ||
      vma < segment->vma || requested_end > segment_end)
    return NULL;
  return segment;
}

void *nxloader_vma_pointer(const nxloader_module *module, uint64_t vma,
                           size_t size) {
  uint64_t offset;
  if (!module || !module->mapping || vma < module->minimum_vma)
    return NULL;
  offset = vma - module->minimum_vma;
  if (offset > module->image_size || size > module->image_size - (size_t)offset ||
      !nxloader_segment_for_vma_range(module, vma, size))
    return NULL;
  return (uint8_t *)module->mapping + (size_t)offset;
}

void *nxloader_module_vma_to_pointer(const nxloader_module *module,
                                     uint64_t vma, size_t size) {
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return NULL;
  return nxloader_vma_pointer(module, vma, size);
}

int nxloader_pointer_is_executable(const nxloader_module *module,
                                   uintptr_t address, size_t size) {
  uintptr_t clean = address;
  uint64_t vma;
  const nxloader_segment *segment;
  if (!module)
    return 0;
  if (module->arch == NXLOADER_ARCH_ARMV7)
    clean &= ~(uintptr_t)1;
  if (!module->mapping || clean < module->runtime_bias)
    return 0;
  vma = (uint64_t)(clean - module->runtime_bias);
  segment = nxloader_segment_for_vma_range(module, vma, size);
  return segment && (segment->flags & PF_X) != 0;
}

static nxloader_result nxloader_relocation_metadata_overlap(
    uint64_t target_vma, uint64_t target_end, uint64_t metadata_vma,
    uint64_t metadata_size) {
  uint64_t metadata_end;
  if (metadata_size == 0)
    return NXLOADER_OK;
  if (!nxloader_u64_add(metadata_vma, metadata_size, &metadata_end))
    return NXLOADER_EOVERFLOW;
  if (target_vma < metadata_end && metadata_vma < target_end)
    return NXLOADER_EFORMAT;
  return NXLOADER_OK;
}

nxloader_result nxloader_validate_relocation_metadata_target(
    const nxloader_module *module, uint64_t target_vma, size_t width) {
  const nxloader_dynamic_info *dynamic;
  uint64_t target_end;
  uint64_t symbol_table_size;
  nxloader_result result;
#define NXLOADER_REJECT_METADATA_RANGE(vma, size)                             \
  do {                                                                        \
    result = nxloader_relocation_metadata_overlap(                            \
        target_vma, target_end, (vma), (uint64_t)(size));                     \
    if (result != NXLOADER_OK)                                                \
      return result;                                                          \
  } while (0)
  if (!module || width == 0)
    return NXLOADER_EINVAL;
  if (!nxloader_u64_add(target_vma, width, &target_end))
    return NXLOADER_EOVERFLOW;
  dynamic = &module->dynamic;
  if (!nxloader_u64_mul(module->symbol_count, dynamic->symbol_entry_size,
                        &symbol_table_size))
    return NXLOADER_EOVERFLOW;
  NXLOADER_REJECT_METADATA_RANGE(dynamic->dynamic_vma, dynamic->dynamic_size);
  NXLOADER_REJECT_METADATA_RANGE(dynamic->string_table_vma,
                                 dynamic->string_table_size);
  NXLOADER_REJECT_METADATA_RANGE(dynamic->symbol_table_vma, symbol_table_size);
  NXLOADER_REJECT_METADATA_RANGE(dynamic->sysv_hash_vma,
                                 dynamic->sysv_hash_size);
  NXLOADER_REJECT_METADATA_RANGE(dynamic->gnu_hash_vma,
                                 dynamic->gnu_hash_size);
  NXLOADER_REJECT_METADATA_RANGE(dynamic->rel_vma, dynamic->rel_size);
  NXLOADER_REJECT_METADATA_RANGE(dynamic->rela_vma, dynamic->rela_size);
  NXLOADER_REJECT_METADATA_RANGE(dynamic->plt_relocation_vma,
                                 dynamic->plt_relocation_size);
#undef NXLOADER_REJECT_METADATA_RANGE
  return NXLOADER_OK;
}

nxloader_result nxloader_pending_add(nxloader_pending_list *pending,
                                     void *target, uint64_t value,
                                     uint8_t width) {
  nxloader_pending_write *items;
  size_t capacity;
  uintptr_t target_start;
  if (!pending || !target || (width != 2 && width != 4 && width != 8))
    return NXLOADER_EINVAL;
  target_start = (uintptr_t)target;
  if (target_start > UINTPTR_MAX - width)
    return NXLOADER_EOVERFLOW;
  if (pending->count == pending->capacity) {
    capacity = pending->capacity ? pending->capacity * 2 : 64;
    if (capacity < pending->count ||
        capacity > SIZE_MAX / sizeof(*pending->items))
      return NXLOADER_EOVERFLOW;
    items = (nxloader_pending_write *)realloc(
        pending->items, capacity * sizeof(*pending->items));
    if (!items)
      return NXLOADER_ENOMEM;
    pending->items = items;
    pending->capacity = capacity;
  }
  pending->items[pending->count].target = target;
  pending->items[pending->count].value = value;
  pending->items[pending->count].width = width;
  pending->count++;
  return NXLOADER_OK;
}

static int nxloader_pending_compare(const nxloader_pending_write *left,
                                    const nxloader_pending_write *right) {
  uintptr_t left_start = (uintptr_t)left->target;
  uintptr_t right_start = (uintptr_t)right->target;
  if (left_start < right_start)
    return -1;
  if (left_start > right_start)
    return 1;
  if (left->width < right->width)
    return -1;
  if (left->width > right->width)
    return 1;
  return 0;
}

static void nxloader_pending_swap(nxloader_pending_write *left,
                                  nxloader_pending_write *right) {
  nxloader_pending_write temporary = *left;
  *left = *right;
  *right = temporary;
}

static void nxloader_pending_sift_down(nxloader_pending_write *items,
                                       size_t root, size_t count) {
  while (root < count / 2) {
    size_t child = root * 2 + 1;
    if (child + 1 < count &&
        nxloader_pending_compare(&items[child], &items[child + 1]) < 0)
      child++;
    if (nxloader_pending_compare(&items[root], &items[child]) >= 0)
      return;
    nxloader_pending_swap(&items[root], &items[child]);
    root = child;
  }
}

static void nxloader_pending_sort(nxloader_pending_write *items, size_t count) {
  size_t start;
  size_t end;
  if (count < 2)
    return;
  for (start = count / 2; start > 0; --start)
    nxloader_pending_sift_down(items, start - 1, count);
  for (end = count; end > 1; --end) {
    nxloader_pending_swap(&items[0], &items[end - 1]);
    nxloader_pending_sift_down(items, 0, end - 1);
  }
}

nxloader_result nxloader_pending_validate(nxloader_pending_list *pending) {
  size_t index;
  if (!pending)
    return NXLOADER_EINVAL;
  if (pending->count > pending->capacity ||
      (pending->count != 0 && !pending->items))
    return NXLOADER_EINVAL;
  nxloader_pending_sort(pending->items, pending->count);
  for (index = 0; index < pending->count; ++index) {
    uintptr_t start = (uintptr_t)pending->items[index].target;
    uint8_t width = pending->items[index].width;
    if (!pending->items[index].target ||
        (width != 2 && width != 4 && width != 8))
      return NXLOADER_EINVAL;
    if (start > UINTPTR_MAX - width)
      return NXLOADER_EOVERFLOW;
    if (index != 0) {
      uintptr_t previous_start =
          (uintptr_t)pending->items[index - 1].target;
      uint8_t previous_width = pending->items[index - 1].width;
      if (previous_start > UINTPTR_MAX - previous_width)
        return NXLOADER_EOVERFLOW;
      if (start < previous_start + previous_width)
        return NXLOADER_EFORMAT;
    }
  }
  return NXLOADER_OK;
}

nxloader_result nxloader_pending_commit(nxloader_pending_list *pending) {
  size_t index;
  nxloader_result result = nxloader_pending_validate(pending);
  if (result != NXLOADER_OK)
    return result;
  for (index = 0; index < pending->count; ++index) {
    if (pending->items[index].width == 2)
      nxloader_write_u16(pending->items[index].target,
                         (uint16_t)pending->items[index].value);
    else if (pending->items[index].width == 4)
      nxloader_write_u32(pending->items[index].target,
                         (uint32_t)pending->items[index].value);
    else
      nxloader_write_u64(pending->items[index].target,
                         pending->items[index].value);
  }
  return NXLOADER_OK;
}

void nxloader_pending_dispose(nxloader_pending_list *pending) {
  if (!pending)
    return;
  free(pending->items);
  memset(pending, 0, sizeof(*pending));
}

uint16_t nxloader_read_u16(const void *pointer) {
  uint16_t value;
  memcpy(&value, pointer, sizeof(value));
  return value;
}

uint32_t nxloader_read_u32(const void *pointer) {
  uint32_t value;
  memcpy(&value, pointer, sizeof(value));
  return value;
}

uint64_t nxloader_read_u64(const void *pointer) {
  uint64_t value;
  memcpy(&value, pointer, sizeof(value));
  return value;
}

void nxloader_write_u16(void *pointer, uint16_t value) {
  memcpy(pointer, &value, sizeof(value));
}

void nxloader_write_u32(void *pointer, uint32_t value) {
  memcpy(pointer, &value, sizeof(value));
}

void nxloader_write_u64(void *pointer, uint64_t value) {
  memcpy(pointer, &value, sizeof(value));
}

nxloader_result nxloader_apply_relocation_hook(
    nxloader_module *module, const nxloader_reloc_info *info,
    uint64_t default_value, int has_default, int *should_write,
    uint64_t *write_value) {
  nxloader_reloc_action action;
  uint64_t value = default_value;
  if (!module || !info || !should_write || !write_value)
    return NXLOADER_EINVAL;
  *should_write = has_default;
  *write_value = default_value;
  if (!module->config.relocation_hook)
    return has_default ? NXLOADER_OK : NXLOADER_ERELOC;
  if (nxloader_callback_begin(module) != NXLOADER_OK)
    return NXLOADER_EREENTRANT;
  action = module->config.relocation_hook(module->config.userdata, module, info,
                                          &value);
  if (nxloader_callback_end(module) != NXLOADER_OK)
    return NXLOADER_EREENTRANT;
  switch (action) {
  case NXLOADER_RELOC_USE_DEFAULT:
    return has_default ? NXLOADER_OK : NXLOADER_ERELOC;
  case NXLOADER_RELOC_WRITE:
    *should_write = 1;
    *write_value = value;
    return NXLOADER_OK;
  case NXLOADER_RELOC_SKIP:
    *should_write = 0;
    return NXLOADER_OK;
  case NXLOADER_RELOC_REJECT:
  default:
    return NXLOADER_ECALLBACK;
  }
}

nxloader_result nxloader_apply_alias(const nxloader_module *module,
                                     const char *name,
                                     const char **out_alias) {
  const char *alias;
  if (!module || !name || !out_alias)
    return NXLOADER_EINVAL;
  *out_alias = name;
  if (!module->config.alias)
    return NXLOADER_OK;
  if (nxloader_callback_begin((nxloader_module *)module) != NXLOADER_OK)
    return NXLOADER_EREENTRANT;
  alias = module->config.alias(module->config.userdata, module, name);
  if (nxloader_callback_end((nxloader_module *)module) != NXLOADER_OK)
    return NXLOADER_EREENTRANT;
  if (alias && *alias)
    *out_alias = alias;
  return NXLOADER_OK;
}

nxloader_result nxloader_apply_initializer_filter(
    nxloader_module *module, const nxloader_initializer_info *info,
    nxloader_initializer_action *out_action) {
  nxloader_initializer_action action = NXLOADER_INITIALIZER_RUN;
  if (!module || !info || !out_action)
    return NXLOADER_EINVAL;
  if (module->config.initializer_filter) {
    if (nxloader_callback_begin(module) != NXLOADER_OK)
      return NXLOADER_EREENTRANT;
    action = module->config.initializer_filter(module->config.userdata, module,
                                               info);
    if (nxloader_callback_end(module) != NXLOADER_OK)
      return NXLOADER_EREENTRANT;
  }
  if (action != NXLOADER_INITIALIZER_RUN &&
      action != NXLOADER_INITIALIZER_SKIP &&
      action != NXLOADER_INITIALIZER_REJECT)
    return NXLOADER_ECALLBACK;
  *out_action = action;
  return action == NXLOADER_INITIALIZER_REJECT ? NXLOADER_ECALLBACK
                                                : NXLOADER_OK;
}

const char *nxloader_checked_string(const nxloader_module *module,
                                    uint32_t offset) {
  const char *value;
  size_t available;
  size_t bounded;
  if (!module || !module->string_table ||
      offset >= module->dynamic.string_table_size)
    return NULL;
  value = module->string_table + offset;
  available = module->dynamic.string_table_size - offset;
  bounded = available;
  if (bounded > (size_t)NXLOADER_MAX_DYNAMIC_NAME_LENGTH + 1u)
    bounded = (size_t)NXLOADER_MAX_DYNAMIC_NAME_LENGTH + 1u;
  if (!memchr(value, '\0', bounded))
    return NULL;
  return value;
}

int nxloader_dynamic_name_valid(const char *name) {
  const unsigned char *cursor = (const unsigned char *)name;
  if (!cursor || !*cursor || strcmp(name, ".") == 0 ||
      strcmp(name, "..") == 0)
    return 0;
  while (*cursor) {
    if (*cursor <= 0x20 || *cursor == 0x7f || *cursor == '/' ||
        *cursor == '\\')
      return 0;
    cursor++;
  }
  return 1;
}

nxloader_result nxloader_module_relocate(nxloader_module *module) {
  nxloader_result result;
  if (!module)
    return NXLOADER_EINVAL;
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return NXLOADER_EREENTRANT;
  if (module->state != NXLOADER_STATE_LOADED)
    return NXLOADER_ESTATE;
  result = module->arch == NXLOADER_ARCH_ARMV7
               ? nxloader_relocate_elf32(module)
               : nxloader_relocate_elf64(module);
  if (result == NXLOADER_OK)
    module->state = NXLOADER_STATE_RELOCATED;
  return result;
}

nxloader_result nxloader_module_resolve(
    nxloader_module *module, const nxloader_registry *registry, uint32_t flags,
    nxloader_resolution_report *report) {
  nxloader_resolution_report local_report;
  nxloader_resolution_report *active_report = NULL;
  nxloader_result result;
  if (!module)
    return NXLOADER_EINVAL;
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return NXLOADER_EREENTRANT;
  if (!registry)
    return NXLOADER_EINVAL;
  if (module->state != NXLOADER_STATE_RELOCATED)
    return NXLOADER_ESTATE;
  if (flags & ~NXLOADER_RESOLVE_ALLOW_UNRESOLVED)
    return NXLOADER_EINVAL;
  if (report) {
    if (report->struct_size < sizeof(*report))
      return NXLOADER_EINVAL;
    memset(&local_report, 0, sizeof(local_report));
    local_report.struct_size = sizeof(local_report);
    active_report = &local_report;
  }
  result = module->arch == NXLOADER_ARCH_ARMV7
               ? nxloader_resolve_elf32(module, registry, flags, active_report)
               : nxloader_resolve_elf64(module, registry, flags, active_report);
  if (report && result != NXLOADER_EREENTRANT) {
    size_t struct_size = report->struct_size;
    *report = local_report;
    report->struct_size = struct_size;
  }
  if (result == NXLOADER_OK)
    module->state = NXLOADER_STATE_RESOLVED;
  return result;
}

nxloader_result nxloader_module_finalize(nxloader_module *module) {
  nxloader_result result;
  if (!module)
    return NXLOADER_EINVAL;
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return NXLOADER_EREENTRANT;
  if (module->state != NXLOADER_STATE_RESOLVED)
    return NXLOADER_ESTATE;
  result = nxloader_protect_image(module);
  if (result == NXLOADER_OK)
    module->state = NXLOADER_STATE_FINALIZED;
  else
    module->state = NXLOADER_STATE_ERROR;
  return result;
}

nxloader_result nxloader_module_call_initializers(nxloader_module *module) {
  nxloader_result result;
  if (!module)
    return NXLOADER_EINVAL;
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return NXLOADER_EREENTRANT;
  if (module->state != NXLOADER_STATE_FINALIZED)
    return NXLOADER_ESTATE;
  result = module->arch == NXLOADER_ARCH_ARMV7
               ? nxloader_call_initializers_elf32(module)
               : nxloader_call_initializers_elf64(module);
  if (result == NXLOADER_OK) {
    if (module->state != NXLOADER_STATE_INITIALIZING) {
      module->state = NXLOADER_STATE_ERROR;
      return NXLOADER_ESTATE;
    }
    module->state = NXLOADER_STATE_INITIALIZED;
  } else if (result != NXLOADER_EARCH) {
    module->state = NXLOADER_STATE_ERROR;
  }
  return result;
}

nxloader_result nxloader_module_call_jni_onload(
    nxloader_module *module, const nxloader_jni_onload_options *options,
    int32_t *out_version) {
  typedef int32_t (*nxloader_jni_onload_fn)(void *java_vm, void *reserved);
  nxloader_jni_onload_fn function;
  uintptr_t address = 0;
  uint32_t symbol_type = STT_NOTYPE;
  nxloader_result result;
  int32_t version;
  size_t index;
  size_t prior;
  int accepted = 0;
  if (!module)
    return NXLOADER_EINVAL;
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return NXLOADER_EREENTRANT;
  if (!options || !out_version)
    return NXLOADER_EINVAL;
  if (options->struct_size < sizeof(*options) || !options->java_vm ||
      options->reserved || !options->accepted_versions ||
      options->accepted_version_count == 0 ||
      options->accepted_version_count >
          NXLOADER_JNI_ONLOAD_MAX_ACCEPTED_VERSIONS ||
      (options->flags & ~NXLOADER_JNI_ONLOAD_OPTIONAL))
    return NXLOADER_EINVAL;
  for (index = 0; index < options->accepted_version_count; ++index) {
    if (options->accepted_versions[index] <= 0)
      return NXLOADER_EINVAL;
    for (prior = 0; prior < index; ++prior) {
      if (options->accepted_versions[prior] ==
          options->accepted_versions[index])
        return NXLOADER_EINVAL;
    }
  }
  if (module->state != NXLOADER_STATE_INITIALIZED)
    return NXLOADER_ESTATE;
  result = module->arch == NXLOADER_ARCH_ARMV7
               ? nxloader_find_export_elf32(module, "JNI_OnLoad", &address,
                                            NULL, &symbol_type)
               : nxloader_find_export_elf64(module, "JNI_OnLoad", &address,
                                            NULL, &symbol_type);
  if (result == NXLOADER_EUNRESOLVED &&
      (options->flags & NXLOADER_JNI_ONLOAD_OPTIONAL)) {
    *out_version = 0;
    module->state = NXLOADER_STATE_READY;
    return NXLOADER_OK;
  }
  if (result != NXLOADER_OK) {
    module->state = NXLOADER_STATE_ERROR;
    return result;
  }
  if (symbol_type != STT_FUNC ||
      !nxloader_pointer_is_executable(module, address, 1)) {
    module->state = NXLOADER_STATE_ERROR;
    return NXLOADER_EUNSUPPORTED;
  }
  if (nxloader_process_arch() != module->arch) {
    module->state = NXLOADER_STATE_ERROR;
    return NXLOADER_EARCH;
  }
  if (sizeof(function) != sizeof(address)) {
    module->state = NXLOADER_STATE_ERROR;
    return NXLOADER_EUNSUPPORTED;
  }
  memcpy(&function, &address, sizeof(function));
  module->state = NXLOADER_STATE_JNI_LOADING;
  version = function(options->java_vm, NULL);
  for (index = 0; index < options->accepted_version_count; ++index) {
    if (options->accepted_versions[index] == version) {
      accepted = 1;
      break;
    }
  }
  if (!accepted) {
    module->state = NXLOADER_STATE_ERROR;
    return NXLOADER_EUNSUPPORTED;
  }
  *out_version = version;
  module->state = NXLOADER_STATE_READY;
  return NXLOADER_OK;
}

nxloader_result nxloader_module_get_info(const nxloader_module *module,
                                         nxloader_module_info *info) {
  if (!module)
    return NXLOADER_EINVAL;
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return NXLOADER_EREENTRANT;
  if (!info || info->struct_size < sizeof(*info))
    return NXLOADER_EINVAL;
  info->arch = module->arch;
  info->state = module->state;
  info->elf_flags = module->elf_flags;
  info->arm_float_abi = module->arm_float_abi;
  info->mapping_base = module->mapping;
  info->mapping_size = module->mapping_size;
  info->image_size = module->image_size;
  info->load_alignment = module->load_alignment;
  info->minimum_vma = module->minimum_vma;
  info->maximum_vma = module->maximum_vma;
  info->segment_count = module->segment_count;
  info->symbol_count = module->symbol_count;
  info->relocation_count = module->relocation_count;
  info->needed_count = module->dynamic.needed_count;
  return NXLOADER_OK;
}

nxloader_state nxloader_module_get_state(const nxloader_module *module) {
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return NXLOADER_STATE_ERROR;
  return module ? module->state : NXLOADER_STATE_ERROR;
}

nxloader_arch nxloader_module_get_arch(const nxloader_module *module) {
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return NXLOADER_ARCH_AUTO;
  return module ? module->arch : NXLOADER_ARCH_AUTO;
}

nxloader_result nxloader_module_find_export(const nxloader_module *module,
                                            const char *name,
                                            uintptr_t *address) {
  const char *query;
  nxloader_result result;
  if (!module)
    return NXLOADER_EINVAL;
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return NXLOADER_EREENTRANT;
  if (!name || !*name || !address)
    return NXLOADER_EINVAL;
  if (module->state == NXLOADER_STATE_EMPTY ||
      module->state == NXLOADER_STATE_ERROR)
    return NXLOADER_ESTATE;
  result = nxloader_apply_alias(module, name, &query);
  if (result != NXLOADER_OK)
    return result;
  return module->arch == NXLOADER_ARCH_ARMV7
             ? nxloader_find_export_elf32(module, query, address, NULL, NULL)
             : nxloader_find_export_elf64(module, query, address, NULL, NULL);
}

nxloader_result nxloader_module_find_relocation(const nxloader_module *module,
                                                const char *name,
                                                uintptr_t *slot_address) {
  if (!module)
    return NXLOADER_EINVAL;
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return NXLOADER_EREENTRANT;
  if (!name || !*name || !slot_address)
    return NXLOADER_EINVAL;
  if (module->state == NXLOADER_STATE_EMPTY ||
      module->state == NXLOADER_STATE_ERROR)
    return NXLOADER_ESTATE;
  return module->arch == NXLOADER_ARCH_ARMV7
             ? nxloader_find_relocation_elf32(module, name, slot_address)
             : nxloader_find_relocation_elf64(module, name, slot_address);
}

size_t nxloader_module_needed_count(const nxloader_module *module) {
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return 0;
  return module ? module->dynamic.needed_count : 0;
}

const char *nxloader_module_needed(const nxloader_module *module,
                                   size_t index) {
  if (!module)
    return NULL;
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return NULL;
  if (index >= module->dynamic.needed_count)
    return NULL;
  return nxloader_checked_string(module, module->dynamic.needed_offsets[index]);
}

const char *nxloader_module_soname(const nxloader_module *module) {
  if (!module)
    return NULL;
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return NULL;
  if (!module->dynamic.has_soname)
    return NULL;
  return nxloader_checked_string(module, module->dynamic.soname_offset);
}

nxloader_result nxloader_module_find_arm_exidx(
    const nxloader_module *module, uintptr_t program_counter,
    uintptr_t *table_address, size_t *entry_count) {
  void *table;
  if (!module)
    return NXLOADER_EINVAL;
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return NXLOADER_EREENTRANT;
  if (!table_address || !entry_count)
    return NXLOADER_EINVAL;
  if (module->state == NXLOADER_STATE_EMPTY ||
      module->state == NXLOADER_STATE_ERROR)
    return NXLOADER_ESTATE;
  if (module->arch != NXLOADER_ARCH_ARMV7)
    return NXLOADER_EARCH;
  if (!nxloader_pointer_is_executable(module, program_counter, 1))
    return NXLOADER_EUNRESOLVED;
  if (!module->dynamic.arm_exidx_vma || !module->dynamic.arm_exidx_size ||
      module->dynamic.arm_exidx_size % 8 != 0)
    return NXLOADER_EUNSUPPORTED;
  table = nxloader_vma_pointer(module, module->dynamic.arm_exidx_vma,
                               module->dynamic.arm_exidx_size);
  if (!table)
    return NXLOADER_EBOUNDS;
  *table_address = (uintptr_t)table;
  *entry_count = module->dynamic.arm_exidx_size / 8;
  return NXLOADER_OK;
}

const char *nxloader_result_string(nxloader_result result) {
  switch (result) {
  case NXLOADER_OK: return "success";
  case NXLOADER_EINVAL: return "invalid argument";
  case NXLOADER_ENOMEM: return "out of memory";
  case NXLOADER_EIO: return "I/O failure";
  case NXLOADER_EFORMAT: return "invalid ELF format";
  case NXLOADER_EARCH: return "architecture mismatch";
  case NXLOADER_EBOUNDS: return "out-of-bounds ELF data";
  case NXLOADER_ESTATE: return "invalid lifecycle state";
  case NXLOADER_EPROTECT: return "memory protection failure";
  case NXLOADER_ERELOC: return "unsupported or invalid relocation";
  case NXLOADER_EUNRESOLVED: return "unresolved strong import";
  case NXLOADER_ECOLLISION: return "import-provider collision";
  case NXLOADER_EOVERFLOW: return "integer or address overflow";
  case NXLOADER_EUNSUPPORTED: return "unsupported operation";
  case NXLOADER_ECALLBACK: return "callback rejected operation";
  case NXLOADER_EREENTRANT: return "reentrant nxloader callback operation";
  default: return "unknown nxloader error";
  }
}

const char *nxloader_arch_string(nxloader_arch arch) {
  switch (arch) {
  case NXLOADER_ARCH_ARMV7: return "ARMv7/ELF32";
  case NXLOADER_ARCH_AARCH64: return "AArch64/ELF64";
  case NXLOADER_ARCH_AUTO: default: return "unknown";
  }
}

const char *nxloader_arm_float_abi_string(nxloader_arm_float_abi abi) {
  switch (abi) {
  case NXLOADER_ARM_FLOAT_ABI_NOT_APPLICABLE: return "not-applicable";
  case NXLOADER_ARM_FLOAT_ABI_UNSPECIFIED: return "unspecified";
  case NXLOADER_ARM_FLOAT_ABI_SOFT: return "soft";
  case NXLOADER_ARM_FLOAT_ABI_HARD: return "hard";
  default: return "invalid";
  }
}

const char *nxloader_state_string(nxloader_state state) {
  switch (state) {
  case NXLOADER_STATE_EMPTY: return "empty";
  case NXLOADER_STATE_LOADED: return "loaded";
  case NXLOADER_STATE_RELOCATED: return "relocated";
  case NXLOADER_STATE_RESOLVED: return "resolved";
  case NXLOADER_STATE_FINALIZED: return "finalized";
  case NXLOADER_STATE_INITIALIZED: return "initialized";
  case NXLOADER_STATE_INITIALIZING: return "initializing";
  case NXLOADER_STATE_JNI_LOADING: return "jni-loading";
  case NXLOADER_STATE_READY: return "ready";
  case NXLOADER_STATE_ERROR: default: return "error";
  }
}
