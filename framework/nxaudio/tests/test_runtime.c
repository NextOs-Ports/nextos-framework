/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxaudio_runtime.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);    \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

static const nxaudio_format k_format = {
    48000u, NXAUDIO_SAMPLE_S16LE, 2u, 256u, 10000u};

static void open_session(nxaudio_runtime_session *session,
                         nxaudio_runtime_identity *identity, uint32_t run_id,
                         uint32_t generation, uint32_t now_ms) {
  nxaudio_runtime_init(session, run_id, generation);
  CHECK(nxaudio_runtime_open(session, "fixture-api", "fixture-backend",
                             &k_format, &k_format, now_ms, identity) ==
        NXAUDIO_OK);
  CHECK(nxaudio_runtime_expect_callbacks(session, identity, 4u, now_ms) ==
        NXAUDIO_OK);
  CHECK(identity->run_id == run_id);
  CHECK(identity->generation == generation);
  CHECK(identity->api_tag != 0u);
  CHECK(identity->backend_tag != 0u);
}

static void make_live(nxaudio_runtime_session *session,
                      nxaudio_runtime_identity *identity, uint32_t now_ms) {
  CHECK(nxaudio_runtime_set_live(session, identity, now_ms) == NXAUDIO_OK);
  CHECK(nxaudio_runtime_get_state(session) == NXAUDIO_RUNTIME_STATE_LIVE);
}

static void add_live_pcm(nxaudio_runtime_session *session,
                         nxaudio_runtime_identity *identity,
                         uint32_t producer_id, uint32_t now_ms) {
  CHECK(nxaudio_runtime_update_callback(session, identity, producer_id, now_ms,
                                        4u, 128u, 512u, 64u, 0.5) ==
        NXAUDIO_OK);
}

static void close_session(nxaudio_runtime_session *session,
                          nxaudio_runtime_identity *identity,
                          uint32_t draining_ms, uint32_t closed_ms) {
  CHECK(nxaudio_runtime_set_draining(session, identity, draining_ms) ==
        NXAUDIO_OK);
  CHECK(nxaudio_runtime_set_closed(session, identity,
                                   NXAUDIO_SAFE_EXIT_CONFIRMED, closed_ms) ==
        NXAUDIO_OK);
  CHECK(nxaudio_runtime_get_state(session) == NXAUDIO_RUNTIME_STATE_CLOSED);
}

static nxaudio_backend_recovery recovery_result(
    nxaudio_recovery_fault fault, nxaudio_recovery_outcome outcome) {
  nxaudio_backend_recovery recovery;
  nxaudio_backend_recovery_init(&recovery);
  recovery.attempts = 1u;
  recovery.fault = fault;
  if (outcome == NXAUDIO_RECOVERY_RECOVERED) {
    recovery.recover_status = NXAUDIO_RECOVERY_STEP_OK;
    recovery.reopen_status = NXAUDIO_RECOVERY_STEP_NOT_ATTEMPTED;
  } else if (outcome == NXAUDIO_RECOVERY_REOPENED) {
    recovery.recover_status = NXAUDIO_RECOVERY_STEP_FAILED;
    recovery.reopen_status = NXAUDIO_RECOVERY_STEP_OK;
  } else if (outcome == NXAUDIO_RECOVERY_EXHAUSTED) {
    recovery.recover_status = NXAUDIO_RECOVERY_STEP_NOT_ATTEMPTED;
    recovery.reopen_status = NXAUDIO_RECOVERY_STEP_NOT_ATTEMPTED;
  } else {
    recovery.recover_status = NXAUDIO_RECOVERY_STEP_FAILED;
    recovery.reopen_status = NXAUDIO_RECOVERY_STEP_FAILED;
  }
  recovery.outcome = outcome;
  return recovery;
}

