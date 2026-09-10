/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxcompat_registry_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define NXCOMPAT_GRAPHICS_PROOF_ALL                                        \
  (NXCOMPAT_GRAPHICS_PROOF_WINDOW_CREATED |                               \
   NXCOMPAT_GRAPHICS_PROOF_CONTEXT_CURRENT |                              \
   NXCOMPAT_GRAPHICS_PROOF_GL_STRINGS_REAL |                              \
   NXCOMPAT_GRAPHICS_PROOF_EGL_DISPLAY_CURRENT |                          \
   NXCOMPAT_GRAPHICS_PROOF_EGL_CONTEXT_CURRENT |                          \
   NXCOMPAT_GRAPHICS_PROOF_EGL_CONFIG_QUERIED |                           \
   NXCOMPAT_GRAPHICS_PROOF_DRAWABLE_POSITIVE)

#define NXCOMPAT_AUDIO_PROOF_ALL                                           \
  (NXCOMPAT_AUDIO_PROOF_BACKEND_INITIALIZED |                             \
   NXCOMPAT_AUDIO_PROOF_DEVICE_OPENED | NXCOMPAT_AUDIO_PROOF_SPEC_OBTAINED)

#define NXCOMPAT_INPUT_PROOF_ALL                                           \
  (NXCOMPAT_INPUT_PROOF_CONTROLLER_SUBSYSTEM_ACTIVE |                     \
   NXCOMPAT_INPUT_PROOF_MAPPING_AVAILABLE |                               \
   NXCOMPAT_INPUT_PROOF_INITIAL_SCAN_DONE |                               \
   NXCOMPAT_INPUT_PROOF_EVENT_WATCH_ACTIVE |                              \
   NXCOMPAT_INPUT_PROOF_RESCAN_ACTIVE |                                   \
   NXCOMPAT_INPUT_PROOF_CONTROLLER_OPENED)

#define NXCOMPAT_EGL_OPENGL_ES_BIT 0x0001
#define NXCOMPAT_EGL_OPENGL_ES2_BIT 0x0004
#define NXCOMPAT_EGL_OPENGL_ES3_BIT 0x0040
#define NXCOMPAT_EGL_WINDOW_BIT 0x0004

static int nxcompat_dimension_valid(int value) {
  return value > 0 && value <= 32768;
}

static int nxcompat_bits_valid(int value) {
  return value >= 0 && value <= 64;
}

static int nxcompat_case_equal(const char *left, const char *right) {
  if (!left || !right)
    return 0;
  while (*left && *right) {
    if (tolower((unsigned char)*left) != tolower((unsigned char)*right))
      return 0;
    ++left;
    ++right;
  }
  return *left == '\0' && *right == '\0';
}

static int nxcompat_backend_name_valid(const char *value, size_t size) {
  size_t index;
  if (!nxcompat_registry_bounded_string(value, size) || !value[0])
    return 0;
  for (index = 0u; value[index]; ++index) {
    unsigned char character = (unsigned char)value[index];
    if (!isalnum(character) && character != (unsigned char)'_' &&
        character != (unsigned char)'-' && character != (unsigned char)'.')
      return 0;
  }
  return !nxcompat_case_equal(value, "dummy") &&
         !nxcompat_case_equal(value, "disk") &&
         !nxcompat_case_equal(value, "offscreen");
}

static int nxcompat_backend_name_fake(const char *value, size_t size) {
  if (!nxcompat_registry_bounded_string(value, size))
    return 0;
  return nxcompat_case_equal(value, "dummy") ||
         nxcompat_case_equal(value, "disk") ||
         nxcompat_case_equal(value, "offscreen");
}

static void nxcompat_receipt_store_reason(nxcompat_reason_code *reason,
                                          nxcompat_reason_code value) {
  if (reason)
    *reason = value;
}

static int nxcompat_extension_present(const char *extensions,
                                      const char *wanted) {
  size_t wanted_length;
  const char *cursor;
  if (!extensions || !wanted || !wanted[0])
    return 0;
  wanted_length = strlen(wanted);
  cursor = extensions;
  while ((cursor = strstr(cursor, wanted)) != NULL) {
    int starts_token = cursor == extensions || cursor[-1] == ' ';
    int ends_token = cursor[wanted_length] == '\0' ||
                     cursor[wanted_length] == ' ';
    if (starts_token && ends_token)
      return 1;
    cursor += wanted_length;
  }
  return 0;
}

