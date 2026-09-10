/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxcompat.h"
#include "nxcompat_registry_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,         \
                    #condition);                                             \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static void copy_text(char *destination, size_t size, const char *value) {
  (void)snprintf(destination, size, "%s", value ? value : "");
}

static nxcompat_host make_host(void) {
  nxcompat_host host;
  memset(&host, 0, sizeof(host));
  host.api_version = NXCOMPAT_API_VERSION;
  host.struct_size = sizeof(host);
  host.process_arch = NXCOMPAT_ARCH_ARMV7;
  host.kernel_arch = NXCOMPAT_ARCH_AARCH64;
  host.memory_class = NXCOMPAT_MEMORY_SHORT;
  host.filesystem_class = NXCOMPAT_FILESYSTEM_NETWORK;
  host.memory_total_kib = 262144u;
  copy_text(host.port_id, sizeof(host.port_id), "chrono-trigger");
  copy_text(host.device_model, sizeof(host.device_model), "Synthetic M10");
  copy_text(host.os_id, sizeof(host.os_id), "fixture-os");
  copy_text(host.os_version, sizeof(host.os_version), "1.0");
  copy_text(host.libc_version, sizeof(host.libc_version), "glibc 2.30");
  copy_text(host.filesystem_type, sizeof(host.filesystem_type), "cifs");
  copy_text(host.session_runtime_dir, sizeof(host.session_runtime_dir),
            "/run/user/1000");
  host.capabilities =
      NXCOMPAT_CAP_PORTMASTER | NXCOMPAT_CAP_ARMHF_LIBS |
      NXCOMPAT_CAP_SHORT_MEMORY | NXCOMPAT_CAP_FBDEV | NXCOMPAT_CAP_ALSA |
      NXCOMPAT_CAP_CONTROLLER_MAPPING;
  return host;
}

static nxcompat_graphics_receipt make_graphics(uint64_t generation) {
  nxcompat_graphics_receipt receipt;
  memset(&receipt, 0, sizeof(receipt));
  receipt.api_version = NXCOMPAT_API_VERSION;
  receipt.struct_size = sizeof(receipt);
  receipt.source = NXCOMPAT_SOURCE_NXGL;
  receipt.generation = generation;
  receipt.proof_flags = NXCOMPAT_GRAPHICS_PROOF_WINDOW_CREATED |
                        NXCOMPAT_GRAPHICS_PROOF_CONTEXT_CURRENT |
                        NXCOMPAT_GRAPHICS_PROOF_GL_STRINGS_REAL |
                        NXCOMPAT_GRAPHICS_PROOF_EGL_DISPLAY_CURRENT |
                        NXCOMPAT_GRAPHICS_PROOF_EGL_CONTEXT_CURRENT |
                        NXCOMPAT_GRAPHICS_PROOF_EGL_CONFIG_QUERIED |
                        NXCOMPAT_GRAPHICS_PROOF_DRAWABLE_POSITIVE;
  receipt.window_width = 640;
  receipt.window_height = 480;
  receipt.drawable_width = 640;
  receipt.drawable_height = 480;
  receipt.gles_major = 2;
  receipt.gles_minor = 0;
  receipt.red_bits = 8;
  receipt.green_bits = 8;
  receipt.blue_bits = 8;
  receipt.alpha_bits = 8;
  receipt.depth_bits = 24;
  receipt.stencil_bits = 8;
  receipt.double_buffer = 1;
  receipt.profile_mask = 4;
  receipt.egl_config_id = 7;
  receipt.egl_red_bits = 8;
  receipt.egl_green_bits = 8;
  receipt.egl_blue_bits = 8;
  receipt.egl_alpha_bits = 8;
  receipt.egl_depth_bits = 24;
  receipt.egl_stencil_bits = 8;
  receipt.egl_renderable_type = 4;
  receipt.egl_surface_type = 4;
  copy_text(receipt.video_backend, sizeof(receipt.video_backend), "kmsdrm");
  copy_text(receipt.gl_vendor, sizeof(receipt.gl_vendor), "ARM");
  copy_text(receipt.gl_renderer, sizeof(receipt.gl_renderer), "Mali-450 MP");
  copy_text(receipt.gl_version, sizeof(receipt.gl_version),
            "OpenGL ES 2.0 fixture");
  copy_text(receipt.glsl_version, sizeof(receipt.glsl_version),
            "OpenGL ES GLSL ES 1.00");
  copy_text(receipt.gl_extensions, sizeof(receipt.gl_extensions),
            "GL_OES_compressed_ETC1_RGB8_texture GL_OES_texture_npot");
  copy_text(receipt.egl_vendor, sizeof(receipt.egl_vendor), "Mesa Project");
  copy_text(receipt.egl_version, sizeof(receipt.egl_version), "1.4");
  copy_text(receipt.egl_client_apis, sizeof(receipt.egl_client_apis),
            "OpenGL_ES");
  return receipt;
}

