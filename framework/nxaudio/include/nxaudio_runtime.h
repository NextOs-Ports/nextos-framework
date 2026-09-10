/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXAUDIO_RUNTIME_H
#define NXAUDIO_RUNTIME_H

/* nxaudio 0.4.0: a provider-neutral, generation-bound runtime attestation.
 *
 * The adapter supplies already measured facts.  This module does not select
 * a provider, read an environment, open a device, allocate memory, create a
 * thread or call guest code.  Callback-facing entry points use bounded
 * 32-bit lock-free atomics and never format text.  Receipt formatting is a
 * worker/terminal operation.
 *
 * The public 0.3.1 interfaces in nxaudio.h and nxaudio_receipt.h remain
 * unchanged.  This is a separately versioned additive surface. */

#include <stddef.h>
#include <stdint.h>

#include "nxaudio.h"
#include "nxaudio_receipt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NXAUDIO_RUNTIME_API_VERSION 2u
#define NXAUDIO_RUNTIME_RECEIPT_LINE_MAX 768u

typedef enum nxaudio_runtime_state {
  NXAUDIO_RUNTIME_STATE_NEW = 0,
  NXAUDIO_RUNTIME_STATE_OPENED = 1,
  NXAUDIO_RUNTIME_STATE_LIVE = 2,
  NXAUDIO_RUNTIME_STATE_PAUSED = 3,
  NXAUDIO_RUNTIME_STATE_DEGRADED = 4,
  NXAUDIO_RUNTIME_STATE_RECOVERING = 5,
  NXAUDIO_RUNTIME_STATE_FAILED = 6,
  NXAUDIO_RUNTIME_STATE_DRAINING = 7,
  NXAUDIO_RUNTIME_STATE_CLOSED = 8
} nxaudio_runtime_state;

/* A compact token is copied to every callback/worker event.  The API and
 * backend names stay in the session for the terminal receipt; their hashes
 * bind this token without putting strings on the realtime path. */
typedef struct nxaudio_runtime_identity {
  uint32_t api_version;
  size_t struct_size;
  uint32_t run_id;
  uint32_t generation;
  uint32_t api_tag;
  uint32_t backend_tag;
} nxaudio_runtime_identity;

typedef enum nxaudio_runtime_terminal_reason {
  NXAUDIO_RUNTIME_REASON_NONE = 0,
  NXAUDIO_RUNTIME_REASON_COHERENT_FIXTURE = 1,
  NXAUDIO_RUNTIME_REASON_NOT_TERMINAL = 2,
  NXAUDIO_RUNTIME_REASON_CALLBACK_MISSING = 3,
  NXAUDIO_RUNTIME_REASON_EXPECTED_MISSING = 4,
  NXAUDIO_RUNTIME_REASON_PCM_ZERO = 5,
  NXAUDIO_RUNTIME_REASON_SHUTDOWN_TIMEOUT = 6,
  NXAUDIO_RUNTIME_REASON_RECOVERY_PENDING = 7,
  NXAUDIO_RUNTIME_REASON_RECOVERY_FAILED = 8,
  NXAUDIO_RUNTIME_REASON_IDENTITY_MISMATCH = 9,
  NXAUDIO_RUNTIME_REASON_PRODUCER_MISMATCH = 10,
  NXAUDIO_RUNTIME_REASON_TIME_INVALID = 11,
  NXAUDIO_RUNTIME_REASON_EVENT_ORDER = 12,
  NXAUDIO_RUNTIME_REASON_COUNTER_OVERFLOW = 13,
  NXAUDIO_RUNTIME_REASON_STATE_FAILED = 14,
  NXAUDIO_RUNTIME_REASON_REOPEN_REQUIRED = 15,
  NXAUDIO_RUNTIME_REASON_FORMAT_MISMATCH = 16,
  NXAUDIO_RUNTIME_REASON_ALREADY_FORMATTED = 17
} nxaudio_runtime_terminal_reason;

typedef struct nxaudio_runtime_snapshot {
  uint32_t api_version;
  size_t struct_size;
  nxaudio_runtime_identity identity;
  nxaudio_runtime_state state;
  nxaudio_format requested_format;
  nxaudio_format granted_format;
  nxaudio_reason format_match_reason;
  uint32_t callbacks_expected;
  uint32_t callbacks_observed;
  uint32_t frames_delivered;
  uint32_t bytes_delivered;
  uint32_t nonzero_samples;
  uint32_t peak_milli;
  uint32_t underrun_count;
  uint32_t silence_frames;
  uint32_t device_loss_count;
  uint32_t opened_ms;
  uint32_t last_callback_ms;
  uint32_t closed_ms;
  uint32_t callback_epoch;
  uint32_t recovery_recorded;
  uint32_t overflowed;
  uint32_t identity_violation;
  uint32_t producer_violation;
  uint32_t time_violation;
  uint32_t order_violation;
  nxaudio_backend_recovery recovery;
  nxaudio_safe_exit_status shutdown_status;
  nxaudio_result terminal_result;
  nxaudio_runtime_terminal_reason terminal_reason;
  uint32_t formatted;
} nxaudio_runtime_snapshot;

/* Fields marked realtime are accessed only through the API.  They are kept
 * as 32-bit objects so GCC/Clang __atomic operations are lock-free on the
 * supported ARMv7/AArch64 hosts.  Saturation is sticky and never wraps. */
