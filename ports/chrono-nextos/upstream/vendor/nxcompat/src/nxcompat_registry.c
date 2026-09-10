/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxcompat_internal.h"
#include "nxcompat_registry_internal.h"

#include <stdlib.h>
#include <string.h>

/* Audit mirror of capabilities-v1.json, registry_version 1.0.0, source SHA-256
 * 0f302c49572c34e57448342cf0ecf605c96c9af2b02d5a2adccfa5dc93d75b4c.
 * The table is deliberately positional: index == stable numeric ID. */
static const nxcompat_capability_definition nxcompat_capabilities[] = {
    {0u, "host.portmaster", NXCOMPAT_PHASE_PREFLIGHT, NXCOMPAT_SOURCE_PROBE,
     NXCOMPAT_EVIDENCE_OBSERVED, NXCOMPAT_ROLE_OBSERVATION},
    {1u, "host.armhf-libs", NXCOMPAT_PHASE_PREFLIGHT, NXCOMPAT_SOURCE_PROBE,
     NXCOMPAT_EVIDENCE_OBSERVED, NXCOMPAT_ROLE_OBSERVATION},
    {2u, "host.aarch64-libs", NXCOMPAT_PHASE_PREFLIGHT, NXCOMPAT_SOURCE_PROBE,
     NXCOMPAT_EVIDENCE_OBSERVED, NXCOMPAT_ROLE_OBSERVATION},
    {3u, "host.i386-libs", NXCOMPAT_PHASE_PREFLIGHT, NXCOMPAT_SOURCE_PROBE,
     NXCOMPAT_EVIDENCE_OBSERVED, NXCOMPAT_ROLE_OBSERVATION},
    {4u, "host.session-runtime", NXCOMPAT_PHASE_PREFLIGHT,
     NXCOMPAT_SOURCE_PROBE, NXCOMPAT_EVIDENCE_OBSERVED,
     NXCOMPAT_ROLE_OBSERVATION},
    {5u, "host.short-memory", NXCOMPAT_PHASE_PREFLIGHT, NXCOMPAT_SOURCE_PROBE,
     NXCOMPAT_EVIDENCE_OBSERVED, NXCOMPAT_ROLE_OBSERVATION},
    {6u, "host.fuse-like-filesystem", NXCOMPAT_PHASE_PREFLIGHT,
     NXCOMPAT_SOURCE_PROBE, NXCOMPAT_EVIDENCE_OBSERVED,
     NXCOMPAT_ROLE_OBSERVATION},
    {7u, "host.network-filesystem", NXCOMPAT_PHASE_PREFLIGHT,
     NXCOMPAT_SOURCE_PROBE, NXCOMPAT_EVIDENCE_OBSERVED,
     NXCOMPAT_ROLE_OBSERVATION},
    {8u, "host.fbdev", NXCOMPAT_PHASE_PREFLIGHT, NXCOMPAT_SOURCE_PROBE,
     NXCOMPAT_EVIDENCE_OBSERVED, NXCOMPAT_ROLE_OBSERVATION},
    {9u, "host.drm", NXCOMPAT_PHASE_PREFLIGHT, NXCOMPAT_SOURCE_PROBE,
     NXCOMPAT_EVIDENCE_OBSERVED, NXCOMPAT_ROLE_OBSERVATION},
    {10u, "host.drm-connected", NXCOMPAT_PHASE_PREFLIGHT,
     NXCOMPAT_SOURCE_PROBE, NXCOMPAT_EVIDENCE_OBSERVED,
     NXCOMPAT_ROLE_OBSERVATION},
    {11u, "host.wayland", NXCOMPAT_PHASE_PREFLIGHT, NXCOMPAT_SOURCE_PROBE,
     NXCOMPAT_EVIDENCE_OBSERVED, NXCOMPAT_ROLE_OBSERVATION},
    {12u, "host.x11", NXCOMPAT_PHASE_PREFLIGHT, NXCOMPAT_SOURCE_PROBE,
     NXCOMPAT_EVIDENCE_OBSERVED, NXCOMPAT_ROLE_OBSERVATION},
    {13u, "audio.pulse-socket", NXCOMPAT_PHASE_PREFLIGHT,
     NXCOMPAT_SOURCE_PROBE, NXCOMPAT_EVIDENCE_OBSERVED,
     NXCOMPAT_ROLE_OBSERVATION},
    {14u, "audio.pipewire-socket", NXCOMPAT_PHASE_PREFLIGHT,
     NXCOMPAT_SOURCE_PROBE, NXCOMPAT_EVIDENCE_OBSERVED,
     NXCOMPAT_ROLE_OBSERVATION},
    {15u, "audio.alsa", NXCOMPAT_PHASE_PREFLIGHT, NXCOMPAT_SOURCE_PROBE,
     NXCOMPAT_EVIDENCE_OBSERVED, NXCOMPAT_ROLE_OBSERVATION},
    {16u, "graphics.window", NXCOMPAT_PHASE_GRAPHICS, NXCOMPAT_SOURCE_NXGL,
     NXCOMPAT_EVIDENCE_OPENED, NXCOMPAT_ROLE_BASELINE_GRAPHICS},
    {17u, "graphics.gles2", NXCOMPAT_PHASE_GRAPHICS, NXCOMPAT_SOURCE_NXGL,
     NXCOMPAT_EVIDENCE_OPENED, NXCOMPAT_ROLE_BASELINE_GRAPHICS},
    {18u, "graphics.gles3", NXCOMPAT_PHASE_GRAPHICS, NXCOMPAT_SOURCE_NXGL,
     NXCOMPAT_EVIDENCE_OPENED, NXCOMPAT_ROLE_OPTIONAL_ENHANCEMENT},
    {19u, "graphics.egl", NXCOMPAT_PHASE_GRAPHICS, NXCOMPAT_SOURCE_NXGL,
     NXCOMPAT_EVIDENCE_OPENED, NXCOMPAT_ROLE_BASELINE_GRAPHICS},
    {20u, "graphics.egl-config", NXCOMPAT_PHASE_GRAPHICS,
     NXCOMPAT_SOURCE_NXGL, NXCOMPAT_EVIDENCE_OPENED,
     NXCOMPAT_ROLE_BASELINE_GRAPHICS},
    {21u, "graphics.drawable", NXCOMPAT_PHASE_GRAPHICS, NXCOMPAT_SOURCE_NXGL,
     NXCOMPAT_EVIDENCE_OPENED, NXCOMPAT_ROLE_BASELINE_GRAPHICS},
    {22u, "graphics.etc1", NXCOMPAT_PHASE_GRAPHICS, NXCOMPAT_SOURCE_NXGL,
     NXCOMPAT_EVIDENCE_OPENED, NXCOMPAT_ROLE_OPTIONAL_ENHANCEMENT},
    {23u, "graphics.etc2", NXCOMPAT_PHASE_GRAPHICS, NXCOMPAT_SOURCE_NXGL,
     NXCOMPAT_EVIDENCE_OPENED, NXCOMPAT_ROLE_OPTIONAL_ENHANCEMENT},
    {24u, "graphics.astc", NXCOMPAT_PHASE_GRAPHICS, NXCOMPAT_SOURCE_NXGL,
     NXCOMPAT_EVIDENCE_OPENED, NXCOMPAT_ROLE_OPTIONAL_ENHANCEMENT},
    {25u, "graphics.npot-full", NXCOMPAT_PHASE_GRAPHICS,
     NXCOMPAT_SOURCE_NXGL, NXCOMPAT_EVIDENCE_OPENED,
     NXCOMPAT_ROLE_OPTIONAL_ENHANCEMENT},
    {26u, "audio.output-open", NXCOMPAT_PHASE_AUDIO,
     NXCOMPAT_SOURCE_SDL2_AUDIO, NXCOMPAT_EVIDENCE_OPENED,
     NXCOMPAT_ROLE_PORT_DECLARED},
    {27u, "input.controller-mapping", NXCOMPAT_PHASE_INPUT,
     NXCOMPAT_SOURCE_PROBE, NXCOMPAT_EVIDENCE_OBSERVED,
     NXCOMPAT_ROLE_PORT_DECLARED},
    {28u, "input.controller-api", NXCOMPAT_PHASE_INPUT,
     NXCOMPAT_SOURCE_NXINPUT, NXCOMPAT_EVIDENCE_ACTIVE,
     NXCOMPAT_ROLE_PORT_DECLARED},
    {29u, "input.controller-connected", NXCOMPAT_PHASE_INPUT,
     NXCOMPAT_SOURCE_NXINPUT, NXCOMPAT_EVIDENCE_ACTIVE,
     NXCOMPAT_ROLE_OPTIONAL_RUNTIME},
    {30u, "input.hotplug", NXCOMPAT_PHASE_INPUT, NXCOMPAT_SOURCE_NXINPUT,
     NXCOMPAT_EVIDENCE_ACTIVE, NXCOMPAT_ROLE_OPTIONAL_RUNTIME}};

