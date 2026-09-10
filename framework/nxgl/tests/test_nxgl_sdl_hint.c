/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "nxgl.h"
#include "nxgl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static const char *fake_drivers[NXGL_SDL_VIDEO_DRIVER_MAX];
static int fake_driver_count;
static Uint32 fake_video_initialized;
static const char *fake_selected_driver;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                             \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

int nxgl_test_sdl_get_num_video_drivers(void) { return fake_driver_count; }

const char *nxgl_test_sdl_get_video_driver(int index) {
  if (index < 0 || index >= fake_driver_count)
    return NULL;
  return fake_drivers[index];
}

Uint32 nxgl_test_sdl_was_init(Uint32 flags) {
  return (flags & SDL_INIT_VIDEO) != 0u ? fake_video_initialized : 0u;
}

const char *nxgl_test_sdl_current_video_driver(void) {
  return fake_selected_driver;
}

static char *duplicate_text(const char *value) {
  char *copy;
  size_t size;
  if (!value)
    return NULL;
  size = strlen(value) + 1u;
  copy = (char *)malloc(size);
  if (copy)
    memcpy(copy, value, size);
  return copy;
}

static int environment_equals(const char *name, const char *expected) {
  const char *actual = getenv(name);
  return actual && strcmp(actual, expected) == 0;
}

static void set_fake_drivers(const char *first, const char *second) {
  memset(fake_drivers, 0, sizeof(fake_drivers));
  fake_driver_count = 0;
  if (first)
    fake_drivers[fake_driver_count++] = first;
  if (second)
    fake_drivers[fake_driver_count++] = second;
  fake_video_initialized = 0u;
  fake_selected_driver = NULL;
}

static nxgl_sdl_video_hint_receipt_v2 sanitize(const char *hint,
                                               int expected_result) {
  nxgl_sdl_video_hint_options_v2 options;
  nxgl_sdl_video_hint_receipt_v2 receipt;
  nxgl_sdl_video_hint_options_v2_init(&options);
  options.enabled = 1;
  if (hint)
    CHECK(setenv("SDL_VIDEODRIVER", hint, 1) == 0);
  else
    CHECK(unsetenv("SDL_VIDEODRIVER") == 0);
  memset(&receipt, 0xA5, sizeof(receipt));
  CHECK(nxgl_sanitize_sdl_video_hint_v2(&options, &receipt) ==
        expected_result);
  return receipt;
}

static void test_disabled_is_inert(void) {
  nxgl_sdl_video_hint_options_v2 options;
  nxgl_sdl_video_hint_receipt_v2 receipt;
  nxgl_sdl_video_hint_options_v2_init(&options);
  nxgl_sdl_video_hint_receipt_v2_init(&receipt);
  CHECK(setenv("SDL_VIDEODRIVER", "frontend64", 1) == 0);
  fake_driver_count = -1;
  CHECK(nxgl_sanitize_sdl_video_hint_v2(&options, &receipt) ==
        NXGL_NO_ACTION);
  CHECK(receipt.action == NXGL_SDL_VIDEO_HINT_V2_DISABLED);
  CHECK(environment_equals("SDL_VIDEODRIVER", "frontend64"));
}

static void test_no_hint_keeps_autodetection(void) {
  nxgl_sdl_video_hint_receipt_v2 receipt;
  nxgl_sdl_video_hint_receipt_v2 before;
  set_fake_drivers("KMSDRM", "dummy");
  receipt = sanitize(NULL, NXGL_SUCCESS);
  CHECK(receipt.action == NXGL_SDL_VIDEO_HINT_V2_NO_HINT);
  CHECK(receipt.inherited_hint_present == 0);
  CHECK(receipt.hint_removed == 0);
  CHECK(receipt.video_driver_count == 2);
  CHECK(strcmp(receipt.compiled_video_drivers, "KMSDRM,dummy") == 0);
  CHECK(getenv("SDL_VIDEODRIVER") == NULL);
  before = receipt;
  CHECK(nxgl_complete_sdl_video_hint_receipt_v2(&receipt) ==
        NXGL_ERROR_INVALID_STATE);
  CHECK(memcmp(&receipt, &before, sizeof(receipt)) == 0);
}

