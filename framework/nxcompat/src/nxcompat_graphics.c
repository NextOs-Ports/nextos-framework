/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxcompat_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nxcompat_extension_present(const char *extensions,
                                      const char *wanted) {
  size_t wanted_length;
  const char *cursor;
  if (!extensions || !wanted || !*wanted)
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
  const char *cursor;
  char *end;
  long parsed_major;
  long parsed_minor;
  if (!version || !strstr(version, "OpenGL ES"))
    return -1;
  cursor = version;
  while (*cursor && !isdigit((unsigned char)*cursor))
    ++cursor;
  if (!*cursor)
    return -1;
  parsed_major = strtol(cursor, &end, 10);
  if (end == cursor || *end != '.')
    return -1;
  cursor = end + 1;
  parsed_minor = strtol(cursor, &end, 10);
  if (end == cursor || parsed_major < 1 || parsed_major > 9 ||
      parsed_minor < 0 || parsed_minor > 99)
    return -1;
  *major = (int)parsed_major;
  *minor = (int)parsed_minor;
  return 0;
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
  if (wanted_length == 0 || wanted_length > text_length)
    return 0;
  for (start = 0; start + wanted_length <= text_length; ++start) {
    for (index = 0; index < wanted_length; ++index)
      if (tolower((unsigned char)text[start + index]) !=
          tolower((unsigned char)wanted[index]))
        break;
    if (index == wanted_length)
      return 1;
  }
  return 0;
}

static nxcompat_gpu_family nxcompat_classify_gpu(const char *vendor,
                                                 const char *renderer) {
  if (nxcompat_contains_case_insensitive(renderer, "panfrost"))
    return NXCOMPAT_GPU_PANFROST;
  if (nxcompat_contains_case_insensitive(renderer, "mali-4") ||
      nxcompat_contains_case_insensitive(renderer, "mali 4"))
    return NXCOMPAT_GPU_MALI_4XX;
  if (nxcompat_contains_case_insensitive(renderer, "mali") ||
      nxcompat_contains_case_insensitive(vendor, "arm"))
    return NXCOMPAT_GPU_MALI_MODERN;
  if ((renderer && *renderer) || (vendor && *vendor))
    return NXCOMPAT_GPU_OTHER;
  return NXCOMPAT_GPU_UNKNOWN;
}

const char *nxcompat_gpu_family_name(nxcompat_gpu_family value) {
  switch (value) {
  case NXCOMPAT_GPU_MALI_4XX:
    return "mali-4xx";
  case NXCOMPAT_GPU_MALI_MODERN:
    return "mali-modern";
  case NXCOMPAT_GPU_PANFROST:
    return "panfrost";
  case NXCOMPAT_GPU_OTHER:
    return "other";
  default:
    return "unknown";
  }
}

int nxcompat_capture_graphics(const nxcompat_graphics_options *options,
                              nxcompat_graphics *graphics) {
  if (!options || !nxcompat_api_version_supported(options->api_version) ||
      options->struct_size < sizeof(*options) || !graphics)
    return -1;
  memset(graphics, 0, sizeof(*graphics));
  graphics->api_version = options->api_version;
  graphics->struct_size = sizeof(*graphics);
  graphics->gpu_family = nxcompat_classify_gpu(options->vendor,
                                               options->renderer);
  graphics->drawable_width = options->drawable_width;
  graphics->drawable_height = options->drawable_height;
  graphics->red_bits = options->red_bits;
  graphics->green_bits = options->green_bits;
  graphics->blue_bits = options->blue_bits;
  graphics->alpha_bits = options->alpha_bits;
  graphics->depth_bits = options->depth_bits;
  graphics->stencil_bits = options->stencil_bits;
  nxcompat_copy_string(graphics->video_driver,
                       sizeof(graphics->video_driver), options->video_driver);
  nxcompat_copy_string(graphics->vendor, sizeof(graphics->vendor),
                       options->vendor);
  nxcompat_copy_string(graphics->renderer, sizeof(graphics->renderer),
                       options->renderer);
  nxcompat_copy_string(graphics->version, sizeof(graphics->version),
                       options->version);
  nxcompat_copy_string(graphics->shading_language_version,
                       sizeof(graphics->shading_language_version),
                       options->shading_language_version);
  if (nxcompat_parse_gles_version(options->version, &graphics->gles_major,
                                  &graphics->gles_minor) == 0) {
    graphics->capabilities |= NXCOMPAT_GRAPHICS_GLES;
    if (graphics->gles_major >= 2)
      graphics->capabilities |= NXCOMPAT_GRAPHICS_GLES2;
    if (graphics->gles_major >= 3)
      graphics->capabilities |= NXCOMPAT_GRAPHICS_GLES3 |
                                NXCOMPAT_GRAPHICS_ETC2 |
                                NXCOMPAT_GRAPHICS_NPOT_FULL;
  }
  if (nxcompat_extension_present(
          options->extensions, "GL_OES_compressed_ETC1_RGB8_texture"))
    graphics->capabilities |= NXCOMPAT_GRAPHICS_ETC1;
  if (nxcompat_extension_present(options->extensions,
                                 "GL_OES_compressed_ETC2_RGB8_texture") ||
      nxcompat_extension_present(options->extensions,
                                 "GL_ARB_ES3_compatibility"))
    graphics->capabilities |= NXCOMPAT_GRAPHICS_ETC2;
  if (nxcompat_extension_present(
          options->extensions, "GL_KHR_texture_compression_astc_ldr") ||
      nxcompat_extension_present(
          options->extensions, "GL_OES_texture_compression_astc"))
    graphics->capabilities |= NXCOMPAT_GRAPHICS_ASTC;
  if (nxcompat_extension_present(options->extensions,
                                 "GL_OES_texture_npot"))
    graphics->capabilities |= NXCOMPAT_GRAPHICS_NPOT_FULL;
  return 0;
}

int nxcompat_format_graphics_line(const nxcompat_graphics *graphics,
                                  char *output, size_t output_size) {
  int count;
  if (!graphics || !nxcompat_api_version_supported(graphics->api_version) ||
      graphics->struct_size < sizeof(*graphics) || !output ||
      output_size == 0)
    return -1;
  count = snprintf(
      output, output_size,
      "video %s | renderer %s | %s %d.%d | drawable %dx%d | rgba %d/%d/%d/%d d%d s%d",
      graphics->video_driver[0] ? graphics->video_driver : "unknown",
      graphics->renderer[0] ? graphics->renderer : "unknown",
      (graphics->capabilities & NXCOMPAT_GRAPHICS_GLES) ? "GLES" : "GL",
      graphics->gles_major, graphics->gles_minor, graphics->drawable_width,
      graphics->drawable_height, graphics->red_bits, graphics->green_bits,
      graphics->blue_bits, graphics->alpha_bits, graphics->depth_bits,
      graphics->stencil_bits);
  if (count < 0 || (size_t)count >= output_size) {
    output[output_size - 1] = '\0';
    return -1;
  }
  return count;
}
