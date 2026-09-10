/* ab_audio_policy.h — adapter-local low-latency buffering policy.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AB_AUDIO_POLICY_H
#define AB_AUDIO_POLICY_H

#include <stdint.h>

/* 1024 frames at 44.1 kHz is 23.22 ms. Six queued periods cover routine
 * 123 ms guest-mixer calls without reproducing the old eight-by-4096 frame
 * delay. Longer content-transition stalls become temporary silence instead
 * of permanent latency. */
#define AB_AUDIO_PERIOD_FRAMES 1024u
#define AB_AUDIO_QUEUE_PERIODS 6u
#define AB_AUDIO_MIX_FRAMES 4096u
#define AB_AUDIO_PREFILL_TIMEOUT_MS 2000u

static inline uint32_t ab_audio_queue_capacity_frames(uint32_t period_frames) {
  return period_frames * AB_AUDIO_QUEUE_PERIODS;
}

static inline uint32_t ab_audio_frames_to_us(uint32_t frames,
                                             uint32_t frequency) {
  if (frequency == 0u)
    return 0u;
  return (uint32_t)(((uint64_t)frames * 1000000u + frequency - 1u) /
                    frequency);
}

/* The producer may hold one complete guest-mix chunk while the SPSC ring is
 * full. Report that explicitly instead of pretending only the ring matters. */
static inline uint32_t ab_audio_software_lead_frames(uint32_t period_frames,
                                                     uint32_t mix_frames) {
  return ab_audio_queue_capacity_frames(period_frames) + mix_frames;
}

#endif
