/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "nxgl_provider_recovery.h"

#include "nxgl_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NXGL_PROVIDER_LOADED_MAX 256u
#define NXGL_PROVIDER_SYMBOL_NAME_MAX 128u

typedef struct nxgl_provider_file_identity {
  uint64_t device;
  uint64_t inode;
  uint64_t size;
  int64_t mtime_seconds;
  int64_t mtime_nanoseconds;
} nxgl_provider_file_identity;

typedef struct nxgl_provider_loaded_snapshot {
  nxgl_provider_file_identity entries[NXGL_PROVIDER_LOADED_MAX];
  size_t count;
  int overflow;
} nxgl_provider_loaded_snapshot;

typedef void *(*nxgl_egl_get_display_fn)(void *native_display);
typedef unsigned int (*nxgl_egl_initialize_fn)(void *display, int *major,
                                               int *minor);
typedef unsigned int (*nxgl_egl_terminate_fn)(void *display);

/* A failed eglTerminate means provider code may still own a live display.
 * Keep the retained handle and permanently refuse another probe/re-exec in
 * this process. The flag is read and written only under nxgl's arbiter. */
static int nxgl_provider_process_poisoned;

#if defined(NXGL_PROVIDER_RECOVERY_TESTING)
static nxgl_provider_recovery_exec_test_hook nxgl_provider_exec_test_hook;
static nxgl_provider_recovery_setenv_test_hook nxgl_provider_setenv_test_hook;
static nxgl_provider_recovery_unsetenv_test_hook
    nxgl_provider_unsetenv_test_hook;

void nxgl_test_provider_recovery_set_exec_hook(
    nxgl_provider_recovery_exec_test_hook hook) {
  nxgl_provider_exec_test_hook = hook;
}

void nxgl_test_provider_recovery_set_environment_hooks(
    nxgl_provider_recovery_setenv_test_hook setenv_hook,
    nxgl_provider_recovery_unsetenv_test_hook unsetenv_hook) {
  nxgl_provider_setenv_test_hook = setenv_hook;
  nxgl_provider_unsetenv_test_hook = unsetenv_hook;
}

static int nxgl_provider_execv(const char *path, char *const argv[]) {
  if (nxgl_provider_exec_test_hook)
    return nxgl_provider_exec_test_hook(path, argv);
  errno = ENOSYS;
  return -1;
}

static int nxgl_provider_setenv(const char *name, const char *value,
                                int overwrite) {
  if (nxgl_provider_setenv_test_hook)
    return nxgl_provider_setenv_test_hook(name, value, overwrite);
  return setenv(name, value, overwrite);
}

static int nxgl_provider_unsetenv(const char *name) {
  if (nxgl_provider_unsetenv_test_hook)
    return nxgl_provider_unsetenv_test_hook(name);
  return unsetenv(name);
}
#else
static int nxgl_provider_execv(const char *path, char *const argv[]) {
  return execv(path, argv);
}

static int nxgl_provider_setenv(const char *name, const char *value,
                                int overwrite) {
  return setenv(name, value, overwrite);
}

static int nxgl_provider_unsetenv(const char *name) {
  return unsetenv(name);
}
#endif

static int nxgl_provider_boolean_valid(int value) {
  return value == 0 || value == 1;
}

static int nxgl_provider_file_identity_read(
    const char *path, nxgl_provider_file_identity *identity) {
  struct stat status;

  if (!path || !path[0] || !identity || stat(path, &status) != 0 ||
      !S_ISREG(status.st_mode) || status.st_size < 0)
    return 0;
  identity->device = (uint64_t)status.st_dev;
  identity->inode = (uint64_t)status.st_ino;
  identity->size = (uint64_t)status.st_size;
  identity->mtime_seconds = (int64_t)status.st_mtim.tv_sec;
  identity->mtime_nanoseconds = (int64_t)status.st_mtim.tv_nsec;
  return 1;
}