typedef char nxcompat_capability_count_must_match[
    sizeof(nxcompat_capabilities) / sizeof(nxcompat_capabilities[0]) ==
            NXCOMPAT_CAPABILITY_COUNT
        ? 1
        : -1];

static int nxcompat_phase_valid(nxcompat_phase phase) {
  return phase >= NXCOMPAT_PHASE_PREFLIGHT && phase <= NXCOMPAT_PHASE_READY;
}

static int nxcompat_requirements_instance_valid(
    const nxcompat_requirements *requirements) {
  size_t index;
  size_t prior;
  if (!requirements ||
      requirements->api_version != NXCOMPAT_API_VERSION ||
      requirements->struct_size < sizeof(*requirements) ||
      requirements->count > NXCOMPAT_MAX_REQUIREMENTS)
    return 0;
  for (index = 0u; index < requirements->count; ++index) {
    if (requirements->capability_ids[index] >= NXCOMPAT_CAPABILITY_COUNT)
      return 0;
    for (prior = 0u; prior < index; ++prior)
      if (requirements->capability_ids[prior] ==
          requirements->capability_ids[index])
        return 0;
  }
  return 1;
}

size_t nxcompat_capability_definition_count(void) {
  return NXCOMPAT_CAPABILITY_COUNT;
}