static int nxcompat_parse_gles_version(const char *version, int *major,
                                       int *minor) {
  const char *prefix = "OpenGL ES";
  const char *cursor;
  char *end;
  long parsed_major;
  long parsed_minor;
  if (!version || strncmp(version, prefix, strlen(prefix)) != 0)
    return 0;
  cursor = version + strlen(prefix);
  while (*cursor == ' ' || *cursor == '-')
    ++cursor;
  if (!isdigit((unsigned char)*cursor))
    return 0;
  parsed_major = strtol(cursor, &end, 10);
  if (end == cursor || *end != '.')
    return 0;
  cursor = end + 1;
  parsed_minor = strtol(cursor, &end, 10);
  if (end == cursor || parsed_major < 1 || parsed_major > 9 ||
      parsed_minor < 0 || parsed_minor > 99)
    return 0;
  *major = (int)parsed_major;
  *minor = (int)parsed_minor;
  return 1;
}

static int nxcompat_graphics_strings_bounded(
    const nxcompat_graphics_receipt *receipt) {
  return nxcompat_registry_bounded_string(receipt->video_backend,
                                          sizeof(receipt->video_backend)) &&
         nxcompat_registry_bounded_string(receipt->gl_vendor,
                                          sizeof(receipt->gl_vendor)) &&
         nxcompat_registry_bounded_string(receipt->gl_renderer,
                                          sizeof(receipt->gl_renderer)) &&
         nxcompat_registry_bounded_string(receipt->gl_version,
                                          sizeof(receipt->gl_version)) &&
         nxcompat_registry_bounded_string(receipt->glsl_version,
                                          sizeof(receipt->glsl_version)) &&
         nxcompat_registry_bounded_string(receipt->gl_extensions,
                                          sizeof(receipt->gl_extensions)) &&
         nxcompat_registry_bounded_string(receipt->egl_vendor,
                                          sizeof(receipt->egl_vendor)) &&
         nxcompat_registry_bounded_string(receipt->egl_version,
                                          sizeof(receipt->egl_version)) &&
         nxcompat_registry_bounded_string(receipt->egl_client_apis,
                                          sizeof(receipt->egl_client_apis));
}

