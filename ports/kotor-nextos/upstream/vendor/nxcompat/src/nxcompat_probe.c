/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "nxcompat_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

static volatile int nxcompat_global_arbiter;

nxcompat_result_code nxcompat_global_arbiter_try_acquire(void) {
#if defined(__GNUC__) || defined(__clang__)
  return __sync_lock_test_and_set(&nxcompat_global_arbiter, 1)
             ? NXCOMPAT_BUSY
             : NXCOMPAT_OK;
#else
#error "nxcompat requires GCC/Clang atomic builtins for its global arbiter"
#endif
}

void nxcompat_global_arbiter_release(void) {
#if defined(__GNUC__) || defined(__clang__)
  __sync_lock_release(&nxcompat_global_arbiter);
#endif
}

int nxcompat_api_version_supported(uint32_t api_version) {
  return api_version == NXCOMPAT_API_VERSION_V1 ||
         api_version == NXCOMPAT_API_VERSION_V2;
}

static int nxcompat_probe_root_valid(const char *root) {
  size_t length;
  size_t component_start;
  size_t index;
  if (!root)
    return 1;
  length = strnlen(root, NXCOMPAT_PATH_MAX);
  if (length == 0u || length >= NXCOMPAT_PATH_MAX || root[0] != '/')
    return 0;
  component_start = 1u;
  for (index = 1u; index <= length; ++index) {
    if (index == length || root[index] == '/') {
      size_t component_length = index - component_start;
      if ((component_length == 1u && root[component_start] == '.') ||
          (component_length == 2u && root[component_start] == '.' &&
           root[component_start + 1u] == '.'))
        return 0;
      component_start = index + 1u;
    }
  }
  return 1;
}

static int nxcompat_options_valid(const nxcompat_probe_options *options) {
  size_t minimum;
  if (!options || !nxcompat_api_version_supported(options->api_version))
    return 0;
  minimum = options->api_version == NXCOMPAT_API_VERSION_V1
                ? NXCOMPAT_PROBE_OPTIONS_V1_SIZE
                : sizeof(*options);
  if (options->struct_size < minimum)
    return 0;
  /* A non-NULL root is an explicit filesystem boundary.  Never turn a typo,
   * an empty string, or a truncated path into an implicit probe of the live
   * host root. */
  if (!nxcompat_probe_root_valid(options->probe_root))
    return 0;
  return 1;
}

static void nxcompat_context_init(const nxcompat_probe_options *options,
                                  nxcompat_context *context) {
  size_t length;
  const char *root = options->probe_root;
  if (!root)
    root = "/";
  nxcompat_copy_string(context->root, sizeof(context->root), root);
  length = strlen(context->root);
  while (length > 1 && context->root[length - 1] == '/') {
    context->root[length - 1] = '\0';
    --length;
  }
}

static nxcompat_arch nxcompat_compiled_arch(void) {
#if defined(__aarch64__)
  return NXCOMPAT_ARCH_AARCH64;
#elif defined(__arm__)
#if defined(__ARM_ARCH) && __ARM_ARCH >= 7
  return NXCOMPAT_ARCH_ARMV7;
#else
  /* __arm__ proves 32-bit ARM, not ARMv7. */
  return NXCOMPAT_ARCH_UNKNOWN;
#endif
#elif defined(__i386__)
  return NXCOMPAT_ARCH_I386;
#elif defined(__x86_64__)
  return NXCOMPAT_ARCH_X86_64;
#else
  return NXCOMPAT_ARCH_UNKNOWN;
#endif
}

static nxcompat_arch nxcompat_parse_arch(const char *machine) {
  if (!machine)
    return NXCOMPAT_ARCH_UNKNOWN;
  if (strcmp(machine, "aarch64") == 0 || strcmp(machine, "arm64") == 0)
    return NXCOMPAT_ARCH_AARCH64;
  if (strcmp(machine, "armv7") == 0 || strcmp(machine, "armv7l") == 0 ||
      strcmp(machine, "armv7-a") == 0 || strcmp(machine, "armv8l") == 0)
    return NXCOMPAT_ARCH_ARMV7;
  if (strcmp(machine, "x86_64") == 0 || strcmp(machine, "amd64") == 0)
    return NXCOMPAT_ARCH_X86_64;
  if (strcmp(machine, "x86") == 0 || strcmp(machine, "i386") == 0 ||
      strcmp(machine, "i486") == 0 || strcmp(machine, "i586") == 0 ||
      strcmp(machine, "i686") == 0)
    return NXCOMPAT_ARCH_I386;
  return NXCOMPAT_ARCH_UNKNOWN;
}

typedef struct nxcompat_default_provider_state {
  nxcompat_context context;
  int cgroup_cached;
  unsigned cgroup_mode;
  int cgroup_limit_present;
  int cgroup_current_present;
  uint64_t cgroup_limit_bytes;
  uint64_t cgroup_current_bytes;
} nxcompat_default_provider_state;

static void nxcompat_decode_mount_field(char *value);
static int nxcompat_path_has_prefix(const char *path, const char *prefix);

static int nxcompat_parse_u64_decimal(const char *text, uint64_t *value) {
  const unsigned char *cursor = (const unsigned char *)text;
  uint64_t parsed = 0;
  int digits = 0;
  if (!text || !value)
    return -1;
  while (*cursor && isspace(*cursor))
    ++cursor;
  while (*cursor >= '0' && *cursor <= '9') {
    unsigned digit = (unsigned)(*cursor - '0');
    if (parsed > (UINT64_MAX - digit) / UINT64_C(10))
      return -1;
    parsed = parsed * UINT64_C(10) + digit;
    ++cursor;
    digits = 1;
  }
  while (*cursor && isspace(*cursor))
    ++cursor;
  if (!digits || *cursor)
    return -1;
  *value = parsed;
  return 0;
}

static int nxcompat_read_key_kib(const nxcompat_context *context,
                                 const char *path, const char *key,
                                 char *value, size_t value_size) {
  char host_path[NXCOMPAT_PATH_MAX * 2u];
  char line[256];
  FILE *stream;
  size_t key_length = strlen(key);
  if (nxcompat_join_root(context, path, host_path, sizeof(host_path)) != 0)
    return -1;
  stream = fopen(host_path, "r");
  if (!stream)
    return -1;
  while (fgets(line, sizeof(line), stream)) {
    char *cursor;
    char *end;
    if (strncmp(line, key, key_length) != 0 || line[key_length] != ':')
      continue;
    cursor = line + key_length + 1u;
    while (*cursor && isspace((unsigned char)*cursor))
      ++cursor;
    end = cursor;
    while (*end >= '0' && *end <= '9')
      ++end;
    if (end == cursor ||
        (*end && !isspace((unsigned char)*end))) {
      (void)fclose(stream);
      return -1;
    }
    *end = '\0';
    nxcompat_copy_string(value, value_size, cursor);
    (void)fclose(stream);
    return 0;
  }
  (void)fclose(stream);
  return -1;
}

static int nxcompat_copy_bounded(char *destination, size_t destination_size,
                                 const char *source) {
  size_t length;
  if (!destination || destination_size == 0u || !source)
    return -1;
  length = strnlen(source, destination_size);
  if (length >= destination_size)
    return -1;
  memcpy(destination, source, length + 1u);
  return 0;
}

static int nxcompat_cgroup_list_has(const char *list, const char *wanted) {
  const char *cursor = list;
  size_t wanted_length = strlen(wanted);
  if (!list || !wanted || wanted_length == 0u)
    return 0;
  while (*cursor) {
    const char *end = strchr(cursor, ',');
    size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
    if (length == wanted_length &&
        memcmp(cursor, wanted, wanted_length) == 0)
      return 1;
    if (!end)
      break;
    cursor = end + 1;
  }
  return 0;
}

static int nxcompat_default_cgroup_membership(
    const nxcompat_context *context, unsigned requested_mode, char *membership,
    size_t membership_size) {
  char host_path[NXCOMPAT_PATH_MAX * 2u];
  char line[1024];
  FILE *stream;
  if (nxcompat_join_root(context, "/proc/self/cgroup", host_path,
                         sizeof(host_path)) != 0)
    return -1;
  stream = fopen(host_path, "r");
  if (!stream)
    return -1;
  while (fgets(line, sizeof(line), stream)) {
    char *first = strchr(line, ':');
    char *second;
    char *controllers;
    char *path;
    if (!strchr(line, '\n') && !feof(stream)) {
      int byte;
      while ((byte = fgetc(stream)) != '\n' && byte != EOF)
        ;
      (void)fclose(stream);
      return -1;
    }
    if (!first) {
      (void)fclose(stream);
      return -1;
    }
    second = strchr(first + 1, ':');
    if (!second) {
      (void)fclose(stream);
      return -1;
    }
    *first = '\0';
    *second = '\0';
    controllers = first + 1;
    path = second + 1;
    path[strcspn(path, "\r\n")] = '\0';
    if (requested_mode == 2u && line[0] == '0' && line[1] == '\0') {
      if (controllers[0] != '\0' || path[0] != '/') {
        (void)fclose(stream);
        return -1;
      }
      if (nxcompat_copy_bounded(membership, membership_size, path) != 0) {
        (void)fclose(stream);
        return -1;
      }
      (void)fclose(stream);
      return 0;
    }
    if (requested_mode == 1u &&
        nxcompat_cgroup_list_has(controllers, "memory")) {
      if (path[0] != '/' ||
          nxcompat_copy_bounded(membership, membership_size, path) != 0) {
        (void)fclose(stream);
        return -1;
      }
      (void)fclose(stream);
      return 0;
    }
  }
  if (ferror(stream)) {
    (void)fclose(stream);
    return -1;
  }
  (void)fclose(stream);
  return 1;
}