const nxcompat_capability_definition *
nxcompat_capability_definition_at(size_t index) {
  if (index >= NXCOMPAT_CAPABILITY_COUNT)
    return NULL;
  return &nxcompat_capabilities[index];
}

const nxcompat_capability_definition *
nxcompat_capability_definition_by_id(nxcompat_capability_id capability_id) {
  return nxcompat_capability_definition_at((size_t)capability_id);
}

const nxcompat_capability_definition *
nxcompat_capability_definition_by_name(const char *name) {
  const char *terminator;
  size_t name_length;
  size_t index;
  if (!name)
    return NULL;
  terminator = (const char *)memchr(name, '\0', NXCOMPAT_NAME_MAX);
  if (!terminator || terminator == name)
    return NULL;
  name_length = (size_t)(terminator - name);
  for (index = 0u; index < NXCOMPAT_CAPABILITY_COUNT; ++index)
    if (strlen(nxcompat_capabilities[index].name) == name_length &&
        memcmp(nxcompat_capabilities[index].name, name, name_length) == 0)
      return &nxcompat_capabilities[index];
  return NULL;
}

int nxcompat_registry_bounded_string(const char *value, size_t size) {
  return value && size > 0u && memchr(value, '\0', size) != NULL;
}

int nxcompat_registry_instance_valid(const nxcompat_registry *registry) {
  size_t index;
  if (!registry || registry->magic != NXCOMPAT_REGISTRY_MAGIC ||
      (registry->probe_controller_mapping != 0 &&
       registry->probe_controller_mapping != 1) ||
      (registry->probe_controller_mapping &&
       registry->probe_controller_mapping_generation == 0u) ||
      (registry->has_graphics != 0 && registry->has_graphics != 1) ||
      (registry->has_audio != 0 && registry->has_audio != 1) ||
      (registry->has_input != 0 && registry->has_input != 1))
    return 0;
  for (index = 0u; index < NXCOMPAT_CAPABILITY_COUNT; ++index) {
    const nxcompat_capability_evidence *evidence = &registry->evidence[index];
    if (evidence->capability_id != index ||
        evidence->state < NXCOMPAT_EVIDENCE_ABSENT ||
        evidence->state > NXCOMPAT_EVIDENCE_LOST ||
        !nxcompat_phase_valid(evidence->phase) ||
        evidence->phase != nxcompat_capabilities[index].phase ||
        evidence->source < NXCOMPAT_SOURCE_PROBE ||
        evidence->source > NXCOMPAT_SOURCE_ENGINE_ADAPTER)
      return 0;
  }
  if (registry->has_graphics &&
      (registry->graphics.api_version != NXCOMPAT_API_VERSION ||
       registry->graphics.struct_size < sizeof(registry->graphics) ||
       registry->graphics.generation == 0u ||
       (registry->graphics.source != NXCOMPAT_SOURCE_NXGL &&
        registry->graphics.source != NXCOMPAT_SOURCE_ENGINE_ADAPTER) ||
       (registry->graphics.proof_flags &
        ~(uint32_t)(NXCOMPAT_GRAPHICS_PROOF_WINDOW_CREATED |
                    NXCOMPAT_GRAPHICS_PROOF_CONTEXT_CURRENT |
                    NXCOMPAT_GRAPHICS_PROOF_GL_STRINGS_REAL |
                    NXCOMPAT_GRAPHICS_PROOF_EGL_DISPLAY_CURRENT |
                    NXCOMPAT_GRAPHICS_PROOF_EGL_CONTEXT_CURRENT |
                    NXCOMPAT_GRAPHICS_PROOF_EGL_CONFIG_QUERIED |
                    NXCOMPAT_GRAPHICS_PROOF_DRAWABLE_POSITIVE)) != 0u ||
       !nxcompat_registry_bounded_string(
           registry->graphics.video_backend,
           sizeof(registry->graphics.video_backend)) ||
       !nxcompat_registry_bounded_string(registry->graphics.gl_vendor,
                                         sizeof(registry->graphics.gl_vendor)) ||
       !nxcompat_registry_bounded_string(
           registry->graphics.gl_renderer,
           sizeof(registry->graphics.gl_renderer)) ||
       !nxcompat_registry_bounded_string(
           registry->graphics.gl_version,
           sizeof(registry->graphics.gl_version)) ||
       !nxcompat_registry_bounded_string(
           registry->graphics.glsl_version,
           sizeof(registry->graphics.glsl_version)) ||
       !nxcompat_registry_bounded_string(
           registry->graphics.gl_extensions,
           sizeof(registry->graphics.gl_extensions)) ||
       !nxcompat_registry_bounded_string(
           registry->graphics.egl_vendor,
           sizeof(registry->graphics.egl_vendor)) ||
       !nxcompat_registry_bounded_string(
           registry->graphics.egl_version,
           sizeof(registry->graphics.egl_version)) ||
       !nxcompat_registry_bounded_string(
           registry->graphics.egl_client_apis,
           sizeof(registry->graphics.egl_client_apis))))
    return 0;
  if (registry->has_graphics)
    for (index = NXCOMPAT_CAPABILITY_GRAPHICS_WINDOW;
         index <= NXCOMPAT_CAPABILITY_GRAPHICS_NPOT_FULL; ++index)
      if (registry->evidence[index].generation !=
              registry->graphics.generation ||
          registry->evidence[index].source != registry->graphics.source)
        return 0;
  if (registry->has_audio &&
      (registry->audio.api_version != NXCOMPAT_API_VERSION ||
       registry->audio.struct_size < sizeof(registry->audio) ||
       registry->audio.generation == 0u ||
       (registry->audio.source != NXCOMPAT_SOURCE_SDL2_AUDIO &&
        registry->audio.source != NXCOMPAT_SOURCE_ENGINE_ADAPTER) ||
       registry->audio.lifetime < NXCOMPAT_AUDIO_OPENED_THEN_CLOSED ||
       registry->audio.lifetime > NXCOMPAT_AUDIO_ACTIVE_ENGINE_OWNED ||
       !nxcompat_registry_bounded_string(registry->audio.backend,
                                         sizeof(registry->audio.backend))))
    return 0;
  if (registry->has_audio &&
      (registry->evidence[NXCOMPAT_CAPABILITY_AUDIO_OUTPUT_OPEN].generation !=
           registry->audio.generation ||
       registry->evidence[NXCOMPAT_CAPABILITY_AUDIO_OUTPUT_OPEN].source !=
           registry->audio.source))
    return 0;
  if (registry->has_input &&
      (registry->input.api_version != NXCOMPAT_API_VERSION ||
       registry->input.struct_size < sizeof(registry->input) ||
       registry->input.topology_generation == 0u ||
       (registry->input.source != NXCOMPAT_SOURCE_NXINPUT &&
        registry->input.source != NXCOMPAT_SOURCE_ENGINE_ADAPTER) ||
       registry->input.connected_count > 4u ||
       registry->input.mapping_source < NXCOMPAT_INPUT_MAPPING_NONE ||
       registry->input.mapping_source > NXCOMPAT_INPUT_MAPPING_ENGINE ||
       registry->input.last_change < NXCOMPAT_INPUT_CHANGE_NONE ||
       registry->input.last_change > NXCOMPAT_INPUT_CHANGE_RESCAN))
    return 0;
  if (registry->has_input)
    for (index = NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_API;
         index <= NXCOMPAT_CAPABILITY_INPUT_HOTPLUG; ++index)
      if (registry->evidence[index].generation !=
          registry->input.topology_generation)
        return 0;
  return 1;
}

