/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxcompat_sdl2.h"

#include <SDL.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(NXCOMPAT_SDL2_TESTING) &&                                      \
    (defined(__GNUC__) || defined(__clang__))
/* The dedicated test-only adapter replaces the audio device boundary.  The
 * normal nxcompat-sdl2 target never defines NXCOMPAT_SDL2_TESTING. */
extern const char *nxcompat_test_sdl2_audio_driver(void)
    __attribute__((weak));
extern int nxcompat_test_sdl2_audio_open(const SDL_AudioSpec *wanted,
                                         SDL_AudioSpec *obtained,
                                         SDL_AudioDeviceID *device)
    __attribute__((weak));
extern int nxcompat_test_sdl2_audio_close(SDL_AudioDeviceID device)
    __attribute__((weak));
#endif

typedef struct nxcompat_sdl2_context {
  nxcompat_backend_kind kind;
  Uint32 init_flags;
  Uint32 primary_flag;
  Uint32 initialized_on_entry;
  Uint32 owned_flags;
  uint32_t nxcompat_flags;
  int entry_captured;
  int audio_device_open;
  int audio_proof_complete;
  SDL_AudioSpec audio_obtained;
} nxcompat_sdl2_context;

static int nxcompat_sdl2_name_equal(const char *left, const char *right) {
  if (!left || !right)
    return 0;
  while (*left && *right) {
    if (tolower((unsigned char)*left) != tolower((unsigned char)*right))
      return 0;
    ++left;
    ++right;
  }
  return *left == '\0' && *right == '\0';
}

static int nxcompat_sdl2_name_accepted(nxcompat_sdl2_context *context,
                                       const char *name) {
  if (!name || !*name)
    return 0;
  if (context->kind == NXCOMPAT_BACKEND_AUDIO)
    return !nxcompat_sdl2_name_equal(name, "dummy") &&
           !nxcompat_sdl2_name_equal(name, "disk");
  if ((context->nxcompat_flags & NXCOMPAT_SDL2_ALLOW_NONDISPLAY) != 0)
    return 1;
  return !nxcompat_sdl2_name_equal(name, "dummy") &&
         !nxcompat_sdl2_name_equal(name, "offscreen");
}

static nxcompat_backend_attempt_outcome
nxcompat_sdl2_failure_outcome(const nxcompat_sdl2_context *context) {
  return (context->initialized_on_entry & context->primary_flag) != 0
             ? NXCOMPAT_BACKEND_ATTEMPT_OWNERSHIP_BUSY
             : NXCOMPAT_BACKEND_ATTEMPT_RETRYABLE_FAILURE;
}

static void nxcompat_sdl2_report_error(nxcompat_backend_attempt_report *report,
                                       nxcompat_reason_code reason,
                                       const char *fallback) {
  const char *error = SDL_GetError();
  report->reason = reason;
  (void)snprintf(report->error, sizeof(report->error), "%s",
                 error && *error ? error : fallback);
}

static int nxcompat_sdl2_audio_spec_valid(const SDL_AudioSpec *spec) {
  return spec && spec->freq >= 4000 && spec->freq <= 384000 &&
         spec->format != 0 && spec->channels > 0 && spec->channels <= 32 &&
         spec->samples > 0;
}

static const char *nxcompat_sdl2_current_audio_driver(void) {
#if defined(NXCOMPAT_SDL2_TESTING) &&                                      \
    (defined(__GNUC__) || defined(__clang__))
  if (nxcompat_test_sdl2_audio_driver) {
    const char *injected = nxcompat_test_sdl2_audio_driver();
    if (injected)
      return injected;
  }
#endif
  return SDL_GetCurrentAudioDriver();
}

static SDL_AudioDeviceID nxcompat_sdl2_open_audio_device(
    const SDL_AudioSpec *wanted, SDL_AudioSpec *obtained, int *injected) {
  *injected = 0;
#if defined(NXCOMPAT_SDL2_TESTING) &&                                      \
    (defined(__GNUC__) || defined(__clang__))
  /* A test build is a sealed fake boundary: a missing/declining hook must
   * fail closed and can never fall through to a physical audio device. */
  *injected = 1;
  if (nxcompat_test_sdl2_audio_open) {
    SDL_AudioDeviceID device = 0;
    if (nxcompat_test_sdl2_audio_open(wanted, obtained, &device))
      return device;
  }
  return 0;
#else
  return SDL_OpenAudioDevice(NULL, 0, wanted, obtained,
                             SDL_AUDIO_ALLOW_ANY_CHANGE);
#endif
}