static nxcompat_audio_receipt make_audio(uint64_t generation) {
  nxcompat_audio_receipt receipt;
  memset(&receipt, 0, sizeof(receipt));
  receipt.api_version = NXCOMPAT_API_VERSION;
  receipt.struct_size = sizeof(receipt);
  receipt.source = NXCOMPAT_SOURCE_SDL2_AUDIO;
  receipt.generation = generation;
  receipt.proof_flags = NXCOMPAT_AUDIO_PROOF_BACKEND_INITIALIZED |
                        NXCOMPAT_AUDIO_PROOF_DEVICE_OPENED |
                        NXCOMPAT_AUDIO_PROOF_SPEC_OBTAINED;
  receipt.lifetime = NXCOMPAT_AUDIO_OPENED_THEN_CLOSED;
  receipt.frequency = 48000;
  receipt.format = 0x8010u;
  receipt.channels = 2u;
  receipt.samples = 1024u;
  receipt.device_id_was_nonzero = 1;
  copy_text(receipt.backend, sizeof(receipt.backend), "alsa");
  return receipt;
}

static nxcompat_input_receipt make_input(uint64_t generation,
                                         unsigned connected) {
  nxcompat_input_receipt receipt;
  memset(&receipt, 0, sizeof(receipt));
  receipt.api_version = NXCOMPAT_API_VERSION;
  receipt.struct_size = sizeof(receipt);
  receipt.source = NXCOMPAT_SOURCE_NXINPUT;
  receipt.topology_generation = generation;
  receipt.proof_flags =
      NXCOMPAT_INPUT_PROOF_CONTROLLER_SUBSYSTEM_ACTIVE |
      NXCOMPAT_INPUT_PROOF_INITIAL_SCAN_DONE |
      NXCOMPAT_INPUT_PROOF_EVENT_WATCH_ACTIVE |
      NXCOMPAT_INPUT_PROOF_RESCAN_ACTIVE;
  receipt.connected_count = connected;
  if (connected != 0u) {
    receipt.proof_flags |= NXCOMPAT_INPUT_PROOF_MAPPING_AVAILABLE |
                           NXCOMPAT_INPUT_PROOF_CONTROLLER_OPENED;
    receipt.mapping_source = NXCOMPAT_INPUT_MAPPING_RUNTIME;
  }
  return receipt;
}

static nxcompat_capability_evidence evidence(nxcompat_registry *registry,
                                             nxcompat_capability_id id) {
  nxcompat_capability_evidence value;
  memset(&value, 0, sizeof(value));
  CHECK(nxcompat_registry_get(registry, id, &value) == NXCOMPAT_OK);
  return value;
}

static void test_catalog_exact(void) {
  static const char *const names[NXCOMPAT_CAPABILITY_COUNT] = {
      "host.portmaster",
      "host.armhf-libs",
      "host.aarch64-libs",
      "host.i386-libs",
      "host.session-runtime",
      "host.short-memory",
      "host.fuse-like-filesystem",
      "host.network-filesystem",
      "host.fbdev",
      "host.drm",
      "host.drm-connected",
      "host.wayland",
      "host.x11",
      "audio.pulse-socket",
      "audio.pipewire-socket",
      "audio.alsa",
      "graphics.window",
      "graphics.gles2",
      "graphics.gles3",
      "graphics.egl",
      "graphics.egl-config",
      "graphics.drawable",
      "graphics.etc1",
      "graphics.etc2",
      "graphics.astc",
      "graphics.npot-full",
      "audio.output-open",
      "input.controller-mapping",
      "input.controller-api",
      "input.controller-connected",
      "input.hotplug",
      "audio.embedded-openal",
      "graphics.gles1"};
  size_t index;
  CHECK(nxcompat_capability_definition_count() == NXCOMPAT_CAPABILITY_COUNT);
  for (index = 0u; index < NXCOMPAT_CAPABILITY_COUNT; ++index) {
    const nxcompat_capability_definition *definition =
        nxcompat_capability_definition_at(index);
    CHECK(definition != NULL);
    CHECK(definition && definition->capability_id == index);
    CHECK(definition && strcmp(definition->name, names[index]) == 0);
    CHECK(nxcompat_capability_definition_by_id((uint32_t)index) == definition);
    CHECK(nxcompat_capability_definition_by_name(names[index]) == definition);
  }
  CHECK(nxcompat_capability_definition_at(NXCOMPAT_CAPABILITY_COUNT) == NULL);
  CHECK(nxcompat_capability_definition_by_name("host.unregistered") == NULL);
  CHECK(strcmp(nxcompat_evidence_state_name(NXCOMPAT_EVIDENCE_LOST), "lost") ==
        0);
  CHECK(strcmp(nxcompat_source_name(NXCOMPAT_SOURCE_SDL2_AUDIO),
               "sdl2-audio") == 0);
  CHECK(strcmp(nxcompat_capability_role_name(NXCOMPAT_ROLE_PORT_DECLARED),
               "port-declared") == 0);
}

