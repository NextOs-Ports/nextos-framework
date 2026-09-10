/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXAUDIO_H
#define NXAUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXAUDIO_API_VERSION 1u
#define NXAUDIO_NAME_MAX 48u
#define NXAUDIO_PATH_MAX 512u

typedef enum nxaudio_result {
  NXAUDIO_OK = 0,
  NXAUDIO_INVALID = -1,
  NXAUDIO_WRONG_STATE = -2,
  NXAUDIO_FULL = -3,
  NXAUDIO_UNSUPPORTED = -4
} nxaudio_result;

typedef enum nxaudio_reason {
  NXAUDIO_REASON_NONE = 0,
  NXAUDIO_REASON_INHERITED_BACKEND = 1,
  NXAUDIO_REASON_AUTODETECT_BACKEND = 2,
  NXAUDIO_REASON_FAKE_BACKEND = 3,
  NXAUDIO_REASON_SERVER_UNAVAILABLE = 4,
  NXAUDIO_REASON_DEVICE_OPEN_FAILED = 5,
  NXAUDIO_REASON_MIXER_STARVED = 6,
  NXAUDIO_REASON_DEVICE_LOST = 7,
  NXAUDIO_REASON_PAUSED = 8,
  NXAUDIO_REASON_CLEAN_SHUTDOWN = 9,
  NXAUDIO_REASON_ASOUNDRC_REQUIRED = 10,
  NXAUDIO_REASON_ASOUNDRC_EXPOSED = 11,
  NXAUDIO_REASON_CONTRACT_UNPROVEN = 12,
  NXAUDIO_REASON_PROVIDER_INCOMPATIBLE = 13,
  NXAUDIO_REASON_AUDIBILITY_UNPROVEN = 14,
  NXAUDIO_REASON_AUDIBLE_CONFIRMED = 15
} nxaudio_reason;

typedef enum nxaudio_sample_format {
  NXAUDIO_SAMPLE_S16LE = 1,
  NXAUDIO_SAMPLE_F32LE = 2
} nxaudio_sample_format;

typedef struct nxaudio_format {
  uint32_t frequency;
  nxaudio_sample_format sample_format;
  uint16_t channels;
  uint16_t period_frames;
  uint32_t latency_us;
} nxaudio_format;

typedef enum nxaudio_stream_state {
  NXAUDIO_STREAM_READY = 0,
  NXAUDIO_STREAM_RUNNING = 1,
  NXAUDIO_STREAM_PAUSED = 2,
  NXAUDIO_STREAM_DEVICE_LOST = 3,
  NXAUDIO_STREAM_CLOSED = 4
} nxaudio_stream_state;

typedef struct nxaudio_stream nxaudio_stream;

typedef struct nxaudio_stream_options {
  uint32_t api_version;
  size_t struct_size;
  nxaudio_format format;
  uint32_t capacity_frames;
} nxaudio_stream_options;

typedef struct nxaudio_pull_result {
  uint32_t frames_requested;
  uint32_t frames_copied;
  uint32_t silence_frames;
  nxaudio_reason reason;
} nxaudio_pull_result;

typedef struct nxaudio_stream_stats {
  nxaudio_stream_state state;
  nxaudio_format format;
  uint32_t queued_frames;
  uint32_t submitted_frames;
  uint32_t consumed_frames;
  uint32_t underrun_frames;
  uint32_t silence_frames;
  uint32_t device_loss_count;
} nxaudio_stream_stats;

/* The stream is single-producer/single-consumer. The worker is the only
 * producer; the realtime callback is the only consumer. The consumer copies
 * or zero-fills PCM and never invokes guest code, allocates or takes a lock. */
nxaudio_result nxaudio_stream_create(const nxaudio_stream_options *options,
                                     nxaudio_stream **out_stream);