static int nxcompat_cgroup_mount(const nxcompat_context *context,
                                 unsigned mode, const char *membership,
                                 char *mountpoint, size_t mountpoint_size,
                                 char *mount_root, size_t mount_root_size,
                                 int *global_root) {
  char host_path[NXCOMPAT_PATH_MAX * 2u];
  char line[4096];
  char selected_mount[NXCOMPAT_PATH_MAX] = "";
  char selected_root[NXCOMPAT_PATH_MAX] = "";
  size_t selected_root_length = 0u;
  int selected_absolute = 0;
  FILE *stream;
  if (nxcompat_join_root(context, "/proc/self/mountinfo", host_path,
                         sizeof(host_path)) != 0)
    return -1;
  stream = fopen(host_path, "r");
  if (!stream)
    return -1;
  while (fgets(line, sizeof(line), stream)) {
    char *tokens[128];
    char *token;
    char *save = NULL;
    size_t count = 0u;
    size_t separator = 0u;
    char root[NXCOMPAT_PATH_MAX];
    char mounted_at[NXCOMPAT_PATH_MAX];
    int matches;
    int absolute_mapping;
    if (!strchr(line, '\n') && !feof(stream)) {
      int byte;
      while ((byte = fgetc(stream)) != '\n' && byte != EOF)
        ;
      if (byte == EOF)
        break;
      continue;
    }
    line[strcspn(line, "\r\n")] = '\0';
    for (token = strtok_r(line, " ", &save); token && count < 128u;
         token = strtok_r(NULL, " ", &save))
      tokens[count++] = token;
    if (token || count < 10u)
      continue;
    for (separator = 6u; separator < count; ++separator)
      if (strcmp(tokens[separator], "-") == 0)
        break;
    if (separator >= count || separator + 3u >= count)
      continue;
    matches = mode == 2u
                  ? strcmp(tokens[separator + 1u], "cgroup2") == 0
                  : strcmp(tokens[separator + 1u], "cgroup") == 0 &&
                        (nxcompat_cgroup_list_has(tokens[5], "memory") ||
                         nxcompat_cgroup_list_has(tokens[separator + 3u],
                                                  "memory"));
    if (!matches ||
        nxcompat_copy_bounded(root, sizeof(root), tokens[3]) != 0 ||
        nxcompat_copy_bounded(mounted_at, sizeof(mounted_at), tokens[4]) != 0)
      continue;
    nxcompat_decode_mount_field(root);
    nxcompat_decode_mount_field(mounted_at);
    if (root[0] != '/' || mounted_at[0] != '/')
      continue;
    absolute_mapping = nxcompat_path_has_prefix(membership, root);
    if ((selected_absolute && !absolute_mapping) ||
        (selected_absolute == absolute_mapping && selected_mount[0] &&
         strlen(root) < selected_root_length))
      continue;
    if (nxcompat_copy_bounded(selected_root, sizeof(selected_root),
                              absolute_mapping ? root : "/") != 0 ||
        nxcompat_copy_bounded(selected_mount, sizeof(selected_mount),
                              mounted_at) != 0)
      continue;
    selected_absolute = absolute_mapping;
    selected_root_length = strlen(root);
  }
  (void)fclose(stream);
  if (!selected_mount[0])
    return -1;
  if (nxcompat_copy_bounded(mountpoint, mountpoint_size, selected_mount) != 0 ||
      nxcompat_copy_bounded(mount_root, mount_root_size, selected_root) != 0)
    return -1;
  if (global_root)
    *global_root = selected_absolute && strcmp(selected_root, "/") == 0;
  return 0;
}

static int nxcompat_cgroup_directory(const char *mountpoint,
                                     const char *mount_root,
                                     const char *membership, char *directory,
                                     size_t directory_size) {
  const char *relative;
  size_t length;
  int status;
  if (strcmp(mount_root, "/") == 0)
    relative = membership;
  else {
    relative = membership + strlen(mount_root);
    if (!*relative)
      relative = "/";
  }
  status = nxcompat_join_path(mountpoint, relative, directory, directory_size);
  if (status != 0)
    return status;
  length = strlen(directory);
  while (length > 1u && directory[length - 1u] == '/')
    directory[--length] = '\0';
  return 0;
}

static int nxcompat_cgroup_read_value(const nxcompat_context *context,
                                      const char *directory,
                                      const char *leaf, unsigned mode,
                                      int is_limit, nxcompat_arch kernel_arch,
                                      uint64_t *value, int *finite) {
  char path[NXCOMPAT_PATH_MAX * 2u];
  char host_path[NXCOMPAT_PATH_MAX * 2u];
  char text[128];
  struct stat status;
  if (snprintf(path, sizeof(path), "%s/%s", directory, leaf) >=
      (int)sizeof(path))
    return -1;
  if (nxcompat_join_root(context, path, host_path, sizeof(host_path)) != 0)
    return -1;
  if (stat(host_path, &status) != 0)
    return errno == ENOENT || errno == ENOTDIR ? 1 : -1;
  if (!S_ISREG(status.st_mode))
    return -1;
  if (nxcompat_read_first_line(context, path, text, sizeof(text)) != 0)
    return -1;
  if (is_limit && mode == 2u && strcmp(text, "max") == 0) {
    *value = UINT64_MAX;
    *finite = 0;
    return 0;
  }
  if (nxcompat_parse_u64_decimal(text, value) != 0)
    return -1;
  if (is_limit && mode == 1u) {
    if ((kernel_arch == NXCOMPAT_ARCH_ARMV7 ||
         kernel_arch == NXCOMPAT_ARCH_I386) &&
        *value >= UINT64_C(0x7ffff000)) {
      *finite = 0;
      return 0;
    }
    if ((kernel_arch == NXCOMPAT_ARCH_AARCH64 ||
         kernel_arch == NXCOMPAT_ARCH_X86_64) &&
        *value >= (UINT64_C(1) << 60)) {
      *finite = 0;
      return 0;
    }
  }
  *finite = 1;
  return 0;
}

static int nxcompat_cgroup_parent(char *directory, const char *mountpoint) {
  char *slash;
  size_t mount_length;
  if (strcmp(directory, mountpoint) == 0)
    return 0;
  mount_length = strlen(mountpoint);
  slash = strrchr(directory, '/');
  if (!slash)
    return -1;
  if (strcmp(mountpoint, "/") == 0) {
    if (slash == directory)
      directory[1] = '\0';
    else
      *slash = '\0';
    return 1;
  }
  if ((size_t)(slash - directory) < mount_length)
    return -1;
  if (slash == directory)
    slash[1] = '\0';
  else
    *slash = '\0';
  return nxcompat_path_has_prefix(directory, mountpoint) ? 1 : -1;
}

typedef struct nxcompat_cgroup_candidate {
  unsigned mode;
  int mounted;
  int mount_unresolved;
  int incomplete;
  int missing_limit;
  int limit_observed;
  int all_limits_unbounded;
  int incomplete_limit_present;
  uint64_t incomplete_limit;
  int finite_pair;
  uint64_t limit;
  uint64_t current;
  uint64_t headroom;
} nxcompat_cgroup_candidate;

static void nxcompat_cgroup_candidate_collect(
    const nxcompat_context *context, unsigned mode,
    nxcompat_arch kernel_arch, nxcompat_cgroup_candidate *candidate) {
  char membership[NXCOMPAT_PATH_MAX];
  char mountpoint[NXCOMPAT_PATH_MAX];
  char mount_root[NXCOMPAT_PATH_MAX];
  char directory[NXCOMPAT_PATH_MAX * 2u];
  int global_root = 0;
  int membership_status;
  const char *limit_leaf =
      mode == 2u ? "memory.max" : "memory.limit_in_bytes";
  const char *current_leaf =
      mode == 2u ? "memory.current" : "memory.usage_in_bytes";
  memset(candidate, 0, sizeof(*candidate));
  candidate->mode = mode;
  candidate->all_limits_unbounded = 1;
  membership_status = nxcompat_default_cgroup_membership(
      context, mode, membership, sizeof(membership));
  if (membership_status == 1)
    return;
  /* Once the kernel reports membership, losing the matching mount or being
   * unable to map it is unknown constraint state, never proof of absence. */
  candidate->mounted = 1;
  if (membership_status != 0) {
    candidate->incomplete = 1;
    return;
  }
  if (nxcompat_cgroup_mount(context, mode, membership, mountpoint,
                            sizeof(mountpoint), mount_root,
                            sizeof(mount_root), &global_root) != 0) {
    candidate->mount_unresolved = 1;
    candidate->incomplete = 1;
    return;
  }
  if (nxcompat_cgroup_directory(mountpoint, mount_root, membership, directory,
                                sizeof(directory)) != 0 ||
      !nxcompat_path_has_prefix(directory, mountpoint)) {
    candidate->mount_unresolved = 1;
    candidate->incomplete = 1;
    return;
  }
  for (;;) {
    uint64_t limit;
    uint64_t current;
    uint64_t headroom;
    int finite;
    int limit_status;
    int parent_status;
    limit_status = nxcompat_cgroup_read_value(
        context, directory, limit_leaf, mode, 1, kernel_arch, &limit, &finite);
    if (limit_status != 0) {
      /* ENOENT in cgroup v2 means that the memory controller is not delegated
       * at this level; keep walking toward the mount root.  Malformed or
       * unreadable values remain fail-closed at every level, including the
       * global root.  Cgroup v1 missing values are made incomplete below. */
      if (limit_status == 1) {
        candidate->missing_limit = 1;
        if (mode == 2u) {
          int current_finite;
          int current_status = nxcompat_cgroup_read_value(
              context, directory, current_leaf, mode, 0, kernel_arch,
              &current, &current_finite);
          /* A visible/malformed usage file without its matching limit is a
           * partial view, not evidence that the controller is absent. */
          if (current_status != 1)
            candidate->incomplete = 1;
        }
        if (mode == 1u || candidate->incomplete)
          candidate->all_limits_unbounded = 0;
      } else {
        candidate->incomplete = 1;
        candidate->all_limits_unbounded = 0;
      }
    } else {
      candidate->limit_observed = 1;
      if (finite) {
        int current_finite;
        candidate->all_limits_unbounded = 0;
        if (nxcompat_cgroup_read_value(context, directory, current_leaf, mode,
                                       0, kernel_arch, &current,
                                       &current_finite) != 0 ||
            !current_finite) {
          candidate->incomplete = 1;
          if (!candidate->incomplete_limit_present ||
              limit < candidate->incomplete_limit) {
            candidate->incomplete_limit_present = 1;
            candidate->incomplete_limit = limit;
          }
        } else {
          headroom = current >= limit ? 0u : limit - current;
          if (!candidate->finite_pair || headroom < candidate->headroom) {
            candidate->finite_pair = 1;
            candidate->limit = limit;
            candidate->current = current;
            candidate->headroom = headroom;
          }
        }
      }
    }
    parent_status = nxcompat_cgroup_parent(directory, mountpoint);
    if (parent_status < 0) {
      candidate->incomplete = 1;
      break;
    }
    if (parent_status == 0)
      break;
  }
  if (mode == 2u && !candidate->limit_observed &&
      candidate->missing_limit && !candidate->incomplete) {
    /* The hierarchy exists but the memory controller is not applicable. */
    candidate->mounted = 0;
    candidate->missing_limit = 0;
  } else if (mode == 1u && candidate->missing_limit) {
    candidate->incomplete = 1;
  }
}

static void
nxcompat_default_cgroup_cache(nxcompat_default_provider_state *state) {
  nxcompat_cgroup_candidate v2;
  nxcompat_cgroup_candidate v1;
  const nxcompat_cgroup_candidate *selected = NULL;
  int v1_memory_proven;
  nxcompat_arch kernel_arch = NXCOMPAT_ARCH_UNKNOWN;
  struct utsname uts;
  if (state->cgroup_cached)
    return;
  state->cgroup_cached = 1;
  if (uname(&uts) == 0)
    kernel_arch = nxcompat_parse_arch(uts.machine);
  nxcompat_cgroup_candidate_collect(&state->context, 2u, kernel_arch, &v2);
  nxcompat_cgroup_candidate_collect(&state->context, 1u, kernel_arch, &v1);
  v1_memory_proven =
      !v1.incomplete &&
      (v1.finite_pair || (v1.limit_observed && v1.all_limits_unbounded));
  /* A mounted hierarchy with an unreadable applicable constraint makes the
   * effective headroom unknown even if another hierarchy is readable. */
  if (v2.mounted && v2.incomplete &&
      !(v2.mount_unresolved && v1_memory_proven))
    selected = &v2;
  else if (v1.mounted && v1.incomplete)
    selected = &v1;
  else if (v2.mount_unresolved && v1_memory_proven)
    selected = &v1;
  else if (v2.finite_pair)
    selected = &v2;
  if (!selected && v1.finite_pair)
    selected = &v1;
  else if (selected && !selected->incomplete && v1.finite_pair &&
           v1.headroom < selected->headroom)
    selected = &v1;
  if (!selected && v2.mounted)
    selected = &v2;
  if (!selected && v1.mounted)
    selected = &v1;
  if (!selected)
    return;
  state->cgroup_mode = selected->mode;
  if (selected->incomplete) {
    if (selected->incomplete_limit_present) {
      state->cgroup_limit_present = 1;
      state->cgroup_limit_bytes = selected->incomplete_limit;
    }
  } else if (selected->finite_pair) {
    state->cgroup_limit_present = 1;
    state->cgroup_current_present = 1;
    state->cgroup_limit_bytes = selected->limit;
    state->cgroup_current_bytes = selected->current;
  } else if (selected->limit_observed && selected->all_limits_unbounded) {
    state->cgroup_limit_present = 1;
    state->cgroup_limit_bytes = UINT64_MAX;
  }
}

