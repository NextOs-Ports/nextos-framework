/* SPDX-License-Identifier: GPL-3.0-only */
/* V3-AUDIO-01 host test: receipt formatting, liveness, underrun counting,
 * safe-exit pump and probe-order stability. Hermetic: no device, no thread,
 * no environment read. */
#include "nxaudio_receipt.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,     \
              #condition);                                                     \
      return -1;                                                               \
    }                                                                          \
  } while (0)

static nxaudio_receipt canonical_receipt(void) {
  nxaudio_receipt receipt;
  nxaudio_receipt_init(&receipt);
  memcpy(receipt.api, "sdl2", sizeof("sdl2"));
  memcpy(receipt.backend, "alsa", sizeof("alsa"));
  receipt.requested_rate = 44100u;
  receipt.requested_channels = 2u;
  receipt.requested_sample_format = NXAUDIO_SAMPLE_S16LE;
  receipt.obtained_rate = 44100u;
  receipt.obtained_channels = 2u;
  receipt.obtained_sample_format = NXAUDIO_SAMPLE_S16LE;
  receipt.callbacks_expected = 600u;
  receipt.callbacks_observed = 512u;
  receipt.bytes_delivered = 12345678u;
  receipt.peak_abs = 0.708;
  receipt.underrun_count = 3u;
  return receipt;
}

static int test_receipt_format_canonical(void) {
  nxaudio_receipt receipt = canonical_receipt();
  char line[NXAUDIO_RECEIPT_LINE_MAX];
  CHECK(nxaudio_receipt_format(&receipt, line, sizeof(line)) == NXAUDIO_OK);
  CHECK(strcmp(line,
               "AUDIO-RECEIPT: api=sdl2 backend=alsa "
               "req=44100Hz/2ch/s16le got=44100Hz/2ch/s16le "
               "callbacks=512 bytes=12345678 peak=0.708 underruns=3") == 0);
  /* Single line, machine-parsable. */
  CHECK(strchr(line, '\n') == NULL);
  return 0;
}

static int test_receipt_format_conversion(void) {
  /* requested != obtained: the divergence is RECORDED in the same line,
   * never silently substituted. */
  nxaudio_receipt receipt = canonical_receipt();
  char line[NXAUDIO_RECEIPT_LINE_MAX];
  nxaudio_reason reason = NXAUDIO_REASON_NONE;
  nxaudio_format requested, granted;
  memcpy(receipt.api, "fmod-bridge", sizeof("fmod-bridge"));
  memcpy(receipt.backend, "pulseaudio", sizeof("pulseaudio"));
  receipt.requested_rate = 44100u;
  receipt.requested_channels = 1u;
  receipt.requested_sample_format = NXAUDIO_SAMPLE_S16LE;
  receipt.obtained_rate = 48000u;
  receipt.obtained_channels = 2u;
  receipt.obtained_sample_format = NXAUDIO_SAMPLE_F32LE;
  receipt.callbacks_observed = 1u;
  receipt.bytes_delivered = 4096u;
  receipt.peak_abs = 1.0;
  receipt.underrun_count = 0u;
  CHECK(nxaudio_receipt_format(&receipt, line, sizeof(line)) == NXAUDIO_OK);
  CHECK(strcmp(line,
               "AUDIO-RECEIPT: api=fmod-bridge backend=pulseaudio "
               "req=44100Hz/1ch/s16le got=48000Hz/2ch/f32le "
               "callbacks=1 bytes=4096 peak=1.000 underruns=0") == 0);
  /* The same divergence has a stable reason in the existing classifier. */
  memset(&requested, 0, sizeof(requested));
  memset(&granted, 0, sizeof(granted));
  requested.frequency = receipt.requested_rate;
  requested.channels = receipt.requested_channels;
  requested.sample_format = receipt.requested_sample_format;
  granted.frequency = receipt.obtained_rate;
  granted.channels = receipt.obtained_channels;
  granted.sample_format = receipt.obtained_sample_format;
  CHECK(nxaudio_classify_granted_format(&requested, &granted, &reason) ==
        NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_RATE_CHANGED);
  return 0;
}

