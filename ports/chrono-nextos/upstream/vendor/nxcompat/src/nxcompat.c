/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxcompat_internal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

void nxcompat_copy_string(char *destination, size_t destination_size,
                          const char *source) {
  if (!destination || destination_size == 0)
    return;
  if (!source)
    source = "";
  (void)snprintf(destination, destination_size, "%s", source);
}

int nxcompat_join_path(const char *directory, const char *suffix, char *output,
                       size_t output_size) {
  size_t directory_length;
  size_t suffix_length;
  size_t remaining;
  int needs_separator;

  if (!directory || !suffix || !output || output_size == 0)
    return -1;
  directory_length = strlen(directory);
  suffix_length = strlen(suffix);
  while (directory_length > 1 && directory[directory_length - 1] == '/' &&
         suffix_length > 0 && suffix[0] == '/')
    --directory_length;
  while (directory_length > 0 && directory[directory_length - 1] == '/' &&
         suffix_length > 0 && suffix[0] == '/') {
    ++suffix;
    --suffix_length;
  }
  needs_separator = directory_length > 0 &&
                    directory[directory_length - 1] != '/' &&
                    suffix_length > 0 && suffix[0] != '/';
  remaining = output_size - 1;
  if (directory_length > remaining) {
    output[0] = '\0';
    return -1;
  }
  remaining -= directory_length;
  if ((needs_separator && remaining == 0) ||
      suffix_length > remaining - (needs_separator ? 1u : 0u)) {
    output[0] = '\0';
    return -1;
  }
  memcpy(output, directory, directory_length);
  if (needs_separator)
    output[directory_length++] = '/';
  memcpy(output + directory_length, suffix, suffix_length + 1);
  return 0;
}

int nxcompat_join_root(const nxcompat_context *context, const char *device_path,
                       char *host_path, size_t host_path_size) {
  int count;
  const char *root;
  const char *component;

  if (!context || !device_path || device_path[0] != '/' || !host_path ||
      host_path_size == 0)
    return -1;
  component = device_path;
  while (*component) {
    const char *end;
    while (*component == '/')
      ++component;
    if (!*component)
      break;
    end = strchr(component, '/');
    if (!end)
      end = component + strlen(component);
    if ((size_t)(end - component) == 2u && component[0] == '.' &&
        component[1] == '.') {
      host_path[0] = '\0';
      return -1;
    }
    component = end;
  }
  root = context->root[0] ? context->root : "/";
  if (strcmp(root, "/") == 0)
    count = snprintf(host_path, host_path_size, "%s", device_path);
  else
    count = snprintf(host_path, host_path_size, "%s%s", root, device_path);
  if (count < 0 || (size_t)count >= host_path_size) {
    host_path[0] = '\0';
    return -1;
  }
  return 0;
}

static int nxcompat_stat_path(const nxcompat_context *context,
                              const char *device_path, struct stat *status) {
  char host_path[NXCOMPAT_PATH_MAX * 2u];
  if (nxcompat_join_root(context, device_path, host_path, sizeof(host_path)) !=
      0)
    return 0;
  return stat(host_path, status) == 0;
}

int nxcompat_path_exists(const nxcompat_context *context,
                         const char *device_path) {
  struct stat status;
  return nxcompat_stat_path(context, device_path, &status);
}

int nxcompat_directory_exists(const nxcompat_context *context,
                              const char *device_path) {
  struct stat status;
  return nxcompat_stat_path(context, device_path, &status) &&
         S_ISDIR(status.st_mode);
}

int nxcompat_directory_usable(const nxcompat_context *context,
                              const char *device_path) {
  char host_path[NXCOMPAT_PATH_MAX * 2u];
  return nxcompat_directory_exists(context, device_path) &&
         nxcompat_join_root(context, device_path, host_path,
                            sizeof(host_path)) == 0 &&
         access(host_path, W_OK | X_OK) == 0;
}

int nxcompat_socket_exists(const nxcompat_context *context,
                           const char *device_path) {
  struct stat status;
  return nxcompat_stat_path(context, device_path, &status) &&
         S_ISSOCK(status.st_mode);
}

