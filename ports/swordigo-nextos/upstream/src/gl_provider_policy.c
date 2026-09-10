/* SPDX-License-Identifier: GPL-3.0-only */
#include "gl_provider_policy.h"

static int policy_boolean_valid(int value) {
  return value == 0 || value == 1;
}

static int ascii_lower(int value) {
  if (value >= 'A' && value <= 'Z')
    return value + ('a' - 'A');
  return value;
}

static int ascii_contains(const char *text, const char *needle) {
  const char *cursor;
  const char *left;
  const char *right;

  if (!text || !needle || !needle[0])
    return 0;
  for (cursor = text; *cursor; ++cursor) {
    left = cursor;
    right = needle;
    while (*left && *right &&
           ascii_lower((unsigned char)*left) ==
               ascii_lower((unsigned char)*right)) {
      ++left;
      ++right;
    }
    if (!*right)
      return 1;
  }
  return 0;
}

int gl_provider_name_compatible(const char *video_backend,
                                const char *provider_name) {
  int has_direct;
  int has_wayland;
  int has_x11;

  if (!provider_name || !provider_name[0] ||
      ascii_contains(provider_name, "dummy") ||
      ascii_contains(provider_name, "stub") ||
      ascii_contains(provider_name, "headless") ||
      ascii_contains(provider_name, "surfaceless"))
    return 0;

  has_direct = ascii_contains(provider_name, "gbm") ||
               ascii_contains(provider_name, "drm") ||
               ascii_contains(provider_name, "fbdev");
  has_wayland = ascii_contains(provider_name, "wayland");
  has_x11 = ascii_contains(provider_name, "x11");

  if (ascii_contains(video_backend, "wayland"))
    return !(has_x11 && !has_wayland);
  if (ascii_contains(video_backend, "x11"))
    return !(has_wayland && !has_x11);
  if (ascii_contains(video_backend, "kmsdrm") ||
      ascii_contains(video_backend, "drm") ||
      ascii_contains(video_backend, "fbdev") ||
      ascii_contains(video_backend, "directfb"))
    return (!(has_wayland || has_x11) || has_direct);

  /* SDL may not have published its backend yet on a pre-window failure.
   * Explicit GBM/DRM remains useful evidence; a compositor-only name does not. */
  return (!(has_wayland || has_x11) || has_direct);
}

gl_provider_sdl_pair_plan gl_provider_plan_sdl_pair(
    const char *video_backend, const char *renderer,
    const char *provider_name, int window_opened, int context_current,
    int drawable_positive, int exports_egl, int exports_engine_gles) {
  if (!policy_boolean_valid(window_opened) ||
      !policy_boolean_valid(context_current) ||
      !policy_boolean_valid(drawable_positive) ||
      !policy_boolean_valid(exports_egl) ||
      !policy_boolean_valid(exports_engine_gles))
    return GL_PROVIDER_SDL_PAIR_INVALID;
  if (!window_opened || !context_current || !drawable_positive ||
      (renderer && renderer[0] != '\0'))
    return GL_PROVIDER_SDL_PAIR_NO_ACTION;
  if (!exports_egl || !exports_engine_gles ||
      !gl_provider_name_compatible(video_backend, provider_name))
    return GL_PROVIDER_SDL_PAIR_NO_ACTION;
  return GL_PROVIDER_SDL_PAIR_BIND_COHERENT;
}