static nxcompat_arch
nxcompat_default_userland_arch(const nxcompat_context *context) {
  unsigned char header[20];
  char host_path[NXCOMPAT_PATH_MAX * 2u];
  FILE *stream;
  uint16_t machine;
  if (nxcompat_join_root(context, "/proc/self/exe", host_path,
                         sizeof(host_path)) != 0)
    return NXCOMPAT_ARCH_UNKNOWN;
  stream = fopen(host_path, "rb");
  if (!stream)
    return NXCOMPAT_ARCH_UNKNOWN;
  if (fread(header, 1, sizeof(header), stream) != sizeof(header)) {
    (void)fclose(stream);
    return NXCOMPAT_ARCH_UNKNOWN;
  }
  (void)fclose(stream);
  if (header[0] != 0x7fu || header[1] != 'E' || header[2] != 'L' ||
      header[3] != 'F' || (header[4] != 1u && header[4] != 2u) ||
      (header[5] != 1u && header[5] != 2u) || header[6] != 1u)
    return NXCOMPAT_ARCH_UNKNOWN;
  machine = header[5] == 1u
                ? (uint16_t)((uint16_t)header[18] |
                             ((uint16_t)header[19] << 8))
                : (uint16_t)(((uint16_t)header[18] << 8) |
                             (uint16_t)header[19]);
  switch (machine) {
  case 3u:
    return header[4] == 1u ? NXCOMPAT_ARCH_I386 : NXCOMPAT_ARCH_UNKNOWN;
  case 40u:
    /* ELF e_machine only proves 32-bit ARM.  The compiled ISA is sufficient
     * for the live /proc/self/exe, but an offline ELF needs richer evidence
     * than this parser currently accepts. */
    return header[4] == 1u && strcmp(context->root, "/") == 0 &&
                   nxcompat_compiled_arch() == NXCOMPAT_ARCH_ARMV7
               ? NXCOMPAT_ARCH_ARMV7
               : NXCOMPAT_ARCH_UNKNOWN;
  case 62u:
    return header[4] == 2u ? NXCOMPAT_ARCH_X86_64
                           : NXCOMPAT_ARCH_UNKNOWN;
  case 183u:
    return header[4] == 2u ? NXCOMPAT_ARCH_AARCH64
                           : NXCOMPAT_ARCH_UNKNOWN;
  default:
    return NXCOMPAT_ARCH_UNKNOWN;
  }
}

static nxcompat_provider_result nxcompat_default_read_observation(
    void *userdata, nxcompat_observation_id id, char *value,
    size_t value_size) {
  nxcompat_default_provider_state *state =
      (nxcompat_default_provider_state *)userdata;
  struct utsname uts;
  struct rlimit limit;
  nxcompat_arch arch;
  if (!state || !value || value_size == 0)
    return NXCOMPAT_PROVIDER_ERROR;
  value[0] = '\0';
  switch (id) {
  case NXCOMPAT_OBSERVATION_PROCESS_MACHINE:
    nxcompat_copy_string(value, value_size,
                         nxcompat_arch_name(nxcompat_compiled_arch()));
    return NXCOMPAT_PROVIDER_VALUE;
  case NXCOMPAT_OBSERVATION_KERNEL_MACHINE:
    if (uname(&uts) != 0)
      return NXCOMPAT_PROVIDER_ABSENT;
    nxcompat_copy_string(value, value_size, uts.machine);
    return NXCOMPAT_PROVIDER_VALUE;
  case NXCOMPAT_OBSERVATION_USERLAND_MACHINE:
    arch = nxcompat_default_userland_arch(&state->context);
    if (arch == NXCOMPAT_ARCH_UNKNOWN)
      return NXCOMPAT_PROVIDER_ABSENT;
    nxcompat_copy_string(value, value_size, nxcompat_arch_name(arch));
    return NXCOMPAT_PROVIDER_VALUE;
  case NXCOMPAT_OBSERVATION_MEM_TOTAL_KIB:
    return nxcompat_read_key_kib(&state->context, "/proc/meminfo", "MemTotal",
                                 value, value_size) == 0
               ? NXCOMPAT_PROVIDER_VALUE
               : NXCOMPAT_PROVIDER_ABSENT;
  case NXCOMPAT_OBSERVATION_MEM_AVAILABLE_KIB:
    return nxcompat_read_key_kib(&state->context, "/proc/meminfo",
                                 "MemAvailable", value, value_size) == 0
               ? NXCOMPAT_PROVIDER_VALUE
               : NXCOMPAT_PROVIDER_ABSENT;
  case NXCOMPAT_OBSERVATION_SWAP_TOTAL_KIB:
    return nxcompat_read_key_kib(&state->context, "/proc/meminfo", "SwapTotal",
                                 value, value_size) == 0
               ? NXCOMPAT_PROVIDER_VALUE
               : NXCOMPAT_PROVIDER_ABSENT;
  case NXCOMPAT_OBSERVATION_VM_SIZE_KIB:
    return nxcompat_read_key_kib(&state->context, "/proc/self/status", "VmSize",
                                 value, value_size) == 0
               ? NXCOMPAT_PROVIDER_VALUE
               : NXCOMPAT_PROVIDER_ABSENT;
  case NXCOMPAT_OBSERVATION_CGROUP_MODE:
    nxcompat_default_cgroup_cache(state);
    if (state->cgroup_mode == 0u)
      return NXCOMPAT_PROVIDER_ABSENT;
    (void)snprintf(value, value_size, "%u", state->cgroup_mode);
    return NXCOMPAT_PROVIDER_VALUE;
  case NXCOMPAT_OBSERVATION_CGROUP_LIMIT_BYTES:
    nxcompat_default_cgroup_cache(state);
    if (!state->cgroup_limit_present)
      return NXCOMPAT_PROVIDER_ABSENT;
    if (state->cgroup_limit_bytes == UINT64_MAX)
      nxcompat_copy_string(value, value_size, "max");
    else
      (void)snprintf(value, value_size, "%llu",
                     (unsigned long long)state->cgroup_limit_bytes);
    return NXCOMPAT_PROVIDER_VALUE;
  case NXCOMPAT_OBSERVATION_CGROUP_CURRENT_BYTES:
    nxcompat_default_cgroup_cache(state);
    if (!state->cgroup_current_present)
      return NXCOMPAT_PROVIDER_ABSENT;
    (void)snprintf(value, value_size, "%llu",
                   (unsigned long long)state->cgroup_current_bytes);
    return NXCOMPAT_PROVIDER_VALUE;
  case NXCOMPAT_OBSERVATION_RLIMIT_AS_BYTES:
    if (getrlimit(RLIMIT_AS, &limit) != 0)
      return NXCOMPAT_PROVIDER_ABSENT;
    if (limit.rlim_cur == RLIM_INFINITY)
      nxcompat_copy_string(value, value_size, "max");
    else
      (void)snprintf(value, value_size, "%llu",
                     (unsigned long long)limit.rlim_cur);
    return NXCOMPAT_PROVIDER_VALUE;
  default:
    return NXCOMPAT_PROVIDER_ERROR;
  }
}

static nxcompat_observation *nxcompat_add_observation(
    nxcompat_probe_result *result, nxcompat_observation_id id) {
  nxcompat_observation *observation;
  if (!result || result->observation_count >= NXCOMPAT_MAX_OBSERVATIONS)
    return NULL;
  observation = &result->observations[result->observation_count++];
  memset(observation, 0, sizeof(*observation));
  observation->id = id;
  observation->state = NXCOMPAT_OBSERVATION_ABSENT;
  observation->reason = NXCOMPAT_REASON_OBSERVATION_ABSENT;
  observation->arch_value = NXCOMPAT_ARCH_UNKNOWN;
  return observation;
}

static int nxcompat_observation_is_machine(nxcompat_observation_id id) {
  return id == NXCOMPAT_OBSERVATION_PROCESS_MACHINE ||
         id == NXCOMPAT_OBSERVATION_KERNEL_MACHINE ||
         id == NXCOMPAT_OBSERVATION_USERLAND_MACHINE;
}

static nxcompat_reason_code
nxcompat_machine_reason(nxcompat_observation_id id) {
  if (id == NXCOMPAT_OBSERVATION_PROCESS_MACHINE)
    return NXCOMPAT_REASON_PROCESS_ARCH_VERIFIED;
  if (id == NXCOMPAT_OBSERVATION_KERNEL_MACHINE)
    return NXCOMPAT_REASON_KERNEL_ARCH_VERIFIED;
  return NXCOMPAT_REASON_USERLAND_ARCH_VERIFIED;
}

static nxcompat_observation *nxcompat_collect_observation(
    const nxcompat_probe_provider *provider, nxcompat_probe_result *result,
    nxcompat_observation_id id) {
  char value[128];
  nxcompat_provider_result provider_result;
  nxcompat_observation *observation = nxcompat_add_observation(result, id);
  uint64_t numeric = 0;
  if (!observation)
    return NULL;
  memset(value, 0xa5, sizeof(value));
  provider_result = provider->read(provider->userdata, id, value,
                                   sizeof(value));
  if (provider_result == NXCOMPAT_PROVIDER_ABSENT)
    return observation;
  if (provider_result != NXCOMPAT_PROVIDER_VALUE ||
      !memchr(value, '\0', sizeof(value))) {
    observation->state = NXCOMPAT_OBSERVATION_REJECTED;
    observation->reason = NXCOMPAT_REASON_PROVIDER_CONTRACT;
    return observation;
  }
  if (nxcompat_observation_is_machine(id)) {
    observation->arch_value = nxcompat_parse_arch(value);
    if (observation->arch_value == NXCOMPAT_ARCH_UNKNOWN) {
      observation->state = NXCOMPAT_OBSERVATION_REJECTED;
      observation->reason = NXCOMPAT_REASON_OBSERVATION_MALFORMED;
      return observation;
    }
    observation->state = NXCOMPAT_OBSERVATION_PRESENT;
    observation->reason = nxcompat_machine_reason(id);
    return observation;
  }
  if ((id == NXCOMPAT_OBSERVATION_CGROUP_LIMIT_BYTES ||
       id == NXCOMPAT_OBSERVATION_RLIMIT_AS_BYTES) &&
      strcmp(value, "max") == 0) {
    observation->state = NXCOMPAT_OBSERVATION_PRESENT;
    observation->reason = NXCOMPAT_REASON_LIMIT_UNBOUNDED;
    observation->numeric_value = UINT64_MAX;
    return observation;
  }
  if (nxcompat_parse_u64_decimal(value, &numeric) != 0) {
    observation->state = NXCOMPAT_OBSERVATION_REJECTED;
    observation->reason = NXCOMPAT_REASON_OBSERVATION_MALFORMED;
    return observation;
  }
  if (id == NXCOMPAT_OBSERVATION_CGROUP_MODE && numeric != 1u &&
      numeric != 2u) {
    observation->state = NXCOMPAT_OBSERVATION_REJECTED;
    observation->reason = NXCOMPAT_REASON_OBSERVATION_OUT_OF_RANGE;
    return observation;
  }
  observation->state = NXCOMPAT_OBSERVATION_PRESENT;
  observation->numeric_value = numeric;
  if (id == NXCOMPAT_OBSERVATION_MEM_AVAILABLE_KIB)
    observation->reason = NXCOMPAT_REASON_MEMAVAILABLE_VERIFIED;
  else if (id == NXCOMPAT_OBSERVATION_RLIMIT_AS_BYTES)
    observation->reason = NXCOMPAT_REASON_RLIMIT_AS_VERIFIED;
  else
    observation->reason = NXCOMPAT_REASON_PROBE_COMPLETE;
  return observation;
}

