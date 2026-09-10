/* SPDX-License-Identifier: GPL-3.0-only */
/* nxgl_graphics_present_gate -- see include/nxgl_graphics_present_gate.h.
 * Pure: no SDL, no EGL, no GL, no clock, no I/O. The adapter measures and
 * injects everything, including monotonic timestamps. */
#include "nxgl_graphics_present_gate.h"

#include <stdio.h>
#include <string.h>

static const char *nxgl_pg_field(const char *s) {
  return (s != NULL && s[0] != '\0') ? s : "-";
}

int nxgl_graphics_present_gate_init(nxgl_graphics_present_gate *gate) {
  if (gate == NULL) {
    return -1;
  }
  memset(gate, 0, sizeof(*gate));
  gate->api_version = NXGL_GRAPHICS_PRESENT_GATE_API_VERSION;
  gate->struct_size = sizeof(*gate);
  gate->phase = NXGL_GRAPHICS_GATE_PHASE_READY;
  gate->status = NXGL_GRAPHICS_GATE_REJECTED; /* nothing proven yet */
  gate->reason = NXGL_GRAPHICS_OK;
  gate->first_present_ms = -1;
  gate->deadline_ms = -1;
  (void)nxgl_graphics_evidence_init(&gate->evidence);
  return 0;
}

void nxgl_graphics_present_gate_reset(nxgl_graphics_present_gate *gate) {
  if (gate == NULL) {
    return;
  }
  (void)nxgl_graphics_present_gate_init(gate);
}

int nxgl_graphics_gate_result_init(nxgl_graphics_gate_result *result) {
  if (result == NULL) {
    return -1;
  }
  memset(result, 0, sizeof(*result));
  result->api_version = NXGL_GRAPHICS_PRESENT_GATE_API_VERSION;
  result->struct_size = sizeof(*result);
  result->status = NXGL_GRAPHICS_GATE_REJECTED;
  result->reason = NXGL_GRAPHICS_GATE_MISUSE;
  (void)nxgl_graphics_evidence_init(&result->evidence);
  return 0;
}

/* A structurally usable gate: correctly versioned and sized. Phase checks are
 * separate so misuse can be reported with a stable reason. */
static int nxgl_pg_gate_struct_ok(const nxgl_graphics_present_gate *gate) {
  return gate != NULL &&
         gate->api_version == NXGL_GRAPHICS_PRESENT_GATE_API_VERSION &&
         gate->struct_size == sizeof(*gate);
}

static int nxgl_pg_result_struct_ok(const nxgl_graphics_gate_result *result) {
  return result != NULL &&
         result->api_version == NXGL_GRAPHICS_PRESENT_GATE_API_VERSION &&
         result->struct_size == sizeof(*result);
}

static void nxgl_pg_fill_result(const nxgl_graphics_present_gate *gate,
                                nxgl_graphics_gate_result *result) {
  if (result == NULL) {
    return;
  }
  result->status = gate->status;
  result->reason = gate->reason;
  result->evidence = gate->evidence;
}

/* Terminal rejection: latch phase/status/reason and mirror the verdict into
 * the stored evidence so a diagnostic receipt names the same reason. */
static nxgl_graphics_gate_status nxgl_pg_reject(
    nxgl_graphics_present_gate *gate, nxgl_graphics_reason reason,
    nxgl_graphics_gate_result *result) {
  gate->phase = NXGL_GRAPHICS_GATE_PHASE_REJECTED;
  gate->status = NXGL_GRAPHICS_GATE_REJECTED;
  gate->reason = reason;
  gate->evidence.verdict = reason;
  nxgl_pg_fill_result(gate, result);
  return NXGL_GRAPHICS_GATE_REJECTED;
}

/* When the measure step fails (the GL string query answered nothing) the adapter
 * hands over a fully zeroed obtained struct -- api_version and struct_size
 * included, so it can never be mistaken for a real "GLES 0.0" measurement.
 * That is a dead provider, not a malformed call. */
