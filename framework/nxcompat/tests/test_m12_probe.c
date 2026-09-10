/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "nxcompat.h"

#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
      return -1;                                                               \
    }                                                                          \
  } while (0)

#define FIXTURE_CHECK(condition)                                               \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
      status = -1;                                                             \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)

typedef struct nxcompat_probe_options_v1_layout {
  uint32_t api_version;
  size_t struct_size;
  const char *port_id;
  const char *game_dir;
  const char *portmaster_dir;
  const char *probe_root;
} nxcompat_probe_options_v1_layout;

typedef char api_v1_options_prefix_is_frozen
    [NXCOMPAT_PROBE_OPTIONS_V1_SIZE ==
             sizeof(nxcompat_probe_options_v1_layout)
         ? 1
         : -1];
typedef char api_numbers_are_frozen
    [NXCOMPAT_API_VERSION_V1 == 1u && NXCOMPAT_API_VERSION_V2 == 2u &&
             NXCOMPAT_API_VERSION == NXCOMPAT_API_VERSION_V2
         ? 1
         : -1];
typedef char public_status_numbers_are_frozen
    [NXCOMPAT_OK == 0 && NXCOMPAT_INVALID == -1 && NXCOMPAT_BUSY == -2 &&
             NXCOMPAT_FAILED == -3 && NXCOMPAT_ROLLBACK_FAILED == -4
         ? 1
         : -1];

typedef struct synthetic_provider {
  const char *values[NXCOMPAT_MAX_OBSERVATIONS];
  unsigned calls;
  unsigned unknown_calls;
  int unterminated_mem_available;
  int reenter;
  int reentry_status;
  int reentry_host_unchanged;
  int reentry_result_unchanged;
  nxcompat_probe_options *reentry_options;
} synthetic_provider;

static int fixture_path(char *output, size_t output_size, const char *root,
                        const char *suffix) {
  int count;
  if (!output || !root || !suffix || suffix[0] != '/')
    return -1;
  count = snprintf(output, output_size, "%s%s", root, suffix);
  return count >= 0 && (size_t)count < output_size ? 0 : -1;
}

static int fixture_mkdirs(const char *root, const char *suffix) {
  char path[NXCOMPAT_PATH_MAX * 2u];
  char *cursor;
  if (fixture_path(path, sizeof(path), root, suffix) != 0)
    return -1;
  for (cursor = path + strlen(root) + 1u; *cursor; ++cursor) {
    if (*cursor != '/')
      continue;
    *cursor = '\0';
    if (mkdir(path, 0700) != 0 && errno != EEXIST)
      return -1;
    *cursor = '/';
  }
  if (mkdir(path, 0700) != 0 && errno != EEXIST)
    return -1;
  return 0;
}

static int fixture_write_bytes(const char *root, const char *suffix,
                               const void *contents, size_t length) {
  char path[NXCOMPAT_PATH_MAX * 2u];
  char parent[NXCOMPAT_PATH_MAX * 2u];
  char *slash;
  FILE *stream;
  size_t written;
  int close_status;
  if (fixture_path(path, sizeof(path), root, suffix) != 0 ||
      snprintf(parent, sizeof(parent), "%s", suffix) >= (int)sizeof(parent))
    return -1;
  slash = strrchr(parent, '/');
  if (!slash)
    return -1;
  *slash = '\0';
  if (parent[0] && fixture_mkdirs(root, parent) != 0)
    return -1;
  stream = fopen(path, "wb");
  if (!stream)
    return -1;
  written = fwrite(contents, 1u, length, stream);
  close_status = fclose(stream);
  if (written != length || close_status != 0)
    return -1;
  return 0;
}

static int fixture_write(const char *root, const char *suffix,
                         const char *contents) {
  return fixture_write_bytes(root, suffix, contents, strlen(contents));
}

static int fixture_unlink(const char *root, const char *suffix) {
  char path[NXCOMPAT_PATH_MAX * 2u];
  return fixture_path(path, sizeof(path), root, suffix) == 0 &&
                 unlink(path) == 0
             ? 0
             : -1;
}

static int fixture_remove_tree(const char *path) {
  DIR *directory = opendir(path);
  struct dirent *entry;
  if (!directory)
    return unlink(path) == 0 ? 0 : -1;
  while ((entry = readdir(directory)) != NULL) {
    char child[NXCOMPAT_PATH_MAX * 2u];
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >=
        (int)sizeof(child) ||
        fixture_remove_tree(child) != 0) {
      (void)closedir(directory);
      return -1;
    }
  }
  if (closedir(directory) != 0)
    return -1;
  return rmdir(path) == 0 ? 0 : -1;
}

static size_t observation_slot(nxcompat_observation_id id) {
  switch (id) {
  case NXCOMPAT_OBSERVATION_PROCESS_MACHINE:
    return 0u;
  case NXCOMPAT_OBSERVATION_KERNEL_MACHINE:
    return 1u;
  case NXCOMPAT_OBSERVATION_USERLAND_MACHINE:
    return 2u;
  case NXCOMPAT_OBSERVATION_MEM_TOTAL_KIB:
    return 3u;
  case NXCOMPAT_OBSERVATION_MEM_AVAILABLE_KIB:
    return 4u;
  case NXCOMPAT_OBSERVATION_SWAP_TOTAL_KIB:
    return 5u;
  case NXCOMPAT_OBSERVATION_VM_SIZE_KIB:
    return 6u;
  case NXCOMPAT_OBSERVATION_CGROUP_MODE:
    return 7u;
  case NXCOMPAT_OBSERVATION_CGROUP_LIMIT_BYTES:
    return 8u;
  case NXCOMPAT_OBSERVATION_CGROUP_CURRENT_BYTES:
    return 9u;
  case NXCOMPAT_OBSERVATION_RLIMIT_AS_BYTES:
    return 10u;
  default:
    return NXCOMPAT_MAX_OBSERVATIONS;
  }
}

