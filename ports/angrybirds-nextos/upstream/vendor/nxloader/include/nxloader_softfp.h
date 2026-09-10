/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NXLOADER_SOFTFP_H
#define NXLOADER_SOFTFP_H

#include "nxloader.h"

#if defined(__arm__) && (defined(__GNUC__) || defined(__clang__))
#define NXLOADER_ARM_SOFTFP __attribute__((pcs("aapcs")))
#else
#define NXLOADER_ARM_SOFTFP
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Optional ARMv7-only provider for the proven softfp -> hardfp libm boundary.
 * GLES functions are intentionally engine/driver adapter concerns and must be
 * registered explicitly by the port. */
nxloader_result nxloader_softfp_add_libm(nxloader_registry *registry,
                                        const char *provider_name,
                                        int priority,
                                        nxloader_registry_report *report);

#ifdef __cplusplus
}
#endif

#endif /* NXLOADER_SOFTFP_H */