static void test_requirements_and_phases(void) {
  nxcompat_registry *registry = NULL;
  nxcompat_requirements requirements;
  nxcompat_requirements before;
  nxcompat_requirement_report report;
  nxcompat_reason_code reason = NXCOMPAT_REASON_NONE;
  char unterminated_name[NXCOMPAT_NAME_MAX];
  char unterminated_text[NXCOMPAT_REQUIREMENTS_TEXT_MAX];
  CHECK(nxcompat_registry_create(&registry) == NXCOMPAT_OK);
  CHECK(registry != NULL);
  CHECK(nxcompat_requirements_parse(
            "graphics.gles2\naudio.output-open\ninput.controller-api",
            &requirements) == NXCOMPAT_OK);
  CHECK(requirements.count == 3u);
  CHECK(requirements.capability_ids[0] == NXCOMPAT_CAPABILITY_GRAPHICS_GLES2);
  memcpy(&before, &requirements, sizeof(before));
  CHECK(nxcompat_requirements_parse_ex("host.unregistered", &requirements,
                                       &reason) == NXCOMPAT_INVALID);
  CHECK(reason == NXCOMPAT_REASON_REQUIREMENT_UNKNOWN);
  CHECK(memcmp(&requirements, &before, sizeof(before)) == 0);
  CHECK(nxcompat_requirements_parse_ex("host.portmaster\nhost.portmaster",
                                       &requirements, &reason) ==
        NXCOMPAT_INVALID);
  CHECK(reason == NXCOMPAT_REASON_REQUIREMENT_DUPLICATE);
  CHECK(nxcompat_requirements_parse_ex(
            "host.portmaster\n\ngraphics.gles2", &requirements, &reason) ==
        NXCOMPAT_INVALID);
  CHECK(reason == NXCOMPAT_REASON_OBSERVATION_MALFORMED);
  memset(unterminated_name, 'a', sizeof(unterminated_name));
  CHECK(nxcompat_capability_definition_by_name(unterminated_name) == NULL);
  memset(unterminated_text, 'a', sizeof(unterminated_text));
  memcpy(&before, &requirements, sizeof(before));
  CHECK(nxcompat_requirements_parse_ex(unterminated_text, &requirements,
                                       &reason) == NXCOMPAT_INVALID);
  CHECK(reason == NXCOMPAT_REASON_OBSERVATION_MALFORMED);
  CHECK(memcmp(&requirements, &before, sizeof(before)) == 0);
  CHECK(nxcompat_requirements_parse("graphics.gles2", &requirements) ==
        NXCOMPAT_OK);
  CHECK(nxcompat_requirements_evaluate(registry, &requirements,
                                       NXCOMPAT_PHASE_PREFLIGHT,
                                       &report) == NXCOMPAT_OK);
  CHECK(report.pending_count == 1u && report.missing_count == 0u);
  CHECK(report.results[0].state == NXCOMPAT_REQUIREMENT_PENDING);
  CHECK(nxcompat_requirements_evaluate(registry, &requirements,
                                       NXCOMPAT_PHASE_GRAPHICS,
                                       &report) == NXCOMPAT_FAILED);
  CHECK(report.missing_count == 1u);
  CHECK(setenv("NXCOMPAT_REQUIRED_CAPABILITIES",
               "host.portmaster\ngraphics.gles2", 1) == 0);
  CHECK(nxcompat_requirements_parse_runtime_ex(&requirements, &reason) ==
        NXCOMPAT_OK);
  CHECK(reason == NXCOMPAT_REASON_NONE);
  CHECK(requirements.count == 2u);
  CHECK(unsetenv("NXCOMPAT_REQUIRED_CAPABILITIES") == 0);
  CHECK(nxcompat_requirements_parse_runtime(&requirements) == NXCOMPAT_OK);
  CHECK(requirements.count == 0u);
  nxcompat_registry_destroy(registry);
}