const nxcompat_observation *
nxcompat_probe_find_observation(const nxcompat_probe_result *result,
                                nxcompat_observation_id id) {
  size_t index;
  if (!result || result->api_version != NXCOMPAT_API_VERSION_V2 ||
      result->struct_size < sizeof(*result) ||
      result->observation_count > NXCOMPAT_MAX_OBSERVATIONS)
    return NULL;
  for (index = 0; index < result->observation_count; ++index)
    if (result->observations[index].id == id)
      return &result->observations[index];
  return NULL;
}

static uint64_t nxcompat_observation_numeric(
    const nxcompat_probe_result *result, nxcompat_observation_id id,
    int *present) {
  const nxcompat_observation *observation =
      nxcompat_probe_find_observation(result, id);
  if (!observation || observation->state != NXCOMPAT_OBSERVATION_PRESENT) {
    *present = 0;
    return 0;
  }
  *present = 1;
  return observation->numeric_value;
}

static nxcompat_observation *nxcompat_find_observation_mutable(
    nxcompat_probe_result *result, nxcompat_observation_id id) {
  size_t index;
  if (!result || result->observation_count > NXCOMPAT_MAX_OBSERVATIONS)
    return NULL;
  for (index = 0; index < result->observation_count; ++index)
    if (result->observations[index].id == id)
      return &result->observations[index];
  return NULL;
}

static nxcompat_arch nxcompat_observation_arch_value(
    const nxcompat_probe_result *result, nxcompat_observation_id id) {
  const nxcompat_observation *observation =
      nxcompat_probe_find_observation(result, id);
  return observation && observation->state == NXCOMPAT_OBSERVATION_PRESENT
             ? observation->arch_value
             : NXCOMPAT_ARCH_UNKNOWN;
}

static uint64_t nxcompat_bytes_to_kib(uint64_t bytes) {
  return bytes / UINT64_C(1024);
}

static void nxcompat_effective_min(nxcompat_probe_result *result,
                                   uint32_t source, uint64_t value) {
  if (result->memory_sources == 0 || value < result->memory_effective_kib)
    result->memory_effective_kib = value;
  result->memory_sources |= source;
}

static int nxcompat_limit_is_finite(uint64_t value, unsigned cgroup_mode,
                                    nxcompat_arch kernel_arch) {
  if (value == UINT64_MAX)
    return 0;
  /* Linux cgroup-v1 represents unlimited with a page-rounded LONG_MAX. */
  if (cgroup_mode == 1u) {
    if ((kernel_arch == NXCOMPAT_ARCH_ARMV7 ||
         kernel_arch == NXCOMPAT_ARCH_I386) &&
        value >= UINT64_C(0x7ffff000))
      return 0;
    if ((kernel_arch == NXCOMPAT_ARCH_AARCH64 ||
         kernel_arch == NXCOMPAT_ARCH_X86_64) &&
        value >= (UINT64_C(1) << 60))
      return 0;
  }
  return 1;
}

static void nxcompat_compute_effective_memory(nxcompat_probe_result *result) {
  uint64_t value;
  uint64_t limit;
  uint64_t current;
  uint64_t vm_size;
  int have_value;
  int have_limit;
  int have_current;
  int have_mode;
  int have_vm_size;
  int cgroup_incomplete = 0;
  int rlimit_incomplete = 0;
  nxcompat_observation *limit_observation;

  value = nxcompat_observation_numeric(
      result, NXCOMPAT_OBSERVATION_MEM_AVAILABLE_KIB, &have_value);
  if (have_value) {
    result->memory_available_kib = value;
    nxcompat_effective_min(result, NXCOMPAT_MEMORY_SOURCE_MEMAVAILABLE, value);
  }
  result->memory_total_kib = nxcompat_observation_numeric(
      result, NXCOMPAT_OBSERVATION_MEM_TOTAL_KIB, &have_value);
  result->swap_total_kib = nxcompat_observation_numeric(
      result, NXCOMPAT_OBSERVATION_SWAP_TOTAL_KIB, &have_value);
  result->process_vm_kib = nxcompat_observation_numeric(
      result, NXCOMPAT_OBSERVATION_VM_SIZE_KIB, &have_vm_size);
  result->cgroup_version = (uint32_t)nxcompat_observation_numeric(
      result, NXCOMPAT_OBSERVATION_CGROUP_MODE, &have_mode);

  limit = nxcompat_observation_numeric(
      result, NXCOMPAT_OBSERVATION_CGROUP_LIMIT_BYTES, &have_limit);
  current = nxcompat_observation_numeric(
      result, NXCOMPAT_OBSERVATION_CGROUP_CURRENT_BYTES, &have_current);
  limit_observation = nxcompat_find_observation_mutable(
      result, NXCOMPAT_OBSERVATION_CGROUP_LIMIT_BYTES);
  if (!have_mode && (have_limit || have_current))
    cgroup_incomplete = 1;
  if (have_mode && result->cgroup_version == 1u &&
      result->kernel_arch == NXCOMPAT_ARCH_UNKNOWN && have_limit &&
      limit != UINT64_MAX && limit >= UINT64_C(0x7ffff000))
    cgroup_incomplete = 1;
  if (have_limit &&
      (result->cgroup_version == 1u || result->cgroup_version == 2u) &&
      nxcompat_limit_is_finite(limit, result->cgroup_version,
                               result->kernel_arch)) {
    if (limit_observation)
      limit_observation->reason =
          result->cgroup_version == 1u
              ? NXCOMPAT_REASON_CGROUP_V1_LIMIT_VERIFIED
              : NXCOMPAT_REASON_CGROUP_V2_LIMIT_VERIFIED;
    result->cgroup_limit_kib = nxcompat_bytes_to_kib(limit);
    if (have_current) {
      result->cgroup_current_kib = nxcompat_bytes_to_kib(current);
      value = current >= limit ? 0 : nxcompat_bytes_to_kib(limit - current);
      nxcompat_effective_min(result, NXCOMPAT_MEMORY_SOURCE_CGROUP, value);
    } else
      cgroup_incomplete = 1;
  } else if (have_limit &&
             (result->cgroup_version == 1u ||
              result->cgroup_version == 2u) &&
             limit_observation)
    limit_observation->reason = NXCOMPAT_REASON_LIMIT_UNBOUNDED;
  else if (result->cgroup_version == 1u || result->cgroup_version == 2u)
    cgroup_incomplete = 1;

  limit = nxcompat_observation_numeric(
      result, NXCOMPAT_OBSERVATION_RLIMIT_AS_BYTES, &have_limit);
  if (have_limit &&
      nxcompat_limit_is_finite(limit, 0u, result->kernel_arch)) {
    result->rlimit_as_kib = nxcompat_bytes_to_kib(limit);
    if (have_vm_size) {
      if (result->process_vm_kib > UINT64_MAX / UINT64_C(1024))
        value = 0;
      else {
        vm_size = result->process_vm_kib * UINT64_C(1024);
        value = vm_size >= limit ? 0 : nxcompat_bytes_to_kib(limit - vm_size);
      }
      nxcompat_effective_min(result, NXCOMPAT_MEMORY_SOURCE_RLIMIT_AS, value);
    } else
      rlimit_incomplete = 1;
  }

  if (cgroup_incomplete || rlimit_incomplete) {
    result->memory_sources = 0u;
    result->memory_effective_kib = 0u;
  }

  result->final_reason = result->memory_sources
                             ? NXCOMPAT_REASON_EFFECTIVE_MEMORY_MINIMUM
                             : NXCOMPAT_REASON_EFFECTIVE_MEMORY_UNKNOWN;
}

static nxcompat_result_code nxcompat_probe_core(
    const nxcompat_probe_provider *provider, nxcompat_probe_result *result,
    int strict_provider) {
  static const nxcompat_observation_id ids[] = {
      NXCOMPAT_OBSERVATION_PROCESS_MACHINE,
      NXCOMPAT_OBSERVATION_KERNEL_MACHINE,
      NXCOMPAT_OBSERVATION_USERLAND_MACHINE,
      NXCOMPAT_OBSERVATION_MEM_TOTAL_KIB,
      NXCOMPAT_OBSERVATION_MEM_AVAILABLE_KIB,
      NXCOMPAT_OBSERVATION_SWAP_TOTAL_KIB,
      NXCOMPAT_OBSERVATION_VM_SIZE_KIB,
      NXCOMPAT_OBSERVATION_CGROUP_MODE,
      NXCOMPAT_OBSERVATION_CGROUP_LIMIT_BYTES,
      NXCOMPAT_OBSERVATION_CGROUP_CURRENT_BYTES,
      NXCOMPAT_OBSERVATION_RLIMIT_AS_BYTES};
  size_t index;
  if (!result)
    return NXCOMPAT_INVALID;
  memset(result, 0, sizeof(*result));
  result->api_version = NXCOMPAT_API_VERSION_V2;
  result->struct_size = sizeof(*result);
  result->final_reason = NXCOMPAT_REASON_PROBE_COMPLETE;
  if (!provider || provider->api_version != NXCOMPAT_API_VERSION_V2 ||
      provider->struct_size < sizeof(*provider) || !provider->read) {
    result->final_reason = NXCOMPAT_REASON_PROVIDER_CONTRACT;
    return NXCOMPAT_INVALID;
  }
  for (index = 0; index < sizeof(ids) / sizeof(ids[0]); ++index)
    if (!nxcompat_collect_observation(provider, result, ids[index])) {
      result->final_reason = NXCOMPAT_REASON_PROVIDER_CONTRACT;
      return NXCOMPAT_FAILED;
    }
  result->process_arch = nxcompat_observation_arch_value(
      result, NXCOMPAT_OBSERVATION_PROCESS_MACHINE);
  result->kernel_arch = nxcompat_observation_arch_value(
      result, NXCOMPAT_OBSERVATION_KERNEL_MACHINE);
  result->userland_arch = nxcompat_observation_arch_value(
      result, NXCOMPAT_OBSERVATION_USERLAND_MACHINE);
  nxcompat_compute_effective_memory(result);
  if (strict_provider)
    for (index = 0; index < result->observation_count; ++index)
      if (result->observations[index].state ==
          NXCOMPAT_OBSERVATION_REJECTED) {
        result->final_reason = NXCOMPAT_REASON_PROVIDER_CONTRACT;
        return NXCOMPAT_FAILED;
      }
  return NXCOMPAT_OK;
}

