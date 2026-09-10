/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_GPTK_PREINIT_H
#define NXINPUT_GPTK_PREINIT_H

/*
 * nxinput_gptk_preinit -- nxinput 0.10.0: the narrow pre-init boundary.
 *
 * The GPTK owner/default map -- and with it the V3 FACE_LAYOUT -- must be
 * read EXACTLY ONCE, before the port declares its bundle, stages the live
 * mapping and calls SDL_Init. Loading it lazily after SDL was up meant the
 * layout could not select which bundle variant became authority 3, and a
 * TOCTOU re-read could decide something different later.
 *
 * The engine glue calls nxinput_gptk_preinit_load() at the very top of its
 * joystick initialisation (Tearscape: JoypadSDL::initialize()), keeps the
 * result in memory for the whole run, hands `face_layout` to
 * nxc6_declare_port_bundle_for_layout() and reuses the SAME map/receipt for
 * the live runtime. Nothing re-reads the file per frame or per admission.
 *
 * The directories are opened as REAL directories with O_NOFOLLOW; the load
 * itself is the canonical bounded nxinput_gptk_load_at().
 */

#include "nxinput_gptk.h"
#include "nxinput_gptk_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_GPTK_PREINIT_API_VERSION 1u

typedef struct nxinput_gptk_preinit_result {
  uint32_t api_version;
  size_t struct_size;
  /* 1 when `map` holds a valid owner/default map; 0 when the port stays
   * native (missing directories, no valid mapping...). */
  int loaded;
  int rc; /* 0, or the NXI#### code of the failed load */
  /* The FACE_LAYOUT this run will use: the parsed map's value when loaded,
   * AUTO otherwise. Valid in BOTH cases so the declare boundary always has
   * an answer. */
  uint8_t face_layout;
  nxinput_gptk map;
  nxinput_gptk_load_receipt receipt;
} nxinput_gptk_preinit_result;

/* Load once, before any SDL subsystem. `gamedir` is the game directory that
 * holds the owner's NEXTOSCONTROLLERS.gptk and the immutable default under
 * defaults/. Returns 0 when the boundary ran (even when the port stays
 * native), -1 only on structurally invalid arguments. */
int nxinput_gptk_preinit_load(const char *gamedir,
                              const char *const *allowed_actions,
                              size_t allowed_count,
                              nxinput_gptk_preinit_result *out);

#ifdef __cplusplus
}
#endif

#endif /* NXINPUT_GPTK_PREINIT_H */
