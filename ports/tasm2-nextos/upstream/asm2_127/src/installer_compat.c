#include "installer_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "so_util.h"

/*
 * The Android 1.2.7d installer runs before GL2JNIActivity.  Its successful
 * nativeStart path leaves four one-element counters allocated for the game
 * runtime.  A direct native launcher has no Java installer activity, so
 * reproduce that postcondition before entering the activity lifecycle.
 *
 * These VMAs are guarded by their relocated GOT entries.  A different game
 * binary fails closed instead of receiving writes at unverified offsets.
 */
struct installer_slot {
  uintptr_t storage_vma;
  uintptr_t got_vma;
};

static const struct installer_slot installer_slots[] = {
#if defined(__i386__)
    {0x01F15DF0u, 0x01E928F0u},
    {0x01F15DF4u, 0x01E928D0u},
    {0x01F15DF8u, 0x01E928F4u},
    {0x01F15DFCu, 0x01E928D4u},
#else
    {0x013B7B30u, 0x0133A8BCu},
    {0x013B7B34u, 0x0133A8B4u},
    {0x013B7B38u, 0x0133A8C0u},
    {0x013B7B3Cu, 0x0133A8B8u},
#endif
};

int asm2_installer_compat_initialize(void) {
  uint32_t **storage[sizeof(installer_slots) / sizeof(installer_slots[0])];
  uint32_t *values[sizeof(installer_slots) / sizeof(installer_slots[0])] = {0};
  size_t initialized = 0;

  for (size_t index = 0;
       index < sizeof(installer_slots) / sizeof(installer_slots[0]); ++index) {
    storage[index] =
        so_guest_vma(installer_slots[index].storage_vma, sizeof(*storage[index]));
    uintptr_t *got =
        so_guest_vma(installer_slots[index].got_vma, sizeof(*got));
    if (!storage[index] || !got ||
        *got != (uintptr_t)storage[index]) {
      fprintf(stderr,
              "ASM2_INSTALLER_COMPAT_ERROR layout index=%zu storage=%p "
              "got=%p relocated=%p\n",
              index, (void *)storage[index], (void *)got,
              got ? (void *)*got : NULL);
      return -1;
    }
    if (*storage[index] != NULL)
      ++initialized;
  }

  if (initialized != 0) {
    if (initialized == sizeof(installer_slots) / sizeof(installer_slots[0])) {
      fprintf(stderr, "ASM2_INSTALLER_COMPAT_OK state=already-initialized\n");
      return 0;
    }
    fprintf(stderr,
            "ASM2_INSTALLER_COMPAT_ERROR partial-state initialized=%zu\n",
            initialized);
    return -1;
  }

  for (size_t index = 0;
       index < sizeof(installer_slots) / sizeof(installer_slots[0]); ++index) {
    values[index] = malloc(sizeof(*values[index]));
    if (!values[index]) {
      for (size_t cleanup = 0; cleanup < index; ++cleanup)
        free(values[cleanup]);
      fprintf(stderr, "ASM2_INSTALLER_COMPAT_ERROR allocation index=%zu\n",
              index);
      return -1;
    }
    *values[index] = 1;
  }

  for (size_t index = 0;
       index < sizeof(installer_slots) / sizeof(installer_slots[0]); ++index)
    *storage[index] = values[index];

  fprintf(stderr,
          "ASM2_INSTALLER_COMPAT_OK version=1.2.7d counters=1/1 "
          "workers=1/1\n");
  return 0;
}