const char *nxcompat_observation_name(nxcompat_observation_id value) {
  switch (value) {
  case NXCOMPAT_OBSERVATION_PROCESS_MACHINE:
    return "process-machine";
  case NXCOMPAT_OBSERVATION_KERNEL_MACHINE:
    return "kernel-machine";
  case NXCOMPAT_OBSERVATION_USERLAND_MACHINE:
    return "userland-machine";
  case NXCOMPAT_OBSERVATION_MEM_TOTAL_KIB:
    return "mem-total-kib";
  case NXCOMPAT_OBSERVATION_MEM_AVAILABLE_KIB:
    return "mem-available-kib";
  case NXCOMPAT_OBSERVATION_SWAP_TOTAL_KIB:
    return "swap-total-kib";
  case NXCOMPAT_OBSERVATION_VM_SIZE_KIB:
    return "vm-size-kib";
  case NXCOMPAT_OBSERVATION_CGROUP_MODE:
    return "cgroup-mode";
  case NXCOMPAT_OBSERVATION_CGROUP_LIMIT_BYTES:
    return "cgroup-limit-bytes";
  case NXCOMPAT_OBSERVATION_CGROUP_CURRENT_BYTES:
    return "cgroup-current-bytes";
  case NXCOMPAT_OBSERVATION_RLIMIT_AS_BYTES:
    return "rlimit-as-bytes";
  default:
    return "unknown";
  }
}

const char *nxcompat_reason_name(nxcompat_reason_code value) {
  switch (value) {
  case NXCOMPAT_REASON_NONE:
    return "none";
  case NXCOMPAT_REASON_PROBE_COMPLETE:
    return "probe-complete";
  case NXCOMPAT_REASON_INVALID_ARGUMENT:
    return "invalid-argument";
  case NXCOMPAT_REASON_UNSUPPORTED_API:
    return "unsupported-api";
  case NXCOMPAT_REASON_STRUCT_TOO_SMALL:
    return "struct-too-small";
  case NXCOMPAT_REASON_ARBITER_BUSY:
    return "arbiter-busy";
  case NXCOMPAT_REASON_PROVIDER_CONTRACT:
    return "provider-contract";
  case NXCOMPAT_REASON_OBSERVATION_ABSENT:
    return "observation-absent";
  case NXCOMPAT_REASON_OBSERVATION_MALFORMED:
    return "observation-malformed";
  case NXCOMPAT_REASON_OBSERVATION_OUT_OF_RANGE:
    return "observation-out-of-range";
  case NXCOMPAT_REASON_PROCESS_ARCH_VERIFIED:
    return "process-arch-verified";
  case NXCOMPAT_REASON_KERNEL_ARCH_VERIFIED:
    return "kernel-arch-verified";
  case NXCOMPAT_REASON_USERLAND_ARCH_VERIFIED:
    return "userland-arch-verified";
  case NXCOMPAT_REASON_MEMAVAILABLE_VERIFIED:
    return "memavailable-verified";
  case NXCOMPAT_REASON_CGROUP_V1_LIMIT_VERIFIED:
    return "cgroup-v1-limit-verified";
  case NXCOMPAT_REASON_CGROUP_V2_LIMIT_VERIFIED:
    return "cgroup-v2-limit-verified";
  case NXCOMPAT_REASON_RLIMIT_AS_VERIFIED:
    return "rlimit-as-verified";
  case NXCOMPAT_REASON_EFFECTIVE_MEMORY_MINIMUM:
    return "effective-memory-minimum";
  case NXCOMPAT_REASON_EFFECTIVE_MEMORY_UNKNOWN:
    return "effective-memory-unknown";
  case NXCOMPAT_REASON_LIMIT_UNBOUNDED:
    return "limit-unbounded";
  case NXCOMPAT_REASON_BACKEND_INHERITED_OK:
    return "backend-inherited-ok";
  case NXCOMPAT_REASON_BACKEND_AUTODETECT_OK:
    return "backend-autodetect-ok";
  case NXCOMPAT_REASON_BACKEND_ATTEMPT_RETRYABLE:
    return "backend-attempt-retryable";
  case NXCOMPAT_REASON_BACKEND_ATTEMPT_BUSY:
    return "backend-attempt-busy";
  case NXCOMPAT_REASON_BACKEND_ATTEMPT_FATAL:
    return "backend-attempt-fatal";
  case NXCOMPAT_REASON_BACKEND_CLEANUP_FAILED:
    return "backend-cleanup-failed";
  case NXCOMPAT_REASON_ENV_CLEAR_FAILED:
    return "environment-clear-failed";
  case NXCOMPAT_REASON_ENV_RESTORE_FAILED:
    return "environment-restore-failed";
  case NXCOMPAT_REASON_BACKEND_NAME_REJECTED:
    return "backend-name-rejected";
  case NXCOMPAT_REASON_BACKEND_FAKE_OUTPUT:
    return "backend-fake-output";
  case NXCOMPAT_REASON_VIDEO_OPEN_FAILED:
    return "video-open-failed";
  case NXCOMPAT_REASON_AUDIO_OPEN_FAILED:
    return "audio-open-failed";
  case NXCOMPAT_REASON_BACKEND_FOREIGN_OWNED:
    return "backend-foreign-owned";
  case NXCOMPAT_REASON_AUDIO_DEVICE_INVALID:
    return "audio-device-invalid";
  case NXCOMPAT_REASON_PLAN_CAPABILITY_UNAVAILABLE:
    return "plan-capability-unavailable";
  case NXCOMPAT_REASON_PLAN_INHERITED_PRESERVED:
    return "plan-inherited-preserved";
  case NXCOMPAT_REASON_PLAN_CAPABILITY_MATCHED:
    return "plan-capability-matched";
  case NXCOMPAT_REASON_PLAN_POLICY_NOT_NEEDED:
    return "plan-policy-not-needed";
  case NXCOMPAT_REASON_ENV_LATE_VALUE_PRESERVED:
    return "environment-late-value-preserved";
  case NXCOMPAT_REASON_ENV_APPLIED:
    return "environment-applied";
  case NXCOMPAT_REASON_ENV_APPLY_FAILED:
    return "environment-apply-failed";
  case NXCOMPAT_REASON_ENV_ROLLED_BACK:
    return "environment-rolled-back";
  case NXCOMPAT_REASON_ENV_ROLLBACK_FAILED:
    return "environment-rollback-failed";
  case NXCOMPAT_REASON_PLAN_COMPLETE:
    return "plan-complete";
  case NXCOMPAT_REASON_ENV_APPLY_COMPLETE:
    return "environment-apply-complete";
  case NXCOMPAT_REASON_CAPABILITY_PUBLISHED:
    return "capability-published";
  case NXCOMPAT_REASON_CAPABILITY_LOST:
    return "capability-lost";
  case NXCOMPAT_REASON_CAPABILITY_STALE:
    return "capability-stale";
  case NXCOMPAT_REASON_REQUIREMENT_UNKNOWN:
    return "requirement-unknown";
  case NXCOMPAT_REASON_REQUIREMENT_DUPLICATE:
    return "requirement-duplicate";
  case NXCOMPAT_REASON_REQUIREMENT_PENDING:
    return "requirement-pending";
  case NXCOMPAT_REASON_REQUIREMENT_MISSING:
    return "requirement-missing";
  case NXCOMPAT_REASON_REQUIREMENT_SATISFIED:
    return "requirement-satisfied";
  case NXCOMPAT_REASON_GRAPHICS_WINDOW_OPENED:
    return "graphics-window-opened";
  case NXCOMPAT_REASON_GRAPHICS_GLES_OPENED:
    return "graphics-gles-opened";
  case NXCOMPAT_REASON_GRAPHICS_EGL_OPENED:
    return "graphics-egl-opened";
  case NXCOMPAT_REASON_GRAPHICS_EGL_CONFIG_OPENED:
    return "graphics-egl-config-opened";
  case NXCOMPAT_REASON_GRAPHICS_DRAWABLE_OPENED:
    return "graphics-drawable-opened";
  case NXCOMPAT_REASON_AUDIO_OUTPUT_OPENED:
    return "audio-output-opened";
  case NXCOMPAT_REASON_INPUT_CONTROLLER_API_ACTIVE:
    return "input-controller-api-active";
  case NXCOMPAT_REASON_INPUT_CONTROLLER_CONNECTED:
    return "input-controller-connected";
  case NXCOMPAT_REASON_INPUT_HOTPLUG_ACTIVE:
    return "input-hotplug-active";
  case NXCOMPAT_REASON_INPUT_CONTROLLER_LOST:
    return "input-controller-lost";
  case NXCOMPAT_REASON_REPORT_SANITIZED:
    return "report-sanitized";
  default:
    return "unknown";
  }
}

static void nxcompat_probe_kernel(nxcompat_host *host) {
  struct utsname value;
  if (uname(&value) == 0) {
    nxcompat_copy_string(host->kernel_machine, sizeof(host->kernel_machine),
                         value.machine);
    host->kernel_arch = nxcompat_parse_arch(value.machine);
  } else {
    nxcompat_copy_string(host->kernel_machine, sizeof(host->kernel_machine),
                         "unknown");
    host->kernel_arch = NXCOMPAT_ARCH_UNKNOWN;
  }
}

static void nxcompat_sanitize_label(char *value) {
  unsigned char *cursor = (unsigned char *)value;
  char *end;
  while (*cursor) {
    if (*cursor < 0x20u || *cursor == 0x7fu)
      *cursor = ' ';
    ++cursor;
  }
  end = value + strlen(value);
  while (end > value && end[-1] == ' ')
    *--end = '\0';
}

static void nxcompat_probe_device_model(const nxcompat_context *context,
                                        nxcompat_host *host) {
  static const char *const candidates[] = {
      "/proc/device-tree/model", "/sys/firmware/devicetree/base/model",
      "/sys/devices/virtual/dmi/id/product_name", NULL};
  const char *const *candidate;
  for (candidate = candidates; *candidate; ++candidate) {
    if (nxcompat_read_first_line(context, *candidate, host->device_model,
                                 sizeof(host->device_model)) == 0 &&
        host->device_model[0]) {
      nxcompat_sanitize_label(host->device_model);
      if (host->device_model[0])
        return;
    }
  }
}

static void nxcompat_probe_libc(nxcompat_host *host) {
#ifdef _CS_GNU_LIBC_VERSION
  char value[NXCOMPAT_NAME_MAX];
  size_t length = confstr(_CS_GNU_LIBC_VERSION, value, sizeof(value));
  if (length > 0 && length < sizeof(value)) {
    nxcompat_copy_string(host->libc_version, sizeof(host->libc_version), value);
    return;
  }
#endif
  nxcompat_copy_string(host->libc_version, sizeof(host->libc_version),
                       "unknown");
}

static void nxcompat_unquote(char *value) {
  size_t length;
  if (!value)
    return;
  length = strlen(value);
  if (length >= 2 && value[0] == '"' && value[length - 1] == '"') {
    memmove(value, value + 1, length - 2);
    value[length - 2] = '\0';
  }
}

