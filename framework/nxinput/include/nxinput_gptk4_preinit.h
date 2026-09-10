/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_GPTK4_PREINIT_H
#define NXINPUT_GPTK4_PREINIT_H
/*
 * nxinput_gptk4_preinit -- nxinput 0.11.1 (V5-M1a item 1): the UNIVERSAL
 * schema-4 pre-init boundary.
 *
 * Until 0.11.0 every port carried its own copy of the same sixty lines
 * (FP2, Nameless Cat, Blossom Tales in C; Tearscape in the Godot driver):
 * open the game directory and its `defaults/` as REAL directories
 * (O_NOFOLLOW, O_DIRECTORY), call nxinput_gptk4_load_project_at(), close the
 * fds, format the bridge receipt as JSON, project the schema-4 selection
 * onto the V3 `nxinput_gptk_preinit_result` the live runtime consumes, and
 * print one log line. Four copies = four places to fix. This is the one.
 *
 * Contract (identical to what the ports did, now provable once):
 *   - `gamedir` NULL/"" falls back to "." exactly like the ports did; the
 *     result records `gamedir_fallback_cwd` so a receipt can say so;
 *   - both directories are opened O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC:
 *     a symlinked game directory or a symlinked `defaults/` is NOT followed
 *     (the loader's own O_NOFOLLOW on the files still applies below);
 *   - default first, then the owner's editable copy; a rejected owner keeps
 *     the default and the receipt names line/column/reason; the owner's bytes
 *     are never rewritten (that is nxinput_gptk4_load_project_at());
 *   - the V3 projection (`v3.receipt.source`, owner/default error codes,
 *     selected schema = 4, selected sha256, face_layout = AUTO because the
 *     Xbox surface is positional) is exactly the one the 0.11.0 ports
 *     computed by hand, so a port that switches to this call changes no
 *     receipt byte;
 *   - `receipt_json` is the bounded path-free JSON of the bridge receipt and
 *     `log_line` the one human line (loaded: source/layout/sha prefix;
 *     not loaded: the NXI#### code with line:column and reason). The port
 *     prefixes them with its own tag and prints/records them; nothing else.
 *
 * Returns 0 when the boundary ran (loaded or not: the port stays native
 * when `v3.loaded == 0`), -1 only on structurally invalid arguments
 * (NULL contract/out). Never mutates the filesystem. Call it ONCE, before
 * any SDL subsystem, and keep the result for the whole run.
 */
#include "nxinput_gptk4.h"
#include "nxinput_gptk4_bridge.h"
#include "nxinput_gptk_preinit.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_GPTK4_PREINIT_API_VERSION 1u
#define NXINPUT_GPTK4_PREINIT_JSON_MAX 1024u
#define NXINPUT_GPTK4_PREINIT_LOG_MAX 256u
#define NXINPUT_GPTK4_OWNER_BASENAME "NEXTOSCONTROLLERS.gptk"
#define NXINPUT_GPTK4_DEFAULTS_DIRNAME "defaults"

typedef struct nxinput_gptk4_preinit_result {
  uint32_t api_version;
  size_t struct_size;
  /* What the live runtime and the receipts read: loaded/rc/face_layout,
   * the projected V3 map and the V3 load receipt (source, error codes,
   * schema 4, sha256). */
  nxinput_gptk_preinit_result v3;
  /* The schema-4 owner as parsed (registry, prompts, authority, sticks). */
  nxinput_gptk4 map4;
  /* The bridge selection receipt (default/owner, rejection, sha256...). */
  nxinput_gptk4_bridge_receipt bridge;
  /* Evidence about the directories: 1 when opened as real directories. */
  uint8_t gamedir_opened;
  uint8_t defaults_opened;
  uint8_t gamedir_fallback_cwd; /* gamedir was NULL/"" -> "." was used */
  char receipt_json[NXINPUT_GPTK4_PREINIT_JSON_MAX];
  char log_line[NXINPUT_GPTK4_PREINIT_LOG_MAX];
} nxinput_gptk4_preinit_result;

int nxinput_gptk4_preinit_load(const char *gamedir,
                               const nxinput_gptk4_contract *contract,
                               nxinput_gptk4_preinit_result *out);

/* The V3 load source the bridge selection projects to (the same three-way
 * mapping every 0.11.0 port wrote by hand). Exposed so a test can pin it. */
nxinput_gptk_load_source nxinput_gptk4_preinit_v3_source(
    const nxinput_gptk4_bridge_receipt *bridge);

#ifdef __cplusplus
}
#endif

#endif /* NXINPUT_GPTK4_PREINIT_H */
