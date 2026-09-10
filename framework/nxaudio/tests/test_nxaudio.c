/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxaudio.h"

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

static nxaudio_stream_options stream_options(void) {
  nxaudio_stream_options options;
  memset(&options, 0, sizeof(options));
  options.api_version = NXAUDIO_API_VERSION;
  options.struct_size = sizeof(options);
  options.format.frequency = 32000u;
  options.format.sample_format = NXAUDIO_SAMPLE_S16LE;
  options.format.channels = 2u;
  options.format.period_frames = 4u;
  options.format.latency_us = 25000u;
  options.capacity_frames = 8u;
  return options;
}

static int test_stream(void) {
  nxaudio_stream_options options = stream_options();
  nxaudio_stream *stream = NULL;
  nxaudio_pull_result pull;
  nxaudio_stream_stats stats;
  int16_t source[16];
  int16_t output[16];
  uint32_t written;
  size_t index;

  for (index = 0u; index < sizeof(source) / sizeof(source[0]); ++index)
    source[index] = (int16_t)(index + 1u);
  CHECK(nxaudio_stream_create(&options, &stream) == NXAUDIO_OK);
  CHECK(stream != NULL);
  CHECK(nxaudio_worker_submit(stream, source, 6u, &written) == NXAUDIO_OK);
  CHECK(written == 6u);
  CHECK(nxaudio_stream_start(stream) == NXAUDIO_OK);

  memset(output, 0, sizeof(output));
  CHECK(nxaudio_realtime_pull(stream, output, 4u, &pull) == NXAUDIO_OK);
  CHECK(pull.frames_copied == 4u && pull.silence_frames == 0u);
  CHECK(memcmp(output, source, 4u * 2u * sizeof(int16_t)) == 0);

  memset(output, 0x55, sizeof(output));
  CHECK(nxaudio_realtime_pull(stream, output, 4u, &pull) == NXAUDIO_OK);
  CHECK(pull.frames_copied == 2u && pull.silence_frames == 2u);
  CHECK(pull.reason == NXAUDIO_REASON_MIXER_STARVED);
  CHECK(output[4] == 0 && output[5] == 0 && output[6] == 0 && output[7] == 0);

  CHECK(nxaudio_stream_pause(stream) == NXAUDIO_OK);
  memset(output, 0x55, sizeof(output));
  CHECK(nxaudio_realtime_pull(stream, output, 4u, &pull) == NXAUDIO_OK);
  CHECK(pull.reason == NXAUDIO_REASON_PAUSED && pull.silence_frames == 4u);
  CHECK(output[0] == 0 && output[7] == 0);
  CHECK(nxaudio_stream_resume(stream) == NXAUDIO_OK);
  CHECK(nxaudio_worker_submit(stream, source, 4u, &written) == NXAUDIO_OK);
  CHECK(written == 4u);

  CHECK(nxaudio_stream_mark_device_lost(stream) == NXAUDIO_OK);
  CHECK(nxaudio_realtime_pull(stream, output, 4u, &pull) == NXAUDIO_OK);
  CHECK(pull.reason == NXAUDIO_REASON_DEVICE_LOST);
  CHECK(nxaudio_stream_recover(stream, 0) == NXAUDIO_OK);
  CHECK(nxaudio_stream_resume(stream) == NXAUDIO_OK);
  CHECK(nxaudio_realtime_pull(stream, output, 4u, &pull) == NXAUDIO_OK);
  CHECK(pull.frames_copied == 4u && pull.silence_frames == 0u);

  CHECK(nxaudio_stream_get_stats(stream, &stats) == NXAUDIO_OK);
  CHECK(stats.state == NXAUDIO_STREAM_RUNNING);
  CHECK(stats.format.frequency == 32000u && stats.format.channels == 2u &&
        stats.format.period_frames == 4u && stats.format.latency_us == 25000u);
  CHECK(stats.underrun_frames == 2u && stats.device_loss_count == 1u);
  nxaudio_stream_close(&stream);
  CHECK(stream == NULL);
  nxaudio_stream_close(&stream);

  options.format.frequency = 0u;
  stream = (nxaudio_stream *)(uintptr_t)1u;
  CHECK(nxaudio_stream_create(&options, &stream) == NXAUDIO_INVALID);
  CHECK(stream == NULL);
  return 0;
}