static void test_host_seed_and_receipts(void) {
  nxcompat_registry *registry = NULL;
  nxcompat_host host = make_host();
  nxcompat_graphics_receipt graphics = make_graphics(1u);
  nxcompat_audio_receipt audio = make_audio(1u);
  nxcompat_input_receipt input = make_input(1u, 0u);
  nxcompat_capability_evidence value;
  nxcompat_requirements none;
  nxcompat_runtime_report before;
  nxcompat_runtime_report after;
  nxcompat_graphics_options diagnostic;
  nxcompat_graphics free_graphics;
  nxcompat_reason_code reason = NXCOMPAT_REASON_NONE;
  uint64_t probe_mapping_generation = 0u;

  CHECK(nxcompat_registry_create(&registry) == NXCOMPAT_OK);
  CHECK(nxcompat_registry_seed_host(registry, &host) == NXCOMPAT_OK);
  value = evidence(registry, NXCOMPAT_CAPABILITY_HOST_PORTMASTER);
  CHECK(value.state == NXCOMPAT_EVIDENCE_OBSERVED);
  value = evidence(registry, NXCOMPAT_CAPABILITY_HOST_NETWORK_FILESYSTEM);
  CHECK(value.state == NXCOMPAT_EVIDENCE_OBSERVED);
  value = evidence(registry, NXCOMPAT_CAPABILITY_HOST_FUSE_LIKE_FILESYSTEM);
  CHECK(value.state == NXCOMPAT_EVIDENCE_ABSENT);
  value = evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING);
  CHECK(value.state == NXCOMPAT_EVIDENCE_OBSERVED &&
        value.source == NXCOMPAT_SOURCE_PROBE);

  memset(&diagnostic, 0, sizeof(diagnostic));
  diagnostic.api_version = NXCOMPAT_API_VERSION;
  diagnostic.struct_size = sizeof(diagnostic);
  diagnostic.version = "OpenGL ES 3.2 diagnostic-only";
  diagnostic.renderer = "free-form";
  CHECK(nxcompat_capture_graphics(&diagnostic, &free_graphics) == 0);
  CHECK((free_graphics.capabilities & NXCOMPAT_GRAPHICS_GLES3) != 0u);
  value = evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_GLES3);
  CHECK(value.state == NXCOMPAT_EVIDENCE_ABSENT);

  CHECK(nxcompat_registry_publish_graphics(registry, &graphics) == NXCOMPAT_OK);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_WINDOW).state ==
        NXCOMPAT_EVIDENCE_OPENED);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_GLES2).state ==
        NXCOMPAT_EVIDENCE_OPENED);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_GLES3).state ==
        NXCOMPAT_EVIDENCE_ABSENT);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_EGL_CONFIG).state ==
        NXCOMPAT_EVIDENCE_OPENED);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_DRAWABLE).state ==
        NXCOMPAT_EVIDENCE_OPENED);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_ETC1).state ==
        NXCOMPAT_EVIDENCE_OPENED);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_NPOT_FULL).state ==
        NXCOMPAT_EVIDENCE_OPENED);

  CHECK(nxcompat_requirements_parse(NULL, &none) == NXCOMPAT_OK);
  CHECK(nxcompat_registry_runtime_report(registry, &none,
                                         NXCOMPAT_PHASE_READY,
                                         &before) == NXCOMPAT_OK);
  graphics.generation = 2u;
  graphics.drawable_width = 0;
  CHECK(nxcompat_registry_publish_graphics(registry, &graphics) ==
        NXCOMPAT_INVALID);
  CHECK(nxcompat_registry_runtime_report(registry, &none,
                                         NXCOMPAT_PHASE_READY,
                                         &after) == NXCOMPAT_OK);
  CHECK(memcmp(&before, &after, sizeof(before)) == 0);
  graphics = make_graphics(1u);
  CHECK(nxcompat_registry_publish_graphics_ex(registry, &graphics, &reason) ==
        NXCOMPAT_FAILED);
  CHECK(reason == NXCOMPAT_REASON_CAPABILITY_STALE);

  memset(&graphics, 0, sizeof(graphics));
  graphics.api_version = NXCOMPAT_API_VERSION;
  graphics.struct_size = sizeof(graphics);
  graphics.source = NXCOMPAT_SOURCE_NXGL;
  graphics.generation = 2u;
  CHECK(nxcompat_registry_publish_graphics(registry, &graphics) == NXCOMPAT_OK);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_WINDOW).state ==
        NXCOMPAT_EVIDENCE_LOST);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_GRAPHICS_EGL_CONFIG).state ==
        NXCOMPAT_EVIDENCE_LOST);

  CHECK(nxcompat_registry_publish_audio(registry, &audio) == NXCOMPAT_OK);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_AUDIO_OUTPUT_OPEN).state ==
        NXCOMPAT_EVIDENCE_OPENED);
  audio.generation = 2u;
  copy_text(audio.backend, sizeof(audio.backend), "dummy");
  CHECK(nxcompat_registry_publish_audio_ex(registry, &audio, &reason) ==
        NXCOMPAT_INVALID);
  CHECK(reason == NXCOMPAT_REASON_BACKEND_FAKE_OUTPUT);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_AUDIO_OUTPUT_OPEN).state ==
        NXCOMPAT_EVIDENCE_OPENED);
  memset(&audio, 0, sizeof(audio));
  audio.api_version = NXCOMPAT_API_VERSION;
  audio.struct_size = sizeof(audio);
  audio.source = NXCOMPAT_SOURCE_SDL2_AUDIO;
  audio.generation = 2u;
  CHECK(nxcompat_registry_publish_audio(registry, &audio) == NXCOMPAT_OK);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_AUDIO_OUTPUT_OPEN).state ==
        NXCOMPAT_EVIDENCE_LOST);
  audio = make_audio(3u);
  audio.source = NXCOMPAT_SOURCE_ENGINE_ADAPTER;
  audio.lifetime = NXCOMPAT_AUDIO_ACTIVE_ENGINE_OWNED;
  CHECK(nxcompat_registry_publish_audio(registry, &audio) == NXCOMPAT_OK);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_AUDIO_OUTPUT_OPEN).state ==
        NXCOMPAT_EVIDENCE_ACTIVE);
  audio.generation = 4u;
  audio.frequency = 0;
  CHECK(nxcompat_registry_publish_audio(registry, &audio) == NXCOMPAT_INVALID);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_AUDIO_OUTPUT_OPEN).state ==
        NXCOMPAT_EVIDENCE_ACTIVE);

  CHECK(nxcompat_registry_publish_input(registry, &input) == NXCOMPAT_OK);
  value = evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING);
  CHECK(value.state == NXCOMPAT_EVIDENCE_OBSERVED &&
        value.source == NXCOMPAT_SOURCE_PROBE);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_API).state ==
        NXCOMPAT_EVIDENCE_ACTIVE);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_INPUT_HOTPLUG).state ==
        NXCOMPAT_EVIDENCE_ACTIVE);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_CONNECTED)
            .state == NXCOMPAT_EVIDENCE_ABSENT);
  input = make_input(2u, 1u);
  CHECK(nxcompat_registry_publish_input(registry, &input) == NXCOMPAT_OK);
  value = evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING);
  CHECK(value.state == NXCOMPAT_EVIDENCE_OPENED &&
        value.source == NXCOMPAT_SOURCE_NXINPUT &&
        value.reason == NXCOMPAT_REASON_INPUT_CONTROLLER_MAPPING_RESOLVED);
  CHECK(strcmp(nxcompat_reason_name(value.reason),
               "input-controller-mapping-resolved") == 0);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_CONNECTED)
            .state == NXCOMPAT_EVIDENCE_ACTIVE);
  input = make_input(3u, 0u);
  CHECK(nxcompat_registry_publish_input(registry, &input) == NXCOMPAT_OK);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_CONNECTED)
            .state == NXCOMPAT_EVIDENCE_LOST);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_CONNECTED)
            .reason == NXCOMPAT_REASON_INPUT_CONTROLLER_LOST);
  value = evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING);
  CHECK(value.state == NXCOMPAT_EVIDENCE_OBSERVED &&
        value.source == NXCOMPAT_SOURCE_PROBE);
  CHECK(value.generation == input.topology_generation);
  CHECK(nxcompat_registry_seed_host(registry, &host) == NXCOMPAT_OK);
  value = evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING);
  CHECK(value.state == NXCOMPAT_EVIDENCE_OBSERVED &&
        value.source == NXCOMPAT_SOURCE_PROBE);
  probe_mapping_generation = value.generation;
  CHECK(probe_mapping_generation > input.topology_generation);
  CHECK(nxcompat_registry_publish_input_ex(registry, &input, &reason) ==
        NXCOMPAT_FAILED);
  CHECK(reason == NXCOMPAT_REASON_CAPABILITY_STALE);
  input = make_input(4u, 0u);
  input.proof_flags &= ~(uint32_t)NXCOMPAT_INPUT_PROOF_RESCAN_ACTIVE;
  CHECK(nxcompat_registry_publish_input(registry, &input) == NXCOMPAT_OK);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_API).state ==
        NXCOMPAT_EVIDENCE_ACTIVE);
  CHECK(evidence(registry, NXCOMPAT_CAPABILITY_INPUT_HOTPLUG).state ==
        NXCOMPAT_EVIDENCE_LOST);
  value = evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING);
  CHECK(value.state == NXCOMPAT_EVIDENCE_OBSERVED &&
        value.source == NXCOMPAT_SOURCE_PROBE &&
        value.generation == probe_mapping_generation);
  nxcompat_registry_destroy(registry);
}