static void test_open_validation_and_state_matrix(void) {
  nxaudio_runtime_session session;
  nxaudio_runtime_identity identity;
  nxaudio_runtime_identity initial_identity;
  nxaudio_format invalid_format = k_format;
  char hostile[NXAUDIO_NAME_MAX];

  nxaudio_runtime_init(&session, 0u, 1u);
  CHECK(nxaudio_runtime_open(&session, "fixture-api", "fixture-backend",
                             &k_format, &k_format, 1u, &identity) ==
        NXAUDIO_INVALID);
  nxaudio_runtime_init(&session, 1u, 0u);
  CHECK(nxaudio_runtime_open(&session, "fixture-api", "fixture-backend",
                             &k_format, &k_format, 1u, &identity) ==
        NXAUDIO_INVALID);

  nxaudio_runtime_init(&session, 1u, 1u);
  session.api_version = 0u;
  CHECK(nxaudio_runtime_open(&session, "fixture-api", "fixture-backend",
                             &k_format, &k_format, 1u, &identity) ==
        NXAUDIO_INVALID);
  nxaudio_runtime_init(&session, 1u, 1u);
  session.struct_size = sizeof(session) - 1u;
  CHECK(nxaudio_runtime_open(&session, "fixture-api", "fixture-backend",
                             &k_format, &k_format, 1u, &identity) ==
        NXAUDIO_INVALID);

  nxaudio_runtime_init(&session, 1u, 1u);
  memset(hostile, 'x', sizeof(hostile));
  CHECK(nxaudio_runtime_open(&session, hostile, "fixture-backend", &k_format,
                             &k_format, 1u, &identity) == NXAUDIO_INVALID);
  CHECK(nxaudio_runtime_open(&session, "bad name", "fixture-backend",
                             &k_format, &k_format, 1u, &identity) ==
        NXAUDIO_INVALID);
  CHECK(nxaudio_runtime_open(&session, "bad=name", "fixture-backend",
                             &k_format, &k_format, 1u, &identity) ==
        NXAUDIO_INVALID);
  CHECK(nxaudio_runtime_open(&session, "1not-a-name", "fixture-backend",
                             &k_format, &k_format, 1u, &identity) ==
        NXAUDIO_INVALID);
  CHECK(nxaudio_runtime_open(&session, "bad/path", "fixture-backend",
                             &k_format, &k_format, 1u, &identity) ==
        NXAUDIO_INVALID);
  invalid_format.frequency = 0u;
  CHECK(nxaudio_runtime_open(&session, "fixture-api", "fixture-backend",
                             &invalid_format, &k_format, 1u, &identity) ==
        NXAUDIO_INVALID);

  nxaudio_runtime_init(&session, 2u, 1u);
  initial_identity = session.identity;
  CHECK(nxaudio_runtime_set_live(&session, &initial_identity, 1u) ==
        NXAUDIO_WRONG_STATE);

  open_session(&session, &identity, 3u, 1u, 10u);
  CHECK(nxaudio_runtime_expect_callbacks(&session, &identity, 0u, 10u) ==
        NXAUDIO_INVALID);
  CHECK(nxaudio_runtime_expect_callbacks(&session, &identity, 1u, 10u) ==
        NXAUDIO_WRONG_STATE);
  CHECK(nxaudio_runtime_set_paused(&session, &identity, 11u) ==
        NXAUDIO_WRONG_STATE);

  open_session(&session, &identity, 4u, 1u, 10u);
  {
    nxaudio_runtime_identity malformed = identity;
    malformed.api_version = 0u;
    CHECK(nxaudio_runtime_set_live(&session, &malformed, 10u) ==
          NXAUDIO_INVALID);
    malformed = identity;
    malformed.struct_size = sizeof(malformed) - 1u;
    CHECK(nxaudio_runtime_set_live(&session, &malformed, 10u) ==
          NXAUDIO_INVALID);
  }
  CHECK(nxaudio_runtime_set_failed(
            &session, &identity, NXAUDIO_RUNTIME_REASON_COHERENT_FIXTURE,
            10u) == NXAUDIO_INVALID);
  make_live(&session, &identity, 11u);
  CHECK(nxaudio_runtime_set_recovering(&session, &identity, 12u) ==
        NXAUDIO_INVALID);

  open_session(&session, &identity, 5u, 1u, 10u);
  make_live(&session, &identity, 11u);
  CHECK(nxaudio_runtime_set_paused(&session, &identity, 12u) == NXAUDIO_OK);
  CHECK(nxaudio_runtime_set_degraded(
            &session, &identity, NXAUDIO_RECOVERY_FAULT_CALLBACK_STALLED,
            13u) == NXAUDIO_WRONG_STATE);

  open_session(&session, &identity, 6u, 1u, 10u);
  make_live(&session, &identity, 11u);
  CHECK(nxaudio_runtime_set_degraded(
            &session, &identity, NXAUDIO_RECOVERY_FAULT_CALLBACK_STALLED,
            12u) == NXAUDIO_OK);
  CHECK(nxaudio_runtime_set_paused(&session, &identity, 13u) ==
        NXAUDIO_WRONG_STATE);
}

static void test_valid_flow_and_exact_fixture_receipt(void) {
  static const char expected[] =
      "AUDIO-RUNTIME-RECEIPT: schema=nxaudio-runtime-v2 class=FIXTURE "
      "physical=0 human=0 run=42 gen=7 api=fixture-api "
      "backend=fixture-backend req=48000Hz/2ch/s16le "
      "got=48000Hz/2ch/s16le format=format-match state=CLOSED "
      "callbacks=4/4 frames=128 bytes=512 nonzero=64 peak_milli=500 "
      "underruns=0 silence=0 devlost=0 fault=none recovery=none attempts=0 "
      "shutdown=confirmed result=PASS reason=coherent-fixture";
  nxaudio_runtime_session session;
  nxaudio_runtime_identity identity;
  nxaudio_runtime_snapshot snapshot;
  char line[NXAUDIO_RUNTIME_RECEIPT_LINE_MAX];

  open_session(&session, &identity, 42u, 7u, 100u);
  make_live(&session, &identity, 101u);
  add_live_pcm(&session, &identity, 9u, 102u);
  close_session(&session, &identity, 103u, 104u);
  CHECK(nxaudio_runtime_format_receipt(&session, &identity, line,
                                       sizeof(line)) == NXAUDIO_OK);
  CHECK(strcmp(line, expected) == 0);
  CHECK(strchr(line, '\n') == NULL);
  CHECK(strstr(line, "class=PHYSICAL") == NULL);
  CHECK(strstr(line, "audible-confirmed") == NULL);
  CHECK(nxaudio_runtime_get_snapshot(&session, &identity, &snapshot) ==
        NXAUDIO_OK);
  CHECK(snapshot.formatted == 1u);
  CHECK(snapshot.terminal_result == NXAUDIO_OK);
  CHECK(snapshot.terminal_reason == NXAUDIO_RUNTIME_REASON_COHERENT_FIXTURE);
  CHECK(nxaudio_runtime_format_receipt(&session, &identity, line,
                                       sizeof(line)) == NXAUDIO_INVALID);
}