static int nxcompat_graphics_receipt_valid(
    const nxcompat_graphics_receipt *receipt) {
  uint32_t flags;
  int parsed_major = 0;
  int parsed_minor = 0;
  if (!receipt || receipt->api_version != NXCOMPAT_API_VERSION ||
      receipt->struct_size < sizeof(*receipt) || receipt->generation == 0u ||
      (receipt->source != NXCOMPAT_SOURCE_NXGL &&
       receipt->source != NXCOMPAT_SOURCE_ENGINE_ADAPTER) ||
      (receipt->proof_flags & ~(uint32_t)NXCOMPAT_GRAPHICS_PROOF_ALL) != 0u ||
      !nxcompat_graphics_strings_bounded(receipt))
    return 0;
  flags = receipt->proof_flags;
  if ((flags & NXCOMPAT_GRAPHICS_PROOF_CONTEXT_CURRENT) != 0u &&
      (flags & NXCOMPAT_GRAPHICS_PROOF_WINDOW_CREATED) == 0u)
    return 0;
  if ((flags & NXCOMPAT_GRAPHICS_PROOF_GL_STRINGS_REAL) != 0u &&
      (flags & NXCOMPAT_GRAPHICS_PROOF_CONTEXT_CURRENT) == 0u)
    return 0;
  if ((flags & NXCOMPAT_GRAPHICS_PROOF_EGL_CONTEXT_CURRENT) != 0u &&
      ((flags & NXCOMPAT_GRAPHICS_PROOF_EGL_DISPLAY_CURRENT) == 0u ||
       (flags & NXCOMPAT_GRAPHICS_PROOF_CONTEXT_CURRENT) == 0u))
    return 0;
  if ((flags & NXCOMPAT_GRAPHICS_PROOF_EGL_CONFIG_QUERIED) != 0u &&
      (flags & (NXCOMPAT_GRAPHICS_PROOF_EGL_DISPLAY_CURRENT |
                NXCOMPAT_GRAPHICS_PROOF_EGL_CONTEXT_CURRENT |
                NXCOMPAT_GRAPHICS_PROOF_GL_STRINGS_REAL)) !=
          (NXCOMPAT_GRAPHICS_PROOF_EGL_DISPLAY_CURRENT |
           NXCOMPAT_GRAPHICS_PROOF_EGL_CONTEXT_CURRENT |
           NXCOMPAT_GRAPHICS_PROOF_GL_STRINGS_REAL))
    return 0;
  if ((flags & NXCOMPAT_GRAPHICS_PROOF_DRAWABLE_POSITIVE) != 0u &&
      (flags & NXCOMPAT_GRAPHICS_PROOF_CONTEXT_CURRENT) == 0u)
    return 0;

  if ((flags & NXCOMPAT_GRAPHICS_PROOF_WINDOW_CREATED) != 0u) {
    if (!nxcompat_dimension_valid(receipt->window_width) ||
        !nxcompat_dimension_valid(receipt->window_height) ||
        !nxcompat_backend_name_valid(receipt->video_backend,
                                     sizeof(receipt->video_backend)))
      return 0;
  } else if (receipt->window_width != 0 || receipt->window_height != 0 ||
             receipt->video_backend[0]) {
    return 0;
  }

  if ((flags & NXCOMPAT_GRAPHICS_PROOF_GL_STRINGS_REAL) != 0u) {
    if (!receipt->gl_vendor[0] || !receipt->gl_renderer[0] ||
        !receipt->gl_version[0] || !receipt->glsl_version[0] ||
        !nxcompat_parse_gles_version(receipt->gl_version, &parsed_major,
                                     &parsed_minor) ||
        parsed_major != receipt->gles_major ||
        parsed_minor != receipt->gles_minor ||
        !nxcompat_bits_valid(receipt->red_bits) ||
        !nxcompat_bits_valid(receipt->green_bits) ||
        !nxcompat_bits_valid(receipt->blue_bits) ||
        !nxcompat_bits_valid(receipt->alpha_bits) ||
        !nxcompat_bits_valid(receipt->depth_bits) ||
        !nxcompat_bits_valid(receipt->stencil_bits) ||
        (receipt->double_buffer != 0 && receipt->double_buffer != 1) ||
        receipt->profile_mask < 0)
      return 0;
  } else if (receipt->gles_major != 0 || receipt->gles_minor != 0 ||
             receipt->red_bits != 0 || receipt->green_bits != 0 ||
             receipt->blue_bits != 0 || receipt->alpha_bits != 0 ||
             receipt->depth_bits != 0 || receipt->stencil_bits != 0 ||
             receipt->double_buffer != 0 || receipt->profile_mask != 0 ||
             receipt->gl_vendor[0] || receipt->gl_renderer[0] ||
             receipt->gl_version[0] || receipt->glsl_version[0] ||
             receipt->gl_extensions[0]) {
    return 0;
  }

  if ((flags & NXCOMPAT_GRAPHICS_PROOF_DRAWABLE_POSITIVE) != 0u) {
    if (!nxcompat_dimension_valid(receipt->drawable_width) ||
        !nxcompat_dimension_valid(receipt->drawable_height))
      return 0;
  } else if (receipt->drawable_width != 0 || receipt->drawable_height != 0) {
    return 0;
  }

  if ((flags & NXCOMPAT_GRAPHICS_PROOF_EGL_DISPLAY_CURRENT) != 0u) {
    if (!receipt->egl_vendor[0] || !receipt->egl_version[0] ||
        !receipt->egl_client_apis[0])
      return 0;
  } else if (receipt->egl_vendor[0] || receipt->egl_version[0] ||
             receipt->egl_client_apis[0]) {
    return 0;
  }

  if ((flags & NXCOMPAT_GRAPHICS_PROOF_EGL_CONFIG_QUERIED) != 0u) {
    if (receipt->egl_config_id <= 0 ||
        !nxcompat_bits_valid(receipt->egl_red_bits) ||
        !nxcompat_bits_valid(receipt->egl_green_bits) ||
        !nxcompat_bits_valid(receipt->egl_blue_bits) ||
        !nxcompat_bits_valid(receipt->egl_alpha_bits) ||
        !nxcompat_bits_valid(receipt->egl_depth_bits) ||
        !nxcompat_bits_valid(receipt->egl_stencil_bits) ||
        receipt->egl_renderable_type <= 0 || receipt->egl_surface_type <= 0 ||
        (receipt->egl_surface_type & NXCOMPAT_EGL_WINDOW_BIT) == 0 ||
        (receipt->gles_major >= 2 &&
         (receipt->egl_renderable_type &
          (NXCOMPAT_EGL_OPENGL_ES2_BIT | NXCOMPAT_EGL_OPENGL_ES3_BIT)) == 0) ||
        (receipt->gles_major == 1 &&
         (receipt->egl_renderable_type & NXCOMPAT_EGL_OPENGL_ES_BIT) == 0))
      return 0;
  } else if (receipt->egl_config_id != 0 || receipt->egl_red_bits != 0 ||
             receipt->egl_green_bits != 0 || receipt->egl_blue_bits != 0 ||
             receipt->egl_alpha_bits != 0 || receipt->egl_depth_bits != 0 ||
             receipt->egl_stencil_bits != 0 ||
             receipt->egl_renderable_type != 0 ||
             receipt->egl_surface_type != 0) {
    return 0;
  }
  return 1;
}

