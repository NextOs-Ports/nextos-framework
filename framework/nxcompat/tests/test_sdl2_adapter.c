/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "nxcompat_sdl2.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__,          \
              #condition, SDL_GetError());                                     \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static int environment_equals(const char *name, const char *expected) {
  const char *value = getenv(name);
  return value && strcmp(value, expected) == 0;
}

static void options_init(nxcompat_sdl2_options *options,
                         nxcompat_backend_kind kind) {
  memset(options, 0, sizeof(*options));
  options->api_version = NXCOMPAT_API_VERSION_V2;
  options->struct_size = sizeof(*options);
  options->kind = kind;
}

#if defined(NXCOMPAT_SDL2_TESTING)
#define TEST_AUDIO_HINT "NXCOMPAT_TEST_SDL_AUDIODRIVER"
#define TEST_VIDEO_HINT "NXCOMPAT_TEST_SDL_VIDEODRIVER"

typedef struct fake_audio_device {
  int override_driver;
  int handle_open;
  int close_succeeds;
  SDL_AudioDeviceID device;
  SDL_AudioDeviceID closed_device;
  SDL_AudioSpec wanted;
  SDL_AudioSpec obtained;
  unsigned open_calls;
  unsigned close_calls;
} fake_audio_device;

static fake_audio_device fake_audio;

const char *nxcompat_test_sdl2_audio_driver(void) {
  return fake_audio.override_driver ? "synthetic-real-audio" : NULL;
}

int nxcompat_test_sdl2_audio_open(const SDL_AudioSpec *wanted,
                                  SDL_AudioSpec *obtained,
                                  SDL_AudioDeviceID *device) {
  ++fake_audio.open_calls;
  if (!fake_audio.handle_open)
    return 0;
  fake_audio.wanted = *wanted;
  *obtained = fake_audio.obtained;
  *device = fake_audio.device;
  return 1;
}

int nxcompat_test_sdl2_audio_close(SDL_AudioDeviceID device) {
  ++fake_audio.close_calls;
  if (!fake_audio.handle_open)
    return 0;
  fake_audio.closed_device = device;
  return fake_audio.close_succeeds;
}

static void fake_audio_reset(SDL_AudioDeviceID device, int valid_spec) {
  memset(&fake_audio, 0, sizeof(fake_audio));
  fake_audio.override_driver = 1;
  fake_audio.handle_open = 1;
  fake_audio.close_succeeds = 1;
  fake_audio.device = device;
  fake_audio.obtained.freq = valid_spec ? 48000 : 0;
  fake_audio.obtained.format = AUDIO_S16SYS;
  fake_audio.obtained.channels = 2;
  fake_audio.obtained.samples = 1024;
}

