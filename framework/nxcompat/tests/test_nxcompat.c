/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxcompat_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
      return -1;                                                               \
    }                                                                          \
  } while (0)

static int environment_equals(const char *name, const char *expected) {
  const char *value = getenv(name);
  return value && strcmp(value, expected) == 0;
}

static int environment_starts_with(const char *name, const char *prefix) {
  const char *value = getenv(name);
  return value && strncmp(value, prefix, strlen(prefix)) == 0;
}

static int environment_is_empty(const char *name) {
  const char *value = getenv(name);
  return value && !*value;
}

static int host_path(char *output, size_t output_size, const char *root,
                     const char *device_path) {
  int count = snprintf(output, output_size, "%s%s", root, device_path);
  return count >= 0 && (size_t)count < output_size ? 0 : -1;
}

static int make_directories(const char *path) {
  char copy[1024];
  char *cursor;
  if (snprintf(copy, sizeof(copy), "%s", path) >= (int)sizeof(copy))
    return -1;
  for (cursor = copy + 1; *cursor; ++cursor) {
    if (*cursor != '/')
      continue;
    *cursor = '\0';
    if (mkdir(copy, 0755) != 0 && errno != EEXIST)
      return -1;
    *cursor = '/';
  }
  return mkdir(copy, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int make_parent(const char *path) {
  char parent[1024];
  char *slash;
  if (snprintf(parent, sizeof(parent), "%s", path) >= (int)sizeof(parent))
    return -1;
  slash = strrchr(parent, '/');
  if (!slash)
    return -1;
  *slash = '\0';
  return make_directories(parent);
}

static int write_text(const char *path, const char *text) {
  FILE *stream;
  if (make_parent(path) != 0)
    return -1;
  stream = fopen(path, "w");
  if (!stream)
    return -1;
  if (fputs(text, stream) == EOF || fclose(stream) != 0)
    return -1;
  return 0;
}

static int make_socket_node(const char *path) {
  struct sockaddr_un address;
  int descriptor;
  if (make_parent(path) != 0 || strlen(path) >= sizeof(address.sun_path))
    return -1;
  descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
  if (descriptor < 0)
    return -1;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path, path, strlen(path) + 1U);
  (void)unlink(path);
  if (bind(descriptor, (struct sockaddr *)&address, sizeof(address)) != 0) {
    (void)close(descriptor);
    return -1;
  }
  return close(descriptor);
}

static int make_listening_socket_node(const char *path) {
  struct sockaddr_un address;
  int descriptor;
  if (make_parent(path) != 0 || strlen(path) >= sizeof(address.sun_path))
    return -1;
  descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
  if (descriptor < 0)
    return -1;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path, path, strlen(path) + 1U);
  (void)unlink(path);
  if (bind(descriptor, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(descriptor, 4) != 0) {
    (void)close(descriptor);
    return -1;
  }
  return descriptor;
}

static int write_armhf_shared_object(const char *path) {
  unsigned char header[52];
  FILE *stream;
  memset(header, 0, sizeof(header));
  header[0] = 0x7f;
  header[1] = 'E';
  header[2] = 'L';
  header[3] = 'F';
  header[4] = 1; /* ELFCLASS32 */
  header[5] = 1; /* little endian */
  header[6] = 1; /* ELF version */
  header[16] = 3; /* ET_DYN */
  header[18] = 40; /* EM_ARM */
  header[20] = 1;
  header[37] = 0x04; /* EF_ARM_ABI_FLOAT_HARD */
  if (make_parent(path) != 0)
    return -1;
  stream = fopen(path, "wb");
  if (!stream)
    return -1;
  if (fwrite(header, 1, sizeof(header), stream) != sizeof(header) ||
      fclose(stream) != 0)
    return -1;
  return 0;
}

static int write_aarch64_shared_object(const char *path) {
  unsigned char header[64];
  FILE *stream;
  memset(header, 0, sizeof(header));
  header[0] = 0x7f;
  header[1] = 'E';
  header[2] = 'L';
  header[3] = 'F';
  header[4] = 2; /* ELFCLASS64 */
  header[5] = 1; /* little endian */
  header[6] = 1; /* ELF version */
  header[16] = 3; /* ET_DYN */
  header[18] = 183; /* EM_AARCH64 */
  header[20] = 1;
  if (make_parent(path) != 0)
    return -1;
  stream = fopen(path, "wb");
  if (!stream)
    return -1;
  if (fwrite(header, 1, sizeof(header), stream) != sizeof(header) ||
      fclose(stream) != 0)
    return -1;
  return 0;
}

static int remove_tree(const char *path) {
  struct stat status;
  if (lstat(path, &status) != 0)
    return errno == ENOENT ? 0 : -1;
  if (S_ISDIR(status.st_mode)) {
    DIR *directory = opendir(path);
    struct dirent *entry;
    if (!directory)
      return -1;
    while ((entry = readdir(directory)) != NULL) {
      char child[1200];
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        continue;
      if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >=
          (int)sizeof(child) ||
          remove_tree(child) != 0) {
        (void)closedir(directory);
        return -1;
      }
    }
    (void)closedir(directory);
    return rmdir(path);
  }
  return unlink(path);
}

static int prepare_fixture(const char *root, char *runtime_dir,
                           size_t runtime_dir_size, int *pulse_listener,
                           int *pipewire_listener) {
  char path[1200];
  unsigned uid = (unsigned)getuid();
  CHECK(host_path(path, sizeof(path), root, "/etc/os-release") == 0);
  CHECK(write_text(path, "ID=fixtureos\nVERSION_ID=42\n") == 0);
  CHECK(host_path(path, sizeof(path), root, "/proc/device-tree/model") == 0);
  CHECK(write_text(path, "Synthetic Handheld\n") == 0);
  CHECK(host_path(path, sizeof(path), root, "/proc/meminfo") == 0);
  CHECK(write_text(path,
                   "MemTotal: 786432 kB\n"
                   "MemAvailable: 524288 kB\n"
                   "SwapTotal: 0 kB\n") == 0);
  CHECK(host_path(path, sizeof(path), root, "/proc/self/cgroup") == 0);
  CHECK(write_text(path, "") == 0);
  CHECK(host_path(path, sizeof(path), root, "/proc/mounts") == 0);
  CHECK(write_text(path,
                   "/dev/root / ext4 rw 0 0\n"
                   "/dev/mmc /userdata exfat rw 0 0\n") == 0);
  CHECK(host_path(path, sizeof(path), root,
                  "/sys/class/drm/card0-DSI-1/status") == 0);
  CHECK(write_text(path, "connected\n") == 0);
  CHECK(host_path(path, sizeof(path), root,
                  "/sys/class/drm/card0-DSI-1/modes") == 0);
  CHECK(write_text(path, "640x480\n") == 0);
  CHECK(host_path(path, sizeof(path), root, "/dev/dri/.keep") == 0);
  CHECK(write_text(path, "") == 0);
  CHECK(host_path(path, sizeof(path), root, "/dev/fb0") == 0);
  CHECK(write_text(path, "") == 0);
  CHECK(host_path(path, sizeof(path), root, "/proc/asound/cards") == 0);
  CHECK(write_text(path, " 0 [Codec]\n") == 0);
  CHECK(host_path(path, sizeof(path), root,
                  "/opt/tools/PortMaster/control.txt") == 0);
  CHECK(write_text(path, "# fixture\n") == 0);
  CHECK(host_path(path, sizeof(path), root,
                  "/opt/tools/PortMaster/gamecontrollerdb.txt") == 0);
  CHECK(write_text(path, "fixture mapping\n") == 0);
  CHECK(host_path(path, sizeof(path), root,
                  "/userdata/roms/ports/PortMaster/control.txt") == 0);
  CHECK(write_text(path, "# sibling fixture\n") == 0);
  CHECK(host_path(path, sizeof(path), root,
                  "/userdata/roms/ports/test/.keep") == 0);
  CHECK(write_text(path, "") == 0);
  CHECK(host_path(path, sizeof(path), root,
                  "/usr/lib/arm-linux-gnueabihf/pipewire-0.3/.keep") == 0);
  CHECK(write_text(path, "") == 0);
  CHECK(host_path(path, sizeof(path), root,
                  "/usr/lib/arm-linux-gnueabihf/pipewire-0.3/"
                  "libpipewire-module-protocol-native.so") == 0);
  CHECK(write_armhf_shared_object(path) == 0);
  CHECK(host_path(path, sizeof(path), root,
                  "/usr/lib/arm-linux-gnueabihf/spa-0.2/.keep") == 0);
  CHECK(write_text(path, "") == 0);
  CHECK(host_path(path, sizeof(path), root,
                  "/usr/lib/arm-linux-gnueabihf/spa-0.2/support/"
                  "libspa-support.so") == 0);
  CHECK(write_armhf_shared_object(path) == 0);
  CHECK(host_path(path, sizeof(path), root,
                  "/usr/lib/arm-linux-gnueabihf/alsa-lib/"
                  "libasound_module_pcm_pipewire.so") == 0);
  CHECK(write_armhf_shared_object(path) == 0);
  CHECK(host_path(path, sizeof(path), root, "/usr/lib/libc.so.6") == 0);
  CHECK(write_aarch64_shared_object(path) == 0);

  CHECK(snprintf(runtime_dir, runtime_dir_size, "/run/user/%u", uid) <
        (int)runtime_dir_size);
  CHECK(snprintf(path, sizeof(path), "%s%s/wayland-0", root, runtime_dir) <
        (int)sizeof(path));
  CHECK(make_socket_node(path) == 0);
  CHECK(snprintf(path, sizeof(path), "%s%s/pulse/native", root, runtime_dir) <
        (int)sizeof(path));
  *pulse_listener = make_listening_socket_node(path);
  CHECK(*pulse_listener >= 0);
  CHECK(snprintf(path, sizeof(path), "%s%s/pipewire-0", root, runtime_dir) <
        (int)sizeof(path));
  *pipewire_listener = make_listening_socket_node(path);
  CHECK(*pipewire_listener >= 0);
  return 0;
}

