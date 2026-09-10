/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxaudio_receipt.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* V3-AUDIO-01. Everything here is pure bookkeeping over caller-measured
 * facts: no thread, no clock read, no environment read, no device open and
 * no _exit/exit/abort. Backend choice is capability-measured only — see
 * nxaudio_backend_probe_order(); no function in this module (or in nxaudio
 * at all, enforced by the receipt runner's source grep) branches on a
 * device or brand name. */

static int nxaudio_receipt_name_valid(const char *name, size_t limit) {
  size_t index;
  if (!name || !memchr(name, '\0', limit) || name[0] == '\0')
    return 0;
  for (index = 0u; name[index]; ++index) {
    char c = name[index];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '=')
      return 0;
  }
  return 1;
}

static const char *nxaudio_receipt_sample_name(nxaudio_sample_format format) {
  return format == NXAUDIO_SAMPLE_S16LE ? "s16le" :
         format == NXAUDIO_SAMPLE_F32LE ? "f32le" : NULL;
}

void nxaudio_receipt_init(nxaudio_receipt *receipt) {
  if (!receipt)
    return;
  memset(receipt, 0, sizeof(*receipt));
  receipt->api_version = NXAUDIO_RECEIPT_API_VERSION;
  receipt->struct_size = sizeof(*receipt);
}

nxaudio_result nxaudio_receipt_format(const nxaudio_receipt *receipt,
                                      char *line, size_t line_size) {
  const char *requested_sample, *obtained_sample;
  int written;
  if (line && line_size)
    line[0] = '\0';
  if (!receipt || !line || line_size == 0u ||
      receipt->api_version != NXAUDIO_RECEIPT_API_VERSION ||
      receipt->struct_size < sizeof(*receipt) ||
      !nxaudio_receipt_name_valid(receipt->api, sizeof(receipt->api)) ||
      !nxaudio_receipt_name_valid(receipt->backend,
                                  sizeof(receipt->backend)) ||
      receipt->requested_rate == 0u || receipt->obtained_rate == 0u ||
      receipt->requested_channels == 0u || receipt->obtained_channels == 0u ||
      !(receipt->peak_abs >= 0.0))
    return NXAUDIO_INVALID;
  requested_sample =
      nxaudio_receipt_sample_name(receipt->requested_sample_format);
  obtained_sample =
      nxaudio_receipt_sample_name(receipt->obtained_sample_format);
  if (!requested_sample || !obtained_sample)
    return NXAUDIO_INVALID;
  written = snprintf(
      line, line_size,
      "AUDIO-RECEIPT: api=%s backend=%s req=%dHz/%dch/%s got=%dHz/%dch/%s "
      "callbacks=%u bytes=%llu peak=%.3f underruns=%u",
      receipt->api, receipt->backend, (int)receipt->requested_rate,
      (int)receipt->requested_channels, requested_sample,
      (int)receipt->obtained_rate, (int)receipt->obtained_channels,
      obtained_sample, receipt->callbacks_observed,
      (unsigned long long)receipt->bytes_delivered, receipt->peak_abs,
      receipt->underrun_count);
  if (written < 0 || (size_t)written >= line_size) {
    line[0] = '\0';
    return NXAUDIO_FULL;
  }
  return NXAUDIO_OK;
}

size_t nxaudio_backend_probe_order(const char *const **out_stages) {
  /* The one documented capability order. Stage 1: an inherited/explicit
   * backend environment selection is sovereign (never overridden — same
   * rule nxaudio_plan_backend_retry enforces). Stage 2: candidates are
   * ranked by a MEASURED open success — real open, valid obtained spec,
   * live callbacks — on this machine, never by a device or brand name.
   * Stage 3: the adapter's statically declared fallback, still fail-closed
   * under the one-retry policy. */
  static const char *const stages[] = {
      "inherited-environment",
      "measured-open-success",
      "declared-fallback",
  };
  if (out_stages)
    *out_stages = stages;
  return sizeof(stages) / sizeof(stages[0]);
}

void nxaudio_liveness_init(nxaudio_liveness *liveness, uint64_t now_ns) {
  if (!liveness)
    return;
  memset(liveness, 0, sizeof(*liveness));
  liveness->api_version = NXAUDIO_RECEIPT_API_VERSION;
  liveness->struct_size = sizeof(*liveness);
  liveness->last_tick_ns = now_ns;
}

