/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxgl.h"

#include <stdint.h>
#include <string.h>

static int nxgl_diagnostic_boolean_valid(int value) {
  return value == 0 || value == 1;
}

static int nxgl_ascii_lower(int value) {
  if (value >= 'A' && value <= 'Z')
    return value + ('a' - 'A');
  return value;
}

static int nxgl_ascii_contains(const char *text, const char *needle) {
  const char *cursor;
  const char *left;
  const char *right;

  if (!text || !needle || !needle[0])
    return 0;
  for (cursor = text; *cursor; ++cursor) {
    left = cursor;
    right = needle;
    while (*left && *right &&
           nxgl_ascii_lower((unsigned char)*left) ==
               nxgl_ascii_lower((unsigned char)*right)) {
      ++left;
      ++right;
    }
    if (!*right)
      return 1;
  }
  return 0;
}

int nxgl_provider_name_compatible(const char *video_backend,
                                  const char *provider_name) {
  int has_direct;
  int has_wayland;
  int has_x11;

  if (!provider_name || !provider_name[0] ||
      nxgl_ascii_contains(provider_name, "dummy") ||
      nxgl_ascii_contains(provider_name, "stub") ||
      nxgl_ascii_contains(provider_name, "headless") ||
      nxgl_ascii_contains(provider_name, "surfaceless"))
    return 0;

  has_direct = nxgl_ascii_contains(provider_name, "gbm") ||
               nxgl_ascii_contains(provider_name, "drm") ||
               nxgl_ascii_contains(provider_name, "fbdev");
  has_wayland = nxgl_ascii_contains(provider_name, "wayland");
  has_x11 = nxgl_ascii_contains(provider_name, "x11");

  if (nxgl_ascii_contains(video_backend, "wayland"))
    return !(has_x11 && !has_wayland);
  if (nxgl_ascii_contains(video_backend, "x11"))
    return !(has_wayland && !has_x11);

  if (nxgl_ascii_contains(video_backend, "kmsdrm") ||
      nxgl_ascii_contains(video_backend, "drm") ||
      nxgl_ascii_contains(video_backend, "fbdev") ||
      nxgl_ascii_contains(video_backend, "directfb"))
    return (!(has_wayland || has_x11) || has_direct);

  /* Before SDL publishes a backend, retain generic/direct candidates but do
   * not gamble on an object that advertises only a compositor transport. */
  return (!(has_wayland || has_x11) || has_direct);
}

static int nxgl_observation_dimensions_valid(
    const nxgl_surface_observation_v2 *observation) {
  return observation->window_width > 0 && observation->window_height > 0 &&
         observation->drawable_width > 0 &&
         observation->drawable_height > 0 &&
         observation->window_width <= NXGL_SURFACE_DIMENSION_MAX &&
         observation->window_height <= NXGL_SURFACE_DIMENSION_MAX &&
         observation->drawable_width <= NXGL_SURFACE_DIMENSION_MAX &&
         observation->drawable_height <= NXGL_SURFACE_DIMENSION_MAX;
}

static int nxgl_observation_dimensions_well_formed(
    const nxgl_surface_observation_v2 *observation) {
  return (observation->window_width == 0 &&
          observation->window_height == 0 &&
          observation->drawable_width == 0 &&
          observation->drawable_height == 0) ||
         nxgl_observation_dimensions_valid(observation);
}

static int nxgl_state_dimensions_valid(const nxgl_surface_state_v2 *state) {
  const int all_zero = state->window_width == 0 && state->window_height == 0 &&
                       state->drawable_width == 0 &&
                       state->drawable_height == 0;
  return all_zero ||
         (state->window_width > 0 &&
          state->window_width <= NXGL_SURFACE_DIMENSION_MAX &&
          state->window_height > 0 &&
          state->window_height <= NXGL_SURFACE_DIMENSION_MAX &&
          state->drawable_width > 0 &&
          state->drawable_width <= NXGL_SURFACE_DIMENSION_MAX &&
          state->drawable_height > 0 &&
          state->drawable_height <= NXGL_SURFACE_DIMENSION_MAX);
}

void nxgl_surface_state_v2_init(nxgl_surface_state_v2 *state) {
  if (!state)
    return;
  memset(state, 0, sizeof(*state));
  state->api_version = NXGL_API_VERSION_V2;
  state->struct_size = sizeof(*state);
  /* This helper describes an already-created context.  Loss is observational;
   * no transition in this module creates or destroys a native context. */
  state->context_generation = 1u;
  state->focused = 1;
}

