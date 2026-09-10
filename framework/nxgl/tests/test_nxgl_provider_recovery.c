/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L

#include "nxgl_provider_recovery.h"
#include "nxgl_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static const char *expected_exec_provider;
static const char *failed_setenv_name;
static int coherent_exec_environment_observed;
static int unexpected_exec_calls;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                    #condition);                                             \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

typedef struct saved_environment {
  int present;
  char *value;
} saved_environment;

static saved_environment save_environment(const char *name) {
  saved_environment saved;
  const char *value = getenv(name);
  saved.present = value != NULL;
  saved.value = value ? strdup(value) : NULL;
  return saved;
}

static void restore_environment(const char *name,
                                saved_environment *saved) {
  if (saved->present)
    (void)setenv(name, saved->value ? saved->value : "", 1);
  else
    (void)unsetenv(name);
  free(saved->value);
  saved->value = NULL;
}

static void clear_recovery_environment(void) {
  (void)unsetenv("SDL_VIDEO_EGL_DRIVER");
  (void)unsetenv("SDL_VIDEO_GL_DRIVER");
  (void)unsetenv(NXGL_PROVIDER_RECOVERY_MARKER);
}

static int failing_exec_hook(const char *path, char *const argv[]) {
  CHECK(path != NULL && strcmp(path, "/proc/self/exe") == 0);
  CHECK(argv != NULL && argv[0] != NULL);
  errno = EACCES;
  return -1;
}

static int observing_exec_hook(const char *path, char *const argv[]) {
  const char *egl = getenv("SDL_VIDEO_EGL_DRIVER");
  const char *gles = getenv("SDL_VIDEO_GL_DRIVER");
  const char *marker = getenv(NXGL_PROVIDER_RECOVERY_MARKER);
  const char *preload = getenv("LD_PRELOAD");

  CHECK(path != NULL && strcmp(path, "/proc/self/exe") == 0);
  CHECK(argv != NULL && argv[0] != NULL);
  CHECK(egl != NULL && gles != NULL && marker != NULL);
  if (egl && gles && marker) {
    CHECK(strcmp(marker, "1") == 0);
    CHECK(strcmp(egl, expected_exec_provider) == 0);
    CHECK(strcmp(gles, egl) == 0);
  }
  CHECK(preload != NULL &&
        strcmp(preload, "nxgl-test-preload-sentinel") == 0);
  coherent_exec_environment_observed = 1;
  errno = EINTR;
  return -1;
}

static int unexpected_exec_hook(const char *path, char *const argv[]) {
  (void)path;
  (void)argv;
  ++unexpected_exec_calls;
  errno = ECANCELED;
  return -1;
}

static int selective_setenv_hook(const char *name, const char *value,
                                 int overwrite) {
  if (failed_setenv_name && strcmp(name, failed_setenv_name) == 0) {
    errno = ENOMEM;
    return -1;
  }
  return setenv(name, value, overwrite);
}

static int passthrough_unsetenv_hook(const char *name) {
  return unsetenv(name);
}

static nxgl_sdl_provider_probe_receipt_v2 probe_good_provider(
    const char *provider, nxgl_provider_probe_mode_v2 mode) {
  static const char *const symbols[] = {"glCreateShader", "glGetString"};
  nxgl_sdl_provider_probe_options_v2 options;
  nxgl_sdl_provider_probe_receipt_v2 receipt;

  nxgl_sdl_provider_probe_options_v2_init(&options);
  options.enabled = 1;
  options.mode = mode;
  options.video_torn_down =
      mode == NXGL_PROVIDER_PROBE_V2_EGL_DEFAULT_DISPLAY;
  options.video_backend = "kmsdrm";
  options.provider = provider;
  options.required_engine_gles_symbols = symbols;
  options.required_engine_gles_symbol_count =
      sizeof(symbols) / sizeof(symbols[0]);
  nxgl_sdl_provider_probe_receipt_v2_init(&receipt);
  CHECK(nxgl_probe_sdl_provider_v2(&options, &receipt) == NXGL_SUCCESS);
  CHECK(receipt.reason == NXGL_PROVIDER_RECOVERY_V2_NONE);
  CHECK(receipt.usable == 1);
  CHECK(receipt.exports_egl == 1);
  CHECK(receipt.exports_engine_gles == 1);
  CHECK(receipt.provider_path[0] == '/');
  if (mode == NXGL_PROVIDER_PROBE_V2_EGL_DEFAULT_DISPLAY) {
    CHECK(receipt.egl_initialized == 1);
    CHECK(receipt.egl_major == 1);
    CHECK(receipt.egl_minor == 4);
  }
  return receipt;
}