static int nxaudio_liveness_valid(const nxaudio_liveness *liveness) {
  return liveness && liveness->api_version == NXAUDIO_RECEIPT_API_VERSION &&
         liveness->struct_size >= sizeof(*liveness);
}

nxaudio_result nxaudio_liveness_tick(nxaudio_liveness *liveness,
                                     uint64_t now_ns) {
  if (!nxaudio_liveness_valid(liveness) || now_ns < liveness->last_tick_ns)
    return NXAUDIO_INVALID;
  liveness->last_tick_ns = now_ns;
  liveness->tick_count += 1u;
  return NXAUDIO_OK;
}

int nxaudio_liveness_dead(const nxaudio_liveness *liveness, uint64_t now_ns,
                          uint64_t budget_ns) {
  if (!nxaudio_liveness_valid(liveness) || budget_ns == 0u ||
      now_ns < liveness->last_tick_ns)
    return -1;
  return now_ns - liveness->last_tick_ns > budget_ns ? 1 : 0;
}

nxaudio_result nxaudio_liveness_underrun(nxaudio_liveness *liveness) {
  if (!nxaudio_liveness_valid(liveness))
    return NXAUDIO_INVALID;
  liveness->underrun_count += 1u;
  return NXAUDIO_OK;
}

uint32_t nxaudio_liveness_underruns(const nxaudio_liveness *liveness) {
  return nxaudio_liveness_valid(liveness) ? liveness->underrun_count : 0u;
}

nxaudio_result nxaudio_safe_exit_pump(nxaudio_safe_exit *state,
                                      uint64_t deadline_ns,
                                      nxaudio_safe_exit_poll_fn poll_fn,
                                      void *user) {
  /* Shutdown contract: keep pumping the backend's own confirmation until
   * it confirms or the deadline passes. Never _exit here — a port's final
   * `_exit(0)` happens AFTER this returns, in the port, once saves and
   * unlocks are done. No sleep and no thread: the caller models time via
   * poll_interval_ns. */
  if (!state || !poll_fn ||
      state->api_version != NXAUDIO_RECEIPT_API_VERSION ||
      state->struct_size < sizeof(*state) || state->poll_interval_ns == 0u) {
    if (state)
      state->status = NXAUDIO_SAFE_EXIT_PENDING;
    return NXAUDIO_INVALID;
  }
  state->polls = 0u;
  state->status = NXAUDIO_SAFE_EXIT_PENDING;
  while (state->now_ns <= deadline_ns) {
    state->polls += 1u;
    if (poll_fn(user)) {
      state->status = NXAUDIO_SAFE_EXIT_CONFIRMED;
      return NXAUDIO_OK;
    }
    /* Saturate instead of wrapping so a huge interval still terminates. */
    if (deadline_ns - state->now_ns < state->poll_interval_ns) {
      state->now_ns = deadline_ns;
      break;
    }
    state->now_ns += state->poll_interval_ns;
  }
  state->status = NXAUDIO_SAFE_EXIT_TIMEOUT;
  return NXAUDIO_UNSUPPORTED;
}

void nxaudio_backend_recovery_init(nxaudio_backend_recovery *state) {
  if (!state)
    return;
  memset(state, 0, sizeof(*state));
  state->api_version = NXAUDIO_RECEIPT_API_VERSION;
  state->struct_size = sizeof(*state);
}

static int nxaudio_recovery_fault_valid(nxaudio_recovery_fault fault) {
  return fault == NXAUDIO_RECOVERY_FAULT_XRUN_EPIPE ||
         fault == NXAUDIO_RECOVERY_FAULT_DEVICE_LOST ||
         fault == NXAUDIO_RECOVERY_FAULT_CALLBACK_STALLED;
}