static const nxcompat_action *find_action(const nxcompat_plan *plan,
                                          nxcompat_action_id id) {
  size_t index;
  for (index = 0; index < plan->action_count; ++index)
    if (plan->actions[index].id == id)
      return &plan->actions[index];
  return NULL;
}

#if defined(NXCOMPAT_CORE_TESTING)
static unsigned environment_set_calls;
static unsigned environment_unset_calls;
static unsigned environment_fail_set_at;
static unsigned environment_fail_unset_at;
static unsigned environment_partial_set_at;

int nxcompat_test_environment_set(const char *name, const char *value,
                                  int overwrite) {
  ++environment_set_calls;
  if (environment_fail_set_at &&
      environment_set_calls == environment_fail_set_at) {
    if (environment_partial_set_at == environment_set_calls)
      (void)setenv(name, value, overwrite);
    errno = EIO;
    return -1;
  }
  return setenv(name, value, overwrite);
}

int nxcompat_test_environment_unset(const char *name) {
  ++environment_unset_calls;
  if (environment_fail_unset_at &&
      environment_unset_calls == environment_fail_unset_at) {
    errno = EIO;
    return -1;
  }
  return unsetenv(name);
}

static void reset_environment_faults(void) {
  environment_set_calls = 0;
  environment_unset_calls = 0;
  environment_fail_set_at = 0;
  environment_fail_unset_at = 0;
  environment_partial_set_at = 0;
}
#else
static void reset_environment_faults(void) {}
#endif

typedef struct fake_backend_v2 {
  nxcompat_backend_attempt_outcome outcomes[3];
  nxcompat_reason_code reasons[3];
  const char *names[3];
  const char *errors[3];
  nxcompat_result_code cleanup_results[3];
  unsigned calls;
  unsigned cleanups;
  unsigned status_calls;
  unsigned legacy_resets;
  unsigned legacy_discover_calls;
  int first_record_visible_in_cleanup;
  int arbiter_busy_in_attempt;
  int status_ran_after_unlock;
  unsigned mutate_on_call;
  const char *mutate_name;
  const char *mutate_value;
  nxcompat_backend_result_v2 *result_under_test;
  char seen_video[3][128];
  char status_message[NXCOMPAT_DETAIL_MAX];
} fake_backend_v2;

static nxcompat_backend_attempt_outcome fake_backend_attempt_v2(
    void *userdata, nxcompat_backend_attempt_report *report) {
  fake_backend_v2 *backend = (fake_backend_v2 *)userdata;
  unsigned index = backend->calls < 3 ? backend->calls : 2;
  const char *environment = getenv("NXCOMPAT_TEST_VIDEO");
  nxcompat_result_code lock_status;
  ++backend->calls;
  (void)snprintf(backend->seen_video[index],
                 sizeof(backend->seen_video[index]), "%s",
                 environment ? environment : "<absent>");
  if (backend->mutate_on_call == backend->calls && backend->mutate_name)
    (void)setenv(backend->mutate_name,
                 backend->mutate_value ? backend->mutate_value : "mutated", 1);
  lock_status = nxcompat_global_arbiter_try_acquire();
  if (lock_status == NXCOMPAT_BUSY)
    backend->arbiter_busy_in_attempt = 1;
  else if (lock_status == NXCOMPAT_OK)
    nxcompat_global_arbiter_release();
  report->api_version = NXCOMPAT_API_VERSION_V2;
  report->struct_size = sizeof(*report);
  report->reason = backend->reasons[index];
  if (backend->names[index])
    (void)snprintf(report->selected, sizeof(report->selected), "%s",
                   backend->names[index]);
  if (backend->errors[index])
    (void)snprintf(report->error, sizeof(report->error), "%s",
                   backend->errors[index]);
  else if (backend->outcomes[index] != NXCOMPAT_BACKEND_ATTEMPT_OK)
    (void)snprintf(report->error, sizeof(report->error),
                   "synthetic attempt %u failed", backend->calls);
  return backend->outcomes[index];
}

static nxcompat_result_code fake_backend_cleanup_v2(void *userdata, char *error,
                                                     size_t error_size) {
  fake_backend_v2 *backend = (fake_backend_v2 *)userdata;
  unsigned index = backend->cleanups < 3 ? backend->cleanups : 2;
  ++backend->cleanups;
  if (backend->cleanups == 1 && backend->result_under_test &&
      backend->result_under_test->first_reason != NXCOMPAT_REASON_NONE &&
      getenv("NXCOMPAT_TEST_VIDEO"))
    backend->first_record_visible_in_cleanup = 1;
  if (error_size)
    error[0] = '\0';
  if (backend->cleanup_results[index] != NXCOMPAT_OK && error_size)
    (void)snprintf(error, error_size, "synthetic cleanup failed");
  return backend->cleanup_results[index];
}

static int fake_backend_accept(void *userdata, const char *name) {
  (void)userdata;
  return name && *name && strcmp(name, "dummy") != 0 &&
         strcmp(name, "disk") != 0;
}

static void fake_status_v2(void *userdata, nxcompat_status_kind kind,
                           const char *message) {
  fake_backend_v2 *backend = (fake_backend_v2 *)userdata;
  nxcompat_result_code status;
  (void)kind;
  ++backend->status_calls;
  status = nxcompat_global_arbiter_try_acquire();
  if (status == NXCOMPAT_OK) {
    backend->status_ran_after_unlock = 1;
    nxcompat_global_arbiter_release();
  }
  (void)snprintf(backend->status_message, sizeof(backend->status_message),
                 "%s", message);
}

static void backend_options_v2_init(nxcompat_backend_options_v2 *options,
                                    fake_backend_v2 *backend,
                                    nxcompat_backend_kind kind) {
  memset(options, 0, sizeof(*options));
  options->api_version = NXCOMPAT_API_VERSION_V2;
  options->struct_size = sizeof(*options);
  options->kind = kind;
  options->attempt = fake_backend_attempt_v2;
  options->cleanup = fake_backend_cleanup_v2;
  options->accept_name = fake_backend_accept;
  options->status = fake_status_v2;
  options->userdata = backend;
  options->status_userdata = backend;
}

static int fake_legacy_attempt(void *userdata, char *error, size_t error_size) {
  fake_backend_v2 *backend = (fake_backend_v2 *)userdata;
  unsigned index = backend->calls < 3 ? backend->calls : 2;
  ++backend->calls;
  if (backend->outcomes[index] != NXCOMPAT_BACKEND_ATTEMPT_OK) {
    (void)snprintf(error, error_size, "legacy synthetic failure");
    return -1;
  }
  return 0;
}

static void fake_legacy_reset(void *userdata) {
  ++((fake_backend_v2 *)userdata)->legacy_resets;
}

static const char *fake_legacy_name(void *userdata) {
  fake_backend_v2 *backend = (fake_backend_v2 *)userdata;
  unsigned index = backend->calls ? backend->calls - 1u : 0u;
  return backend->names[index < 3 ? index : 2];
}