static void test_short_buffer_is_retryable(void) {
  nxaudio_runtime_session session;
  nxaudio_runtime_identity identity;
  char short_line[16];
  char line[NXAUDIO_RUNTIME_RECEIPT_LINE_MAX];

  open_session(&session, &identity, 43u, 1u, 10u);
  make_live(&session, &identity, 11u);
  add_live_pcm(&session, &identity, 1u, 12u);
  close_session(&session, &identity, 13u, 14u);
  memset(short_line, 'x', sizeof(short_line));
  CHECK(nxaudio_runtime_format_receipt(&session, &identity, short_line,
                                       sizeof(short_line)) == NXAUDIO_FULL);
  CHECK(short_line[0] == '\0');
  CHECK(nxaudio_runtime_format_receipt(&session, &identity, line,
                                       sizeof(line)) == NXAUDIO_OK);
}

static void test_identity_stale_and_reopen(void) {
  nxaudio_runtime_session previous;
  nxaudio_runtime_session next;
  nxaudio_runtime_identity old_identity;
  nxaudio_runtime_identity new_identity;
  nxaudio_runtime_identity wrong_identity;
  nxaudio_runtime_snapshot snapshot;
  char line[NXAUDIO_RUNTIME_RECEIPT_LINE_MAX];

  open_session(&previous, &old_identity, 50u, 9u, 100u);
  make_live(&previous, &old_identity, 101u);
  add_live_pcm(&previous, &old_identity, 7u, 102u);
  close_session(&previous, &old_identity, 103u, 104u);
  CHECK(nxaudio_runtime_reopen(&previous, &old_identity, &next, "fixture-api",
                               "fixture-backend", &k_format, &k_format,
                               200u, &new_identity) == NXAUDIO_INVALID);
  CHECK(nxaudio_runtime_format_receipt(&previous, &old_identity, line,
                                       sizeof(line)) == NXAUDIO_OK);
  CHECK(nxaudio_runtime_reopen(&previous, &old_identity, &next, "fixture-api",
                               "fixture-backend", &k_format, &k_format,
                               200u, &new_identity) == NXAUDIO_OK);
  CHECK(new_identity.run_id == old_identity.run_id);
  CHECK(new_identity.generation == old_identity.generation + 1u);
  CHECK(nxaudio_runtime_get_snapshot(&next, &new_identity, &snapshot) ==
        NXAUDIO_OK);
  CHECK(snapshot.callbacks_observed == 0u);
  CHECK(snapshot.bytes_delivered == 0u);
  CHECK(nxaudio_runtime_expect_callbacks(&next, &new_identity, 4u, 200u) ==
        NXAUDIO_OK);
  make_live(&next, &new_identity, 201u);
  CHECK(nxaudio_runtime_update_callback(&next, &old_identity, 7u, 202u, 1u,
                                        1u, 4u, 1u, 0.25) ==
        NXAUDIO_INVALID);
  CHECK(nxaudio_runtime_get_snapshot(&next, &new_identity, &snapshot) ==
        NXAUDIO_OK);
  CHECK(snapshot.identity_violation == 1u);

  wrong_identity = new_identity;
  wrong_identity.backend_tag ^= 1u;
  CHECK(nxaudio_runtime_set_degraded(
            &next, &wrong_identity, NXAUDIO_RECOVERY_FAULT_DEVICE_LOST,
            203u) == NXAUDIO_INVALID);
  CHECK(nxaudio_runtime_get_snapshot(&next, &new_identity, &snapshot) ==
        NXAUDIO_OK);
  CHECK(snapshot.recovery.fault == NXAUDIO_RECOVERY_FAULT_NONE);
  CHECK(nxaudio_runtime_set_paused(&next, &wrong_identity, 203u) ==
        NXAUDIO_INVALID);

  open_session(&previous, &old_identity, 51u, UINT32_MAX, 300u);
  make_live(&previous, &old_identity, 301u);
  add_live_pcm(&previous, &old_identity, 7u, 302u);
  close_session(&previous, &old_identity, 303u, 304u);
  CHECK(nxaudio_runtime_format_receipt(&previous, &old_identity, line,
                                       sizeof(line)) == NXAUDIO_OK);
  CHECK(nxaudio_runtime_reopen(&previous, &old_identity, &next, "fixture-api",
                               "fixture-backend", &k_format, &k_format,
                               400u, &new_identity) == NXAUDIO_INVALID);
}