int nxcompat_socket_connectable(const nxcompat_context *context,
                                const char *device_path) {
  char host_path[NXCOMPAT_PATH_MAX * 2u];
  struct sockaddr_un address;
  struct pollfd poll_descriptor;
  socklen_t error_size;
  int descriptor;
  int flags;
  int socket_error = 0;
  int status;
  if (!nxcompat_socket_exists(context, device_path) ||
      nxcompat_join_root(context, device_path, host_path, sizeof(host_path)) !=
          0 ||
      strlen(host_path) >= sizeof(address.sun_path))
    return 0;
  descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
  if (descriptor < 0)
    return 0;
  flags = fcntl(descriptor, F_GETFL, 0);
  if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
    (void)close(descriptor);
    return 0;
  }
  (void)fcntl(descriptor, F_SETFD, FD_CLOEXEC);
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  nxcompat_copy_string(address.sun_path, sizeof(address.sun_path), host_path);
  status = connect(descriptor, (const struct sockaddr *)&address,
                   sizeof(address));
  if (status != 0 && errno != EINPROGRESS && errno != EAGAIN) {
    (void)close(descriptor);
    return 0;
  }
  if (status != 0) {
    memset(&poll_descriptor, 0, sizeof(poll_descriptor));
    poll_descriptor.fd = descriptor;
    poll_descriptor.events = POLLOUT;
    status = poll(&poll_descriptor, 1, 50);
    error_size = sizeof(socket_error);
    if (status <= 0 ||
        getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error,
                   &error_size) != 0 ||
        socket_error != 0) {
      (void)close(descriptor);
      return 0;
    }
  }
  (void)close(descriptor);
  return 1;
}

int nxcompat_regular_file_exists(const nxcompat_context *context,
                                 const char *device_path) {
  struct stat status;
  return nxcompat_stat_path(context, device_path, &status) &&
         S_ISREG(status.st_mode);
}

static int nxcompat_bounded_string(const char *value, size_t size) {
  return value && size != 0 && memchr(value, '\0', size) != NULL;
}

int nxcompat_host_instance_valid(const nxcompat_host *host) {
  if (!host || !nxcompat_api_version_supported(host->api_version) ||
      host->struct_size < sizeof(*host) ||
      host->process_arch < NXCOMPAT_ARCH_UNKNOWN ||
      host->process_arch > NXCOMPAT_ARCH_X86_64 ||
      host->kernel_arch < NXCOMPAT_ARCH_UNKNOWN ||
      host->kernel_arch > NXCOMPAT_ARCH_X86_64 ||
      host->memory_class < NXCOMPAT_MEMORY_UNKNOWN ||
      host->memory_class > NXCOMPAT_MEMORY_HIGH ||
      host->filesystem_class < NXCOMPAT_FILESYSTEM_UNKNOWN ||
      host->filesystem_class > NXCOMPAT_FILESYSTEM_NETWORK)
    return 0;
#define NXCOMPAT_CHECK_HOST_STRING(field)                                    \
  if (!nxcompat_bounded_string(host->field, sizeof(host->field)))            \
    return 0
  NXCOMPAT_CHECK_HOST_STRING(port_id);
  NXCOMPAT_CHECK_HOST_STRING(device_model);
  NXCOMPAT_CHECK_HOST_STRING(kernel_machine);
  NXCOMPAT_CHECK_HOST_STRING(libc_version);
  NXCOMPAT_CHECK_HOST_STRING(os_id);
  NXCOMPAT_CHECK_HOST_STRING(os_version);
  NXCOMPAT_CHECK_HOST_STRING(filesystem_type);
  NXCOMPAT_CHECK_HOST_STRING(inherited_video_driver);
  NXCOMPAT_CHECK_HOST_STRING(inherited_audio_driver);
  NXCOMPAT_CHECK_HOST_STRING(game_dir);
  NXCOMPAT_CHECK_HOST_STRING(portmaster_dir);
  NXCOMPAT_CHECK_HOST_STRING(session_runtime_dir);
  NXCOMPAT_CHECK_HOST_STRING(wayland_socket);
  NXCOMPAT_CHECK_HOST_STRING(pulse_socket);
  NXCOMPAT_CHECK_HOST_STRING(pipewire_socket);
  NXCOMPAT_CHECK_HOST_STRING(drm_connector);
  NXCOMPAT_CHECK_HOST_STRING(display_source);
  NXCOMPAT_CHECK_HOST_STRING(armhf_pipewire_modules);
  NXCOMPAT_CHECK_HOST_STRING(armhf_spa_plugins);
  NXCOMPAT_CHECK_HOST_STRING(armhf_alsa_plugins);
  NXCOMPAT_CHECK_HOST_STRING(controller_db);
#undef NXCOMPAT_CHECK_HOST_STRING
  return 1;
}

int nxcompat_plan_instance_valid(const nxcompat_plan *plan) {
  size_t index;
  if (!plan || !nxcompat_api_version_supported(plan->api_version) ||
      plan->struct_size < sizeof(*plan) ||
      plan->runtime_arch < NXCOMPAT_ARCH_UNKNOWN ||
      plan->runtime_arch > NXCOMPAT_ARCH_X86_64 ||
      plan->action_count > NXCOMPAT_MAX_ACTIONS)
    return 0;
  for (index = 0; index < plan->action_count; ++index) {
    const nxcompat_action *action = &plan->actions[index];
    if (action->id < NXCOMPAT_ACTION_NONE ||
        action->id > NXCOMPAT_ACTION_MALLOC_ARENAS ||
        action->state < NXCOMPAT_ACTION_UNAVAILABLE ||
        action->state > NXCOMPAT_ACTION_FAILED ||
        !nxcompat_bounded_string(action->variable,
                                 sizeof(action->variable)) ||
        !nxcompat_bounded_string(action->value, sizeof(action->value)) ||
        !nxcompat_bounded_string(action->reason, sizeof(action->reason)) ||
        (action->state == NXCOMPAT_ACTION_PLANNED && !action->variable[0]))
      return 0;
  }
  return 1;
}

