/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxaudio.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct nxaudio_stream {
  nxaudio_format format;
  uint8_t *ring;
  uint32_t capacity_frames;
  uint32_t frame_bytes;
  _Atomic uint32_t read_pos;
  _Atomic uint32_t write_pos;
  _Atomic int state;
  _Atomic uint32_t submitted_frames;
  _Atomic uint32_t consumed_frames;
  _Atomic uint32_t underrun_frames;
  _Atomic uint32_t silence_frames;
  _Atomic uint32_t device_loss_count;
};

static const nxaudio_adapter_contract nxaudio_contracts[] = {
    {NXAUDIO_STACK_SDL2, "nxcompat-sdl2-audio-v2", "framework", 0, 0},
    {NXAUDIO_STACK_OPENSL_ES, "tasm2-opensl-sdl-v1", "asm2_127", 0, 0},
    {NXAUDIO_STACK_OPENSL_ES, "castle-opensl-sdl-v1", "castleofillusion", 0,
     0},
    {NXAUDIO_STACK_OPENAL, "bully2-openal-v1", "bully2", 0, 1},
    {NXAUDIO_STACK_FMOD, "horizon-fmod-sdl-v1", "horizonchase", 0, 0},
    {NXAUDIO_STACK_FMOD_EX, "castle-fmodex-v1", "castleofillusion", 0, 0},
    {NXAUDIO_STACK_WWISE, "sor4-wwise-openal-glibc230-v1", "sor4", 1, 1},
};

static int nxaudio_bool_valid(int value) { return value == 0 || value == 1; }

static int nxaudio_format_valid(const nxaudio_format *format) {
  uint32_t sample_bytes;
  if (!format || format->frequency < 8000u || format->frequency > 192000u ||
      format->channels == 0u || format->channels > 8u ||
      format->period_frames == 0u || format->period_frames > 16384u ||
      format->latency_us == 0u || format->latency_us > 2000000u)
    return 0;
  sample_bytes = format->sample_format == NXAUDIO_SAMPLE_S16LE ? 2u :
                 format->sample_format == NXAUDIO_SAMPLE_F32LE ? 4u : 0u;
  return sample_bytes != 0u && sample_bytes * format->channels <= 32u;
}

static uint32_t nxaudio_frame_bytes(const nxaudio_format *format) {
  return (format->sample_format == NXAUDIO_SAMPLE_S16LE ? 2u : 4u) *
         format->channels;
}

static int nxaudio_bounded_string(const char *text, size_t limit) {
  return text && memchr(text, '\0', limit) != NULL;
}

static int nxaudio_path_safe(const char *path) {
  const char *cursor;
  size_t length;
  if (!path || path[0] != '/')
    return 0;
  length = strnlen(path, NXAUDIO_PATH_MAX);
  if (length == 0u || length >= NXAUDIO_PATH_MAX)
    return 0;
  for (cursor = path; *cursor; ++cursor) {
    if ((cursor == path || cursor[-1] == '/') && cursor[0] == '.' &&
        (cursor[1] == '/' || cursor[1] == '\0' ||
         (cursor[1] == '.' && (cursor[2] == '/' || cursor[2] == '\0'))))
      return 0;
  }
  return 1;
}

nxaudio_result nxaudio_stream_create(const nxaudio_stream_options *options,
                                     nxaudio_stream **out_stream) {
  nxaudio_stream *stream;
  size_t bytes;
  if (!out_stream)
    return NXAUDIO_INVALID;
  *out_stream = NULL;
  if (!options || options->api_version != NXAUDIO_API_VERSION ||
      options->struct_size < sizeof(*options) ||
      !nxaudio_format_valid(&options->format) ||
      options->capacity_frames < options->format.period_frames * 2u ||
      options->capacity_frames > 1048576u)
    return NXAUDIO_INVALID;
  stream = (nxaudio_stream *)calloc(1u, sizeof(*stream));
  if (!stream)
    return NXAUDIO_FULL;
  stream->frame_bytes = nxaudio_frame_bytes(&options->format);
  bytes = (size_t)options->capacity_frames * stream->frame_bytes;
  stream->ring = (uint8_t *)malloc(bytes);
  if (!stream->ring) {
    free(stream);
    return NXAUDIO_FULL;
  }
  stream->format = options->format;
  stream->capacity_frames = options->capacity_frames;
  atomic_init(&stream->read_pos, 0u);
  atomic_init(&stream->write_pos, 0u);
  atomic_init(&stream->state, NXAUDIO_STREAM_READY);
  atomic_init(&stream->submitted_frames, 0u);
  atomic_init(&stream->consumed_frames, 0u);
  atomic_init(&stream->underrun_frames, 0u);
  atomic_init(&stream->silence_frames, 0u);
  atomic_init(&stream->device_loss_count, 0u);
  *out_stream = stream;
  return NXAUDIO_OK;
}