nxcompat_result_code nxcompat_registry_publish_graphics_ex(
    nxcompat_registry *registry, const nxcompat_graphics_receipt *receipt,
    nxcompat_reason_code *reason) {
  nxcompat_registry staged;
  int gl_real;
  int egl_real;
  int gles2;
  int gles3;
  nxcompat_receipt_store_reason(reason, NXCOMPAT_REASON_INVALID_ARGUMENT);
  if (!nxcompat_registry_instance_valid(registry) || !receipt)
    return NXCOMPAT_INVALID;
  if (receipt->api_version != NXCOMPAT_API_VERSION) {
    nxcompat_receipt_store_reason(reason, NXCOMPAT_REASON_UNSUPPORTED_API);
    return NXCOMPAT_INVALID;
  }
  if (receipt->struct_size < sizeof(*receipt)) {
    nxcompat_receipt_store_reason(reason, NXCOMPAT_REASON_STRUCT_TOO_SMALL);
    return NXCOMPAT_INVALID;
  }
  if (!nxcompat_graphics_receipt_valid(receipt)) {
    nxcompat_receipt_store_reason(
        reason,
        nxcompat_backend_name_fake(receipt->video_backend,
                                   sizeof(receipt->video_backend))
            ? NXCOMPAT_REASON_BACKEND_FAKE_OUTPUT
            : NXCOMPAT_REASON_PROVIDER_CONTRACT);
    return NXCOMPAT_INVALID;
  }
  if (registry->has_graphics &&
      receipt->generation <= registry->graphics.generation) {
    nxcompat_receipt_store_reason(reason, NXCOMPAT_REASON_CAPABILITY_STALE);
    return NXCOMPAT_FAILED;
  }
  if (registry->generation == UINT64_MAX) {
    nxcompat_receipt_store_reason(reason,
                                  NXCOMPAT_REASON_OBSERVATION_OUT_OF_RANGE);
    return NXCOMPAT_FAILED;
  }
  staged = *registry;
  staged.has_graphics = 1;
  staged.graphics = *receipt;
  gl_real = (receipt->proof_flags &
             (NXCOMPAT_GRAPHICS_PROOF_CONTEXT_CURRENT |
              NXCOMPAT_GRAPHICS_PROOF_GL_STRINGS_REAL)) ==
            (NXCOMPAT_GRAPHICS_PROOF_CONTEXT_CURRENT |
             NXCOMPAT_GRAPHICS_PROOF_GL_STRINGS_REAL);
  egl_real = (receipt->proof_flags &
              (NXCOMPAT_GRAPHICS_PROOF_EGL_DISPLAY_CURRENT |
               NXCOMPAT_GRAPHICS_PROOF_EGL_CONTEXT_CURRENT)) ==
             (NXCOMPAT_GRAPHICS_PROOF_EGL_DISPLAY_CURRENT |
              NXCOMPAT_GRAPHICS_PROOF_EGL_CONTEXT_CURRENT);
  gles2 = gl_real && receipt->gles_major >= 2;
  gles3 = gl_real && receipt->gles_major >= 3;
  nxcompat_registry_stage_boolean(
      &staged, NXCOMPAT_CAPABILITY_GRAPHICS_WINDOW,
      (receipt->proof_flags & NXCOMPAT_GRAPHICS_PROOF_WINDOW_CREATED) != 0u,
      NXCOMPAT_EVIDENCE_OPENED, receipt->source,
      NXCOMPAT_REASON_GRAPHICS_WINDOW_OPENED, receipt->generation);
  nxcompat_registry_stage_boolean(
      &staged, NXCOMPAT_CAPABILITY_GRAPHICS_GLES2, gles2,
      NXCOMPAT_EVIDENCE_OPENED, receipt->source,
      NXCOMPAT_REASON_GRAPHICS_GLES_OPENED, receipt->generation);
  nxcompat_registry_stage_boolean(
      &staged, NXCOMPAT_CAPABILITY_GRAPHICS_GLES3, gles3,
      NXCOMPAT_EVIDENCE_OPENED, receipt->source,
      NXCOMPAT_REASON_GRAPHICS_GLES_OPENED, receipt->generation);
  nxcompat_registry_stage_boolean(
      &staged, NXCOMPAT_CAPABILITY_GRAPHICS_EGL, egl_real,
      NXCOMPAT_EVIDENCE_OPENED, receipt->source,
      NXCOMPAT_REASON_GRAPHICS_EGL_OPENED, receipt->generation);
  nxcompat_registry_stage_boolean(
      &staged, NXCOMPAT_CAPABILITY_GRAPHICS_EGL_CONFIG,
      (receipt->proof_flags & NXCOMPAT_GRAPHICS_PROOF_EGL_CONFIG_QUERIED) != 0u,
      NXCOMPAT_EVIDENCE_OPENED, receipt->source,
      NXCOMPAT_REASON_GRAPHICS_EGL_CONFIG_OPENED, receipt->generation);
  nxcompat_registry_stage_boolean(
      &staged, NXCOMPAT_CAPABILITY_GRAPHICS_DRAWABLE,
      (receipt->proof_flags & NXCOMPAT_GRAPHICS_PROOF_DRAWABLE_POSITIVE) != 0u,
      NXCOMPAT_EVIDENCE_OPENED, receipt->source,
      NXCOMPAT_REASON_GRAPHICS_DRAWABLE_OPENED, receipt->generation);
  nxcompat_registry_stage_boolean(
      &staged, NXCOMPAT_CAPABILITY_GRAPHICS_ETC1,
      gl_real && nxcompat_extension_present(
                     receipt->gl_extensions,
                     "GL_OES_compressed_ETC1_RGB8_texture"),
      NXCOMPAT_EVIDENCE_OPENED, receipt->source,
      NXCOMPAT_REASON_CAPABILITY_PUBLISHED, receipt->generation);
  nxcompat_registry_stage_boolean(
      &staged, NXCOMPAT_CAPABILITY_GRAPHICS_ETC2,
      gles3 ||
          (gl_real &&
           (nxcompat_extension_present(
                receipt->gl_extensions,
                "GL_OES_compressed_ETC2_RGB8_texture") ||
            nxcompat_extension_present(receipt->gl_extensions,
                                       "GL_ARB_ES3_compatibility"))),
      NXCOMPAT_EVIDENCE_OPENED, receipt->source,
      NXCOMPAT_REASON_CAPABILITY_PUBLISHED, receipt->generation);
  nxcompat_registry_stage_boolean(
      &staged, NXCOMPAT_CAPABILITY_GRAPHICS_ASTC,
      gl_real &&
          (nxcompat_extension_present(
               receipt->gl_extensions,
               "GL_KHR_texture_compression_astc_ldr") ||
           nxcompat_extension_present(
               receipt->gl_extensions,
               "GL_KHR_texture_compression_astc_hdr") ||
           nxcompat_extension_present(receipt->gl_extensions,
                                      "GL_OES_texture_compression_astc")),
      NXCOMPAT_EVIDENCE_OPENED, receipt->source,
      NXCOMPAT_REASON_CAPABILITY_PUBLISHED, receipt->generation);
  nxcompat_registry_stage_boolean(
      &staged, NXCOMPAT_CAPABILITY_GRAPHICS_NPOT_FULL,
      gles3 ||
          (gl_real && nxcompat_extension_present(receipt->gl_extensions,
                                                 "GL_OES_texture_npot")),
      NXCOMPAT_EVIDENCE_OPENED, receipt->source,
      NXCOMPAT_REASON_CAPABILITY_PUBLISHED, receipt->generation);
  staged.generation++;
  *registry = staged;
  nxcompat_receipt_store_reason(reason, NXCOMPAT_REASON_CAPABILITY_PUBLISHED);
  return NXCOMPAT_OK;
}

