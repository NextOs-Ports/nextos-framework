/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>

#include "../src/ab_audio_policy.h"

int main(void) {
  const uint32_t rate = 44100u;
  const uint32_t period = AB_AUDIO_PERIOD_FRAMES;
  const uint32_t queue = ab_audio_queue_capacity_frames(period);
  const uint32_t queue_us = ab_audio_frames_to_us(queue, rate);
  const uint32_t software_lead_us =
      ab_audio_frames_to_us(
          ab_audio_software_lead_frames(period, AB_AUDIO_MIX_FRAMES), rate);

  assert(period == 1024u);
  assert(queue == 6144u);
  assert(AB_AUDIO_MIX_FRAMES == 4096u);
  assert(AB_AUDIO_PREFILL_TIMEOUT_MS == 2000u);
  assert(ab_audio_frames_to_us(period, rate) == 23220u);
  assert(queue_us == 139320u);
  assert(software_lead_us == 232200u);
  assert(queue_us < 140000u);
  assert(software_lead_us < 235000u);

  printf("angrybirds audio policy: PASS period=%u queue=%u queue_us=%u "
         "software_lead_us=%u\n",
         period, queue, queue_us, software_lead_us);
  return 0;
}
