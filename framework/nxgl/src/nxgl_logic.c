/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxgl_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void nxgl_copy_string(char *destination, size_t destination_size,
                      const char *source) {
  if (!destination || destination_size == 0)
    return;
  if (!source)
    source = "";
  (void)snprintf(destination, destination_size, "%s", source);
}

void nxgl_emit(nxgl_status_callback callback, void *userdata,
               nxgl_status_kind kind, const char *format, ...) {
  char message[NXGL_DETAIL_MAX];
  va_list arguments;
  va_start(arguments, format);
  (void)vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  if (callback) {
    callback(userdata, kind, message);
  } else {
    static const char *const labels[] = {"info", "try", "selected",
                                         "warning", "error"};
    unsigned index = (unsigned)kind;
    if (index >= sizeof(labels) / sizeof(labels[0]))
      index = 0;
    (void)fprintf(stderr, "[nxgl:%s] %s\n", labels[index], message);
  }
}

void nxgl_open_options_init(nxgl_open_options *options) {
  if (!options)
    return;
  memset(options, 0, sizeof(*options));
  options->api_version = NXGL_API_VERSION;
  options->struct_size = sizeof(*options);
  options->flags = NXGL_OPEN_INITIALIZE_VIDEO |
                   NXGL_OPEN_RETRY_GLES_HINT_AFTER_DESKTOP |
                   NXGL_OPEN_RETRY_AUTODETECT_AFTER_REAL_FAILURE;
  options->window_title = "nxgl";
  options->window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN |
                          SDL_WINDOW_FULLSCREEN_DESKTOP;
  options->display_index = 0;
  options->drawable_wait_ms = 300;
}

void nxgl_resolution_sources_init(nxgl_resolution_sources *sources) {
  if (!sources)
    return;
  memset(sources, 0, sizeof(*sources));
  sources->api_version = NXGL_API_VERSION;
  sources->struct_size = sizeof(*sources);
}

void nxgl_engine_requirements_init(nxgl_engine_requirements *requirements) {
  if (!requirements)
    return;
  memset(requirements, 0, sizeof(*requirements));
  requirements->api_version = NXGL_API_VERSION;
  requirements->struct_size = sizeof(*requirements);
  requirements->minimum_gles_major = 2;
  requirements->minimum_red_bits = 8;
  requirements->minimum_green_bits = 8;
  requirements->minimum_blue_bits = 8;
  requirements->require_double_buffer = 0;
}

static int nxgl_dimensions_valid(int width, int height) {
  return width > 0 && height > 0 && width <= 32768 && height <= 32768;
}

