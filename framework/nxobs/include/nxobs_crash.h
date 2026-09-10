/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXOBS_CRASH_H
#define NXOBS_CRASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXOBS_CRASH_API_VERSION 1

/*
 * Crash receipts are opt-in and additive.  Install only after the adapter has
 * selected a private runtime directory.  The handler observes the native
 * lifecycle and re-raises the original signal; it never invents an exit path.
 */
struct nxobs_crash_config {
    const char *runtime_dir;
    const char *port_id;
    const char *initial_phase;
    const char *initial_provider;
};

int nxobs_crash_install(const struct nxobs_crash_config *config);
void nxobs_crash_uninstall(void);

/* Onda v2: re-enumera os modulos carregados (dl_iterate_phdr). Chamar apos
 * dlopen/so_load do provedor ou da engine, do caminho normal (nunca de um
 * handler), para o recibo de crash saber nomear module/build_id/offset. */
void nxobs_crash_refresh_modules(void);

void nxobs_crash_set_phase(const char *phase);
void nxobs_crash_set_frame(uint64_t frame);
void nxobs_crash_set_last_asset(const char *asset_id);
void nxobs_crash_set_last_graphics_call(const char *call_id);
void nxobs_crash_set_provider(const char *provider_id);

const char *nxobs_crash_receipt_path(void);
const char *nxobs_crash_maps_path(void);

#ifdef __cplusplus
}
#endif

#endif