static void test_probe_is_explicit_and_fail_closed(const char *good,
                                                   const char *mixed,
                                                   const char *dependency) {
  static const char *const one_symbol[] = {"glCreateShader"};
  static const char *const missing_symbol[] = {"glNxglDefinitelyMissing"};
  nxgl_sdl_provider_probe_options_v2 options;
  nxgl_sdl_provider_probe_receipt_v2 receipt;
  nxgl_sdl_provider_probe_receipt_v2 receipt_before;
  void *loaded;

  nxgl_sdl_provider_probe_options_v2_init(&options);
  nxgl_sdl_provider_probe_receipt_v2_init(&receipt);
  options.provider = "/not/opened/while/disabled.so";
  CHECK(nxgl_probe_sdl_provider_v2(&options, &receipt) == NXGL_NO_ACTION);
  CHECK(receipt.reason == NXGL_PROVIDER_RECOVERY_V2_DISABLED);

  memset(&receipt, 0xA5, sizeof(receipt));
  receipt_before = receipt;
  options.api_version = 0u;
  CHECK(nxgl_probe_sdl_provider_v2(&options, &receipt) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&receipt, &receipt_before, sizeof(receipt)) == 0);

  nxgl_sdl_provider_probe_options_v2_init(&options);
  options.enabled = 1;
  options.mode = NXGL_PROVIDER_PROBE_V2_SYMBOLS_ONLY;
  options.video_backend = "kmsdrm";
  options.provider = good;
  options.required_engine_gles_symbols = one_symbol;
  options.required_engine_gles_symbol_count = 1u;
  memset(&receipt, 0x5A, sizeof(receipt));
  receipt_before = receipt;
  CHECK(nxgl_arbiter_try_acquire() == 1);
  CHECK(nxgl_probe_sdl_provider_v2(&options, &receipt) == NXGL_ERROR_BUSY);
  nxgl_arbiter_release();
  CHECK(memcmp(&receipt, &receipt_before, sizeof(receipt)) == 0);
  CHECK(getenv("SDL_VIDEO_EGL_DRIVER") == NULL);
  CHECK(getenv("SDL_VIDEO_GL_DRIVER") == NULL);
  CHECK(getenv(NXGL_PROVIDER_RECOVERY_MARKER) == NULL);

  nxgl_sdl_provider_probe_options_v2_init(&options);
  options.enabled = 1;
  options.mode = NXGL_PROVIDER_PROBE_V2_SYMBOLS_ONLY;
  options.video_backend = "kmsdrm";
  options.provider = "/nxgl/provider/does-not-exist.so";
  options.required_engine_gles_symbols = one_symbol;
  options.required_engine_gles_symbol_count = 1u;
  CHECK(nxgl_probe_sdl_provider_v2(&options, &receipt) == NXGL_NO_ACTION);
  CHECK(receipt.reason ==
        NXGL_PROVIDER_RECOVERY_V2_CANDIDATE_UNAVAILABLE);

  options.provider = dependency;
  CHECK(nxgl_probe_sdl_provider_v2(&options, &receipt) == NXGL_NO_ACTION);
  CHECK(receipt.reason == NXGL_PROVIDER_RECOVERY_V2_EGL_SYMBOLS_MISSING);

  options.provider = good;
  options.required_engine_gles_symbols = missing_symbol;
  CHECK(nxgl_probe_sdl_provider_v2(&options, &receipt) == NXGL_NO_ACTION);
  CHECK(receipt.reason == NXGL_PROVIDER_RECOVERY_V2_ENGINE_SYMBOLS_MISSING);

  (void)probe_good_provider(good, NXGL_PROVIDER_PROBE_V2_SYMBOLS_ONLY);
  (void)probe_good_provider(good,
                            NXGL_PROVIDER_PROBE_V2_EGL_DEFAULT_DISPLAY);

  nxgl_sdl_provider_probe_options_v2_init(&options);
  options.enabled = 1;
  options.mode = NXGL_PROVIDER_PROBE_V2_SYMBOLS_ONLY;
  options.video_backend = "kmsdrm";
  options.provider = mixed;
  options.required_engine_gles_symbols = one_symbol;
  options.required_engine_gles_symbol_count = 1u;
  CHECK(nxgl_probe_sdl_provider_v2(&options, &receipt) == NXGL_NO_ACTION);
  CHECK(receipt.reason == NXGL_PROVIDER_RECOVERY_V2_MIXED_OBJECTS);

  options.provider = good;
  options.mode = NXGL_PROVIDER_PROBE_V2_EGL_DEFAULT_DISPLAY;
  options.video_torn_down = 0;
  CHECK(nxgl_probe_sdl_provider_v2(&options, &receipt) == NXGL_NO_ACTION);
  CHECK(receipt.reason ==
        NXGL_PROVIDER_RECOVERY_V2_VIDEO_NOT_TORN_DOWN);

  options.video_torn_down = 1;
  CHECK(setenv("NXGL_TEST_EGL_INITIALIZE_FAIL", "1", 1) == 0);
  CHECK(nxgl_probe_sdl_provider_v2(&options, &receipt) == NXGL_NO_ACTION);
  CHECK(receipt.reason ==
        NXGL_PROVIDER_RECOVERY_V2_EGL_INITIALIZE_FAILED);
  CHECK(unsetenv("NXGL_TEST_EGL_INITIALIZE_FAIL") == 0);

  loaded = dlopen(good, RTLD_NOW | RTLD_LOCAL);
  CHECK(loaded != NULL);
  options.mode = NXGL_PROVIDER_PROBE_V2_SYMBOLS_ONLY;
  options.reject_if_already_loaded = 1;
  CHECK(nxgl_probe_sdl_provider_v2(&options, &receipt) == NXGL_NO_ACTION);
  CHECK(receipt.reason ==
        NXGL_PROVIDER_RECOVERY_V2_PROVIDER_ALREADY_LOADED);
  if (loaded)
    CHECK(dlclose(loaded) == 0);
}

