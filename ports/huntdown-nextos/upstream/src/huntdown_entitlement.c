#define _GNU_SOURCE
#include "huntdown_entitlement.h"
#include "huntdown_build.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Huntdown 0.1.23 stores the owner's full-game entitlement through Unity IAP
 * and then exposes it to every menu through LocalStorage.IsPayed.  The native
 * Linux host cannot restore the Android store session, even though NXExtract
 * has validated the complete owner-supplied payload.  Bridge only that read
 * abstraction, and only when the exact pinned libil2cpp method is present. */
static const unsigned char hd_is_payed_signature_200023[16] = {
    0xfe, 0x57, 0xbe, 0xa9, 0xf4, 0x4f, 0x01, 0xa9,
    0x94, 0xd1, 0x00, 0xb0, 0xb5, 0xc1, 0x00, 0x90,
};

static const unsigned char hd_is_payed_signature_200036[16] = {
    0xfe, 0x57, 0xbe, 0xa9, 0xf4, 0x4f, 0x01, 0xa9,
    0xf4, 0x0d, 0x01, 0xf0, 0x75, 0xfa, 0x00, 0xf0,
};

static int hd_is_payed(void *self, void *method_info) {
  (void)self;
  (void)method_info;
  return 1;
}

int hd_entitlement_install(uintptr_t il2cpp_base) {
  static int installed;
  if (installed) return 1;
  if (!il2cpp_base) return 0;

  uintptr_t rva = 0;
  const unsigned char *signature = NULL;
  if (hd_build_current() == HD_BUILD_200023) {
    rva = 0x1515A40u;
    signature = hd_is_payed_signature_200023;
  } else if (hd_build_current() == HD_BUILD_200036) {
    rva = 0x1AD0F28u;
    signature = hd_is_payed_signature_200036;
  }
  if (!rva || !signature) return 0;
  uintptr_t address = il2cpp_base + rva;
  if (memcmp((const void *)address, signature, 16) != 0) {
    fprintf(stderr,
            "[HDENTITLEMENT] assinatura divergente em LocalStorage.IsPayed; "
            "bridge recusado\n");
    return 0;
  }

  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) return 0;
  uintptr_t page = address & ~((uintptr_t)page_size - 1u);
  if (mprotect((void *)page, (size_t)page_size,
               PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    fprintf(stderr, "[HDENTITLEMENT] mprotect: %s\n", strerror(errno));
    return 0;
  }

  uint32_t *code = (uint32_t *)address;
  code[0] = 0x58000050u; /* ldr x16, [pc, #8] */
  code[1] = 0xd61f0200u; /* br x16 */
  *(uint64_t *)(code + 2) = (uint64_t)(uintptr_t)hd_is_payed;
  __builtin___clear_cache((char *)address, (char *)address + 16);

  if (mprotect((void *)page, (size_t)page_size,
               PROT_READ | PROT_EXEC) != 0)
    fprintf(stderr, "[HDENTITLEMENT] aviso: não restaurou RX: %s\n",
            strerror(errno));

  installed = 1;
  fprintf(stderr,
          "[HDENTITLEMENT] cópia completa validada; restauração offline ativa "
          "(Huntdown %s)\n", hd_build_version_name());
  return 1;
}