nxaudio_result nxaudio_backend_recovery_run(
    nxaudio_backend_recovery *state, nxaudio_recovery_fault fault,
    nxaudio_recovery_step_fn recover_step,
    nxaudio_recovery_step_fn reopen_step, void *user) {
  if (!state ||
      state->api_version != NXAUDIO_RECEIPT_API_VERSION ||
      state->struct_size < sizeof(*state) ||
      !nxaudio_recovery_fault_valid(fault) ||
      (!recover_step && !reopen_step))
    return NXAUDIO_INVALID;

  state->fault = fault;
  state->recover_status = NXAUDIO_RECOVERY_STEP_NOT_ATTEMPTED;
  state->reopen_status = NXAUDIO_RECOVERY_STEP_NOT_ATTEMPTED;
  if (state->attempts >= 1u) {
    state->outcome = NXAUDIO_RECOVERY_EXHAUSTED;
    return NXAUDIO_UNSUPPORTED;
  }

  state->attempts += 1u;
  state->outcome = NXAUDIO_RECOVERY_PENDING;
  if (recover_step) {
    state->recover_status = recover_step(user) == 0
                                ? NXAUDIO_RECOVERY_STEP_OK
                                : NXAUDIO_RECOVERY_STEP_FAILED;
    if (state->recover_status == NXAUDIO_RECOVERY_STEP_OK) {
      state->outcome = NXAUDIO_RECOVERY_RECOVERED;
      return NXAUDIO_OK;
    }
  }

  if (reopen_step) {
    state->reopen_status = reopen_step(user) == 0
                               ? NXAUDIO_RECOVERY_STEP_OK
                               : NXAUDIO_RECOVERY_STEP_FAILED;
    if (state->reopen_status == NXAUDIO_RECOVERY_STEP_OK) {
      state->outcome = NXAUDIO_RECOVERY_REOPENED;
      return NXAUDIO_OK;
    }
  }

  state->outcome = NXAUDIO_RECOVERY_FAILED;
  return NXAUDIO_UNSUPPORTED;
}

nxaudio_recovery_fault nxaudio_recovery_fault_from_errno(int backend_error) {
  if (backend_error == EPIPE || backend_error == -EPIPE)
    return NXAUDIO_RECOVERY_FAULT_XRUN_EPIPE;
#ifdef ESTRPIPE
  if (backend_error == ESTRPIPE || backend_error == -ESTRPIPE)
    return NXAUDIO_RECOVERY_FAULT_XRUN_EPIPE;
#endif
  return NXAUDIO_RECOVERY_FAULT_NONE;
}

static const char *nxaudio_recovery_fault_name(nxaudio_recovery_fault fault) {
  switch (fault) {
    case NXAUDIO_RECOVERY_FAULT_XRUN_EPIPE:
      return "xrun-epipe";
    case NXAUDIO_RECOVERY_FAULT_DEVICE_LOST:
      return "device-lost";
    case NXAUDIO_RECOVERY_FAULT_CALLBACK_STALLED:
      return "callback-stalled";
    default:
      return NULL;
  }
}

static const char *nxaudio_recovery_step_name(
    nxaudio_recovery_step_status status) {
  switch (status) {
    case NXAUDIO_RECOVERY_STEP_NOT_ATTEMPTED:
      return "not-attempted";
    case NXAUDIO_RECOVERY_STEP_OK:
      return "ok";
    case NXAUDIO_RECOVERY_STEP_FAILED:
      return "failed";
    default:
      return NULL;
  }
}

static const char *nxaudio_recovery_outcome_name(
    nxaudio_recovery_outcome outcome) {
  switch (outcome) {
    case NXAUDIO_RECOVERY_RECOVERED:
      return "recovered";
    case NXAUDIO_RECOVERY_REOPENED:
      return "reopened";
    case NXAUDIO_RECOVERY_FAILED:
      return "failed";
    case NXAUDIO_RECOVERY_EXHAUSTED:
      return "exhausted";
    default:
      return NULL;
  }
}

nxaudio_result nxaudio_backend_recovery_format(
    const nxaudio_backend_recovery *state, char *line, size_t line_size) {
  const char *fault_name;
  const char *recover_name;
  const char *reopen_name;
  const char *outcome_name;
  int written;

  if (line && line_size)
    line[0] = '\0';
  if (!state || !line || line_size == 0u ||
      state->api_version != NXAUDIO_RECEIPT_API_VERSION ||
      state->struct_size < sizeof(*state) || state->attempts != 1u)
    return NXAUDIO_INVALID;
  fault_name = nxaudio_recovery_fault_name(state->fault);
  recover_name = nxaudio_recovery_step_name(state->recover_status);
  reopen_name = nxaudio_recovery_step_name(state->reopen_status);
  outcome_name = nxaudio_recovery_outcome_name(state->outcome);
  if (!fault_name || !recover_name || !reopen_name || !outcome_name)
    return NXAUDIO_INVALID;
  written = snprintf(
      line, line_size,
      "AUDIO-RECOVERY: fault=%s attempt=%u recover=%s reopen=%s result=%s",
      fault_name, state->attempts, recover_name, reopen_name, outcome_name);
  if (written < 0 || (size_t)written >= line_size) {
    line[0] = '\0';
    return NXAUDIO_FULL;
  }
  return NXAUDIO_OK;
}
