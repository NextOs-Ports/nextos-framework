/* SPDX-License-Identifier: GPL-3.0-only */
#include "otr_audio_recovery.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void otr_audio_recovery_init(otr_audio_recovery *state) {
  if (state == NULL) return;
  memset(state, 0, sizeof(*state));
  nxaudio_backend_recovery_init(&state->backend);
  atomic_init(&state->post_reopen_armed, false);
  atomic_init(&state->post_reopen_timeout, false);
  atomic_init(&state->post_reopen_callbacks, 0u);
}

nxaudio_result otr_audio_recovery_run_callback_stalled(
    otr_audio_recovery *state, nxaudio_recovery_step_fn reopen_step,
    void *user) {
  nxaudio_result result;
  if (state == NULL || reopen_step == NULL) return NXAUDIO_INVALID;
  /* Preserve the first measured result. A second call cannot mutate it to a
   * misleading new outcome and, most importantly, never invokes reopen. */
  if (state->backend.attempts != 0u) return NXAUDIO_UNSUPPORTED;
  result = nxaudio_backend_recovery_run(
      &state->backend, NXAUDIO_RECOVERY_FAULT_CALLBACK_STALLED, NULL,
      reopen_step, user);
  if (state->backend.outcome != NXAUDIO_RECOVERY_REOPENED)
    atomic_store_explicit(&state->post_reopen_armed, false,
                          memory_order_release);
  return result;
}

void otr_audio_recovery_arm_post_reopen(otr_audio_recovery *state) {
  if (state == NULL) return;
  atomic_store_explicit(&state->post_reopen_callbacks, 0u,
                        memory_order_release);
  atomic_store_explicit(&state->post_reopen_timeout, false,
                        memory_order_release);
  atomic_store_explicit(&state->post_reopen_armed, true,
                        memory_order_release);
}

void otr_audio_recovery_note_callback(otr_audio_recovery *state) {
  if (state == NULL) return;
  if (atomic_exchange_explicit(&state->post_reopen_armed, false,
                               memory_order_acq_rel))
    atomic_fetch_add_explicit(&state->post_reopen_callbacks, 1u,
                              memory_order_relaxed);
}

void otr_audio_recovery_mark_callback_timeout(otr_audio_recovery *state) {
  if (state == NULL) return;
  atomic_store_explicit(&state->post_reopen_armed, false,
                        memory_order_release);
  atomic_store_explicit(&state->post_reopen_timeout, true,
                        memory_order_release);
}

int otr_audio_recovery_callback_confirmed(const otr_audio_recovery *state) {
  return state != NULL &&
                 atomic_load_explicit(&state->post_reopen_callbacks,
                                      memory_order_acquire) != 0u
             ? 1
             : 0;
}

unsigned otr_audio_recovery_callback_count(const otr_audio_recovery *state) {
  return state == NULL
             ? 0u
             : atomic_load_explicit(&state->post_reopen_callbacks,
                                    memory_order_acquire);
}

unsigned otr_audio_recovery_attempt_count(const otr_audio_recovery *state) {
  return state == NULL ? 0u : state->backend.attempts;
}

int otr_audio_recovery_format(const otr_audio_recovery *state, char *line,
                              size_t line_size) {
  char canonical[NXAUDIO_RECEIPT_LINE_MAX];
  const char *callback_status;
  const char *adapter_outcome;
  int written;
  if (line != NULL && line_size != 0u) line[0] = '\0';
  if (state == NULL || line == NULL || line_size == 0u) return -1;
  if (state->backend.attempts == 0u) {
    written = snprintf(
        line, line_size,
        "AUDIO-RECOVERY: fault=none attempt=0 recover=not-attempted "
        "reopen=not-attempted result=not-needed "
        "callback_post_reopen=not-required callbacks=0 "
        "adapter_outcome=not-needed");
  } else {
    if (nxaudio_backend_recovery_format(&state->backend, canonical,
                                        sizeof(canonical)) != NXAUDIO_OK)
      return -1;
    callback_status = otr_audio_recovery_callback_confirmed(state)
                          ? "confirmed"
                          : "missing";
    if (otr_audio_recovery_callback_confirmed(state))
      adapter_outcome = "recovered";
    else if (atomic_load_explicit(&state->post_reopen_timeout,
                                  memory_order_acquire) ||
             state->backend.outcome != NXAUDIO_RECOVERY_REOPENED)
      adapter_outcome = "failed";
    else
      adapter_outcome = "pending";
    written = snprintf(line, line_size, "%s callback_post_reopen=%s "
                                        "callbacks=%u adapter_outcome=%s",
                       canonical, callback_status,
                       otr_audio_recovery_callback_count(state),
                       adapter_outcome);
  }
  if (written < 0 || (size_t)written >= line_size) {
    line[0] = '\0';
    return -1;
  }
  return 0;
}