static void test_liveness_pause_and_time(void) {
  nxaudio_runtime_session session;
  nxaudio_runtime_identity identity;
  nxaudio_runtime_snapshot snapshot;

  open_session(&session, &identity, 60u, 1u, 100u);
  make_live(&session, &identity, 101u);
  CHECK(nxaudio_runtime_is_stalled(&session, &identity, 111u, 10u) == 0);
  CHECK(nxaudio_runtime_is_stalled(&session, &identity, 112u, 10u) == 1);
  CHECK(nxaudio_runtime_update_callback(&session, &identity, 3u, 113u, 1u,
                                        16u, 64u, 8u, 0.25) ==
        NXAUDIO_OK);
  CHECK(nxaudio_runtime_is_stalled(&session, &identity, 120u, 10u) == 0);
  CHECK(nxaudio_runtime_set_paused(&session, &identity, 121u) == NXAUDIO_OK);
  CHECK(nxaudio_runtime_is_stalled(&session, &identity, 10000u, 10u) == 0);
  CHECK(nxaudio_runtime_set_live(&session, &identity, 10001u) == NXAUDIO_OK);
  CHECK(nxaudio_runtime_is_stalled(&session, &identity, 10002u, 10u) == 0);

  CHECK(nxaudio_runtime_update_callback(&session, &identity, 3u, 9999u, 1u,
                                        1u, 4u, 1u, 0.1) ==
        NXAUDIO_INVALID);
  CHECK(nxaudio_runtime_get_snapshot(&session, &identity, &snapshot) ==
        NXAUDIO_OK);
  CHECK(snapshot.time_violation == 1u);
  CHECK(nxaudio_runtime_is_stalled(&session, &identity, 10003u, 0u) == -1);
}

static void test_pcm_and_measurements(void) {
  nxaudio_runtime_session session;
  nxaudio_runtime_identity identity;
  nxaudio_runtime_snapshot snapshot;
  char line[NXAUDIO_RUNTIME_RECEIPT_LINE_MAX];

  open_session(&session, &identity, 70u, 1u, 10u);
  make_live(&session, &identity, 11u);
  CHECK(nxaudio_runtime_update_callback(&session, &identity, 4u, 12u, 4u,
                                        8u, 32u, 0u, 0.0) == NXAUDIO_OK);
  CHECK(nxaudio_runtime_report_underrun(&session, &identity, 4u) ==
        NXAUDIO_OK);
  CHECK(nxaudio_runtime_report_silence(&session, &identity, 4u, 8u) ==
        NXAUDIO_OK);
  CHECK(nxaudio_runtime_get_snapshot(&session, &identity, &snapshot) ==
        NXAUDIO_OK);
  CHECK(snapshot.underrun_count == 1u);
  CHECK(snapshot.silence_frames == 8u);
  CHECK(snapshot.callback_epoch == 3u);
  close_session(&session, &identity, 13u, 14u);
  CHECK(nxaudio_runtime_format_receipt(&session, &identity, line,
                                       sizeof(line)) == NXAUDIO_OK);
  CHECK(strstr(line, "result=FAIL reason=pcm-zero") != NULL);

  open_session(&session, &identity, 71u, 1u, 20u);
  make_live(&session, &identity, 21u);
  CHECK(nxaudio_runtime_update_callback(&session, &identity, 4u, 22u, 1u,
                                        8u, 32u, 1u, NAN) ==
        NXAUDIO_INVALID);
  CHECK(nxaudio_runtime_update_callback(&session, &identity, 4u, 23u, 1u,
                                        8u, 32u, 1u, INFINITY) ==
        NXAUDIO_INVALID);
  CHECK(nxaudio_runtime_update_callback(&session, &identity, 4u, 24u, 1u,
                                        8u, 32u, 1u, -0.1) ==
        NXAUDIO_INVALID);
  CHECK(nxaudio_runtime_update_callback(&session, &identity, 4u, 25u, 1u,
                                        1u, 4u, 3u, 0.5) ==
        NXAUDIO_INVALID);
}

static void test_callback_missing_and_format_mismatch(void) {
  nxaudio_runtime_session session;
  nxaudio_runtime_identity identity;
  nxaudio_format converted = k_format;
  char line[NXAUDIO_RUNTIME_RECEIPT_LINE_MAX];

  open_session(&session, &identity, 75u, 1u, 10u);
  make_live(&session, &identity, 11u);
  close_session(&session, &identity, 12u, 13u);
  CHECK(nxaudio_runtime_format_receipt(&session, &identity, line,
                                       sizeof(line)) == NXAUDIO_OK);
  CHECK(strstr(line, "callbacks=0/4") != NULL);
  CHECK(strstr(line, "result=FAIL reason=callback-missing") != NULL);

  converted.frequency = 44100u;
  nxaudio_runtime_init(&session, 76u, 1u);
  CHECK(nxaudio_runtime_open(&session, "fixture-api", "fixture-backend",
                             &k_format, &converted, 20u, &identity) ==
        NXAUDIO_OK);
  CHECK(nxaudio_runtime_expect_callbacks(&session, &identity, 1u, 20u) ==
        NXAUDIO_OK);
  make_live(&session, &identity, 21u);
  CHECK(nxaudio_runtime_update_callback(&session, &identity, 4u, 22u, 1u,
                                        8u, 32u, 1u, 0.25) == NXAUDIO_OK);
  close_session(&session, &identity, 23u, 24u);
  CHECK(nxaudio_runtime_format_receipt(&session, &identity, line,
                                       sizeof(line)) == NXAUDIO_OK);
  CHECK(strstr(line, "format=rate-changed") != NULL);
  CHECK(strstr(line, "result=FAIL") != NULL);
}