int nxcompat_registry_evidence_satisfies(nxcompat_evidence_state actual,
                                         nxcompat_evidence_state minimum) {
  if (actual == NXCOMPAT_EVIDENCE_LOST ||
      actual == NXCOMPAT_EVIDENCE_ABSENT)
    return 0;
  return actual >= minimum;
}

void nxcompat_registry_stage_evidence(
    nxcompat_registry *registry, nxcompat_capability_id capability_id,
    nxcompat_evidence_state state, nxcompat_source source,
    nxcompat_reason_code reason, uint64_t generation) {
  const nxcompat_capability_definition *definition;
  nxcompat_capability_evidence *evidence;
  if (!registry || capability_id >= NXCOMPAT_CAPABILITY_COUNT)
    return;
  definition = &nxcompat_capabilities[capability_id];
  evidence = &registry->evidence[capability_id];
  evidence->capability_id = capability_id;
  evidence->state = state;
  evidence->phase = definition->phase;
  evidence->source = source;
  evidence->reason = reason;
  evidence->generation = generation;
}

void nxcompat_registry_stage_boolean(
    nxcompat_registry *registry, nxcompat_capability_id capability_id,
    int present, nxcompat_evidence_state present_state, nxcompat_source source,
    nxcompat_reason_code present_reason, uint64_t generation) {
  nxcompat_capability_evidence *old;
  if (!registry || capability_id >= NXCOMPAT_CAPABILITY_COUNT)
    return;
  old = &registry->evidence[capability_id];
  if (present) {
    nxcompat_registry_stage_evidence(registry, capability_id, present_state,
                                     source, present_reason, generation);
  } else if (old->state != NXCOMPAT_EVIDENCE_ABSENT) {
    nxcompat_registry_stage_evidence(
        registry, capability_id, NXCOMPAT_EVIDENCE_LOST, source,
        NXCOMPAT_REASON_CAPABILITY_LOST, generation);
  } else {
    nxcompat_registry_stage_evidence(
        registry, capability_id, NXCOMPAT_EVIDENCE_ABSENT, source,
        NXCOMPAT_REASON_OBSERVATION_ABSENT, generation);
  }
}