static void test_reexec_requires_every_gate(
    const nxgl_sdl_provider_probe_receipt_v2 *receipt, char *const *argv) {
  nxgl_sdl_provider_reexec_options_v2 options;
  nxgl_sdl_provider_reexec_result_v2 result;
  nxgl_sdl_provider_reexec_result_v2 result_before;
  nxgl_sdl_provider_probe_receipt_v2 changed = *receipt;

  nxgl_sdl_provider_reexec_options_v2_init(&options);
  options.enabled = 1;
  options.authorization = NXGL_SDL_PROVIDER_PAIR_BIND_COHERENT;
  options.provider = receipt;
  options.video_torn_down = 1;
  options.argv = argv;
  memset(&result, 0xA5, sizeof(result));
  result_before = result;
  options.api_version = 0u;
  CHECK(nxgl_reexec_sdl_provider_pair_v2(&options, &result) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&result, &result_before, sizeof(result)) == 0);

  options.api_version = NXGL_API_VERSION_V2;
  memset(&result, 0x5A, sizeof(result));
  result_before = result;
  CHECK(nxgl_arbiter_try_acquire() == 1);
  CHECK(nxgl_reexec_sdl_provider_pair_v2(&options, &result) ==
        NXGL_ERROR_BUSY);
  nxgl_arbiter_release();
  CHECK(memcmp(&result, &result_before, sizeof(result)) == 0);
  CHECK(getenv("SDL_VIDEO_EGL_DRIVER") == NULL);
  CHECK(getenv("SDL_VIDEO_GL_DRIVER") == NULL);
  CHECK(getenv(NXGL_PROVIDER_RECOVERY_MARKER) == NULL);

  nxgl_sdl_provider_reexec_result_v2_init(&result);
  options.authorization = NXGL_SDL_PROVIDER_PAIR_NO_ACTION;
  CHECK(nxgl_reexec_sdl_provider_pair_v2(&options, &result) ==
        NXGL_NO_ACTION);
  CHECK(result.reason == NXGL_PROVIDER_RECOVERY_V2_NOT_AUTHORIZED);
  CHECK(getenv("SDL_VIDEO_EGL_DRIVER") == NULL);

  options.authorization = NXGL_SDL_PROVIDER_PAIR_BIND_COHERENT;
  options.video_torn_down = 0;
  CHECK(nxgl_reexec_sdl_provider_pair_v2(&options, &result) ==
        NXGL_NO_ACTION);
  CHECK(result.reason == NXGL_PROVIDER_RECOVERY_V2_VIDEO_NOT_TORN_DOWN);

  options.video_torn_down = 1;
  CHECK(setenv("SDL_VIDEO_EGL_DRIVER", "firmware-owned", 1) == 0);
  CHECK(nxgl_reexec_sdl_provider_pair_v2(&options, &result) ==
        NXGL_NO_ACTION);
  CHECK(result.reason ==
        NXGL_PROVIDER_RECOVERY_V2_INHERITED_PROVIDER_OVERRIDE);
  CHECK(strcmp(getenv("SDL_VIDEO_EGL_DRIVER"), "firmware-owned") == 0);
  CHECK(unsetenv("SDL_VIDEO_EGL_DRIVER") == 0);

  CHECK(setenv(NXGL_PROVIDER_RECOVERY_MARKER, "1", 1) == 0);
  CHECK(nxgl_reexec_sdl_provider_pair_v2(&options, &result) ==
        NXGL_NO_ACTION);
  CHECK(result.reason == NXGL_PROVIDER_RECOVERY_V2_ALREADY_APPLIED);
  CHECK(unsetenv(NXGL_PROVIDER_RECOVERY_MARKER) == 0);

  ++changed.inode;
  options.provider = &changed;
  CHECK(nxgl_reexec_sdl_provider_pair_v2(&options, &result) ==
        NXGL_NO_ACTION);
  CHECK(result.reason == NXGL_PROVIDER_RECOVERY_V2_PROVIDER_CHANGED);
  CHECK(getenv("SDL_VIDEO_EGL_DRIVER") == NULL);
}

