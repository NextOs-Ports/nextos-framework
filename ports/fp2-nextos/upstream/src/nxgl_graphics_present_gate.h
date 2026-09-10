/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxgl_graphics_present_gate -- V4-GRAPHICS-04: the explicit two-phase,
 * post-first-present graphics evidence boundary.
 *
 * WHY THIS EXISTS
 * ---------------
 * On Wayland a perfectly valid window/context keeps the 1x1 placeholder
 * drawable until the guest presents its FIRST buffer. The V3 one-shot API
 * (nxgl_graphics_contract_adapter_evidence) waits for the drawable to leave
 * 1x1 BEFORE returning control -- but the guest cannot present until it gets
 * control back, so the wait can never succeed (the BB2 1.0.6 field case:
 * OpenGL ES 3.1, provider alive, drawable pinned at 1x1, provider aliases
 * retried, runtime status 255). The contract was right to refuse 1x1;
 * the integration error was demanding a POST-present proof at a PRE-present
 * boundary. This module makes that boundary explicit.
 *
 * The machine (pure -- no SDL, no EGL, no GL, no clock, no I/O; the adapter
 * measures and injects every number, including monotonic timestamps):
 *
 *   UNINITIALIZED -> REJECTED | AWAITING_FIRST_PRESENT
 *   AWAITING_FIRST_PRESENT -> REJECTED | PROVED
 *   PROVED  -> PROVED   (stable; never a second receipt)
 *   REJECTED -> REJECTED (terminal; no hidden recovery)
 *
 * Invariants:
 *  - nxgl_graphics_drawable_usable(1,1) stays false; 1x1 pre-present is
 *    AWAITING_FIRST_PRESENT, never success. A large pre-present drawable is
 *    ALSO only pending: pre-present size is diagnosis, not proof of a frame.
 *  - Only an observation made AFTER the guest's real present call-through can
 *    produce PROVED and the final receipt, exactly once.
 *  - The monotonic deadline starts at the FIRST present and never restarts
 *    per frame.
 *  - The gate never clears, draws or swaps; it only classifies measurements.
 *  - One gate per window/context pair; identity change is terminal.
 *  - While status is AWAITING_FIRST_PRESENT, reason NXGL_GRAPHICS_OK must
 *    never be read as approval: the status enum is the authority.
 *  - Absence of the declarative opt-in leaves the V3 one-shot API and its
 *    behavior byte-for-byte untouched; this header is purely additive.
 */
#ifndef NXGL_GRAPHICS_PRESENT_GATE_H
#define NXGL_GRAPHICS_PRESENT_GATE_H

#include "nxgl_graphics_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NXGL_GRAPHICS_PRESENT_GATE_API_VERSION 1u

/* Any single dimension beyond this is not a panel, it is a corrupt read. */
#define NXGL_GRAPHICS_DRAWABLE_MAX_DIMENSION 16384

typedef enum nxgl_graphics_gate_status {
  NXGL_GRAPHICS_GATE_REJECTED = 0,
  NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT = 1,
  NXGL_GRAPHICS_GATE_PROVED = 2
} nxgl_graphics_gate_status;

/* Internal lifecycle phase, exposed only so the struct has a fixed public
 * layout the caller can allocate. UNINITIALIZED is what memset(0) yields, and
 * an all-zero gate is deliberately unusable until _init. */
typedef enum nxgl_graphics_gate_phase {
  NXGL_GRAPHICS_GATE_PHASE_UNINITIALIZED = 0,
  NXGL_GRAPHICS_GATE_PHASE_READY = 1,   /* init done, no preflight yet */
  NXGL_GRAPHICS_GATE_PHASE_AWAITING = 2,
  NXGL_GRAPHICS_GATE_PHASE_PROVED = 3,
  NXGL_GRAPHICS_GATE_PHASE_REJECTED = 4
} nxgl_graphics_gate_phase;

/* Fixed-layout, caller-allocated state. No hidden allocation; _init/_reset
 * are the only sanctioned writers besides the two transition calls. */
typedef struct nxgl_graphics_present_gate {
  uint32_t api_version; /* NXGL_GRAPHICS_PRESENT_GATE_API_VERSION */
  size_t struct_size;   /* sizeof(nxgl_graphics_present_gate) */
  nxgl_graphics_gate_phase phase;
  nxgl_graphics_gate_status status;
  nxgl_graphics_reason reason;
  uintptr_t window_token;  /* opaque identities; never dereferenced */
  uintptr_t context_token;
  int pre_drawable_w;      /* pre-present diagnosis, not proof */
  int pre_drawable_h;
  int timeout_ms;          /* copied from the contract at preflight */
  int64_t first_present_ms; /* monotonic; <0 until the first present */
  int64_t deadline_ms;      /* first_present_ms + timeout; single budget */
  int receipt_emitted;      /* one-shot latch */
  nxgl_graphics_evidence evidence; /* captured at preflight; drawable and
                                    * verdict updated by the final verdict */
} nxgl_graphics_present_gate;

