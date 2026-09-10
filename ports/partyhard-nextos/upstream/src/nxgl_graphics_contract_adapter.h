/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxgl_graphics_contract_adapter -- the half of V3-GRAPHICS-02 that must touch
 * SDL/EGL/GL. It MEASURES the context the driver actually gave (never trusts
 * the request), fills nxgl_graphics_obtained and reads the drawable, then hands
 * the numbers to the PURE validator in nxgl_graphics_contract. Vendored into
 * the port like nxgl_frame_proof_adapter; GL/SDL/EGL are resolved at runtime.
 *
 * The authority for "GLES vs desktop GL" is glGetString(GL_VERSION): a string
 * without "OpenGL ES" is a desktop context no matter what SDL_GL_SetAttribute
 * asked for (the Beach Buggy field case reported "3.1 Mesa"). SDL_GL_GetAttribute
 * and the EGL client APIs are recorded as corroboration, never as the sole call.
 */
#ifndef NXGL_GRAPHICS_CONTRACT_ADAPTER_H
#define NXGL_GRAPHICS_CONTRACT_ADAPTER_H

#include "nxgl_graphics_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional resolver for SDL/EGL/GL symbols (a so-loader port routes them
 * through its own shim). Tried before dlsym(RTLD_DEFAULT) and
 * SDL_GL_GetProcAddress. Passing NULL clears it. */
void nxgl_graphics_contract_adapter_set_resolver(void *(*resolver)(const char *));

/* Context-bearing form used by the nxloader provider. While non-NULL it is
 * authoritative, including a NULL result: no dlsym/SDL fallback can escape the
 * captured provider stack or resolve the bridge recursively. Calling either
 * setter replaces the previously configured resolver. */
typedef void *(*nxgl_graphics_resolver_fn)(void *userdata, const char *name);
void nxgl_graphics_contract_adapter_set_resolver_ex(
    nxgl_graphics_resolver_fn resolver, void *userdata);

/* Measure the obtained context into `obtained` from glGetString(GL_VERSION)
 * (authoritative) plus SDL_GL_GetAttribute (profile mask, for core/compat).
 * Returns 0 on success, -1 if neither source can be resolved/parsed. */
int nxgl_graphics_contract_adapter_measure(nxgl_graphics_obtained *obtained);

/* Read the current drawable size through SDL_GL_GetDrawableSize (SDL2) or
 * SDL_GetWindowSizeInPixels (SDL3), 0x0 if neither resolves. Returns 0 on
 * success, -1 otherwise. */
int nxgl_graphics_contract_adapter_drawable(int *width, int *height);

/* Poll the drawable on a REAL monotonic deadline (CLOCK_MONOTONIC), sleeping a
 * short interval and pumping SDL events between reads, until it leaves the 1x1
 * placeholder or `timeout_ms` elapses. This is how a driver that reports 1x1
 * for the first frames is distinguished from one that is genuinely stuck.
 * Returns 0 once usable, -1 on timeout / unresolved. `width`/`height` receive
 * the last size observed. A timeout_ms <= 0 means a single read (no wait). */
int nxgl_graphics_contract_adapter_drawable_wait(int *width, int *height,
                                                 int timeout_ms);

/* 2 for SDL2, 3 for SDL3, 0 if neither can be told apart. Detected by which
 * drawable-size symbol resolves (SDL3 renamed SDL_GL_GetDrawableSize to
 * SDL_GetWindowSizeInPixels), never by calling SDL_GetVersion with a guessed
 * signature. */
int nxgl_graphics_contract_adapter_sdl_major(void);

/* Run the REAL shader probe: build a minimal shader of the contract's dialect
 * (nxgl_shader_probe_source), compile a vertex + fragment shader against the
 * LIVE context, link them, and read GL_COMPILE_STATUS / GL_LINK_STATUS. Returns
 * NXGL_SHADER_PROBE_PASS only when both compile AND the program links;
 * COMPILE_FAILED / LINK_FAILED name the stage that failed; SKIPPED if the GL
 * entry points cannot be resolved (host build / no context) -- SKIPPED is never
 * a pass. Deletes every object it creates. */
nxgl_shader_probe_result nxgl_graphics_contract_adapter_shader_probe(
    const nxgl_graphics_contract *contract);

/* The full V3 evidence pass: measure the context, wait for a usable drawable on
 * the contract's monotonic timeout, run the shader probe, detect SDL major, and
 * gather provenance (run_id/generation/commit/CFW/EGL+GLES provider paths/DSO
 * build-id) from the environment the launcher established -- provenance is
 * RECORDED, never used to decide. Fills `ev` and writes the structured
 * nx-graphics-evidence JSON document into `receipt`. Returns the verdict. A GL
 * contract whose shader probe fails downgrades the verdict to
 * NXGL_GRAPHICS_SHADER_PROBE_FAILED even when the context itself matched. */
nxgl_graphics_reason nxgl_graphics_contract_adapter_evidence(
    const nxgl_graphics_contract *contract,
    nxgl_graphics_evidence *ev,
    char *receipt, size_t receipt_cap);

/* --- V4-GRAPHICS-04: the additive post-first-present boundary --------------
 * For backends (Wayland) whose drawable stays the 1x1 placeholder until the
 * guest presents its first buffer. The one-shot pass above stays byte-for-byte
 * as it is; a port only reaches these entry points through the declarative
 * opt-in graphics.evidence_boundary=post-first-present. */
#include "nxgl_graphics_present_gate.h"

/* PHASE 1, on the thread owning the context, BEFORE handing the window back
 * to the guest: gathers provenance, measures the context, records provider
 * identities, runs the real shader probe, reads the drawable ONCE (diagnosis
 * only -- no wait, no event pumping) and feeds the pure gate. Returns
 * AWAITING_FIRST_PRESENT on success; a divergent context/provider/shader is
 * REJECTED here, before the guest starts, so a provider retry stays legal.
 * Never emits the final receipt. */
nxgl_graphics_gate_status nxgl_graphics_contract_adapter_pre_present(
    const nxgl_graphics_contract *contract,
    uintptr_t window_token,
    uintptr_t context_token,
    nxgl_graphics_present_gate *gate,
    nxgl_graphics_gate_result *result);

/* PHASE 2, called by the wrapper AFTER the guest's real
 * SDL_GL_SwapWindow/eglSwapBuffers returned. Reads the monotonic clock and
 * the drawable, re-derives provider identity, and feeds the pure gate: the
 * first call stamps the single deadline; a usable drawable concludes PROVED
 * with the one-shot final receipt; persistent <=1x1 fails at the deadline
 * with drawable-stuck-1x1. After a terminal verdict the call is a cheap
 * stable no-op (no SDL/EGL/dl work per frame). Thread-safe: transitions are
 * serialised, and no lock is ever held across an SDL/EGL/GL call. */
nxgl_graphics_gate_status nxgl_graphics_contract_adapter_after_present(
    const nxgl_graphics_contract *contract,
    uintptr_t window_token,
    uintptr_t context_token,
    nxgl_graphics_present_gate *gate,
    nxgl_graphics_gate_result *result,
    char *final_receipt, size_t final_receipt_cap);

#ifdef __cplusplus
}
#endif

#endif /* NXGL_GRAPHICS_CONTRACT_ADAPTER_H */