static nxcompat_provider_result synthetic_read(void *userdata,
                                               nxcompat_observation_id id,
                                               char *value,
                                               size_t value_size) {
  synthetic_provider *provider = (synthetic_provider *)userdata;
  size_t slot = observation_slot(id);
  ++provider->calls;
  if (provider->reenter && provider->calls == 1u) {
    nxcompat_host nested_host;
    nxcompat_host nested_host_before;
    nxcompat_probe_result result_before;
    memset(&nested_host, 0xa5, sizeof(nested_host));
    memcpy(&nested_host_before, &nested_host, sizeof(nested_host_before));
    memcpy(&result_before, provider->reentry_options->result,
           sizeof(result_before));
    provider->reentry_status =
        nxcompat_probe(provider->reentry_options, &nested_host);
    provider->reentry_host_unchanged =
        memcmp(&nested_host, &nested_host_before, sizeof(nested_host)) == 0;
    provider->reentry_result_unchanged =
        memcmp(provider->reentry_options->result, &result_before,
               sizeof(result_before)) == 0;
  }
  if (slot == NXCOMPAT_MAX_OBSERVATIONS) {
    ++provider->unknown_calls;
    return NXCOMPAT_PROVIDER_ERROR;
  }
  if (provider->unterminated_mem_available &&
      id == NXCOMPAT_OBSERVATION_MEM_AVAILABLE_KIB) {
    memset(value, '7', value_size);
    return NXCOMPAT_PROVIDER_VALUE;
  }
  if (!provider->values[slot])
    return NXCOMPAT_PROVIDER_ABSENT;
  (void)snprintf(value, value_size, "%s", provider->values[slot]);
  return NXCOMPAT_PROVIDER_VALUE;
}

static void provider_init(synthetic_provider *fixture) {
  memset(fixture, 0, sizeof(*fixture));
  fixture->values[0] = "armv7";
  fixture->values[1] = "aarch64";
  fixture->values[2] = "armv7";
  fixture->values[3] = "4194304";
  fixture->values[4] = "600000";
  fixture->values[5] = "123";
  fixture->values[6] = "100000";
  fixture->values[7] = "2";
  fixture->values[8] = "536870912";
  fixture->values[9] = "268435456";
  fixture->values[10] = "314572800";
}

static void options_init(nxcompat_probe_options *options,
                         nxcompat_probe_provider *provider,
                         nxcompat_probe_result *result,
                         synthetic_provider *fixture) {
  memset(provider, 0, sizeof(*provider));
  provider->api_version = NXCOMPAT_API_VERSION_V2;
  provider->struct_size = sizeof(*provider);
  provider->read = synthetic_read;
  provider->userdata = fixture;
  memset(options, 0, sizeof(*options));
  options->api_version = NXCOMPAT_API_VERSION_V2;
  options->struct_size = sizeof(*options);
  options->port_id = "hermetic-fixture";
  options->game_dir = "/fixture/game";
  options->probe_root = "/must-not-be-read";
  options->provider = provider;
  options->result = result;
}

static int test_hermetic_effective_memory(void) {
  synthetic_provider fixture;
  nxcompat_probe_provider provider;
  nxcompat_probe_options options;
  nxcompat_probe_result result;
  nxcompat_host host;
  const nxcompat_observation *observation;
  provider_init(&fixture);
  options_init(&options, &provider, &result, &fixture);
  CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  CHECK(fixture.calls == 11u && fixture.unknown_calls == 0u);
  CHECK(result.observation_count == 11u);
  CHECK(result.process_arch == NXCOMPAT_ARCH_ARMV7);
  CHECK(result.kernel_arch == NXCOMPAT_ARCH_AARCH64);
  CHECK(result.userland_arch == NXCOMPAT_ARCH_ARMV7);
  CHECK(result.memory_total_kib == UINT64_C(4194304));
  CHECK(result.memory_available_kib == UINT64_C(600000));
  CHECK(result.cgroup_limit_kib == UINT64_C(524288));
  CHECK(result.cgroup_current_kib == UINT64_C(262144));
  CHECK(result.rlimit_as_kib == UINT64_C(307200));
  CHECK(result.memory_effective_kib == UINT64_C(207200));
  CHECK(result.memory_sources ==
        (NXCOMPAT_MEMORY_SOURCE_MEMAVAILABLE |
         NXCOMPAT_MEMORY_SOURCE_CGROUP | NXCOMPAT_MEMORY_SOURCE_RLIMIT_AS));
  CHECK(result.final_reason == NXCOMPAT_REASON_EFFECTIVE_MEMORY_MINIMUM);
  CHECK(host.api_version == NXCOMPAT_API_VERSION_V2);
  CHECK(host.process_arch == NXCOMPAT_ARCH_ARMV7);
  CHECK(host.kernel_arch == NXCOMPAT_ARCH_AARCH64);
  CHECK(host.memory_class == NXCOMPAT_MEMORY_SHORT);
  CHECK(host.capabilities == NXCOMPAT_CAP_SHORT_MEMORY);
  CHECK(host.os_id[0] == '\0' && host.device_model[0] == '\0');
  CHECK(host.inherited_video_driver[0] == '\0');
  observation = nxcompat_probe_find_observation(
      &result, NXCOMPAT_OBSERVATION_MEM_AVAILABLE_KIB);
  CHECK(observation != NULL);
  CHECK(observation->state == NXCOMPAT_OBSERVATION_PRESENT);
  CHECK(observation->reason == NXCOMPAT_REASON_MEMAVAILABLE_VERIFIED);
  CHECK(strcmp(nxcompat_observation_name(observation->id),
               "mem-available-kib") == 0);
  CHECK(strcmp(nxcompat_reason_name(observation->reason),
               "memavailable-verified") == 0);
  return 0;
}