nxcompat_result_code nxcompat_registry_publish_graphics(
    nxcompat_registry *registry, const nxcompat_graphics_receipt *receipt) {
  return nxcompat_registry_publish_graphics_ex(registry, receipt, NULL);
}

static int
nxcompat_audio_receipt_valid(const nxcompat_audio_receipt *receipt) {
  uint32_t flags;
  if (!receipt || receipt->api_version != NXCOMPAT_API_VERSION ||
      receipt->struct_size < sizeof(*receipt) || receipt->generation == 0u ||
      (receipt->source != NXCOMPAT_SOURCE_SDL2_AUDIO &&
       receipt->source != NXCOMPAT_SOURCE_ENGINE_ADAPTER) ||
      (receipt->proof_flags & ~(uint32_t)NXCOMPAT_AUDIO_PROOF_ALL) != 0u ||
      receipt->lifetime < NXCOMPAT_AUDIO_OPENED_THEN_CLOSED ||
      receipt->lifetime > NXCOMPAT_AUDIO_ACTIVE_ENGINE_OWNED ||
      (receipt->device_id_was_nonzero != 0 &&
       receipt->device_id_was_nonzero != 1) ||
      !nxcompat_registry_bounded_string(receipt->backend,
                                        sizeof(receipt->backend)))
    return 0;
  flags = receipt->proof_flags;
  if ((flags & NXCOMPAT_AUDIO_PROOF_DEVICE_OPENED) != 0u &&
      (flags & NXCOMPAT_AUDIO_PROOF_BACKEND_INITIALIZED) == 0u)
    return 0;
  if ((flags & NXCOMPAT_AUDIO_PROOF_SPEC_OBTAINED) != 0u &&
      (flags & NXCOMPAT_AUDIO_PROOF_DEVICE_OPENED) == 0u)
    return 0;
  if ((flags & NXCOMPAT_AUDIO_PROOF_BACKEND_INITIALIZED) != 0u) {
    if (!nxcompat_backend_name_valid(receipt->backend,
                                     sizeof(receipt->backend)))
      return 0;
  } else if (receipt->backend[0]) {
    return 0;
  }
  if ((flags & NXCOMPAT_AUDIO_PROOF_DEVICE_OPENED) != 0u) {
    if (!receipt->device_id_was_nonzero)
      return 0;
  } else if (receipt->device_id_was_nonzero) {
    return 0;
  }
  if ((flags & NXCOMPAT_AUDIO_PROOF_SPEC_OBTAINED) != 0u) {
    if (receipt->frequency < 4000 || receipt->frequency > 384000 ||
        receipt->format == 0u || receipt->channels == 0u ||
        receipt->channels > 32u || receipt->samples == 0u ||
        receipt->samples > 65535u)
      return 0;
  } else if (receipt->frequency != 0 || receipt->format != 0u ||
             receipt->channels != 0u || receipt->samples != 0u) {
    return 0;
  }
  if (receipt->lifetime == NXCOMPAT_AUDIO_ACTIVE_ENGINE_OWNED &&
      (flags & NXCOMPAT_AUDIO_PROOF_ALL) != NXCOMPAT_AUDIO_PROOF_ALL)
    return 0;
  return 1;
}

