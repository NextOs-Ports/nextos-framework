/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef TITANSOULS_LANGUAGE_MENU_H
#define TITANSOULS_LANGUAGE_MENU_H

#include <stddef.h>
#include <stdint.h>

#include "ts_loader.h"

/* Read the game's own persisted choice before libTestSuite starts. */
int ts_language_menu_prepare(const char *gamedir);

/* Install the three exact-guest hooks while nxloader is still PREPARED. */
int ts_language_menu_install(ts_loader *loader, uintptr_t mapping_base,
                             size_t mapping_size);

#endif /* TITANSOULS_LANGUAGE_MENU_H */