static int fake_legacy_discover(void *userdata, unsigned *attempt_count,
                                char *error, size_t error_size) {
  fake_backend_v2 *backend = (fake_backend_v2 *)userdata;
  (void)error;
  (void)error_size;
  ++backend->legacy_discover_calls;
  *attempt_count = 99;
  return 0;
}

static int test_backend_negotiation(void) {
  nxcompat_backend_options_v2 options;
  nxcompat_backend_result_v2 result;
  fake_backend_v2 backend;
  char long_hint[900];

  reset_environment_faults();
  (void)unsetenv("NXCOMPAT_TEST_VIDEO");
  (void)unsetenv("NXCOMPAT_TEST_VIDEO_ALIAS");

  memset(&backend, 0, sizeof(backend));
  backend.outcomes[0] = NXCOMPAT_BACKEND_ATTEMPT_OK;
  backend.names[0] = "must-not-run";
  backend_options_v2_init(&options, &backend, NXCOMPAT_BACKEND_VIDEO);
  options.cleanup = NULL;
  CHECK(nxcompat_negotiate_backend_v2(&options, &result) == NXCOMPAT_INVALID);
  CHECK(backend.calls == 0 && backend.cleanups == 0);

  memset(&backend, 0, sizeof(backend));
  backend.outcomes[0] = NXCOMPAT_BACKEND_ATTEMPT_OK;
  backend.names[0] = "automatic-real";
  backend_options_v2_init(&options, &backend, NXCOMPAT_BACKEND_VIDEO);
  options.environment_names[0] = "NXCOMPAT_TEST_VIDEO";
  CHECK(nxcompat_negotiate_backend_v2(&options, &result) == NXCOMPAT_OK);
  CHECK(result.state == NXCOMPAT_BACKEND_V2_AUTODETECT_OK);
  CHECK(result.attempt_count == 1 && result.cleanup_count == 1);
  CHECK(result.env_restored && backend.calls == 1 && backend.cleanups == 1);

  memset(&backend, 0, sizeof(backend));
  backend.outcomes[0] = NXCOMPAT_BACKEND_ATTEMPT_OK;
  backend.names[0] = "inherited-real";
  backend_options_v2_init(&options, &backend, NXCOMPAT_BACKEND_VIDEO);
  options.environment_names[0] = "NXCOMPAT_TEST_VIDEO";
  CHECK(setenv("NXCOMPAT_TEST_VIDEO", "inherited-real", 1) == 0);
  CHECK(nxcompat_negotiate_backend_v2(&options, &result) == NXCOMPAT_OK);
  CHECK(result.state == NXCOMPAT_BACKEND_V2_INHERITED_OK);
  CHECK(result.attempt_count == 1 && result.cleanup_count == 1);
  CHECK(result.env_restored && !result.hints_removed);
  CHECK(environment_equals("NXCOMPAT_TEST_VIDEO", "inherited-real"));
  CHECK(unsetenv("NXCOMPAT_TEST_VIDEO") == 0);

  memset(&backend, 0, sizeof(backend));
  backend.outcomes[0] = NXCOMPAT_BACKEND_ATTEMPT_OK;
  backend.names[0] = "must-not-run-while-busy";
  backend_options_v2_init(&options, &backend, NXCOMPAT_BACKEND_VIDEO);
  CHECK(nxcompat_global_arbiter_try_acquire() == NXCOMPAT_OK);
  CHECK(nxcompat_negotiate_backend_v2(&options, &result) == NXCOMPAT_BUSY);
  CHECK(result.state == NXCOMPAT_BACKEND_V2_BUSY);
  CHECK(result.final_reason == NXCOMPAT_REASON_ARBITER_BUSY);
  CHECK(backend.calls == 0 && backend.cleanups == 0);
  nxcompat_global_arbiter_release();

  memset(&backend, 0, sizeof(backend));
  backend.outcomes[0] = NXCOMPAT_BACKEND_ATTEMPT_RETRYABLE_FAILURE;
  backend.outcomes[1] = NXCOMPAT_BACKEND_ATTEMPT_OK;
  backend.names[1] = "firmware-auto";
  backend_options_v2_init(&options, &backend, NXCOMPAT_BACKEND_VIDEO);
  options.environment_names[0] = "NXCOMPAT_TEST_VIDEO";
  backend.result_under_test = &result;
  CHECK(setenv("NXCOMPAT_TEST_VIDEO", "invalid-inherited", 1) == 0);
  CHECK(nxcompat_negotiate_backend_v2(&options, &result) == NXCOMPAT_OK);
  CHECK(result.state == NXCOMPAT_BACKEND_V2_RECOVERED_BY_AUTODETECT);
  CHECK(result.attempt_count == 2 && result.cleanup_count == 2);
  CHECK(backend.calls == 2 && backend.cleanups == 2);
  CHECK(result.first_reason == NXCOMPAT_REASON_BACKEND_ATTEMPT_RETRYABLE);
  CHECK(result.final_reason == NXCOMPAT_REASON_BACKEND_AUTODETECT_OK);
  CHECK(result.hints_removed && !result.env_restored);
  CHECK(strcmp(result.selected, "firmware-auto") == 0);
  CHECK(strcmp(backend.seen_video[0], "invalid-inherited") == 0);
  CHECK(strcmp(backend.seen_video[1], "<absent>") == 0);
  CHECK(backend.first_record_visible_in_cleanup);
  CHECK(backend.arbiter_busy_in_attempt && backend.status_ran_after_unlock);
  CHECK(getenv("NXCOMPAT_TEST_VIDEO") == NULL);

  memset(long_hint, 'L', sizeof(long_hint) - 1u);
  long_hint[sizeof(long_hint) - 1u] = '\0';
  memset(&backend, 0, sizeof(backend));
  backend.outcomes[0] = NXCOMPAT_BACKEND_ATTEMPT_RETRYABLE_FAILURE;
  backend.outcomes[1] = NXCOMPAT_BACKEND_ATTEMPT_RETRYABLE_FAILURE;
  backend.outcomes[2] = NXCOMPAT_BACKEND_ATTEMPT_OK;
  backend.names[2] = "forbidden-third-success";
  backend_options_v2_init(&options, &backend, NXCOMPAT_BACKEND_VIDEO);
  options.environment_names[0] = "NXCOMPAT_TEST_VIDEO";
  CHECK(setenv("NXCOMPAT_TEST_VIDEO", long_hint, 1) == 0);
  CHECK(nxcompat_negotiate_backend_v2(&options, &result) == NXCOMPAT_FAILED);
  CHECK(backend.calls == 2 && backend.cleanups == 2);
  CHECK(result.attempt_count == 2 && result.cleanup_count == 2);
  CHECK(result.env_restored && !result.hints_removed);
  CHECK(getenv("NXCOMPAT_TEST_VIDEO") != NULL);
  CHECK(environment_equals("NXCOMPAT_TEST_VIDEO", long_hint));
  CHECK(unsetenv("NXCOMPAT_TEST_VIDEO") == 0);

  memset(&backend, 0, sizeof(backend));
  backend.outcomes[0] = NXCOMPAT_BACKEND_ATTEMPT_RETRYABLE_FAILURE;
  backend.outcomes[1] = NXCOMPAT_BACKEND_ATTEMPT_OK;
  backend.names[1] = "must-not-run";
  backend_options_v2_init(&options, &backend, NXCOMPAT_BACKEND_VIDEO);
  options.environment_names[0] = "NXCOMPAT_TEST_VIDEO";
  CHECK(nxcompat_negotiate_backend_v2(&options, &result) == NXCOMPAT_FAILED);
  CHECK(backend.calls == 1 && backend.cleanups == 1);
  CHECK(result.attempt_count == 1 && result.cleanup_count == 1);
  CHECK(result.first_reason == NXCOMPAT_REASON_BACKEND_ATTEMPT_RETRYABLE);
  CHECK(result.final_reason == result.first_reason);
  CHECK(strcmp(result.final_error, result.first_error) == 0);

  memset(&backend, 0, sizeof(backend));
  backend.outcomes[0] = NXCOMPAT_BACKEND_ATTEMPT_RETRYABLE_FAILURE;
  backend.cleanup_results[0] = NXCOMPAT_ROLLBACK_FAILED;
  backend_options_v2_init(&options, &backend, NXCOMPAT_BACKEND_VIDEO);
  options.environment_names[0] = "NXCOMPAT_TEST_VIDEO";
  CHECK(setenv("NXCOMPAT_TEST_VIDEO", "cleanup-must-preserve", 1) == 0);
  CHECK(nxcompat_negotiate_backend_v2(&options, &result) ==
        NXCOMPAT_ROLLBACK_FAILED);
  CHECK(backend.calls == 1 && backend.cleanups == 1);
  CHECK(result.state == NXCOMPAT_BACKEND_V2_ROLLBACK_FAILED);
  CHECK(environment_equals("NXCOMPAT_TEST_VIDEO", "cleanup-must-preserve"));
  CHECK(unsetenv("NXCOMPAT_TEST_VIDEO") == 0);

  memset(&backend, 0, sizeof(backend));
  backend.outcomes[0] = NXCOMPAT_BACKEND_ATTEMPT_OWNERSHIP_BUSY;
  backend.reasons[0] = NXCOMPAT_REASON_BACKEND_FOREIGN_OWNED;
  backend_options_v2_init(&options, &backend, NXCOMPAT_BACKEND_VIDEO);
  options.environment_names[0] = "NXCOMPAT_TEST_VIDEO";
  CHECK(setenv("NXCOMPAT_TEST_VIDEO", "foreign-owner", 1) == 0);
  CHECK(nxcompat_negotiate_backend_v2(&options, &result) == NXCOMPAT_BUSY);
  CHECK(result.state == NXCOMPAT_BACKEND_V2_BUSY);
  CHECK(backend.calls == 1 && backend.cleanups == 1);
  CHECK(environment_equals("NXCOMPAT_TEST_VIDEO", "foreign-owner"));
  CHECK(unsetenv("NXCOMPAT_TEST_VIDEO") == 0);

  memset(&backend, 0, sizeof(backend));
  backend.outcomes[0] = NXCOMPAT_BACKEND_ATTEMPT_OWNERSHIP_BUSY;
  backend_options_v2_init(&options, &backend, NXCOMPAT_BACKEND_VIDEO);
  options.environment_names[0] = "NXCOMPAT_TEST_VIDEO";
  CHECK(nxcompat_negotiate_backend_v2(&options, &result) == NXCOMPAT_BUSY);
  CHECK(result.first_reason == NXCOMPAT_REASON_BACKEND_ATTEMPT_BUSY);
  CHECK(result.final_reason == NXCOMPAT_REASON_BACKEND_ATTEMPT_BUSY);
  CHECK(strcmp(result.final_error, result.first_error) == 0);

  memset(&backend, 0, sizeof(backend));
  backend.outcomes[0] = NXCOMPAT_BACKEND_ATTEMPT_OK;
  backend.outcomes[1] = NXCOMPAT_BACKEND_ATTEMPT_OK;
  backend.names[0] = "dummy";
  backend.names[1] = "real-audio";
  backend_options_v2_init(&options, &backend, NXCOMPAT_BACKEND_AUDIO);
  options.environment_names[0] = "NXCOMPAT_TEST_VIDEO";
  CHECK(setenv("NXCOMPAT_TEST_VIDEO", "dummy", 1) == 0);
  CHECK(nxcompat_negotiate_backend_v2(&options, &result) == NXCOMPAT_OK);
  CHECK(result.state == NXCOMPAT_BACKEND_V2_RECOVERED_BY_AUTODETECT);
  CHECK(result.first_reason == NXCOMPAT_REASON_BACKEND_NAME_REJECTED);
  CHECK(strcmp(result.selected, "real-audio") == 0);
  CHECK(unsetenv("NXCOMPAT_TEST_VIDEO") == 0 ||
        getenv("NXCOMPAT_TEST_VIDEO") == NULL);

  memset(&backend, 0, sizeof(backend));
  backend.outcomes[0] = NXCOMPAT_BACKEND_ATTEMPT_RETRYABLE_FAILURE;
  backend.mutate_on_call = 1;
  backend.mutate_name = "NXCOMPAT_TEST_VIDEO";
  backend.mutate_value = "callback-leak";
  backend_options_v2_init(&options, &backend, NXCOMPAT_BACKEND_VIDEO);
  options.environment_names[0] = "NXCOMPAT_TEST_VIDEO";
  CHECK(nxcompat_negotiate_backend_v2(&options, &result) == NXCOMPAT_FAILED);
  CHECK(getenv("NXCOMPAT_TEST_VIDEO") == NULL);

#if defined(NXCOMPAT_CORE_TESTING)
  memset(&backend, 0, sizeof(backend));
  backend.outcomes[0] = NXCOMPAT_BACKEND_ATTEMPT_RETRYABLE_FAILURE;
  backend.outcomes[1] = NXCOMPAT_BACKEND_ATTEMPT_OK;
  backend.names[1] = "must-not-run-after-clear-error";
  backend_options_v2_init(&options, &backend, NXCOMPAT_BACKEND_VIDEO);
  options.environment_names[0] = "NXCOMPAT_TEST_VIDEO";
  options.environment_names[1] = "NXCOMPAT_TEST_VIDEO_ALIAS";
  CHECK(setenv("NXCOMPAT_TEST_VIDEO", "one", 1) == 0);
  CHECK(setenv("NXCOMPAT_TEST_VIDEO_ALIAS", "two", 1) == 0);
  reset_environment_faults();
  environment_fail_unset_at = 2;
  CHECK(nxcompat_negotiate_backend_v2(&options, &result) == NXCOMPAT_FAILED);
  CHECK(result.final_reason == NXCOMPAT_REASON_ENV_CLEAR_FAILED);
  CHECK(backend.calls == 1);
  CHECK(environment_equals("NXCOMPAT_TEST_VIDEO", "one"));
  CHECK(environment_equals("NXCOMPAT_TEST_VIDEO_ALIAS", "two"));
  reset_environment_faults();
  CHECK(unsetenv("NXCOMPAT_TEST_VIDEO") == 0);
  CHECK(unsetenv("NXCOMPAT_TEST_VIDEO_ALIAS") == 0);

  memset(&backend, 0, sizeof(backend));
  backend.outcomes[0] = NXCOMPAT_BACKEND_ATTEMPT_RETRYABLE_FAILURE;
  backend.outcomes[1] = NXCOMPAT_BACKEND_ATTEMPT_RETRYABLE_FAILURE;
  backend_options_v2_init(&options, &backend, NXCOMPAT_BACKEND_VIDEO);
  options.environment_names[0] = "NXCOMPAT_TEST_VIDEO";
  CHECK(setenv("NXCOMPAT_TEST_VIDEO", "restore-will-fail", 1) == 0);
  reset_environment_faults();
  environment_fail_set_at = 1;
  CHECK(nxcompat_negotiate_backend_v2(&options, &result) ==
        NXCOMPAT_ROLLBACK_FAILED);
  CHECK(result.final_reason == NXCOMPAT_REASON_ENV_RESTORE_FAILED);
  CHECK(!result.env_restored);
  reset_environment_faults();
  CHECK(setenv("NXCOMPAT_TEST_VIDEO", "restore-will-fail", 1) == 0);
  CHECK(unsetenv("NXCOMPAT_TEST_VIDEO") == 0);
#endif

  {
    nxcompat_backend_options legacy_options;
    nxcompat_backend_result legacy_result;
    memset(&backend, 0, sizeof(backend));
    backend.outcomes[0] = NXCOMPAT_BACKEND_ATTEMPT_RETRYABLE_FAILURE;
    backend.outcomes[1] = NXCOMPAT_BACKEND_ATTEMPT_RETRYABLE_FAILURE;
    memset(&legacy_options, 0, sizeof(legacy_options));
    legacy_options.api_version = NXCOMPAT_API_VERSION_V1;
    legacy_options.struct_size = sizeof(legacy_options);
    legacy_options.kind = NXCOMPAT_BACKEND_VIDEO;
    legacy_options.environment_names[0] = "NXCOMPAT_TEST_VIDEO";
    legacy_options.attempt = fake_legacy_attempt;
    legacy_options.reset = fake_legacy_reset;
    legacy_options.current_name = fake_legacy_name;
    legacy_options.discover = fake_legacy_discover;
    legacy_options.userdata = &backend;
    CHECK(setenv("NXCOMPAT_TEST_VIDEO", "legacy-hint", 1) == 0);
    CHECK(nxcompat_negotiate_backend(&legacy_options, &legacy_result) != 0);
    CHECK(backend.calls == 2 && backend.legacy_resets == 2);
    CHECK(backend.legacy_discover_calls == 0);
    CHECK(legacy_result.attempt_count == 2);
    CHECK(environment_equals("NXCOMPAT_TEST_VIDEO", "legacy-hint"));
    CHECK(unsetenv("NXCOMPAT_TEST_VIDEO") == 0);
  }
  reset_environment_faults();
  return 0;
}