static nxaudio_result nxaudio_transition(nxaudio_stream *stream, int from,
                                         int to) {
  if (!stream)
    return NXAUDIO_INVALID;
  return atomic_compare_exchange_strong(&stream->state, &from, to)
             ? NXAUDIO_OK
             : NXAUDIO_WRONG_STATE;
}

nxaudio_result nxaudio_stream_start(nxaudio_stream *stream) {
  return nxaudio_transition(stream, NXAUDIO_STREAM_READY,
                            NXAUDIO_STREAM_RUNNING);
}

nxaudio_result nxaudio_stream_pause(nxaudio_stream *stream) {
  return nxaudio_transition(stream, NXAUDIO_STREAM_RUNNING,
                            NXAUDIO_STREAM_PAUSED);
}

nxaudio_result nxaudio_stream_resume(nxaudio_stream *stream) {
  return nxaudio_transition(stream, NXAUDIO_STREAM_PAUSED,
                            NXAUDIO_STREAM_RUNNING);
}

nxaudio_result nxaudio_stream_mark_device_lost(nxaudio_stream *stream) {
  int state;
  if (!stream)
    return NXAUDIO_INVALID;
  state = atomic_load_explicit(&stream->state, memory_order_acquire);
  if (state != NXAUDIO_STREAM_RUNNING && state != NXAUDIO_STREAM_PAUSED)
    return NXAUDIO_WRONG_STATE;
  if (!atomic_compare_exchange_strong(&stream->state, &state,
                                      NXAUDIO_STREAM_DEVICE_LOST))
    return NXAUDIO_WRONG_STATE;
  atomic_fetch_add_explicit(&stream->device_loss_count, 1u,
                            memory_order_relaxed);
  return NXAUDIO_OK;
}

nxaudio_result nxaudio_stream_recover(nxaudio_stream *stream, int resume) {
  if (!nxaudio_bool_valid(resume))
    return NXAUDIO_INVALID;
  return nxaudio_transition(stream, NXAUDIO_STREAM_DEVICE_LOST,
                            resume ? NXAUDIO_STREAM_RUNNING
                                   : NXAUDIO_STREAM_PAUSED);
}

nxaudio_result nxaudio_worker_submit(nxaudio_stream *stream, const void *pcm,
                                     uint32_t frames, uint32_t *written) {
  uint32_t read_pos, write_pos, queued, available, first;
  if (written)
    *written = 0u;
  if (!stream || !pcm || !written || frames == 0u)
    return NXAUDIO_INVALID;
  if (atomic_load_explicit(&stream->state, memory_order_acquire) ==
      NXAUDIO_STREAM_CLOSED)
    return NXAUDIO_WRONG_STATE;
  read_pos = atomic_load_explicit(&stream->read_pos, memory_order_acquire);
  write_pos = atomic_load_explicit(&stream->write_pos, memory_order_relaxed);
  queued = write_pos - read_pos;
  available = stream->capacity_frames - queued;
  if (frames > available)
    frames = available;
  if (frames == 0u)
    return NXAUDIO_FULL;
  first = stream->capacity_frames - (write_pos % stream->capacity_frames);
  if (first > frames)
    first = frames;
  memcpy(stream->ring + (size_t)(write_pos % stream->capacity_frames) *
                            stream->frame_bytes,
         pcm, (size_t)first * stream->frame_bytes);
  if (first < frames)
    memcpy(stream->ring,
           (const uint8_t *)pcm + (size_t)first * stream->frame_bytes,
           (size_t)(frames - first) * stream->frame_bytes);
  atomic_store_explicit(&stream->write_pos, write_pos + frames,
                        memory_order_release);
  atomic_fetch_add_explicit(&stream->submitted_frames, frames,
                            memory_order_relaxed);
  *written = frames;
  return NXAUDIO_OK;
}