nxcompat_result_code nxcompat_registry_publish_audio_ex(
    nxcompat_registry *registry, const nxcompat_audio_receipt *receipt,
    nxcompat_reason_code *reason) {
  nxcompat_registry staged;
  int opened;
  nxcompat_receipt_store_reason(reason, NXCOMPAT_REASON_INVALID_ARGUMENT);
  if (!nxcompat_registry_instance_valid(registry) || !receipt)
    return NXCOMPAT_INVALID;
  if (receipt->api_version != NXCOMPAT_API_VERSION) {
    nxcompat_receipt_store_reason(reason, NXCOMPAT_REASON_UNSUPPORTED_API);
    return NXCOMPAT_INVALID;
  }
  if (receipt->struct_size < sizeof(*receipt)) {
    nxcompat_receipt_store_reason(reason, NXCOMPAT_REASON_STRUCT_TOO_SMALL);
    return NXCOMPAT_INVALID;
  }
  if (!nxcompat_audio_receipt_valid(receipt)) {
    nxcompat_reason_code invalid_reason = NXCOMPAT_REASON_PROVIDER_CONTRACT;
    if (nxcompat_backend_name_fake(receipt->backend,
                                   sizeof(receipt->backend)))
      invalid_reason = NXCOMPAT_REASON_BACKEND_FAKE_OUTPUT;
    else if ((receipt->proof_flags & NXCOMPAT_AUDIO_PROOF_DEVICE_OPENED) != 0u &&
             !receipt->device_id_was_nonzero)
      invalid_reason = NXCOMPAT_REASON_AUDIO_DEVICE_INVALID;
    nxcompat_receipt_store_reason(reason, invalid_reason);
    return NXCOMPAT_INVALID;
  }
  if (registry->has_audio &&
      receipt->generation <= registry->audio.generation) {
    nxcompat_receipt_store_reason(reason, NXCOMPAT_REASON_CAPABILITY_STALE);
    return NXCOMPAT_FAILED;
  }
  if (registry->generation == UINT64_MAX) {
    nxcompat_receipt_store_reason(reason,
                                  NXCOMPAT_REASON_OBSERVATION_OUT_OF_RANGE);
    return NXCOMPAT_FAILED;
  }
  staged = *registry;
  staged.has_audio = 1;
  staged.audio = *receipt;
  opened = (receipt->proof_flags & NXCOMPAT_AUDIO_PROOF_ALL) ==
           NXCOMPAT_AUDIO_PROOF_ALL;
  nxcompat_registry_stage_boolean(
      &staged, NXCOMPAT_CAPABILITY_AUDIO_OUTPUT_OPEN, opened,
      opened && receipt->lifetime == NXCOMPAT_AUDIO_ACTIVE_ENGINE_OWNED
          ? NXCOMPAT_EVIDENCE_ACTIVE
          : NXCOMPAT_EVIDENCE_OPENED,
      receipt->source, NXCOMPAT_REASON_AUDIO_OUTPUT_OPENED,
      receipt->generation);
  staged.generation++;
  *registry = staged;
  nxcompat_receipt_store_reason(reason, NXCOMPAT_REASON_CAPABILITY_PUBLISHED);
  return NXCOMPAT_OK;
}

nxcompat_result_code nxcompat_registry_publish_audio(
    nxcompat_registry *registry, const nxcompat_audio_receipt *receipt) {
  return nxcompat_registry_publish_audio_ex(registry, receipt, NULL);
}