static nxcompat_action_v2 *find_action_v2(nxcompat_plan_v2 *plan,
                                          nxcompat_action_id id) {
  size_t index;
  for (index = 0; index < plan->action_count; ++index)
    if (plan->actions[index].id == id)
      return &plan->actions[index];
  return NULL;
}

static int test_plan_v2(void) {
  nxcompat_host host;
  nxcompat_host renamed_host;
  nxcompat_host incapable_host;
  nxcompat_plan_options options;
  nxcompat_plan_v2 plan;
  nxcompat_plan_v2 renamed_plan;
  nxcompat_plan_v2 incapable_plan;
  nxcompat_action_v2 *action;
  size_t index;

  memset(&host, 0, sizeof(host));
  host.api_version = NXCOMPAT_API_VERSION_V2;
  host.struct_size = sizeof(host);
  host.process_arch = NXCOMPAT_ARCH_AARCH64;
  host.kernel_arch = NXCOMPAT_ARCH_AARCH64;
  host.memory_class = NXCOMPAT_MEMORY_MEDIUM;
  host.filesystem_class = NXCOMPAT_FILESYSTEM_POSIX;
  host.capabilities = NXCOMPAT_CAP_PULSE_SOCKET;
  (void)snprintf(host.pulse_socket, sizeof(host.pulse_socket),
                 "/run/test/pulse/native");
  (void)snprintf(host.session_runtime_dir, sizeof(host.session_runtime_dir),
                 "/run/test");
  (void)snprintf(host.device_model, sizeof(host.device_model), "RG40XX-H");
  (void)snprintf(host.os_id, sizeof(host.os_id), "muos");
  (void)snprintf(host.os_version, sizeof(host.os_version), "synthetic");

  memset(&options, 0, sizeof(options));
  options.api_version = NXCOMPAT_API_VERSION_V2;
  options.struct_size = sizeof(options);
  options.runtime_arch = NXCOMPAT_ARCH_UNKNOWN;
  options.policy_flags = NXCOMPAT_POLICY_PULSE_SERVER;

  CHECK(unsetenv("PULSE_SERVER") == 0);
  CHECK(nxcompat_plan_environment_v2(&host, &options, &plan) == NXCOMPAT_OK);
  CHECK(plan.final_reason == NXCOMPAT_REASON_PLAN_COMPLETE);
  CHECK(plan.env_restored && plan.action_count == 1);
  action = find_action_v2(&plan, NXCOMPAT_ACTION_PULSE_SERVER);
  CHECK(action && action->state == NXCOMPAT_ACTION_V2_PLANNED);
  CHECK(action->reason_code == NXCOMPAT_REASON_PLAN_CAPABILITY_MATCHED);
  CHECK(strcmp(action->variable, "PULSE_SERVER") == 0);

  renamed_host = host;
  (void)snprintf(renamed_host.device_model, sizeof(renamed_host.device_model),
                 "unrelated-device-name");
  (void)snprintf(renamed_host.os_id, sizeof(renamed_host.os_id), "rocknix");
  (void)snprintf(renamed_host.os_version, sizeof(renamed_host.os_version),
                 "different-cfw");
  CHECK(nxcompat_plan_environment_v2(&renamed_host, &options, &renamed_plan) ==
        NXCOMPAT_OK);
  CHECK(plan.action_count == renamed_plan.action_count);
  CHECK(memcmp(plan.actions, renamed_plan.actions,
               plan.action_count * sizeof(plan.actions[0])) == 0);

  incapable_host = host;
  incapable_host.capabilities = 0;
  CHECK(nxcompat_plan_environment_v2(&incapable_host, &options,
                                     &incapable_plan) == NXCOMPAT_OK);
  action = find_action_v2(&incapable_plan, NXCOMPAT_ACTION_PULSE_SERVER);
  CHECK(action && action->state == NXCOMPAT_ACTION_V2_UNAVAILABLE);
  CHECK(action->reason_code == NXCOMPAT_REASON_PLAN_CAPABILITY_UNAVAILABLE);

  CHECK(setenv("PULSE_SERVER", "", 1) == 0);
  CHECK(nxcompat_plan_environment_v2(&host, &options, &plan) == NXCOMPAT_OK);
  action = find_action_v2(&plan, NXCOMPAT_ACTION_PULSE_SERVER);
  CHECK(action && action->state == NXCOMPAT_ACTION_V2_NOT_NEEDED);
  CHECK(action->reason_code == NXCOMPAT_REASON_PLAN_INHERITED_PRESERVED);
  CHECK(environment_is_empty("PULSE_SERVER"));
  CHECK(unsetenv("PULSE_SERVER") == 0);

  CHECK(nxcompat_plan_environment_v2(&host, &options, &plan) == NXCOMPAT_OK);
  CHECK(setenv("PULSE_SERVER", "", 1) == 0);
  CHECK(nxcompat_apply_environment_v2(&plan) == NXCOMPAT_OK);
  action = find_action_v2(&plan, NXCOMPAT_ACTION_PULSE_SERVER);
  CHECK(action && action->state == NXCOMPAT_ACTION_V2_NOT_NEEDED);
  CHECK(action->reason_code == NXCOMPAT_REASON_ENV_LATE_VALUE_PRESERVED);
  CHECK(plan.apply_count == 0 && plan.env_restored);
  CHECK(unsetenv("PULSE_SERVER") == 0);

  CHECK(nxcompat_plan_environment_v2(&host, &options, &plan) == NXCOMPAT_OK);
  CHECK(nxcompat_apply_environment_v2(&plan) == NXCOMPAT_OK);
  action = find_action_v2(&plan, NXCOMPAT_ACTION_PULSE_SERVER);
  CHECK(action && action->state == NXCOMPAT_ACTION_V2_APPLIED);
  CHECK(action->reason_code == NXCOMPAT_REASON_ENV_APPLIED);
  CHECK(plan.apply_count == 1 && !plan.env_restored);
  CHECK(environment_equals("PULSE_SERVER", "unix:/run/test/pulse/native"));
  CHECK(nxcompat_apply_environment_v2(&plan) == NXCOMPAT_INVALID);
  CHECK(environment_equals("PULSE_SERVER", "unix:/run/test/pulse/native"));
  CHECK(unsetenv("PULSE_SERVER") == 0);

  CHECK(nxcompat_global_arbiter_try_acquire() == NXCOMPAT_OK);
  CHECK(nxcompat_plan_environment_v2(&host, &options, &plan) == NXCOMPAT_BUSY);
  CHECK(plan.final_reason == NXCOMPAT_REASON_ARBITER_BUSY);
  nxcompat_global_arbiter_release();

  CHECK(nxcompat_plan_environment_v2(&host, &options, &plan) == NXCOMPAT_OK);
  CHECK(nxcompat_global_arbiter_try_acquire() == NXCOMPAT_OK);
  CHECK(nxcompat_apply_environment_v2(&plan) == NXCOMPAT_BUSY);
  CHECK(plan.final_reason == NXCOMPAT_REASON_ARBITER_BUSY);
  CHECK(getenv("PULSE_SERVER") == NULL);
  nxcompat_global_arbiter_release();

  options.policy_flags = NXCOMPAT_POLICY_SESSION_RUNTIME |
                         NXCOMPAT_POLICY_PULSE_SERVER;
  CHECK(unsetenv("XDG_RUNTIME_DIR") == 0);
  CHECK(unsetenv("PULSE_SERVER") == 0);
  CHECK(nxcompat_plan_environment_v2(&host, &options, &plan) == NXCOMPAT_OK);
  CHECK(plan.action_count == 2);
#if defined(NXCOMPAT_CORE_TESTING)
  reset_environment_faults();
  environment_fail_set_at = 2;
  CHECK(nxcompat_apply_environment_v2(&plan) == NXCOMPAT_FAILED);
  CHECK(plan.apply_count == 1 && plan.rollback_count == 1);
  CHECK(plan.env_restored);
  CHECK(plan.final_reason == NXCOMPAT_REASON_ENV_APPLY_FAILED);
  action = find_action_v2(&plan, NXCOMPAT_ACTION_SESSION_RUNTIME);
  CHECK(action && action->state == NXCOMPAT_ACTION_V2_ROLLED_BACK);
  CHECK(action->reason_code == NXCOMPAT_REASON_ENV_ROLLED_BACK);
  action = find_action_v2(&plan, NXCOMPAT_ACTION_PULSE_SERVER);
  CHECK(action && action->state == NXCOMPAT_ACTION_V2_FAILED);
  CHECK(action->reason_code == NXCOMPAT_REASON_ENV_APPLY_FAILED);
  CHECK(getenv("XDG_RUNTIME_DIR") == NULL && getenv("PULSE_SERVER") == NULL);

  reset_environment_faults();
  CHECK(nxcompat_plan_environment_v2(&host, &options, &plan) == NXCOMPAT_OK);
  environment_fail_set_at = 2;
  environment_partial_set_at = 2;
  CHECK(nxcompat_apply_environment_v2(&plan) == NXCOMPAT_FAILED);
  CHECK(plan.apply_count == 1 && plan.rollback_count == 2);
  CHECK(plan.env_restored);
  CHECK(plan.final_reason == NXCOMPAT_REASON_ENV_APPLY_FAILED);
  action = find_action_v2(&plan, NXCOMPAT_ACTION_SESSION_RUNTIME);
  CHECK(action && action->state == NXCOMPAT_ACTION_V2_ROLLED_BACK);
  action = find_action_v2(&plan, NXCOMPAT_ACTION_PULSE_SERVER);
  CHECK(action && action->state == NXCOMPAT_ACTION_V2_ROLLED_BACK);
  CHECK(getenv("XDG_RUNTIME_DIR") == NULL && getenv("PULSE_SERVER") == NULL);

  reset_environment_faults();
  CHECK(nxcompat_plan_environment_v2(&host, &options, &plan) == NXCOMPAT_OK);
  environment_fail_set_at = 2;
  environment_fail_unset_at = 1;
  CHECK(nxcompat_apply_environment_v2(&plan) == NXCOMPAT_ROLLBACK_FAILED);
  CHECK(!plan.env_restored);
  CHECK(plan.final_reason == NXCOMPAT_REASON_ENV_ROLLBACK_FAILED);
  action = find_action_v2(&plan, NXCOMPAT_ACTION_SESSION_RUNTIME);
  CHECK(action && action->state == NXCOMPAT_ACTION_V2_ROLLBACK_FAILED);
  CHECK(action->reason_code == NXCOMPAT_REASON_ENV_ROLLBACK_FAILED);
  reset_environment_faults();
  CHECK(unsetenv("XDG_RUNTIME_DIR") == 0);
  CHECK(unsetenv("PULSE_SERVER") == 0);
#endif

  CHECK(nxcompat_plan_environment_v2(&host, &options, &plan) == NXCOMPAT_OK);
  CHECK(plan.action_count == 2);
  plan.actions[1] = plan.actions[0];
  CHECK(nxcompat_apply_environment_v2(&plan) == NXCOMPAT_INVALID);
  CHECK(getenv("XDG_RUNTIME_DIR") == NULL && getenv("PULSE_SERVER") == NULL);

  CHECK(nxcompat_plan_environment_v2(&host, &options, &plan) == NXCOMPAT_OK);
  plan.actions[0].reason_code = NXCOMPAT_REASON_NONE;
  CHECK(nxcompat_apply_environment_v2(&plan) == NXCOMPAT_INVALID);
  CHECK(getenv("XDG_RUNTIME_DIR") == NULL && getenv("PULSE_SERVER") == NULL);

  CHECK(nxcompat_plan_environment_v2(&host, &options, &plan) == NXCOMPAT_OK);
  plan.final_reason = NXCOMPAT_REASON_ARBITER_BUSY;
  CHECK(nxcompat_apply_environment_v2(&plan) == NXCOMPAT_INVALID);
  CHECK(getenv("XDG_RUNTIME_DIR") == NULL && getenv("PULSE_SERVER") == NULL);

  CHECK(nxcompat_plan_environment_v2(&host, &options, &plan) == NXCOMPAT_OK);
  for (index = 0; index < plan.action_count; ++index) {
    CHECK(plan.actions[index].reason_code != NXCOMPAT_REASON_NONE);
    CHECK(strcmp(plan.actions[index].variable, "SDL_VIDEODRIVER") != 0);
    CHECK(strcmp(plan.actions[index].variable, "SDL_AUDIODRIVER") != 0);
  }
  reset_environment_faults();
  return 0;
}