nxaudio_result nxaudio_realtime_pull(nxaudio_stream *stream, void *pcm,
                                     uint32_t frames,
                                     nxaudio_pull_result *result) {
  uint32_t read_pos, write_pos, queued, copied, first;
  int state;
  if (!stream || !pcm || !result || frames == 0u)
    return NXAUDIO_INVALID;
  memset(result, 0, sizeof(*result));
  result->frames_requested = frames;
  memset(pcm, 0, (size_t)frames * stream->frame_bytes);
  state = atomic_load_explicit(&stream->state, memory_order_acquire);
  if (state == NXAUDIO_STREAM_PAUSED || state == NXAUDIO_STREAM_DEVICE_LOST ||
      state == NXAUDIO_STREAM_READY) {
    result->silence_frames = frames;
    result->reason = state == NXAUDIO_STREAM_DEVICE_LOST
                         ? NXAUDIO_REASON_DEVICE_LOST
                         : NXAUDIO_REASON_PAUSED;
    atomic_fetch_add_explicit(&stream->silence_frames, frames,
                              memory_order_relaxed);
    return NXAUDIO_OK;
  }
  if (state != NXAUDIO_STREAM_RUNNING)
    return NXAUDIO_WRONG_STATE;
  read_pos = atomic_load_explicit(&stream->read_pos, memory_order_relaxed);
  write_pos = atomic_load_explicit(&stream->write_pos, memory_order_acquire);
  queued = write_pos - read_pos;
  copied = queued < frames ? queued : frames;
  first = stream->capacity_frames - (read_pos % stream->capacity_frames);
  if (first > copied)
    first = copied;
  memcpy(pcm,
         stream->ring + (size_t)(read_pos % stream->capacity_frames) *
                            stream->frame_bytes,
         (size_t)first * stream->frame_bytes);
  if (first < copied)
    memcpy((uint8_t *)pcm + (size_t)first * stream->frame_bytes, stream->ring,
           (size_t)(copied - first) * stream->frame_bytes);
  atomic_store_explicit(&stream->read_pos, read_pos + copied,
                        memory_order_release);
  atomic_fetch_add_explicit(&stream->consumed_frames, copied,
                            memory_order_relaxed);
  result->frames_copied = copied;
  result->silence_frames = frames - copied;
  if (copied < frames) {
    result->reason = NXAUDIO_REASON_MIXER_STARVED;
    atomic_fetch_add_explicit(&stream->underrun_frames, frames - copied,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&stream->silence_frames, frames - copied,
                              memory_order_relaxed);
  }
  return NXAUDIO_OK;
}

nxaudio_result nxaudio_stream_get_stats(const nxaudio_stream *stream,
                                        nxaudio_stream_stats *stats) {
  uint32_t read_pos, write_pos;
  if (!stream || !stats)
    return NXAUDIO_INVALID;
  read_pos = atomic_load_explicit(&stream->read_pos, memory_order_acquire);
  write_pos = atomic_load_explicit(&stream->write_pos, memory_order_acquire);
  memset(stats, 0, sizeof(*stats));
  stats->state = (nxaudio_stream_state)atomic_load(&stream->state);
  stats->format = stream->format;
  stats->queued_frames = write_pos - read_pos;
  stats->submitted_frames = atomic_load(&stream->submitted_frames);
  stats->consumed_frames = atomic_load(&stream->consumed_frames);
  stats->underrun_frames = atomic_load(&stream->underrun_frames);
  stats->silence_frames = atomic_load(&stream->silence_frames);
  stats->device_loss_count = atomic_load(&stream->device_loss_count);
  return NXAUDIO_OK;
}

void nxaudio_stream_close(nxaudio_stream **stream_ptr) {
  nxaudio_stream *stream;
  if (!stream_ptr || !*stream_ptr)
    return;
  stream = *stream_ptr;
  *stream_ptr = NULL;
  atomic_store_explicit(&stream->state, NXAUDIO_STREAM_CLOSED,
                        memory_order_release);
  memset(stream->ring, 0,
         (size_t)stream->capacity_frames * stream->frame_bytes);
  free(stream->ring);
  free(stream);
}