static int nxcompat_sdl2_close_audio_device(SDL_AudioDeviceID device,
                                             int injected) {
#if defined(NXCOMPAT_SDL2_TESTING) &&                                      \
    (defined(__GNUC__) || defined(__clang__))
  if (!injected)
    return -1;
  return nxcompat_test_sdl2_audio_close &&
                 nxcompat_test_sdl2_audio_close(device)
             ? 0
             : -1;
#else
  (void)injected;
  SDL_CloseAudioDevice(device);
  return 0;
#endif
}

static nxcompat_backend_attempt_outcome nxcompat_sdl2_attempt_v2(
    void *userdata, nxcompat_backend_attempt_report *report) {
  nxcompat_sdl2_context *context = (nxcompat_sdl2_context *)userdata;
  const char *current;
  Uint32 initialized;
  Uint32 missing;
  Uint32 initialized_after;

  report->api_version = NXCOMPAT_API_VERSION_V2;
  report->struct_size = sizeof(*report);
  SDL_ClearError();
  if (!context->entry_captured) {
    context->initialized_on_entry = SDL_WasInit(context->init_flags);
    context->entry_captured = 1;
  }
  initialized = SDL_WasInit(context->init_flags);
  missing = context->init_flags & ~initialized;
  if (missing != 0 && SDL_InitSubSystem(missing) != 0) {
    initialized_after = SDL_WasInit(missing);
    context->owned_flags |= initialized_after & missing;
    nxcompat_sdl2_report_error(
        report, context->kind == NXCOMPAT_BACKEND_AUDIO
                    ? NXCOMPAT_REASON_AUDIO_OPEN_FAILED
                    : NXCOMPAT_REASON_VIDEO_OPEN_FAILED,
        "SDL subsystem initialization failed");
    return nxcompat_sdl2_failure_outcome(context);
  }
  if (missing != 0) {
    initialized_after = SDL_WasInit(missing);
    context->owned_flags |= initialized_after & missing;
  }

  current = context->kind == NXCOMPAT_BACKEND_AUDIO
                ? nxcompat_sdl2_current_audio_driver()
                : SDL_GetCurrentVideoDriver();
  if (!current || !*current) {
    nxcompat_sdl2_report_error(
        report, context->kind == NXCOMPAT_BACKEND_AUDIO
                    ? NXCOMPAT_REASON_AUDIO_OPEN_FAILED
                    : NXCOMPAT_REASON_VIDEO_OPEN_FAILED,
        "SDL initialized without a current backend");
    return nxcompat_sdl2_failure_outcome(context);
  }
  (void)snprintf(report->selected, sizeof(report->selected), "%s", current);
  if (!nxcompat_sdl2_name_accepted(context, current)) {
    report->reason = NXCOMPAT_REASON_BACKEND_FAKE_OUTPUT;
    (void)snprintf(report->error, sizeof(report->error),
                   "%s is not a real %s output", current,
                   context->kind == NXCOMPAT_BACKEND_AUDIO ? "audio" : "video");
    return nxcompat_sdl2_failure_outcome(context);
  }

  if (context->kind == NXCOMPAT_BACKEND_AUDIO) {
    SDL_AudioSpec wanted;
    SDL_AudioSpec obtained;
    SDL_AudioDeviceID device;
    int injected;
    memset(&wanted, 0, sizeof(wanted));
    memset(&obtained, 0, sizeof(obtained));
    wanted.freq = 48000;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    wanted.samples = 1024;
    device = nxcompat_sdl2_open_audio_device(&wanted, &obtained, &injected);
    if (device == 0) {
      nxcompat_sdl2_report_error(report, NXCOMPAT_REASON_AUDIO_OPEN_FAILED,
                                 "SDL default audio device did not open");
      return nxcompat_sdl2_failure_outcome(context);
    }
    context->audio_device_open = 1;
    if (nxcompat_sdl2_close_audio_device(device, injected) != 0) {
      report->reason = NXCOMPAT_REASON_PROVIDER_CONTRACT;
      (void)snprintf(report->error, sizeof(report->error),
                     "synthetic SDL audio device did not close");
      return NXCOMPAT_BACKEND_ATTEMPT_FATAL_FAILURE;
    }
    context->audio_device_open = 0;
    if (!nxcompat_sdl2_audio_spec_valid(&obtained)) {
      report->reason = NXCOMPAT_REASON_AUDIO_DEVICE_INVALID;
      (void)snprintf(report->error, sizeof(report->error),
                     "SDL returned an invalid audio device format");
      return nxcompat_sdl2_failure_outcome(context);
    }
    context->audio_obtained = obtained;
    context->audio_proof_complete = 1;
  }
  return NXCOMPAT_BACKEND_ATTEMPT_OK;
}