nxcompat_result_code nxcompat_registry_create(nxcompat_registry **registry) {
  nxcompat_registry *created;
  size_t index;
  if (!registry)
    return NXCOMPAT_INVALID;
  *registry = NULL;
  created = (nxcompat_registry *)calloc(1u, sizeof(*created));
  if (!created)
    return NXCOMPAT_FAILED;
  created->magic = NXCOMPAT_REGISTRY_MAGIC;
  for (index = 0u; index < NXCOMPAT_CAPABILITY_COUNT; ++index)
    nxcompat_registry_stage_evidence(
        created, (nxcompat_capability_id)index, NXCOMPAT_EVIDENCE_ABSENT,
        nxcompat_capabilities[index].source,
        NXCOMPAT_REASON_OBSERVATION_ABSENT, 0u);
  *registry = created;
  return NXCOMPAT_OK;
}

void nxcompat_registry_destroy(nxcompat_registry *registry) {
  if (!registry)
    return;
  memset(registry, 0, sizeof(*registry));
  free(registry);
}

nxcompat_result_code nxcompat_registry_get(
    const nxcompat_registry *registry, nxcompat_capability_id capability_id,
    nxcompat_capability_evidence *evidence) {
  if (!nxcompat_registry_instance_valid(registry) || !evidence ||
      capability_id >= NXCOMPAT_CAPABILITY_COUNT)
    return NXCOMPAT_INVALID;
  *evidence = registry->evidence[capability_id];
  return NXCOMPAT_OK;
}

static int nxcompat_host_capability(const nxcompat_host *host,
                                    nxcompat_capability_id capability_id) {
  switch (capability_id) {
  case NXCOMPAT_CAPABILITY_HOST_PORTMASTER:
    return (host->capabilities & NXCOMPAT_CAP_PORTMASTER) != 0u;
  case NXCOMPAT_CAPABILITY_HOST_ARMHF_LIBS:
    return (host->capabilities & NXCOMPAT_CAP_ARMHF_LIBS) != 0u;
  case NXCOMPAT_CAPABILITY_HOST_AARCH64_LIBS:
    return (host->capabilities & NXCOMPAT_CAP_AARCH64_LIBS) != 0u;
  case NXCOMPAT_CAPABILITY_HOST_I386_LIBS:
    return (host->capabilities & NXCOMPAT_CAP_I386_LIBS) != 0u;
  case NXCOMPAT_CAPABILITY_HOST_SESSION_RUNTIME:
    return host->session_runtime_dir[0] != '\0';
  case NXCOMPAT_CAPABILITY_HOST_SHORT_MEMORY:
    return (host->capabilities & NXCOMPAT_CAP_SHORT_MEMORY) != 0u;
  case NXCOMPAT_CAPABILITY_HOST_FUSE_LIKE_FILESYSTEM:
    return host->filesystem_class == NXCOMPAT_FILESYSTEM_FUSE_LIKE;
  case NXCOMPAT_CAPABILITY_HOST_NETWORK_FILESYSTEM:
    return host->filesystem_class == NXCOMPAT_FILESYSTEM_NETWORK;
  case NXCOMPAT_CAPABILITY_HOST_FBDEV:
    return (host->capabilities & NXCOMPAT_CAP_FBDEV) != 0u;
  case NXCOMPAT_CAPABILITY_HOST_DRM:
    return (host->capabilities & NXCOMPAT_CAP_DRM) != 0u;
  case NXCOMPAT_CAPABILITY_HOST_DRM_CONNECTED:
    return (host->capabilities & NXCOMPAT_CAP_DRM_CONNECTED) != 0u;
  case NXCOMPAT_CAPABILITY_HOST_WAYLAND:
    return (host->capabilities & NXCOMPAT_CAP_WAYLAND) != 0u;
  case NXCOMPAT_CAPABILITY_HOST_X11:
    return (host->capabilities & NXCOMPAT_CAP_X11) != 0u;
  case NXCOMPAT_CAPABILITY_AUDIO_PULSE_SOCKET:
    return (host->capabilities & NXCOMPAT_CAP_PULSE_SOCKET) != 0u;
  case NXCOMPAT_CAPABILITY_AUDIO_PIPEWIRE_SOCKET:
    return (host->capabilities & NXCOMPAT_CAP_PIPEWIRE_SOCKET) != 0u;
  case NXCOMPAT_CAPABILITY_AUDIO_ALSA:
    return (host->capabilities & NXCOMPAT_CAP_ALSA) != 0u;
  case NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING:
    return (host->capabilities & NXCOMPAT_CAP_CONTROLLER_MAPPING) != 0u;
  default:
    return 0;
  }
}