static int test_provider_contract_and_unbounded_limits(void) {
  synthetic_provider fixture;
  nxcompat_probe_provider provider;
  nxcompat_probe_options options;
  nxcompat_probe_result result;
  nxcompat_host host;
  const nxcompat_observation *observation;
  provider_init(&fixture);
  fixture.unterminated_mem_available = 1;
  fixture.values[7] = "1";
  fixture.values[8] = "9223372036854771712";
  fixture.values[10] = "max";
  options_init(&options, &provider, &result, &fixture);
  memset(&host, 0x5a, sizeof(host));
  CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_FAILED);
  observation = nxcompat_probe_find_observation(
      &result, NXCOMPAT_OBSERVATION_MEM_AVAILABLE_KIB);
  CHECK(observation &&
        observation->state == NXCOMPAT_OBSERVATION_REJECTED);
  CHECK(observation->reason == NXCOMPAT_REASON_PROVIDER_CONTRACT);
  observation = nxcompat_probe_find_observation(
      &result, NXCOMPAT_OBSERVATION_RLIMIT_AS_BYTES);
  CHECK(observation && observation->state == NXCOMPAT_OBSERVATION_PRESENT);
  CHECK(observation->reason == NXCOMPAT_REASON_LIMIT_UNBOUNDED);
  CHECK(result.final_reason == NXCOMPAT_REASON_PROVIDER_CONTRACT);
  CHECK(host.capabilities == 0u);
  CHECK(host.process_arch == NXCOMPAT_ARCH_UNKNOWN);
  CHECK(result.memory_sources == 0u);
  CHECK(result.memory_effective_kib == 0u);

  provider_init(&fixture);
  fixture.values[4] = "18446744073709551616";
  options_init(&options, &provider, &result, &fixture);
  CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_FAILED);
  observation = nxcompat_probe_find_observation(
      &result, NXCOMPAT_OBSERVATION_MEM_AVAILABLE_KIB);
  CHECK(observation &&
        observation->state == NXCOMPAT_OBSERVATION_REJECTED);
  CHECK(observation->reason == NXCOMPAT_REASON_OBSERVATION_MALFORMED);
  CHECK(result.final_reason == NXCOMPAT_REASON_PROVIDER_CONTRACT);
  return 0;
}

static int test_exhausted_limits_are_zero(void) {
  synthetic_provider fixture;
  nxcompat_probe_provider provider;
  nxcompat_probe_options options;
  nxcompat_probe_result result;
  nxcompat_host host;

  provider_init(&fixture);
  fixture.values[8] = "1024";
  fixture.values[9] = "2048";
  fixture.values[10] = "max";
  options_init(&options, &provider, &result, &fixture);
  CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  CHECK((result.memory_sources & NXCOMPAT_MEMORY_SOURCE_CGROUP) != 0u);
  CHECK(result.memory_effective_kib == 0u);
  CHECK(host.memory_class == NXCOMPAT_MEMORY_SHORT);

  provider_init(&fixture);
  fixture.values[8] = "max";
  fixture.values[10] = "1024";
  options_init(&options, &provider, &result, &fixture);
  CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  CHECK((result.memory_sources & NXCOMPAT_MEMORY_SOURCE_RLIMIT_AS) != 0u);
  CHECK(result.memory_effective_kib == 0u);
  CHECK(host.memory_class == NXCOMPAT_MEMORY_SHORT);
  return 0;
}

static int test_context_labels_do_not_change_capabilities(void) {
  synthetic_provider fixture;
  nxcompat_probe_provider provider;
  nxcompat_probe_options options;
  nxcompat_probe_result first;
  nxcompat_probe_result second;
  nxcompat_host first_host;
  nxcompat_host second_host;
  provider_init(&fixture);
  options_init(&options, &provider, &first, &fixture);
  options.port_id = "context-a";
  options.game_dir = "/context/a";
  CHECK(nxcompat_probe(&options, &first_host) == NXCOMPAT_OK);
  fixture.calls = 0u;
  options.result = &second;
  options.port_id = "entirely-different-label";
  options.game_dir = "/unrelated/context/b";
  CHECK(nxcompat_probe(&options, &second_host) == NXCOMPAT_OK);
  CHECK(first.memory_effective_kib == second.memory_effective_kib);
  CHECK(first.memory_sources == second.memory_sources);
  CHECK(first.process_arch == second.process_arch);
  CHECK(first_host.capabilities == second_host.capabilities);
  CHECK(first_host.os_id[0] == '\0' && second_host.os_id[0] == '\0');
  CHECK(first_host.device_model[0] == '\0' &&
        second_host.device_model[0] == '\0');
  return 0;
}

static int test_arbiter_reentry_is_busy(void) {
  synthetic_provider fixture;
  nxcompat_probe_provider provider;
  nxcompat_probe_options options;
  nxcompat_probe_result result;
  nxcompat_host host;
  provider_init(&fixture);
  options_init(&options, &provider, &result, &fixture);
  fixture.reenter = 1;
  fixture.reentry_options = &options;
  fixture.reentry_status = 99;
  CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  CHECK(fixture.reentry_status == NXCOMPAT_BUSY);
  CHECK(fixture.reentry_host_unchanged == 1);
  CHECK(fixture.reentry_result_unchanged == 1);
  CHECK(result.observation_count == 11u);
  CHECK(result.process_arch == NXCOMPAT_ARCH_ARMV7);
  CHECK(result.kernel_arch == NXCOMPAT_ARCH_AARCH64);
  CHECK(result.final_reason == NXCOMPAT_REASON_EFFECTIVE_MEMORY_MINIMUM);
  return 0;
}

static int test_limits_without_usage_do_not_claim_headroom(void) {
  synthetic_provider fixture;
  nxcompat_probe_provider provider;
  nxcompat_probe_options options;
  nxcompat_probe_result result;
  nxcompat_host host;
  provider_init(&fixture);
  /* A proven finite cgroup limit without usage blocks all effective-memory
   * classification: another source cannot prove that constraint has room. */
  fixture.values[9] = NULL; /* cgroup current */
  options_init(&options, &provider, &result, &fixture);
  CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  CHECK(result.cgroup_limit_kib == UINT64_C(524288));
  CHECK(result.rlimit_as_kib == UINT64_C(307200));
  CHECK(result.memory_sources == 0u);
  CHECK(result.memory_effective_kib == 0u);
  CHECK(result.final_reason == NXCOMPAT_REASON_EFFECTIVE_MEMORY_UNKNOWN);
  CHECK(host.memory_class == NXCOMPAT_MEMORY_UNKNOWN);
  CHECK((host.capabilities & NXCOMPAT_CAP_SHORT_MEMORY) == 0u);

  /* A finite RLIMIT without VmSize is likewise only a ceiling, not usable
   * headroom.  Even a present MemAvailable cannot prove that applied limit. */
  provider_init(&fixture);
  fixture.values[6] = NULL;
  fixture.values[7] = NULL;
  fixture.values[8] = NULL;
  fixture.values[9] = NULL;
  options_init(&options, &provider, &result, &fixture);
  CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  CHECK(result.rlimit_as_kib == UINT64_C(307200));
  CHECK(result.memory_sources == 0u);
  CHECK(result.memory_effective_kib == 0u);
  CHECK(host.memory_class == NXCOMPAT_MEMORY_UNKNOWN);
  return 0;
}

