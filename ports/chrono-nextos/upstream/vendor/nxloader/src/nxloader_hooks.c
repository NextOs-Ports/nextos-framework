/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nxloader_internal.h"

#include <limits.h>
#include <string.h>

static nxloader_result nxloader_install_aarch64_hook(nxloader_module *module,
                                                     uintptr_t target,
                                                     uintptr_t destination,
                                                     size_t available_bytes) {
  uint8_t *slot;
  uintptr_t slot_address;
  int64_t displacement;
  uint32_t branch;
  if (available_bytes < 4 || (target & 3u) != 0 ||
      (destination & 3u) != 0)
    return NXLOADER_EINVAL;
  if (!module->trampoline_pool ||
      module->trampoline_pool_used > module->trampoline_pool_size ||
      module->trampoline_pool_size - module->trampoline_pool_used < 16)
    return NXLOADER_EBOUNDS;
  slot = module->trampoline_pool + module->trampoline_pool_used;
  slot_address = (uintptr_t)slot;
  if ((slot_address & 3u) != 0)
    return NXLOADER_EOVERFLOW;
  if (slot_address >= target) {
    uintptr_t distance = slot_address - target;
    if (distance > (uintptr_t)INT64_MAX)
      return NXLOADER_EOVERFLOW;
    displacement = (int64_t)distance;
  } else {
    uintptr_t distance = target - slot_address;
    if (distance > (uintptr_t)INT64_MAX)
      return NXLOADER_EOVERFLOW;
    displacement = -(int64_t)distance;
  }
  if ((displacement & 3) != 0 || displacement < -(INT64_C(1) << 27) ||
      displacement > (INT64_C(1) << 27) - 4)
    return NXLOADER_EOVERFLOW;
  /* LDR X17, literal +8; BR X17; 64-bit destination. */
  nxloader_write_u32(slot + 0, UINT32_C(0x58000051));
  nxloader_write_u32(slot + 4, UINT32_C(0xd61f0220));
  nxloader_write_u64(slot + 8, (uint64_t)destination);
  branch = UINT32_C(0x14000000) |
           ((uint32_t)((uint64_t)(displacement >> 2)) & UINT32_C(0x03ffffff));
  nxloader_write_u32((void *)target, branch);
  module->trampoline_pool_used += 16;
  return NXLOADER_OK;
}

static nxloader_result nxloader_install_armv7_hook(nxloader_module *module,
                                                   uintptr_t target,
                                                   uintptr_t destination,
                                                   size_t available_bytes) {
  uintptr_t clean_target = target & ~(uintptr_t)1;
  (void)module;
  if (available_bytes < 8 || destination > UINT32_MAX)
    return available_bytes < 8 ? NXLOADER_EINVAL : NXLOADER_EOVERFLOW;
  if (!(target & 1u) && (clean_target & 3u))
    return NXLOADER_EINVAL;
  if (target & 1u) {
    /* Thumb-2 LDR.W PC literal, followed by the absolute destination. */
    uint16_t first = UINT16_C(0xf8df);
    /* Thumb PC is Align(address + 4, 4). A 2-mod-4 entry therefore needs an
     * immediate of 2 to reach the literal at entry+4. */
    uint16_t second = (uint16_t)(UINT16_C(0xf000) |
                                 (clean_target & 2u ? 2u : 0u));
    memcpy((void *)clean_target, &first, sizeof(first));
    memcpy((void *)(clean_target + 2), &second, sizeof(second));
    nxloader_write_u32((void *)(clean_target + 4), (uint32_t)destination);
  } else {
    /* ARM: LDR PC,[PC,#-4], followed by the absolute destination. */
    nxloader_write_u32((void *)clean_target, UINT32_C(0xe51ff004));
    nxloader_write_u32((void *)(clean_target + 4), (uint32_t)destination);
  }
  return NXLOADER_OK;
}

nxloader_result nxloader_module_install_hook(nxloader_module *module,
                                             uintptr_t target,
                                             uintptr_t destination,
                                             size_t available_bytes) {
  if (!module)
    return NXLOADER_EINVAL;
  if (nxloader_module_callback_guard(module) != NXLOADER_OK)
    return NXLOADER_EREENTRANT;
  if (!target || !destination)
    return NXLOADER_EINVAL;
  if (module->state != NXLOADER_STATE_RESOLVED)
    return NXLOADER_ESTATE;
  if (!nxloader_pointer_is_executable(module, target, available_bytes))
    return NXLOADER_EBOUNDS;
  if (module->arch == NXLOADER_ARCH_AARCH64)
    return nxloader_install_aarch64_hook(module, target, destination,
                                         available_bytes);
  if (module->arch == NXLOADER_ARCH_ARMV7)
    return nxloader_install_armv7_hook(module, target, destination,
                                       available_bytes);
  return NXLOADER_EARCH;
}