static void test_recovery_outcomes(void) {
  const nxaudio_recovery_outcome positive[] = {
      NXAUDIO_RECOVERY_RECOVERED, NXAUDIO_RECOVERY_REOPENED};
  const nxaudio_recovery_outcome negative[] = {
      NXAUDIO_RECOVERY_FAILED, NXAUDIO_RECOVERY_EXHAUSTED};
  size_t index;

  for (index = 0u; index < sizeof(positive) / sizeof(positive[0]); ++index) {
    nxaudio_runtime_session session;
    nxaudio_runtime_identity identity;
    nxaudio_backend_recovery recovery;
    nxaudio_runtime_snapshot snapshot;
    open_session(&session, &identity, 80u + (uint32_t)index, 1u, 10u);
    make_live(&session, &identity, 11u);
    add_live_pcm(&session, &identity, 5u, 12u);
    CHECK(nxaudio_runtime_report_device_lost(&session, &identity, 5u, 13u) ==
          NXAUDIO_OK);
    CHECK(nxaudio_runtime_set_recovering(&session, &identity, 14u) ==
          NXAUDIO_OK);
    recovery = recovery_result(NXAUDIO_RECOVERY_FAULT_DEVICE_LOST,
                               positive[index]);
    CHECK(nxaudio_runtime_record_recovery(&session, &identity, &recovery,
                                           15u) ==
          (positive[index] == NXAUDIO_RECOVERY_REOPENED ? NXAUDIO_UNSUPPORTED
                                                        : NXAUDIO_OK));
    if (positive[index] == NXAUDIO_RECOVERY_RECOVERED) {
      char line[NXAUDIO_RUNTIME_RECEIPT_LINE_MAX];
      CHECK(nxaudio_runtime_set_live(&session, &identity, 16u) == NXAUDIO_OK);
      add_live_pcm(&session, &identity, 5u, 17u);
      CHECK(nxaudio_runtime_get_snapshot(&session, &identity, &snapshot) ==
            NXAUDIO_OK);
      CHECK(snapshot.device_loss_count == 1u);
      CHECK(snapshot.recovery.outcome == positive[index]);
      close_session(&session, &identity, 19u, 20u);
      CHECK(nxaudio_runtime_format_receipt(&session, &identity, line,
                                           sizeof(line)) == NXAUDIO_OK);
      CHECK(strstr(line, "fault=device-lost") != NULL);
      CHECK(strstr(line, "recovery=recovered") != NULL);
    } else {
      nxaudio_runtime_session next;
      nxaudio_runtime_identity next_identity;
      char line[NXAUDIO_RUNTIME_RECEIPT_LINE_MAX];
      CHECK(nxaudio_runtime_get_state(&session) ==
            NXAUDIO_RUNTIME_STATE_FAILED);
      CHECK(nxaudio_runtime_format_receipt(&session, &identity, line,
                                           sizeof(line)) == NXAUDIO_OK);
      CHECK(strstr(line, "fault=device-lost") != NULL);
      CHECK(strstr(line, "recovery=reopened") != NULL);
      CHECK(strstr(line, "result=FAIL reason=reopen-required") != NULL);
      CHECK(nxaudio_runtime_set_live(&session, &identity, 16u) != NXAUDIO_OK);
      CHECK(nxaudio_runtime_reopen(&session, &identity, &next, "fixture-api",
                                   "fixture-backend", &k_format, &k_format,
                                   20u, &next_identity) == NXAUDIO_OK);
      CHECK(next_identity.generation == identity.generation + 1u);
    }
  }

  for (index = 0u; index < sizeof(negative) / sizeof(negative[0]); ++index) {
    nxaudio_runtime_session session;
    nxaudio_runtime_identity identity;
    nxaudio_backend_recovery recovery;
    char line[NXAUDIO_RUNTIME_RECEIPT_LINE_MAX];
    open_session(&session, &identity, 90u + (uint32_t)index, 1u, 10u);
    make_live(&session, &identity, 11u);
    CHECK(nxaudio_runtime_set_degraded(
              &session, &identity, NXAUDIO_RECOVERY_FAULT_XRUN_EPIPE, 12u) ==
          NXAUDIO_OK);
    CHECK(nxaudio_runtime_set_recovering(&session, &identity, 13u) ==
          NXAUDIO_OK);
    recovery =
        recovery_result(NXAUDIO_RECOVERY_FAULT_XRUN_EPIPE, negative[index]);
    CHECK(nxaudio_runtime_record_recovery(&session, &identity, &recovery,
                                           14u) == NXAUDIO_UNSUPPORTED);
    CHECK(nxaudio_runtime_get_state(&session) == NXAUDIO_RUNTIME_STATE_FAILED);
    CHECK(nxaudio_runtime_format_receipt(&session, &identity, line,
                                         sizeof(line)) == NXAUDIO_OK);
    CHECK(strstr(line, "result=FAIL reason=recovery-failed") != NULL);
  }

  {
    nxaudio_runtime_session session;
    nxaudio_runtime_identity identity;
    nxaudio_backend_recovery recovery;
    open_session(&session, &identity, 99u, 1u, 10u);
    make_live(&session, &identity, 11u);
    CHECK(nxaudio_runtime_set_degraded(
              &session, &identity, NXAUDIO_RECOVERY_FAULT_CALLBACK_STALLED,
              12u) == NXAUDIO_OK);
    CHECK(nxaudio_runtime_set_recovering(&session, &identity, 13u) ==
          NXAUDIO_OK);
    recovery = recovery_result(NXAUDIO_RECOVERY_FAULT_CALLBACK_STALLED,
                               NXAUDIO_RECOVERY_PENDING);
    CHECK(nxaudio_runtime_record_recovery(&session, &identity, &recovery,
                                           14u) == NXAUDIO_INVALID);
    CHECK(nxaudio_runtime_set_live(&session, &identity, 15u) ==
          NXAUDIO_WRONG_STATE);
  }

  {
    nxaudio_runtime_session session;
    nxaudio_runtime_identity identity;
    nxaudio_backend_recovery recovery;
    open_session(&session, &identity, 98u, 1u, 10u);
    make_live(&session, &identity, 11u);
    CHECK(nxaudio_runtime_set_degraded(
              &session, &identity, NXAUDIO_RECOVERY_FAULT_XRUN_EPIPE,
              12u) == NXAUDIO_OK);
    CHECK(nxaudio_runtime_set_recovering(&session, &identity, 13u) ==
          NXAUDIO_OK);
    recovery = recovery_result(NXAUDIO_RECOVERY_FAULT_XRUN_EPIPE,
                               NXAUDIO_RECOVERY_RECOVERED);
    recovery.reopen_status = NXAUDIO_RECOVERY_STEP_OK;
    CHECK(nxaudio_runtime_record_recovery(&session, &identity, &recovery,
                                           14u) == NXAUDIO_INVALID);
    recovery.reopen_status = NXAUDIO_RECOVERY_STEP_NOT_ATTEMPTED;
    CHECK(nxaudio_runtime_record_recovery(&session, &identity, &recovery,
                                           15u) == NXAUDIO_OK);
    CHECK(nxaudio_runtime_record_recovery(&session, &identity, &recovery,
                                           16u) == NXAUDIO_WRONG_STATE);
  }
}