int nxgl_surface_observe_v2(
    nxgl_surface_state_v2 *state,
    const nxgl_surface_observation_v2 *observation) {
  nxgl_surface_state_v2 next;

  if (!state || !observation ||
      state->api_version != NXGL_API_VERSION_V2 ||
      state->struct_size < sizeof(*state) ||
      observation->api_version != NXGL_API_VERSION_V2 ||
      observation->struct_size < sizeof(*observation) ||
      state->generation == UINT64_MAX || state->context_generation == 0u ||
      state->context_generation - 1u > state->generation ||
      !nxgl_diagnostic_boolean_valid(state->focused) ||
      !nxgl_diagnostic_boolean_valid(state->minimized) ||
      !nxgl_diagnostic_boolean_valid(state->context_lost) ||
      (state->focused && state->minimized) ||
      !nxgl_state_dimensions_valid(state) ||
      !nxgl_observation_dimensions_well_formed(observation))
    return NXGL_ERROR_INVALID_ARGUMENT;

  next = *state;
  switch (observation->event) {
  case NXGL_SURFACE_EVENT_V2_FOCUS_GAINED:
    next.focused = 1;
    next.minimized = 0;
    break;
  case NXGL_SURFACE_EVENT_V2_FOCUS_LOST:
    next.focused = 0;
    break;
  case NXGL_SURFACE_EVENT_V2_MINIMIZED:
    next.minimized = 1;
    next.focused = 0;
    break;
  case NXGL_SURFACE_EVENT_V2_RESTORED:
    next.minimized = 0;
    break;
  case NXGL_SURFACE_EVENT_V2_RESIZED:
    if (!nxgl_observation_dimensions_valid(observation))
      return NXGL_ERROR_INVALID_ARGUMENT;
    next.window_width = observation->window_width;
    next.window_height = observation->window_height;
    next.drawable_width = observation->drawable_width;
    next.drawable_height = observation->drawable_height;
    break;
  case NXGL_SURFACE_EVENT_V2_CONTEXT_LOST:
    if (next.context_lost)
      return NXGL_ERROR_INVALID_ARGUMENT;
    next.context_lost = 1;
    break;
  case NXGL_SURFACE_EVENT_V2_CONTEXT_RECREATED:
    if (!next.context_lost || next.context_generation == UINT64_MAX ||
        !nxgl_observation_dimensions_valid(observation))
      return NXGL_ERROR_INVALID_ARGUMENT;
    next.context_lost = 0;
    ++next.context_generation;
    next.window_width = observation->window_width;
    next.window_height = observation->window_height;
    next.drawable_width = observation->drawable_width;
    next.drawable_height = observation->drawable_height;
    break;
  default:
    return NXGL_ERROR_INVALID_ARGUMENT;
  }
  ++next.generation;
  *state = next;
  return NXGL_SUCCESS;
}

int nxgl_classify_black_silhouette_v2(
    const nxgl_silhouette_observation_v2 *observation,
    nxgl_silhouette_diagnosis_v2 *diagnosis) {
  if (!observation || !diagnosis ||
      observation->api_version != NXGL_API_VERSION_V2 ||
      observation->struct_size < sizeof(*observation) ||
      !nxgl_diagnostic_boolean_valid(observation->pixels_are_black) ||
      !nxgl_diagnostic_boolean_valid(observation->silhouette_is_intact) ||
      !nxgl_diagnostic_boolean_valid(observation->uses_texture_atlas) ||
      !nxgl_diagnostic_boolean_valid(observation->repeating_or_mirrored_uv) ||
      !nxgl_diagnostic_boolean_valid(observation->forced_clamp_to_edge) ||
      !nxgl_diagnostic_boolean_valid(observation->sampler_override_active))
    return NXGL_ERROR_INVALID_ARGUMENT;

  if (!observation->pixels_are_black) {
    *diagnosis = NXGL_SILHOUETTE_V2_NOT_APPLICABLE;
  } else if (observation->silhouette_is_intact) {
    /* A geometrically correct black object is classified here even when the
     * caller has not yet collected sampler evidence.  This deliberately puts
     * atlas UVs, wrap mode, and sampler overrides ahead of shader or lighting
     * speculation; the classifier is pure and cannot change GL state. */
    *diagnosis = NXGL_SILHOUETTE_V2_AUDIT_SAMPLER_WRAP_ATLAS;
  } else {
    *diagnosis = NXGL_SILHOUETTE_V2_AUDIT_RENDER_PIPELINE;
  }
  return NXGL_SUCCESS;
}