static int test_probe_root_is_fail_closed(void) {
  synthetic_provider fixture;
  nxcompat_probe_provider provider;
  nxcompat_probe_options options;
  nxcompat_probe_result result;
  nxcompat_probe_result result_before;
  nxcompat_host host;
  nxcompat_host host_before;
  char overlong[NXCOMPAT_PATH_MAX + 1u];
  const char *invalid_roots[5];
  size_t index;

  memset(overlong, 'r', sizeof(overlong));
  overlong[0] = '/';
  overlong[sizeof(overlong) - 1u] = '\0';
  invalid_roots[0] = "relative-root";
  invalid_roots[1] = "";
  invalid_roots[2] = overlong;
  invalid_roots[3] = "/..";
  invalid_roots[4] = "/tmp/../fixture";
  for (index = 0; index < 5u; ++index) {
    provider_init(&fixture);
    options_init(&options, &provider, &result, &fixture);
    options.probe_root = invalid_roots[index];
    memset(&result, 0x3c, sizeof(result));
    memcpy(&result_before, &result, sizeof(result_before));
    memset(&host, 0xa7, sizeof(host));
    memcpy(&host_before, &host, sizeof(host_before));
    CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_INVALID);
    CHECK(fixture.calls == 0u);
    CHECK(memcmp(&host, &host_before, sizeof(host)) == 0);
    CHECK(memcmp(&result, &result_before, sizeof(result)) == 0);
  }
  return 0;
}

static int test_architecture_labels_are_exact(void) {
  static const char *invalid[] = {"armv7garbage", "armhf", "arm", NULL};
  synthetic_provider fixture;
  nxcompat_probe_provider provider;
  nxcompat_probe_options options;
  nxcompat_probe_result result;
  nxcompat_host host;
  size_t index;

  for (index = 0; invalid[index]; ++index) {
    const nxcompat_observation *observation;
    provider_init(&fixture);
    fixture.values[0] = invalid[index];
    options_init(&options, &provider, &result, &fixture);
    CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_FAILED);
    observation = nxcompat_probe_find_observation(
        &result, NXCOMPAT_OBSERVATION_PROCESS_MACHINE);
    CHECK(observation != NULL);
    CHECK(observation->state == NXCOMPAT_OBSERVATION_REJECTED);
    CHECK(observation->reason == NXCOMPAT_REASON_OBSERVATION_MALFORMED);
    CHECK(host.process_arch == NXCOMPAT_ARCH_UNKNOWN);
  }
  return 0;
}

static int test_invalid_provider_clears_result(void) {
  synthetic_provider fixture;
  nxcompat_probe_provider provider;
  nxcompat_probe_options options;
  nxcompat_probe_result result;
  nxcompat_host host;
  unsigned variant;

  for (variant = 0; variant < 3u; ++variant) {
    provider_init(&fixture);
    options_init(&options, &provider, &result, &fixture);
    if (variant == 0u)
      provider.api_version = NXCOMPAT_API_VERSION_V1;
    else if (variant == 1u)
      provider.struct_size = sizeof(provider) - 1u;
    else
      provider.read = NULL;
    memset(&result, 0xa5, sizeof(result));
    memset(&host, 0xa5, sizeof(host));
    CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_INVALID);
    CHECK(fixture.calls == 0u);
    CHECK(result.api_version == NXCOMPAT_API_VERSION_V2);
    CHECK(result.struct_size == sizeof(result));
    CHECK(result.final_reason == NXCOMPAT_REASON_PROVIDER_CONTRACT);
    CHECK(result.observation_count == 0u);
    CHECK(result.memory_sources == 0u);
    CHECK(result.memory_effective_kib == 0u);
    CHECK(result.observations[0].id == 0);
    CHECK(host.api_version == NXCOMPAT_API_VERSION_V2);
    CHECK(host.struct_size == sizeof(host));
    CHECK(host.capabilities == 0u);
    CHECK(host.process_arch == NXCOMPAT_ARCH_UNKNOWN);
  }
  return 0;
}

static int test_cgroup_observation_consistency(void) {
  synthetic_provider fixture;
  nxcompat_probe_provider provider;
  nxcompat_probe_options options;
  nxcompat_probe_result result;
  nxcompat_host host;

  provider_init(&fixture);
  fixture.values[7] = NULL; /* mode absent but values illegally present */
  options_init(&options, &provider, &result, &fixture);
  CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  CHECK(result.memory_sources == 0u);
  CHECK(result.memory_effective_kib == 0u);
  CHECK(host.memory_class == NXCOMPAT_MEMORY_UNKNOWN);

  provider_init(&fixture);
  fixture.values[1] = NULL; /* kernel width is unknown */
  fixture.values[7] = "1";
  fixture.values[8] = "2147479552";
  fixture.values[9] = "0";
  options_init(&options, &provider, &result, &fixture);
  CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  CHECK(result.kernel_arch == NXCOMPAT_ARCH_UNKNOWN);
  CHECK(result.memory_sources == 0u);
  CHECK(host.memory_class == NXCOMPAT_MEMORY_UNKNOWN);

  provider_init(&fixture);
  fixture.values[1] = "armv7";
  fixture.values[7] = "1";
  fixture.values[8] = "2147479552";
  fixture.values[9] = "0";
  options_init(&options, &provider, &result, &fixture);
  CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  CHECK((result.memory_sources & NXCOMPAT_MEMORY_SOURCE_CGROUP) == 0u);
  CHECK(result.memory_effective_kib == UINT64_C(207200));
  return 0;
}