static int nxgl_provider_file_identity_equal(
    const nxgl_provider_file_identity *left,
    const nxgl_provider_file_identity *right) {
  return left && right && left->device == right->device &&
         left->inode == right->inode && left->size == right->size &&
         left->mtime_seconds == right->mtime_seconds &&
         left->mtime_nanoseconds == right->mtime_nanoseconds;
}

static int nxgl_provider_loaded_snapshot_cb(struct dl_phdr_info *info,
                                            size_t size, void *userdata) {
  nxgl_provider_loaded_snapshot *snapshot =
      (nxgl_provider_loaded_snapshot *)userdata;
  nxgl_provider_file_identity identity;
  (void)size;

  if (!snapshot || !info || !info->dlpi_name || !info->dlpi_name[0] ||
      !nxgl_provider_file_identity_read(info->dlpi_name, &identity))
    return 0;
  if (snapshot->count >= NXGL_PROVIDER_LOADED_MAX) {
    snapshot->overflow = 1;
    return 1;
  }
  snapshot->entries[snapshot->count++] = identity;
  return 0;
}

static int nxgl_provider_snapshot_contains(
    const nxgl_provider_loaded_snapshot *snapshot,
    const nxgl_provider_file_identity *identity) {
  size_t index;

  if (!snapshot || !identity || snapshot->overflow)
    return 1;
  for (index = 0u; index < snapshot->count; ++index) {
    if (nxgl_provider_file_identity_equal(&snapshot->entries[index],
                                          identity))
      return 1;
  }
  return 0;
}