static nxcompat_result_code nxcompat_sdl2_cleanup_v2(void *userdata,
                                                      char *error,
                                                      size_t error_size) {
  nxcompat_sdl2_context *context = (nxcompat_sdl2_context *)userdata;
  Uint32 owned = context->owned_flags;
  Uint32 after;
  context->owned_flags = 0;
  if (owned != 0)
    SDL_QuitSubSystem(owned);
  after = SDL_WasInit(context->init_flags) & context->init_flags;
  if (context->audio_device_open ||
      after != (context->initialized_on_entry & context->init_flags)) {
    if (error_size)
      (void)snprintf(error, error_size,
                     "SDL subsystem ownership rollback did not restore entry state");
    return NXCOMPAT_ROLLBACK_FAILED;
  }
  if (error_size)
    error[0] = '\0';
  SDL_ClearError();
  return NXCOMPAT_OK;
}

static int nxcompat_sdl2_accept_v2(void *userdata, const char *name) {
  return nxcompat_sdl2_name_accepted((nxcompat_sdl2_context *)userdata, name);
}

static void nxcompat_sdl2_initialize_audio_receipt(
    nxcompat_audio_receipt *receipt) {
  if (!receipt)
    return;
  memset(receipt, 0, sizeof(*receipt));
  receipt->api_version = NXCOMPAT_API_VERSION_V2;
  receipt->struct_size = sizeof(*receipt);
  receipt->source = NXCOMPAT_SOURCE_SDL2_AUDIO;
  receipt->lifetime = NXCOMPAT_AUDIO_OPENED_THEN_CLOSED;
}

