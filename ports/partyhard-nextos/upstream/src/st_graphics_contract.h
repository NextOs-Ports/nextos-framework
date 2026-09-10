/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ST_GRAPHICS_CONTRACT_H
#define ST_GRAPHICS_CONTRACT_H

#include <stdint.h>

/* Arm the canonical nxgl resolver before the normal Unity/Android lifecycle. */
void st_graphics_contract_prepare(void);

/* Bind the SDL window whose live drawable the canonical adapter measures. */
void st_graphics_contract_set_sdl_window(void *window);

/* Phase one: on the actual guest context, measure GLES/profile/version and
 * compile+link an ESSL100 probe.  No health receipt exists at this point. */
int st_graphics_contract_pre_present(uintptr_t context_token);

/* Phase two: call only after the real SDL present returned.  It emits the
 * canonical receipt once the post-present drawable is proven usable. */
int st_graphics_contract_after_present(uintptr_t context_token);

#endif
