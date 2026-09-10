/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef TITANSOULS_RUNTIME_HOOKS_H
#define TITANSOULS_RUNTIME_HOOKS_H

#include "nxgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* To be called exactly once by egl_shim after the guest-created GLES context
 * is current and nxgl-v2 has produced its authoritative report.  Until EGL
 * calls this boundary, ts_lifecycle deliberately remains fail-closed. */
int ts_runtime_graphics_ready(const nxgl_report_v2 *report);

#ifdef __cplusplus
}
#endif

#endif /* TITANSOULS_RUNTIME_HOOKS_H */