static int nxgl_pg_obtained_is_zero_sentinel(const nxgl_graphics_obtained *o) {
  return o->api_version == 0u && o->struct_size == 0u && o->api == 0 &&
         o->profile == 0 && o->version_major == 0 && o->version_minor == 0;
}

nxgl_graphics_gate_status nxgl_graphics_present_gate_preflight(
    nxgl_graphics_present_gate *gate,
    const nxgl_graphics_contract *contract,
    const nxgl_graphics_evidence *measured,
    uintptr_t window_token,
    uintptr_t context_token,
    nxgl_graphics_gate_result *result) {
  nxgl_graphics_reason reason;

  if (nxgl_pg_result_struct_ok(result)) {
    (void)nxgl_graphics_gate_result_init(result);
  } else {
    result = NULL; /* an unusable envelope is never written */
  }
  if (!nxgl_pg_gate_struct_ok(gate)) {
    return NXGL_GRAPHICS_GATE_REJECTED;
  }
  /* Preflight is only legal exactly once, on a READY gate. Anything else is
   * misuse and fails closed with no hidden recovery. */
  if (gate->phase != NXGL_GRAPHICS_GATE_PHASE_READY) {
    return nxgl_pg_reject(gate, NXGL_GRAPHICS_GATE_MISUSE, result);
  }
  if (measured == NULL ||
      measured->api_version != NXGL_GRAPHICS_CONTRACT_API_VERSION ||
      measured->struct_size != sizeof(*measured)) {
    return nxgl_pg_reject(gate, NXGL_GRAPHICS_GATE_MISUSE, result);
  }
  if (!nxgl_graphics_contract_is_valid(contract)) {
    return nxgl_pg_reject(gate, NXGL_GRAPHICS_CONTRACT_INVALID, result);
  }

  /* Capture everything measured before deciding, so even a rejection carries
   * the full diagnosis. */
  gate->evidence = *measured;
  gate->window_token = window_token;
  gate->context_token = context_token;
  gate->pre_drawable_w = measured->drawable_w;
  gate->pre_drawable_h = measured->drawable_h;
  gate->timeout_ms = contract->drawable_ready_timeout_ms;

  /* Dead provider: the GL string query answered nothing at all. */
  if (!nxgl_graphics_obtained_is_valid(&measured->obtained)) {
    if (nxgl_pg_obtained_is_zero_sentinel(&measured->obtained)) {
      return nxgl_pg_reject(gate, NXGL_GRAPHICS_PROVIDER_NOMINAL_ONLY, result);
    }
    return nxgl_pg_reject(gate, NXGL_GRAPHICS_GATE_MISUSE, result);
  }
  /* Context contract: API first, then profile, then version policy. These are
   * terminal BEFORE the pending state, so a provider retry may still happen
   * before the guest starts. */
  reason = nxgl_graphics_contract_validate(contract, &measured->obtained);
  if (reason != NXGL_GRAPHICS_OK) {
    return nxgl_pg_reject(gate, reason, result);
  }
  /* The declared dialect must have really compiled and linked on the LIVE
   * context. SKIPPED is not a pass. */
  if (measured->shader_probe != NXGL_SHADER_PROBE_PASS) {
    return nxgl_pg_reject(gate, NXGL_GRAPHICS_SHADER_PROBE_FAILED, result);
  }

  /* Everything provable pre-present passed. The drawable -- 1x1 or already
   * large -- is diagnosis only: PENDING, never success, never a receipt. */
  gate->phase = NXGL_GRAPHICS_GATE_PHASE_AWAITING;
  gate->status = NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT;
  gate->reason = NXGL_GRAPHICS_OK;
  nxgl_pg_fill_result(gate, result);
  return NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT;
}