static void test_supported_hint_is_preserved_and_completed(void) {
  nxgl_sdl_video_hint_receipt_v2 receipt;
  char text[NXGL_SDL_VIDEO_RECEIPT_TEXT_MAX];
  set_fake_drivers("KMSDRM", "dummy");
  receipt = sanitize(" wayland , kmsdrm ", NXGL_SUCCESS);
  CHECK(receipt.action == NXGL_SDL_VIDEO_HINT_V2_PRESERVED_SUPPORTED);
  CHECK(receipt.inherited_hint_present == 1);
  CHECK(receipt.inherited_hint_supported == 1);
  CHECK(receipt.hint_removed == 0);
  CHECK(environment_equals("SDL_VIDEODRIVER", " wayland , kmsdrm "));
  fake_video_initialized = SDL_INIT_VIDEO;
  fake_selected_driver = "KMSDRM";
  CHECK(nxgl_complete_sdl_video_hint_receipt_v2(&receipt) == NXGL_SUCCESS);
  CHECK(receipt.selected_recorded == 1);
  CHECK(strcmp(receipt.selected_video_driver, "KMSDRM") == 0);
  CHECK(nxgl_format_sdl_video_hint_receipt_v2(
            &receipt, text, sizeof(text)) == NXGL_SUCCESS);
  CHECK(strcmp(text,
               "SDL VIDEO HINT RECEIPT: inherited= wayland , kmsdrm  "
               "available=[KMSDRM,dummy] action=preserved-supported "
               "selected=KMSDRM") == 0);
}

static void test_unsupported_hint_is_cleared(void) {
  nxgl_sdl_video_hint_receipt_v2 receipt;
  set_fake_drivers("KMSDRM", "dummy");
  receipt = sanitize("mali", NXGL_SUCCESS);
  CHECK(receipt.action == NXGL_SDL_VIDEO_HINT_V2_CLEARED_UNSUPPORTED);
  CHECK(receipt.inherited_hint_supported == 0);
  CHECK(receipt.hint_removed == 1);
  CHECK(strcmp(receipt.inherited_hint, "mali") == 0);
  CHECK(getenv("SDL_VIDEODRIVER") == NULL);
  fake_video_initialized = SDL_INIT_VIDEO;
  fake_selected_driver = "KMSDRM";
  CHECK(nxgl_complete_sdl_video_hint_receipt_v2(&receipt) == NXGL_SUCCESS);
  CHECK(strcmp(receipt.selected_video_driver, "KMSDRM") == 0);
}

static void test_another_supported_backend_is_never_forced(void) {
  nxgl_sdl_video_hint_receipt_v2 receipt;
  set_fake_drivers("wayland", "x11");
  receipt = sanitize("wayland", NXGL_SUCCESS);
  CHECK(receipt.action == NXGL_SDL_VIDEO_HINT_V2_PRESERVED_SUPPORTED);
  CHECK(environment_equals("SDL_VIDEODRIVER", "wayland"));
  fake_video_initialized = SDL_INIT_VIDEO;
  fake_selected_driver = "wayland";
  CHECK(nxgl_complete_sdl_video_hint_receipt_v2(&receipt) == NXGL_SUCCESS);
  CHECK(strcmp(receipt.selected_video_driver, "wayland") == 0);
}

static void test_only_canonical_hint_may_change(void) {
  nxgl_sdl_video_hint_receipt_v2 receipt;
  set_fake_drivers("KMSDRM", "dummy");
  CHECK(setenv("SDL_VIDEO_DRIVER", "frontend-alias", 1) == 0);
  CHECK(setenv("SDL_DYNAMIC_API", "/frontend/libSDL2.so", 1) == 0);
  CHECK(setenv("SDL_VIDEO_EGL_DRIVER", "/frontend/libEGL.so", 1) == 0);
  CHECK(setenv("SDL_VIDEO_GL_DRIVER", "/frontend/libGLESv2.so", 1) == 0);
  CHECK(setenv("LD_PRELOAD", "/frontend/provider.so", 1) == 0);

  receipt = sanitize("mali", NXGL_SUCCESS);
  CHECK(receipt.action == NXGL_SDL_VIDEO_HINT_V2_CLEARED_UNSUPPORTED);
  CHECK(getenv("SDL_VIDEODRIVER") == NULL);
  CHECK(environment_equals("SDL_VIDEO_DRIVER", "frontend-alias"));
  CHECK(environment_equals("SDL_DYNAMIC_API", "/frontend/libSDL2.so"));
  CHECK(environment_equals("SDL_VIDEO_EGL_DRIVER", "/frontend/libEGL.so"));
  CHECK(environment_equals("SDL_VIDEO_GL_DRIVER",
                           "/frontend/libGLESv2.so"));
  CHECK(environment_equals("LD_PRELOAD", "/frontend/provider.so"));

  CHECK(unsetenv("SDL_VIDEO_DRIVER") == 0);
  CHECK(unsetenv("SDL_DYNAMIC_API") == 0);
  CHECK(unsetenv("SDL_VIDEO_EGL_DRIVER") == 0);
  CHECK(unsetenv("SDL_VIDEO_GL_DRIVER") == 0);
  CHECK(unsetenv("LD_PRELOAD") == 0);
}