nxcompat_result_code nxcompat_registry_seed_host(nxcompat_registry *registry,
                                                 const nxcompat_host *host) {
  nxcompat_registry staged;
  uint64_t generation;
  nxcompat_capability_id capability_id;
  if (!nxcompat_registry_instance_valid(registry) ||
      !nxcompat_host_instance_valid(host))
    return NXCOMPAT_INVALID;
  staged = *registry;
  generation = registry->generation + 1u;
  if (generation == 0u)
    return NXCOMPAT_FAILED;
  staged.generation = generation;
  for (capability_id = NXCOMPAT_CAPABILITY_HOST_PORTMASTER;
       capability_id <= NXCOMPAT_CAPABILITY_AUDIO_ALSA; ++capability_id) {
    int present = nxcompat_host_capability(host, capability_id);
    nxcompat_registry_stage_evidence(
        &staged, capability_id,
        present ? NXCOMPAT_EVIDENCE_OBSERVED : NXCOMPAT_EVIDENCE_ABSENT,
        NXCOMPAT_SOURCE_PROBE,
        present ? NXCOMPAT_REASON_PROBE_COMPLETE
                : NXCOMPAT_REASON_OBSERVATION_ABSENT,
        generation);
  }
  {
    int present = nxcompat_host_capability(
        host, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING);
    staged.probe_controller_mapping = present;
    staged.probe_controller_mapping_generation = generation;
    if (staged.evidence[NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING].source ==
            NXCOMPAT_SOURCE_PROBE ||
        (present &&
         !nxcompat_registry_evidence_satisfies(
             staged.evidence[NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING]
                 .state,
             NXCOMPAT_EVIDENCE_OBSERVED)) ||
        (!present &&
         staged.evidence[NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING].state ==
             NXCOMPAT_EVIDENCE_ABSENT))
      nxcompat_registry_stage_evidence(
          &staged, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING,
          present ? NXCOMPAT_EVIDENCE_OBSERVED : NXCOMPAT_EVIDENCE_ABSENT,
          NXCOMPAT_SOURCE_PROBE,
          present ? NXCOMPAT_REASON_PROBE_COMPLETE
                  : NXCOMPAT_REASON_OBSERVATION_ABSENT,
          generation);
  }
  *registry = staged;
  return NXCOMPAT_OK;
}

static int nxcompat_requirement_name_valid(const char *value, size_t length) {
  size_t index;
  if (!value || length == 0u || length >= NXCOMPAT_NAME_MAX)
    return 0;
  for (index = 0u; index < length; ++index) {
    unsigned char character = (unsigned char)value[index];
    if (!((character >= (unsigned char)'a' &&
           character <= (unsigned char)'z') ||
          (character >= (unsigned char)'0' &&
           character <= (unsigned char)'9') ||
          character == (unsigned char)'.' || character == (unsigned char)'-'))
      return 0;
  }
  return value[0] != '.' && value[length - 1u] != '.';
}

static void nxcompat_store_reason(nxcompat_reason_code *reason,
                                  nxcompat_reason_code value) {
  if (reason)
    *reason = value;
}

