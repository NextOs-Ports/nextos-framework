/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxcompat_internal.h"
#include "nxcompat_registry_internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct nxcompat_buffer {
  char *data;
  size_t size;
  size_t used;
  int failed;
} nxcompat_buffer;

#if defined(__GNUC__) || defined(__clang__)
static void nxcompat_append(nxcompat_buffer *buffer, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
#endif
static void nxcompat_append(nxcompat_buffer *buffer, const char *format, ...) {
  va_list arguments;
  int count;
  if (!buffer || buffer->failed || buffer->used >= buffer->size)
    return;
  va_start(arguments, format);
  count = vsnprintf(buffer->data + buffer->used, buffer->size - buffer->used,
                    format, arguments);
  va_end(arguments);
  if (count < 0 || (size_t)count >= buffer->size - buffer->used) {
    buffer->failed = 1;
    if (buffer->size)
      buffer->data[buffer->size - 1] = '\0';
    return;
  }
  buffer->used += (size_t)count;
}

static const char *nxcompat_display_name(const nxcompat_host *host) {
  if ((host->capabilities & NXCOMPAT_CAP_WAYLAND) != 0 &&
      (host->capabilities & NXCOMPAT_CAP_DRM) != 0)
    return "wayland+drm";
  if ((host->capabilities & NXCOMPAT_CAP_WAYLAND) != 0)
    return "wayland";
  if ((host->capabilities & NXCOMPAT_CAP_DRM_CONNECTED) != 0)
    return "drm";
  if ((host->capabilities & NXCOMPAT_CAP_FBDEV) != 0)
    return "fbdev";
  if ((host->capabilities & NXCOMPAT_CAP_X11) != 0)
    return "x11";
  return "unknown";
}

static void nxcompat_append_audio(nxcompat_buffer *buffer,
                                  const nxcompat_host *host) {
  int needs_separator = 0;
  if ((host->capabilities & NXCOMPAT_CAP_PIPEWIRE_SOCKET) != 0) {
    nxcompat_append(buffer, "pipewire");
    needs_separator = 1;
  }
  if ((host->capabilities & NXCOMPAT_CAP_PULSE_SOCKET) != 0) {
    nxcompat_append(buffer, "%spulse", needs_separator ? "+" : "");
    needs_separator = 1;
  }
  if ((host->capabilities & NXCOMPAT_CAP_ALSA) != 0) {
    nxcompat_append(buffer, "%salsa", needs_separator ? "+" : "");
    needs_separator = 1;
  }
  if (!needs_separator)
    nxcompat_append(buffer, "unknown");
}

int nxcompat_format_device_line(const nxcompat_host *host, char *output,
                                size_t output_size) {
  nxcompat_buffer buffer;
  if (!nxcompat_host_instance_valid(host) || !output || output_size == 0)
    return -1;
  memset(&buffer, 0, sizeof(buffer));
  buffer.data = output;
  buffer.size = output_size;
  output[0] = '\0';
  if (host->device_model[0])
    nxcompat_append(&buffer, "%s | ", host->device_model);
  nxcompat_append(&buffer, "%s%s%s | runtime %s",
                  host->os_id[0] ? host->os_id : "linux",
                  host->os_version[0] ? " " : "",
                  host->os_version[0] ? host->os_version : "",
                  nxcompat_arch_name(host->process_arch));
  if (host->kernel_arch != NXCOMPAT_ARCH_UNKNOWN &&
      host->kernel_arch != host->process_arch)
    nxcompat_append(&buffer, " on %s kernel",
                    nxcompat_arch_name(host->kernel_arch));
  nxcompat_append(&buffer, " | display %s", nxcompat_display_name(host));
  if (host->display_width > 0 && host->display_height > 0)
    nxcompat_append(&buffer, " %dx%d", host->display_width,
                    host->display_height);
  nxcompat_append(&buffer, " | audio ");
  nxcompat_append_audio(&buffer, host);
  nxcompat_append(&buffer, " | memory %s | fs %s",
                  nxcompat_memory_class_name(host->memory_class),
                  host->filesystem_type[0] ? host->filesystem_type : "unknown");
  return buffer.failed ? -1 : (int)buffer.used;
}

int nxcompat_format_fix_line(const nxcompat_plan *plan, char *output,
                             size_t output_size) {
  nxcompat_buffer buffer;
  size_t index;
  int emitted = 0;
  if (!nxcompat_plan_instance_valid(plan) || !output || output_size == 0)
    return -1;
  memset(&buffer, 0, sizeof(buffer));
  buffer.data = output;
  buffer.size = output_size;
  output[0] = '\0';
  for (index = 0; index < plan->action_count; ++index) {
    const nxcompat_action *action = &plan->actions[index];
    if (action->state != NXCOMPAT_ACTION_PLANNED &&
        action->state != NXCOMPAT_ACTION_APPLIED &&
        action->state != NXCOMPAT_ACTION_FAILED)
      continue;
    nxcompat_append(&buffer, "%s%s:%s", emitted ? ", " : "",
                    nxcompat_action_name(action->id),
                    nxcompat_action_state_name(action->state));
    emitted = 1;
  }
  if (!emitted)
    nxcompat_append(&buffer, "no compatibility adjustment needed");
  return buffer.failed ? -1 : (int)buffer.used;
}

int nxcompat_emit_startup_status(const nxcompat_host *host,
                                 const nxcompat_plan *plan,
                                 nxcompat_status_callback callback,
                                 void *userdata) {
  char line[1024];
  if (!nxcompat_host_instance_valid(host) ||
      (plan && !nxcompat_plan_instance_valid(plan)) || !callback)
    return -1;
  if (nxcompat_format_device_line(host, line, sizeof(line)) < 0)
    return -1;
  callback(userdata, NXCOMPAT_STATUS_DEVICE, line);
  if (plan) {
    if (nxcompat_format_fix_line(plan, line, sizeof(line)) < 0)
      return -1;
    callback(userdata, NXCOMPAT_STATUS_FIX, line);
  }
  return 0;
}

static void nxcompat_append_json_string(nxcompat_buffer *buffer,
                                        const char *value) {
  const unsigned char *cursor = (const unsigned char *)(value ? value : "");
  nxcompat_append(buffer, "\"");
  while (*cursor && !buffer->failed) {
    switch (*cursor) {
    case '\"':
      nxcompat_append(buffer, "\\\"");
      break;
    case '\\':
      nxcompat_append(buffer, "\\\\");
      break;
    case '\b':
      nxcompat_append(buffer, "\\b");
      break;
    case '\f':
      nxcompat_append(buffer, "\\f");
      break;
    case '\n':
      nxcompat_append(buffer, "\\n");
      break;
    case '\r':
      nxcompat_append(buffer, "\\r");
      break;
    case '\t':
      nxcompat_append(buffer, "\\t");
      break;
    default:
      if (*cursor < 0x20u)
        nxcompat_append(buffer, "\\u%04x", (unsigned)*cursor);
      else
        nxcompat_append(buffer, "%c", (char)*cursor);
      break;
    }
    ++cursor;
  }
  nxcompat_append(buffer, "\"");
}

static void nxcompat_append_json_key_string(nxcompat_buffer *buffer,
                                            const char *key,
                                            const char *value) {
  nxcompat_append_json_string(buffer, key);
  nxcompat_append(buffer, ":");
  nxcompat_append_json_string(buffer, value);
}

int nxcompat_format_json(const nxcompat_host *host, const nxcompat_plan *plan,
                         char *output, size_t output_size) {
  nxcompat_buffer buffer;
  size_t index;
  if (!nxcompat_host_instance_valid(host) ||
      (plan && !nxcompat_plan_instance_valid(plan)) || !output ||
      output_size == 0)
    return -1;
  memset(&buffer, 0, sizeof(buffer));
  buffer.data = output;
  buffer.size = output_size;
  output[0] = '\0';

  nxcompat_append(&buffer, "{");
  nxcompat_append_json_key_string(&buffer, "nxcompat_version",
                                  NXCOMPAT_VERSION);
  nxcompat_append(&buffer, ",");
  nxcompat_append_json_key_string(&buffer, "port_id", host->port_id);
  nxcompat_append(&buffer, ",");
  nxcompat_append_json_key_string(&buffer, "device_model", host->device_model);
  nxcompat_append(&buffer, ",");
  nxcompat_append_json_key_string(&buffer, "os_id", host->os_id);
  nxcompat_append(&buffer, ",");
  nxcompat_append_json_key_string(&buffer, "os_version", host->os_version);
  nxcompat_append(&buffer, ",");
  nxcompat_append_json_key_string(&buffer, "process_arch",
                                  nxcompat_arch_name(host->process_arch));
  nxcompat_append(&buffer, ",");
  nxcompat_append_json_key_string(&buffer, "kernel_arch",
                                  nxcompat_arch_name(host->kernel_arch));
  nxcompat_append(&buffer, ",");
  nxcompat_append_json_key_string(&buffer, "libc", host->libc_version);
  nxcompat_append(&buffer, ",");
  nxcompat_append_json_key_string(&buffer, "display",
                                  nxcompat_display_name(host));
  nxcompat_append(&buffer,
                  ",\"display_width\":%d,\"display_height\":%d,"
                  "\"memory_kib\":%llu,",
                  host->display_width, host->display_height,
                  (unsigned long long)host->memory_total_kib);
  nxcompat_append_json_key_string(
      &buffer, "memory_class", nxcompat_memory_class_name(host->memory_class));
  nxcompat_append(&buffer, ",");
  nxcompat_append_json_key_string(&buffer, "filesystem",
                                  host->filesystem_type);
  nxcompat_append(&buffer,
                  ",\"capabilities\":%llu,\"audio\":{"
                  "\"pipewire_socket\":%s,\"pulse_socket\":%s,"
                  "\"alsa\":%s},\"actions\":[",
                  (unsigned long long)host->capabilities,
                  (host->capabilities & NXCOMPAT_CAP_PIPEWIRE_SOCKET) ? "true"
                                                                       : "false",
                  (host->capabilities & NXCOMPAT_CAP_PULSE_SOCKET) ? "true"
                                                                    : "false",
                  (host->capabilities & NXCOMPAT_CAP_ALSA) ? "true" : "false");
  if (plan) {
    for (index = 0; index < plan->action_count; ++index) {
      const nxcompat_action *action = &plan->actions[index];
      if (index)
        nxcompat_append(&buffer, ",");
      nxcompat_append(&buffer, "{");
      nxcompat_append_json_key_string(&buffer, "id",
                                      nxcompat_action_name(action->id));
      nxcompat_append(&buffer, ",");
      nxcompat_append_json_key_string(
          &buffer, "state", nxcompat_action_state_name(action->state));
      nxcompat_append(&buffer, ",");
      nxcompat_append_json_key_string(&buffer, "variable", action->variable);
      nxcompat_append(&buffer, ",");
      nxcompat_append_json_key_string(&buffer, "reason", action->reason);
      nxcompat_append(&buffer, "}");
    }
  }
  nxcompat_append(&buffer, "]}");
  return buffer.failed ? -1 : (int)buffer.used;
}

static int nxcompat_contains_case_insensitive(const char *text,
                                              const char *wanted) {
  size_t text_length;
  size_t wanted_length;
  size_t start;
  size_t index;
  if (!text || !wanted)
    return 0;
  text_length = strlen(text);
  wanted_length = strlen(wanted);
  if (wanted_length == 0u || wanted_length > text_length)
    return 0;
  for (start = 0u; start + wanted_length <= text_length; ++start) {
    for (index = 0u; index < wanted_length; ++index)
      if (tolower((unsigned char)text[start + index]) !=
          tolower((unsigned char)wanted[index]))
        break;
    if (index == wanted_length)
      return 1;
  }
  return 0;
}

static int nxcompat_contains_ipv4_shape(const char *text) {
  const unsigned char *cursor = (const unsigned char *)text;
  if (!cursor)
    return 0;
  while (*cursor) {
    const unsigned char *candidate = cursor;
    unsigned group;
    if (!isdigit(*candidate) ||
        (candidate != (const unsigned char *)text &&
         isdigit(candidate[-1]))) {
      ++cursor;
      continue;
    }
    for (group = 0u; group < 4u; ++group) {
      unsigned digits = 0u;
      unsigned value = 0u;
      while (digits < 3u && isdigit(*candidate)) {
        value = value * 10u + (unsigned)(*candidate - (unsigned char)'0');
        ++candidate;
        ++digits;
      }
      if (digits == 0u || value > 255u || isdigit(*candidate))
        break;
      if (group < 3u) {
        if (*candidate != (unsigned char)'.')
          break;
        ++candidate;
      }
    }
    if (group == 4u && !isdigit(*candidate))
      return 1;
    ++cursor;
  }
  return 0;
}

static int nxcompat_label_is_sensitive(const char *value) {
  static const char *const patterns[] = {
      "/home/",       "/var/home/",  "/root/",     "/mnt/",
      "/tmp/",        "/storage/",   "/users/",    "\\users\\",
      "file://",      "://",         "::",          "password",
      "passwd",       "credential",  "secret",      "token",
      "api-key",      "api_key",     "apikey",      "private-key",
      "private_key",  "bearer ",     "key=",        "-----begin",
      NULL};
  size_t index;
  if (!value)
    return 0;
  if (strchr(value, '@') != NULL || nxcompat_contains_ipv4_shape(value))
    return 1;
  for (index = 0u; patterns[index]; ++index)
    if (nxcompat_contains_case_insensitive(value, patterns[index]))
      return 1;
  return 0;
}

static void nxcompat_sanitize_public_label(const char *value, char *output,
                                           size_t output_size) {
  size_t read_index;
  size_t write_index = 0u;
  if (!output || output_size == 0u)
    return;
  output[0] = '\0';
  if (!value)
    value = "";
  if (nxcompat_label_is_sensitive(value)) {
    (void)snprintf(output, output_size, "%s", "[redacted]");
    return;
  }
  for (read_index = 0u; value[read_index] && write_index + 1u < output_size;
       ++read_index) {
    unsigned char character = (unsigned char)value[read_index];
    if (character < 0x20u || character == 0x7fu || character > 0x7eu)
      character = (unsigned char)'?';
    output[write_index++] = (char)character;
  }
  while (write_index > 0u && output[write_index - 1u] == ' ')
    --write_index;
  output[write_index] = '\0';
}

static void nxcompat_sanitize_field(char *value, size_t value_size) {
  char sanitized[NXCOMPAT_RECEIPT_EXTENSIONS_MAX];
  if (!value || value_size == 0u || value_size > sizeof(sanitized))
    return;
  memset(sanitized, 0, sizeof(sanitized));
  nxcompat_sanitize_public_label(value, sanitized, value_size);
  memcpy(value, sanitized, value_size);
}

void nxcompat_runtime_report_sanitize(nxcompat_runtime_report *report) {
  if (!report)
    return;
  if (report->has_graphics) {
    nxcompat_sanitize_field(report->graphics.video_backend,
                            sizeof(report->graphics.video_backend));
    nxcompat_sanitize_field(report->graphics.gl_vendor,
                            sizeof(report->graphics.gl_vendor));
    nxcompat_sanitize_field(report->graphics.gl_renderer,
                            sizeof(report->graphics.gl_renderer));
    nxcompat_sanitize_field(report->graphics.gl_version,
                            sizeof(report->graphics.gl_version));
    nxcompat_sanitize_field(report->graphics.glsl_version,
                            sizeof(report->graphics.glsl_version));
    /* Raw extension text is unnecessary once finite texture capabilities have
     * been derived and is intentionally excluded from aggregated reports. */
    memset(report->graphics.gl_extensions, 0,
           sizeof(report->graphics.gl_extensions));
    nxcompat_sanitize_field(report->graphics.egl_vendor,
                            sizeof(report->graphics.egl_vendor));
    nxcompat_sanitize_field(report->graphics.egl_version,
                            sizeof(report->graphics.egl_version));
    nxcompat_sanitize_field(report->graphics.egl_client_apis,
                            sizeof(report->graphics.egl_client_apis));
  }
  if (report->has_audio)
    nxcompat_sanitize_field(report->audio.backend,
                            sizeof(report->audio.backend));
}

static int nxcompat_runtime_plan_valid(const nxcompat_plan_v2 *plan) {
  size_t index;
  if (!plan)
    return 1;
  if (plan->api_version != NXCOMPAT_API_VERSION_V2 ||
      plan->struct_size < sizeof(*plan) ||
      plan->runtime_arch < NXCOMPAT_ARCH_UNKNOWN ||
      plan->runtime_arch > NXCOMPAT_ARCH_X86_64 ||
      plan->action_count > NXCOMPAT_MAX_ACTIONS)
    return 0;
  for (index = 0u; index < plan->action_count; ++index) {
    const nxcompat_action_v2 *action = &plan->actions[index];
    if (action->id <= NXCOMPAT_ACTION_NONE ||
        action->id > NXCOMPAT_ACTION_MALLOC_ARENAS ||
        action->state < NXCOMPAT_ACTION_V2_UNAVAILABLE ||
        action->state > NXCOMPAT_ACTION_V2_ROLLBACK_FAILED ||
        strcmp(nxcompat_reason_name(action->reason_code), "unknown") == 0)
      return 0;
  }
  return 1;
}

static int nxcompat_runtime_report_valid(
    const nxcompat_runtime_report *report) {
  size_t index;
  size_t satisfied_count = 0u;
  size_t pending_count = 0u;
  size_t missing_count = 0u;
  const nxcompat_requirement_report *requirements;
  if (!report || report->api_version != NXCOMPAT_API_VERSION ||
      report->struct_size < sizeof(*report) ||
      report->phase < NXCOMPAT_PHASE_PREFLIGHT ||
      report->phase > NXCOMPAT_PHASE_READY ||
      report->evidence_count != NXCOMPAT_CAPABILITY_COUNT ||
      (report->has_graphics != 0 && report->has_graphics != 1) ||
      (report->has_audio != 0 && report->has_audio != 1) ||
      (report->has_input != 0 && report->has_input != 1))
    return 0;
  for (index = 0u; index < report->evidence_count; ++index) {
    const nxcompat_capability_evidence *evidence = &report->evidence[index];
    const nxcompat_capability_definition *definition =
        nxcompat_capability_definition_by_id(evidence->capability_id);
    if (evidence->capability_id != index ||
        !definition || evidence->phase != definition->phase ||
        evidence->state < NXCOMPAT_EVIDENCE_ABSENT ||
        evidence->state > NXCOMPAT_EVIDENCE_LOST ||
        evidence->phase < NXCOMPAT_PHASE_PREFLIGHT ||
        evidence->phase > NXCOMPAT_PHASE_READY ||
        evidence->source < NXCOMPAT_SOURCE_PROBE ||
        evidence->source > NXCOMPAT_SOURCE_ENGINE_ADAPTER ||
        strcmp(nxcompat_reason_name(evidence->reason), "unknown") == 0)
      return 0;
  }
  requirements = &report->requirements;
  if (requirements->api_version != NXCOMPAT_API_VERSION ||
      requirements->struct_size < sizeof(*requirements) ||
      requirements->phase != report->phase ||
      requirements->count > NXCOMPAT_MAX_REQUIREMENTS ||
      requirements->satisfied_count + requirements->pending_count +
              requirements->missing_count !=
          requirements->count)
    return 0;
  for (index = 0u; index < requirements->count; ++index) {
    const nxcompat_requirement_result *result =
        &requirements->results[index];
    const nxcompat_capability_definition *definition;
    const nxcompat_capability_evidence *evidence;
    nxcompat_requirement_state expected_state;
    nxcompat_reason_code expected_reason;
    size_t prior;
    if (result->capability_id >= NXCOMPAT_CAPABILITY_COUNT ||
        result->state < NXCOMPAT_REQUIREMENT_PENDING ||
        result->state > NXCOMPAT_REQUIREMENT_MISSING)
      return 0;
    for (prior = 0u; prior < index; ++prior)
      if (requirements->results[prior].capability_id == result->capability_id)
        return 0;
    definition = nxcompat_capability_definition_by_id(result->capability_id);
    evidence = &report->evidence[result->capability_id];
    if (report->phase < definition->phase) {
      expected_state = NXCOMPAT_REQUIREMENT_PENDING;
      expected_reason = NXCOMPAT_REASON_REQUIREMENT_PENDING;
      ++pending_count;
    } else if (nxcompat_registry_evidence_satisfies(
                   evidence->state, definition->minimum_evidence)) {
      expected_state = NXCOMPAT_REQUIREMENT_SATISFIED;
      expected_reason = NXCOMPAT_REASON_REQUIREMENT_SATISFIED;
      ++satisfied_count;
    } else {
      expected_state = NXCOMPAT_REQUIREMENT_MISSING;
      expected_reason = NXCOMPAT_REASON_REQUIREMENT_MISSING;
      ++missing_count;
    }
    if (result->state != expected_state || result->reason != expected_reason)
      return 0;
  }
  if (requirements->satisfied_count != satisfied_count ||
      requirements->pending_count != pending_count ||
      requirements->missing_count != missing_count ||
      requirements->final_reason !=
          (missing_count != 0u
               ? NXCOMPAT_REASON_REQUIREMENT_MISSING
               : (pending_count != 0u
                      ? NXCOMPAT_REASON_REQUIREMENT_PENDING
                      : NXCOMPAT_REASON_REQUIREMENT_SATISFIED)))
    return 0;
  if (report->has_graphics &&
      (report->graphics.api_version != NXCOMPAT_API_VERSION ||
       report->graphics.struct_size < sizeof(report->graphics) ||
       report->graphics.generation == 0u ||
       (report->graphics.source != NXCOMPAT_SOURCE_NXGL &&
        report->graphics.source != NXCOMPAT_SOURCE_ENGINE_ADAPTER) ||
       (report->graphics.proof_flags &
        ~(uint32_t)(NXCOMPAT_GRAPHICS_PROOF_WINDOW_CREATED |
                    NXCOMPAT_GRAPHICS_PROOF_CONTEXT_CURRENT |
                    NXCOMPAT_GRAPHICS_PROOF_GL_STRINGS_REAL |
                    NXCOMPAT_GRAPHICS_PROOF_EGL_DISPLAY_CURRENT |
                    NXCOMPAT_GRAPHICS_PROOF_EGL_CONTEXT_CURRENT |
                    NXCOMPAT_GRAPHICS_PROOF_EGL_CONFIG_QUERIED |
                    NXCOMPAT_GRAPHICS_PROOF_DRAWABLE_POSITIVE)) != 0u ||
       !nxcompat_registry_bounded_string(
           report->graphics.video_backend,
           sizeof(report->graphics.video_backend)) ||
       !nxcompat_registry_bounded_string(report->graphics.gl_vendor,
                                         sizeof(report->graphics.gl_vendor)) ||
       !nxcompat_registry_bounded_string(
           report->graphics.gl_renderer,
           sizeof(report->graphics.gl_renderer)) ||
       !nxcompat_registry_bounded_string(
           report->graphics.gl_version,
           sizeof(report->graphics.gl_version)) ||
       !nxcompat_registry_bounded_string(
           report->graphics.glsl_version,
           sizeof(report->graphics.glsl_version)) ||
       !nxcompat_registry_bounded_string(
           report->graphics.gl_extensions,
           sizeof(report->graphics.gl_extensions)) ||
       !nxcompat_registry_bounded_string(
           report->graphics.egl_vendor,
           sizeof(report->graphics.egl_vendor)) ||
       !nxcompat_registry_bounded_string(
           report->graphics.egl_version,
           sizeof(report->graphics.egl_version)) ||
       !nxcompat_registry_bounded_string(
           report->graphics.egl_client_apis,
           sizeof(report->graphics.egl_client_apis))))
    return 0;
  if (report->has_audio &&
      (report->audio.api_version != NXCOMPAT_API_VERSION ||
       report->audio.struct_size < sizeof(report->audio) ||
       report->audio.generation == 0u ||
       (report->audio.source != NXCOMPAT_SOURCE_SDL2_AUDIO &&
        report->audio.source != NXCOMPAT_SOURCE_ENGINE_ADAPTER) ||
       report->audio.lifetime < NXCOMPAT_AUDIO_OPENED_THEN_CLOSED ||
       report->audio.lifetime > NXCOMPAT_AUDIO_ACTIVE_ENGINE_OWNED ||
       !nxcompat_registry_bounded_string(report->audio.backend,
                                         sizeof(report->audio.backend))))
    return 0;
  if (report->has_input &&
      (report->input.api_version != NXCOMPAT_API_VERSION ||
       report->input.struct_size < sizeof(report->input) ||
       report->input.topology_generation == 0u ||
       (report->input.source != NXCOMPAT_SOURCE_NXINPUT &&
        report->input.source != NXCOMPAT_SOURCE_ENGINE_ADAPTER) ||
       report->input.connected_count > 4u ||
       report->input.mapping_source < NXCOMPAT_INPUT_MAPPING_NONE ||
       report->input.mapping_source > NXCOMPAT_INPUT_MAPPING_ENGINE ||
       report->input.last_change < NXCOMPAT_INPUT_CHANGE_NONE ||
       report->input.last_change > NXCOMPAT_INPUT_CHANGE_RESCAN))
    return 0;
  return 1;
}

static const char *
nxcompat_action_state_v2_name(nxcompat_action_state_v2 state) {
  static const char *const names[] = {"unavailable", "not-needed", "planned",
                                      "applied", "failed", "rolled-back",
                                      "rollback-failed"};
  if (state < NXCOMPAT_ACTION_V2_UNAVAILABLE ||
      state > NXCOMPAT_ACTION_V2_ROLLBACK_FAILED)
    return "unknown";
  return names[(unsigned)state];
}

static void nxcompat_append_sanitized_json(nxcompat_buffer *buffer,
                                           const char *key,
                                           const char *value) {
  char sanitized[NXCOMPAT_DETAIL_MAX];
  nxcompat_sanitize_public_label(value, sanitized, sizeof(sanitized));
  nxcompat_append_json_key_string(buffer, key, sanitized);
}

int nxcompat_format_runtime_json(const nxcompat_host *host,
                                 const nxcompat_plan_v2 *plan,
                                 const nxcompat_runtime_report *report,
                                 char *output, size_t output_size) {
  nxcompat_buffer buffer;
  size_t index;
  if (!nxcompat_host_instance_valid(host) ||
      !nxcompat_runtime_plan_valid(plan) ||
      !nxcompat_runtime_report_valid(report) || !output || output_size == 0u)
    return -1;
  memset(&buffer, 0, sizeof(buffer));
  buffer.data = output;
  buffer.size = output_size;
  output[0] = '\0';

  nxcompat_append(&buffer,
                  "{\"nxcompat_version\":\"%s\",\"api_version\":%u,"
                  "\"sanitized\":true,\"report_reason_code\":%d,"
                  "\"host\":{",
                  NXCOMPAT_VERSION, NXCOMPAT_API_VERSION,
                  (int)NXCOMPAT_REASON_REPORT_SANITIZED);
  nxcompat_append_sanitized_json(&buffer, "port_id", host->port_id);
  nxcompat_append(&buffer, ",");
  nxcompat_append_sanitized_json(&buffer, "device_model", host->device_model);
  nxcompat_append(&buffer, ",");
  nxcompat_append_sanitized_json(&buffer, "os_id", host->os_id);
  nxcompat_append(&buffer, ",");
  nxcompat_append_sanitized_json(&buffer, "os_version", host->os_version);
  nxcompat_append(&buffer, ",");
  nxcompat_append_sanitized_json(&buffer, "libc", host->libc_version);
  nxcompat_append(&buffer,
                  ",\"process_arch\":\"%s\",\"kernel_arch\":\"%s\","
                  "\"memory_kib\":%llu,\"memory_class\":\"%s\","
                  "\"filesystem_class\":\"%s\"},\"phase\":\"%s\","
                  "\"capabilities\":[",
                  nxcompat_arch_name(host->process_arch),
                  nxcompat_arch_name(host->kernel_arch),
                  (unsigned long long)host->memory_total_kib,
                  nxcompat_memory_class_name(host->memory_class),
                  nxcompat_filesystem_class_name(host->filesystem_class),
                  nxcompat_phase_name(report->phase));
  for (index = 0u; index < report->evidence_count; ++index) {
    const nxcompat_capability_evidence *evidence = &report->evidence[index];
    const nxcompat_capability_definition *definition =
        nxcompat_capability_definition_by_id(evidence->capability_id);
    nxcompat_append(
        &buffer,
        "%s{\"numeric_id\":%u,\"id\":\"%s\",\"state\":\"%s\","
        "\"phase\":\"%s\",\"source\":\"%s\",\"reason_code\":%d,"
        "\"generation\":%llu}",
        index ? "," : "", (unsigned)evidence->capability_id,
        definition->name, nxcompat_evidence_state_name(evidence->state),
        nxcompat_phase_name(evidence->phase),
        nxcompat_source_name(evidence->source), (int)evidence->reason,
        (unsigned long long)evidence->generation);
  }
  nxcompat_append(
      &buffer,
      "],\"requirements\":{\"final_reason_code\":%d,\"satisfied\":%llu,"
      "\"pending\":%llu,\"missing\":%llu,\"entries\":[",
      (int)report->requirements.final_reason,
      (unsigned long long)report->requirements.satisfied_count,
      (unsigned long long)report->requirements.pending_count,
      (unsigned long long)report->requirements.missing_count);
  for (index = 0u; index < report->requirements.count; ++index) {
    const nxcompat_requirement_result *result =
        &report->requirements.results[index];
    const nxcompat_capability_definition *definition =
        nxcompat_capability_definition_by_id(result->capability_id);
    nxcompat_append(
        &buffer,
        "%s{\"id\":\"%s\",\"state\":\"%s\",\"reason_code\":%d}",
        index ? "," : "", definition->name,
        nxcompat_requirement_state_name(result->state), (int)result->reason);
  }
  nxcompat_append(&buffer, "]},\"receipts\":{\"graphics\":");
  if (!report->has_graphics) {
    nxcompat_append(&buffer, "null");
  } else {
    char backend[NXCOMPAT_NAME_MAX];
    char vendor[NXCOMPAT_DETAIL_MAX];
    char renderer[NXCOMPAT_DETAIL_MAX];
    char gl_version[NXCOMPAT_DETAIL_MAX];
    char glsl_version[NXCOMPAT_DETAIL_MAX];
    char egl_vendor[NXCOMPAT_DETAIL_MAX];
    char egl_version[NXCOMPAT_DETAIL_MAX];
    char egl_apis[NXCOMPAT_DETAIL_MAX];
    const nxcompat_graphics_receipt *graphics = &report->graphics;
    nxcompat_sanitize_public_label(graphics->video_backend, backend,
                                   sizeof(backend));
    nxcompat_sanitize_public_label(graphics->gl_vendor, vendor, sizeof(vendor));
    nxcompat_sanitize_public_label(graphics->gl_renderer, renderer,
                                   sizeof(renderer));
    nxcompat_sanitize_public_label(graphics->gl_version, gl_version,
                                   sizeof(gl_version));
    nxcompat_sanitize_public_label(graphics->glsl_version, glsl_version,
                                   sizeof(glsl_version));
    nxcompat_sanitize_public_label(graphics->egl_vendor, egl_vendor,
                                   sizeof(egl_vendor));
    nxcompat_sanitize_public_label(graphics->egl_version, egl_version,
                                   sizeof(egl_version));
    nxcompat_sanitize_public_label(graphics->egl_client_apis, egl_apis,
                                   sizeof(egl_apis));
    nxcompat_append(
        &buffer,
        "{\"source\":\"%s\",\"generation\":%llu,\"proof_flags\":%u,"
        "\"window\":[%d,%d],\"drawable\":[%d,%d],\"gles\":[%d,%d],"
        "\"rgba\":[%d,%d,%d,%d],\"depth\":%d,\"stencil\":%d,"
        "\"double_buffer\":%s,\"profile_mask\":%d,\"egl_config\":{"
        "\"id\":%d,\"rgba\":[%d,%d,%d,%d],\"depth\":%d,"
        "\"stencil\":%d,\"renderable_type\":%d,\"surface_type\":%d},"
        "\"backend\":",
        nxcompat_source_name(graphics->source),
        (unsigned long long)graphics->generation, graphics->proof_flags,
        graphics->window_width, graphics->window_height,
        graphics->drawable_width, graphics->drawable_height,
        graphics->gles_major, graphics->gles_minor, graphics->red_bits,
        graphics->green_bits, graphics->blue_bits, graphics->alpha_bits,
        graphics->depth_bits, graphics->stencil_bits,
        graphics->double_buffer ? "true" : "false", graphics->profile_mask,
        graphics->egl_config_id, graphics->egl_red_bits,
        graphics->egl_green_bits, graphics->egl_blue_bits,
        graphics->egl_alpha_bits, graphics->egl_depth_bits,
        graphics->egl_stencil_bits, graphics->egl_renderable_type,
        graphics->egl_surface_type);
    nxcompat_append_json_string(&buffer, backend);
    nxcompat_append(&buffer, ",\"gl_vendor\":");
    nxcompat_append_json_string(&buffer, vendor);
    nxcompat_append(&buffer, ",\"gl_renderer\":");
    nxcompat_append_json_string(&buffer, renderer);
    nxcompat_append(&buffer, ",\"gl_version\":");
    nxcompat_append_json_string(&buffer, gl_version);
    nxcompat_append(&buffer, ",\"glsl_version\":");
    nxcompat_append_json_string(&buffer, glsl_version);
    nxcompat_append(&buffer, ",\"egl_vendor\":");
    nxcompat_append_json_string(&buffer, egl_vendor);
    nxcompat_append(&buffer, ",\"egl_version\":");
    nxcompat_append_json_string(&buffer, egl_version);
    nxcompat_append(&buffer, ",\"egl_client_apis\":");
    nxcompat_append_json_string(&buffer, egl_apis);
    nxcompat_append(&buffer, "}");
  }
  nxcompat_append(&buffer, ",\"audio\":");
  if (!report->has_audio) {
    nxcompat_append(&buffer, "null");
  } else {
    char backend[NXCOMPAT_NAME_MAX];
    const nxcompat_audio_receipt *audio = &report->audio;
    nxcompat_sanitize_public_label(audio->backend, backend, sizeof(backend));
    nxcompat_append(
        &buffer,
        "{\"source\":\"%s\",\"generation\":%llu,\"proof_flags\":%u,"
        "\"lifetime\":\"%s\",\"frequency\":%d,\"format\":%u,"
        "\"channels\":%u,\"samples\":%u,\"device_id_nonzero\":%s,"
        "\"backend\":",
        nxcompat_source_name(audio->source),
        (unsigned long long)audio->generation, audio->proof_flags,
        nxcompat_audio_lifetime_name(audio->lifetime), audio->frequency,
        audio->format, audio->channels, audio->samples,
        audio->device_id_was_nonzero ? "true" : "false");
    nxcompat_append_json_string(&buffer, backend);
    nxcompat_append(&buffer, "}");
  }
  nxcompat_append(&buffer, ",\"input\":");
  if (!report->has_input) {
    nxcompat_append(&buffer, "null");
  } else {
    const nxcompat_input_receipt *input = &report->input;
    nxcompat_append(
        &buffer,
        "{\"source\":\"%s\",\"topology_generation\":%llu,"
        "\"proof_flags\":%u,\"connected_count\":%u,"
        "\"mapping_source\":\"%s\",\"hotplug_events\":%llu,"
        "\"rescans\":%llu,\"last_change\":\"%s\"}",
        nxcompat_source_name(input->source),
        (unsigned long long)input->topology_generation, input->proof_flags,
        input->connected_count,
        nxcompat_input_mapping_source_name(input->mapping_source),
        (unsigned long long)input->hotplug_event_count,
        (unsigned long long)input->rescan_count,
        nxcompat_input_change_name(input->last_change));
  }
  nxcompat_append(&buffer, "},\"actions\":[");
  if (plan) {
    for (index = 0u; index < plan->action_count; ++index) {
      const nxcompat_action_v2 *action = &plan->actions[index];
      nxcompat_append(
          &buffer,
          "%s{\"id\":\"%s\",\"state\":\"%s\",\"reason_code\":%d}",
          index ? "," : "", nxcompat_action_name(action->id),
          nxcompat_action_state_v2_name(action->state),
          (int)action->reason_code);
    }
  }
  nxcompat_append(&buffer, "]}");
  return buffer.failed ? -1 : (int)buffer.used;
}