static int test_default_cgroup_mounts_ancestors_and_hybrid(void) {
  char root[] = "/tmp/nxcompat-m12-cgroup.XXXXXX";
  nxcompat_probe_options options;
  nxcompat_probe_result result;
  nxcompat_host host;
  const nxcompat_observation *limit_observation;
  int status = -1;

  FIXTURE_CHECK(mkdtemp(root) != NULL);
  FIXTURE_CHECK(fixture_write(
                    root, "/proc/self/cgroup",
                    "0::/tenant/group/leaf\n") == 0);
  FIXTURE_CHECK(fixture_write(
                    root, "/proc/self/mountinfo",
                    "29 23 0:26 /tenant /custom/v2 rw,nosuid - cgroup2 "
                    "cgroup rw\n") == 0);
  FIXTURE_CHECK(fixture_write(root, "/proc/meminfo",
                              "MemTotal: 4194304 kB\n"
                              "MemAvailable: 1000000 kB\n"
                              "SwapTotal: 0 kB\n") == 0);
  FIXTURE_CHECK(fixture_write(root, "/proc/self/status",
                              "Name: fixture\nVmSize: 1000 kB\n") == 0);

  /* v2: the controller is delegated only to the parent.  ENOENT at the leaf
   * is not an unreadable constraint; the parent still constrains every
   * descendant. */
  FIXTURE_CHECK(fixture_write(root, "/custom/v2/group/memory.max",
                              "500000\n") == 0);
  FIXTURE_CHECK(fixture_write(root, "/custom/v2/group/memory.current",
                              "400000\n") == 0);
  FIXTURE_CHECK(
      fixture_write(root, "/custom/v2/memory.max", "max\n") == 0);

  memset(&options, 0, sizeof(options));
  options.api_version = NXCOMPAT_API_VERSION_V2;
  options.struct_size = sizeof(options);
  options.port_id = "cgroup-fixture";
  options.game_dir = "/game";
  options.probe_root = root;
  options.result = &result;
  FIXTURE_CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  FIXTURE_CHECK(result.cgroup_version == 2u);
  FIXTURE_CHECK(result.cgroup_limit_kib == UINT64_C(488));
  FIXTURE_CHECK(result.cgroup_current_kib == UINT64_C(390));
  FIXTURE_CHECK((result.memory_sources & NXCOMPAT_MEMORY_SOURCE_CGROUP) != 0u);
  FIXTURE_CHECK(result.memory_effective_kib == UINT64_C(97));
  FIXTURE_CHECK(host.memory_class == NXCOMPAT_MEMORY_SHORT);

  /* v1 has the smaller proven headroom, so hybrid selection must expose its
   * mode and the matching limit/current pair rather than mixing hierarchies. */
  FIXTURE_CHECK(fixture_write(
                    root, "/proc/self/cgroup",
                    "0::/tenant/group/leaf\n5:memory:/legacy/group/leaf\n") ==
                0);
  FIXTURE_CHECK(fixture_write(
                    root, "/proc/self/mountinfo",
                    "29 23 0:26 /tenant /custom/v2 rw,nosuid - cgroup2 "
                    "cgroup rw\n"
                    "30 23 0:27 / /custom/v1 rw,nosuid,memory - cgroup "
                    "cgroup rw,memory\n") == 0);
  FIXTURE_CHECK(fixture_write(
                    root,
                    "/custom/v1/legacy/group/leaf/memory.limit_in_bytes",
                    "200000\n") == 0);
  FIXTURE_CHECK(fixture_write(
                    root,
                    "/custom/v1/legacy/group/leaf/memory.usage_in_bytes",
                    "150000\n") == 0);
  FIXTURE_CHECK(fixture_write(
                    root, "/custom/v1/legacy/group/memory.limit_in_bytes",
                    "9223372036854771712\n") == 0);
  FIXTURE_CHECK(fixture_write(
                    root, "/custom/v1/legacy/memory.limit_in_bytes",
                    "9223372036854771712\n") == 0);
  FIXTURE_CHECK(fixture_write(
                    root, "/custom/v1/memory.limit_in_bytes",
                    "9223372036854771712\n") == 0);

  FIXTURE_CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  FIXTURE_CHECK(result.cgroup_version == 1u);
  FIXTURE_CHECK(result.cgroup_limit_kib == UINT64_C(195));
  FIXTURE_CHECK(result.cgroup_current_kib == UINT64_C(146));
  FIXTURE_CHECK((result.memory_sources & NXCOMPAT_MEMORY_SOURCE_CGROUP) != 0u);
  FIXTURE_CHECK(result.memory_effective_kib == UINT64_C(48));
  FIXTURE_CHECK(host.memory_class == NXCOMPAT_MEMORY_SHORT);

  /* A mounted v2 hierarchy with no memory controller is not an incomplete
   * memory constraint and must not poison the valid v1 memory hierarchy. */
  FIXTURE_CHECK(fixture_unlink(root, "/custom/v2/group/memory.max") == 0);
  FIXTURE_CHECK(
      fixture_unlink(root, "/custom/v2/group/memory.current") == 0);
  FIXTURE_CHECK(fixture_unlink(root, "/custom/v2/memory.max") == 0);
  FIXTURE_CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  FIXTURE_CHECK(result.cgroup_version == 1u);
  FIXTURE_CHECK(result.memory_effective_kib == UINT64_C(48));

  /* In a hybrid hierarchy, an explicit v1 memory membership plus a valid v1
   * mount/pair proves that an absent v2 mount does not own memory. */
  FIXTURE_CHECK(fixture_write(
                    root, "/proc/self/mountinfo",
                    "30 23 0:27 / /custom/v1 rw,nosuid,memory - cgroup "
                    "cgroup rw,memory\n") == 0);
  FIXTURE_CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  FIXTURE_CHECK(result.cgroup_version == 1u);
  FIXTURE_CHECK(result.memory_effective_kib == UINT64_C(48));

  /* The same ownership proof applies when v1 is explicitly unbounded. */
  FIXTURE_CHECK(fixture_write(
                    root,
                    "/custom/v1/legacy/group/leaf/memory.limit_in_bytes",
                    "9223372036854771712\n") == 0);
  FIXTURE_CHECK(fixture_write(root, "/proc/meminfo",
                              "MemTotal: 4194304 kB\n"
                              "MemAvailable: 3000000 kB\n"
                              "SwapTotal: 0 kB\n") == 0);
  FIXTURE_CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  FIXTURE_CHECK(result.cgroup_version == 1u);
  limit_observation = nxcompat_probe_find_observation(
      &result, NXCOMPAT_OBSERVATION_CGROUP_LIMIT_BYTES);
  FIXTURE_CHECK(limit_observation != NULL);
  FIXTURE_CHECK(limit_observation->reason == NXCOMPAT_REASON_LIMIT_UNBOUNDED);
  FIXTURE_CHECK((result.memory_sources & NXCOMPAT_MEMORY_SOURCE_CGROUP) == 0u);
  FIXTURE_CHECK(result.memory_effective_kib > UINT64_C(2097152));
  FIXTURE_CHECK(host.memory_class == NXCOMPAT_MEMORY_HIGH);
  status = 0;

cleanup:
  if (root[0] && strstr(root, "/tmp/nxcompat-m12-cgroup.") == root &&
      fixture_remove_tree(root) != 0)
    status = -1;
  return status;
}