static int test_audio_fake_device_lifecycle(void) {
  nxcompat_sdl2_options options;
  nxcompat_backend_result_v2 result;
  nxcompat_audio_receipt receipt;
  nxcompat_capability_evidence evidence;
  nxcompat_registry *registry = NULL;

  options_init(&options, NXCOMPAT_BACKEND_AUDIO);
  CHECK(nxcompat_sdl2_negotiate_audio_v2(&options, 1u, &result, NULL) ==
        NXCOMPAT_INVALID);
  CHECK(result.attempt_count == 0 && result.cleanup_count == 0);
  CHECK(fake_audio.open_calls == 0 && fake_audio.close_calls == 0);
  CHECK(nxcompat_sdl2_negotiate_audio_v2(&options, 0u, &result, &receipt) ==
        NXCOMPAT_INVALID);
  CHECK(receipt.generation == 0u && receipt.proof_flags == 0u);

  /* The test-only boundary must never fall through to SDL_OpenAudioDevice,
   * even when the injected hook explicitly declines both bounded attempts. */
  SDL_Quit();
  CHECK(setenv("SDL_AUDIODRIVER", "dummy", 1) == 0);
  CHECK(setenv(TEST_AUDIO_HINT, "retry-fixture", 1) == 0);
  memset(&fake_audio, 0, sizeof(fake_audio));
  fake_audio.override_driver = 1;
  CHECK(nxcompat_sdl2_negotiate_audio_v2(&options, 40u, &result, &receipt) ==
        NXCOMPAT_FAILED);
  CHECK(result.attempt_count == 2 && result.cleanup_count == 2);
  CHECK(fake_audio.open_calls == 2 && fake_audio.close_calls == 0);
  CHECK(receipt.generation == 0u && receipt.proof_flags == 0u);
  CHECK(environment_equals("SDL_AUDIODRIVER", "dummy"));
  CHECK(environment_equals(TEST_AUDIO_HINT, "retry-fixture"));

  SDL_Quit();
  CHECK(setenv("SDL_AUDIODRIVER", "dummy", 1) == 0);
  CHECK(setenv(TEST_AUDIO_HINT, "inherited-fixture", 1) == 0);
  fake_audio_reset((SDL_AudioDeviceID)73, 1);
  CHECK(nxcompat_sdl2_negotiate_audio_v2(&options, 41u, &result, &receipt) ==
        NXCOMPAT_OK);
  CHECK(result.state == NXCOMPAT_BACKEND_V2_INHERITED_OK);
  CHECK(result.attempt_count == 1 && result.cleanup_count == 1);
  CHECK(strcmp(result.selected, "synthetic-real-audio") == 0);
  CHECK(fake_audio.open_calls == 1 && fake_audio.close_calls == 1);
  CHECK(fake_audio.closed_device == (SDL_AudioDeviceID)73);
  CHECK(fake_audio.wanted.freq == 48000 &&
        fake_audio.wanted.format == AUDIO_S16SYS &&
        fake_audio.wanted.channels == 2 && fake_audio.wanted.samples == 1024);
  CHECK(receipt.api_version == NXCOMPAT_API_VERSION_V2);
  CHECK(receipt.struct_size == sizeof(receipt));
  CHECK(receipt.generation == 41u);
  CHECK(receipt.source == NXCOMPAT_SOURCE_SDL2_AUDIO);
  CHECK(receipt.lifetime == NXCOMPAT_AUDIO_OPENED_THEN_CLOSED);
  CHECK(receipt.proof_flags ==
        (NXCOMPAT_AUDIO_PROOF_BACKEND_INITIALIZED |
         NXCOMPAT_AUDIO_PROOF_DEVICE_OPENED |
         NXCOMPAT_AUDIO_PROOF_SPEC_OBTAINED));
  CHECK(receipt.device_id_was_nonzero);
  CHECK(receipt.frequency == 48000 && receipt.format == AUDIO_S16SYS &&
        receipt.channels == 2 && receipt.samples == 1024);
  CHECK(strcmp(receipt.backend, "synthetic-real-audio") == 0);
  CHECK(nxcompat_registry_create(&registry) == NXCOMPAT_OK);
  CHECK(nxcompat_registry_publish_audio(registry, &receipt) == NXCOMPAT_OK);
  CHECK(nxcompat_registry_get(registry, NXCOMPAT_CAPABILITY_AUDIO_OUTPUT_OPEN,
                              &evidence) == NXCOMPAT_OK);
  CHECK(evidence.state == NXCOMPAT_EVIDENCE_OPENED);
  CHECK(evidence.source == NXCOMPAT_SOURCE_SDL2_AUDIO);
  CHECK(evidence.generation == 41u);
  nxcompat_registry_destroy(registry);
  registry = NULL;
  CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0);

  SDL_Quit();
  CHECK(SDL_InitSubSystem(SDL_INIT_AUDIO) == 0);
  fake_audio_reset((SDL_AudioDeviceID)0, 1);
  CHECK(nxcompat_sdl2_negotiate_audio_v2(&options, 42u, &result, &receipt) ==
        NXCOMPAT_BUSY);
  CHECK(result.first_reason == NXCOMPAT_REASON_AUDIO_OPEN_FAILED);
  CHECK(fake_audio.open_calls == 1 && fake_audio.close_calls == 0);
  CHECK(receipt.generation == 0u && receipt.proof_flags == 0u);
  CHECK(SDL_WasInit(SDL_INIT_AUDIO) != 0);
  SDL_QuitSubSystem(SDL_INIT_AUDIO);

  CHECK(SDL_InitSubSystem(SDL_INIT_AUDIO) == 0);
  fake_audio_reset((SDL_AudioDeviceID)74, 0);
  CHECK(nxcompat_sdl2_negotiate_audio_v2(&options, 43u, &result, &receipt) ==
        NXCOMPAT_BUSY);
  CHECK(result.first_reason == NXCOMPAT_REASON_AUDIO_DEVICE_INVALID);
  CHECK(fake_audio.open_calls == 1 && fake_audio.close_calls == 1);
  CHECK(fake_audio.closed_device == (SDL_AudioDeviceID)74);
  CHECK(receipt.generation == 0u && receipt.proof_flags == 0u);
  CHECK(SDL_WasInit(SDL_INIT_AUDIO) != 0);
  SDL_QuitSubSystem(SDL_INIT_AUDIO);

  CHECK(SDL_InitSubSystem(SDL_INIT_AUDIO) == 0);
  fake_audio_reset((SDL_AudioDeviceID)75, 1);
  fake_audio.close_succeeds = 0;
  CHECK(nxcompat_sdl2_negotiate_audio_v2(&options, 44u, &result, &receipt) ==
        NXCOMPAT_ROLLBACK_FAILED);
  CHECK(result.state == NXCOMPAT_BACKEND_V2_ROLLBACK_FAILED);
  CHECK(result.final_reason == NXCOMPAT_REASON_BACKEND_CLEANUP_FAILED);
  CHECK(fake_audio.open_calls == 1 && fake_audio.close_calls == 1);
  CHECK(receipt.generation == 0u && receipt.proof_flags == 0u);
  CHECK(SDL_WasInit(SDL_INIT_AUDIO) != 0);
  SDL_QuitSubSystem(SDL_INIT_AUDIO);

  memset(&fake_audio, 0, sizeof(fake_audio));
  CHECK(unsetenv("SDL_AUDIODRIVER") == 0);
  CHECK(unsetenv(TEST_AUDIO_HINT) == 0);
  return 0;
}
#endif