static void test_registry_hostile_invariants_and_overflow(void) {
  nxcompat_registry *registry = NULL;
  nxcompat_graphics_receipt graphics = make_graphics(1u);
  nxcompat_audio_receipt audio = make_audio(1u);
  nxcompat_input_receipt input = make_input(1u, 0u);
  nxcompat_capability_evidence output;
  nxcompat_reason_code reason = NXCOMPAT_REASON_NONE;
  unsigned char snapshot[sizeof(*registry)];

  CHECK(nxcompat_registry_create(&registry) == NXCOMPAT_OK);
  registry->evidence[NXCOMPAT_CAPABILITY_HOST_PORTMASTER].phase =
      NXCOMPAT_PHASE_GRAPHICS;
  CHECK(nxcompat_registry_get(registry,
                              NXCOMPAT_CAPABILITY_HOST_PORTMASTER,
                              &output) == NXCOMPAT_INVALID);
  registry->evidence[NXCOMPAT_CAPABILITY_HOST_PORTMASTER].phase =
      NXCOMPAT_PHASE_PREFLIGHT;
  CHECK(nxcompat_registry_get(registry,
                              NXCOMPAT_CAPABILITY_HOST_PORTMASTER,
                              &output) == NXCOMPAT_OK);
  registry->has_audio = 2;
  CHECK(nxcompat_registry_get(registry,
                              NXCOMPAT_CAPABILITY_HOST_PORTMASTER,
                              &output) == NXCOMPAT_INVALID);
  registry->has_audio = 0;

  registry->generation = UINT64_MAX;
  memcpy(snapshot, registry, sizeof(snapshot));
  CHECK(nxcompat_registry_publish_graphics_ex(registry, &graphics, &reason) ==
        NXCOMPAT_FAILED);
  CHECK(reason == NXCOMPAT_REASON_OBSERVATION_OUT_OF_RANGE);
  CHECK(memcmp(snapshot, registry, sizeof(snapshot)) == 0);
  CHECK(nxcompat_registry_publish_audio_ex(registry, &audio, &reason) ==
        NXCOMPAT_FAILED);
  CHECK(reason == NXCOMPAT_REASON_OBSERVATION_OUT_OF_RANGE);
  CHECK(memcmp(snapshot, registry, sizeof(snapshot)) == 0);
  CHECK(nxcompat_registry_publish_input_ex(registry, &input, &reason) ==
        NXCOMPAT_FAILED);
  CHECK(reason == NXCOMPAT_REASON_OBSERVATION_OUT_OF_RANGE);
  CHECK(memcmp(snapshot, registry, sizeof(snapshot)) == 0);
  nxcompat_registry_destroy(registry);
}