nxcompat_result_code nxcompat_requirements_parse_ex(
    const char *newline_separated, nxcompat_requirements *requirements,
    nxcompat_reason_code *reason) {
  nxcompat_requirements staged;
  const char *cursor;
  const char *terminator;
  nxcompat_store_reason(reason, NXCOMPAT_REASON_INVALID_ARGUMENT);
  if (!requirements)
    return NXCOMPAT_INVALID;
  memset(&staged, 0, sizeof(staged));
  staged.api_version = NXCOMPAT_API_VERSION;
  staged.struct_size = sizeof(staged);
  if (!newline_separated || !newline_separated[0]) {
    *requirements = staged;
    nxcompat_store_reason(reason, NXCOMPAT_REASON_NONE);
    return NXCOMPAT_OK;
  }
  terminator = (const char *)memchr(newline_separated, '\0',
                                   NXCOMPAT_REQUIREMENTS_TEXT_MAX);
  if (!terminator) {
    nxcompat_store_reason(reason, NXCOMPAT_REASON_OBSERVATION_MALFORMED);
    return NXCOMPAT_INVALID;
  }
  cursor = newline_separated;
  while (cursor < terminator) {
    const nxcompat_capability_definition *definition;
    const char *end = (const char *)memchr(
        cursor, '\n', (size_t)(terminator - cursor));
    size_t length = end ? (size_t)(end - cursor)
                        : (size_t)(terminator - cursor);
    size_t index;
    char name[NXCOMPAT_NAME_MAX];
    if (length > 0u && cursor[length - 1u] == '\r')
      --length;
    if (!nxcompat_requirement_name_valid(cursor, length)) {
      nxcompat_store_reason(reason, NXCOMPAT_REASON_OBSERVATION_MALFORMED);
      return NXCOMPAT_INVALID;
    }
    if (staged.count >= NXCOMPAT_MAX_REQUIREMENTS) {
      nxcompat_store_reason(reason, NXCOMPAT_REASON_OBSERVATION_OUT_OF_RANGE);
      return NXCOMPAT_INVALID;
    }
    memcpy(name, cursor, length);
    name[length] = '\0';
    definition = nxcompat_capability_definition_by_name(name);
    if (!definition) {
      nxcompat_store_reason(reason, NXCOMPAT_REASON_REQUIREMENT_UNKNOWN);
      return NXCOMPAT_INVALID;
    }
    for (index = 0u; index < staged.count; ++index)
      if (staged.capability_ids[index] == definition->capability_id) {
        nxcompat_store_reason(reason, NXCOMPAT_REASON_REQUIREMENT_DUPLICATE);
        return NXCOMPAT_INVALID;
      }
    staged.capability_ids[staged.count++] = definition->capability_id;
    if (!end)
      break;
    cursor = end + 1;
    if (cursor == terminator)
      break;
  }
  *requirements = staged;
  nxcompat_store_reason(reason, NXCOMPAT_REASON_NONE);
  return NXCOMPAT_OK;
}

nxcompat_result_code nxcompat_requirements_parse(
    const char *newline_separated, nxcompat_requirements *requirements) {
  return nxcompat_requirements_parse_ex(newline_separated, requirements, NULL);
}

nxcompat_result_code
nxcompat_requirements_parse_runtime_ex(nxcompat_requirements *requirements,
                                       nxcompat_reason_code *reason) {
  char snapshot[NXCOMPAT_REQUIREMENTS_TEXT_MAX];
  const char *value;
  size_t length;
  nxcompat_result_code status;
  nxcompat_store_reason(reason, NXCOMPAT_REASON_INVALID_ARGUMENT);
  if (!requirements)
    return NXCOMPAT_INVALID;
  status = nxcompat_global_arbiter_try_acquire();
  if (status != NXCOMPAT_OK) {
    nxcompat_store_reason(reason, NXCOMPAT_REASON_ARBITER_BUSY);
    return status;
  }
  value = getenv("NXCOMPAT_REQUIRED_CAPABILITIES");
  if (!value)
    value = "";
  for (length = 0u; length < sizeof(snapshot) && value[length]; ++length)
    snapshot[length] = value[length];
  if (length == sizeof(snapshot)) {
    nxcompat_global_arbiter_release();
    nxcompat_store_reason(reason, NXCOMPAT_REASON_OBSERVATION_MALFORMED);
    return NXCOMPAT_INVALID;
  }
  snapshot[length] = '\0';
  nxcompat_global_arbiter_release();
  return nxcompat_requirements_parse_ex(snapshot, requirements, reason);
}

nxcompat_result_code
nxcompat_requirements_parse_runtime(nxcompat_requirements *requirements) {
  return nxcompat_requirements_parse_runtime_ex(requirements, NULL);
}

nxcompat_result_code nxcompat_requirements_evaluate(
    const nxcompat_registry *registry,
    const nxcompat_requirements *requirements, nxcompat_phase phase,
    nxcompat_requirement_report *report) {
  size_t index;
  if (!nxcompat_registry_instance_valid(registry) ||
      !nxcompat_requirements_instance_valid(requirements) ||
      !nxcompat_phase_valid(phase) || !report)
    return NXCOMPAT_INVALID;
  memset(report, 0, sizeof(*report));
  report->api_version = NXCOMPAT_API_VERSION;
  report->struct_size = sizeof(*report);
  report->phase = phase;
  report->count = requirements->count;
  for (index = 0u; index < requirements->count; ++index) {
    nxcompat_capability_id capability_id =
        requirements->capability_ids[index];
    const nxcompat_capability_definition *definition =
        &nxcompat_capabilities[capability_id];
    const nxcompat_capability_evidence *evidence =
        &registry->evidence[capability_id];
    nxcompat_requirement_result *result = &report->results[index];
    result->capability_id = capability_id;
    if (phase < definition->phase) {
      result->state = NXCOMPAT_REQUIREMENT_PENDING;
      result->reason = NXCOMPAT_REASON_REQUIREMENT_PENDING;
      report->pending_count++;
    } else if (nxcompat_registry_evidence_satisfies(
                   evidence->state, definition->minimum_evidence)) {
      result->state = NXCOMPAT_REQUIREMENT_SATISFIED;
      result->reason = NXCOMPAT_REASON_REQUIREMENT_SATISFIED;
      report->satisfied_count++;
    } else {
      result->state = NXCOMPAT_REQUIREMENT_MISSING;
      result->reason = NXCOMPAT_REASON_REQUIREMENT_MISSING;
      report->missing_count++;
    }
  }
  if (report->missing_count != 0u)
    report->final_reason = NXCOMPAT_REASON_REQUIREMENT_MISSING;
  else if (report->pending_count != 0u)
    report->final_reason = NXCOMPAT_REASON_REQUIREMENT_PENDING;
  else
    report->final_reason = NXCOMPAT_REASON_REQUIREMENT_SATISFIED;
  return report->missing_count == 0u ? NXCOMPAT_OK : NXCOMPAT_FAILED;
}