static void nxcompat_probe_os_release(const nxcompat_context *context,
                                      nxcompat_host *host) {
  char path[NXCOMPAT_PATH_MAX * 2u];
  char line[512];
  FILE *stream;
  if (nxcompat_join_root(context, "/etc/os-release", path, sizeof(path)) != 0)
    return;
  stream = fopen(path, "r");
  if (!stream)
    return;
  while (fgets(line, sizeof(line), stream)) {
    char *value;
    line[strcspn(line, "\r\n")] = '\0';
    value = strchr(line, '=');
    if (!value)
      continue;
    *value++ = '\0';
    nxcompat_unquote(value);
    if (strcmp(line, "ID") == 0)
      nxcompat_copy_string(host->os_id, sizeof(host->os_id), value);
    else if (strcmp(line, "VERSION_ID") == 0)
      nxcompat_copy_string(host->os_version, sizeof(host->os_version), value);
  }
  (void)fclose(stream);
}

static void nxcompat_decode_mount_field(char *value) {
  char *read_cursor = value;
  char *write_cursor = value;
  while (read_cursor && *read_cursor) {
    if (read_cursor[0] == '\\' && isdigit((unsigned char)read_cursor[1]) &&
        isdigit((unsigned char)read_cursor[2]) &&
        isdigit((unsigned char)read_cursor[3])) {
      unsigned decoded = (unsigned)(read_cursor[1] - '0') * 64u +
                         (unsigned)(read_cursor[2] - '0') * 8u +
                         (unsigned)(read_cursor[3] - '0');
      *write_cursor++ = (char)decoded;
      read_cursor += 4;
    } else {
      *write_cursor++ = *read_cursor++;
    }
  }
  *write_cursor = '\0';
}

static int nxcompat_path_has_prefix(const char *path, const char *prefix) {
  size_t length;
  if (!path || !prefix)
    return 0;
  length = strlen(prefix);
  if (length == 0 || strncmp(path, prefix, length) != 0)
    return 0;
  return length == 1 || path[length] == '\0' || path[length] == '/';
}

static nxcompat_filesystem_class
nxcompat_classify_filesystem(const char *filesystem) {
  if (!filesystem || !*filesystem)
    return NXCOMPAT_FILESYSTEM_UNKNOWN;
  if (strstr(filesystem, "fuse") || strcmp(filesystem, "exfat") == 0 ||
      strcmp(filesystem, "ntfs") == 0 || strcmp(filesystem, "vfat") == 0)
    return NXCOMPAT_FILESYSTEM_FUSE_LIKE;
  if (strncmp(filesystem, "nfs", 3) == 0 || strcmp(filesystem, "cifs") == 0 ||
      strcmp(filesystem, "smb3") == 0)
    return NXCOMPAT_FILESYSTEM_NETWORK;
  return NXCOMPAT_FILESYSTEM_POSIX;
}

static void nxcompat_probe_filesystem(const nxcompat_context *context,
                                      nxcompat_host *host) {
  char path[NXCOMPAT_PATH_MAX * 2u];
  char line[2048];
  char selected_type[NXCOMPAT_NAME_MAX] = "";
  size_t selected_length = 0;
  FILE *stream;
  if (!host->game_dir[0] ||
      nxcompat_join_root(context, "/proc/mounts", path, sizeof(path)) != 0)
    return;
  stream = fopen(path, "r");
  if (!stream)
    return;
  while (fgets(line, sizeof(line), stream)) {
    char source[NXCOMPAT_PATH_MAX];
    char mountpoint[NXCOMPAT_PATH_MAX];
    char filesystem[NXCOMPAT_NAME_MAX];
    size_t length;
    if (sscanf(line, "%511s %511s %63s", source, mountpoint, filesystem) != 3)
      continue;
    nxcompat_decode_mount_field(mountpoint);
    length = strlen(mountpoint);
    if (length >= selected_length &&
        nxcompat_path_has_prefix(host->game_dir, mountpoint)) {
      nxcompat_copy_string(selected_type, sizeof(selected_type), filesystem);
      selected_length = length;
    }
  }
  (void)fclose(stream);
  nxcompat_copy_string(host->filesystem_type, sizeof(host->filesystem_type),
                       selected_type[0] ? selected_type : "unknown");
  host->filesystem_class = nxcompat_classify_filesystem(selected_type);
  if (host->filesystem_class == NXCOMPAT_FILESYSTEM_FUSE_LIKE)
    host->capabilities |= NXCOMPAT_CAP_FUSE_LIKE_FILESYSTEM;
}

static int nxcompat_open_device_directory(const nxcompat_context *context,
                                          const char *device_path,
                                          DIR **directory) {
  char host_path[NXCOMPAT_PATH_MAX * 2u];
  if (nxcompat_join_root(context, device_path, host_path, sizeof(host_path)) !=
      0)
    return -1;
  *directory = opendir(host_path);
  return *directory ? 0 : -1;
}

static void nxcompat_probe_drm(const nxcompat_context *context,
                               nxcompat_host *host) {
  DIR *directory = NULL;
  struct dirent *entry;
  char best_connector[NXCOMPAT_PATH_MAX] = "";
  if (nxcompat_directory_exists(context, "/dev/dri"))
    host->capabilities |= NXCOMPAT_CAP_DRM;
  if (nxcompat_open_device_directory(context, "/sys/class/drm", &directory) !=
      0)
    return;
  host->capabilities |= NXCOMPAT_CAP_DRM;
  while ((entry = readdir(directory)) != NULL) {
    char status_path[NXCOMPAT_PATH_MAX];
    char modes_path[NXCOMPAT_PATH_MAX];
    char status[64];
    char mode[128];
    if (strncmp(entry->d_name, "card", 4) != 0 ||
        strchr(entry->d_name, '-') == NULL)
      continue;
    if (snprintf(status_path, sizeof(status_path),
                 "/sys/class/drm/%s/status", entry->d_name) >=
            (int)sizeof(status_path) ||
        nxcompat_read_first_line(context, status_path, status, sizeof(status)) !=
            0 ||
        strcmp(status, "connected") != 0)
      continue;
    if (!best_connector[0] || strcmp(status_path, best_connector) < 0)
      nxcompat_copy_string(best_connector, sizeof(best_connector), status_path);
    host->capabilities |= NXCOMPAT_CAP_DRM_CONNECTED;
    if (host->display_width > 0)
      continue;
    if (snprintf(modes_path, sizeof(modes_path), "/sys/class/drm/%s/modes",
                 entry->d_name) >= (int)sizeof(modes_path))
      continue;
    if (nxcompat_read_first_line(context, modes_path, mode, sizeof(mode)) == 0 &&
        nxcompat_parse_display_mode(mode, &host->display_width,
                                    &host->display_height) == 0)
      nxcompat_copy_string(host->display_source, sizeof(host->display_source),
                           modes_path);
  }
  (void)closedir(directory);
  nxcompat_copy_string(host->drm_connector, sizeof(host->drm_connector),
                       best_connector);
}

static void nxcompat_probe_fbdev(const nxcompat_context *context,
                                 nxcompat_host *host) {
  char mode[128];
  if (!nxcompat_path_exists(context, "/dev/fb0") &&
      !nxcompat_directory_exists(context, "/sys/class/graphics/fb0"))
    return;
  host->capabilities |= NXCOMPAT_CAP_FBDEV;
  if (host->display_width > 0)
    return;
  if (nxcompat_read_first_line(context, "/sys/class/graphics/fb0/mode", mode,
                               sizeof(mode)) == 0 &&
      nxcompat_parse_display_mode(mode, &host->display_width,
                                  &host->display_height) == 0) {
    nxcompat_copy_string(host->display_source, sizeof(host->display_source),
                         "/sys/class/graphics/fb0/mode");
    return;
  }
  if (nxcompat_read_first_line(context, "/sys/class/graphics/fb0/modes", mode,
                               sizeof(mode)) == 0 &&
      nxcompat_parse_display_mode(mode, &host->display_width,
                                  &host->display_height) == 0) {
    nxcompat_copy_string(host->display_source, sizeof(host->display_source),
                         "/sys/class/graphics/fb0/modes");
    return;
  }
  /* Last fbdev fallback (fleet-proven in the universal ports): virtual_size
   * is "W,VH" and VH often carries the double-buffer pan (1280x1440 and
   * 640x960 are panned; 640x480 is not).  Halve VH only when the halved
   * height still looks like a panel for this width. */
  if (nxcompat_read_first_line(context,
                               "/sys/class/graphics/fb0/virtual_size", mode,
                               sizeof(mode)) == 0) {
    long width = 0;
    long virtual_height = 0;
    char *cursor = mode;
    char *end = NULL;
    width = strtol(cursor, &end, 10);
    if (end != cursor && *end == ',') {
      cursor = end + 1;
      virtual_height = strtol(cursor, &end, 10);
      if (end != cursor && width > 0 && virtual_height > 0 &&
          width <= 32767 && virtual_height <= 65534) {
        long height = virtual_height;
        if (virtual_height > width && (virtual_height % 2) == 0) {
          long half = virtual_height / 2;
          if (half <= width && half * 2 >= width)
            height = half;
        }
        host->display_width = (int)width;
        host->display_height = (int)height;
        nxcompat_copy_string(host->display_source,
                             sizeof(host->display_source),
                             "/sys/class/graphics/fb0/virtual_size");
      }
    }
  }
}

static int nxcompat_runtime_dir_usable(const nxcompat_context *context,
                                       const char *path) {
  return path && path[0] == '/' && nxcompat_directory_usable(context, path);
}

static void nxcompat_find_runtime_dir(const nxcompat_context *context,
                                      nxcompat_host *host) {
  char run_user[64];
  char var_run_user[64];
  char run_legacy[64];
  const char *inherited = getenv("XDG_RUNTIME_DIR");
  const char *candidates[6];
  size_t index;
  unsigned uid = (unsigned)getuid();

  (void)snprintf(run_user, sizeof(run_user), "/run/user/%u", uid);
  (void)snprintf(var_run_user, sizeof(var_run_user), "/var/run/user/%u", uid);
  (void)snprintf(run_legacy, sizeof(run_legacy), "/run/%u-runtime-dir", uid);
  if (nxcompat_runtime_dir_usable(context, inherited)) {
    nxcompat_copy_string(host->session_runtime_dir,
                         sizeof(host->session_runtime_dir), inherited);
    return;
  }
  candidates[0] = run_legacy;
  candidates[1] = run_user;
  candidates[2] = var_run_user;
  candidates[3] = "/run/0-runtime-dir";
  candidates[4] = "/var/run/0-runtime-dir";
  candidates[5] = NULL;
  for (index = 0; candidates[index]; ++index) {
    if (nxcompat_runtime_dir_usable(context, candidates[index])) {
      nxcompat_copy_string(host->session_runtime_dir,
                           sizeof(host->session_runtime_dir),
                           candidates[index]);
      return;
    }
  }
}