static void test_failures_do_not_guess(void) {
  nxgl_sdl_video_hint_receipt_v2 receipt;
  nxgl_sdl_video_hint_receipt_v2 before;
  nxgl_sdl_video_hint_options_v2 options;
  char text[NXGL_SDL_VIDEO_RECEIPT_TEXT_MAX];

  set_fake_drivers(NULL, NULL);
  receipt = sanitize("kmsdrm", NXGL_ERROR_VIDEO_UNAVAILABLE);
  CHECK(receipt.action == NXGL_SDL_VIDEO_HINT_V2_DRIVER_QUERY_FAILED);
  CHECK(strcmp(receipt.inherited_hint, "kmsdrm") == 0);
  CHECK(environment_equals("SDL_VIDEODRIVER", "kmsdrm"));

  set_fake_drivers("KMSDRM", "dummy");
  receipt = sanitize("kmsdrm\ninjected", NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(receipt.action == NXGL_SDL_VIDEO_HINT_V2_INVALID_HINT);
  CHECK(strcmp(receipt.inherited_hint, "<invalid>") == 0);
  CHECK(environment_equals("SDL_VIDEODRIVER", "kmsdrm\ninjected"));
  CHECK(nxgl_format_sdl_video_hint_receipt_v2(
            &receipt, text, sizeof(text)) == NXGL_SUCCESS);
  CHECK(strchr(text, '\n') == NULL);

  fake_video_initialized = SDL_INIT_VIDEO;
  receipt = sanitize("KMSDRM", NXGL_ERROR_INVALID_STATE);
  CHECK(receipt.action == NXGL_SDL_VIDEO_HINT_V2_REFUSED_VIDEO_ACTIVE);
  CHECK(strcmp(receipt.inherited_hint, "KMSDRM") == 0);
  CHECK(environment_equals("SDL_VIDEODRIVER", "KMSDRM"));

  fake_video_initialized = 0u;
  nxgl_sdl_video_hint_options_v2_init(&options);
  options.enabled = 1;
  memset(&receipt, 0x5A, sizeof(receipt));
  before = receipt;
  CHECK(nxgl_arbiter_try_acquire());
  CHECK(nxgl_sanitize_sdl_video_hint_v2(&options, &receipt) ==
        NXGL_ERROR_BUSY);
  nxgl_arbiter_release();
  CHECK(memcmp(&receipt, &before, sizeof(receipt)) == 0);
}

static void test_selected_driver_must_belong_to_target_sdl(void) {
  nxgl_sdl_video_hint_receipt_v2 receipt;
  nxgl_sdl_video_hint_receipt_v2 before;
  set_fake_drivers("KMSDRM", "dummy");
  receipt = sanitize(NULL, NXGL_SUCCESS);
  fake_video_initialized = SDL_INIT_VIDEO;
  fake_selected_driver = "wayland";
  before = receipt;
  CHECK(nxgl_complete_sdl_video_hint_receipt_v2(&receipt) ==
        NXGL_ERROR_STACK_MISMATCH);
  CHECK(memcmp(&receipt, &before, sizeof(receipt)) == 0);
}

int main(void) {
  const char *original = getenv("SDL_VIDEODRIVER");
  char *saved = duplicate_text(original);
  int existed = original != NULL;
  CHECK(!existed || saved != NULL);

  test_disabled_is_inert();
  test_no_hint_keeps_autodetection();
  test_supported_hint_is_preserved_and_completed();
  test_unsupported_hint_is_cleared();
  test_another_supported_backend_is_never_forced();
  test_only_canonical_hint_may_change();
  test_failures_do_not_guess();
  test_selected_driver_must_belong_to_target_sdl();

  if (existed)
    CHECK(setenv("SDL_VIDEODRIVER", saved, 1) == 0);
  else
    CHECK(unsetenv("SDL_VIDEODRIVER") == 0);
  free(saved);
  if (failures) {
    (void)fprintf(stderr, "%d SDL hint sanitizer test(s) failed\n", failures);
    return 1;
  }
  (void)fprintf(stdout,
                "nxgl SDL hint sanitizer tests passed: valid=preserved "
                "invalid=cleared absent=autodetect other=preserved\n");
  return 0;
}