static int test_video_ownership(void) {
  nxcompat_sdl2_options options;
  nxcompat_backend_result_v2 result;

  SDL_Quit();
  CHECK(setenv("SDL_VIDEODRIVER", "dummy", 1) == 0);
#if defined(NXCOMPAT_SDL2_TESTING)
  CHECK(setenv(TEST_VIDEO_HINT, "dummy", 1) == 0);
#endif
  options_init(&options, NXCOMPAT_BACKEND_VIDEO);
  options.nxcompat_flags = NXCOMPAT_SDL2_ALLOW_NONDISPLAY;
  CHECK(nxcompat_sdl2_negotiate_v2(&options, &result) == NXCOMPAT_OK);
  CHECK(result.state == NXCOMPAT_BACKEND_V2_INHERITED_OK);
  CHECK(result.attempt_count == 1 && result.cleanup_count == 1);
  CHECK(strcmp(result.selected, "dummy") == 0);
  CHECK(SDL_WasInit(SDL_INIT_VIDEO) == 0);
  CHECK(environment_equals("SDL_VIDEODRIVER", "dummy"));

  options_init(&options, NXCOMPAT_BACKEND_VIDEO);
  options.nxcompat_flags = NXCOMPAT_SDL2_ALLOW_NONDISPLAY;
  options.additional_sdl_init_flags = SDL_INIT_TIMER;
  CHECK(nxcompat_sdl2_negotiate_v2(&options, &result) == NXCOMPAT_OK);
  CHECK(SDL_WasInit(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0);

  options_init(&options, NXCOMPAT_BACKEND_VIDEO);
  options.additional_sdl_init_flags = SDL_INIT_AUDIO;
  CHECK(nxcompat_sdl2_negotiate_v2(&options, &result) == NXCOMPAT_INVALID);
  CHECK(SDL_WasInit(SDL_INIT_VIDEO | SDL_INIT_AUDIO) == 0);

  CHECK(SDL_InitSubSystem(SDL_INIT_VIDEO) == 0);
  options_init(&options, NXCOMPAT_BACKEND_VIDEO);
  options.nxcompat_flags = NXCOMPAT_SDL2_ALLOW_NONDISPLAY;
  CHECK(nxcompat_sdl2_negotiate_v2(&options, &result) == NXCOMPAT_OK);
  CHECK(SDL_WasInit(SDL_INIT_VIDEO) != 0);

  options_init(&options, NXCOMPAT_BACKEND_VIDEO);
  CHECK(nxcompat_sdl2_negotiate_v2(&options, &result) == NXCOMPAT_BUSY);
  CHECK(result.state == NXCOMPAT_BACKEND_V2_BUSY);
  CHECK(result.attempt_count == 1 && result.cleanup_count == 1);
  CHECK(result.first_reason == NXCOMPAT_REASON_BACKEND_FAKE_OUTPUT);
  CHECK(SDL_WasInit(SDL_INIT_VIDEO) != 0);
  CHECK(environment_equals("SDL_VIDEODRIVER", "dummy"));
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
  CHECK(SDL_WasInit(SDL_INIT_VIDEO) == 0);
  CHECK(unsetenv("SDL_VIDEODRIVER") == 0);
#if defined(NXCOMPAT_SDL2_TESTING)
  CHECK(unsetenv(TEST_VIDEO_HINT) == 0);
#endif
  return 0;
}

static int test_audio_rejects_fake_outputs(void) {
  static const char *const fake_drivers[] = {"dummy", "disk"};
  nxcompat_sdl2_options options;
  nxcompat_backend_result_v2 result;
  nxcompat_audio_receipt receipt;
  size_t index;

  for (index = 0; index < sizeof(fake_drivers) / sizeof(fake_drivers[0]);
       ++index) {
    SDL_Quit();
    CHECK(setenv("SDL_AUDIODRIVER", fake_drivers[index], 1) == 0);
    CHECK(setenv("SDL_VIDEODRIVER", "video-sentinel", 1) == 0);
    CHECK(SDL_InitSubSystem(SDL_INIT_AUDIO) == 0);
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) != 0);
    options_init(&options, NXCOMPAT_BACKEND_AUDIO);
#if defined(NXCOMPAT_SDL2_TESTING)
    CHECK(unsetenv(TEST_AUDIO_HINT) == 0);
    fake_audio_reset((SDL_AudioDeviceID)79, 1);
    fake_audio.override_driver = 0;
#endif
    CHECK(nxcompat_sdl2_negotiate_audio_v2(
              &options, (uint64_t)index + 1u, &result, &receipt) ==
          NXCOMPAT_BUSY);
    CHECK(result.state == NXCOMPAT_BACKEND_V2_BUSY);
    CHECK(result.attempt_count == 1 && result.cleanup_count == 1);
    CHECK(result.first_reason == NXCOMPAT_REASON_BACKEND_FAKE_OUTPUT);
    CHECK(receipt.generation == 0u && receipt.proof_flags == 0u);
#if defined(NXCOMPAT_SDL2_TESTING)
    CHECK(fake_audio.open_calls == 0 && fake_audio.close_calls == 0);
#endif
    CHECK(environment_equals("SDL_AUDIODRIVER", fake_drivers[index]));
    CHECK(environment_equals("SDL_VIDEODRIVER", "video-sentinel"));
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) != 0);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    CHECK(SDL_WasInit(SDL_INIT_AUDIO) == 0);
    CHECK(unsetenv("SDL_AUDIODRIVER") == 0);
    CHECK(unsetenv("SDL_VIDEODRIVER") == 0);
  }
