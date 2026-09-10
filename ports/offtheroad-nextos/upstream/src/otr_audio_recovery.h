/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef OTR_AUDIO_RECOVERY_H
#define OTR_AUDIO_RECOVERY_H

#include "nxaudio_receipt.h"

#include <stdatomic.h>
#include <stddef.h>

#define OTR_AUDIO_RECOVERY_LINE_MAX 384u

typedef struct otr_audio_recovery {
  nxaudio_backend_recovery backend;
  atomic_bool post_reopen_armed;
  atomic_bool post_reopen_timeout;
  atomic_uint post_reopen_callbacks;
} otr_audio_recovery;

void otr_audio_recovery_init(otr_audio_recovery *state);

/* OTR only observes a callback stall through SDL2. It must not label this as
 * ALSA EPIPE without a backend error. The canonical helper still guarantees
 * that reopen_step can run at most once. */
nxaudio_result otr_audio_recovery_run_callback_stalled(
    otr_audio_recovery *state, nxaudio_recovery_step_fn reopen_step,
    void *user);

/* Called by the concrete SDL reopen step after a replacement device opened
 * and before it is unpaused; therefore an observed callback is necessarily
 * from the replacement, never from the device being closed. */
void otr_audio_recovery_arm_post_reopen(otr_audio_recovery *state);
void otr_audio_recovery_note_callback(otr_audio_recovery *state);
void otr_audio_recovery_mark_callback_timeout(otr_audio_recovery *state);
int otr_audio_recovery_callback_confirmed(const otr_audio_recovery *state);
unsigned otr_audio_recovery_callback_count(const otr_audio_recovery *state);
unsigned otr_audio_recovery_attempt_count(const otr_audio_recovery *state);
int otr_audio_recovery_format(const otr_audio_recovery *state, char *line,
                              size_t line_size);

#endif /* OTR_AUDIO_RECOVERY_H */