static void nxcompat_probe_wayland(const nxcompat_context *context,
                                   nxcompat_host *host) {
  const char *display = getenv("WAYLAND_DISPLAY");
  char candidate[NXCOMPAT_PATH_MAX];
  if (!host->session_runtime_dir[0])
    return;
  if (display && *display) {
    if (display[0] == '/')
      nxcompat_copy_string(candidate, sizeof(candidate), display);
    else if (snprintf(candidate, sizeof(candidate), "%s/%s",
                      host->session_runtime_dir, display) >=
             (int)sizeof(candidate))
      candidate[0] = '\0';
    if (candidate[0] && nxcompat_socket_exists(context, candidate)) {
      host->capabilities |= NXCOMPAT_CAP_WAYLAND;
      nxcompat_copy_string(host->wayland_socket,
                           sizeof(host->wayland_socket), candidate);
      return;
    }
  }
  {
    DIR *directory = NULL;
    struct dirent *entry;
    if (nxcompat_open_device_directory(context, host->session_runtime_dir,
                                       &directory) != 0)
      return;
    while ((entry = readdir(directory)) != NULL) {
      if (strncmp(entry->d_name, "wayland-", 8) != 0 ||
          strstr(entry->d_name, ".lock") != NULL)
        continue;
      if (snprintf(candidate, sizeof(candidate), "%s/%s",
                   host->session_runtime_dir, entry->d_name) >=
          (int)sizeof(candidate))
        continue;
      if (nxcompat_socket_exists(context, candidate)) {
        host->capabilities |= NXCOMPAT_CAP_WAYLAND;
        nxcompat_copy_string(host->wayland_socket,
                             sizeof(host->wayland_socket), candidate);
        break;
      }
    }
    (void)closedir(directory);
  }
}

static void nxcompat_probe_audio(const nxcompat_context *context,
                                 nxcompat_host *host) {
  char runtime_pulse[NXCOMPAT_PATH_MAX];
  char runtime_pipewire[NXCOMPAT_PATH_MAX];
  const char *pulse_candidates[5];
  size_t index;
  runtime_pulse[0] = '\0';
  runtime_pipewire[0] = '\0';
  if (host->session_runtime_dir[0]) {
    (void)nxcompat_join_path(host->session_runtime_dir, "pulse/native",
                             runtime_pulse, sizeof(runtime_pulse));
    (void)nxcompat_join_path(host->session_runtime_dir, "pipewire-0",
                             runtime_pipewire, sizeof(runtime_pipewire));
  }
  pulse_candidates[0] = runtime_pulse;
  pulse_candidates[1] = "/var/run/pulse/native";
  pulse_candidates[2] = "/run/pulse/native";
  pulse_candidates[3] = NULL;
  for (index = 0; pulse_candidates[index]; ++index) {
    if (*pulse_candidates[index] &&
        nxcompat_socket_connectable(context, pulse_candidates[index])) {
      host->capabilities |= NXCOMPAT_CAP_PULSE_SOCKET;
      nxcompat_copy_string(host->pulse_socket, sizeof(host->pulse_socket),
                           pulse_candidates[index]);
      break;
    }
  }
  if (runtime_pipewire[0] &&
      nxcompat_socket_connectable(context, runtime_pipewire)) {
    host->capabilities |= NXCOMPAT_CAP_PIPEWIRE_SOCKET;
    nxcompat_copy_string(host->pipewire_socket, sizeof(host->pipewire_socket),
                         runtime_pipewire);
  }
  if (nxcompat_path_exists(context, "/proc/asound/cards") ||
      nxcompat_directory_exists(context, "/dev/snd"))
    host->capabilities |= NXCOMPAT_CAP_ALSA;
}

static nxcompat_arch
nxcompat_shared_object_arch(const nxcompat_context *context,
                            const char *device_path) {
  unsigned char header[64];
  char host_path[NXCOMPAT_PATH_MAX * 2u];
  FILE *stream;
  uint16_t type;
  uint16_t machine;
  uint32_t flags;

  if (!nxcompat_regular_file_exists(context, device_path) ||
      nxcompat_join_root(context, device_path, host_path, sizeof(host_path)) !=
          0)
    return NXCOMPAT_ARCH_UNKNOWN;
  stream = fopen(host_path, "rb");
  if (!stream)
    return NXCOMPAT_ARCH_UNKNOWN;
  if (fread(header, 1, sizeof(header), stream) != sizeof(header)) {
    (void)fclose(stream);
    return NXCOMPAT_ARCH_UNKNOWN;
  }
  (void)fclose(stream);
  if (header[0] != 0x7fu || header[1] != 'E' || header[2] != 'L' ||
      header[3] != 'F' || (header[4] != 1u && header[4] != 2u) ||
      header[5] != 1u || header[6] != 1u)
    return NXCOMPAT_ARCH_UNKNOWN;
  type = (uint16_t)header[16] | ((uint16_t)header[17] << 8);
  machine = (uint16_t)header[18] | ((uint16_t)header[19] << 8);
  if (type != 3u)
    return NXCOMPAT_ARCH_UNKNOWN;
  if (header[4] == 2u && machine == 183u)
    return NXCOMPAT_ARCH_AARCH64;
  if (header[4] == 1u && machine == 3u)
    return NXCOMPAT_ARCH_I386;
  if (header[4] != 1u || machine != 40u)
    return NXCOMPAT_ARCH_UNKNOWN;
  flags = (uint32_t)header[36] | ((uint32_t)header[37] << 8) |
          ((uint32_t)header[38] << 16) | ((uint32_t)header[39] << 24);
  return (flags & UINT32_C(0x400)) != 0u ? NXCOMPAT_ARCH_ARMV7
                                         : NXCOMPAT_ARCH_UNKNOWN;
}

static void nxcompat_probe_generic_library_arches(
    const nxcompat_context *context, nxcompat_host *host) {
  static const char *const roots[] = {
      "/usr/local/lib64", "/usr/lib64", "/lib64", "/usr/local/lib",
      "/usr/lib", "/lib", NULL};
  static const char *const names[] = {
      "libc.so.6", "libm.so.6", "libdl.so.2", NULL};
  const char *const *root;

  for (root = roots; *root; ++root) {
    const char *const *name;
    if (!nxcompat_directory_exists(context, *root))
      continue;
    for (name = names; *name; ++name) {
      char candidate[NXCOMPAT_PATH_MAX];
      nxcompat_arch arch;
      if (snprintf(candidate, sizeof(candidate), "%s/%s", *root, *name) >=
          (int)sizeof(candidate))
        continue;
      arch = nxcompat_shared_object_arch(context, candidate);
      if (arch == NXCOMPAT_ARCH_ARMV7)
        host->capabilities |= NXCOMPAT_CAP_ARMHF_LIBS;
      else if (arch == NXCOMPAT_ARCH_AARCH64)
        host->capabilities |= NXCOMPAT_CAP_AARCH64_LIBS;
      else if (arch == NXCOMPAT_ARCH_I386)
        host->capabilities |= NXCOMPAT_CAP_I386_LIBS;
    }
  }
}

static void nxcompat_probe_library_arches(const nxcompat_context *context,
                                          nxcompat_host *host) {
  static const char *const armhf_dirs[] = {
      "/usr/local/lib32", "/usr/lib32",
      "/usr/local/lib/arm-linux-gnueabihf", "/usr/lib/arm-linux-gnueabihf",
      "/lib/arm-linux-gnueabihf", NULL};
  static const char *const aarch64_dirs[] = {
      "/usr/local/lib/aarch64-linux-gnu", "/usr/lib/aarch64-linux-gnu",
      "/lib/aarch64-linux-gnu", NULL};
  static const char *const i386_dirs[] = {
      "/usr/local/lib/i386-linux-gnu", "/usr/lib/i386-linux-gnu",
      "/lib/i386-linux-gnu", NULL};
  const char *const *candidate;

  for (candidate = armhf_dirs; *candidate; ++candidate)
    if (nxcompat_directory_exists(context, *candidate)) {
      host->capabilities |= NXCOMPAT_CAP_ARMHF_LIBS;
      break;
    }
  for (candidate = aarch64_dirs; *candidate; ++candidate)
    if (nxcompat_directory_exists(context, *candidate)) {
      host->capabilities |= NXCOMPAT_CAP_AARCH64_LIBS;
      break;
    }
  for (candidate = i386_dirs; *candidate; ++candidate)
    if (nxcompat_directory_exists(context, *candidate)) {
      host->capabilities |= NXCOMPAT_CAP_I386_LIBS;
      break;
    }

  /* Embedded pure-architecture systems commonly keep every native library in
   * /usr/lib instead of a multiarch directory.  A generic directory name is
   * not evidence by itself: promote it only after a real shared-object ELF
   * header proves the ABI. */
  nxcompat_probe_generic_library_arches(context, host);
}

static int nxcompat_armhf_shared_object(const nxcompat_context *context,
                                        const char *device_path) {
  unsigned char header[52];
  char host_path[NXCOMPAT_PATH_MAX * 2u];
  FILE *stream;
  uint16_t type;
  uint16_t machine;
  uint32_t flags;
  if (nxcompat_join_root(context, device_path, host_path, sizeof(host_path)) !=
      0)
    return 0;
  stream = fopen(host_path, "rb");
  if (!stream)
    return 0;
  if (fread(header, 1, sizeof(header), stream) != sizeof(header)) {
    (void)fclose(stream);
    return 0;
  }
  (void)fclose(stream);
  if (header[0] != 0x7fu || header[1] != 'E' || header[2] != 'L' ||
      header[3] != 'F' || header[4] != 1u || header[5] != 1u ||
      header[6] != 1u)
    return 0;
  type = (uint16_t)header[16] | ((uint16_t)header[17] << 8);
  machine = (uint16_t)header[18] | ((uint16_t)header[19] << 8);
  flags = (uint32_t)header[36] | ((uint32_t)header[37] << 8) |
          ((uint32_t)header[38] << 16) | ((uint32_t)header[39] << 24);
  return type == 3u && machine == 40u && (flags & UINT32_C(0x400)) != 0;
}