static void test_shutdown_fail_closed(void) {
  nxaudio_runtime_session session;
  nxaudio_runtime_identity identity;
  nxaudio_runtime_identity wrong;
  char line[NXAUDIO_RUNTIME_RECEIPT_LINE_MAX];

  open_session(&session, &identity, 100u, 1u, 10u);
  make_live(&session, &identity, 11u);
  add_live_pcm(&session, &identity, 8u, 12u);
  CHECK(nxaudio_runtime_set_draining(&session, &identity, 13u) == NXAUDIO_OK);
  wrong = identity;
  wrong.generation++;
  CHECK(nxaudio_runtime_set_closed(&session, &wrong,
                                   NXAUDIO_SAFE_EXIT_CONFIRMED, 14u) ==
        NXAUDIO_INVALID);
  CHECK(nxaudio_runtime_set_closed(&session, &identity,
                                   NXAUDIO_SAFE_EXIT_CONFIRMED, 15u) ==
        NXAUDIO_OK);
  CHECK(nxaudio_runtime_format_receipt(&session, &identity, line,
                                       sizeof(line)) == NXAUDIO_OK);
  CHECK(strstr(line, "result=FAIL reason=identity-mismatch") != NULL);

  open_session(&session, &identity, 101u, 1u, 20u);
  make_live(&session, &identity, 21u);
  add_live_pcm(&session, &identity, 8u, 22u);
  CHECK(nxaudio_runtime_set_draining(&session, &identity, 23u) == NXAUDIO_OK);
  CHECK(nxaudio_runtime_set_closed(&session, &identity,
                                   NXAUDIO_SAFE_EXIT_TIMEOUT, 24u) ==
        NXAUDIO_UNSUPPORTED);
  CHECK(nxaudio_runtime_get_state(&session) == NXAUDIO_RUNTIME_STATE_FAILED);
  CHECK(nxaudio_runtime_format_receipt(&session, &identity, line,
                                       sizeof(line)) == NXAUDIO_OK);
  CHECK(strstr(line, "shutdown=timeout result=FAIL reason=shutdown-timeout") !=
        NULL);

  open_session(&session, &identity, 102u, 1u, 30u);
  make_live(&session, &identity, 31u);
  add_live_pcm(&session, &identity, 8u, 32u);
  CHECK(nxaudio_runtime_set_draining(&session, &identity, 33u) == NXAUDIO_OK);
  CHECK(nxaudio_runtime_set_closed(
            &session, &identity, (nxaudio_safe_exit_status)99, 34u) ==
        NXAUDIO_INVALID);
  CHECK(nxaudio_runtime_get_state(&session) ==
        NXAUDIO_RUNTIME_STATE_DRAINING);
  CHECK(session.shutdown_status == NXAUDIO_SAFE_EXIT_PENDING);
  CHECK(nxaudio_runtime_set_closed(&session, &identity,
                                   NXAUDIO_SAFE_EXIT_CONFIRMED, 35u) ==
        NXAUDIO_OK);
}