nxaudio_result nxaudio_stream_start(nxaudio_stream *stream);
nxaudio_result nxaudio_stream_pause(nxaudio_stream *stream);
nxaudio_result nxaudio_stream_resume(nxaudio_stream *stream);
nxaudio_result nxaudio_stream_mark_device_lost(nxaudio_stream *stream);
nxaudio_result nxaudio_stream_recover(nxaudio_stream *stream, int resume);
nxaudio_result nxaudio_worker_submit(nxaudio_stream *stream, const void *pcm,
                                     uint32_t frames, uint32_t *written);
nxaudio_result nxaudio_realtime_pull(nxaudio_stream *stream, void *pcm,
                                     uint32_t frames,
                                     nxaudio_pull_result *result);
nxaudio_result nxaudio_stream_get_stats(const nxaudio_stream *stream,
                                        nxaudio_stream_stats *stats);
/* The caller first quiesces its device callback, then closes the stream. */
void nxaudio_stream_close(nxaudio_stream **stream);

typedef struct nxaudio_backend_observation {
  uint32_t api_version;
  size_t struct_size;
  char backend[NXAUDIO_NAME_MAX];
  int inherited_attempt;
  int server_reachable;
  int device_opened;
  uint32_t callback_count;
  uint32_t produced_frames;
} nxaudio_backend_observation;

nxaudio_result nxaudio_classify_backend(
    const nxaudio_backend_observation *observation, nxaudio_reason *reason);

typedef struct nxaudio_asoundrc_options {
  uint32_t api_version;
  size_t struct_size;
  const char *home;
  int isolated_home;
  int asoundrc_required;
  int asoundrc_present;
} nxaudio_asoundrc_options;

typedef struct nxaudio_environment_plan {
  int expose;
  char name[NXAUDIO_NAME_MAX];
  char value[NXAUDIO_PATH_MAX];
  nxaudio_reason reason;
} nxaudio_environment_plan;

nxaudio_result nxaudio_plan_asoundrc(
    const nxaudio_asoundrc_options *options,
    nxaudio_environment_plan *plan);

typedef enum nxaudio_stack {
  NXAUDIO_STACK_SDL2 = 0,
  NXAUDIO_STACK_OPENSL_ES = 1,
  NXAUDIO_STACK_AAUDIO = 2,
  NXAUDIO_STACK_OPENAL = 3,
  NXAUDIO_STACK_FMOD = 4,
  NXAUDIO_STACK_FMOD_EX = 5,
  NXAUDIO_STACK_WWISE = 6
} nxaudio_stack;

typedef struct nxaudio_adapter_contract {
  nxaudio_stack stack;
  const char *contract_id;
  const char *port_id;
  int canonical_recipe_required;
  int external_provider_forbidden;
} nxaudio_adapter_contract;

typedef struct nxaudio_adapter_request {
  uint32_t api_version;
  size_t struct_size;
  nxaudio_stack stack;
  const char *contract_id;
  int guest_uses_stack;
  int canonical_recipe;
  int bundles_external_provider;
} nxaudio_adapter_request;

size_t nxaudio_adapter_contract_count(void);
const nxaudio_adapter_contract *nxaudio_adapter_contract_at(size_t index);
nxaudio_result nxaudio_adapter_validate(const nxaudio_adapter_request *request,
                                        nxaudio_reason *reason);

typedef enum nxaudio_evidence_scope {
  NXAUDIO_EVIDENCE_SYNTHETIC = 0,
  NXAUDIO_EVIDENCE_IMPORTED_APPROVED_PHYSICAL = 1,
  NXAUDIO_EVIDENCE_CURRENT_PHYSICAL = 2
} nxaudio_evidence_scope;

typedef struct nxaudio_audibility_evidence {
  uint32_t api_version;
  size_t struct_size;
  nxaudio_evidence_scope scope;
  int device_opened;
  int human_audible_confirmed;
  uint64_t produced_frames;
  uint64_t nonzero_samples;
  uint32_t peak;
} nxaudio_audibility_evidence;

nxaudio_result nxaudio_verify_audibility(
    const nxaudio_audibility_evidence *evidence, nxaudio_reason *reason);

const char *nxaudio_reason_name(nxaudio_reason reason);
const char *nxaudio_stack_name(nxaudio_stack stack);

#ifdef __cplusplus
}
#endif

#endif