static int test_display_parser(void) {
  int width = 0;
  int height = 0;
  char path[64];
  CHECK(nxcompat_parse_display_mode("U:1280x720p-60", &width, &height) == 0);
  CHECK(width == 1280 && height == 720);
  CHECK(nxcompat_parse_display_mode("not-a-mode", &width, &height) != 0);
  CHECK(nxcompat_join_path("/", "/pulse/native", path, sizeof(path)) == 0);
  CHECK(strcmp(path, "/pulse/native") == 0);
  CHECK(nxcompat_join_path("/run/user/0/", "/pipewire-0", path,
                           sizeof(path)) == 0);
  CHECK(strcmp(path, "/run/user/0/pipewire-0") == 0);
  CHECK(nxcompat_join_path("/path", "child", path, 5) != 0);
  {
    nxcompat_context context;
    memset(&context, 0, sizeof(context));
    CHECK(snprintf(context.root, sizeof(context.root), "/tmp/fixture") > 0);
    CHECK(nxcompat_join_root(&context, "/run/user/0", path,
                             sizeof(path)) == 0);
    CHECK(strcmp(path, "/tmp/fixture/run/user/0") == 0);
    CHECK(nxcompat_join_root(&context, "/run/../home/private", path,
                             sizeof(path)) != 0);
  }
  return 0;
}