static int
nxcompat_input_receipt_valid(const nxcompat_input_receipt *receipt) {
  uint32_t flags;
  if (!receipt || receipt->api_version != NXCOMPAT_API_VERSION ||
      receipt->struct_size < sizeof(*receipt) ||
      receipt->topology_generation == 0u ||
      (receipt->source != NXCOMPAT_SOURCE_NXINPUT &&
       receipt->source != NXCOMPAT_SOURCE_ENGINE_ADAPTER) ||
      (receipt->proof_flags & ~(uint32_t)NXCOMPAT_INPUT_PROOF_ALL) != 0u ||
      receipt->connected_count > 4u ||
      receipt->mapping_source < NXCOMPAT_INPUT_MAPPING_NONE ||
      receipt->mapping_source > NXCOMPAT_INPUT_MAPPING_ENGINE ||
      receipt->last_change < NXCOMPAT_INPUT_CHANGE_NONE ||
      receipt->last_change > NXCOMPAT_INPUT_CHANGE_RESCAN)
    return 0;
  flags = receipt->proof_flags;
  if ((flags &
       ~(uint32_t)NXCOMPAT_INPUT_PROOF_CONTROLLER_SUBSYSTEM_ACTIVE) != 0u &&
      (flags & NXCOMPAT_INPUT_PROOF_CONTROLLER_SUBSYSTEM_ACTIVE) == 0u)
    return 0;
  if ((flags & (NXCOMPAT_INPUT_PROOF_MAPPING_AVAILABLE |
                NXCOMPAT_INPUT_PROOF_EVENT_WATCH_ACTIVE |
                NXCOMPAT_INPUT_PROOF_RESCAN_ACTIVE |
                NXCOMPAT_INPUT_PROOF_CONTROLLER_OPENED)) != 0u &&
      (flags & NXCOMPAT_INPUT_PROOF_INITIAL_SCAN_DONE) == 0u)
    return 0;
  if ((flags & NXCOMPAT_INPUT_PROOF_MAPPING_AVAILABLE) != 0u) {
    if (receipt->mapping_source == NXCOMPAT_INPUT_MAPPING_NONE)
      return 0;
  } else if (receipt->mapping_source != NXCOMPAT_INPUT_MAPPING_NONE) {
    return 0;
  }
  if ((flags & NXCOMPAT_INPUT_PROOF_CONTROLLER_OPENED) != 0u) {
    if (receipt->connected_count == 0u)
      return 0;
  } else if (receipt->connected_count != 0u) {
    return 0;
  }
  if (receipt->hotplug_event_count != 0u &&
      (flags & NXCOMPAT_INPUT_PROOF_EVENT_WATCH_ACTIVE) == 0u)
    return 0;
  if (receipt->rescan_count != 0u &&
      (flags & NXCOMPAT_INPUT_PROOF_RESCAN_ACTIVE) == 0u)
    return 0;
  if ((receipt->last_change == NXCOMPAT_INPUT_CHANGE_ADD ||
       receipt->last_change == NXCOMPAT_INPUT_CHANGE_REMOVE ||
       receipt->last_change == NXCOMPAT_INPUT_CHANGE_REMAP) &&
      (flags & NXCOMPAT_INPUT_PROOF_EVENT_WATCH_ACTIVE) == 0u)
    return 0;
  if (receipt->last_change == NXCOMPAT_INPUT_CHANGE_RESCAN &&
      (flags & NXCOMPAT_INPUT_PROOF_RESCAN_ACTIVE) == 0u)
    return 0;
  return 1;
}