size_t nxgl_graphics_present_gate_prepresent_line(
    const nxgl_graphics_present_gate *gate, char *buf, size_t cap) {
  int written;

  if (buf == NULL || cap == 0u) {
    return 0u;
  }
  buf[0] = '\0';
  if (!nxgl_pg_gate_struct_ok(gate) ||
      gate->phase != NXGL_GRAPHICS_GATE_PHASE_AWAITING) {
    return 0u;
  }
  written = snprintf(
      buf, cap,
      "GRAPHICS-PREPRESENT-EVIDENCE: state=awaiting-first-present final=0 "
      "drawable=%dx%d",
      gate->pre_drawable_w, gate->pre_drawable_h);
  if (written < 0 || (size_t)written >= cap) {
    buf[0] = '\0';
    return 0u;
  }
  return (size_t)written;
}

/* Emit the final (one-shot) receipt: the classic GRAPHICS-EVIDENCE line plus
 * the post-first-present extension fields nxrelease requires under the
 * declarative opt-in. Unknown key=value tokens are ignored by the legacy
 * parser, so the extension is additive. */
static void nxgl_pg_emit_receipt(nxgl_graphics_present_gate *gate,
                                 const nxgl_graphics_contract *contract,
                                 char *buf, size_t cap) {
  size_t base;
  int written;

  if (buf == NULL || cap == 0u) {
    return;
  }
  buf[0] = '\0';
  if (gate->receipt_emitted) {
    return; /* one-shot: later calls observe an empty receipt */
  }
  base = nxgl_graphics_contract_evidence_receipt(contract, &gate->evidence,
                                                 buf, cap);
  if (base == 0u) {
    return;
  }
  written = snprintf(
      buf + base, cap - base,
      " phase=post-first-present first_present=1 pre_drawable=%dx%d"
      " port_id=%s port_version=%s egl_build_id=%s",
      gate->pre_drawable_w, gate->pre_drawable_h,
      nxgl_pg_field(gate->evidence.port_id),
      nxgl_pg_field(gate->evidence.port_version),
      nxgl_pg_field(gate->evidence.egl_build_id));
  if (written < 0 || (size_t)written >= cap - base) {
    buf[0] = '\0';
    return;
  }
  gate->receipt_emitted = 1;
}