static int test_graphics_capture(void) {
  nxcompat_graphics_options options;
  nxcompat_graphics graphics;
  char line[1024];
  memset(&options, 0, sizeof(options));
  options.api_version = NXCOMPAT_API_VERSION;
  options.struct_size = sizeof(options);
  options.video_driver = "mali";
  options.vendor = "ARM";
  options.renderer = "Mali-450 MP";
  options.version = "OpenGL ES 2.0 build synthetic";
  options.shading_language_version = "OpenGL ES GLSL ES 1.00";
  options.extensions = "GL_OES_compressed_ETC1_RGB8_texture "
                       "GL_OES_texture_npot";
  options.drawable_width = 1280;
  options.drawable_height = 720;
  options.red_bits = 8;
  options.green_bits = 8;
  options.blue_bits = 8;
  options.alpha_bits = 8;
  options.depth_bits = 24;
  options.stencil_bits = 8;
  CHECK(nxcompat_capture_graphics(&options, &graphics) == 0);
  CHECK(graphics.gpu_family == NXCOMPAT_GPU_MALI_4XX);
  CHECK(graphics.gles_major == 2 && graphics.gles_minor == 0);
  CHECK((graphics.capabilities & NXCOMPAT_GRAPHICS_GLES2) != 0);
  CHECK((graphics.capabilities & NXCOMPAT_GRAPHICS_GLES3) == 0);
  CHECK((graphics.capabilities & NXCOMPAT_GRAPHICS_ETC1) != 0);
  CHECK((graphics.capabilities & NXCOMPAT_GRAPHICS_NPOT_FULL) != 0);
  CHECK(nxcompat_format_graphics_line(&graphics, line, sizeof(line)) > 0);
  CHECK(strstr(line, "Mali-450 MP") != NULL);
  CHECK(strstr(line, "GLES 2.0") != NULL);

  options.vendor = "Mesa";
  options.renderer = "Panfrost Mali-G31";
  options.version = "OpenGL ES 3.2 Mesa";
  options.extensions = "GL_KHR_texture_compression_astc_ldr";
  CHECK(nxcompat_capture_graphics(&options, &graphics) == 0);
  CHECK(graphics.gpu_family == NXCOMPAT_GPU_PANFROST);
  CHECK((graphics.capabilities & NXCOMPAT_GRAPHICS_GLES3) != 0);
  CHECK((graphics.capabilities & NXCOMPAT_GRAPHICS_ETC2) != 0);
  CHECK((graphics.capabilities & NXCOMPAT_GRAPHICS_ASTC) != 0);
  return 0;
}

/* fbdev virtual_size fallback: "W,VH" with the double-buffer pan halved only
 * when the halved height still matches the panel width (Horizon/Bully/Sonic
 * fleet heuristic: 640,960 -> 640x480; 640,480 stays; 1280,1440 -> 1280x720;
 * a readable fb0/mode always wins over virtual_size). */