static void test_mapping_source_aggregation(void) {
  nxcompat_registry *registry = NULL;
  nxcompat_host host = make_host();
  nxcompat_input_receipt input = make_input(1u, 1u);
  nxcompat_capability_evidence value;
  uint64_t restored_probe_generation;

  host.capabilities &= ~(uint64_t)NXCOMPAT_CAP_CONTROLLER_MAPPING;
  CHECK(nxcompat_registry_create(&registry) == NXCOMPAT_OK);
  CHECK(nxcompat_registry_seed_host(registry, &host) == NXCOMPAT_OK);
  input = make_input(1u, 0u);
  input.proof_flags |= NXCOMPAT_INPUT_PROOF_MAPPING_AVAILABLE;
  input.mapping_source = NXCOMPAT_INPUT_MAPPING_DATABASE;
  CHECK(nxcompat_registry_publish_input(registry, &input) == NXCOMPAT_OK);
  value = evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING);
  CHECK(value.state == NXCOMPAT_EVIDENCE_OBSERVED &&
        value.source == NXCOMPAT_SOURCE_NXINPUT);

  input = make_input(2u, 1u);
  input.mapping_source = NXCOMPAT_INPUT_MAPPING_DATABASE;
  CHECK(nxcompat_registry_publish_input(registry, &input) == NXCOMPAT_OK);
  value = evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING);
  CHECK(value.state == NXCOMPAT_EVIDENCE_OBSERVED &&
        value.source == NXCOMPAT_SOURCE_NXINPUT);

  input = make_input(3u, 1u);
  CHECK(nxcompat_registry_publish_input(registry, &input) == NXCOMPAT_OK);
  value = evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING);
  CHECK(value.state == NXCOMPAT_EVIDENCE_OPENED &&
        value.source == NXCOMPAT_SOURCE_NXINPUT);

  input = make_input(4u, 0u);
  CHECK(nxcompat_registry_publish_input(registry, &input) == NXCOMPAT_OK);
  value = evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING);
  CHECK(value.state == NXCOMPAT_EVIDENCE_LOST &&
        value.source == NXCOMPAT_SOURCE_NXINPUT);

  host.capabilities |= NXCOMPAT_CAP_CONTROLLER_MAPPING;
  CHECK(nxcompat_registry_seed_host(registry, &host) == NXCOMPAT_OK);
  value = evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING);
  CHECK(value.state == NXCOMPAT_EVIDENCE_OBSERVED &&
        value.source == NXCOMPAT_SOURCE_PROBE);
  restored_probe_generation = value.generation;
  CHECK(restored_probe_generation > input.topology_generation);

  input = make_input(5u, 0u);
  CHECK(nxcompat_registry_publish_input(registry, &input) == NXCOMPAT_OK);
  value = evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING);
  CHECK(value.state == NXCOMPAT_EVIDENCE_OBSERVED &&
        value.source == NXCOMPAT_SOURCE_PROBE &&
        value.generation == restored_probe_generation);

  input = make_input(6u, 1u);
  CHECK(nxcompat_registry_publish_input(registry, &input) == NXCOMPAT_OK);
  host.capabilities &= ~(uint64_t)NXCOMPAT_CAP_CONTROLLER_MAPPING;
  CHECK(nxcompat_registry_seed_host(registry, &host) == NXCOMPAT_OK);
  value = evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING);
  CHECK(value.state == NXCOMPAT_EVIDENCE_OPENED &&
        value.source == NXCOMPAT_SOURCE_NXINPUT &&
        value.generation > restored_probe_generation);
  nxcompat_registry_destroy(registry);
}