static void test_one_shot_reexec(
    const nxgl_sdl_provider_probe_receipt_v2 *receipt, const char *self) {
  nxgl_sdl_provider_reexec_options_v2 options;
  nxgl_sdl_provider_reexec_result_v2 result;
  char self_argument[NXGL_PROVIDER_RECOVERY_PATH_MAX];
  char mode_argument[] = "--reexec-child";
  char provider_argument[NXGL_PROVIDER_RECOVERY_PATH_MAX];
  char *child_argv[4];

  (void)snprintf(self_argument, sizeof(self_argument), "%s", self);
  (void)snprintf(provider_argument, sizeof(provider_argument), "%s",
                 receipt->provider_path);
  child_argv[0] = self_argument;
  child_argv[1] = mode_argument;
  child_argv[2] = provider_argument;
  child_argv[3] = NULL;

  clear_recovery_environment();
  CHECK(setenv("LD_PRELOAD", "nxgl-test-preload-sentinel", 1) == 0);
  nxgl_sdl_provider_reexec_options_v2_init(&options);
  nxgl_sdl_provider_reexec_result_v2_init(&result);
  options.enabled = 1;
  options.authorization = NXGL_SDL_PROVIDER_PAIR_BIND_COHERENT;
  options.provider = receipt;
  options.video_torn_down = 1;
  options.argv = child_argv;

  nxgl_test_provider_recovery_set_exec_hook(failing_exec_hook);
  CHECK(nxgl_reexec_sdl_provider_pair_v2(&options, &result) ==
        NXGL_ERROR_VIDEO_UNAVAILABLE);
  CHECK(result.reason == NXGL_PROVIDER_RECOVERY_V2_EXEC_FAILED);
  CHECK(result.environment_restored == 1);
  CHECK(result.system_error == EACCES);
  CHECK(getenv("SDL_VIDEO_EGL_DRIVER") == NULL);
  CHECK(getenv("SDL_VIDEO_GL_DRIVER") == NULL);
  CHECK(getenv(NXGL_PROVIDER_RECOVERY_MARKER) == NULL);

  expected_exec_provider = receipt->provider_path;
  coherent_exec_environment_observed = 0;
  nxgl_test_provider_recovery_set_exec_hook(observing_exec_hook);
  CHECK(nxgl_reexec_sdl_provider_pair_v2(&options, &result) ==
        NXGL_ERROR_VIDEO_UNAVAILABLE);
  CHECK(coherent_exec_environment_observed == 1);
  CHECK(result.reason == NXGL_PROVIDER_RECOVERY_V2_EXEC_FAILED);
  CHECK(result.environment_restored == 1);
  CHECK(result.system_error == EINTR);
  CHECK(getenv("SDL_VIDEO_EGL_DRIVER") == NULL);
  CHECK(getenv("SDL_VIDEO_GL_DRIVER") == NULL);
  CHECK(getenv(NXGL_PROVIDER_RECOVERY_MARKER) == NULL);
  nxgl_test_provider_recovery_set_exec_hook(NULL);
}