static int test_receipt_format_rejects(void) {
  nxaudio_receipt receipt = canonical_receipt();
  char line[NXAUDIO_RECEIPT_LINE_MAX];
  char tiny[8];
  CHECK(nxaudio_receipt_format(NULL, line, sizeof(line)) == NXAUDIO_INVALID);
  CHECK(nxaudio_receipt_format(&receipt, NULL, 0u) == NXAUDIO_INVALID);
  CHECK(nxaudio_receipt_format(&receipt, tiny, sizeof(tiny)) == NXAUDIO_FULL);
  CHECK(tiny[0] == '\0');
  receipt = canonical_receipt();
  receipt.api_version = 2u;
  CHECK(nxaudio_receipt_format(&receipt, line, sizeof(line)) ==
        NXAUDIO_INVALID);
  receipt = canonical_receipt();
  receipt.api[0] = '\0';
  CHECK(nxaudio_receipt_format(&receipt, line, sizeof(line)) ==
        NXAUDIO_INVALID);
  receipt = canonical_receipt();
  memcpy(receipt.backend, "al sa", sizeof("al sa"));
  CHECK(nxaudio_receipt_format(&receipt, line, sizeof(line)) ==
        NXAUDIO_INVALID);
  receipt = canonical_receipt();
  memcpy(receipt.backend, "a=b", sizeof("a=b"));
  CHECK(nxaudio_receipt_format(&receipt, line, sizeof(line)) ==
        NXAUDIO_INVALID);
  receipt = canonical_receipt();
  receipt.obtained_rate = 0u;
  CHECK(nxaudio_receipt_format(&receipt, line, sizeof(line)) ==
        NXAUDIO_INVALID);
  receipt = canonical_receipt();
  receipt.requested_sample_format = (nxaudio_sample_format)7;
  CHECK(nxaudio_receipt_format(&receipt, line, sizeof(line)) ==
        NXAUDIO_INVALID);
  receipt = canonical_receipt();
  receipt.peak_abs = -0.5;
  CHECK(nxaudio_receipt_format(&receipt, line, sizeof(line)) ==
        NXAUDIO_INVALID);
  return 0;
}

static int test_probe_order(void) {
  /* Order stability: inherited env -> measured open success -> declared
   * fallback. Any reordering is a contract break. */
  const char *const *stages = NULL;
  size_t count = nxaudio_backend_probe_order(&stages);
  CHECK(count == 3u);
  CHECK(stages != NULL);
  CHECK(strcmp(stages[0], "inherited-environment") == 0);
  CHECK(strcmp(stages[1], "measured-open-success") == 0);
  CHECK(strcmp(stages[2], "declared-fallback") == 0);
  /* Same static array on every call. */
  CHECK(nxaudio_backend_probe_order(NULL) == 3u);
  {
    const char *const *again = NULL;
    CHECK(nxaudio_backend_probe_order(&again) == 3u);
    CHECK(again == stages);
  }
  return 0;
}

