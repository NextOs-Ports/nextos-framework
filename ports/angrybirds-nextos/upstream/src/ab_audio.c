/* ab_audio.c — Fusion mixer worker -> nxaudio SPSC -> SDL callback.
 *
 * The APK's nativeMixData still produces every sample in its original order.
 * Guest code never runs in SDL's realtime thread: the worker fills nxaudio's
 * lock-free stream and the callback only pulls PCM or deterministic silence.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <SDL.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ab_audio_policy.h"
#include "ab_port.h"
#include "nxaudio.h"

extern void *ab_jni_new_byte_array(int length);
extern void *ab_jni_array_data(void *array);
extern void *ab_jni_new_object(const char *cls);

typedef void(SDLCALL *MixDataFn)(void *env, void *obj, int64_t mixer,
                                 void *array, int32_t length);

static MixDataFn g_mix;
static void *g_this;
static void *g_array;
static uint8_t *g_array_data;
static int g_array_bytes;
static int64_t g_mixer;
static SDL_AudioDeviceID g_device;
static int g_rate, g_channels, g_bits, g_guest_frame_bytes;
static uint32_t g_period_frames, g_mix_frames;
static nxaudio_stream *g_stream;
static SDL_Thread *g_worker;
static SDL_atomic_t g_worker_running;
static int16_t *g_convert;
static uint32_t g_mix_calls, g_mix_slow_calls, g_mix_max_ms;

void ab_audio_stop(void);

static void SDLCALL audio_callback(void *userdata, Uint8 *output, int length) {
  nxaudio_pull_result result;
  uint32_t frames;
  (void)userdata;
  if (!g_stream || length <= 0) {
    if (length > 0)
      SDL_memset(output, 0, (size_t)length);
    return;
  }
  frames = (uint32_t)length / (uint32_t)(g_channels * (int)sizeof(int16_t));
  if (nxaudio_realtime_pull(g_stream, output, frames, &result) != NXAUDIO_OK)
    SDL_memset(output, 0, (size_t)length);
}

static int SDLCALL mixer_worker(void *userdata) {
  uint32_t frames = g_mix_frames;
  int guest_bytes = (int)frames * g_guest_frame_bytes;
  (void)userdata;

  while (SDL_AtomicGet(&g_worker_running)) {
    const void *pcm;
    uint32_t submitted = 0u;
    uint32_t offset = 0u;
    uint32_t written = 0u;
    uint32_t mix_started;
    uint32_t mix_elapsed;
    nxaudio_result result;
    int i;

    memset(g_array_data, g_bits == 8 ? 0x80 : 0, (size_t)guest_bytes);
    mix_started = SDL_GetTicks();
    g_mix(ab_jni_env(), g_this, g_mixer, g_array, guest_bytes);
    mix_elapsed = SDL_GetTicks() - mix_started;
    g_mix_calls++;
    if (mix_elapsed > g_mix_max_ms)
      g_mix_max_ms = mix_elapsed;
    if ((uint64_t)mix_elapsed * (uint32_t)g_rate >
        (uint64_t)frames * 1000u)
      g_mix_slow_calls++;
    if (g_bits == 8) {
      for (i = 0; i < guest_bytes; i++)
        g_convert[i] = (int16_t)(((int)g_array_data[i] - 128) << 8);
      pcm = g_convert;
    } else {
      pcm = g_array_data;
    }

    while (submitted < frames && SDL_AtomicGet(&g_worker_running)) {
      offset = submitted * (uint32_t)g_channels;
      result = nxaudio_worker_submit(
          g_stream, (const int16_t *)pcm + offset, frames - submitted,
          &written);
      submitted += written;
      if (result == NXAUDIO_FULL || written == 0u)
        SDL_Delay(1);
      else if (result != NXAUDIO_OK)
        break;
    }
  }
  return 0;
}

void ab_audio_create(int64_t mixer, int rate, int channels, int bits,
                     int bufbytes) {
  g_mixer = mixer;
  g_rate = rate > 0 ? rate : 44100;
  g_channels = channels == 1 ? 1 : 2;
  g_bits = bits == 8 ? 8 : 16;
  g_guest_frame_bytes = (g_bits / 8) * g_channels;
  if (bufbytes < 4096)
    bufbytes = 4096;
  if (bufbytes > 262144)
    bufbytes = 262144;
  if (!g_array) {
    g_array_bytes = bufbytes * 2;
    g_array = ab_jni_new_byte_array(g_array_bytes);
    g_array_data = ab_jni_array_data(g_array);
  }
  if (!g_this)
    g_this = ab_jni_new_object("com/rovio/fusion/AudioOutput");
  if (!g_mix)
    g_mix =
        (MixDataFn)ab_sym("Java_com_rovio_fusion_AudioOutput_nativeMixData");
  if (!g_mix)
    ab_log("[audio] export nativeMixData nao encontrado");
}

int ab_audio_start(void) {
  SDL_AudioSpec want, have;
  nxaudio_stream_options options;
  nxaudio_stream_stats prefill_stats;
  uint32_t queue_frames;
  uint32_t queue_us;
  uint32_t software_lead_us;
  uint32_t prefill_started;
  uint32_t prefill_elapsed;
  int samples = (int)AB_AUDIO_PERIOD_FRAMES;

  if (g_device)
    return 1;
  if (!g_mix || !g_array_data)
    return 0;

  SDL_zero(want);
  want.freq = g_rate;
  want.format = AUDIO_S16SYS;
  want.channels = (Uint8)g_channels;
  want.samples = (Uint16)samples;
  want.callback = audio_callback;
  g_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (!g_device) {
    ab_log("[audio] SDL_OpenAudioDevice falhou: %s", SDL_GetError());
    return 0;
  }

  memset(&options, 0, sizeof(options));
  options.api_version = NXAUDIO_API_VERSION;
  options.struct_size = sizeof(options);
  options.format.frequency = (uint32_t)have.freq;
  options.format.sample_format = NXAUDIO_SAMPLE_S16LE;
  options.format.channels = have.channels;
  options.format.period_frames = have.samples;
  options.format.latency_us =
      (uint32_t)((uint64_t)have.samples * 1000000u / (uint32_t)have.freq);
  queue_frames = ab_audio_queue_capacity_frames((uint32_t)have.samples);
  options.capacity_frames = queue_frames;
  if (nxaudio_stream_create(&options, &g_stream) != NXAUDIO_OK ||
      nxaudio_stream_start(g_stream) != NXAUDIO_OK) {
    ab_log("[audio] nxaudio %s nao abriu o stream", NXAUDIO_VERSION);
    SDL_CloseAudioDevice(g_device);
    g_device = 0;
    nxaudio_stream_close(&g_stream);
    return 0;
  }

  g_period_frames = have.samples;
  g_mix_frames = AB_AUDIO_MIX_FRAMES;
  if (g_mix_frames > (uint32_t)g_array_bytes / (uint32_t)g_guest_frame_bytes)
    g_mix_frames =
        (uint32_t)g_array_bytes / (uint32_t)g_guest_frame_bytes;
  if (g_mix_frames < g_period_frames)
    g_mix_frames = g_period_frames;
  if (g_bits == 8) {
    g_convert = (int16_t *)calloc((size_t)g_mix_frames * g_channels,
                                  sizeof(*g_convert));
    if (!g_convert) {
      ab_audio_stop();
      return 0;
    }
  }
  g_mix_calls = 0u;
  g_mix_slow_calls = 0u;
  g_mix_max_ms = 0u;
  SDL_AtomicSet(&g_worker_running, 1);
  g_worker = SDL_CreateThread(mixer_worker, "AngryBirdsMixer", NULL);
  if (!g_worker) {
    ab_log("[audio] worker nao iniciou: %s", SDL_GetError());
    ab_audio_stop();
    return 0;
  }

  /* SDL opens paused. Let the guest worker establish the short target queue
   * before the first realtime callback, otherwise slow thread startup becomes
   * a visible burst of silence even though steady-state mixing is healthy. */
  memset(&prefill_stats, 0, sizeof(prefill_stats));
  prefill_started = SDL_GetTicks();
  do {
    if (nxaudio_stream_get_stats(g_stream, &prefill_stats) != NXAUDIO_OK)
      break;
    if (prefill_stats.queued_frames >= queue_frames)
      break;
    SDL_Delay(1);
  } while (SDL_GetTicks() - prefill_started < AB_AUDIO_PREFILL_TIMEOUT_MS);
  prefill_elapsed = SDL_GetTicks() - prefill_started;

  queue_us = ab_audio_frames_to_us(queue_frames, (uint32_t)have.freq);
  software_lead_us = ab_audio_frames_to_us(
      ab_audio_software_lead_frames((uint32_t)have.samples, g_mix_frames),
      (uint32_t)have.freq);
  ab_log("[audio] nx%s: %d Hz, %d ch, S16, periodo=%d, mix=%u, "
         "fila=%u frames/%u ms, avanco-software<=%u ms (driver=%s)",
         NXAUDIO_VERSION, have.freq, have.channels, have.samples, g_mix_frames,
         queue_frames, (queue_us + 999u) / 1000u,
         (software_lead_us + 999u) / 1000u, SDL_GetCurrentAudioDriver());
  ab_log("[audio] prefill=%u/%u frames em %u ms",
         prefill_stats.queued_frames, queue_frames, prefill_elapsed);
  SDL_PauseAudioDevice(g_device, 0);
  return 1;
}

void ab_audio_stop(void) {
  nxaudio_stream_stats stats;

  if (g_device)
    SDL_PauseAudioDevice(g_device, 1);
  SDL_AtomicSet(&g_worker_running, 0);
  if (g_worker) {
    SDL_WaitThread(g_worker, NULL);
    g_worker = NULL;
  }
  if (g_stream && nxaudio_stream_get_stats(g_stream, &stats) == NXAUDIO_OK)
    ab_log("[audio] stop: fila=%u submetidos=%u consumidos=%u "
           "underrun=%u silencio=%u mix_calls=%u mix_slow=%u mix_max_ms=%u",
           stats.queued_frames, stats.submitted_frames, stats.consumed_frames,
           stats.underrun_frames, stats.silence_frames, g_mix_calls,
           g_mix_slow_calls, g_mix_max_ms);
  if (g_device) {
    SDL_CloseAudioDevice(g_device);
    g_device = 0;
  }
  nxaudio_stream_close(&g_stream);
  free(g_convert);
  g_convert = NULL;
}

void ab_audio_shutdown(void) { ab_audio_stop(); }