nxcompat_result_code nxcompat_registry_publish_input_ex(
    nxcompat_registry *registry, const nxcompat_input_receipt *receipt,
    nxcompat_reason_code *reason) {
  nxcompat_registry staged;
  int api_active;
  int connected;
  int hotplug;
  int mapping;
  nxcompat_receipt_store_reason(reason, NXCOMPAT_REASON_INVALID_ARGUMENT);
  if (!nxcompat_registry_instance_valid(registry) || !receipt)
    return NXCOMPAT_INVALID;
  if (receipt->api_version != NXCOMPAT_API_VERSION) {
    nxcompat_receipt_store_reason(reason, NXCOMPAT_REASON_UNSUPPORTED_API);
    return NXCOMPAT_INVALID;
  }
  if (receipt->struct_size < sizeof(*receipt)) {
    nxcompat_receipt_store_reason(reason, NXCOMPAT_REASON_STRUCT_TOO_SMALL);
    return NXCOMPAT_INVALID;
  }
  if (!nxcompat_input_receipt_valid(receipt)) {
    nxcompat_receipt_store_reason(reason, NXCOMPAT_REASON_PROVIDER_CONTRACT);
    return NXCOMPAT_INVALID;
  }
  if (registry->has_input &&
      receipt->topology_generation <= registry->input.topology_generation) {
    nxcompat_receipt_store_reason(reason, NXCOMPAT_REASON_CAPABILITY_STALE);
    return NXCOMPAT_FAILED;
  }
  if (registry->generation == UINT64_MAX) {
    nxcompat_receipt_store_reason(reason,
                                  NXCOMPAT_REASON_OBSERVATION_OUT_OF_RANGE);
    return NXCOMPAT_FAILED;
  }
  staged = *registry;
  staged.has_input = 1;
  staged.input = *receipt;
  api_active =
      (receipt->proof_flags &
       (NXCOMPAT_INPUT_PROOF_CONTROLLER_SUBSYSTEM_ACTIVE |
        NXCOMPAT_INPUT_PROOF_INITIAL_SCAN_DONE)) ==
      (NXCOMPAT_INPUT_PROOF_CONTROLLER_SUBSYSTEM_ACTIVE |
       NXCOMPAT_INPUT_PROOF_INITIAL_SCAN_DONE);
  mapping = (receipt->proof_flags &
             NXCOMPAT_INPUT_PROOF_MAPPING_AVAILABLE) != 0u;
  connected = (receipt->proof_flags &
               NXCOMPAT_INPUT_PROOF_CONTROLLER_OPENED) != 0u;
  hotplug =
      (receipt->proof_flags &
       (NXCOMPAT_INPUT_PROOF_EVENT_WATCH_ACTIVE |
        NXCOMPAT_INPUT_PROOF_RESCAN_ACTIVE)) ==
      (NXCOMPAT_INPUT_PROOF_EVENT_WATCH_ACTIVE |
       NXCOMPAT_INPUT_PROOF_RESCAN_ACTIVE);
  /* A runtime receipt can strengthen a probe mapping fact, but absence in a
   * controller snapshot does not invalidate a PortMaster/database observation
   * published by the independent probe source. */
  if (mapping) {
    nxcompat_registry_stage_evidence(
        &staged, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING,
        NXCOMPAT_EVIDENCE_OBSERVED, receipt->source,
        NXCOMPAT_REASON_CAPABILITY_PUBLISHED, receipt->topology_generation);
  } else if (staged.probe_controller_mapping) {
    nxcompat_registry_stage_evidence(
        &staged, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING,
        NXCOMPAT_EVIDENCE_OBSERVED, NXCOMPAT_SOURCE_PROBE,
        NXCOMPAT_REASON_PROBE_COMPLETE,
        staged.probe_controller_mapping_generation);
  } else if (staged.evidence[NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING]
                 .source != NXCOMPAT_SOURCE_PROBE) {
    nxcompat_registry_stage_boolean(
        &staged, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_MAPPING, 0,
        NXCOMPAT_EVIDENCE_OBSERVED, receipt->source,
        NXCOMPAT_REASON_CAPABILITY_PUBLISHED, receipt->topology_generation);
  }
  nxcompat_registry_stage_boolean(
      &staged, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_API, api_active,
      NXCOMPAT_EVIDENCE_ACTIVE, receipt->source,
      NXCOMPAT_REASON_INPUT_CONTROLLER_API_ACTIVE,
      receipt->topology_generation);
  nxcompat_registry_stage_boolean(
      &staged, NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_CONNECTED, connected,
      NXCOMPAT_EVIDENCE_ACTIVE, receipt->source,
      NXCOMPAT_REASON_INPUT_CONTROLLER_CONNECTED,
      receipt->topology_generation);
  if (!connected &&
      staged.evidence[NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_CONNECTED].state ==
          NXCOMPAT_EVIDENCE_LOST)
    staged.evidence[NXCOMPAT_CAPABILITY_INPUT_CONTROLLER_CONNECTED].reason =
        NXCOMPAT_REASON_INPUT_CONTROLLER_LOST;
  nxcompat_registry_stage_boolean(
      &staged, NXCOMPAT_CAPABILITY_INPUT_HOTPLUG, hotplug,
      NXCOMPAT_EVIDENCE_ACTIVE, receipt->source,
      NXCOMPAT_REASON_INPUT_HOTPLUG_ACTIVE, receipt->topology_generation);
  staged.generation++;
  *registry = staged;
  nxcompat_receipt_store_reason(reason, NXCOMPAT_REASON_CAPABILITY_PUBLISHED);
  return NXCOMPAT_OK;
}

nxcompat_result_code nxcompat_registry_publish_input(
    nxcompat_registry *registry, const nxcompat_input_receipt *receipt) {
  return nxcompat_registry_publish_input_ex(registry, receipt, NULL);
}