#if defined(NXCOMPAT_SDL2_TESTING)
  memset(&fake_audio, 0, sizeof(fake_audio));
#endif
  return 0;
}

static int test_legacy_wrapper(void) {
  nxcompat_sdl2_options options;
  nxcompat_backend_result result;
  SDL_Quit();
  CHECK(setenv("SDL_VIDEODRIVER", "dummy", 1) == 0);
#if defined(NXCOMPAT_SDL2_TESTING)
  CHECK(setenv(TEST_VIDEO_HINT, "dummy", 1) == 0);
#endif
  options_init(&options, NXCOMPAT_BACKEND_VIDEO);
  options.api_version = NXCOMPAT_API_VERSION_V1;
  options.nxcompat_flags = NXCOMPAT_SDL2_ALLOW_NONDISPLAY;
  CHECK(nxcompat_sdl2_negotiate(&options, &result) == 0);
  CHECK(result.state == NXCOMPAT_BACKEND_INHERITED_OK);
  CHECK(result.attempt_count == 1);
  CHECK(SDL_WasInit(SDL_INIT_VIDEO) == 0);
  CHECK(unsetenv("SDL_VIDEODRIVER") == 0);
#if defined(NXCOMPAT_SDL2_TESTING)
  CHECK(unsetenv(TEST_VIDEO_HINT) == 0);
#endif
  return 0;
}

int main(void) {
  if (test_video_ownership() != 0 || test_audio_rejects_fake_outputs() != 0 ||
      test_legacy_wrapper() != 0)
    return 1;
#if defined(NXCOMPAT_SDL2_TESTING)
  if (test_audio_fake_device_lifecycle() != 0)
    return 1;
#endif
  SDL_Quit();
  puts("nxcompat SDL2 bounded-attempt and ownership tests passed");
  return 0;
}