static int nxgl_pg_provider_changed(const char *recorded,
                                    const char *observed) {
  const char *a = recorded != NULL ? recorded : "";
  const char *b = observed != NULL ? observed : "";
  return strcmp(a, b) != 0;
}

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
    char *final_receipt, size_t final_receipt_cap) {
  if (final_receipt != NULL && final_receipt_cap > 0u) {
    final_receipt[0] = '\0';
  }
  if (nxgl_pg_result_struct_ok(result)) {
    (void)nxgl_graphics_gate_result_init(result);
  } else {
    result = NULL;
  }
  if (!nxgl_pg_gate_struct_ok(gate)) {
    return NXGL_GRAPHICS_GATE_REJECTED;
  }
  /* Terminal states are stable: PROVED stays PROVED with no new receipt;
   * REJECTED stays REJECTED with its original reason. */
  if (gate->phase == NXGL_GRAPHICS_GATE_PHASE_PROVED) {
    nxgl_pg_fill_result(gate, result);
    return NXGL_GRAPHICS_GATE_PROVED;
  }
  if (gate->phase == NXGL_GRAPHICS_GATE_PHASE_REJECTED) {
    nxgl_pg_fill_result(gate, result);
    return NXGL_GRAPHICS_GATE_REJECTED;
  }
  /* after_present on an UNINITIALIZED or merely READY gate is misuse: the
   * preflight proof does not exist, so nothing can be promoted. */
  if (gate->phase != NXGL_GRAPHICS_GATE_PHASE_AWAITING) {
    return nxgl_pg_reject(gate, NXGL_GRAPHICS_GATE_MISUSE, result);
  }
  if (!nxgl_graphics_contract_is_valid(contract)) {
    return nxgl_pg_reject(gate, NXGL_GRAPHICS_CONTRACT_INVALID, result);
  }
  /* Identity: same window, same context, same providers as the preflight.
   * A shader/context proof inherited from another identity is worthless. */
  if (window_token != gate->window_token ||
      context_token != gate->context_token ||
      nxgl_pg_provider_changed(gate->evidence.gles_provider, gles_provider) ||
      nxgl_pg_provider_changed(gate->evidence.egl_provider, egl_provider)) {
    return nxgl_pg_reject(gate, NXGL_GRAPHICS_GATE_IDENTITY_CHANGED, result);
  }
  /* A bounded deadline needs a trustworthy monotonic clock. */
  if (now_ms < 0) {
    return nxgl_pg_reject(gate, NXGL_GRAPHICS_CLOCK_UNAVAILABLE, result);
  }
  /* The single budget starts at the FIRST real present, never per frame. */
  if (gate->first_present_ms < 0) {
    gate->first_present_ms = now_ms;
    gate->deadline_ms = now_ms + (int64_t)gate->timeout_ms;
  }

  if (!drawable_read_ok) {
    gate->evidence.drawable_w = 0;
    gate->evidence.drawable_h = 0;
    return nxgl_pg_reject(gate, NXGL_GRAPHICS_DRAWABLE_UNREADABLE, result);
  }
  gate->evidence.drawable_w = drawable_w;
  gate->evidence.drawable_h = drawable_h;
  if (drawable_w < 0 || drawable_h < 0 ||
      (drawable_w == 0 && drawable_h == 0) ||
      ((drawable_w == 0) != (drawable_h == 0))) {
    return nxgl_pg_reject(gate, NXGL_GRAPHICS_DRAWABLE_UNREADABLE, result);
  }
  if (drawable_w > NXGL_GRAPHICS_DRAWABLE_MAX_DIMENSION ||
      drawable_h > NXGL_GRAPHICS_DRAWABLE_MAX_DIMENSION) {
    return nxgl_pg_reject(gate, NXGL_GRAPHICS_DRAWABLE_ABSURD, result);
  }

  if (nxgl_graphics_drawable_usable(drawable_w, drawable_h)) {
    /* Only here -- an observation AFTER the guest's real present, with the
     * same identity -- may the gate conclude, exactly once. */
    gate->phase = NXGL_GRAPHICS_GATE_PHASE_PROVED;
    gate->status = NXGL_GRAPHICS_GATE_PROVED;
    gate->reason = NXGL_GRAPHICS_OK;
    gate->evidence.verdict = NXGL_GRAPHICS_OK;
    nxgl_pg_emit_receipt(gate, contract, final_receipt, final_receipt_cap);
    nxgl_pg_fill_result(gate, result);
    return NXGL_GRAPHICS_GATE_PROVED;
  }

  if (now_ms >= gate->deadline_ms) {
    nxgl_graphics_gate_status status =
        nxgl_pg_reject(gate, NXGL_GRAPHICS_DRAWABLE_STUCK_1X1, result);
    /* Diagnostic receipt (verdict=FAIL) for the post-present terminal, also
     * one-shot; it is never promotable (nxrelease requires verdict OK). */
    nxgl_pg_emit_receipt(gate, contract, final_receipt, final_receipt_cap);
    return status;
  }

  /* Still pending inside the single budget: the guest keeps presenting. */
  gate->status = NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT;
  gate->reason = NXGL_GRAPHICS_OK;
  nxgl_pg_fill_result(gate, result);
  return NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT;
}

const char *nxgl_graphics_gate_status_name(nxgl_graphics_gate_status status) {
  switch (status) {
    case NXGL_GRAPHICS_GATE_PROVED:
      return "proved";
    case NXGL_GRAPHICS_GATE_AWAITING_FIRST_PRESENT:
      return "awaiting-first-present";
    case NXGL_GRAPHICS_GATE_REJECTED:
    default:
      return "rejected";
  }
}
