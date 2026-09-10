/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <sys/mman.h>

#include "nxloader_internal.h"

#include <limits.h>
#include <string.h>

/* Um `B` do AArch64 alcanca +-128 MiB. O pool de trampolins fica no fim da
 * imagem, entao num modulo grande — o guest medido aqui tem 330 MiB so' de
 * texto — qualquer alvo nos primeiros ~200 MiB fica FORA de alcance e o hook
 * falha com EOVERFLOW. Na pratica isso tornava so' o ultimo pedaco do modulo
 * observavel, o que nao e' um limite defensavel: e' um acidente de onde o pool
 * calhou de ficar.
 *
 * Aqui o slot passa a ser escolhido POR ALCANCE. O pool principal e' tentado
 * primeiro, para nao mudar nada no caso comum; se ele nao alcanca, procuramos um
 * pool auxiliar ja' criado que alcance; e so' entao pedimos uma pagina nova
 * perto do alvo. As dicas de mmap caem FORA da imagem (logo abaixo da base ou
 * logo acima do topo), porque pedir espaco no meio de uma regiao ja' mapeada faz
 * o kernel devolver outro lugar qualquer.
 *
 * Falhar continua sendo uma opcao explicita: sem pagina ao alcance, devolvemos
 * NULL e o chamador recusa o hook, em vez de emitir um desvio que nao chega. */
static int nxloader_slot_reaches(uintptr_t slot_address, uintptr_t target) {
  int64_t displacement;
  if (slot_address >= target) {
    uintptr_t distance = slot_address - target;
    if (distance > (uintptr_t)INT64_MAX)
      return 0;
    displacement = (int64_t)distance;
  } else {
    uintptr_t distance = target - slot_address;
    if (distance > (uintptr_t)INT64_MAX)
      return 0;
    displacement = -(int64_t)distance;
  }
  return (displacement & 3) == 0 && displacement >= -(INT64_C(1) << 27) &&
         displacement <= (INT64_C(1) << 27) - 4;
}

static uint8_t *nxloader_reserve_trampoline_slot(nxloader_module *module,
                                                 uintptr_t target,
                                                 nxloader_result *reason) {
  /* Os dois motivos de recusa sao contratos distintos e continuam distintos:
   * pool sem espaco e' EBOUNDS, pool fora de alcance e' EOVERFLOW. */
  *reason = NXLOADER_EBOUNDS;
  if (module->trampoline_pool &&
      module->trampoline_pool_used <= module->trampoline_pool_size &&
      module->trampoline_pool_size - module->trampoline_pool_used >= 16) {
    uint8_t *slot = module->trampoline_pool + module->trampoline_pool_used;
    if (nxloader_slot_reaches((uintptr_t)slot, target)) {
      module->trampoline_pool_used += 16;
      return slot;
    }
    *reason = NXLOADER_EOVERFLOW;
  }
  if (!(module->config.flags & NXLOADER_CONFIG_FAR_HOOKS))
    return NULL;
  for (size_t i = 0; i < module->aux_pool_count; ++i) {
    if (module->aux_pool[i].size - module->aux_pool[i].used < 16)
      continue;
    uint8_t *slot = module->aux_pool[i].base + module->aux_pool[i].used;
    if (nxloader_slot_reaches((uintptr_t)slot, target)) {
      /* Reabre para escrita so' enquanto o slot e' preenchido; volta a R+X em
       * nxloader_finish_trampoline_slot. Nenhuma pagina fica gravavel e
       * executavel ao mesmo tempo. */
      if (mprotect(module->aux_pool[i].base, module->aux_pool[i].size,
                   PROT_READ | PROT_WRITE) != 0)
        return NULL;
      module->aux_pool[i].used += 16;
      return slot;
    }
  }
  if (module->aux_pool_count >= NXLOADER_AUX_POOL_MAX || !module->mapping)
    return NULL;
  size_t page = module->page_size ? module->page_size : 4096;
  uintptr_t base = (uintptr_t)module->mapping;
  uintptr_t end = base + module->mapping_size;
  static const size_t k_mib[] = {1, 2, 4, 8, 16, 32, 64};
  for (size_t h = 0; h < 2 * (sizeof(k_mib) / sizeof(k_mib[0])); ++h) {
    size_t delta = k_mib[h / 2] * 1024u * 1024u;
    uintptr_t hint;
    if (h % 2 == 0) {
      if (base < delta + page)
        continue;
      hint = (base - delta - page) & ~(uintptr_t)(page - 1);
    } else {
      hint = (end + delta) & ~(uintptr_t)(page - 1);
    }
    void *p = mmap((void *)hint, page, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
      continue;
    if (!nxloader_slot_reaches((uintptr_t)p, target)) {
      (void)munmap(p, page);
      continue;
    }
    size_t index = module->aux_pool_count++;
    module->aux_pool[index].base = (uint8_t *)p;
    module->aux_pool[index].size = page;
    module->aux_pool[index].used = 16;
    return (uint8_t *)p;
  }
  return NULL;
}

/* Fecha o pool auxiliar que contem `slot` de volta em R+X e sincroniza o cache
 * de instrucoes. O pool principal vive dentro da imagem e ja' e' tratado pelo
 * caminho normal de protecao do modulo. */
static void nxloader_finish_trampoline_slot(nxloader_module *module,
                                            uint8_t *slot) {
  for (size_t i = 0; i < module->aux_pool_count; ++i) {
    uint8_t *base = module->aux_pool[i].base;
    if (slot < base || slot >= base + module->aux_pool[i].size)
      continue;
    __builtin___clear_cache((char *)slot, (char *)slot + 16);
    (void)mprotect(base, module->aux_pool[i].size, PROT_READ | PROT_EXEC);
    return;
  }
}

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
  {
    nxloader_result reason = NXLOADER_EBOUNDS;
    slot = nxloader_reserve_trampoline_slot(module, target, &reason);
    if (!slot)
      return reason;
  }
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
  nxloader_finish_trampoline_slot(module, slot);
  nxloader_write_u32((void *)target, branch);
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