static int test_liveness(void) {
  nxaudio_liveness liveness;
  const uint64_t ms = 1000000u;
  nxaudio_liveness_init(&liveness, 100u * ms);
  CHECK(liveness.tick_count == 0u);
  /* Never ticked: alive inside the budget, dead after it. */
  CHECK(nxaudio_liveness_dead(&liveness, 120u * ms, 50u * ms) == 0);
  CHECK(nxaudio_liveness_dead(&liveness, 151u * ms, 50u * ms) == 1);
  /* Ticks keep it alive. */
  CHECK(nxaudio_liveness_tick(&liveness, 130u * ms) == NXAUDIO_OK);
  CHECK(nxaudio_liveness_tick(&liveness, 140u * ms) == NXAUDIO_OK);
  CHECK(liveness.tick_count == 2u);
  CHECK(nxaudio_liveness_dead(&liveness, 190u * ms, 50u * ms) == 0);
  CHECK(nxaudio_liveness_dead(&liveness, 191u * ms, 50u * ms) == 1);
  /* Time never runs backwards; a zero budget is meaningless. */
  CHECK(nxaudio_liveness_tick(&liveness, 100u * ms) == NXAUDIO_INVALID);
  CHECK(nxaudio_liveness_dead(&liveness, 90u * ms, 50u * ms) == -1);
  CHECK(nxaudio_liveness_dead(&liveness, 200u * ms, 0u) == -1);
  CHECK(nxaudio_liveness_dead(NULL, 200u * ms, 50u * ms) == -1);
  CHECK(nxaudio_liveness_tick(NULL, 200u * ms) == NXAUDIO_INVALID);
  /* Underrun counter API. */
  CHECK(nxaudio_liveness_underruns(&liveness) == 0u);
  CHECK(nxaudio_liveness_underrun(&liveness) == NXAUDIO_OK);
  CHECK(nxaudio_liveness_underrun(&liveness) == NXAUDIO_OK);
  CHECK(nxaudio_liveness_underruns(&liveness) == 2u);
  CHECK(nxaudio_liveness_underrun(NULL) == NXAUDIO_INVALID);
  CHECK(nxaudio_liveness_underruns(NULL) == 0u);
  return 0;
}

struct fake_poll {
  int calls;
  int confirm_on; /* poll number that confirms; 0 = never */
};

static int fake_poll_fn(void *user) {
  struct fake_poll *poll = (struct fake_poll *)user;
  poll->calls += 1;
  return poll->confirm_on != 0 && poll->calls >= poll->confirm_on;
}

static nxaudio_safe_exit safe_exit_state(void) {
  nxaudio_safe_exit state;
  memset(&state, 0, sizeof(state));
  state.api_version = NXAUDIO_RECEIPT_API_VERSION;
  state.struct_size = sizeof(state);
  state.now_ns = 0u;
  state.poll_interval_ns = 10u;
  return state;
}

static int test_safe_exit(void) {
  nxaudio_safe_exit state = safe_exit_state();
  struct fake_poll poll;

  /* Backend confirms on the 3rd poll, inside the deadline. */
  memset(&poll, 0, sizeof(poll));
  poll.confirm_on = 3;
  CHECK(nxaudio_safe_exit_pump(&state, 100u, fake_poll_fn, &poll) ==
        NXAUDIO_OK);
  CHECK(state.status == NXAUDIO_SAFE_EXIT_CONFIRMED);
  CHECK(state.polls == 3u && poll.calls == 3);
  CHECK(state.now_ns == 20u);

  /* Backend never confirms: pump keeps polling until the deadline, then
   * reports timeout — it never gives up early and never calls _exit. */
  state = safe_exit_state();
  memset(&poll, 0, sizeof(poll));
  poll.confirm_on = 0;
  CHECK(nxaudio_safe_exit_pump(&state, 100u, fake_poll_fn, &poll) ==
        NXAUDIO_UNSUPPORTED);
  CHECK(state.status == NXAUDIO_SAFE_EXIT_TIMEOUT);
  CHECK(state.polls == 11u && poll.calls == 11);
  CHECK(state.now_ns == 100u);

  /* Deadline already past: zero polls, timeout. */
  state = safe_exit_state();
  state.now_ns = 200u;
  memset(&poll, 0, sizeof(poll));
  poll.confirm_on = 1;
  CHECK(nxaudio_safe_exit_pump(&state, 100u, fake_poll_fn, &poll) ==
        NXAUDIO_UNSUPPORTED);
  CHECK(state.polls == 0u && poll.calls == 0);

  /* Interval larger than the window still terminates (saturation). */
  state = safe_exit_state();
  state.poll_interval_ns = 1000u;
  memset(&poll, 0, sizeof(poll));
  poll.confirm_on = 0;
  CHECK(nxaudio_safe_exit_pump(&state, 100u, fake_poll_fn, &poll) ==
        NXAUDIO_UNSUPPORTED);
  CHECK(state.polls == 1u && state.now_ns == 100u);

  /* Invalid arguments. */
  state = safe_exit_state();
  CHECK(nxaudio_safe_exit_pump(NULL, 100u, fake_poll_fn, &poll) ==
        NXAUDIO_INVALID);
  CHECK(nxaudio_safe_exit_pump(&state, 100u, NULL, &poll) == NXAUDIO_INVALID);
  state.poll_interval_ns = 0u;
  CHECK(nxaudio_safe_exit_pump(&state, 100u, fake_poll_fn, &poll) ==
        NXAUDIO_INVALID);
  state = safe_exit_state();
  state.api_version = 9u;
  CHECK(nxaudio_safe_exit_pump(&state, 100u, fake_poll_fn, &poll) ==
        NXAUDIO_INVALID);
  return 0;
}

