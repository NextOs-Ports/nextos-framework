/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXGL_NXCOMPAT_H
#define NXGL_NXCOMPAT_H

#include "nxcompat.h"
#include "nxgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Converts the preflight display fact already collected by nxcompat. nxgl
 * still asks SDL first and uses this only as DRM/fbdev fallback. */
int nxgl_nxcompat_resolution_sources(const nxcompat_host *host,
                                     nxgl_resolution_sources *sources);

/* Legacy diagnostic conversion only. nxgl_report is intentionally free-form;
 * this function NEVER publishes evidence and cannot satisfy a requirement. */
int nxgl_nxcompat_capture_report(const nxgl_report *report,
                                 nxcompat_graphics *graphics,
                                 nxcompat_status_callback status,
                                 void *status_userdata);

/* Strong bridge for a live opaque nxgl context. It verifies make-current and
 * identity, requeries GL strings/config/drawable, queries the current EGL
 * display/context/config through SDL's own proc-address stack, and only then
 * transactionally publishes a graphics receipt. It never creates a window,
 * opens a context, swaps, changes GL state, or chooses a backend. */
nxcompat_result_code nxgl_nxcompat_publish_context(
    nxcompat_registry *registry, nxgl_context *context, uint64_t generation,
    nxcompat_graphics_receipt *published_receipt);

#ifdef __cplusplus
}
#endif

#endif /* NXGL_NXCOMPAT_H */
