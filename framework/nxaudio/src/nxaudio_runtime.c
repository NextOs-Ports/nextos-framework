/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxaudio_runtime.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#if !defined(__GNUC__) && !defined(__clang__)
#error "nxaudio runtime requires GCC/Clang lock-free __atomic builtins"
#endif

#if UINT32_MAX == UINT_MAX
#if !defined(__GCC_ATOMIC_INT_LOCK_FREE) || __GCC_ATOMIC_INT_LOCK_FREE != 2
#error "nxaudio runtime requires always-lock-free 32-bit unsigned int atomics"
#endif
#elif UINT32_MAX == ULONG_MAX
#if !defined(__GCC_ATOMIC_LONG_LOCK_FREE) || __GCC_ATOMIC_LONG_LOCK_FREE != 2
#error "nxaudio runtime requires always-lock-free 32-bit unsigned long atomics"
#endif
#else
#error "nxaudio runtime could not prove an always-lock-free uint32_t"
#endif

static uint32_t load_u32(const volatile uint32_t *value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void store_u32(volatile uint32_t *value, uint32_t next) {
  __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

static int compare_u32(volatile uint32_t *value, uint32_t *expected,
                       uint32_t desired) {
  return __atomic_compare_exchange_n(value, expected, desired, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static int session_valid(const nxaudio_runtime_session *session) {
  return session && session->api_version == NXAUDIO_RUNTIME_API_VERSION &&
         session->struct_size >= sizeof(*session) &&
         session->identity.api_version == NXAUDIO_RUNTIME_API_VERSION &&
         session->identity.struct_size >= sizeof(session->identity) &&
         session->identity.run_id != 0u && session->identity.generation != 0u;
}

static int format_valid(const nxaudio_format *format) {
  return format && format->frequency != 0u && format->channels != 0u &&
         format->period_frames != 0u &&
         (format->sample_format == NXAUDIO_SAMPLE_S16LE ||
          format->sample_format == NXAUDIO_SAMPLE_F32LE);
}

static int name_valid(const char *name) {
  size_t index;
  if (!name || !memchr(name, '\0', NXAUDIO_NAME_MAX) || name[0] == '\0')
    return 0;
  if (!((name[0] >= 'a' && name[0] <= 'z') ||
        (name[0] >= 'A' && name[0] <= 'Z')))
    return 0;
  for (index = 0u; name[index] != '\0'; ++index) {
    const unsigned char c = (unsigned char)name[index];
    const int alpha_numeric =
        (c >= (unsigned char)'a' && c <= (unsigned char)'z') ||
        (c >= (unsigned char)'A' && c <= (unsigned char)'Z') ||
        (c >= (unsigned char)'0' && c <= (unsigned char)'9');
    if (!alpha_numeric && c != (unsigned char)'-' && c != (unsigned char)'_' &&
        c != (unsigned char)'.' && c != (unsigned char)'+')
      return 0;
  }
  return 1;
}

static uint32_t name_tag(const char *name) {
  uint32_t hash = UINT32_C(2166136261);
  size_t index;
  for (index = 0u; name[index] != '\0'; ++index) {
    hash ^= (uint32_t)(unsigned char)name[index];
    hash *= UINT32_C(16777619);
  }
  return hash == 0u ? 1u : hash;
}

static int identity_equal(const nxaudio_runtime_session *session,
                          const nxaudio_runtime_identity *identity) {
  return session_valid(session) && identity &&
         identity->api_version == NXAUDIO_RUNTIME_API_VERSION &&
         identity->struct_size >= sizeof(*identity) &&
         identity->run_id == session->identity.run_id &&
         identity->generation == session->identity.generation &&
         identity->api_tag == session->identity.api_tag &&
         identity->backend_tag == session->identity.backend_tag;
}

static int identity_event(nxaudio_runtime_session *session,
                          const nxaudio_runtime_identity *identity) {
  if (identity_equal(session, identity))
    return 1;
  if (session_valid(session))
    store_u32(&session->identity_violation, 1u);
  return 0;
}

static int worker_time(nxaudio_runtime_session *session, uint32_t now_ms) {
  uint32_t previous;
  if (now_ms < load_u32(&session->opened_ms)) {
    store_u32(&session->time_violation, 1u);
    return 0;
  }
  previous = load_u32(&session->last_worker_ms);
  if (now_ms < previous) {
    store_u32(&session->time_violation, 1u);
    return 0;
  }
  if (now_ms != previous &&
      !compare_u32(&session->last_worker_ms, &previous, now_ms)) {
    store_u32(&session->time_violation, 1u);
    return 0;
  }
  return 1;
}

static int callback_time(nxaudio_runtime_session *session, uint32_t now_ms) {
  uint32_t previous;
  if (now_ms < load_u32(&session->opened_ms)) {
    store_u32(&session->time_violation, 1u);
    return 0;
  }
  previous = load_u32(&session->last_callback_ms);
  if (now_ms < previous) {
    store_u32(&session->time_violation, 1u);
    return 0;
  }
  if (now_ms != previous &&
      !compare_u32(&session->last_callback_ms, &previous, now_ms)) {
    store_u32(&session->time_violation, 1u);
    return 0;
  }
  return 1;
}

static int producer_event(nxaudio_runtime_session *session,
                          uint32_t producer_id) {
  uint32_t owner;
  if (producer_id == 0u) {
    store_u32(&session->producer_violation, 1u);
    return 0;
  }
  owner = load_u32(&session->callback_producer_id);
  if (owner == 0u) {
    uint32_t unclaimed = 0u;
    if (compare_u32(&session->callback_producer_id, &unclaimed, producer_id))
      return 1;
    owner = unclaimed;
  }
  if (owner == producer_id)
    return 1;
  store_u32(&session->producer_violation, 1u);
  return 0;
}

static int saturating_add(nxaudio_runtime_session *session,
                          volatile uint32_t *value, uint32_t increment) {
  uint32_t previous = load_u32(value);
  uint32_t next;
  if (increment > UINT32_MAX - previous) {
    next = UINT32_MAX;
    store_u32(&session->overflowed, 1u);
  } else {
    next = previous + increment;
  }
  if (!compare_u32(value, &previous, next)) {
    store_u32(&session->order_violation, 1u);
    return 0;
  }
  return 1;
}

static int peak_update(nxaudio_runtime_session *session,
                       volatile uint32_t *value, uint32_t peak_milli) {
  uint32_t previous = load_u32(value);
  if (peak_milli <= previous)
    return 1;
  if (!compare_u32(value, &previous, peak_milli)) {
    store_u32(&session->order_violation, 1u);
    return 0;
  }
  return 1;
}

static int claim_recovery_fault(nxaudio_runtime_session *session,
                                nxaudio_recovery_fault fault) {
  uint32_t unclaimed = 0u;
  if (!compare_u32(&session->recovery_fault, &unclaimed, (uint32_t)fault)) {
    store_u32(&session->order_violation, 1u);
    return 0;
  }
  return 1;
}

static void release_recovery_fault(nxaudio_runtime_session *session,
                                   nxaudio_recovery_fault fault) {
  uint32_t owned = (uint32_t)fault;
  if (!compare_u32(&session->recovery_fault, &owned, 0u))
    store_u32(&session->order_violation, 1u);
}

static nxaudio_result callback_enter(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t producer_id,
    int check_time, uint32_t now_ms) {
  uint32_t inactive = 0u;
  if (!identity_event(session, identity) ||
      !producer_event(session, producer_id))
    return NXAUDIO_INVALID;
  if (!compare_u32(&session->callback_active, &inactive, producer_id)) {
    store_u32(&session->producer_violation, 1u);
    return NXAUDIO_INVALID;
  }
  if (load_u32(&session->state) != (uint32_t)NXAUDIO_RUNTIME_STATE_LIVE) {
    store_u32(&session->order_violation, 1u);
    store_u32(&session->callback_active, 0u);
    return NXAUDIO_WRONG_STATE;
  }
  if (check_time && !callback_time(session, now_ms)) {
    store_u32(&session->callback_active, 0u);
    return NXAUDIO_INVALID;
  }
  return NXAUDIO_OK;
}

static void callback_leave(nxaudio_runtime_session *session) {
  (void)saturating_add(session, &session->callback_epoch, 1u);
  store_u32(&session->callback_active, 0u);
}

static nxaudio_result transition(nxaudio_runtime_session *session,
                                 const nxaudio_runtime_identity *identity,
                                 uint32_t from_a, uint32_t from_b,
                                 uint32_t to, uint32_t now_ms) {
  uint32_t state;
  if (!identity_event(session, identity))
    return NXAUDIO_INVALID;
  if (!worker_time(session, now_ms))
    return NXAUDIO_INVALID;
  state = load_u32(&session->state);
  if (state != from_a && state != from_b) {
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_WRONG_STATE;
  }
  if (!compare_u32(&session->state, &state, to)) {
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_WRONG_STATE;
  }
  return NXAUDIO_OK;
}

void nxaudio_runtime_init(nxaudio_runtime_session *session, uint32_t run_id,
                          uint32_t generation) {
  if (!session)
    return;
  memset(session, 0, sizeof(*session));
  session->api_version = NXAUDIO_RUNTIME_API_VERSION;
  session->struct_size = sizeof(*session);
  session->identity.api_version = NXAUDIO_RUNTIME_API_VERSION;
  session->identity.struct_size = sizeof(session->identity);
  session->identity.run_id = run_id;
  session->identity.generation = generation;
  session->shutdown_status = NXAUDIO_SAFE_EXIT_PENDING;
  session->terminal_result = NXAUDIO_INVALID;
  session->terminal_reason = NXAUDIO_RUNTIME_REASON_NONE;
  nxaudio_backend_recovery_init(&session->recovery);
  store_u32(&session->state, (uint32_t)NXAUDIO_RUNTIME_STATE_NEW);
}

nxaudio_result nxaudio_runtime_open(
    nxaudio_runtime_session *session, const char *api, const char *backend,
    const nxaudio_format *requested, const nxaudio_format *granted,
    uint32_t now_ms, nxaudio_runtime_identity *identity) {
  nxaudio_reason format_reason = NXAUDIO_REASON_NONE;
  uint32_t state;
  if (!session_valid(session) || !identity || !name_valid(api) ||
      !name_valid(backend) || !format_valid(requested) ||
      !format_valid(granted))
    return NXAUDIO_INVALID;
  state = load_u32(&session->state);
  if (state != (uint32_t)NXAUDIO_RUNTIME_STATE_NEW) {
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_WRONG_STATE;
  }
  (void)nxaudio_classify_granted_format(requested, granted, &format_reason);
  memcpy(session->api, api, strlen(api) + 1u);
  memcpy(session->backend, backend, strlen(backend) + 1u);
  session->identity.api_tag = name_tag(api);
  session->identity.backend_tag = name_tag(backend);
  session->requested_format = *requested;
  session->granted_format = *granted;
  session->format_match_reason = format_reason;
  store_u32(&session->opened_ms, now_ms);
  store_u32(&session->last_callback_ms, now_ms);
  store_u32(&session->last_worker_ms, now_ms);
  if (!compare_u32(&session->state, &state,
                   (uint32_t)NXAUDIO_RUNTIME_STATE_OPENED)) {
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_WRONG_STATE;
  }
  *identity = session->identity;
  return NXAUDIO_OK;
}

nxaudio_result nxaudio_runtime_reopen(
    const nxaudio_runtime_session *previous,
    const nxaudio_runtime_identity *previous_identity,
    nxaudio_runtime_session *next,
    const char *api, const char *backend, const nxaudio_format *requested,
    const nxaudio_format *granted, uint32_t now_ms,
    nxaudio_runtime_identity *identity) {
  const uint32_t previous_state =
      session_valid(previous) ? load_u32(&previous->state) : UINT32_MAX;
  if (!identity_equal(previous, previous_identity) || !next ||
      next == previous ||
      (previous_state != (uint32_t)NXAUDIO_RUNTIME_STATE_CLOSED &&
       previous_state != (uint32_t)NXAUDIO_RUNTIME_STATE_FAILED) ||
      load_u32(&previous->formatted) != 2u ||
      previous->identity.generation == UINT32_MAX)
    return NXAUDIO_INVALID;
  nxaudio_runtime_init(next, previous->identity.run_id,
                       previous->identity.generation + 1u);
  return nxaudio_runtime_open(next, api, backend, requested, granted, now_ms,
                              identity);
}

nxaudio_result nxaudio_runtime_expect_callbacks(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t expected,
    uint32_t now_ms) {
  uint32_t unset = 0u;
  if (!identity_event(session, identity) || expected == 0u)
    return NXAUDIO_INVALID;
  if (!worker_time(session, now_ms))
    return NXAUDIO_INVALID;
  if (load_u32(&session->state) != (uint32_t)NXAUDIO_RUNTIME_STATE_OPENED ||
      !compare_u32(&session->callbacks_expected, &unset, expected)) {
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_WRONG_STATE;
  }
  return NXAUDIO_OK;
}

nxaudio_result nxaudio_runtime_set_live(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t now_ms) {
  uint32_t state;
  if (!identity_event(session, identity))
    return NXAUDIO_INVALID;
  if (!worker_time(session, now_ms))
    return NXAUDIO_INVALID;
  state = load_u32(&session->state);
  if (state == (uint32_t)NXAUDIO_RUNTIME_STATE_OPENED &&
      load_u32(&session->callbacks_expected) == 0u) {
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_WRONG_STATE;
  }
  if (state == (uint32_t)NXAUDIO_RUNTIME_STATE_RECOVERING &&
      session->recovery.outcome != NXAUDIO_RECOVERY_RECOVERED) {
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_WRONG_STATE;
  }
  if (state != (uint32_t)NXAUDIO_RUNTIME_STATE_OPENED &&
      state != (uint32_t)NXAUDIO_RUNTIME_STATE_PAUSED &&
      state != (uint32_t)NXAUDIO_RUNTIME_STATE_RECOVERING) {
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_WRONG_STATE;
  }
  /* Reset the liveness baseline before exposing LIVE.  A long legitimate
   * pause or worker-side recover must never look like a callback stall. */
  store_u32(&session->last_callback_ms, now_ms);
  if (!compare_u32(&session->state, &state,
                   (uint32_t)NXAUDIO_RUNTIME_STATE_LIVE)) {
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_WRONG_STATE;
  }
  return NXAUDIO_OK;
}

nxaudio_result nxaudio_runtime_set_paused(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t now_ms) {
  return transition(session, identity,
                    (uint32_t)NXAUDIO_RUNTIME_STATE_LIVE, UINT32_MAX,
                    (uint32_t)NXAUDIO_RUNTIME_STATE_PAUSED, now_ms);
}

nxaudio_result nxaudio_runtime_set_degraded(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, nxaudio_recovery_fault fault,
    uint32_t now_ms) {
  uint32_t state;
  if (!identity_event(session, identity))
    return NXAUDIO_INVALID;
  if (fault != NXAUDIO_RECOVERY_FAULT_XRUN_EPIPE &&
      fault != NXAUDIO_RECOVERY_FAULT_DEVICE_LOST &&
      fault != NXAUDIO_RECOVERY_FAULT_CALLBACK_STALLED)
    return NXAUDIO_INVALID;
  if (load_u32(&session->recovery_recorded) != 0u) {
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_UNSUPPORTED;
  }
  if (!worker_time(session, now_ms))
    return NXAUDIO_INVALID;
  state = load_u32(&session->state);
  if (state != (uint32_t)NXAUDIO_RUNTIME_STATE_LIVE) {
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_WRONG_STATE;
  }
  if (!claim_recovery_fault(session, fault))
    return NXAUDIO_WRONG_STATE;
  if (!compare_u32(&session->state, &state,
                   (uint32_t)NXAUDIO_RUNTIME_STATE_DEGRADED)) {
    release_recovery_fault(session, fault);
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_WRONG_STATE;
  }
  return NXAUDIO_OK;
}

static int recovery_coherent(const nxaudio_backend_recovery *recovery) {
  const int recover_valid =
      recovery->recover_status == NXAUDIO_RECOVERY_STEP_NOT_ATTEMPTED ||
      recovery->recover_status == NXAUDIO_RECOVERY_STEP_OK ||
      recovery->recover_status == NXAUDIO_RECOVERY_STEP_FAILED;
  const int reopen_valid =
      recovery->reopen_status == NXAUDIO_RECOVERY_STEP_NOT_ATTEMPTED ||
      recovery->reopen_status == NXAUDIO_RECOVERY_STEP_OK ||
      recovery->reopen_status == NXAUDIO_RECOVERY_STEP_FAILED;
  if (!recover_valid || !reopen_valid)
    return 0;
  if (recovery->outcome == NXAUDIO_RECOVERY_RECOVERED)
    return recovery->recover_status == NXAUDIO_RECOVERY_STEP_OK &&
           recovery->reopen_status == NXAUDIO_RECOVERY_STEP_NOT_ATTEMPTED;
  if (recovery->outcome == NXAUDIO_RECOVERY_REOPENED)
    return recovery->recover_status != NXAUDIO_RECOVERY_STEP_OK &&
           recovery->reopen_status == NXAUDIO_RECOVERY_STEP_OK;
  if (recovery->outcome == NXAUDIO_RECOVERY_FAILED)
    return recovery->recover_status != NXAUDIO_RECOVERY_STEP_OK &&
           recovery->reopen_status != NXAUDIO_RECOVERY_STEP_OK &&
           (recovery->recover_status == NXAUDIO_RECOVERY_STEP_FAILED ||
            recovery->reopen_status == NXAUDIO_RECOVERY_STEP_FAILED);
  if (recovery->outcome == NXAUDIO_RECOVERY_EXHAUSTED)
    return recovery->recover_status == NXAUDIO_RECOVERY_STEP_NOT_ATTEMPTED &&
           recovery->reopen_status == NXAUDIO_RECOVERY_STEP_NOT_ATTEMPTED;
  return 0;
}

nxaudio_result nxaudio_runtime_set_recovering(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t now_ms) {
  nxaudio_result result;
  nxaudio_recovery_fault fault;
  if (!session_valid(session))
    return NXAUDIO_INVALID;
  fault = (nxaudio_recovery_fault)load_u32(&session->recovery_fault);
  if (fault == NXAUDIO_RECOVERY_FAULT_NONE)
    return NXAUDIO_INVALID;
  if (load_u32(&session->recovery_recorded) != 0u) {
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_UNSUPPORTED;
  }
  result = transition(session, identity,
                      (uint32_t)NXAUDIO_RUNTIME_STATE_DEGRADED, UINT32_MAX,
                      (uint32_t)NXAUDIO_RUNTIME_STATE_RECOVERING, now_ms);
  if (result == NXAUDIO_OK) {
    nxaudio_backend_recovery_init(&session->recovery);
    session->recovery.fault = fault;
    session->recovery.outcome = NXAUDIO_RECOVERY_PENDING;
  }
  return result;
}

nxaudio_result nxaudio_runtime_record_recovery(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity,
    const nxaudio_backend_recovery *recovery, uint32_t now_ms) {
  uint32_t unrecorded = 0u;
  if (!identity_event(session, identity) || !recovery ||
      recovery->api_version != NXAUDIO_RECEIPT_API_VERSION ||
      recovery->struct_size < sizeof(*recovery) || recovery->attempts != 1u ||
      recovery->fault !=
          (nxaudio_recovery_fault)load_u32(&session->recovery_fault) ||
      load_u32(&session->state) !=
          (uint32_t)NXAUDIO_RUNTIME_STATE_RECOVERING ||
      !recovery_coherent(recovery))
    return NXAUDIO_INVALID;
  if (!worker_time(session, now_ms))
    return NXAUDIO_INVALID;
  if (!compare_u32(&session->recovery_recorded, &unrecorded, 1u)) {
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_WRONG_STATE;
  }
  session->recovery = *recovery;
  if (recovery->outcome == NXAUDIO_RECOVERY_REOPENED ||
      recovery->outcome == NXAUDIO_RECOVERY_FAILED ||
      recovery->outcome == NXAUDIO_RECOVERY_EXHAUSTED) {
    uint32_t expected = (uint32_t)NXAUDIO_RUNTIME_STATE_RECOVERING;
    session->terminal_reason =
        recovery->outcome == NXAUDIO_RECOVERY_REOPENED
            ? NXAUDIO_RUNTIME_REASON_REOPEN_REQUIRED
            : NXAUDIO_RUNTIME_REASON_RECOVERY_FAILED;
    session->terminal_result = NXAUDIO_UNSUPPORTED;
    if (!compare_u32(&session->state, &expected,
                     (uint32_t)NXAUDIO_RUNTIME_STATE_FAILED)) {
      store_u32(&session->order_violation, 1u);
      return NXAUDIO_WRONG_STATE;
    }
    return NXAUDIO_UNSUPPORTED;
  }
  return NXAUDIO_OK;
}

static int failure_reason_valid(nxaudio_runtime_terminal_reason reason) {
  return reason == NXAUDIO_RUNTIME_REASON_CALLBACK_MISSING ||
         reason == NXAUDIO_RUNTIME_REASON_EXPECTED_MISSING ||
         reason == NXAUDIO_RUNTIME_REASON_PCM_ZERO ||
         reason == NXAUDIO_RUNTIME_REASON_IDENTITY_MISMATCH ||
         reason == NXAUDIO_RUNTIME_REASON_PRODUCER_MISMATCH ||
         reason == NXAUDIO_RUNTIME_REASON_TIME_INVALID ||
         reason == NXAUDIO_RUNTIME_REASON_EVENT_ORDER ||
         reason == NXAUDIO_RUNTIME_REASON_COUNTER_OVERFLOW ||
         reason == NXAUDIO_RUNTIME_REASON_STATE_FAILED ||
         reason == NXAUDIO_RUNTIME_REASON_FORMAT_MISMATCH;
}

nxaudio_result nxaudio_runtime_set_failed(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity,
    nxaudio_runtime_terminal_reason reason, uint32_t now_ms) {
  uint32_t state;
  if (!identity_event(session, identity) || !failure_reason_valid(reason))
    return NXAUDIO_INVALID;
  if (!worker_time(session, now_ms))
    return NXAUDIO_INVALID;
  state = load_u32(&session->state);
  if (state == (uint32_t)NXAUDIO_RUNTIME_STATE_FAILED ||
      state == (uint32_t)NXAUDIO_RUNTIME_STATE_CLOSED ||
      state == (uint32_t)NXAUDIO_RUNTIME_STATE_DRAINING) {
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_WRONG_STATE;
  }
  if (!compare_u32(&session->state, &state,
                   (uint32_t)NXAUDIO_RUNTIME_STATE_FAILED)) {
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_WRONG_STATE;
  }
  session->terminal_reason = reason;
  session->terminal_result = NXAUDIO_UNSUPPORTED;
  return NXAUDIO_OK;
}

nxaudio_result nxaudio_runtime_set_draining(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t now_ms) {
  uint32_t state;
  if (!identity_event(session, identity))
    return NXAUDIO_INVALID;
  if (!worker_time(session, now_ms))
    return NXAUDIO_INVALID;
  state = load_u32(&session->state);
  if (state == (uint32_t)NXAUDIO_RUNTIME_STATE_LIVE ||
      state == (uint32_t)NXAUDIO_RUNTIME_STATE_PAUSED) {
    if (!compare_u32(&session->state, &state,
                     (uint32_t)NXAUDIO_RUNTIME_STATE_DRAINING)) {
      store_u32(&session->order_violation, 1u);
      return NXAUDIO_WRONG_STATE;
    }
  } else if (state != (uint32_t)NXAUDIO_RUNTIME_STATE_DRAINING) {
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_WRONG_STATE;
  }
  /* DRAINING stops new callback entries.  An already-entered callback may
   * finish, after which the worker retries this bounded quiescence query. */
  return load_u32(&session->callback_active) == 0u ? NXAUDIO_OK
                                                   : NXAUDIO_FULL;
}

nxaudio_result nxaudio_runtime_set_closed(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity,
    nxaudio_safe_exit_status shutdown_status, uint32_t now_ms) {
  uint32_t state;
  if (!identity_event(session, identity))
    return NXAUDIO_INVALID;
  if (shutdown_status != NXAUDIO_SAFE_EXIT_CONFIRMED &&
      shutdown_status != NXAUDIO_SAFE_EXIT_TIMEOUT)
    return NXAUDIO_INVALID;
  if (!worker_time(session, now_ms))
    return NXAUDIO_INVALID;
  state = load_u32(&session->state);
  if (state != (uint32_t)NXAUDIO_RUNTIME_STATE_DRAINING) {
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_WRONG_STATE;
  }
  if (load_u32(&session->callback_active) != 0u)
    return NXAUDIO_FULL;
  session->shutdown_status = shutdown_status;
  if (shutdown_status == NXAUDIO_SAFE_EXIT_TIMEOUT) {
    session->terminal_result = NXAUDIO_UNSUPPORTED;
    session->terminal_reason = NXAUDIO_RUNTIME_REASON_SHUTDOWN_TIMEOUT;
    if (!compare_u32(&session->state, &state,
                     (uint32_t)NXAUDIO_RUNTIME_STATE_FAILED)) {
      store_u32(&session->order_violation, 1u);
      return NXAUDIO_WRONG_STATE;
    }
    return NXAUDIO_UNSUPPORTED;
  }
  store_u32(&session->closed_ms, now_ms);
  if (!compare_u32(&session->state, &state,
                   (uint32_t)NXAUDIO_RUNTIME_STATE_CLOSED)) {
    store_u32(&session->order_violation, 1u);
    return NXAUDIO_WRONG_STATE;
  }
  return NXAUDIO_OK;
}

nxaudio_result nxaudio_runtime_update_callback(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t producer_id,
    uint32_t now_ms, uint32_t callbacks, uint32_t frames_delivered,
    uint32_t bytes_delivered, uint32_t nonzero_samples, double peak_abs) {
  uint64_t sample_capacity;
  uint32_t peak_milli;
  nxaudio_result entered =
      callback_enter(session, identity, producer_id, 1, now_ms);
  if (entered != NXAUDIO_OK)
    return entered;
  sample_capacity = (uint64_t)frames_delivered *
                    (uint64_t)session->granted_format.channels;
  if (callbacks == 0u || frames_delivered == 0u ||
      nonzero_samples > sample_capacity || !(peak_abs >= 0.0) ||
      !(peak_abs <= 1.0) ||
      ((nonzero_samples == 0u) != (peak_abs == 0.0))) {
    store_u32(&session->order_violation, 1u);
    callback_leave(session);
    return NXAUDIO_INVALID;
  }
  peak_milli = (uint32_t)(peak_abs * 1000.0 + 0.5);
  if (!saturating_add(session, &session->callbacks_observed, callbacks) ||
      !saturating_add(session, &session->frames_delivered, frames_delivered) ||
      !saturating_add(session, &session->bytes_delivered, bytes_delivered) ||
      !saturating_add(session, &session->nonzero_samples, nonzero_samples) ||
      !peak_update(session, &session->peak_milli, peak_milli)) {
    callback_leave(session);
    return NXAUDIO_INVALID;
  }
  callback_leave(session);
  return NXAUDIO_OK;
}

nxaudio_result nxaudio_runtime_report_underrun(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t producer_id) {
  nxaudio_result entered =
      callback_enter(session, identity, producer_id, 0, 0u);
  if (entered != NXAUDIO_OK)
    return entered;
  if (!saturating_add(session, &session->underrun_count, 1u)) {
    callback_leave(session);
    return NXAUDIO_INVALID;
  }
  callback_leave(session);
  return NXAUDIO_OK;
}

nxaudio_result nxaudio_runtime_report_silence(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t producer_id,
    uint32_t silence_frames) {
  nxaudio_result entered;
  if (silence_frames == 0u)
    return NXAUDIO_INVALID;
  entered = callback_enter(session, identity, producer_id, 0, 0u);
  if (entered != NXAUDIO_OK)
    return entered;
  if (!saturating_add(session, &session->silence_frames, silence_frames)) {
    callback_leave(session);
    return NXAUDIO_INVALID;
  }
  callback_leave(session);
  return NXAUDIO_OK;
}

nxaudio_result nxaudio_runtime_report_device_lost(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, uint32_t producer_id,
  uint32_t now_ms) {
  uint32_t state = (uint32_t)NXAUDIO_RUNTIME_STATE_LIVE;
  nxaudio_result entered =
      callback_enter(session, identity, producer_id, 1, now_ms);
  if (entered != NXAUDIO_OK)
    return entered;
  if (load_u32(&session->recovery_recorded) != 0u) {
    store_u32(&session->order_violation, 1u);
    callback_leave(session);
    return NXAUDIO_UNSUPPORTED;
  }
  if (!claim_recovery_fault(session, NXAUDIO_RECOVERY_FAULT_DEVICE_LOST)) {
    callback_leave(session);
    return NXAUDIO_WRONG_STATE;
  }
  if (!compare_u32(&session->state, &state,
                   (uint32_t)NXAUDIO_RUNTIME_STATE_DEGRADED)) {
    release_recovery_fault(session, NXAUDIO_RECOVERY_FAULT_DEVICE_LOST);
    store_u32(&session->order_violation, 1u);
    callback_leave(session);
    return NXAUDIO_WRONG_STATE;
  }
  if (!saturating_add(session, &session->device_loss_count, 1u)) {
    callback_leave(session);
    return NXAUDIO_INVALID;
  }
  callback_leave(session);
  return NXAUDIO_OK;
}

int nxaudio_runtime_is_stalled(nxaudio_runtime_session *session,
                               const nxaudio_runtime_identity *identity,
                               uint32_t now_ms, uint32_t budget_ms) {
  uint32_t state;
  uint32_t last;
  if (!identity_event(session, identity) || budget_ms == 0u)
    return -1;
  if (!worker_time(session, now_ms))
    return -1;
  state = load_u32(&session->state);
  if (state == (uint32_t)NXAUDIO_RUNTIME_STATE_PAUSED)
    return 0;
  if (state != (uint32_t)NXAUDIO_RUNTIME_STATE_LIVE)
    return 0;
  last = load_u32(&session->last_callback_ms);
  if (now_ms < last) {
    store_u32(&session->time_violation, 1u);
    return -1;
  }
  return now_ms - last > budget_ms ? 1 : 0;
}

nxaudio_runtime_state nxaudio_runtime_get_state(
    const nxaudio_runtime_session *session) {
  if (!session_valid(session))
    return NXAUDIO_RUNTIME_STATE_FAILED;
  return (nxaudio_runtime_state)load_u32(&session->state);
}

nxaudio_result nxaudio_runtime_get_snapshot(
    const nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity,
    nxaudio_runtime_snapshot *snapshot) {
  uint32_t callback_epoch;
  if (!identity_equal(session, identity) || !snapshot)
    return NXAUDIO_INVALID;
  if (load_u32(&session->callback_active) != 0u)
    return NXAUDIO_FULL;
  callback_epoch = load_u32(&session->callback_epoch);
  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->api_version = NXAUDIO_RUNTIME_API_VERSION;
  snapshot->struct_size = sizeof(*snapshot);
  snapshot->identity = session->identity;
  snapshot->state = (nxaudio_runtime_state)load_u32(&session->state);
  snapshot->requested_format = session->requested_format;
  snapshot->granted_format = session->granted_format;
  snapshot->format_match_reason = session->format_match_reason;
  snapshot->callbacks_expected = load_u32(&session->callbacks_expected);
  snapshot->callbacks_observed = load_u32(&session->callbacks_observed);
  snapshot->frames_delivered = load_u32(&session->frames_delivered);
  snapshot->bytes_delivered = load_u32(&session->bytes_delivered);
  snapshot->nonzero_samples = load_u32(&session->nonzero_samples);
  snapshot->peak_milli = load_u32(&session->peak_milli);
  snapshot->underrun_count = load_u32(&session->underrun_count);
  snapshot->silence_frames = load_u32(&session->silence_frames);
  snapshot->device_loss_count = load_u32(&session->device_loss_count);
  snapshot->opened_ms = load_u32(&session->opened_ms);
  snapshot->last_callback_ms = load_u32(&session->last_callback_ms);
  snapshot->closed_ms = load_u32(&session->closed_ms);
  snapshot->callback_epoch = callback_epoch;
  snapshot->recovery_recorded = load_u32(&session->recovery_recorded);
  snapshot->overflowed = load_u32(&session->overflowed);
  snapshot->identity_violation = load_u32(&session->identity_violation);
  snapshot->producer_violation = load_u32(&session->producer_violation);
  snapshot->time_violation = load_u32(&session->time_violation);
  snapshot->order_violation = load_u32(&session->order_violation);
  snapshot->recovery = session->recovery;
  snapshot->recovery.fault =
      (nxaudio_recovery_fault)load_u32(&session->recovery_fault);
  snapshot->shutdown_status = session->shutdown_status;
  snapshot->terminal_result = session->terminal_result;
  snapshot->terminal_reason = session->terminal_reason;
  snapshot->formatted = load_u32(&session->formatted) == 2u ? 1u : 0u;
  if (load_u32(&session->callback_active) != 0u ||
      load_u32(&session->callback_epoch) != callback_epoch) {
    memset(snapshot, 0, sizeof(*snapshot));
    return NXAUDIO_FULL;
  }
  return NXAUDIO_OK;
}

static const char *sample_name(nxaudio_sample_format format) {
  if (format == NXAUDIO_SAMPLE_S16LE)
    return "s16le";
  if (format == NXAUDIO_SAMPLE_F32LE)
    return "f32le";
  return "invalid";
}

static const char *state_name(nxaudio_runtime_state state) {
  static const char *const names[] = {
      "NEW", "OPENED", "LIVE", "PAUSED", "DEGRADED",
      "RECOVERING", "FAILED", "DRAINING", "CLOSED"};
  return (unsigned int)state < sizeof(names) / sizeof(names[0])
             ? names[(unsigned int)state]
             : "INVALID";
}

static const char *recovery_name(nxaudio_recovery_outcome outcome,
                                 nxaudio_recovery_fault fault) {
  if (fault == NXAUDIO_RECOVERY_FAULT_NONE)
    return "none";
  switch (outcome) {
    case NXAUDIO_RECOVERY_PENDING:
      return "pending";
    case NXAUDIO_RECOVERY_RECOVERED:
      return "recovered";
    case NXAUDIO_RECOVERY_REOPENED:
      return "reopened";
    case NXAUDIO_RECOVERY_FAILED:
      return "failed";
    case NXAUDIO_RECOVERY_EXHAUSTED:
      return "exhausted";
    default:
      return "invalid";
  }
}

static const char *recovery_fault_name(nxaudio_recovery_fault fault) {
  if (fault == NXAUDIO_RECOVERY_FAULT_NONE)
    return "none";
  if (fault == NXAUDIO_RECOVERY_FAULT_XRUN_EPIPE)
    return "xrun-epipe";
  if (fault == NXAUDIO_RECOVERY_FAULT_DEVICE_LOST)
    return "device-lost";
  if (fault == NXAUDIO_RECOVERY_FAULT_CALLBACK_STALLED)
    return "callback-stalled";
  return "invalid";
}

static const char *shutdown_name(nxaudio_safe_exit_status status) {
  if (status == NXAUDIO_SAFE_EXIT_CONFIRMED)
    return "confirmed";
  if (status == NXAUDIO_SAFE_EXIT_TIMEOUT)
    return "timeout";
  return "pending";
}

const char *nxaudio_runtime_reason_name(
    nxaudio_runtime_terminal_reason reason) {
  static const char *const names[] = {
      "none",
      "coherent-fixture",
      "not-terminal",
      "callback-missing",
      "expected-missing",
      "pcm-zero",
      "shutdown-timeout",
      "recovery-pending",
      "recovery-failed",
      "identity-mismatch",
      "producer-mismatch",
      "time-invalid",
      "event-order",
      "counter-overflow",
      "state-failed",
      "reopen-required",
      "format-mismatch",
      "already-formatted"};
  return (unsigned int)reason < sizeof(names) / sizeof(names[0])
             ? names[(unsigned int)reason]
             : "invalid";
}

static nxaudio_runtime_terminal_reason terminal_reason(
    const nxaudio_runtime_session *session,
    const nxaudio_runtime_snapshot *snapshot) {
  if (snapshot->identity_violation)
    return NXAUDIO_RUNTIME_REASON_IDENTITY_MISMATCH;
  if (snapshot->producer_violation)
    return NXAUDIO_RUNTIME_REASON_PRODUCER_MISMATCH;
  if (snapshot->time_violation)
    return NXAUDIO_RUNTIME_REASON_TIME_INVALID;
  if (snapshot->order_violation)
    return NXAUDIO_RUNTIME_REASON_EVENT_ORDER;
  if (snapshot->overflowed)
    return NXAUDIO_RUNTIME_REASON_COUNTER_OVERFLOW;
  if (snapshot->state == NXAUDIO_RUNTIME_STATE_FAILED)
    return session->terminal_reason == NXAUDIO_RUNTIME_REASON_NONE
               ? NXAUDIO_RUNTIME_REASON_STATE_FAILED
               : session->terminal_reason;
  if (snapshot->state != NXAUDIO_RUNTIME_STATE_CLOSED)
    return NXAUDIO_RUNTIME_REASON_NOT_TERMINAL;
  if (snapshot->shutdown_status == NXAUDIO_SAFE_EXIT_TIMEOUT)
    return NXAUDIO_RUNTIME_REASON_SHUTDOWN_TIMEOUT;
  if (snapshot->shutdown_status != NXAUDIO_SAFE_EXIT_CONFIRMED)
    return NXAUDIO_RUNTIME_REASON_NOT_TERMINAL;
  if (snapshot->recovery.fault != NXAUDIO_RECOVERY_FAULT_NONE &&
      (!snapshot->recovery_recorded ||
       snapshot->recovery.outcome == NXAUDIO_RECOVERY_PENDING))
    return NXAUDIO_RUNTIME_REASON_RECOVERY_PENDING;
  if (snapshot->recovery.outcome == NXAUDIO_RECOVERY_FAILED ||
      snapshot->recovery.outcome == NXAUDIO_RECOVERY_EXHAUSTED)
    return NXAUDIO_RUNTIME_REASON_RECOVERY_FAILED;
  if (snapshot->format_match_reason != NXAUDIO_REASON_FORMAT_MATCH)
    return NXAUDIO_RUNTIME_REASON_FORMAT_MISMATCH;
  if (snapshot->callbacks_expected == 0u)
    return NXAUDIO_RUNTIME_REASON_EXPECTED_MISSING;
  if (snapshot->callbacks_observed == 0u ||
      snapshot->callbacks_observed < snapshot->callbacks_expected)
    return NXAUDIO_RUNTIME_REASON_CALLBACK_MISSING;
  if (snapshot->frames_delivered == 0u || snapshot->bytes_delivered == 0u ||
      snapshot->nonzero_samples == 0u || snapshot->peak_milli == 0u)
    return NXAUDIO_RUNTIME_REASON_PCM_ZERO;
  return NXAUDIO_RUNTIME_REASON_COHERENT_FIXTURE;
}

nxaudio_result nxaudio_runtime_format_receipt(
    nxaudio_runtime_session *session,
    const nxaudio_runtime_identity *identity, char *line, size_t line_size) {
  nxaudio_runtime_snapshot snapshot;
  nxaudio_runtime_terminal_reason reason;
  const char *result;
  char rendered[NXAUDIO_RUNTIME_RECEIPT_LINE_MAX];
  int written;
  uint32_t unformatted = 0u;
  nxaudio_result snapshot_result;

  if (line && line_size != 0u)
    line[0] = '\0';
  if (!line || line_size == 0u || !identity_event(session, identity))
    return NXAUDIO_INVALID;
  if (load_u32(&session->formatted) != 0u)
    return NXAUDIO_INVALID;
  snapshot_result = nxaudio_runtime_get_snapshot(session, identity, &snapshot);
  if (snapshot_result != NXAUDIO_OK)
    return snapshot_result;
  if (snapshot.state != NXAUDIO_RUNTIME_STATE_CLOSED &&
      snapshot.state != NXAUDIO_RUNTIME_STATE_FAILED)
    return NXAUDIO_WRONG_STATE;

  reason = terminal_reason(session, &snapshot);
  result = reason == NXAUDIO_RUNTIME_REASON_COHERENT_FIXTURE ? "PASS" : "FAIL";
  written = snprintf(
      rendered, sizeof(rendered),
      "AUDIO-RUNTIME-RECEIPT: schema=nxaudio-runtime-v2 class=FIXTURE "
      "physical=0 human=0 run=%u gen=%u api=%s backend=%s "
      "req=%uHz/%uch/%s got=%uHz/%uch/%s format=%s state=%s "
      "callbacks=%u/%u frames=%u bytes=%u nonzero=%u peak_milli=%u "
      "underruns=%u silence=%u devlost=%u fault=%s recovery=%s attempts=%u "
      "shutdown=%s result=%s reason=%s",
      snapshot.identity.run_id, snapshot.identity.generation, session->api,
      session->backend, snapshot.requested_format.frequency,
      (unsigned int)snapshot.requested_format.channels,
      sample_name(snapshot.requested_format.sample_format),
      snapshot.granted_format.frequency,
      (unsigned int)snapshot.granted_format.channels,
      sample_name(snapshot.granted_format.sample_format),
      nxaudio_reason_name(snapshot.format_match_reason), state_name(snapshot.state),
      snapshot.callbacks_observed, snapshot.callbacks_expected,
      snapshot.frames_delivered, snapshot.bytes_delivered,
      snapshot.nonzero_samples, snapshot.peak_milli,
      snapshot.underrun_count, snapshot.silence_frames,
      snapshot.device_loss_count, recovery_fault_name(snapshot.recovery.fault),
      recovery_name(snapshot.recovery.outcome, snapshot.recovery.fault),
      snapshot.recovery.attempts, shutdown_name(snapshot.shutdown_status),
      result, nxaudio_runtime_reason_name(reason));
  if (written < 0 || (size_t)written >= sizeof(rendered))
    return NXAUDIO_FULL;
  if ((size_t)written >= line_size)
    return NXAUDIO_FULL;
  if (!compare_u32(&session->formatted, &unformatted, 1u))
    return NXAUDIO_INVALID;

  session->terminal_reason = reason;
  session->terminal_result =
      reason == NXAUDIO_RUNTIME_REASON_COHERENT_FIXTURE ? NXAUDIO_OK
                                                       : NXAUDIO_UNSUPPORTED;
  memcpy(line, rendered, (size_t)written + 1u);
  store_u32(&session->formatted, 2u);
  return NXAUDIO_OK;
}