struct fake_recovery {
  int recover_calls;
  int reopen_calls;
  int recover_result;
  int reopen_result;
};

static int fake_recover_step(void *user) {
  struct fake_recovery *fake = (struct fake_recovery *)user;
  fake->recover_calls += 1;
  return fake->recover_result;
}

static int fake_reopen_step(void *user) {
  struct fake_recovery *fake = (struct fake_recovery *)user;
  fake->reopen_calls += 1;
  return fake->reopen_result;
}

static int test_backend_recovery(void) {
  nxaudio_backend_recovery state;
  struct fake_recovery fake;
  char line[NXAUDIO_RECEIPT_LINE_MAX];
  char tiny[8];

  /* Native prepare/recover succeeds: reopening must not happen. */
  memset(&fake, 0, sizeof(fake));
  nxaudio_backend_recovery_init(&state);
  CHECK(nxaudio_backend_recovery_run(
            &state, NXAUDIO_RECOVERY_FAULT_XRUN_EPIPE,
            fake_recover_step, fake_reopen_step, &fake) == NXAUDIO_OK);
  CHECK(fake.recover_calls == 1 && fake.reopen_calls == 0);
  CHECK(state.attempts == 1u);
  CHECK(state.recover_status == NXAUDIO_RECOVERY_STEP_OK);
  CHECK(state.reopen_status == NXAUDIO_RECOVERY_STEP_NOT_ATTEMPTED);
  CHECK(state.outcome == NXAUDIO_RECOVERY_RECOVERED);
  CHECK(nxaudio_backend_recovery_format(&state, line, sizeof(line)) ==
        NXAUDIO_OK);
  CHECK(strcmp(line,
               "AUDIO-RECOVERY: fault=xrun-epipe attempt=1 recover=ok "
               "reopen=not-attempted result=recovered") == 0);

  /* Native recover fails; the same declared backend reopens successfully. */
  memset(&fake, 0, sizeof(fake));
  fake.recover_result = -1;
  nxaudio_backend_recovery_init(&state);
  CHECK(nxaudio_backend_recovery_run(
            &state, NXAUDIO_RECOVERY_FAULT_CALLBACK_STALLED,
            fake_recover_step, fake_reopen_step, &fake) == NXAUDIO_OK);
  CHECK(fake.recover_calls == 1 && fake.reopen_calls == 1);
  CHECK(state.recover_status == NXAUDIO_RECOVERY_STEP_FAILED);
  CHECK(state.reopen_status == NXAUDIO_RECOVERY_STEP_OK);
  CHECK(state.outcome == NXAUDIO_RECOVERY_REOPENED);
  CHECK(nxaudio_backend_recovery_format(&state, line, sizeof(line)) ==
        NXAUDIO_OK);
  CHECK(strcmp(line,
               "AUDIO-RECOVERY: fault=callback-stalled attempt=1 "
               "recover=failed reopen=ok result=reopened") == 0);

  /* One failed cycle is terminal; no callback runs a second time. */
  memset(&fake, 0, sizeof(fake));
  fake.recover_result = -1;
  fake.reopen_result = -2;
  nxaudio_backend_recovery_init(&state);
  CHECK(nxaudio_backend_recovery_run(
            &state, NXAUDIO_RECOVERY_FAULT_DEVICE_LOST,
            fake_recover_step, fake_reopen_step, &fake) ==
        NXAUDIO_UNSUPPORTED);
  CHECK(state.outcome == NXAUDIO_RECOVERY_FAILED);
  CHECK(fake.recover_calls == 1 && fake.reopen_calls == 1);
  CHECK(nxaudio_backend_recovery_run(
            &state, NXAUDIO_RECOVERY_FAULT_DEVICE_LOST,
            fake_recover_step, fake_reopen_step, &fake) ==
        NXAUDIO_UNSUPPORTED);
  CHECK(state.outcome == NXAUDIO_RECOVERY_EXHAUSTED);
  CHECK(state.attempts == 1u);
  CHECK(fake.recover_calls == 1 && fake.reopen_calls == 1);
  CHECK(nxaudio_backend_recovery_format(&state, line, sizeof(line)) ==
        NXAUDIO_OK);
  CHECK(strcmp(line,
               "AUDIO-RECOVERY: fault=device-lost attempt=1 "
               "recover=not-attempted reopen=not-attempted "
               "result=exhausted") == 0);

  /* EPIPE classification is narrow and sign-independent. */
  CHECK(nxaudio_recovery_fault_from_errno(EPIPE) ==
        NXAUDIO_RECOVERY_FAULT_XRUN_EPIPE);
  CHECK(nxaudio_recovery_fault_from_errno(-EPIPE) ==
        NXAUDIO_RECOVERY_FAULT_XRUN_EPIPE);
  CHECK(nxaudio_recovery_fault_from_errno(0) ==
        NXAUDIO_RECOVERY_FAULT_NONE);

  /* Fail closed: invalid state/fault/no real operation/truncated receipt. */
  nxaudio_backend_recovery_init(&state);
  CHECK(nxaudio_backend_recovery_run(
            &state, NXAUDIO_RECOVERY_FAULT_NONE,
            fake_recover_step, fake_reopen_step, &fake) == NXAUDIO_INVALID);
  CHECK(nxaudio_backend_recovery_run(
            &state, NXAUDIO_RECOVERY_FAULT_DEVICE_LOST,
            NULL, NULL, &fake) == NXAUDIO_INVALID);
  CHECK(state.attempts == 0u);
  CHECK(nxaudio_backend_recovery_format(&state, line, sizeof(line)) ==
        NXAUDIO_INVALID);
  state.fault = NXAUDIO_RECOVERY_FAULT_DEVICE_LOST;
  state.attempts = 1u;
  state.outcome = NXAUDIO_RECOVERY_FAILED;
  CHECK(nxaudio_backend_recovery_format(&state, tiny, sizeof(tiny)) ==
        NXAUDIO_FULL);
  CHECK(tiny[0] == '\0');
  CHECK(nxaudio_backend_recovery_run(
            NULL, NXAUDIO_RECOVERY_FAULT_DEVICE_LOST,
            fake_recover_step, fake_reopen_step, &fake) == NXAUDIO_INVALID);
  return 0;
}

int main(void) {
  if (test_receipt_format_canonical() || test_receipt_format_conversion() ||
      test_receipt_format_rejects() || test_probe_order() ||
      test_liveness() || test_safe_exit() || test_backend_recovery())
    return 1;
  printf("nxaudio-receipt-tests: OK\n");
  return 0;
}