static void test_saturation_without_wrap(void) {
  nxaudio_runtime_session session;
  nxaudio_runtime_identity identity;
  nxaudio_runtime_snapshot snapshot;
  char line[NXAUDIO_RUNTIME_RECEIPT_LINE_MAX];

  open_session(&session, &identity, 110u, 1u, 10u);
  make_live(&session, &identity, 11u);
  session.callbacks_observed = UINT32_MAX - 1u;
  session.callbacks_expected = UINT32_MAX - 1u;
  CHECK(nxaudio_runtime_update_callback(&session, &identity, 6u, 12u, 2u,
                                        2u, 8u, 2u, 0.5) == NXAUDIO_OK);
  CHECK(nxaudio_runtime_get_snapshot(&session, &identity, &snapshot) ==
        NXAUDIO_OK);
  CHECK(snapshot.callbacks_observed == UINT32_MAX);
  CHECK(snapshot.callbacks_expected == UINT32_MAX - 1u);
  CHECK(snapshot.overflowed == 1u);
  close_session(&session, &identity, 13u, 14u);
  CHECK(nxaudio_runtime_format_receipt(&session, &identity, line,
                                       sizeof(line)) == NXAUDIO_OK);
  CHECK(strstr(line, "result=FAIL reason=counter-overflow") != NULL);
}

static void test_callback_quiescence(void) {
  nxaudio_runtime_session session;
  nxaudio_runtime_identity identity;
  nxaudio_runtime_snapshot snapshot;
  char line[NXAUDIO_RUNTIME_RECEIPT_LINE_MAX];

  open_session(&session, &identity, 115u, 1u, 10u);
  make_live(&session, &identity, 11u);
  add_live_pcm(&session, &identity, 6u, 12u);
  __atomic_store_n(&session.callback_active, 1u, __ATOMIC_RELEASE);
  CHECK(nxaudio_runtime_set_draining(&session, &identity, 13u) ==
        NXAUDIO_FULL);
  CHECK(nxaudio_runtime_get_snapshot(&session, &identity, &snapshot) ==
        NXAUDIO_FULL);
  CHECK(nxaudio_runtime_set_closed(&session, &identity,
                                   NXAUDIO_SAFE_EXIT_CONFIRMED, 14u) ==
        NXAUDIO_FULL);
  CHECK(nxaudio_runtime_format_receipt(&session, &identity, line,
                                       sizeof(line)) == NXAUDIO_FULL);
  __atomic_store_n(&session.callback_active, 0u, __ATOMIC_RELEASE);
  CHECK(nxaudio_runtime_set_draining(&session, &identity, 15u) == NXAUDIO_OK);
  CHECK(nxaudio_runtime_set_closed(&session, &identity,
                                   NXAUDIO_SAFE_EXIT_CONFIRMED, 16u) ==
        NXAUDIO_OK);
}

typedef struct degradation_job {
  nxaudio_runtime_session *session;
  nxaudio_runtime_identity identity;
  nxaudio_result result;
} degradation_job;

static void *worker_degradation(void *opaque) {
  degradation_job *job = (degradation_job *)opaque;
  job->result = nxaudio_runtime_set_degraded(
      job->session, &job->identity,
      NXAUDIO_RECOVERY_FAULT_CALLBACK_STALLED, 20u);
  return NULL;
}

static void *callback_device_loss(void *opaque) {
  degradation_job *job = (degradation_job *)opaque;
  job->result = nxaudio_runtime_report_device_lost(
      job->session, &job->identity, 31u, 20u);
  return NULL;
}

static void test_degradation_fault_race(void) {
  nxaudio_runtime_session session;
  nxaudio_runtime_identity identity;
  nxaudio_runtime_snapshot snapshot;
  degradation_job worker;
  degradation_job callback;
  pthread_t worker_thread;
  pthread_t callback_thread;
  unsigned int passed;

  open_session(&session, &identity, 118u, 1u, 1u);
  make_live(&session, &identity, 2u);
  worker.session = &session;
  worker.identity = identity;
  worker.result = NXAUDIO_INVALID;
  callback = worker;
  CHECK(pthread_create(&worker_thread, NULL, worker_degradation, &worker) == 0);
  CHECK(pthread_create(&callback_thread, NULL, callback_device_loss,
                       &callback) == 0);
  CHECK(pthread_join(worker_thread, NULL) == 0);
  CHECK(pthread_join(callback_thread, NULL) == 0);
  passed = (worker.result == NXAUDIO_OK ? 1u : 0u) +
           (callback.result == NXAUDIO_OK ? 1u : 0u);
  CHECK(passed == 1u);
  CHECK(nxaudio_runtime_get_snapshot(&session, &identity, &snapshot) ==
        NXAUDIO_OK);
  CHECK(snapshot.state == NXAUDIO_RUNTIME_STATE_DEGRADED);
  CHECK(snapshot.recovery.fault == NXAUDIO_RECOVERY_FAULT_CALLBACK_STALLED ||
        snapshot.recovery.fault == NXAUDIO_RECOVERY_FAULT_DEVICE_LOST);
  CHECK(nxaudio_runtime_set_recovering(&session, &identity, 21u) ==
        NXAUDIO_OK);
}