static int test_default_cgroup_namespace_relative_and_incomplete(void) {
  char root[] = "/tmp/nxcompat-m12-namespace.XXXXXX";
  char long_membership[1200];
  nxcompat_probe_options options;
  nxcompat_probe_result result;
  nxcompat_host host;
  int status = -1;

  FIXTURE_CHECK(mkdtemp(root) != NULL);
  FIXTURE_CHECK(fixture_write(root, "/proc/self/cgroup", "0::/\n") == 0);
  FIXTURE_CHECK(fixture_write(
                    root, "/proc/self/mountinfo",
                    "29 23 0:26 /docker/private /relative/cg rw - cgroup2 "
                    "cgroup rw\n") == 0);
  FIXTURE_CHECK(fixture_write(root, "/proc/meminfo",
                              "MemTotal: 4194304 kB\n"
                              "MemAvailable: 999999 kB\n"
                              "SwapTotal: 0 kB\n") == 0);
  FIXTURE_CHECK(fixture_write(root, "/proc/self/status",
                              "Name: fixture\nVmSize: 1000 kB\n") == 0);
  FIXTURE_CHECK(
      fixture_write(root, "/relative/cg/memory.max", "100000\n") == 0);
  FIXTURE_CHECK(
      fixture_write(root, "/relative/cg/memory.current", "90000\n") == 0);

  memset(&options, 0, sizeof(options));
  options.api_version = NXCOMPAT_API_VERSION_V2;
  options.struct_size = sizeof(options);
  options.port_id = "namespace-fixture";
  options.game_dir = "/game";
  options.probe_root = root;
  options.result = &result;
  FIXTURE_CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  FIXTURE_CHECK(result.cgroup_version == 2u);
  FIXTURE_CHECK(result.cgroup_limit_kib == UINT64_C(97));
  FIXTURE_CHECK(result.cgroup_current_kib == UINT64_C(87));
  FIXTURE_CHECK(result.memory_effective_kib == UINT64_C(9));

  /* Once a finite applicable constraint loses its matching usage value, even
   * MemAvailable must not be promoted as effective headroom. */
  FIXTURE_CHECK(fixture_write(root, "/relative/cg/memory.current", "bad\n") ==
                0);
  FIXTURE_CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  FIXTURE_CHECK(result.cgroup_version == 2u);
  FIXTURE_CHECK(result.memory_sources == 0u);
  FIXTURE_CHECK(result.memory_effective_kib == 0u);
  FIXTURE_CHECK(host.memory_class == NXCOMPAT_MEMORY_UNKNOWN);

  /* At a global cgroup2 root, ENOENT means the memory controller is not
   * applicable, while malformed content remains a fail-closed constraint. */
  FIXTURE_CHECK(fixture_unlink(root, "/relative/cg/memory.max") == 0);
  FIXTURE_CHECK(fixture_unlink(root, "/relative/cg/memory.current") == 0);
  FIXTURE_CHECK(fixture_write(
                    root, "/proc/self/mountinfo",
                    "29 23 0:26 / /relative/cg rw - cgroup2 cgroup rw\n") ==
                0);
  FIXTURE_CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  FIXTURE_CHECK(result.cgroup_version == 0u);
  FIXTURE_CHECK((result.memory_sources & NXCOMPAT_MEMORY_SOURCE_CGROUP) == 0u);
  FIXTURE_CHECK(result.memory_effective_kib > 0u);
  FIXTURE_CHECK(host.memory_class != NXCOMPAT_MEMORY_UNKNOWN);

  FIXTURE_CHECK(
      fixture_write(root, "/relative/cg/memory.current", "0\n") == 0);
  FIXTURE_CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  FIXTURE_CHECK(result.cgroup_version == 2u);
  FIXTURE_CHECK(result.memory_sources == 0u);
  FIXTURE_CHECK(host.memory_class == NXCOMPAT_MEMORY_UNKNOWN);

  FIXTURE_CHECK(
      fixture_write(root, "/relative/cg/memory.max", "malformed\n") == 0);
  FIXTURE_CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  FIXTURE_CHECK(result.cgroup_version == 2u);
  FIXTURE_CHECK(result.memory_sources == 0u);
  FIXTURE_CHECK(result.memory_effective_kib == 0u);
  FIXTURE_CHECK(host.memory_class == NXCOMPAT_MEMORY_UNKNOWN);

  /* Membership without a resolvable mount is an unknown applied constraint,
   * not permission to fall back to the otherwise large MemAvailable value. */
  FIXTURE_CHECK(fixture_unlink(root, "/proc/self/mountinfo") == 0);
  FIXTURE_CHECK(fixture_write(root, "/proc/self/cgroup", "0::/group\n") ==
                0);
  FIXTURE_CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  FIXTURE_CHECK(result.cgroup_version == 2u);
  FIXTURE_CHECK(result.memory_sources == 0u);
  FIXTURE_CHECK(host.memory_class == NXCOMPAT_MEMORY_UNKNOWN);

  FIXTURE_CHECK(fixture_write(root, "/proc/self/cgroup",
                              "5:memory:/legacy/group\n") == 0);
  FIXTURE_CHECK(fixture_write(root, "/proc/self/mountinfo",
                              "malformed mountinfo\n") == 0);
  FIXTURE_CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  FIXTURE_CHECK(result.cgroup_version == 1u);
  FIXTURE_CHECK(result.memory_sources == 0u);
  FIXTURE_CHECK(host.memory_class == NXCOMPAT_MEMORY_UNKNOWN);

  memset(long_membership, 'x', sizeof(long_membership));
  memcpy(long_membership, "0::/", 4u);
  long_membership[sizeof(long_membership) - 2u] = '\n';
  long_membership[sizeof(long_membership) - 1u] = '\0';
  FIXTURE_CHECK(fixture_write(root, "/proc/self/cgroup", long_membership) ==
                0);
  FIXTURE_CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  FIXTURE_CHECK(result.cgroup_version == 2u);
  FIXTURE_CHECK(result.memory_sources == 0u);
  FIXTURE_CHECK(host.memory_class == NXCOMPAT_MEMORY_UNKNOWN);
  status = 0;

cleanup:
  if (root[0] && strstr(root, "/tmp/nxcompat-m12-namespace.") == root &&
      fixture_remove_tree(root) != 0)
    status = -1;
  return status;
}