static void test_partial_environment_failure_rolls_back(
    const nxgl_sdl_provider_probe_receipt_v2 *receipt, char *const *argv) {
  static const char *const failures_to_inject[] = {
      "SDL_VIDEO_EGL_DRIVER",
      "SDL_VIDEO_GL_DRIVER",
      NXGL_PROVIDER_RECOVERY_MARKER,
  };
  nxgl_sdl_provider_reexec_options_v2 options;
  nxgl_sdl_provider_reexec_result_v2 result;
  size_t index;

  nxgl_sdl_provider_reexec_options_v2_init(&options);
  options.enabled = 1;
  options.authorization = NXGL_SDL_PROVIDER_PAIR_BIND_COHERENT;
  options.provider = receipt;
  options.video_torn_down = 1;
  options.argv = argv;
  nxgl_test_provider_recovery_set_environment_hooks(
      selective_setenv_hook, passthrough_unsetenv_hook);

  for (index = 0u;
       index < sizeof(failures_to_inject) / sizeof(failures_to_inject[0]);
       ++index) {
    clear_recovery_environment();
    failed_setenv_name = failures_to_inject[index];
    nxgl_sdl_provider_reexec_result_v2_init(&result);
    CHECK(nxgl_reexec_sdl_provider_pair_v2(&options, &result) ==
          NXGL_ERROR_OUT_OF_MEMORY);
    CHECK(result.reason == NXGL_PROVIDER_RECOVERY_V2_ENVIRONMENT_FAILED);
    CHECK(result.environment_restored == 1);
    CHECK(result.system_error == ENOMEM);
    CHECK(getenv("SDL_VIDEO_EGL_DRIVER") == NULL);
    CHECK(getenv("SDL_VIDEO_GL_DRIVER") == NULL);
    CHECK(getenv(NXGL_PROVIDER_RECOVERY_MARKER) == NULL);
    CHECK(strcmp(getenv("LD_PRELOAD"),
                 "nxgl-test-preload-sentinel") == 0);
  }

  failed_setenv_name = NULL;
  nxgl_test_provider_recovery_set_environment_hooks(NULL, NULL);
}

typedef unsigned int (*provider_counter_fn)(void);