typedef struct nxgl_graphics_gate_result {
  uint32_t api_version;
  size_t struct_size;
  nxgl_graphics_gate_status status;
  nxgl_graphics_reason reason;
  nxgl_graphics_evidence evidence;
} nxgl_graphics_gate_result;

/* Initialise a gate to READY (no proof, no tokens). Returns 0, -1 on NULL. */
int nxgl_graphics_present_gate_init(nxgl_graphics_present_gate *gate);

/* Invalidate every prior proof and return the gate to READY. A new context
 * REQUIRES reset + preflight; nothing is inherited across reset. */
void nxgl_graphics_present_gate_reset(nxgl_graphics_present_gate *gate);

/* Initialise a result envelope. Returns 0, -1 on NULL. */
int nxgl_graphics_gate_result_init(nxgl_graphics_gate_result *result);

/* PHASE 1 (pure): classify the pre-present measurements.
 *
 * `measured` is the evidence the adapter gathered on the thread owning the
 * context: obtained api/profile/version, provider paths/build-ids, GL strings,
 * provenance, sdl_major, the single diagnostic drawable read (drawable_w/h)
 * and the REAL shader probe result. This function decides:
 *   REJECTED               -- structural misuse, contract divergence
 *                             (api/profile/version), dead provider
 *                             (all-zero obtained sentinel), or a shader probe
 *                             that did not PASS. Terminal.
 *   AWAITING_FIRST_PRESENT -- everything above passed. The drawable size --
 *                             1x1 OR already large -- is recorded as diagnosis
 *                             only and NEVER promotes.
 * Never emits the final receipt and never publishes health. */
nxgl_graphics_gate_status nxgl_graphics_present_gate_preflight(
    nxgl_graphics_present_gate *gate,
    const nxgl_graphics_contract *contract,
    const nxgl_graphics_evidence *measured,
    uintptr_t window_token,
    uintptr_t context_token,
    nxgl_graphics_gate_result *result);

/* Optional non-promotional diagnostic line for phase 1, e.g.
 *   GRAPHICS-PREPRESENT-EVIDENCE: state=awaiting-first-present final=0 drawable=1x1
 * Never parsed as health or release evidence. Returns bytes written
 * (excluding NUL), 0 on bad args / short buffer / wrong phase. */
size_t nxgl_graphics_present_gate_prepresent_line(
    const nxgl_graphics_present_gate *gate, char *buf, size_t cap);

/* PHASE 2 (pure): classify one observation made AFTER a real present.
 *
 * The wrapper MUST have called the guest's real SDL_GL_SwapWindow /
 * eglSwapBuffers first; this function only classifies what was measured
 * afterwards. `drawable_read_ok` is 0 when the size could not be read at all
 * (symbol absent / call failed). `gles_provider`/`egl_provider` are the
 * re-derived provider identities ("" allowed when the preflight also recorded
 * ""); `now_ms` is CLOCK_MONOTONIC in ms, or <0 when the clock could not be
 * read (fails closed: NXGL_GRAPHICS_CLOCK_UNAVAILABLE).
 *
 * The first call stamps first_present_ms=now and the single deadline. Then:
 *   PROVED   -- tokens and providers unchanged AND the drawable satisfies
 *               nxgl_graphics_drawable_usable. Emits the final receipt into
 *               `final_receipt` exactly once (one-shot; later calls return
 *               PROVED with an empty receipt).
 *   AWAITING -- drawable still unusable but within the deadline.
 *   REJECTED -- identity changed (gate-identity-changed), unreadable size
 *               (drawable-unreadable), absurd size (drawable-absurd),
 *               deadline expired while <=1x1 (drawable-stuck-1x1), clock
 *               unavailable, or misuse. Terminal; the diagnostic receipt for a
 *               post-present rejection is also emitted exactly once.
 *
 * The final receipt is the GRAPHICS-EVIDENCE line extended with
 * `phase=post-first-present first_present=1 pre_drawable=WxH port_id=...
 * port_version=... egl_build_id=...`; nxrelease requires those fields when the
 * port opted into evidence_boundary=post-first-present. */
nxgl_graphics_gate_status nxgl_graphics_present_gate_observe(
    nxgl_graphics_present_gate *gate,
    const nxgl_graphics_contract *contract,
    uintptr_t window_token,
    uintptr_t context_token,
    int drawable_w, int drawable_h, int drawable_read_ok,
    const char *gles_provider,
    const char *egl_provider,
    int64_t now_ms,
    nxgl_graphics_gate_result *result,
    char *final_receipt, size_t final_receipt_cap);

const char *nxgl_graphics_gate_status_name(nxgl_graphics_gate_status status);

#ifdef __cplusplus
}
#endif

#endif /* NXGL_GRAPHICS_PRESENT_GATE_H */