static nxcompat_result_code nxcompat_sdl2_negotiate_internal(
    const nxcompat_sdl2_options *options, uint64_t generation,
    nxcompat_backend_result_v2 *result, nxcompat_audio_receipt *receipt) {
  nxcompat_backend_options_v2 backend_options;
  nxcompat_sdl2_context context;
  nxcompat_result_code status;
  const uint32_t known_flags = NXCOMPAT_SDL2_ALLOW_NONDISPLAY;
  nxcompat_sdl2_initialize_audio_receipt(receipt);
  if (result) {
    memset(result, 0, sizeof(*result));
    result->api_version = NXCOMPAT_API_VERSION_V2;
    result->struct_size = sizeof(*result);
    result->env_restored = 1;
    result->final_reason = NXCOMPAT_REASON_INVALID_ARGUMENT;
    if (options && (options->kind == NXCOMPAT_BACKEND_VIDEO ||
                    options->kind == NXCOMPAT_BACKEND_AUDIO))
      result->kind = options->kind;
  }
  if (!options || options->api_version != NXCOMPAT_API_VERSION_V2 ||
      options->struct_size < sizeof(*options) || !result ||
      (options->kind != NXCOMPAT_BACKEND_VIDEO &&
       options->kind != NXCOMPAT_BACKEND_AUDIO) ||
      (options->additional_sdl_init_flags &
       (uint32_t)(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) != 0 ||
      (options->nxcompat_flags & ~known_flags) != 0 ||
      (receipt &&
       (options->kind != NXCOMPAT_BACKEND_AUDIO || generation == 0u)))
    return NXCOMPAT_INVALID;

  memset(&context, 0, sizeof(context));
  context.kind = options->kind;
  context.nxcompat_flags = options->nxcompat_flags;
  context.primary_flag = options->kind == NXCOMPAT_BACKEND_AUDIO
                             ? SDL_INIT_AUDIO
                             : SDL_INIT_VIDEO;
  context.init_flags =
      (Uint32)options->additional_sdl_init_flags | context.primary_flag;

  memset(&backend_options, 0, sizeof(backend_options));
  backend_options.api_version = NXCOMPAT_API_VERSION_V2;
  backend_options.struct_size = sizeof(backend_options);
  backend_options.kind = options->kind;
  backend_options.attempt = nxcompat_sdl2_attempt_v2;
  backend_options.cleanup = nxcompat_sdl2_cleanup_v2;
  backend_options.accept_name = nxcompat_sdl2_accept_v2;
  backend_options.status = options->status;
  backend_options.userdata = &context;
  backend_options.status_userdata = options->status_userdata;
  if (options->kind == NXCOMPAT_BACKEND_AUDIO) {
#if defined(NXCOMPAT_SDL2_TESTING)
    /* Test retries may clear only synthetic hints.  SDL_AUDIODRIVER remains
     * pinned to dummy, so SDL_InitSubSystem can never discover Pulse/ALSA. */
    backend_options.environment_names[0] =
        "NXCOMPAT_TEST_SDL_AUDIODRIVER";
    backend_options.environment_names[1] =
        "NXCOMPAT_TEST_SDL_AUDIO_DRIVER";
#else
    backend_options.environment_names[0] = "SDL_AUDIODRIVER";
    backend_options.environment_names[1] = "SDL_AUDIO_DRIVER";
#endif
  } else {
#if defined(NXCOMPAT_SDL2_TESTING)
    backend_options.environment_names[0] =
        "NXCOMPAT_TEST_SDL_VIDEODRIVER";
    backend_options.environment_names[1] =
        "NXCOMPAT_TEST_SDL_VIDEO_DRIVER";
#else
    backend_options.environment_names[0] = "SDL_VIDEODRIVER";
    backend_options.environment_names[1] = "SDL_VIDEO_DRIVER";
#endif
  }
  status = nxcompat_negotiate_backend_v2(&backend_options, result);
  if (receipt && status == NXCOMPAT_OK) {
    if (!context.audio_proof_complete || !result->selected[0]) {
      result->state = NXCOMPAT_BACKEND_V2_FAILED;
      result->final_reason = NXCOMPAT_REASON_PROVIDER_CONTRACT;
      (void)snprintf(result->final_error, sizeof(result->final_error),
                     "SDL audio succeeded without a complete device proof");
      return NXCOMPAT_FAILED;
    }
    receipt->proof_flags = NXCOMPAT_AUDIO_PROOF_BACKEND_INITIALIZED |
                           NXCOMPAT_AUDIO_PROOF_DEVICE_OPENED |
                           NXCOMPAT_AUDIO_PROOF_SPEC_OBTAINED;
    receipt->generation = generation;
    receipt->frequency = context.audio_obtained.freq;
    receipt->format = (uint32_t)context.audio_obtained.format;
    receipt->channels = context.audio_obtained.channels;
    receipt->samples = context.audio_obtained.samples;
    receipt->device_id_was_nonzero = 1;
    (void)snprintf(receipt->backend, sizeof(receipt->backend), "%s",
                   result->selected);
  }
  return status;
}

nxcompat_result_code
nxcompat_sdl2_negotiate_v2(const nxcompat_sdl2_options *options,
                           nxcompat_backend_result_v2 *result) {
  return nxcompat_sdl2_negotiate_internal(options, 0u, result, NULL);
}

nxcompat_result_code nxcompat_sdl2_negotiate_audio_v2(
    const nxcompat_sdl2_options *options, uint64_t generation,
    nxcompat_backend_result_v2 *result, nxcompat_audio_receipt *receipt) {
  if (!receipt) {
    if (result) {
      memset(result, 0, sizeof(*result));
      result->api_version = NXCOMPAT_API_VERSION_V2;
      result->struct_size = sizeof(*result);
      result->env_restored = 1;
      result->final_reason = NXCOMPAT_REASON_INVALID_ARGUMENT;
    }
    return NXCOMPAT_INVALID;
  }
  return nxcompat_sdl2_negotiate_internal(options, generation, result, receipt);
}

int nxcompat_sdl2_negotiate(const nxcompat_sdl2_options *options,
                            nxcompat_backend_result *result) {
  nxcompat_sdl2_options options_v2;
  nxcompat_backend_result_v2 result_v2;
  nxcompat_result_code status;
  if (!options || !result ||
      !((options->api_version == NXCOMPAT_API_VERSION_V1) ||
        (options->api_version == NXCOMPAT_API_VERSION_V2)) ||
      options->struct_size < sizeof(*options))
    return -1;
  options_v2 = *options;
  options_v2.api_version = NXCOMPAT_API_VERSION_V2;
  status = nxcompat_sdl2_negotiate_v2(&options_v2, &result_v2);
  memset(result, 0, sizeof(*result));
  result->api_version = options->api_version;
  result->struct_size = sizeof(*result);
  result->kind = options->kind;
  result->attempt_count = result_v2.attempt_count;
  result->inherited_count = result_v2.inherited_count;
  (void)snprintf(result->selected, sizeof(result->selected), "%s",
                 result_v2.selected);
  (void)snprintf(result->first_error, sizeof(result->first_error), "%s",
                 result_v2.first_error);
  (void)snprintf(result->final_error, sizeof(result->final_error), "%s",
                 result_v2.final_error);
  switch (result_v2.state) {
  case NXCOMPAT_BACKEND_V2_INHERITED_OK:
    result->state = NXCOMPAT_BACKEND_INHERITED_OK;
    break;
  case NXCOMPAT_BACKEND_V2_AUTODETECT_OK:
    result->state = NXCOMPAT_BACKEND_AUTODETECT_OK;
    break;
  case NXCOMPAT_BACKEND_V2_RECOVERED_BY_AUTODETECT:
    result->state = NXCOMPAT_BACKEND_RECOVERED_BY_AUTODETECT;
    break;
  default:
    result->state = NXCOMPAT_BACKEND_FAILED;
    break;
  }
  return status == NXCOMPAT_OK ? 0 : -1;
}
