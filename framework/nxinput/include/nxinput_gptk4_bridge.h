/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_GPTK4_BRIDGE_H
#define NXINPUT_GPTK4_BRIDGE_H
/*
 * nxinput_gptk4_bridge -- V5 (0.11.0): load a NEXTOS_CONTROLLERS/4 owner
 * (default first, then the owner's editable copy, exactly like the V3
 * loader) and PROJECT it onto the live V3 dispatch structure
 * (`nxinput_gptk`) that the shipped ports already drive through
 * `nxinput_gptk_live`. The unified base map becomes every V3 context;
 * sparse `[override.<ctx>]` layers apply only where declared
 * (`menu`, `cursor`; `gameplay` when a port really declares it).
 *
 *   base + override.menu    -> NXINPUT_GPTK_CONTEXT_MENU
 *   base (+override.gameplay) -> NXINPUT_GPTK_CONTEXT_GAMEPLAY
 *   base + override.cursor  -> NXINPUT_GPTK_CONTEXT_CURSOR
 *
 * Triggers: mode analog -> the analog binding on L2/R2; mode digital -> the
 * digital binding. Sticks: mode vector -> LEFT_STICK/RIGHT_STICK vector
 * binding; digital/split modes have no V3 sink and are reported
 * (`stick_digital_unsupported`), never silently dropped. GUIDE has no V3
 * control: reported (`guide_dropped`); the sovereign chord ignores it
 * anyway. `@key` outputs are counted (`key_outputs`) for the router.
 *
 * The owner's bytes are never rewritten; a rejected owner keeps the default
 * (receipt names line/column/reason). The live marker of a port built on
 * this bridge is NXINPUT_GPTK4_RUNTIME_MARKER.
 */
#include "nxinput_gptk.h"
#include "nxinput_gptk4.h"
#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_GPTK4_RUNTIME_MARKER "nxinput-gptk-runtime/4"
#define NXINPUT_GPTK_SCHEMA_V4 4u

typedef enum nxinput_gptk4_bridge_source {
  NXINPUT_GPTK4_SRC_NONE = 0, NXINPUT_GPTK4_SRC_DEFAULT, NXINPUT_GPTK4_SRC_OWNER
} nxinput_gptk4_bridge_source;

typedef struct nxinput_gptk4_bridge_receipt {
  uint8_t source;             /* nxinput_gptk4_bridge_source */
  uint8_t owner_present, owner_rejected, default_rejected;
  int rc;                     /* 0 or the NXI4xxx code of the last failure */
  unsigned line, column; char what[96];
  uint64_t digest;            /* fnv1a64 of the accepted bytes */
  unsigned guide_dropped, stick_digital_unsupported, key_outputs, extensions;
  char sha256[65];            /* sha256 hex of the accepted bytes ("" when none) */
} nxinput_gptk4_bridge_receipt;

/* Pure projection. Returns 0, or -1 on NULL/unsupported (see receipt). */
int nxinput_gptk4_project(const nxinput_gptk4 *g4, nxinput_gptk *out,
                          nxinput_gptk4_bridge_receipt *r);

/* Load default then owner (basename NEXTOSCONTROLLERS.gptk under the two
 * directory fds, O_NOFOLLOW, bounded), parse with `contract`, project.
 * Returns 0 when default or owner was selected; -1 when neither parses
 * (out cleared, receipt says why). Never mutates the filesystem. */
int nxinput_gptk4_load_project_at(int owner_dir_fd, int defaults_dir_fd,
                                  const nxinput_gptk4_contract *contract,
                                  nxinput_gptk4 *g4_out, nxinput_gptk *live_out,
                                  nxinput_gptk4_bridge_receipt *r);

/* Bounded JSON of the receipt (path-free). Returns 0 or -1. */
int nxinput_gptk4_bridge_receipt_json(const nxinput_gptk4_bridge_receipt *r, char *out, size_t cap);
#ifdef __cplusplus
}
#endif
#endif
