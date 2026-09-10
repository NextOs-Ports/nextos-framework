/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_NXCOMPAT_H
#define NXINPUT_NXCOMPAT_H

#include "nxcompat.h"
#include "nxinput.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Strong bridge for a live opaque nxinput context. The context type proves
 * that controller subsystems, inherited mapping application, initial scan and
 * the non-draining event watch completed in nxinput_create(). The bridge
 * cross-checks all four public slots and publishes only aggregate counts and
 * generations; controller names, GUIDs and instance IDs never leave nxinput. */
nxcompat_result_code nxinput_nxcompat_publish_context(
    nxcompat_registry *registry, const nxinput_context *input,
    nxcompat_input_receipt *published_receipt);

#ifdef __cplusplus
}
#endif

#endif /* NXINPUT_NXCOMPAT_H */