static int nxcompat_directory_has_armhf_module(
    const nxcompat_context *context, const char *directory_path,
    const char *pattern, unsigned depth) {
  DIR *directory = NULL;
  struct dirent *entry;
  if (nxcompat_open_device_directory(context, directory_path, &directory) != 0)
    return 0;
  while ((entry = readdir(directory)) != NULL) {
    char candidate[NXCOMPAT_PATH_MAX];
    if (entry->d_name[0] == '.' &&
        (entry->d_name[1] == '\0' ||
         (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
      continue;
    if (snprintf(candidate, sizeof(candidate), "%s/%s", directory_path,
                 entry->d_name) >= (int)sizeof(candidate))
      continue;
    if (fnmatch(pattern, entry->d_name, 0) == 0 &&
        nxcompat_regular_file_exists(context, candidate) &&
        nxcompat_armhf_shared_object(context, candidate)) {
      (void)closedir(directory);
      return 1;
    }
    if (depth != 0 && nxcompat_directory_exists(context, candidate) &&
        nxcompat_directory_has_armhf_module(context, candidate, pattern,
                                             depth - 1u)) {
      (void)closedir(directory);
      return 1;
    }
  }
  (void)closedir(directory);
  return 0;
}

static void nxcompat_probe_armhf_audio_modules(const nxcompat_context *context,
                                                nxcompat_host *host) {
  static const char *const roots[] = {
      "/usr/local/lib32", "/usr/lib32",
      "/usr/local/lib/arm-linux-gnueabihf", "/usr/lib/arm-linux-gnueabihf",
      "/lib/arm-linux-gnueabihf", NULL};
  const char *const *root;
  for (root = roots; *root; ++root) {
    char candidate[NXCOMPAT_PATH_MAX];
    if (!host->armhf_pipewire_modules[0] &&
        snprintf(candidate, sizeof(candidate), "%s/pipewire-0.3", *root) <
            (int)sizeof(candidate) &&
        nxcompat_directory_has_armhf_module(
            context, candidate, "libpipewire-module-*.so*", 0u))
      nxcompat_copy_string(host->armhf_pipewire_modules,
                           sizeof(host->armhf_pipewire_modules), candidate);
    if (!host->armhf_spa_plugins[0] &&
        snprintf(candidate, sizeof(candidate), "%s/spa-0.2", *root) <
            (int)sizeof(candidate) &&
        nxcompat_directory_has_armhf_module(context, candidate,
                                             "libspa-*.so*", 1u))
      nxcompat_copy_string(host->armhf_spa_plugins,
                           sizeof(host->armhf_spa_plugins), candidate);
    if (!host->armhf_alsa_plugins[0] &&
        snprintf(candidate, sizeof(candidate),
                 "%s/alsa-lib/libasound_module_pcm_pipewire.so", *root) <
            (int)sizeof(candidate) &&
        nxcompat_regular_file_exists(context, candidate) &&
        nxcompat_armhf_shared_object(context, candidate)) {
      (void)snprintf(candidate, sizeof(candidate), "%s/alsa-lib", *root);
      nxcompat_copy_string(host->armhf_alsa_plugins,
                           sizeof(host->armhf_alsa_plugins), candidate);
    }
  }
}

static int nxcompat_control_file_exists(const nxcompat_context *context,
                                        const char *directory) {
  char candidate[NXCOMPAT_PATH_MAX];
  return directory && directory[0] == '/' &&
         snprintf(candidate, sizeof(candidate), "%s/control.txt", directory) <
             (int)sizeof(candidate) &&
         nxcompat_regular_file_exists(context, candidate);
}

static void nxcompat_probe_portmaster(const nxcompat_context *context,
                                      const nxcompat_probe_options *options,
                                      nxcompat_host *host) {
  static const char *const roots[] = {
      "/opt/system/Tools/PortMaster", "/opt/tools/PortMaster",
      "/storage/.config/PortMaster", "/PortMaster",
      "/roms/ports/PortMaster", "/roms/Ports/PortMaster",
      "/roms2/ports/PortMaster", "/roms2/Ports/PortMaster",
      "/storage/roms/ports/PortMaster", "/storage/roms/Ports/PortMaster",
      "/userdata/roms/ports/PortMaster", "/userdata/roms/Ports/PortMaster",
      "/mnt/ports/PortMaster", "/mnt/Ports/PortMaster",
      "/mnt/mmc/ports/PortMaster", "/mnt/mmc/Ports/PortMaster",
      "/mnt/mmc/roms/ports/PortMaster", "/mnt/mmc/ROMS/ports/PortMaster",
      "/mnt/sdcard/ports/PortMaster", "/mnt/sdcard/Ports/PortMaster",
      "/mnt/sdcard/roms/ports/PortMaster",
      "/mnt/sdcard/ROMS/ports/PortMaster", NULL};
  static const char *const global_databases[] = {
      "/storage/.config/SDL-GameControllerDB/gamecontrollerdb.txt",
      "/storage/.config/SDL-GameControllerDB/gamecontrollerdb-SDL2.txt",
      "/usr/share/SDL2/gamecontrollerdb.txt", NULL};
  const char *const *root;
  const char *const *database;
  const char *declared = options->portmaster_dir;
  const char *declared_environment;
  const char *controller_environment;
  char sibling[NXCOMPAT_PATH_MAX];

  sibling[0] = '\0';
  declared_environment = getenv("NXCOMPAT_PORTMASTER_DIR");
  if ((!declared || !*declared) && declared_environment &&
      *declared_environment)
    declared = declared_environment;

  if (nxcompat_control_file_exists(context, declared))
    nxcompat_copy_string(host->portmaster_dir, sizeof(host->portmaster_dir),
                         declared);
  else {
    if (options->game_dir && options->game_dir[0] == '/' &&
        snprintf(sibling, sizeof(sibling), "%s", options->game_dir) <
            (int)sizeof(sibling)) {
      char *slash = strrchr(sibling, '/');
      if (slash && slash != sibling) {
        *slash = '\0';
        if (strlen(sibling) + strlen("/PortMaster") + 1u < sizeof(sibling))
          (void)strcat(sibling, "/PortMaster");
        else
          sibling[0] = '\0';
      } else {
        sibling[0] = '\0';
      }
    }
    if (nxcompat_control_file_exists(context, sibling))
      nxcompat_copy_string(host->portmaster_dir,
                           sizeof(host->portmaster_dir), sibling);
    for (root = roots; !host->portmaster_dir[0] && *root; ++root)
      if (nxcompat_control_file_exists(context, *root)) {
        nxcompat_copy_string(host->portmaster_dir,
                             sizeof(host->portmaster_dir), *root);
        break;
      }
  }
  if (host->portmaster_dir[0]) {
    char candidate[NXCOMPAT_PATH_MAX];
    const char *names[] = {"gamecontrollerdb.txt", "gamecontrollerdb-SDL2.txt",
                           NULL};
    size_t index;
    host->capabilities |= NXCOMPAT_CAP_PORTMASTER;
    for (index = 0; names[index]; ++index) {
      if (snprintf(candidate, sizeof(candidate), "%s/%s",
                   host->portmaster_dir, names[index]) >=
          (int)sizeof(candidate))
        continue;
      if (nxcompat_regular_file_exists(context, candidate)) {
        nxcompat_copy_string(host->controller_db, sizeof(host->controller_db),
                             candidate);
        break;
      }
    }
  }
  if (!host->controller_db[0]) {
    for (database = global_databases; *database; ++database)
      if (nxcompat_regular_file_exists(context, *database)) {
        nxcompat_copy_string(host->controller_db, sizeof(host->controller_db),
                             *database);
        break;
      }
  }
  controller_environment = getenv("SDL_GAMECONTROLLERCONFIG");
  if (host->controller_db[0] ||
      (controller_environment && *controller_environment))
    host->capabilities |= NXCOMPAT_CAP_CONTROLLER_MAPPING;
}

int nxcompat_probe(const nxcompat_probe_options *options, nxcompat_host *host) {
  nxcompat_context context;
  nxcompat_default_provider_state default_state;
  nxcompat_probe_provider default_provider;
  nxcompat_probe_result local_result;
  nxcompat_probe_result *probe_result = &local_result;
  const nxcompat_probe_provider *provider;
  const char *environment;
  uint64_t class_memory;
  int custom_provider;
  nxcompat_result_code status;
  if (!nxcompat_options_valid(options) || !host)
    return NXCOMPAT_INVALID;
  /* BUSY is a no-observation path: callers may retry with the same output
   * objects, including from a reentrant provider callback.  Acquire before
   * writing either output so an outer in-flight probe cannot be corrupted. */
  if (nxcompat_global_arbiter_try_acquire() != NXCOMPAT_OK)
    return NXCOMPAT_BUSY;
  memset(host, 0, sizeof(*host));
  host->api_version = options->api_version;
  host->struct_size = sizeof(*host);
  custom_provider = options->api_version == NXCOMPAT_API_VERSION_V2 &&
                    options->provider != NULL;
  if (options->api_version == NXCOMPAT_API_VERSION_V2 && options->result)
    probe_result = options->result;
  nxcompat_context_init(options, &context);
  memset(&default_state, 0, sizeof(default_state));
  default_state.context = context;
  memset(&default_provider, 0, sizeof(default_provider));
  default_provider.api_version = NXCOMPAT_API_VERSION_V2;
  default_provider.struct_size = sizeof(default_provider);
  default_provider.read = nxcompat_default_read_observation;
  default_provider.userdata = &default_state;
  provider = custom_provider ? options->provider : &default_provider;
  status = nxcompat_probe_core(provider, probe_result, custom_provider);
  if (status != NXCOMPAT_OK) {
    if (probe_result) {
      probe_result->api_version = NXCOMPAT_API_VERSION_V2;
      probe_result->struct_size = sizeof(*probe_result);
      probe_result->final_reason = NXCOMPAT_REASON_PROVIDER_CONTRACT;
    }
    nxcompat_global_arbiter_release();
    return status;
  }
  host->process_arch = probe_result->process_arch;
  host->kernel_arch = probe_result->kernel_arch;
  host->memory_class = NXCOMPAT_MEMORY_UNKNOWN;
  host->filesystem_class = NXCOMPAT_FILESYSTEM_UNKNOWN;
  host->memory_total_kib = probe_result->memory_total_kib;
  host->swap_total_kib = probe_result->swap_total_kib;
  nxcompat_copy_string(host->port_id, sizeof(host->port_id), options->port_id);
  nxcompat_copy_string(host->game_dir, sizeof(host->game_dir), options->game_dir);

  class_memory = options->api_version == NXCOMPAT_API_VERSION_V1
                     ? probe_result->memory_total_kib
                     : probe_result->memory_effective_kib;
  if ((options->api_version == NXCOMPAT_API_VERSION_V1 && class_memory != 0) ||
      (options->api_version == NXCOMPAT_API_VERSION_V2 &&
       probe_result->memory_sources != 0)) {
    if (class_memory < UINT64_C(1048576)) {
      host->memory_class = NXCOMPAT_MEMORY_SHORT;
      host->capabilities |= NXCOMPAT_CAP_SHORT_MEMORY;
    } else if (class_memory < UINT64_C(2097152))
      host->memory_class = NXCOMPAT_MEMORY_MEDIUM;
    else
      host->memory_class = NXCOMPAT_MEMORY_HIGH;
  }

  if (custom_provider) {
    nxcompat_copy_string(host->kernel_machine, sizeof(host->kernel_machine),
                         nxcompat_arch_name(host->kernel_arch));
    nxcompat_global_arbiter_release();
    return NXCOMPAT_OK;
  }

  environment = getenv("SDL_VIDEODRIVER");
  if (environment && *environment) {
    host->capabilities |= NXCOMPAT_CAP_SDL_VIDEO_INHERITED;
    nxcompat_copy_string(host->inherited_video_driver,
                         sizeof(host->inherited_video_driver), environment);
  }
  environment = getenv("SDL_AUDIODRIVER");
  if (environment && *environment) {
    host->capabilities |= NXCOMPAT_CAP_SDL_AUDIO_INHERITED;
    nxcompat_copy_string(host->inherited_audio_driver,
                         sizeof(host->inherited_audio_driver), environment);
  }
  environment = getenv("DISPLAY");
  if (environment && *environment)
    host->capabilities |= NXCOMPAT_CAP_X11;

  nxcompat_probe_kernel(host);
  nxcompat_probe_device_model(&context, host);
  nxcompat_probe_libc(host);
  nxcompat_probe_os_release(&context, host);
  nxcompat_probe_filesystem(&context, host);
  nxcompat_probe_drm(&context, host);
  nxcompat_probe_fbdev(&context, host);
  nxcompat_find_runtime_dir(&context, host);
  nxcompat_probe_wayland(&context, host);
  nxcompat_probe_audio(&context, host);
  nxcompat_probe_library_arches(&context, host);
  nxcompat_probe_armhf_audio_modules(&context, host);
  nxcompat_probe_portmaster(&context, options, host);
  nxcompat_global_arbiter_release();
  return NXCOMPAT_OK;
}