typedef struct nxaudio_runtime_session {
  uint32_t api_version;
  size_t struct_size;
  nxaudio_runtime_identity identity;
  char api[NXAUDIO_NAME_MAX];
  char backend[NXAUDIO_NAME_MAX];
  nxaudio_format requested_format;
  nxaudio_format granted_format;
  nxaudio_reason format_match_reason;

  volatile uint32_t state;
  volatile uint32_t callbacks_expected;
  volatile uint32_t callbacks_observed;
  volatile uint32_t frames_delivered;
  volatile uint32_t bytes_delivered;
  volatile uint32_t nonzero_samples;
  volatile uint32_t peak_milli;
  volatile uint32_t underrun_count;
  volatile uint32_t silence_frames;
  volatile uint32_t device_loss_count;
  volatile uint32_t callback_producer_id;
  volatile uint32_t callback_active;
  volatile uint32_t callback_epoch;
  volatile uint32_t opened_ms;
  volatile uint32_t last_callback_ms;
  volatile uint32_t last_worker_ms;
  volatile uint32_t closed_ms;
  volatile uint32_t overflowed;
  volatile uint32_t identity_violation;
  volatile uint32_t producer_violation;
  volatile uint32_t time_violation;
  volatile uint32_t order_violation;
  volatile uint32_t formatted;
  volatile uint32_t recovery_fault;
  volatile uint32_t recovery_recorded;

  nxaudio_backend_recovery recovery;
  nxaudio_safe_exit_status shutdown_status;
  nxaudio_result terminal_result;
  nxaudio_runtime_terminal_reason terminal_reason;
} nxaudio_runtime_session;

/* Bookkeeping initialization and generation-bound open/reopen. */
void nxaudio_runtime_init(nxaudio_runtime_session *session, uint32_t run_id,
                          uint32_t generation);
nxaudio_result nxaudio_runtime_open(
    nxaudio_runtime_session *session, const char *api, const char *backend,
    const nxaudio_format *requested, const nxaudio_format *granted,
    uint32_t now_ms, nxaudio_runtime_identity *identity);
nxaudio_result nxaudio_runtime_reopen(
    const nxaudio_runtime_session *previous,
    const nxaudio_runtime_identity *previous_identity,
    nxaudio_runtime_session *next,
    const char *api, const char *backend, const nxaudio_format *requested,
    const nxaudio_format *granted, uint32_t now_ms,
    nxaudio_runtime_identity *identity);
nxaudio_result nxaudio_runtime_expect_callbacks(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t expected,
    uint32_t now_ms);

/* Worker transitions.  Every event presents the exact identity returned by
 * open/reopen plus caller-supplied monotonic milliseconds. */
nxaudio_result nxaudio_runtime_set_live(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t now_ms);
nxaudio_result nxaudio_runtime_set_paused(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t now_ms);
nxaudio_result nxaudio_runtime_set_degraded(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, nxaudio_recovery_fault fault,
    uint32_t now_ms);
nxaudio_result nxaudio_runtime_set_recovering(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t now_ms);
nxaudio_result nxaudio_runtime_record_recovery(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity,
    const nxaudio_backend_recovery *recovery, uint32_t now_ms);
nxaudio_result nxaudio_runtime_set_failed(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity,
    nxaudio_runtime_terminal_reason reason, uint32_t now_ms);
nxaudio_result nxaudio_runtime_set_draining(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t now_ms);
nxaudio_result nxaudio_runtime_set_closed(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity,
    nxaudio_safe_exit_status shutdown_status, uint32_t now_ms);

/* Realtime callback updates.  producer_id must be nonzero and stable for the
 * generation.  A second producer or stale identity fails closed. */
nxaudio_result nxaudio_runtime_update_callback(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t producer_id,
    uint32_t now_ms, uint32_t callbacks, uint32_t frames_delivered,
    uint32_t bytes_delivered, uint32_t nonzero_samples, double peak_abs);
nxaudio_result nxaudio_runtime_report_underrun(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t producer_id);
nxaudio_result nxaudio_runtime_report_silence(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t producer_id,
    uint32_t silence_frames);
nxaudio_result nxaudio_runtime_report_device_lost(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t producer_id,
    uint32_t now_ms);

/* Pure worker-side liveness query: PAUSED is never classified as stalled.
 * Returns 1 for a real LIVE stall, 0 otherwise and -1 for invalid identity,
 * time regression or a zero budget. */
int nxaudio_runtime_is_stalled(nxaudio_runtime_session *session,
                               const nxaudio_runtime_identity *identity,
                               uint32_t now_ms, uint32_t budget_ms);

nxaudio_runtime_state nxaudio_runtime_get_state(
    const nxaudio_runtime_session *session);
nxaudio_result nxaudio_runtime_get_snapshot(
    const nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity,
    nxaudio_runtime_snapshot *snapshot);

/* Exactly one terminal FIXTURE receipt may be emitted per generation.  A
 * short output buffer is retryable because no line was emitted. */
nxaudio_result nxaudio_runtime_format_receipt(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, char *line, size_t line_size);

const char *nxaudio_runtime_reason_name(
    nxaudio_runtime_terminal_reason reason);

#ifdef __cplusplus
}
#endif

#endif