static void test_receipt_generation_namespace(void) {
  nxcompat_registry *registry = NULL;
  nxcompat_host host = make_host();
  nxcompat_input_receipt input = make_input(UINT64_C(1000000), 0u);
  nxcompat_capability_evidence value;

  CHECK(nxcompat_registry_create(&registry) == NXCOMPAT_OK);
  CHECK(nxcompat_registry_publish_input(registry, &input) == NXCOMPAT_OK);
  value = evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_API);
  CHECK(value.state == NXCOMPAT_EVIDENCE_ACTIVE &&
        value.generation == input.topology_generation);
  /* Receipt/topology generations belong to their provider. They may be much
   * larger than the registry's internal publication counter. */
  CHECK(nxcompat_registry_seed_host(registry, &host) == NXCOMPAT_OK);
  value = evidence(registry, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_API);
  CHECK(value.state == NXCOMPAT_EVIDENCE_ACTIVE &&
        value.generation == input.topology_generation);
  nxcompat_registry_destroy(registry);
}

static void test_sanitized_runtime_report(void) {
  nxcompat_registry *registry = NULL;
  nxcompat_host host = make_host();
  nxcompat_graphics_receipt graphics = make_graphics(1u);
  nxcompat_audio_receipt audio = make_audio(1u);
  nxcompat_input_receipt input = make_input(1u, 1u);
  nxcompat_requirements requirements;
  nxcompat_runtime_report report;
  nxcompat_plan_v2 plan;
  char json[32768];
  char tiny[8];

  copy_text(host.device_model, sizeof(host.device_model),
            "/home/private-user/private-device");
  copy_text(host.os_version, sizeof(host.os_version),
            "198.51.100.42 access_token=do-not-log");
  copy_text(graphics.gl_renderer, sizeof(graphics.gl_renderer),
            "/mnt/synthetic-volume/renderer-secret");
  CHECK(nxcompat_registry_create(&registry) == NXCOMPAT_OK);
  CHECK(nxcompat_registry_seed_host(registry, &host) == NXCOMPAT_OK);
  CHECK(nxcompat_registry_publish_graphics(registry, &graphics) == NXCOMPAT_OK);
  CHECK(nxcompat_registry_publish_audio(registry, &audio) == NXCOMPAT_OK);
  CHECK(nxcompat_registry_publish_input(registry, &input) == NXCOMPAT_OK);
  CHECK(nxcompat_requirements_parse(
            "graphics.gles2\ngraphics.egl-config\ngraphics.drawable\n"
            "audio.output-open\ninput.controller-api",
            &requirements) == NXCOMPAT_OK);
  CHECK(nxcompat_registry_runtime_report(registry, &requirements,
                                         NXCOMPAT_PHASE_READY,
                                         &report) == NXCOMPAT_OK);
  CHECK(strcmp(report.graphics.gl_renderer, "[redacted]") == 0);
  CHECK(report.graphics.gl_extensions[0] == '\0');
  memset(&plan, 0, sizeof(plan));
  plan.api_version = NXCOMPAT_API_VERSION_V2;
  plan.struct_size = sizeof(plan);
  plan.runtime_arch = NXCOMPAT_ARCH_ARMV7;
  plan.action_count = 1u;
  plan.actions[0].id = NXCOMPAT_ACTION_SESSION_RUNTIME;
  plan.actions[0].state = NXCOMPAT_ACTION_V2_APPLIED;
  plan.actions[0].reason_code = NXCOMPAT_REASON_ENV_APPLIED;
  copy_text(plan.actions[0].variable, sizeof(plan.actions[0].variable),
            "PRIVATE_TOKEN");
  copy_text(plan.actions[0].value, sizeof(plan.actions[0].value),
            "/home/private-user/token-value");
  copy_text(plan.actions[0].reason, sizeof(plan.actions[0].reason),
            "password=hidden");
  CHECK(nxcompat_format_runtime_json(&host, &plan, &report, json,
                                     sizeof(json)) > 0);
  CHECK(json[0] == '{');
  CHECK(strstr(json, "\"sanitized\":true") != NULL);
  CHECK(strstr(json, "\"report_reason_code\":550") != NULL);
  CHECK(strstr(json, "graphics.egl-config") != NULL);
  CHECK(strstr(json, "\"device_model\":\"[redacted]\"") != NULL);
  CHECK(strstr(json, "private-user") == NULL);
  CHECK(strstr(json, "/home/") == NULL);
  CHECK(strstr(json, "/mnt/") == NULL);
  CHECK(strstr(json, "198.51.100.42") == NULL);
  CHECK(strstr(json, "do-not-log") == NULL);
  CHECK(strstr(json, "PRIVATE_TOKEN") == NULL);
  CHECK(strstr(json, "token-value") == NULL);
  CHECK(strstr(json, "password=hidden") == NULL);
  CHECK(strstr(json, "session-runtime") != NULL);
  if (getenv("NXCOMPAT_TEST_DUMP_RUNTIME_JSON"))
    (void)fprintf(stdout, "%s\n", json);
  CHECK(nxcompat_format_runtime_json(&host, &plan, &report, tiny,
                                     sizeof(tiny)) == -1);
  CHECK(tiny[sizeof(tiny) - 1u] == '\0');
  nxcompat_registry_destroy(registry);
}

int main(void) {
  test_catalog_exact();
  test_requirements_and_phases();
  test_host_seed_and_receipts();
  test_registry_hostile_invariants_and_overflow();
  test_mapping_source_aggregation();
  test_receipt_generation_namespace();
  test_sanitized_runtime_report();
  if (failures != 0)
    return 1;
  (void)fprintf(stdout, "nxcompat registry/receipt tests passed\n");
  return 0;
}