int nxcompat_read_first_line(const nxcompat_context *context,
                             const char *device_path, char *output,
                             size_t output_size) {
  char host_path[NXCOMPAT_PATH_MAX * 2u];
  FILE *stream;

  if (!output || output_size == 0 ||
      nxcompat_join_root(context, device_path, host_path, sizeof(host_path)) !=
          0)
    return -1;
  output[0] = '\0';
  stream = fopen(host_path, "r");
  if (!stream)
    return -1;
  if (!fgets(output, (int)output_size, stream)) {
    (void)fclose(stream);
    output[0] = '\0';
    return -1;
  }
  (void)fclose(stream);
  output[strcspn(output, "\r\n")] = '\0';
  return 0;
}

int nxcompat_parse_display_mode(const char *text, int *width, int *height) {
  const char *cursor;

  if (!text || !width || !height)
    return -1;
  for (cursor = text; *cursor; ++cursor) {
    char *end = NULL;
    long parsed_width;
    long parsed_height;
    if (!isdigit((unsigned char)*cursor))
      continue;
    errno = 0;
    parsed_width = strtol(cursor, &end, 10);
    if (errno != 0 || !end || (*end != 'x' && *end != 'X'))
      continue;
    cursor = end + 1;
    errno = 0;
    parsed_height = strtol(cursor, &end, 10);
    if (errno == 0 && end != cursor && parsed_width >= 160 &&
        parsed_width <= 16384 && parsed_height >= 120 &&
        parsed_height <= 16384) {
      *width = (int)parsed_width;
      *height = (int)parsed_height;
      return 0;
    }
    if (!*cursor)
      break;
  }
  return -1;
}

const char *nxcompat_arch_name(nxcompat_arch value) {
  switch (value) {
  case NXCOMPAT_ARCH_ARMV7:
    return "armv7";
  case NXCOMPAT_ARCH_AARCH64:
    return "aarch64";
  case NXCOMPAT_ARCH_I386:
    return "i386";
  case NXCOMPAT_ARCH_X86_64:
    return "x86_64";
  default:
    return "unknown";
  }
}

const char *nxcompat_memory_class_name(nxcompat_memory_class value) {
  switch (value) {
  case NXCOMPAT_MEMORY_SHORT:
    return "short";
  case NXCOMPAT_MEMORY_MEDIUM:
    return "medium";
  case NXCOMPAT_MEMORY_HIGH:
    return "high";
  default:
    return "unknown";
  }
}

const char *nxcompat_filesystem_class_name(nxcompat_filesystem_class value) {
  switch (value) {
  case NXCOMPAT_FILESYSTEM_POSIX:
    return "posix";
  case NXCOMPAT_FILESYSTEM_FUSE_LIKE:
    return "fuse-like";
  case NXCOMPAT_FILESYSTEM_NETWORK:
    return "network";
  default:
    return "unknown";
  }
}

const char *nxcompat_action_name(nxcompat_action_id value) {
  switch (value) {
  case NXCOMPAT_ACTION_SESSION_RUNTIME:
    return "session-runtime";
  case NXCOMPAT_ACTION_PULSE_SERVER:
    return "pulse-socket";
  case NXCOMPAT_ACTION_PIPEWIRE_MODULE_DIR:
    return "armhf-pipewire-modules";
  case NXCOMPAT_ACTION_SPA_PLUGIN_DIR:
    return "armhf-spa-plugins";
  case NXCOMPAT_ACTION_ALSA_PLUGIN_DIR:
    return "armhf-alsa-plugins";
  case NXCOMPAT_ACTION_CONTROLLER_DB:
    return "portmaster-controller-db";
  case NXCOMPAT_ACTION_XBOX_BUTTON_LABELS:
    return "xbox-button-layout";
  case NXCOMPAT_ACTION_MALLOC_ARENAS:
    return "low-memory-arenas";
  default:
    return "none";
  }
}

const char *nxcompat_action_state_name(nxcompat_action_state value) {
  switch (value) {
  case NXCOMPAT_ACTION_NOT_NEEDED:
    return "not-needed";
  case NXCOMPAT_ACTION_PLANNED:
    return "planned";
  case NXCOMPAT_ACTION_APPLIED:
    return "applied";
  case NXCOMPAT_ACTION_FAILED:
    return "failed";
  default:
    return "unavailable";
  }
}
