/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TITANSOULS_LOADER_COMPAT_H
#define TITANSOULS_LOADER_COMPAT_H

#include "ts_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bind the two immutable nxloader mappings to the narrow legacy ABI still
 * consumed by platform_shims.c and opensles_shim.c.  This does not load,
 * relocate, resolve, patch or change permissions on a guest image. */
int ts_loader_compat_bind(ts_loader *loader);
void ts_loader_compat_unbind(void);

#ifdef __cplusplus
}
#endif

#endif /* TITANSOULS_LOADER_COMPAT_H */