static int nxaudio_name_equals(const char *left, const char *right) {
  size_t index;
  if (!left || !right)
    return 0;
  for (index = 0u; left[index] && right[index]; ++index) {
    char a = left[index], b = right[index];
    if (a >= 'A' && a <= 'Z')
      a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z')
      b = (char)(b - 'A' + 'a');
    if (a != b)
      return 0;
  }
  return left[index] == '\0' && right[index] == '\0';
}

nxaudio_result nxaudio_classify_backend(
    const nxaudio_backend_observation *observation, nxaudio_reason *reason) {
  if (!observation || !reason ||
      observation->api_version != NXAUDIO_API_VERSION ||
      observation->struct_size < sizeof(*observation) ||
      !nxaudio_bounded_string(observation->backend,
                              sizeof(observation->backend)) ||
      observation->backend[0] == '\0' ||
      !nxaudio_bool_valid(observation->inherited_attempt) ||
      !nxaudio_bool_valid(observation->server_reachable) ||
      !nxaudio_bool_valid(observation->device_opened))
    return NXAUDIO_INVALID;
  if (nxaudio_name_equals(observation->backend, "dummy") ||
      nxaudio_name_equals(observation->backend, "disk")) {
    *reason = NXAUDIO_REASON_FAKE_BACKEND;
    return NXAUDIO_UNSUPPORTED;
  }
  if (!observation->server_reachable) {
    *reason = NXAUDIO_REASON_SERVER_UNAVAILABLE;
    return NXAUDIO_UNSUPPORTED;
  }
  if (!observation->device_opened) {
    *reason = NXAUDIO_REASON_DEVICE_OPEN_FAILED;
    return NXAUDIO_UNSUPPORTED;
  }
  if (observation->callback_count && !observation->produced_frames) {
    *reason = NXAUDIO_REASON_MIXER_STARVED;
    return NXAUDIO_UNSUPPORTED;
  }
  *reason = observation->inherited_attempt
                ? NXAUDIO_REASON_INHERITED_BACKEND
                : NXAUDIO_REASON_AUTODETECT_BACKEND;
  return NXAUDIO_OK;
}

nxaudio_result nxaudio_plan_asoundrc(
    const nxaudio_asoundrc_options *options,
    nxaudio_environment_plan *plan) {
  int written;
  if (!options || !plan || options->api_version != NXAUDIO_API_VERSION ||
      options->struct_size < sizeof(*options) ||
      !nxaudio_bool_valid(options->isolated_home) ||
      !nxaudio_bool_valid(options->asoundrc_required) ||
      !nxaudio_bool_valid(options->asoundrc_present))
    return NXAUDIO_INVALID;
  memset(plan, 0, sizeof(*plan));
  if (!options->isolated_home || !options->asoundrc_required)
    return NXAUDIO_OK;
  if (!options->asoundrc_present || !nxaudio_path_safe(options->home)) {
    plan->reason = NXAUDIO_REASON_ASOUNDRC_REQUIRED;
    return NXAUDIO_UNSUPPORTED;
  }
  written = snprintf(plan->value, sizeof(plan->value), "%s/.asoundrc",
                     options->home);
  if (written < 0 || (size_t)written >= sizeof(plan->value)) {
    memset(plan, 0, sizeof(*plan));
    return NXAUDIO_INVALID;
  }
  memcpy(plan->name, "ALSA_CONFIG_PATH", sizeof("ALSA_CONFIG_PATH"));
  plan->expose = 1;
  plan->reason = NXAUDIO_REASON_ASOUNDRC_EXPOSED;
  return NXAUDIO_OK;
}

size_t nxaudio_adapter_contract_count(void) {
  return sizeof(nxaudio_contracts) / sizeof(nxaudio_contracts[0]);
}

const nxaudio_adapter_contract *nxaudio_adapter_contract_at(size_t index) {
  return index < nxaudio_adapter_contract_count() ? &nxaudio_contracts[index]
                                                   : NULL;
}