int nxgl_choose_resolution(const nxgl_resolution_sources *sources,
                           nxgl_resolution *resolution) {
  if (!sources || sources->api_version != NXGL_API_VERSION ||
      sources->struct_size < sizeof(*sources) || !resolution)
    return NXGL_ERROR_INVALID_ARGUMENT;
  memset(resolution, 0, sizeof(*resolution));
  resolution->api_version = NXGL_API_VERSION;
  resolution->struct_size = sizeof(*resolution);
#define NXGL_PICK_RESOLUTION(prefix, source_value)                           \
  do {                                                                       \
    if (nxgl_dimensions_valid(sources->prefix##_width,                       \
                              sources->prefix##_height)) {                   \
      resolution->source = source_value;                                     \
      resolution->width = sources->prefix##_width;                           \
      resolution->height = sources->prefix##_height;                         \
      return NXGL_SUCCESS;                                                    \
    }                                                                        \
  } while (0)
  NXGL_PICK_RESOLUTION(sdl_desktop, NXGL_RESOLUTION_SDL_DESKTOP);
  NXGL_PICK_RESOLUTION(sdl_current, NXGL_RESOLUTION_SDL_CURRENT);
  NXGL_PICK_RESOLUTION(sdl_bounds, NXGL_RESOLUTION_SDL_BOUNDS);
  NXGL_PICK_RESOLUTION(drm, NXGL_RESOLUTION_DRM_FACT);
  NXGL_PICK_RESOLUTION(fbdev, NXGL_RESOLUTION_FBDEV_FACT);
#undef NXGL_PICK_RESOLUTION
  return NXGL_ERROR_RESOLUTION_UNAVAILABLE;
}

const char *nxgl_resolution_source_name(nxgl_resolution_source source) {
  switch (source) {
  case NXGL_RESOLUTION_SDL_DESKTOP:
    return "sdl-desktop";
  case NXGL_RESOLUTION_SDL_CURRENT:
    return "sdl-current";
  case NXGL_RESOLUTION_SDL_BOUNDS:
    return "sdl-bounds";
  case NXGL_RESOLUTION_DRM_FACT:
    return "drm-fact";
  case NXGL_RESOLUTION_FBDEV_FACT:
    return "fbdev-fact";
  default:
    return "none";
  }
}

int nxgl_parse_gles_version(const char *version, int *major, int *minor) {
  const char *cursor;
  char *end = NULL;
  long parsed_major;
  long parsed_minor;
  if (!version || !major || !minor)
    return NXGL_ERROR_INVALID_ARGUMENT;
  cursor = strstr(version, "OpenGL ES");
  if (!cursor)
    return NXGL_ERROR_NO_GLES_CONFIG;
  cursor += strlen("OpenGL ES");
  while (*cursor && !isdigit((unsigned char)*cursor))
    ++cursor;
  if (!*cursor)
    return NXGL_ERROR_NO_GLES_CONFIG;
  parsed_major = strtol(cursor, &end, 10);
  if (end == cursor || *end != '.')
    return NXGL_ERROR_NO_GLES_CONFIG;
  cursor = end + 1;
  parsed_minor = strtol(cursor, &end, 10);
  if (end == cursor || parsed_major < 1 || parsed_major > 9 ||
      parsed_minor < 0 || parsed_minor > 99)
    return NXGL_ERROR_NO_GLES_CONFIG;
  *major = (int)parsed_major;
  *minor = (int)parsed_minor;
  return NXGL_SUCCESS;
}

static int nxgl_version_less(int left_major, int left_minor, int right_major,
                             int right_minor) {
  return left_major < right_major ||
         (left_major == right_major && left_minor < right_minor);
}

static int nxgl_version_greater(int left_major, int left_minor,
                                int right_major, int right_minor) {
  return left_major > right_major ||
         (left_major == right_major && left_minor > right_minor);
}

static int nxgl_bits_valid(int value) { return value >= 0 && value <= 64; }

static int nxgl_requirements_valid(
    const nxgl_engine_requirements *requirements) {
  if (!requirements || requirements->api_version != NXGL_API_VERSION ||
      requirements->struct_size < sizeof(*requirements) ||
      requirements->minimum_gles_major < 2 ||
      requirements->minimum_gles_major > 9 ||
      requirements->minimum_gles_minor < 0 ||
      requirements->minimum_gles_minor > 99 ||
      requirements->maximum_gles_major < 0 ||
      requirements->maximum_gles_major > 9 ||
      requirements->maximum_gles_minor < 0 ||
      requirements->maximum_gles_minor > 99 ||
      (requirements->maximum_gles_major == 0 &&
       requirements->maximum_gles_minor != 0) ||
      !nxgl_bits_valid(requirements->minimum_red_bits) ||
      !nxgl_bits_valid(requirements->minimum_green_bits) ||
      !nxgl_bits_valid(requirements->minimum_blue_bits) ||
      !nxgl_bits_valid(requirements->minimum_alpha_bits) ||
      !nxgl_bits_valid(requirements->minimum_depth_bits) ||
      !nxgl_bits_valid(requirements->minimum_stencil_bits) ||
      requirements->require_double_buffer < 0 ||
      requirements->require_double_buffer > 1)
    return 0;
  if (requirements->maximum_gles_major > 0 &&
      nxgl_version_less(requirements->maximum_gles_major,
                        requirements->maximum_gles_minor,
                        requirements->minimum_gles_major,
                        requirements->minimum_gles_minor))
    return 0;
  return 1;
}

int nxgl_candidate_compatible(const nxgl_engine_requirements *requirements,
                              const nxgl_config_candidate *candidate) {
  if (!nxgl_requirements_valid(requirements) || !candidate ||
      candidate->gles_major < 2 || candidate->gles_major > 9 ||
      candidate->gles_minor < 0 || candidate->gles_minor > 99 ||
      candidate->double_buffer < 0 ||
      candidate->double_buffer > 1 ||
      !nxgl_bits_valid(candidate->red_bits) ||
      !nxgl_bits_valid(candidate->green_bits) ||
      !nxgl_bits_valid(candidate->blue_bits) ||
      !nxgl_bits_valid(candidate->alpha_bits) ||
      !nxgl_bits_valid(candidate->depth_bits) ||
      !nxgl_bits_valid(candidate->stencil_bits))
    return 0;
  if (nxgl_version_less(candidate->gles_major, candidate->gles_minor,
                        requirements->minimum_gles_major,
                        requirements->minimum_gles_minor))
    return 0;
  if (requirements->maximum_gles_major > 0 &&
      nxgl_version_greater(candidate->gles_major, candidate->gles_minor,
                           requirements->maximum_gles_major,
                           requirements->maximum_gles_minor))
    return 0;
  if (candidate->red_bits < requirements->minimum_red_bits ||
      candidate->green_bits < requirements->minimum_green_bits ||
      candidate->blue_bits < requirements->minimum_blue_bits ||
      candidate->alpha_bits < requirements->minimum_alpha_bits ||
      candidate->depth_bits < requirements->minimum_depth_bits ||
      candidate->stencil_bits < requirements->minimum_stencil_bits ||
      (requirements->require_double_buffer && !candidate->double_buffer))
    return 0;
  return 1;
}

int nxgl_validate_actual(const nxgl_engine_requirements *requirements,
                         const nxgl_config_actual *actual, char *error,
                         size_t error_size) {
  if (!requirements || !actual || !error || error_size == 0)
    return NXGL_ERROR_INVALID_ARGUMENT;
  error[0] = '\0';
  if (nxgl_version_less(actual->gles_major, actual->gles_minor,
                        requirements->minimum_gles_major,
                        requirements->minimum_gles_minor)) {
    (void)snprintf(error, error_size, "GLES %d.%d is below engine minimum %d.%d",
                   actual->gles_major, actual->gles_minor,
                   requirements->minimum_gles_major,
                   requirements->minimum_gles_minor);
    return NXGL_ERROR_NO_GLES_CONFIG;
  }
  if (requirements->maximum_gles_major > 0 &&
      nxgl_version_greater(actual->gles_major, actual->gles_minor,
                           requirements->maximum_gles_major,
                           requirements->maximum_gles_minor)) {
    (void)snprintf(error, error_size, "GLES %d.%d is above engine maximum %d.%d",
                   actual->gles_major, actual->gles_minor,
                   requirements->maximum_gles_major,
                   requirements->maximum_gles_minor);
    return NXGL_ERROR_NO_GLES_CONFIG;
  }
#define NXGL_REQUIRE_BITS(field, label)                                      \
  do {                                                                       \
    if (actual->field < requirements->minimum_##field) {                     \
      (void)snprintf(error, error_size, label " %d is below engine minimum %d", \
                     actual->field, requirements->minimum_##field);          \
      return NXGL_ERROR_NO_GLES_CONFIG;                                      \
    }                                                                        \
  } while (0)
  NXGL_REQUIRE_BITS(red_bits, "red bits");
  NXGL_REQUIRE_BITS(green_bits, "green bits");
  NXGL_REQUIRE_BITS(blue_bits, "blue bits");
  NXGL_REQUIRE_BITS(alpha_bits, "alpha bits");
  NXGL_REQUIRE_BITS(depth_bits, "depth bits");
  NXGL_REQUIRE_BITS(stencil_bits, "stencil bits");
#undef NXGL_REQUIRE_BITS
  if (requirements->require_double_buffer && actual->double_buffer != 1) {
    nxgl_copy_string(error, error_size,
                     "single-buffer config does not meet engine requirement");
    return NXGL_ERROR_NO_GLES_CONFIG;
  }
  return NXGL_SUCCESS;
}

int nxgl_format_profile_line(const nxgl_report *report, char *output,
                             size_t output_size) {
  int count;
  if (!report || report->api_version != NXGL_API_VERSION ||
      report->struct_size < sizeof(*report) || !output || output_size == 0)
    return NXGL_ERROR_INVALID_ARGUMENT;
  count = snprintf(output, output_size,
                   "video %.31s | GLES %d.%d | drawable %dx%d | "
                   "rgba %d/%d/%d/%d d%d s%d db%d | %.63s",
                   report->video_backend[0] ? report->video_backend : "unknown",
                   report->actual.gles_major, report->actual.gles_minor,
                   report->drawable_width, report->drawable_height,
                   report->actual.red_bits, report->actual.green_bits,
                   report->actual.blue_bits, report->actual.alpha_bits,
                   report->actual.depth_bits, report->actual.stencil_bits,
                   report->actual.double_buffer,
                   report->renderer[0] ? report->renderer : "unknown");
  if (count < 0 || (size_t)count >= output_size) {
    output[output_size - 1] = '\0';
    return NXGL_ERROR_INVALID_ARGUMENT;
  }
  return count;
}
