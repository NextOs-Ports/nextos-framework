/*
 * egl_config_contract.c -- framebuffer contract expected by Huntdown's
 * Unity 2022.3.47f1 EGL backend.
 *
 * This is the Huntdown-scoped adaptation of the RGBA8888 contract physically
 * validated by Horizon Chase 1.0.3 on ROCKNIX/Wayland/Panfrost.
 */

#include "egl_config_contract.h"

#include <string.h>

size_t hd_egl_build_window_config_attributes(
    int es_major, int depth, int stencil, int *attributes, size_t capacity) {
  const int renderable =
      es_major >= 3 ? HD_EGL_OPENGL_ES3_BIT_KHR : HD_EGL_OPENGL_ES2_BIT;
  const int values[HD_EGL_WINDOW_CONFIG_ATTR_CAPACITY] = {
      HD_EGL_RED_SIZE, HD_EGL_RGBA_CHANNEL_BITS,
      HD_EGL_GREEN_SIZE, HD_EGL_RGBA_CHANNEL_BITS,
      HD_EGL_BLUE_SIZE, HD_EGL_RGBA_CHANNEL_BITS,
      HD_EGL_ALPHA_SIZE, HD_EGL_RGBA_CHANNEL_BITS,
      HD_EGL_DEPTH_SIZE, depth,
      HD_EGL_STENCIL_SIZE, stencil,
      HD_EGL_SURFACE_TYPE, HD_EGL_WINDOW_BIT | HD_EGL_PBUFFER_BIT,
      HD_EGL_RENDERABLE_TYPE, renderable,
      HD_EGL_NONE
  };

  if (!attributes || capacity < HD_EGL_WINDOW_CONFIG_ATTR_CAPACITY)
    return 0;
  memcpy(attributes, values, sizeof values);
  return HD_EGL_WINDOW_CONFIG_ATTR_CAPACITY;
}

int hd_egl_attribute_value(
    const int *attributes, int attribute, int fallback) {
  if (!attributes)
    return fallback;
  for (size_t index = 0; index + 1 < 128; index += 2) {
    if (attributes[index] == HD_EGL_NONE)
      break;
    if (attributes[index] == attribute)
      return attributes[index + 1];
  }
  return fallback;
}

int hd_egl_config_meets_unity(
    const hd_egl_config_properties *candidate,
    const hd_egl_config_properties *required) {
  if (!candidate || !required)
    return 0;

  return candidate->native_visual_type != 0x108 &&
         candidate->red == required->red &&
         candidate->green == required->green &&
         candidate->blue == required->blue &&
         candidate->alpha == required->alpha &&
         candidate->depth >= required->depth &&
         candidate->stencil >= required->stencil &&
         candidate->samples >= required->samples &&
         (candidate->renderable & required->renderable) ==
             required->renderable &&
         (candidate->surfaces & required->surfaces) == required->surfaces;
}