typedef struct callback_job {
  nxaudio_runtime_session *session;
  nxaudio_runtime_identity identity;
  uint32_t producer_id;
  uint32_t iterations;
  nxaudio_result result;
} callback_job;

static void *callback_worker(void *opaque) {
  callback_job *job = (callback_job *)opaque;
  uint32_t index;
  job->result = NXAUDIO_OK;
  for (index = 0u; index < job->iterations; ++index) {
    nxaudio_result result = nxaudio_runtime_update_callback(
        job->session, &job->identity, job->producer_id, 100u + index, 1u, 1u,
        4u, 1u, 0.1);
    if (result != NXAUDIO_OK) {
      job->result = result;
      break;
    }
  }
  return NULL;
}

static void test_concurrency_and_second_producer(void) {
  nxaudio_runtime_session session;
  nxaudio_runtime_identity identity;
  nxaudio_runtime_snapshot snapshot;
  callback_job producer_a;
  callback_job producer_b;
  pthread_t thread_a;
  pthread_t thread_b;
  uint32_t index;
  unsigned int passed;

  open_session(&session, &identity, 119u, 1u, 1u);
  make_live(&session, &identity, 2u);
  CHECK(nxaudio_runtime_update_callback(&session, &identity, 0u, 3u, 1u, 1u,
                                        4u, 1u, 0.1) == NXAUDIO_INVALID);
  CHECK(nxaudio_runtime_get_snapshot(&session, &identity, &snapshot) ==
        NXAUDIO_OK);
  CHECK(snapshot.producer_violation == 1u);

  open_session(&session, &identity, 120u, 1u, 1u);
  make_live(&session, &identity, 2u);
  producer_a.session = &session;
  producer_a.identity = identity;
  producer_a.producer_id = 11u;
  producer_a.iterations = 1u;
  producer_a.result = NXAUDIO_INVALID;
  producer_b = producer_a;
  producer_b.producer_id = 12u;
  CHECK(pthread_create(&thread_a, NULL, callback_worker, &producer_a) == 0);
  CHECK(pthread_create(&thread_b, NULL, callback_worker, &producer_b) == 0);
  CHECK(pthread_join(thread_a, NULL) == 0);
  CHECK(pthread_join(thread_b, NULL) == 0);
  passed = (producer_a.result == NXAUDIO_OK ? 1u : 0u) +
           (producer_b.result == NXAUDIO_OK ? 1u : 0u);
  CHECK(passed == 1u);
  CHECK(nxaudio_runtime_get_snapshot(&session, &identity, &snapshot) ==
        NXAUDIO_OK);
  CHECK(snapshot.producer_violation == 1u);

  open_session(&session, &identity, 121u, 1u, 1u);
  make_live(&session, &identity, 2u);
  producer_a.session = &session;
  producer_a.identity = identity;
  producer_a.producer_id = 21u;
  producer_a.iterations = 1000u;
  producer_a.result = NXAUDIO_INVALID;
  CHECK(pthread_create(&thread_a, NULL, callback_worker, &producer_a) == 0);
  for (index = 0u; index < 1000u; ++index) {
    nxaudio_result result =
        nxaudio_runtime_get_snapshot(&session, &identity, &snapshot);
    CHECK(result == NXAUDIO_OK || result == NXAUDIO_FULL);
    if (result == NXAUDIO_OK)
      CHECK(snapshot.callbacks_observed <= 1000u);
  }
  CHECK(pthread_join(thread_a, NULL) == 0);
  CHECK(producer_a.result == NXAUDIO_OK);
  CHECK(nxaudio_runtime_get_snapshot(&session, &identity, &snapshot) ==
        NXAUDIO_OK);
  CHECK(snapshot.callbacks_observed == 1000u);
}

int main(void) {
  test_open_validation_and_state_matrix();
  test_valid_flow_and_exact_fixture_receipt();
  test_short_buffer_is_retryable();
  test_identity_stale_and_reopen();
  test_liveness_pause_and_time();
  test_pcm_and_measurements();
  test_callback_missing_and_format_mismatch();
  test_recovery_outcomes();
  test_shutdown_fail_closed();
  test_saturation_without_wrap();
  test_callback_quiescence();
  test_degradation_fault_race();
  test_concurrency_and_second_producer();
  puts("nxaudio-runtime: PASS guards=13 class=FIXTURE physical=0");
  return 0;
}