nxaudio_result nxaudio_adapter_validate(const nxaudio_adapter_request *request,
                                        nxaudio_reason *reason) {
  size_t index;
  const nxaudio_adapter_contract *contract = NULL;
  if (!request || !reason || request->api_version != NXAUDIO_API_VERSION ||
      request->struct_size < sizeof(*request) ||
      request->stack < NXAUDIO_STACK_SDL2 || request->stack > NXAUDIO_STACK_WWISE ||
      !nxaudio_bool_valid(request->guest_uses_stack) ||
      !nxaudio_bool_valid(request->canonical_recipe) ||
      !nxaudio_bool_valid(request->bundles_external_provider) ||
      !nxaudio_bounded_string(request->contract_id, NXAUDIO_NAME_MAX))
    return NXAUDIO_INVALID;
  if (!request->guest_uses_stack) {
    *reason = NXAUDIO_REASON_CONTRACT_UNPROVEN;
    return NXAUDIO_UNSUPPORTED;
  }
  for (index = 0u; index < nxaudio_adapter_contract_count(); ++index) {
    if (nxaudio_contracts[index].stack == request->stack &&
        strcmp(nxaudio_contracts[index].contract_id, request->contract_id) == 0) {
      contract = &nxaudio_contracts[index];
      break;
    }
  }
  /* AAudio deliberately has no generic contract. It stays unavailable until
   * an approved guest proves its exact API and lifecycle. */
  if (!contract) {
    *reason = NXAUDIO_REASON_CONTRACT_UNPROVEN;
    return NXAUDIO_UNSUPPORTED;
  }
  if ((contract->canonical_recipe_required && !request->canonical_recipe) ||
      (contract->external_provider_forbidden &&
       request->bundles_external_provider)) {
    *reason = NXAUDIO_REASON_PROVIDER_INCOMPATIBLE;
    return NXAUDIO_UNSUPPORTED;
  }
  *reason = NXAUDIO_REASON_NONE;
  return NXAUDIO_OK;
}

nxaudio_result nxaudio_verify_audibility(
    const nxaudio_audibility_evidence *evidence, nxaudio_reason *reason) {
  if (!evidence || !reason || evidence->api_version != NXAUDIO_API_VERSION ||
      evidence->struct_size < sizeof(*evidence) ||
      evidence->scope < NXAUDIO_EVIDENCE_SYNTHETIC ||
      evidence->scope > NXAUDIO_EVIDENCE_CURRENT_PHYSICAL ||
      !nxaudio_bool_valid(evidence->device_opened) ||
      !nxaudio_bool_valid(evidence->human_audible_confirmed))
    return NXAUDIO_INVALID;
  if (evidence->scope == NXAUDIO_EVIDENCE_SYNTHETIC ||
      !evidence->device_opened || !evidence->human_audible_confirmed ||
      !evidence->produced_frames || !evidence->nonzero_samples ||
      !evidence->peak) {
    *reason = NXAUDIO_REASON_AUDIBILITY_UNPROVEN;
    return NXAUDIO_UNSUPPORTED;
  }
  *reason = NXAUDIO_REASON_AUDIBLE_CONFIRMED;
  return NXAUDIO_OK;
}

const char *nxaudio_reason_name(nxaudio_reason reason) {
  static const char *const names[] = {
      "none",          "inherited-backend", "autodetect-backend",
      "fake-backend",  "server-unavailable", "device-open-failed",
      "mixer-starved", "device-lost",       "paused",
      "clean-shutdown", "asoundrc-required", "asoundrc-exposed",
      "contract-unproven", "provider-incompatible",
      "audibility-unproven", "audible-confirmed"};
  return reason >= NXAUDIO_REASON_NONE && reason <= NXAUDIO_REASON_AUDIBLE_CONFIRMED
             ? names[reason]
             : "unknown";
}

const char *nxaudio_stack_name(nxaudio_stack stack) {
  static const char *const names[] = {"sdl2", "opensl-es", "aaudio", "openal",
                                      "fmod", "fmod-ex", "wwise"};
  return stack >= NXAUDIO_STACK_SDL2 && stack <= NXAUDIO_STACK_WWISE
             ? names[stack]
             : "unknown";
}