static nxaudio_backend_observation observation(const char *backend) {
  nxaudio_backend_observation value;
  memset(&value, 0, sizeof(value));
  value.api_version = NXAUDIO_API_VERSION;
  value.struct_size = sizeof(value);
  snprintf(value.backend, sizeof(value.backend), "%s", backend);
  value.server_reachable = 1;
  value.device_opened = 1;
  value.callback_count = 1u;
  value.produced_frames = 256u;
  return value;
}

static int test_backend_reasons(void) {
  nxaudio_backend_observation value = observation("pulse");
  nxaudio_reason reason = NXAUDIO_REASON_NONE;
  value.inherited_attempt = 1;
  CHECK(nxaudio_classify_backend(&value, &reason) == NXAUDIO_OK);
  CHECK(reason == NXAUDIO_REASON_INHERITED_BACKEND);
  value.inherited_attempt = 0;
  CHECK(nxaudio_classify_backend(&value, &reason) == NXAUDIO_OK);
  CHECK(reason == NXAUDIO_REASON_AUTODETECT_BACKEND);

  value = observation("DuMmY");
  CHECK(nxaudio_classify_backend(&value, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_FAKE_BACKEND);
  value = observation("DISK");
  CHECK(nxaudio_classify_backend(&value, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_FAKE_BACKEND);

  value = observation("pulse");
  value.server_reachable = 0;
  value.device_opened = 0;
  CHECK(nxaudio_classify_backend(&value, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_SERVER_UNAVAILABLE);
  value.server_reachable = 1;
  CHECK(nxaudio_classify_backend(&value, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_DEVICE_OPEN_FAILED);
  value.device_opened = 1;
  value.produced_frames = 0u;
  CHECK(nxaudio_classify_backend(&value, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_MIXER_STARVED);
  value = observation("");
  CHECK(nxaudio_classify_backend(&value, &reason) == NXAUDIO_INVALID);
  return 0;
}

static nxaudio_backend_retry_request retry_request(void) {
  nxaudio_backend_retry_request request;
  memset(&request, 0, sizeof(request));
  request.api_version = NXAUDIO_API_VERSION;
  request.struct_size = sizeof(request);
  request.current_backend = "pulse";
  request.fallback_backend = "alsa";
  request.failure_reason = NXAUDIO_REASON_DEVICE_OPEN_FAILED;
  request.fallback_available = 1;
  request.fallback_compiled = 1;
  return request;
}

static int test_backend_retry_policy(void) {
  nxaudio_backend_retry_request request = retry_request();
  nxaudio_backend_retry_plan plan;

  CHECK(nxaudio_plan_backend_retry(&request, &plan) == NXAUDIO_OK);
  CHECK(plan.retry_allowed == 1 && plan.next_retry_count == 1u);
  CHECK(strcmp(plan.backend, "alsa") == 0);
  CHECK(plan.reason == NXAUDIO_REASON_BACKEND_RETRY);
  CHECK(strcmp(nxaudio_reason_name(plan.reason), "backend-retry") == 0);

  request.retry_count = 1u;
  CHECK(nxaudio_plan_backend_retry(&request, &plan) == NXAUDIO_UNSUPPORTED);
  CHECK(plan.retry_allowed == 0 && plan.next_retry_count == 1u);
  CHECK(plan.reason == NXAUDIO_REASON_RETRY_EXHAUSTED);

  request = retry_request();
  request.backend_explicit = 1;
  CHECK(nxaudio_plan_backend_retry(&request, &plan) == NXAUDIO_UNSUPPORTED);
  CHECK(plan.reason == NXAUDIO_REASON_EXPLICIT_BACKEND_PRESERVED);
  request.backend_explicit = 0;
  request.environment_explicit = 1;
  CHECK(nxaudio_plan_backend_retry(&request, &plan) == NXAUDIO_UNSUPPORTED);
  CHECK(plan.reason == NXAUDIO_REASON_EXPLICIT_BACKEND_PRESERVED);

  request = retry_request();
  request.fallback_available = 0;
  CHECK(nxaudio_plan_backend_retry(&request, &plan) == NXAUDIO_UNSUPPORTED);
  CHECK(plan.reason == NXAUDIO_REASON_FALLBACK_UNAVAILABLE);
  request.fallback_available = 1;
  request.fallback_compiled = 0;
  CHECK(nxaudio_plan_backend_retry(&request, &plan) == NXAUDIO_UNSUPPORTED);
  CHECK(plan.reason == NXAUDIO_REASON_FALLBACK_UNAVAILABLE);

  request = retry_request();
  request.fallback_backend = "PuLsE";
  CHECK(nxaudio_plan_backend_retry(&request, &plan) == NXAUDIO_UNSUPPORTED);
  CHECK(plan.reason == NXAUDIO_REASON_FALLBACK_SAME_BACKEND);
  request.fallback_backend = "DuMmY";
  CHECK(nxaudio_plan_backend_retry(&request, &plan) == NXAUDIO_UNSUPPORTED);
  CHECK(plan.reason == NXAUDIO_REASON_FAKE_BACKEND);
  request.fallback_backend = "disk";
  CHECK(nxaudio_plan_backend_retry(&request, &plan) == NXAUDIO_UNSUPPORTED);
  CHECK(plan.reason == NXAUDIO_REASON_FAKE_BACKEND);

  request = retry_request();
  request.failure_reason = NXAUDIO_REASON_MIXER_STARVED;
  CHECK(nxaudio_plan_backend_retry(&request, &plan) == NXAUDIO_UNSUPPORTED);
  CHECK(plan.reason == NXAUDIO_REASON_BACKEND_FAILURE_UNPROVEN);
  request.failure_reason = (nxaudio_reason)99;
  CHECK(nxaudio_plan_backend_retry(&request, &plan) == NXAUDIO_INVALID);
  return 0;
}

static int test_asoundrc(void) {
  nxaudio_asoundrc_options options;
  nxaudio_environment_plan plan;
  memset(&options, 0, sizeof(options));
  options.api_version = NXAUDIO_API_VERSION;
  options.struct_size = sizeof(options);
  options.home = "/runtime/profile";
  options.isolated_home = 1;
  options.asoundrc_required = 1;
  options.asoundrc_present = 1;
  CHECK(nxaudio_plan_asoundrc(&options, &plan) == NXAUDIO_OK);
  CHECK(plan.expose == 1 && strcmp(plan.name, "ALSA_CONFIG_PATH") == 0);
  CHECK(strcmp(plan.value, "/runtime/profile/.asoundrc") == 0);
  CHECK(plan.reason == NXAUDIO_REASON_ASOUNDRC_EXPOSED);

  options.asoundrc_present = 0;
  CHECK(nxaudio_plan_asoundrc(&options, &plan) == NXAUDIO_UNSUPPORTED);
  CHECK(plan.reason == NXAUDIO_REASON_ASOUNDRC_REQUIRED);
  options.asoundrc_present = 1;
  options.home = "/runtime/../escape";
  CHECK(nxaudio_plan_asoundrc(&options, &plan) == NXAUDIO_UNSUPPORTED);
  options.isolated_home = 0;
  CHECK(nxaudio_plan_asoundrc(&options, &plan) == NXAUDIO_OK);
  CHECK(plan.expose == 0);
  return 0;
}

static nxaudio_adapter_request adapter(nxaudio_stack stack,
                                       const char *contract) {
  nxaudio_adapter_request request;
  memset(&request, 0, sizeof(request));
  request.api_version = NXAUDIO_API_VERSION;
  request.struct_size = sizeof(request);
  request.stack = stack;
  request.contract_id = contract;
  request.guest_uses_stack = 1;
  return request;
}

static int test_adapter_policy(void) {
  nxaudio_adapter_request request;
  const nxaudio_adapter_contract *titan_v1 = NULL;
  const nxaudio_adapter_contract *titan_v2 = NULL;
  nxaudio_reason reason;
  size_t index;
  CHECK(strcmp(NXAUDIO_VERSION, "0.4.0") == 0);
  CHECK(nxaudio_adapter_contract_count() == 9u);
  for (index = 0u; index < nxaudio_adapter_contract_count(); ++index) {
    const nxaudio_adapter_contract *contract =
        nxaudio_adapter_contract_at(index);
    CHECK(contract != NULL);
    if (strcmp(contract->contract_id,
               "titansouls-fmodex-opensl-sdl-v1") == 0)
      titan_v1 = contract;
    if (strcmp(contract->contract_id,
               "titansouls-fmodex-opensl-sdl-v2") == 0)
      titan_v2 = contract;
  }
  CHECK(nxaudio_adapter_contract_at(9u) == NULL);
  CHECK(titan_v1 != NULL && titan_v2 != NULL);
  CHECK(titan_v2->stack == NXAUDIO_STACK_FMOD_EX);
  CHECK(strcmp(titan_v2->port_id, "titansouls") == 0);
  CHECK(titan_v2->canonical_recipe_required == 0);
  CHECK(titan_v2->external_provider_forbidden == 1);

  request = adapter(NXAUDIO_STACK_OPENSL_ES, "tasm2-opensl-sdl-v1");
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_OK);
  request = adapter(NXAUDIO_STACK_AAUDIO, "generic-aaudio");
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_CONTRACT_UNPROVEN);
  request = adapter(NXAUDIO_STACK_OPENAL, "bully2-openal-v1");
  request.bundles_external_provider = 1;
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_PROVIDER_INCOMPATIBLE);
  request = adapter(NXAUDIO_STACK_FMOD, "horizon-fmod-sdl-v1");
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_OK);
  request = adapter(NXAUDIO_STACK_FMOD_EX, "castle-fmodex-v1");
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_OK);
  request = adapter(NXAUDIO_STACK_FMOD_EX,
                    "titansouls-fmodex-opensl-sdl-v1");
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_OK);
  CHECK(reason == NXAUDIO_REASON_NONE);
  request = adapter(NXAUDIO_STACK_FMOD_EX,
                    "titansouls-fmodex-opensl-sdl-v2");
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_OK);
  CHECK(reason == NXAUDIO_REASON_NONE);

  request.stack = NXAUDIO_STACK_FMOD;
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_CONTRACT_UNPROVEN);
  request.stack = NXAUDIO_STACK_FMOD_EX;
  request.bundles_external_provider = 1;
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_PROVIDER_INCOMPATIBLE);
  request.bundles_external_provider = 0;
  request.guest_uses_stack = 0;
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_CONTRACT_UNPROVEN);

  request = adapter(NXAUDIO_STACK_WWISE, "sor4-wwise-openal-glibc230-v1");
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_UNSUPPORTED);
  request.canonical_recipe = 1;
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_OK);
  return 0;
}