nxcompat_result_code nxcompat_registry_runtime_report(
    const nxcompat_registry *registry,
    const nxcompat_requirements *requirements, nxcompat_phase phase,
    nxcompat_runtime_report *report) {
  nxcompat_result_code evaluation;
  if (!nxcompat_registry_instance_valid(registry) ||
      !nxcompat_requirements_instance_valid(requirements) ||
      !nxcompat_phase_valid(phase) || !report)
    return NXCOMPAT_INVALID;
  memset(report, 0, sizeof(*report));
  report->api_version = NXCOMPAT_API_VERSION;
  report->struct_size = sizeof(*report);
  report->phase = phase;
  report->evidence_count = NXCOMPAT_CAPABILITY_COUNT;
  memcpy(report->evidence, registry->evidence, sizeof(report->evidence));
  report->has_graphics = registry->has_graphics;
  report->has_audio = registry->has_audio;
  report->has_input = registry->has_input;
  report->graphics = registry->graphics;
  report->audio = registry->audio;
  report->input = registry->input;
  nxcompat_runtime_report_sanitize(report);
  evaluation = nxcompat_requirements_evaluate(
      registry, requirements, phase, &report->requirements);
  return evaluation;
}

const char *nxcompat_evidence_state_name(nxcompat_evidence_state value) {
  static const char *const names[] = {"absent", "observed", "opened",
                                      "active", "lost"};
  if (value < NXCOMPAT_EVIDENCE_ABSENT || value > NXCOMPAT_EVIDENCE_LOST)
    return "unknown";
  return names[(unsigned)value];
}

const char *nxcompat_phase_name(nxcompat_phase value) {
  static const char *const names[] = {"preflight", "graphics", "audio",
                                      "input", "ready"};
  if (!nxcompat_phase_valid(value))
    return "unknown";
  return names[(unsigned)value];
}

const char *nxcompat_source_name(nxcompat_source value) {
  static const char *const names[] = {"probe", "nxgl", "sdl2-audio",
                                      "nxinput", "engine-adapter"};
  if (value < NXCOMPAT_SOURCE_PROBE ||
      value > NXCOMPAT_SOURCE_ENGINE_ADAPTER)
    return "unknown";
  return names[(unsigned)value];
}

const char *nxcompat_capability_role_name(nxcompat_capability_role value) {
  static const char *const names[] = {
      "observation", "baseline-graphics", "port-declared",
      "optional-enhancement", "optional-runtime"};
  if (value < NXCOMPAT_ROLE_OBSERVATION ||
      value > NXCOMPAT_ROLE_OPTIONAL_RUNTIME)
    return "unknown";
  return names[(unsigned)value];
}

const char *nxcompat_requirement_state_name(nxcompat_requirement_state value) {
  static const char *const names[] = {"pending", "satisfied", "missing"};
  if (value < NXCOMPAT_REQUIREMENT_PENDING ||
      value > NXCOMPAT_REQUIREMENT_MISSING)
    return "unknown";
  return names[(unsigned)value];
}

const char *nxcompat_audio_lifetime_name(nxcompat_audio_lifetime value) {
  switch (value) {
  case NXCOMPAT_AUDIO_OPENED_THEN_CLOSED:
    return "opened-then-closed";
  case NXCOMPAT_AUDIO_ACTIVE_ENGINE_OWNED:
    return "active-engine-owned";
  default:
    return "unknown";
  }
}

const char *
nxcompat_input_mapping_source_name(nxcompat_input_mapping_source value) {
  static const char *const names[] = {"none", "portmaster-env", "database",
                                      "runtime", "engine"};
  if (value < NXCOMPAT_INPUT_MAPPING_NONE ||
      value > NXCOMPAT_INPUT_MAPPING_ENGINE)
    return "unknown";
  return names[(unsigned)value];
}

const char *nxcompat_input_change_name(nxcompat_input_change value) {
  static const char *const names[] = {"none", "add", "remove", "remap",
                                      "rescan"};
  if (value < NXCOMPAT_INPUT_CHANGE_NONE ||
      value > NXCOMPAT_INPUT_CHANGE_RESCAN)
    return "unknown";
  return names[(unsigned)value];
}