static int test_fbdev_virtual_size(void) {
  static const struct {
    const char *virtual_size;
    const char *mode;
    int width;
    int height;
    const char *source;
  } cases[] = {
    {"640,960\n", NULL, 640, 480, "/sys/class/graphics/fb0/virtual_size"},
    {"640,480\n", NULL, 640, 480, "/sys/class/graphics/fb0/virtual_size"},
    {"1280,1440\n", NULL, 1280, 720, "/sys/class/graphics/fb0/virtual_size"},
    {"480,1620\n", NULL, 480, 1620, "/sys/class/graphics/fb0/virtual_size"},
    {"640,960\n", "U:800x600p-0\n", 800, 600, "/sys/class/graphics/fb0/mode"},
  };
  size_t index;
  for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    char root_template[] = "/tmp/nxcompat-vsz-XXXXXX";
    char *root = mkdtemp(root_template);
    char path[NXCOMPAT_PATH_MAX];
    nxcompat_probe_options probe_options;
    nxcompat_host host;
    CHECK(root != NULL);
    CHECK(host_path(path, sizeof(path), root, "/dev/fb0") == 0);
    CHECK(write_text(path, "") == 0);
    CHECK(host_path(path, sizeof(path), root,
                    "/sys/class/graphics/fb0/virtual_size") == 0);
    CHECK(write_text(path, cases[index].virtual_size) == 0);
    if (cases[index].mode != NULL) {
      CHECK(host_path(path, sizeof(path), root,
                      "/sys/class/graphics/fb0/mode") == 0);
      CHECK(write_text(path, cases[index].mode) == 0);
    }
    memset(&probe_options, 0, sizeof(probe_options));
    probe_options.api_version = NXCOMPAT_API_VERSION;
    probe_options.struct_size = sizeof(probe_options);
    probe_options.port_id = "fixture";
    probe_options.probe_root = root;
    CHECK(nxcompat_probe(&probe_options, &host) == 0);
    CHECK(host.display_width == cases[index].width);
    CHECK(host.display_height == cases[index].height);
    CHECK(strcmp(host.display_source, cases[index].source) == 0);
    CHECK((host.capabilities & NXCOMPAT_CAP_FBDEV) != 0);
  }
  return 0;
}

#undef CHECK
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
      result = -1;                                                             \
      goto out;                                                                \
    }                                                                          \
  } while (0)

static int test_fixture(void) {
  char root_template[] = "/tmp/nxcompat-test-XXXXXX";
  char *root = mkdtemp(root_template);
  char runtime_dir[NXCOMPAT_PATH_MAX];
  char inherited_db_path[NXCOMPAT_PATH_MAX];
  char relative_db_path[NXCOMPAT_PATH_MAX];
  char device_line[1024];
  char fix_line[1024];
  char json[8192];
  nxcompat_probe_options probe_options;
  nxcompat_plan_options plan_options;
  nxcompat_host host;
  nxcompat_plan plan;
  nxcompat_host stale_host;
  const nxcompat_action *action;
  int pulse_listener = -1;
  int pipewire_listener = -1;
  int result = -1;

  CHECK(root != NULL);
  if (prepare_fixture(root, runtime_dir, sizeof(runtime_dir), &pulse_listener,
                      &pipewire_listener) != 0)
    goto out;
  CHECK(unsetenv("XDG_RUNTIME_DIR") == 0);
  CHECK(setenv("WAYLAND_DISPLAY", "wayland-0", 1) == 0);
  CHECK(setenv("SDL_VIDEODRIVER", "wayland", 1) == 0);
  CHECK(setenv("SDL_AUDIODRIVER", "pulseaudio", 1) == 0);
  CHECK(unsetenv("PULSE_SERVER") == 0);
  CHECK(unsetenv("PIPEWIRE_MODULE_DIR") == 0);
  CHECK(unsetenv("SPA_PLUGIN_DIR") == 0);
  CHECK(unsetenv("ALSA_PLUGIN_DIR") == 0);
  CHECK(unsetenv("SDL_GAMECONTROLLERCONFIG") == 0);
  CHECK(unsetenv("SDL_GAMECONTROLLERCONFIG_FILE") == 0);
  CHECK(unsetenv("SDL_GAMECONTROLLER_USE_BUTTON_LABELS") == 0);

  memset(&probe_options, 0, sizeof(probe_options));
  probe_options.api_version = NXCOMPAT_API_VERSION;
  probe_options.struct_size = sizeof(probe_options);
  probe_options.port_id = "fixture";
  probe_options.game_dir = "/userdata/roms/ports/test";
  probe_options.portmaster_dir = "/opt/tools/PortMaster";
  probe_options.probe_root = root;
  CHECK(nxcompat_probe(&probe_options, &host) == 0);
  CHECK(strcmp(host.os_id, "fixtureos") == 0);
  CHECK(strcmp(host.device_model, "Synthetic Handheld") == 0);
  CHECK(strcmp(host.os_version, "42") == 0);
  CHECK(host.memory_class == NXCOMPAT_MEMORY_SHORT);
  CHECK(host.filesystem_class == NXCOMPAT_FILESYSTEM_FUSE_LIKE);
  CHECK(host.display_width == 640 && host.display_height == 480);
  CHECK((host.capabilities & NXCOMPAT_CAP_DRM_CONNECTED) != 0);
  CHECK((host.capabilities & NXCOMPAT_CAP_FBDEV) != 0);
  CHECK((host.capabilities & NXCOMPAT_CAP_WAYLAND) != 0);
  CHECK((host.capabilities & NXCOMPAT_CAP_PULSE_SOCKET) != 0);
  CHECK((host.capabilities & NXCOMPAT_CAP_PIPEWIRE_SOCKET) != 0);
  CHECK((host.capabilities & NXCOMPAT_CAP_ALSA) != 0);
  CHECK((host.capabilities & NXCOMPAT_CAP_PORTMASTER) != 0);
  CHECK((host.capabilities & NXCOMPAT_CAP_ARMHF_LIBS) != 0);
  CHECK((host.capabilities & NXCOMPAT_CAP_AARCH64_LIBS) != 0);
  CHECK((host.capabilities & NXCOMPAT_CAP_I386_LIBS) == 0);
  CHECK(strcmp(host.controller_db,
               "/opt/tools/PortMaster/gamecontrollerdb.txt") == 0);

  /* The launcher-selected database is authoritative. A non-empty inherited
   * file wins over discovery; an explicitly selected empty file must not be
   * hidden by an unrelated full PortMaster database. */
  CHECK(host_path(inherited_db_path, sizeof(inherited_db_path), root,
                  "/tmp/inherited-gamecontrollerdb.txt") == 0);
  CHECK(write_text(inherited_db_path, "inherited mapping\n") == 0);
  CHECK(setenv("SDL_GAMECONTROLLERCONFIG_FILE",
               "/tmp/inherited-gamecontrollerdb.txt", 1) == 0);
  CHECK(nxcompat_probe(&probe_options, &stale_host) == 0);
  CHECK(strcmp(stale_host.controller_db,
               "/tmp/inherited-gamecontrollerdb.txt") == 0);
  CHECK((stale_host.capabilities & NXCOMPAT_CAP_CONTROLLER_MAPPING) != 0u);

  CHECK(host_path(relative_db_path, sizeof(relative_db_path), root,
                  "/userdata/roms/ports/test/relative-controllerdb.txt") ==
        0);
  CHECK(write_text(relative_db_path, "relative mapping\n") == 0);
  CHECK(setenv("SDL_GAMECONTROLLERCONFIG_FILE",
               "relative-controllerdb.txt", 1) == 0);
  CHECK(nxcompat_probe(&probe_options, &stale_host) == 0);
  CHECK(strcmp(stale_host.controller_db,
               "/userdata/roms/ports/test/relative-controllerdb.txt") == 0);
  CHECK((stale_host.capabilities & NXCOMPAT_CAP_CONTROLLER_MAPPING) != 0u);

  CHECK(setenv("SDL_GAMECONTROLLERCONFIG_FILE",
               "/tmp/inherited-gamecontrollerdb.txt", 1) == 0);
  CHECK(write_text(inherited_db_path, "") == 0);
  CHECK(nxcompat_probe(&probe_options, &stale_host) == 0);
  CHECK(stale_host.controller_db[0] == '\0');
  CHECK((stale_host.capabilities & NXCOMPAT_CAP_CONTROLLER_MAPPING) == 0u);
  CHECK(unsetenv("SDL_GAMECONTROLLERCONFIG_FILE") == 0);

  /* An AArch64 host planning an ARMHF child is the muOS/ROCKNIX case. */
  host.kernel_arch = NXCOMPAT_ARCH_AARCH64;
  CHECK(unsetenv("XDG_RUNTIME_DIR") == 0);
  memset(&plan_options, 0, sizeof(plan_options));
  plan_options.api_version = NXCOMPAT_API_VERSION;
  plan_options.struct_size = sizeof(plan_options);
  plan_options.runtime_arch = NXCOMPAT_ARCH_ARMV7;
  plan_options.policy_flags = NXCOMPAT_POLICY_AUTOMATIC_SAFE |
                              NXCOMPAT_POLICY_LOW_MEMORY_ARENAS;
  plan_options.low_memory_arena_max = 2;
  CHECK(nxcompat_plan_environment(&host, &plan_options, &plan) == 0);
  action = find_action(&plan, NXCOMPAT_ACTION_SESSION_RUNTIME);
  CHECK(action && action->state == NXCOMPAT_ACTION_PLANNED);
  action = find_action(&plan, NXCOMPAT_ACTION_PULSE_SERVER);
  CHECK(action && action->state == NXCOMPAT_ACTION_PLANNED);
  action = find_action(&plan, NXCOMPAT_ACTION_PIPEWIRE_MODULE_DIR);
  CHECK(action && action->state == NXCOMPAT_ACTION_PLANNED);
  action = find_action(&plan, NXCOMPAT_ACTION_CONTROLLER_DB);
  CHECK(action && action->state == NXCOMPAT_ACTION_PLANNED);
  action = find_action(&plan, NXCOMPAT_ACTION_MALLOC_ARENAS);
  CHECK(action && action->state == NXCOMPAT_ACTION_PLANNED);
  CHECK(nxcompat_apply_environment(&plan) == 0);
  CHECK(environment_equals("XDG_RUNTIME_DIR", runtime_dir));
  CHECK(environment_starts_with("PULSE_SERVER", "unix:/run/user/"));
  CHECK(environment_equals(
      "PIPEWIRE_MODULE_DIR",
      "/usr/lib/arm-linux-gnueabihf/pipewire-0.3"));
  CHECK(environment_equals(
      "SDL_GAMECONTROLLERCONFIG_FILE",
      "/opt/tools/PortMaster/gamecontrollerdb.txt"));
  CHECK(environment_equals("MALLOC_ARENA_MAX", "2"));
  CHECK(nxcompat_format_device_line(&host, device_line, sizeof(device_line)) >
        0);
  CHECK(strstr(device_line, "fixtureos 42") != NULL);
  CHECK(strstr(device_line, "Synthetic Handheld") != NULL);
  CHECK(strstr(device_line, "display wayland+drm 640x480") != NULL);
  CHECK(nxcompat_format_fix_line(&plan, fix_line, sizeof(fix_line)) > 0);
  CHECK(strstr(fix_line, "pulse-socket:applied") != NULL);
  CHECK(nxcompat_format_json(&host, &plan, json, sizeof(json)) > 0);
  CHECK(strstr(json, "\"port_id\":\"fixture\"") != NULL);
  CHECK(strstr(json, root) == NULL);

  probe_options.portmaster_dir = NULL;
  CHECK(unsetenv("NXCOMPAT_PORTMASTER_DIR") == 0);
  CHECK(nxcompat_probe(&probe_options, &stale_host) == 0);
  CHECK(strcmp(stale_host.portmaster_dir,
               "/userdata/roms/ports/PortMaster") == 0);
  CHECK(setenv("NXCOMPAT_PORTMASTER_DIR", "/opt/tools/PortMaster", 1) == 0);
  CHECK(nxcompat_probe(&probe_options, &stale_host) == 0);
  CHECK(strcmp(stale_host.portmaster_dir, "/opt/tools/PortMaster") == 0);
  CHECK(unsetenv("NXCOMPAT_PORTMASTER_DIR") == 0);

  CHECK(close(pulse_listener) == 0);
  pulse_listener = -1;
  CHECK(close(pipewire_listener) == 0);
  pipewire_listener = -1;
  CHECK(host_path(device_line, sizeof(device_line), root,
                  "/usr/lib/arm-linux-gnueabihf/pipewire-0.3/"
                  "libpipewire-module-protocol-native.so") == 0);
  CHECK(unlink(device_line) == 0);
  CHECK(host_path(device_line, sizeof(device_line), root,
                  "/usr/lib/arm-linux-gnueabihf/spa-0.2/support/"
                  "libspa-support.so") == 0);
  CHECK(unlink(device_line) == 0);
  CHECK(host_path(device_line, sizeof(device_line), root,
                  "/usr/lib/arm-linux-gnueabihf/alsa-lib/"
                  "libasound_module_pcm_pipewire.so") == 0);
  CHECK(unlink(device_line) == 0);
  CHECK(nxcompat_probe(&probe_options, &stale_host) == 0);
  CHECK((stale_host.capabilities & NXCOMPAT_CAP_PULSE_SOCKET) == 0);
  CHECK((stale_host.capabilities & NXCOMPAT_CAP_PIPEWIRE_SOCKET) == 0);
  CHECK(stale_host.armhf_pipewire_modules[0] == '\0');
  CHECK(stale_host.armhf_spa_plugins[0] == '\0');
  CHECK(stale_host.armhf_alsa_plugins[0] == '\0');
  result = 0;

out:
  if (pulse_listener >= 0)
    (void)close(pulse_listener);
  if (pipewire_listener >= 0)
    (void)close(pipewire_listener);
  if (root && strncmp(root, "/tmp/nxcompat-test-", 19) == 0 &&
      remove_tree(root) != 0) {
    fprintf(stderr, "FAIL %s:%d: remove_tree(root) == 0\n", __FILE__,
            __LINE__);
    result = -1;
  }
  return result;
}