static int test_default_cgroup2_unbounded_root_enoent(void) {
  char root[] = "/tmp/nxcompat-m12-unbounded.XXXXXX";
  nxcompat_probe_options options;
  nxcompat_probe_result result;
  nxcompat_host host;
  const nxcompat_observation *limit_observation;
  int status = -1;

  FIXTURE_CHECK(mkdtemp(root) != NULL);
  FIXTURE_CHECK(fixture_write(root, "/proc/self/cgroup",
                              "0::/group/leaf\n") == 0);
  FIXTURE_CHECK(fixture_write(
                    root, "/proc/self/mountinfo",
                    "29 23 0:26 / /cg rw - cgroup2 cgroup rw\n") == 0);
  FIXTURE_CHECK(fixture_write(root, "/proc/meminfo",
                              "MemTotal: 8388608 kB\n"
                              "MemAvailable: 4194304 kB\n"
                              "SwapTotal: 0 kB\n") == 0);
  FIXTURE_CHECK(fixture_write(root, "/proc/self/status",
                              "Name: fixture\nVmSize: 1000 kB\n") == 0);
  FIXTURE_CHECK(
      fixture_write(root, "/cg/group/leaf/memory.max", "max\n") == 0);
  FIXTURE_CHECK(
      fixture_write(root, "/cg/group/memory.max", "max\n") == 0);

  memset(&options, 0, sizeof(options));
  options.api_version = NXCOMPAT_API_VERSION_V2;
  options.struct_size = sizeof(options);
  options.port_id = "unbounded-fixture";
  options.game_dir = "/game";
  options.probe_root = root;
  options.result = &result;
  FIXTURE_CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  FIXTURE_CHECK(result.cgroup_version == 2u);
  limit_observation = nxcompat_probe_find_observation(
      &result, NXCOMPAT_OBSERVATION_CGROUP_LIMIT_BYTES);
  FIXTURE_CHECK(limit_observation != NULL);
  FIXTURE_CHECK(limit_observation->state == NXCOMPAT_OBSERVATION_PRESENT);
  FIXTURE_CHECK(limit_observation->reason == NXCOMPAT_REASON_LIMIT_UNBOUNDED);
  FIXTURE_CHECK((result.memory_sources &
                 NXCOMPAT_MEMORY_SOURCE_MEMAVAILABLE) != 0u);
  FIXTURE_CHECK((result.memory_sources & NXCOMPAT_MEMORY_SOURCE_CGROUP) == 0u);
  FIXTURE_CHECK(result.memory_effective_kib > UINT64_C(2097152));
  FIXTURE_CHECK(host.memory_class == NXCOMPAT_MEMORY_HIGH);
  status = 0;

cleanup:
  if (root[0] && strstr(root, "/tmp/nxcompat-m12-unbounded.") == root &&
      fixture_remove_tree(root) != 0)
    status = -1;
  return status;
}

static int test_userland_elf_identity_is_exact(void) {
  char root[] = "/tmp/nxcompat-m12-userland.XXXXXX";
  unsigned char header[20];
  nxcompat_probe_options options;
  nxcompat_probe_result result;
  nxcompat_host host;
  const nxcompat_observation *observation;
  int status = -1;

  FIXTURE_CHECK(mkdtemp(root) != NULL);
  FIXTURE_CHECK(fixture_write(root, "/proc/self/cgroup", "") == 0);
  FIXTURE_CHECK(fixture_write(root, "/proc/meminfo",
                              "MemTotal: 2097152 kB\n"
                              "MemAvailable: 1048576 kB\n"
                              "SwapTotal: 0 kB\n") == 0);
  memset(&options, 0, sizeof(options));
  options.api_version = NXCOMPAT_API_VERSION_V2;
  options.struct_size = sizeof(options);
  options.port_id = "elf-identity-fixture";
  options.game_dir = "/game";
  options.probe_root = root;
  options.result = &result;

  memset(header, 0, sizeof(header));
  header[0] = 0x7fu;
  header[1] = 'E';
  header[2] = 'L';
  header[3] = 'F';
  header[4] = 1u; /* ELF32 cannot prove EM_AARCH64. */
  header[5] = 1u;
  header[6] = 1u;
  header[18] = 183u;
  FIXTURE_CHECK(fixture_write_bytes(root, "/proc/self/exe", header,
                                    sizeof(header)) == 0);
  FIXTURE_CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  observation = nxcompat_probe_find_observation(
      &result, NXCOMPAT_OBSERVATION_USERLAND_MACHINE);
  FIXTURE_CHECK(observation != NULL);
  FIXTURE_CHECK(observation->state == NXCOMPAT_OBSERVATION_ABSENT);
  FIXTURE_CHECK(result.userland_arch == NXCOMPAT_ARCH_UNKNOWN);

  header[4] = 2u;
  header[6] = 0u; /* Non-current ELF identity is not verified. */
  FIXTURE_CHECK(fixture_write_bytes(root, "/proc/self/exe", header,
                                    sizeof(header)) == 0);
  FIXTURE_CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  observation = nxcompat_probe_find_observation(
      &result, NXCOMPAT_OBSERVATION_USERLAND_MACHINE);
  FIXTURE_CHECK(observation != NULL);
  FIXTURE_CHECK(observation->state == NXCOMPAT_OBSERVATION_ABSENT);

  header[6] = 1u;
  FIXTURE_CHECK(fixture_write_bytes(root, "/proc/self/exe", header,
                                    sizeof(header)) == 0);
  FIXTURE_CHECK(nxcompat_probe(&options, &host) == NXCOMPAT_OK);
  observation = nxcompat_probe_find_observation(
      &result, NXCOMPAT_OBSERVATION_USERLAND_MACHINE);
  FIXTURE_CHECK(observation != NULL);
  FIXTURE_CHECK(observation->state == NXCOMPAT_OBSERVATION_PRESENT);
  FIXTURE_CHECK(observation->reason ==
                NXCOMPAT_REASON_USERLAND_ARCH_VERIFIED);
  FIXTURE_CHECK(result.userland_arch == NXCOMPAT_ARCH_AARCH64);
  status = 0;

cleanup:
  if (root[0] && strstr(root, "/tmp/nxcompat-m12-userland.") == root &&
      fixture_remove_tree(root) != 0)
    status = -1;
  return status;
}

