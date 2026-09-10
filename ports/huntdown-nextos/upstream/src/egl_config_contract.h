/*
 * egl_config_contract.h -- EGLConfig requirements shared by Huntdown's
 * SDL/EGL bridge and its host-side regression test.
 */

#ifndef HD_EGL_CONFIG_CONTRACT_H
#define HD_EGL_CONFIG_CONTRACT_H

#include <stddef.h>

enum {
  HD_EGL_BUFFER_SIZE = 0x3020,
  HD_EGL_ALPHA_SIZE = 0x3021,
  HD_EGL_BLUE_SIZE = 0x3022,
  HD_EGL_GREEN_SIZE = 0x3023,
  HD_EGL_RED_SIZE = 0x3024,
  HD_EGL_DEPTH_SIZE = 0x3025,
  HD_EGL_STENCIL_SIZE = 0x3026,
  HD_EGL_NATIVE_VISUAL_TYPE = 0x302f,
  HD_EGL_SAMPLES = 0x3031,
  HD_EGL_SAMPLE_BUFFERS = 0x3032,
  HD_EGL_SURFACE_TYPE = 0x3033,
  HD_EGL_NONE = 0x3038,
  HD_EGL_COLOR_BUFFER_TYPE = 0x303f,
  HD_EGL_RENDERABLE_TYPE = 0x3040,
  HD_EGL_CONFORMANT = 0x3042,
  HD_EGL_COVERAGE_SAMPLES_NV = 0x30e1,
  HD_EGL_DEPTH_ENCODING_NV = 0x30e2,
  HD_EGL_RGB_BUFFER = 0x308e,
  HD_EGL_PBUFFER_BIT = 0x0001,
  HD_EGL_WINDOW_BIT = 0x0004,
  HD_EGL_OPENGL_ES2_BIT = 0x0004,
  HD_EGL_OPENGL_ES3_BIT_KHR = 0x0040,
  HD_EGL_RGBA_CHANNEL_BITS = 8,
  HD_EGL_WINDOW_CONFIG_ATTR_CAPACITY = 17
};

typedef struct {
  int red;
  int green;
  int blue;
  int alpha;
  int depth;
  int stencil;
  int samples;
  int renderable;
  int surfaces;
  int native_visual_type;
} hd_egl_config_properties;

size_t hd_egl_build_window_config_attributes(
    int es_major, int depth, int stencil, int *attributes, size_t capacity);

int hd_egl_attribute_value(
    const int *attributes, int attribute, int fallback);

int hd_egl_config_meets_unity(
    const hd_egl_config_properties *candidate,
    const hd_egl_config_properties *required);

#endif
