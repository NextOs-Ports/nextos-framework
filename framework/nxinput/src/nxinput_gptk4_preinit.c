/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_gptk4_preinit -- see include/nxinput_gptk4_preinit.h. */
#define _POSIX_C_SOURCE 200809L

#include "nxinput_gptk4_preinit.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

nxinput_gptk_load_source nxinput_gptk4_preinit_v3_source(
    const nxinput_gptk4_bridge_receipt *bridge) {
  if (bridge == 0) {
    return NXINPUT_GPTK_LOAD_NONE;
  }
  if (bridge->source == NXINPUT_GPTK4_SRC_OWNER) {
    return NXINPUT_GPTK_LOAD_OWNER;
  }
  if (bridge->source == NXINPUT_GPTK4_SRC_DEFAULT) {
    return bridge->owner_rejected ? NXINPUT_GPTK_LOAD_DEFAULT_OWNER_REJECTED
                                  : NXINPUT_GPTK_LOAD_DEFAULT_OWNER_MISSING;
  }
  return NXINPUT_GPTK_LOAD_NONE;
}

int nxinput_gptk4_preinit_load(const char *gamedir,
                               const nxinput_gptk4_contract *contract,
                               nxinput_gptk4_preinit_result *out) {
  const char *dir;
  int owner_fd;
  int defaults_fd;
  int rc;

  if (out == 0) {
    return -1;
  }
  memset(out, 0, sizeof *out);
  out->api_version = NXINPUT_GPTK4_PREINIT_API_VERSION;
  out->struct_size = sizeof *out;
  out->v3.api_version = NXINPUT_GPTK_PREINIT_API_VERSION;
  out->v3.struct_size = sizeof out->v3;
  out->v3.receipt.api_version = NXINPUT_GPTK_LOAD_API_VERSION;
  /* Positional Xbox surface: the plastic legend never selects anything. */
  out->v3.face_layout = (uint8_t)NXINPUT_GPTK_FACE_LAYOUT_AUTO;
  if (contract == 0) {
    return -1;
  }
  if (gamedir == 0 || gamedir[0] == '\0') {
    dir = ".";
    out->gamedir_fallback_cwd = 1;
  } else {
    dir = gamedir;
  }
  /* Real directories only, links refused: the same discipline the loader
   * applies to the files themselves. A failed open is NOT an error of the
   * boundary: the load below simply finds nothing and the port stays native
   * with the receipt saying why. */
  owner_fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  out->gamedir_opened = owner_fd >= 0 ? 1u : 0u;
  defaults_fd = owner_fd >= 0
      ? openat(owner_fd, NXINPUT_GPTK4_DEFAULTS_DIRNAME,
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
      : -1;
  out->defaults_opened = defaults_fd >= 0 ? 1u : 0u;
  rc = nxinput_gptk4_load_project_at(owner_fd, defaults_fd, contract,
                                     &out->map4, &out->v3.map, &out->bridge);
  if (defaults_fd >= 0) {
    (void)close(defaults_fd);
  }
  if (owner_fd >= 0) {
    (void)close(owner_fd);
  }
  if (nxinput_gptk4_bridge_receipt_json(&out->bridge, out->receipt_json,
                                        sizeof out->receipt_json) != 0) {
    out->receipt_json[0] = '\0';
  }
  /* The V3 projection, byte for byte what the 0.11.0 ports computed. */
  out->v3.loaded = rc == 0;
  out->v3.rc = out->bridge.rc;
  out->v3.receipt.source = (uint8_t)nxinput_gptk4_preinit_v3_source(&out->bridge);
  out->v3.receipt.owner_present = out->bridge.owner_present;
  out->v3.receipt.owner_error_code = out->bridge.owner_rejected ? out->bridge.rc : 0;
  out->v3.receipt.default_error_code = out->bridge.default_rejected ? out->bridge.rc : 0;
  out->v3.receipt.selected_gptk_schema = NXINPUT_GPTK_SCHEMA_V4;
  (void)snprintf(out->v3.receipt.selected_sha256,
                 sizeof out->v3.receipt.selected_sha256, "%s",
                 out->bridge.sha256);
  if (out->v3.loaded) {
    (void)snprintf(out->log_line, sizeof out->log_line,
                   "preinit: NEXTOS_CONTROLLERS/%u source=%s layout=%s "
                   "sha256=%.16s...",
                   (unsigned)out->v3.map.schema_version,
                   nxinput_gptk_load_source_name(
                       (nxinput_gptk_load_source)out->v3.receipt.source),
                   nxinput_gptk_face_layout_name((int)out->v3.face_layout),
                   out->v3.receipt.selected_sha256);
  } else {
    (void)snprintf(out->log_line, sizeof out->log_line,
                   "NXI%04d: no valid %s (%s:%u:%u %s) -- controls stay native",
                   out->bridge.rc, NXINPUT_GPTK4_OWNER_BASENAME,
                   NXINPUT_GPTK4_OWNER_BASENAME, out->bridge.line,
                   out->bridge.column, out->bridge.what);
  }
  return 0;
}