static int test_api_v1_runtime_prefix(void) {
  struct legacy_call {
    uint64_t before;
    nxcompat_probe_options_v1_layout options;
    uint64_t after;
  } legacy;
  char root[] = "/tmp/nxcompat-m12-v1.XXXXXX";
  char device_line[1024];
  char fix_line[1024];
  char graphics_line[1024];
  nxcompat_graphics_options graphics_options;
  nxcompat_graphics graphics;
  nxcompat_host host;
  nxcompat_plan_options plan_options;
  nxcompat_plan plan;
  int status = -1;

  FIXTURE_CHECK(mkdtemp(root) != NULL);
  FIXTURE_CHECK(fixture_write(root, "/proc/meminfo",
                              "MemTotal: 3145728 kB\n"
                              "MemAvailable: 1234 kB\n"
                              "SwapTotal: 0 kB\n") == 0);
  memset(&legacy, 0, sizeof(legacy));
  legacy.before = UINT64_C(0x1122334455667788);
  legacy.after = UINT64_C(0x8877665544332211);
  legacy.options.api_version = NXCOMPAT_API_VERSION_V1;
  legacy.options.struct_size = sizeof(legacy.options);
  legacy.options.port_id = "legacy-v1";
  legacy.options.game_dir = "/game";
  legacy.options.probe_root = root;
  FIXTURE_CHECK(nxcompat_probe((const nxcompat_probe_options *)&legacy.options,
                               &host) == NXCOMPAT_OK);
  FIXTURE_CHECK(legacy.before == UINT64_C(0x1122334455667788));
  FIXTURE_CHECK(legacy.after == UINT64_C(0x8877665544332211));
  FIXTURE_CHECK(host.api_version == NXCOMPAT_API_VERSION_V1);
  FIXTURE_CHECK(host.memory_total_kib == UINT64_C(3145728));
  FIXTURE_CHECK(host.memory_class == NXCOMPAT_MEMORY_HIGH);
  memset(&plan_options, 0, sizeof(plan_options));
  plan_options.api_version = NXCOMPAT_API_VERSION_V1;
  plan_options.struct_size = sizeof(plan_options);
  plan_options.runtime_arch = NXCOMPAT_ARCH_UNKNOWN;
  FIXTURE_CHECK(nxcompat_plan_environment(&host, &plan_options, &plan) == 0);
  FIXTURE_CHECK(plan.api_version == NXCOMPAT_API_VERSION_V1);
  FIXTURE_CHECK(nxcompat_format_device_line(&host, device_line,
                                             sizeof(device_line)) > 0);
  FIXTURE_CHECK(nxcompat_format_fix_line(&plan, fix_line,
                                          sizeof(fix_line)) > 0);
  FIXTURE_CHECK(nxcompat_apply_environment(&plan) == 0);
  FIXTURE_CHECK(plan.api_version == NXCOMPAT_API_VERSION_V1);
  memset(&graphics_options, 0, sizeof(graphics_options));
  graphics_options.api_version = NXCOMPAT_API_VERSION_V1;
  graphics_options.struct_size = sizeof(graphics_options);
  graphics_options.video_driver = "legacy-fixture";
  graphics_options.version = "OpenGL ES 2.0 legacy";
  graphics_options.drawable_width = 320;
  graphics_options.drawable_height = 240;
  FIXTURE_CHECK(nxcompat_capture_graphics(&graphics_options, &graphics) == 0);
  FIXTURE_CHECK(graphics.api_version == NXCOMPAT_API_VERSION_V1);
  FIXTURE_CHECK((graphics.capabilities & NXCOMPAT_GRAPHICS_GLES2) != 0u);
  FIXTURE_CHECK(nxcompat_format_graphics_line(
                    &graphics, graphics_line, sizeof(graphics_line)) > 0);
  status = 0;

cleanup:
  if (root[0] && strstr(root, "/tmp/nxcompat-m12-v1.") == root &&
      fixture_remove_tree(root) != 0)
    status = -1;
  return status;
}

int main(void) {
  CHECK(test_hermetic_effective_memory() == 0);
  CHECK(test_provider_contract_and_unbounded_limits() == 0);
  CHECK(test_exhausted_limits_are_zero() == 0);
  CHECK(test_context_labels_do_not_change_capabilities() == 0);
  CHECK(test_arbiter_reentry_is_busy() == 0);
  CHECK(test_limits_without_usage_do_not_claim_headroom() == 0);
  CHECK(test_probe_root_is_fail_closed() == 0);
  CHECK(test_architecture_labels_are_exact() == 0);
  CHECK(test_invalid_provider_clears_result() == 0);
  CHECK(test_cgroup_observation_consistency() == 0);
  CHECK(test_default_cgroup_mounts_ancestors_and_hybrid() == 0);
  CHECK(test_default_cgroup_namespace_relative_and_incomplete() == 0);
  CHECK(test_default_cgroup2_unbounded_root_enoent() == 0);
  CHECK(test_userland_elf_identity_is_exact() == 0);
  CHECK(test_api_v1_runtime_prefix() == 0);
  puts("nxcompat M12 probe: hermetic provider and effective memory passed");
  return 0;
}