static void test_terminate_failure_poison(
    const char *good,
    const nxgl_sdl_provider_probe_receipt_v2 *previous_receipt,
    char *const *argv) {
  static const char *const symbols[] = {"glCreateShader", "glGetString"};
  nxgl_sdl_provider_probe_options_v2 probe_options;
  nxgl_sdl_provider_probe_receipt_v2 receipt;
  nxgl_sdl_provider_reexec_options_v2 reexec_options;
  nxgl_sdl_provider_reexec_result_v2 result;
  provider_counter_fn get_initialize_calls;
  provider_counter_fn get_terminate_calls;
  unsigned int initialize_calls;
  unsigned int terminate_calls;
  void *loaded;

  nxgl_sdl_provider_probe_options_v2_init(&probe_options);
  probe_options.enabled = 1;
  probe_options.mode = NXGL_PROVIDER_PROBE_V2_EGL_DEFAULT_DISPLAY;
  probe_options.video_torn_down = 1;
  probe_options.video_backend = "kmsdrm";
  probe_options.provider = good;
  probe_options.required_engine_gles_symbols = symbols;
  probe_options.required_engine_gles_symbol_count =
      sizeof(symbols) / sizeof(symbols[0]);

  CHECK(setenv("NXGL_TEST_EGL_TERMINATE_FAIL", "1", 1) == 0);
  nxgl_sdl_provider_probe_receipt_v2_init(&receipt);
  CHECK(nxgl_probe_sdl_provider_v2(&probe_options, &receipt) ==
        NXGL_NO_ACTION);
  CHECK(receipt.reason ==
        NXGL_PROVIDER_RECOVERY_V2_EGL_TERMINATE_FAILED);
  CHECK(receipt.usable == 0);
  CHECK(unsetenv("NXGL_TEST_EGL_TERMINATE_FAIL") == 0);

  loaded = dlopen(good, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
  CHECK(loaded != NULL);
  get_initialize_calls = loaded
                             ? (provider_counter_fn)dlsym(
                                   loaded, "nxglTestGetInitializeCalls")
                             : NULL;
  get_terminate_calls = loaded
                            ? (provider_counter_fn)dlsym(
                                  loaded, "nxglTestGetTerminateCalls")
                            : NULL;
  CHECK(get_initialize_calls != NULL);
  CHECK(get_terminate_calls != NULL);
  initialize_calls = get_initialize_calls ? get_initialize_calls() : 0u;
  terminate_calls = get_terminate_calls ? get_terminate_calls() : 0u;

  probe_options.mode = NXGL_PROVIDER_PROBE_V2_SYMBOLS_ONLY;
  probe_options.video_torn_down = 0;
  nxgl_sdl_provider_probe_receipt_v2_init(&receipt);
  CHECK(nxgl_probe_sdl_provider_v2(&probe_options, &receipt) ==
        NXGL_NO_ACTION);
  CHECK(receipt.reason ==
        NXGL_PROVIDER_RECOVERY_V2_EGL_TERMINATE_FAILED);
  CHECK(receipt.usable == 0);
  if (get_initialize_calls)
    CHECK(get_initialize_calls() == initialize_calls);
  if (get_terminate_calls)
    CHECK(get_terminate_calls() == terminate_calls);

  nxgl_sdl_provider_reexec_options_v2_init(&reexec_options);
  reexec_options.enabled = 1;
  reexec_options.authorization = NXGL_SDL_PROVIDER_PAIR_BIND_COHERENT;
  reexec_options.provider = previous_receipt;
  reexec_options.video_torn_down = 1;
  reexec_options.argv = argv;
  unexpected_exec_calls = 0;
  nxgl_test_provider_recovery_set_exec_hook(unexpected_exec_hook);
  nxgl_sdl_provider_reexec_result_v2_init(&result);
  CHECK(nxgl_reexec_sdl_provider_pair_v2(&reexec_options, &result) ==
        NXGL_NO_ACTION);
  CHECK(result.reason ==
        NXGL_PROVIDER_RECOVERY_V2_EGL_TERMINATE_FAILED);
  CHECK(unexpected_exec_calls == 0);
  CHECK(getenv("SDL_VIDEO_EGL_DRIVER") == NULL);
  CHECK(getenv("SDL_VIDEO_GL_DRIVER") == NULL);
  CHECK(getenv(NXGL_PROVIDER_RECOVERY_MARKER) == NULL);

  if (loaded)
    CHECK(dlclose(loaded) == 0);
  nxgl_test_provider_recovery_set_exec_hook(NULL);
}

int main(int argc, char **argv) {
  saved_environment saved_egl;
  saved_environment saved_gl;
  saved_environment saved_marker;
  saved_environment saved_preload;
  nxgl_sdl_provider_probe_receipt_v2 receipt;
  char *gate_argv[2];

  if (argc != 4) {
    (void)fprintf(stderr,
                  "usage: %s GOOD_PROVIDER MIXED_PROVIDER DEPENDENCY\n",
                  argv[0]);
    return 2;
  }

  saved_egl = save_environment("SDL_VIDEO_EGL_DRIVER");
  saved_gl = save_environment("SDL_VIDEO_GL_DRIVER");
  saved_marker = save_environment(NXGL_PROVIDER_RECOVERY_MARKER);
  saved_preload = save_environment("LD_PRELOAD");
  clear_recovery_environment();

  test_probe_is_explicit_and_fail_closed(argv[1], argv[2], argv[3]);
  receipt = probe_good_provider(
      argv[1], NXGL_PROVIDER_PROBE_V2_EGL_DEFAULT_DISPLAY);
  gate_argv[0] = argv[0];
  gate_argv[1] = NULL;
  test_reexec_requires_every_gate(&receipt, gate_argv);
  CHECK(setenv("LD_PRELOAD", "nxgl-test-preload-sentinel", 1) == 0);
  test_partial_environment_failure_rolls_back(&receipt, gate_argv);
  test_one_shot_reexec(&receipt, argv[0]);
  test_terminate_failure_poison(argv[1], &receipt, gate_argv);

  restore_environment("SDL_VIDEO_EGL_DRIVER", &saved_egl);
  restore_environment("SDL_VIDEO_GL_DRIVER", &saved_gl);
  restore_environment(NXGL_PROVIDER_RECOVERY_MARKER, &saved_marker);
  restore_environment("LD_PRELOAD", &saved_preload);

  if (failures) {
    (void)fprintf(stderr, "%d provider recovery test(s) failed\n", failures);
    return 1;
  }
  (void)fprintf(stdout, "nxgl provider recovery tests passed\n");
  return 0;
}