static int test_audibility(void) {
  nxaudio_audibility_evidence evidence;
  nxaudio_reason reason;
  memset(&evidence, 0, sizeof(evidence));
  evidence.api_version = NXAUDIO_API_VERSION;
  evidence.struct_size = sizeof(evidence);
  evidence.device_opened = 1;
  evidence.human_audible_confirmed = 1;
  evidence.produced_frames = 6222u;
  evidence.nonzero_samples = 1000u;
  evidence.peak = 8192u;
  evidence.scope = NXAUDIO_EVIDENCE_SYNTHETIC;
  CHECK(nxaudio_verify_audibility(&evidence, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_AUDIBILITY_UNPROVEN);
  evidence.scope = NXAUDIO_EVIDENCE_IMPORTED_APPROVED_PHYSICAL;
  CHECK(nxaudio_verify_audibility(&evidence, &reason) == NXAUDIO_OK);
  CHECK(reason == NXAUDIO_REASON_AUDIBLE_CONFIRMED);
  evidence.peak = 0u;
  CHECK(nxaudio_verify_audibility(&evidence, &reason) == NXAUDIO_UNSUPPORTED);
  return 0;
}

/* Onda v2: capacidade NAO potencia de dois com churn atravessando a costura
 * do anel muitas vezes -- a continuidade dos dados prova a aritmetica por
 * mascara (anel interno pow2). Antes, "% capacidade" saltava descontinuo no
 * wrap de 2^32 dos contadores livres. */
static int test_stream_non_pow2_capacity_churn(void) {
  nxaudio_stream_options options = stream_options();
  nxaudio_stream *stream = NULL;
  nxaudio_pull_result pull;
  int16_t in[2 * 6];
  int16_t out[2 * 6];
  uint32_t written = 0;
  uint32_t sequence = 0;
  uint32_t round;

  options.format.period_frames = 3u;
  options.capacity_frames = 6u; /* anel interno vira 8; contabilidade fica 6 */
  CHECK(nxaudio_stream_create(&options, &stream) == NXAUDIO_OK);
  CHECK(nxaudio_stream_start(stream) == NXAUDIO_OK);
  for (round = 0; round < 100u; ++round) {
    uint32_t frames = (round % 5u) + 1u; /* 1..5 desalinha da costura */
    uint32_t i;
    for (i = 0; i < frames * 2u; ++i)
      in[i] = (int16_t)(sequence + i);
    CHECK(nxaudio_worker_submit(stream, in, frames, &written) == NXAUDIO_OK);
    CHECK(written == frames);
    memset(out, 0, sizeof(out));
    CHECK(nxaudio_realtime_pull(stream, out, frames, &pull) == NXAUDIO_OK);
    CHECK(pull.frames_copied == frames);
    for (i = 0; i < frames * 2u; ++i)
      CHECK(out[i] == (int16_t)(sequence + i));
    sequence += frames * 2u;
  }
  nxaudio_stream_close(&stream);
  return 0;
}

/* Onda v2: pedido vs concedido tem NOME -- 44100/mono virando 48000/estereo
 * tocava em pitch errado sem nenhum erro. */
static int test_granted_format_classifier(void) {
  nxaudio_format requested;
  nxaudio_format granted;
  nxaudio_reason reason = NXAUDIO_REASON_NONE;

  memset(&requested, 0, sizeof(requested));
  requested.frequency = 44100u;
  requested.channels = 1u;
  requested.sample_format = NXAUDIO_SAMPLE_S16LE;
  granted = requested;
  CHECK(nxaudio_classify_granted_format(&requested, &granted, &reason) ==
        NXAUDIO_OK);
  CHECK(reason == NXAUDIO_REASON_FORMAT_MATCH);
  CHECK(strcmp(nxaudio_reason_name(reason), "format-match") == 0);
  granted.frequency = 48000u;
  granted.channels = 2u;
  CHECK(nxaudio_classify_granted_format(&requested, &granted, &reason) ==
        NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_RATE_CHANGED);
  CHECK(strcmp(nxaudio_reason_name(reason), "rate-changed") == 0);
  granted.frequency = requested.frequency;
  CHECK(nxaudio_classify_granted_format(&requested, &granted, &reason) ==
        NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_CHANNELS_CHANGED);
  granted.channels = requested.channels;
  granted.sample_format = NXAUDIO_SAMPLE_F32LE;
  CHECK(nxaudio_classify_granted_format(&requested, &granted, &reason) ==
        NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_SAMPLE_FORMAT_CHANGED);
  CHECK(nxaudio_classify_granted_format(NULL, &granted, &reason) ==
        NXAUDIO_INVALID);
  return 0;
}

int main(void) {
  if (test_stream() != 0 || test_backend_reasons() != 0 ||
      test_backend_retry_policy() != 0 || test_asoundrc() != 0 ||
      test_adapter_policy() != 0 || test_audibility() != 0 ||
      test_stream_non_pow2_capacity_churn() != 0 ||
      test_granted_format_classifier() != 0)
    return 1;
  puts("nxaudio M14 host contract tests passed");
  puts("guest_code_executed=0 hardware_ran=0 device_access=0 network_access=0");
  return 0;
}