#undef CHECK

/* Onda v2 (V2-01): a regra de idioma escrita e testada UMA vez -- inclusive
 * o "nunca japones" (#5), que cada port reimplementava por conta. */
#include "nxcompat_language.h"

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,       \
              #condition);                                                     \
      return -1;                                                               \
    }                                                                          \
  } while (0)

static int test_language_helper(void) {
  static const char *const supported[] = {"en", "fr", "pt-br", "zh-cn"};
  char receipt[64];

  setenv("NXPORT_LANGUAGE", "FR", 1);
  CHECK(strcmp(nxcompat_language_select(supported, 4, "en", receipt,
                                        sizeof receipt), "fr") == 0);
  CHECK(strstr(receipt, "exact") != NULL);

  setenv("NXPORT_LANGUAGE", "pt_BR", 1); /* underscore + maiuscula */
  CHECK(strcmp(nxcompat_language_select(supported, 4, "en", NULL, 0),
               "pt-br") == 0);

  setenv("NXPORT_LANGUAGE", "pt", 1); /* subtag primaria acha pt-br */
  CHECK(strcmp(nxcompat_language_select(supported, 4, "en", NULL, 0),
               "pt-br") == 0);

  setenv("NXPORT_LANGUAGE", "zh", 1);
  CHECK(strcmp(nxcompat_language_select(supported, 4, "en", NULL, 0),
               "zh-cn") == 0);

  setenv("NXPORT_LANGUAGE", "ja", 1); /* regra #5: pedido nao vale */
  CHECK(strcmp(nxcompat_language_select(supported, 4, "en", NULL, 0),
               "en") == 0);

  setenv("NXPORT_LANGUAGE", "auto", 1);
  setenv("GAME_LANGUAGE", "fr", 1); /* reforco entra quando o 1o e' auto */
  CHECK(strcmp(nxcompat_language_select(supported, 4, "en", NULL, 0),
               "fr") == 0);

  unsetenv("NXPORT_LANGUAGE");
  unsetenv("GAME_LANGUAGE");
  CHECK(strcmp(nxcompat_language_select(supported, 4, "pt-br", NULL, 0),
               "pt-br") == 0); /* fallback declarado */
  CHECK(strcmp(nxcompat_language_select(supported, 4, "ja", receipt,
                                        sizeof receipt), "en") == 0);
  CHECK(strstr(receipt, "fallback-invalido-en") != NULL); /* #5 no fallback */
  return 0;
}

#undef CHECK

int main(void) {
  if (test_display_parser() != 0 || test_graphics_capture() != 0 ||
      test_fbdev_virtual_size() != 0 ||
      test_backend_negotiation() != 0 || test_plan_v2() != 0 ||
      test_fixture() != 0 || test_language_helper() != 0)
    return 1;
  puts("nxcompat tests passed");
  return 0;
}