static int nxgl_provider_symbol_name_valid(const char *name) {
  size_t index;
  size_t length;

  if (!name)
    return 0;
  length = strnlen(name, NXGL_PROVIDER_SYMBOL_NAME_MAX + 1u);
  if (length == 0u || length > NXGL_PROVIDER_SYMBOL_NAME_MAX)
    return 0;
  for (index = 0u; index < length; ++index) {
    const unsigned char value = (unsigned char)name[index];
    if (!((value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
          (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
          (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
          value == (unsigned char)'_'))
      return 0;
  }
  return 1;
}

static int nxgl_provider_canonical_symbol_owner(
    void *symbol, char output[NXGL_PROVIDER_RECOVERY_PATH_MAX]) {
  Dl_info info;

  if (!symbol || !output || !dladdr(symbol, &info) || !info.dli_fname ||
      !info.dli_fname[0] || !realpath(info.dli_fname, output))
    return 0;
  return output[0] == '/';
}

/* 1 = present in the selected DSO, 0 = absent, -1 = resolved from a
 * dependency/different DSO. */
static int nxgl_provider_symbol_from_object(void *handle, const char *name,
                                            const char *provider_path,
                                            void **symbol) {
  char owner[NXGL_PROVIDER_RECOVERY_PATH_MAX];
  void *resolved;

  if (!handle || !name || !provider_path)
    return 0;
  dlerror();
  resolved = dlsym(handle, name);
  if (!resolved || dlerror() != NULL)
    return 0;
  if (!nxgl_provider_canonical_symbol_owner(resolved, owner) ||
      strcmp(owner, provider_path) != 0)
    return -1;
  if (symbol)
    *symbol = resolved;
  return 1;
}

static void nxgl_provider_receipt_set_identity(
    nxgl_sdl_provider_probe_receipt_v2 *receipt,
    const nxgl_provider_file_identity *identity) {
  receipt->device = identity->device;
  receipt->inode = identity->inode;
  receipt->size = identity->size;
  receipt->mtime_seconds = identity->mtime_seconds;
  receipt->mtime_nanoseconds = identity->mtime_nanoseconds;
}

static void nxgl_provider_receipt_get_identity(
    const nxgl_sdl_provider_probe_receipt_v2 *receipt,
    nxgl_provider_file_identity *identity) {
  identity->device = receipt->device;
  identity->inode = receipt->inode;
  identity->size = receipt->size;
  identity->mtime_seconds = receipt->mtime_seconds;
  identity->mtime_nanoseconds = receipt->mtime_nanoseconds;
}

void nxgl_sdl_provider_probe_options_v2_init(
    nxgl_sdl_provider_probe_options_v2 *options) {
  if (!options)
    return;
  memset(options, 0, sizeof(*options));
  options->api_version = NXGL_API_VERSION_V2;
  options->struct_size = sizeof(*options);
}

void nxgl_sdl_provider_probe_receipt_v2_init(
    nxgl_sdl_provider_probe_receipt_v2 *receipt) {
  if (!receipt)
    return;
  memset(receipt, 0, sizeof(*receipt));
  receipt->api_version = NXGL_API_VERSION_V2;
  receipt->struct_size = sizeof(*receipt);
}

void nxgl_sdl_provider_reexec_options_v2_init(
    nxgl_sdl_provider_reexec_options_v2 *options) {
  if (!options)
    return;
  memset(options, 0, sizeof(*options));
  options->api_version = NXGL_API_VERSION_V2;
  options->struct_size = sizeof(*options);
}

void nxgl_sdl_provider_reexec_result_v2_init(
    nxgl_sdl_provider_reexec_result_v2 *result) {
  if (!result)
    return;
  memset(result, 0, sizeof(*result));
  result->api_version = NXGL_API_VERSION_V2;
  result->struct_size = sizeof(*result);
  result->environment_restored = 1;
}

static int nxgl_provider_probe_options_valid(
    const nxgl_sdl_provider_probe_options_v2 *options) {
  size_t index;

  if (!options || options->api_version != NXGL_API_VERSION_V2 ||
      options->struct_size < sizeof(*options) ||
      !nxgl_provider_boolean_valid(options->enabled))
    return 0;
  if (!options->enabled)
    return 1;
  if ((options->mode != NXGL_PROVIDER_PROBE_V2_SYMBOLS_ONLY &&
      options->mode != NXGL_PROVIDER_PROBE_V2_EGL_DEFAULT_DISPLAY) ||
      !nxgl_provider_boolean_valid(options->reject_if_already_loaded) ||
      !nxgl_provider_boolean_valid(options->video_torn_down) ||
      !options->provider || !options->provider[0] ||
      strnlen(options->provider, NXGL_PROVIDER_RECOVERY_PATH_MAX) >=
          NXGL_PROVIDER_RECOVERY_PATH_MAX ||
      !options->required_engine_gles_symbols ||
      options->required_engine_gles_symbol_count == 0u ||
      options->required_engine_gles_symbol_count >
          NXGL_PROVIDER_RECOVERY_SYMBOL_MAX)
    return 0;
  for (index = 0u; index < options->required_engine_gles_symbol_count;
       ++index) {
    if (!nxgl_provider_symbol_name_valid(
            options->required_engine_gles_symbols[index]))
      return 0;
  }
  return 1;
}

int nxgl_probe_sdl_provider_v2(
    const nxgl_sdl_provider_probe_options_v2 *options,
    nxgl_sdl_provider_probe_receipt_v2 *receipt) {
  nxgl_sdl_provider_probe_receipt_v2 local;
  nxgl_provider_loaded_snapshot loaded;
  nxgl_provider_file_identity identity;
  char provider_path[NXGL_PROVIDER_RECOVERY_PATH_MAX];
  const char *provider_name;
  void *handle = NULL;
  void *already_loaded = NULL;
  void *get_display_symbol = NULL;
  void *initialize_symbol = NULL;
  void *terminate_symbol = NULL;
  size_t index;
  int symbol_status;
  int result = NXGL_NO_ACTION;
  int retain_handle = 0;

  if (!receipt || !nxgl_provider_probe_options_valid(options))
    return NXGL_ERROR_INVALID_ARGUMENT;
  nxgl_sdl_provider_probe_receipt_v2_init(&local);
  local.mode = options->mode;
  if (!options->enabled) {
    local.reason = NXGL_PROVIDER_RECOVERY_V2_DISABLED;
    *receipt = local;
    return NXGL_NO_ACTION;
  }
  if (!nxgl_arbiter_try_acquire())
    return NXGL_ERROR_BUSY;

  if (nxgl_provider_process_poisoned) {
    local.reason = NXGL_PROVIDER_RECOVERY_V2_EGL_TERMINATE_FAILED;
    goto done;
  }
  if (options->mode == NXGL_PROVIDER_PROBE_V2_EGL_DEFAULT_DISPLAY &&
      !options->video_torn_down) {
    local.reason = NXGL_PROVIDER_RECOVERY_V2_VIDEO_NOT_TORN_DOWN;
    goto done;
  }

  memset(&loaded, 0, sizeof(loaded));
  if (options->reject_if_already_loaded) {
    (void)dl_iterate_phdr(nxgl_provider_loaded_snapshot_cb, &loaded);
    already_loaded =
        dlopen(options->provider, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    if (already_loaded) {
      (void)dlclose(already_loaded);
      local.reason = NXGL_PROVIDER_RECOVERY_V2_PROVIDER_ALREADY_LOADED;
      goto done;
    }
  }

  handle = dlopen(options->provider, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    local.reason = NXGL_PROVIDER_RECOVERY_V2_CANDIDATE_UNAVAILABLE;
    goto done;
  }
  dlerror();
  get_display_symbol = dlsym(handle, "eglGetDisplay");
  if (!get_display_symbol || dlerror() != NULL ||
      !nxgl_provider_canonical_symbol_owner(get_display_symbol,
                                            provider_path) ||
      !nxgl_provider_file_identity_read(provider_path, &identity)) {
    local.reason = NXGL_PROVIDER_RECOVERY_V2_EGL_SYMBOLS_MISSING;
    goto done;
  }
  if (options->reject_if_already_loaded &&
      nxgl_provider_snapshot_contains(&loaded, &identity)) {
    local.reason = NXGL_PROVIDER_RECOVERY_V2_PROVIDER_ALREADY_LOADED;
    goto done;
  }
  provider_name = strrchr(provider_path, '/');
  provider_name = provider_name ? provider_name + 1 : provider_path;
  if (!nxgl_provider_name_compatible(options->video_backend, provider_name)) {
    local.reason = NXGL_PROVIDER_RECOVERY_V2_TRANSPORT_MISMATCH;
    goto done;
  }
  symbol_status = nxgl_provider_symbol_from_object(
      handle, "eglGetDisplay", provider_path, &get_display_symbol);
  if (symbol_status != 1) {
    local.reason = symbol_status < 0
                       ? NXGL_PROVIDER_RECOVERY_V2_MIXED_OBJECTS
                       : NXGL_PROVIDER_RECOVERY_V2_EGL_SYMBOLS_MISSING;
    goto done;
  }
  symbol_status = nxgl_provider_symbol_from_object(
      handle, "eglInitialize", provider_path, &initialize_symbol);
  if (symbol_status != 1) {
    local.reason = symbol_status < 0
                       ? NXGL_PROVIDER_RECOVERY_V2_MIXED_OBJECTS
                       : NXGL_PROVIDER_RECOVERY_V2_EGL_SYMBOLS_MISSING;
    goto done;
  }
  symbol_status = nxgl_provider_symbol_from_object(
      handle, "eglTerminate", provider_path, &terminate_symbol);
  if (symbol_status != 1) {
    local.reason = symbol_status < 0
                       ? NXGL_PROVIDER_RECOVERY_V2_MIXED_OBJECTS
                       : NXGL_PROVIDER_RECOVERY_V2_EGL_SYMBOLS_MISSING;
    goto done;
  }
  local.exports_egl = 1;
  for (index = 0u; index < options->required_engine_gles_symbol_count;
       ++index) {
    symbol_status = nxgl_provider_symbol_from_object(
        handle, options->required_engine_gles_symbols[index], provider_path,
        NULL);
    if (symbol_status != 1) {
      local.reason = symbol_status < 0
                         ? NXGL_PROVIDER_RECOVERY_V2_MIXED_OBJECTS
                         : NXGL_PROVIDER_RECOVERY_V2_ENGINE_SYMBOLS_MISSING;
      goto done;
    }
  }
  local.exports_engine_gles = 1;

  if (options->mode == NXGL_PROVIDER_PROBE_V2_EGL_DEFAULT_DISPLAY) {
    nxgl_egl_get_display_fn get_display =
        (nxgl_egl_get_display_fn)get_display_symbol;
    nxgl_egl_initialize_fn initialize =
        (nxgl_egl_initialize_fn)initialize_symbol;
    nxgl_egl_terminate_fn terminate =
        (nxgl_egl_terminate_fn)terminate_symbol;
    void *display = get_display(NULL);
    int major = 0;
    int minor = 0;
    if (!display || !initialize(display, &major, &minor)) {
      local.reason = NXGL_PROVIDER_RECOVERY_V2_EGL_INITIALIZE_FAILED;
      goto done;
    }
    local.egl_initialized = 1;
    local.egl_major = major;
    local.egl_minor = minor;
    if (!terminate(display)) {
      local.egl_initialized = 0;
      local.reason = NXGL_PROVIDER_RECOVERY_V2_EGL_TERMINATE_FAILED;
      /* The provider may still own a live display. Unloading its code is less
       * safe than retaining this one process-local handle. This receipt stays
       * unusable, so re-exec is impossible; the adapter should abort startup. */
      retain_handle = 1;
      nxgl_provider_process_poisoned = 1;
      goto done;
    }
  }

  local.usable = 1;
  local.reason = NXGL_PROVIDER_RECOVERY_V2_NONE;
  nxgl_provider_receipt_set_identity(&local, &identity);
  (void)snprintf(local.provider_path, sizeof(local.provider_path), "%s",
                 provider_path);
  result = NXGL_SUCCESS;

done:
  if (handle && !retain_handle)
    (void)dlclose(handle);
  nxgl_arbiter_release();
  *receipt = local;
  return result;
}

static int nxgl_provider_receipt_valid(
    const nxgl_sdl_provider_probe_receipt_v2 *receipt) {
  return receipt && receipt->api_version == NXGL_API_VERSION_V2 &&
         receipt->struct_size >= sizeof(*receipt) && receipt->usable == 1 &&
         receipt->exports_egl == 1 && receipt->exports_engine_gles == 1 &&
         (receipt->mode == NXGL_PROVIDER_PROBE_V2_SYMBOLS_ONLY ||
          (receipt->mode == NXGL_PROVIDER_PROBE_V2_EGL_DEFAULT_DISPLAY &&
           receipt->egl_initialized == 1)) &&
         receipt->reason == NXGL_PROVIDER_RECOVERY_V2_NONE &&
         receipt->provider_path[0] == '/' &&
         memchr(receipt->provider_path, '\0',
                sizeof(receipt->provider_path)) != NULL;
}

static int nxgl_provider_argv_valid(char *const *argv) {
  size_t index;

  if (!argv || !argv[0] || !argv[0][0])
    return 0;
  for (index = 0u; index < NXGL_PROVIDER_RECOVERY_ARG_MAX; ++index) {
    if (!argv[index])
      return 1;
  }
  return 0;
}

static int nxgl_provider_reexec_options_valid(
    const nxgl_sdl_provider_reexec_options_v2 *options) {
  if (!options || options->api_version != NXGL_API_VERSION_V2 ||
      options->struct_size < sizeof(*options) ||
      !nxgl_provider_boolean_valid(options->enabled))
    return 0;
  if (!options->enabled)
    return 1;
  if ((options->authorization != NXGL_SDL_PROVIDER_PAIR_NO_ACTION &&
       options->authorization != NXGL_SDL_PROVIDER_PAIR_BIND_COHERENT) ||
      !nxgl_provider_boolean_valid(options->video_torn_down) ||
      !nxgl_provider_receipt_valid(options->provider) ||
      !nxgl_provider_argv_valid(options->argv))
    return 0;
  return 1;
}

static int nxgl_provider_receipt_still_matches(
    const nxgl_sdl_provider_probe_receipt_v2 *receipt) {
  nxgl_provider_file_identity expected;
  nxgl_provider_file_identity actual;
  char canonical[NXGL_PROVIDER_RECOVERY_PATH_MAX];

  if (!realpath(receipt->provider_path, canonical) ||
      strcmp(canonical, receipt->provider_path) != 0 ||
      !nxgl_provider_file_identity_read(canonical, &actual))
    return 0;
  nxgl_provider_receipt_get_identity(receipt, &expected);
  return nxgl_provider_file_identity_equal(&expected, &actual);
}

int nxgl_reexec_sdl_provider_pair_v2(
    const nxgl_sdl_provider_reexec_options_v2 *options,
    nxgl_sdl_provider_reexec_result_v2 *result) {
  nxgl_sdl_provider_reexec_result_v2 local;
  int saved_error;
  int restored;

  if (!result || !nxgl_provider_reexec_options_valid(options))
    return NXGL_ERROR_INVALID_ARGUMENT;
  nxgl_sdl_provider_reexec_result_v2_init(&local);
  if (!options->enabled) {
    local.reason = NXGL_PROVIDER_RECOVERY_V2_DISABLED;
    *result = local;
    return NXGL_NO_ACTION;
  }
  if (options->authorization != NXGL_SDL_PROVIDER_PAIR_BIND_COHERENT) {
    local.reason = NXGL_PROVIDER_RECOVERY_V2_NOT_AUTHORIZED;
    *result = local;
    return NXGL_NO_ACTION;
  }
  if (!options->video_torn_down) {
    local.reason = NXGL_PROVIDER_RECOVERY_V2_VIDEO_NOT_TORN_DOWN;
    *result = local;
    return NXGL_NO_ACTION;
  }
  if (!nxgl_arbiter_try_acquire())
    return NXGL_ERROR_BUSY;

  if (nxgl_provider_process_poisoned) {
    local.reason = NXGL_PROVIDER_RECOVERY_V2_EGL_TERMINATE_FAILED;
    nxgl_arbiter_release();
    *result = local;
    return NXGL_NO_ACTION;
  }
  if (getenv(NXGL_PROVIDER_RECOVERY_MARKER) != NULL) {
    local.reason = NXGL_PROVIDER_RECOVERY_V2_ALREADY_APPLIED;
    nxgl_arbiter_release();
    *result = local;
    return NXGL_NO_ACTION;
  }
  if (getenv("SDL_VIDEO_EGL_DRIVER") != NULL ||
      getenv("SDL_VIDEO_GL_DRIVER") != NULL) {
    local.reason = NXGL_PROVIDER_RECOVERY_V2_INHERITED_PROVIDER_OVERRIDE;
    nxgl_arbiter_release();
    *result = local;
    return NXGL_NO_ACTION;
  }
  if (!nxgl_provider_receipt_still_matches(options->provider)) {
    local.reason = NXGL_PROVIDER_RECOVERY_V2_PROVIDER_CHANGED;
    nxgl_arbiter_release();
    *result = local;
    return NXGL_NO_ACTION;
  }

  local.environment_restored = 0;
  if (nxgl_provider_setenv("SDL_VIDEO_EGL_DRIVER",
                           options->provider->provider_path, 1) !=
          0 ||
      nxgl_provider_setenv("SDL_VIDEO_GL_DRIVER",
                           options->provider->provider_path, 1) !=
          0 ||
      nxgl_provider_setenv(NXGL_PROVIDER_RECOVERY_MARKER, "1", 1) != 0) {
    saved_error = errno;
    restored = nxgl_provider_unsetenv("SDL_VIDEO_EGL_DRIVER") == 0;
    restored = nxgl_provider_unsetenv("SDL_VIDEO_GL_DRIVER") == 0 && restored;
    restored = nxgl_provider_unsetenv(NXGL_PROVIDER_RECOVERY_MARKER) == 0 &&
               restored;
    local.reason = NXGL_PROVIDER_RECOVERY_V2_ENVIRONMENT_FAILED;
    local.environment_restored = restored;
    local.system_error = saved_error;
    nxgl_arbiter_release();
    *result = local;
    return restored ? NXGL_ERROR_OUT_OF_MEMORY : NXGL_ERROR_ROLLBACK;
  }

  (void)nxgl_provider_execv("/proc/self/exe", options->argv);
  saved_error = errno;
  restored = nxgl_provider_unsetenv("SDL_VIDEO_EGL_DRIVER") == 0;
  restored = nxgl_provider_unsetenv("SDL_VIDEO_GL_DRIVER") == 0 && restored;
  restored = nxgl_provider_unsetenv(NXGL_PROVIDER_RECOVERY_MARKER) == 0 &&
             restored;
  local.reason = NXGL_PROVIDER_RECOVERY_V2_EXEC_FAILED;
  local.environment_restored = restored;
  local.system_error = saved_error;
  nxgl_arbiter_release();
  *result = local;
  return restored ? NXGL_ERROR_VIDEO_UNAVAILABLE : NXGL_ERROR_ROLLBACK;
}

const char *nxgl_provider_recovery_reason_name_v2(
    nxgl_provider_recovery_reason_v2 reason) {
  switch (reason) {
  case NXGL_PROVIDER_RECOVERY_V2_NONE:
    return "none";
  case NXGL_PROVIDER_RECOVERY_V2_DISABLED:
    return "disabled";
  case NXGL_PROVIDER_RECOVERY_V2_CANDIDATE_UNAVAILABLE:
    return "candidate-unavailable";
  case NXGL_PROVIDER_RECOVERY_V2_PROVIDER_ALREADY_LOADED:
    return "provider-already-loaded";
  case NXGL_PROVIDER_RECOVERY_V2_TRANSPORT_MISMATCH:
    return "transport-mismatch";
  case NXGL_PROVIDER_RECOVERY_V2_EGL_SYMBOLS_MISSING:
    return "egl-symbols-missing";
  case NXGL_PROVIDER_RECOVERY_V2_ENGINE_SYMBOLS_MISSING:
    return "engine-symbols-missing";
  case NXGL_PROVIDER_RECOVERY_V2_MIXED_OBJECTS:
    return "mixed-objects";
  case NXGL_PROVIDER_RECOVERY_V2_EGL_INITIALIZE_FAILED:
    return "egl-initialize-failed";
  case NXGL_PROVIDER_RECOVERY_V2_EGL_TERMINATE_FAILED:
    return "egl-terminate-failed";
  case NXGL_PROVIDER_RECOVERY_V2_NOT_AUTHORIZED:
    return "not-authorized";
  case NXGL_PROVIDER_RECOVERY_V2_VIDEO_NOT_TORN_DOWN:
    return "video-not-torn-down";
  case NXGL_PROVIDER_RECOVERY_V2_ALREADY_APPLIED:
    return "already-applied";
  case NXGL_PROVIDER_RECOVERY_V2_INHERITED_PROVIDER_OVERRIDE:
    return "inherited-provider-override";
  case NXGL_PROVIDER_RECOVERY_V2_PROVIDER_CHANGED:
    return "provider-changed";
  case NXGL_PROVIDER_RECOVERY_V2_ENVIRONMENT_FAILED:
    return "environment-failed";
  case NXGL_PROVIDER_RECOVERY_V2_EXEC_FAILED:
    return "exec-failed";
  default:
    return "unknown";
  }
}
