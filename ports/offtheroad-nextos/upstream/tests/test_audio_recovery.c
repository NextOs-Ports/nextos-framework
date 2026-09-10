/* SPDX-License-Identifier: GPL-3.0-only */
#include "otr_audio_recovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct recovery_probe {
  otr_audio_recovery *state;
  unsigned reopen_calls;
} recovery_probe;

static void require(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "test_audio_recovery: %s\n", message);
    exit(1);
  }
}

static int reopen_and_callback(void *opaque) {
  recovery_probe *probe = (recovery_probe *)opaque;
  probe->reopen_calls++;
  /* Runtime arms after SDL_OpenAudioDevice succeeds and before unpause. */
  otr_audio_recovery_arm_post_reopen(probe->state);
  otr_audio_recovery_note_callback(probe->state);
  return 0;
}

static int reopen_without_callback(void *opaque) {
  recovery_probe *probe = (recovery_probe *)opaque;
  probe->reopen_calls++;
  otr_audio_recovery_arm_post_reopen(probe->state);
  return 0;
}

int main(void) {
  otr_audio_recovery state;
  otr_audio_recovery timed_out;
  recovery_probe probe;
  recovery_probe timeout_probe;
  char line[OTR_AUDIO_RECOVERY_LINE_MAX];

  otr_audio_recovery_init(&state);
  probe.state = &state;
  probe.reopen_calls = 0u;
  require(otr_audio_recovery_format(&state, line, sizeof(line)) == 0 &&
              strstr(line, "fault=none") != NULL &&
              strstr(line, "result=not-needed") != NULL,
          "not-needed evidence is missing");
  require(otr_audio_recovery_run_callback_stalled(
              &state, reopen_and_callback, &probe) == NXAUDIO_OK,
          "measured callback stall did not reopen");
  require(probe.reopen_calls == 1u &&
              otr_audio_recovery_attempt_count(&state) == 1u &&
              state.backend.fault ==
                  NXAUDIO_RECOVERY_FAULT_CALLBACK_STALLED &&
              state.backend.outcome == NXAUDIO_RECOVERY_REOPENED,
          "reopen result/fault was not recorded honestly");
  require(otr_audio_recovery_callback_confirmed(&state) == 1 &&
              otr_audio_recovery_callback_count(&state) == 1u,
          "replacement callback was not observed");
  require(otr_audio_recovery_format(&state, line, sizeof(line)) == 0 &&
              strstr(line, "fault=callback-stalled") != NULL &&
              strstr(line, "attempt=1") != NULL &&
              strstr(line, "reopen=ok") != NULL &&
              strstr(line, "result=reopened") != NULL &&
              strstr(line, "callback_post_reopen=confirmed") != NULL &&
              strstr(line, "adapter_outcome=recovered") != NULL,
          "recovery evidence is incomplete");

  require(otr_audio_recovery_run_callback_stalled(
              &state, reopen_and_callback, &probe) == NXAUDIO_UNSUPPORTED &&
              probe.reopen_calls == 1u &&
              state.backend.outcome == NXAUDIO_RECOVERY_REOPENED,
          "bounded recovery invoked reopen more than once");

  otr_audio_recovery_init(&timed_out);
  timeout_probe.state = &timed_out;
  timeout_probe.reopen_calls = 0u;
  require(otr_audio_recovery_run_callback_stalled(
              &timed_out, reopen_without_callback, &timeout_probe) ==
              NXAUDIO_OK,
          "timeout fixture did not open its replacement");
  otr_audio_recovery_mark_callback_timeout(&timed_out);
  require(otr_audio_recovery_format(&timed_out, line, sizeof(line)) == 0 &&
              strstr(line, "callback_post_reopen=missing") != NULL &&
              strstr(line, "adapter_outcome=failed") != NULL,
          "missing replacement callback was not a failed outcome");
  puts("test_audio_recovery: PASS");
  return 0;
}
