/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Titan Souls adapter import table and its narrow guest-image compatibility. */

#ifndef TITANSOULS_GUEST_IMPORT_H
#define TITANSOULS_GUEST_IMPORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ts_guest_import {
  char *symbol;
  uintptr_t func;
} DynLibFunction;

/* The OpenSL callback discriminator and ARM EHABI shim consume only this
 * immutable view of the game module mapped by nxloader. */
extern void *text_base;
extern size_t text_size;
uintptr_t so_find_exidx(uintptr_t program_counter, int *count);

#ifdef __cplusplus
}
#endif

#endif /* TITANSOULS_GUEST_IMPORT_H */
