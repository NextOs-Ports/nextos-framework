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

  /* 0.2.14: classificar pelo BASENAME, nunca pelo caminho inteiro. O nome
   * chega como caminho real (realpath): um blob legitimo guardado num
   * diretorio chamado .../stubs/ era vetado, e um objeto qualquer num
   * diretorio .../drm/ era promovido a "direto". So' o nome do arquivo diz
   * algo sobre a variante do provedor. */
  if (provider_name != NULL) {
    const char *slash = provider_name;
    const char *cursor;
    for (cursor = provider_name; *cursor; ++cursor)
      if (*cursor == '/')
        slash = cursor + 1;
    provider_name = slash;
  }

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

nxgl_sdl_provider_pair_plan nxgl_plan_sdl_provider_pair(
    const char *video_backend, const char *renderer,
    const char *provider_name, int window_opened, int context_current,
    int drawable_positive, int exports_egl, int exports_engine_gles) {
  if (!nxgl_diagnostic_boolean_valid(window_opened) ||
      !nxgl_diagnostic_boolean_valid(context_current) ||
      !nxgl_diagnostic_boolean_valid(drawable_positive) ||
      !nxgl_diagnostic_boolean_valid(exports_egl) ||
      !nxgl_diagnostic_boolean_valid(exports_engine_gles))
    return NXGL_SDL_PROVIDER_PAIR_INVALID;

  /* Authorize recovery only after the inherited stack created a real output
   * while exposing no renderer. This excludes ordinary startup failures and
   * every healthy Mesa/vendor stack. */
  if (!window_opened || !context_current || !drawable_positive ||
      (renderer && renderer[0] != '\0'))
    return NXGL_SDL_PROVIDER_PAIR_NO_ACTION;

  if (!exports_egl || !exports_engine_gles ||
      !nxgl_provider_name_compatible(video_backend, provider_name))
    return NXGL_SDL_PROVIDER_PAIR_NO_ACTION;

  return NXGL_SDL_PROVIDER_PAIR_BIND_COHERENT;
}

void nxgl_sdl_precontext_recovery_v2_init(
    nxgl_sdl_precontext_recovery_v2 *recovery) {
  if (!recovery)
    return;
  memset(recovery, 0, sizeof(*recovery));
  recovery->api_version = NXGL_API_VERSION_V2;
  recovery->struct_size = sizeof(*recovery);
}

nxgl_sdl_provider_pair_plan nxgl_plan_sdl_precontext_recovery_v2(
    const nxgl_sdl_precontext_recovery_v2 *recovery) {
  if (!recovery || recovery->api_version != NXGL_API_VERSION_V2 ||
      recovery->struct_size < sizeof(*recovery) ||
      !nxgl_diagnostic_boolean_valid(recovery->attempts_exhausted) ||
      !nxgl_diagnostic_boolean_valid(recovery->inherited_provider_hint) ||
      !nxgl_diagnostic_boolean_valid(recovery->same_object) ||
      !nxgl_diagnostic_boolean_valid(recovery->exports_egl) ||
      !nxgl_diagnostic_boolean_valid(recovery->exports_engine_gles))
    return NXGL_SDL_PROVIDER_PAIR_INVALID;

  if (!recovery->attempts_exhausted || recovery->inherited_provider_hint ||
      !((recovery->failed_stage == NXGL_OPEN_STAGE_V2_WINDOW_CREATE &&
         recovery->final_reason == NXGL_OPEN_REASON_V2_WINDOW_FAILED) ||
        (recovery->failed_stage == NXGL_OPEN_STAGE_V2_CONTEXT_CREATE &&
         recovery->final_reason == NXGL_OPEN_REASON_V2_CONTEXT_FAILED)))
    return NXGL_SDL_PROVIDER_PAIR_NO_ACTION;

  if (!recovery->same_object || !recovery->exports_egl ||
      !recovery->exports_engine_gles ||
      !nxgl_provider_name_compatible(recovery->video_backend,
                                     recovery->provider_name))
    return NXGL_SDL_PROVIDER_PAIR_NO_ACTION;

  return NXGL_SDL_PROVIDER_PAIR_BIND_COHERENT;
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

int nxgl_classify_frame_proof_v2(
    const nxgl_frame_proof_observation_v2 *observation,
    nxgl_frame_proof_verdict_v2 *verdict) {
  if (!observation || !verdict ||
      observation->api_version != NXGL_API_VERSION_V2 ||
      observation->struct_size < sizeof(*observation) ||
      observation->samples < 0 || observation->best_non_black_percent < 0.0 ||
      observation->best_non_black_percent > 100.0 ||
      observation->minimum_non_black_percent < 0.0 ||
      observation->minimum_non_black_percent > 100.0)
    return NXGL_ERROR_INVALID_ARGUMENT;

  if (observation->samples == 0) {
    /* No evidence either way. Never report this as a pass: the whole point of
     * the verdict is that silence stops counting as success. */
    *verdict = NXGL_FRAME_PROOF_V2_UNKNOWN;
    return NXGL_SUCCESS;
  }

  double minimum = observation->minimum_non_black_percent > 0.0
                       ? observation->minimum_non_black_percent
                       : NXGL_FRAME_PROOF_DEFAULT_MIN_NON_BLACK;
  *verdict = observation->best_non_black_percent < minimum
                 ? NXGL_FRAME_PROOF_V2_BLACK
                 : NXGL_FRAME_PROOF_V2_OK;
  return NXGL_SUCCESS;
}

int nxgl_classify_client_array_bridge_v2(
    const nxgl_client_array_observation_v2 *observation,
    int *bridge_required) {
  if (!observation || !bridge_required ||
      observation->api_version != NXGL_API_VERSION_V2 ||
      observation->struct_size < sizeof(*observation))
    return NXGL_ERROR_INVALID_ARGUMENT;

  const char *driver = observation->video_driver;
  const char *renderer = observation->renderer;
  const char *version = observation->version;

  /* Every field is required: a partial tuple is not evidence, and guessing
   * here means enabling a CPU-side mirror on a driver that never needed it. */
  if (!driver || !renderer || !version) {
    *bridge_required = 0;
    return NXGL_SUCCESS;
  }

  *bridge_required = strcmp(driver, "wayland") == 0 &&
                     strstr(renderer, "Mali-G52") != NULL &&
                     strstr(version, "g24p0") != NULL;
  return NXGL_SUCCESS;
}

int nxgl_classify_launch_context_v2(
    const nxgl_launch_observation_v2 *observation,
    nxgl_launch_context_v2 *context) {
  if (!observation || !context ||
      observation->api_version != NXGL_API_VERSION_V2 ||
      observation->struct_size < sizeof(*observation) ||
      !nxgl_diagnostic_boolean_valid(observation->frontend_launched) ||
      !nxgl_diagnostic_boolean_valid(observation->remote_session) ||
      !nxgl_diagnostic_boolean_valid(observation->seat_vt))
    return NXGL_ERROR_INVALID_ARGUMENT;

  /* The frontend owns the display and hands it over, so it wins even when the
   * operator also happens to be logged in over the network. */
  if (observation->frontend_launched)
    *context = NXGL_LAUNCH_CONTEXT_V2_FRONTEND;
  else if (observation->remote_session)
    *context = NXGL_LAUNCH_CONTEXT_V2_REMOTE;
  else if (observation->seat_vt)
    *context = NXGL_LAUNCH_CONTEXT_V2_CONSOLE;
  else
    *context = NXGL_LAUNCH_CONTEXT_V2_UNKNOWN;
  return NXGL_SUCCESS;
}

int nxgl_frame_proof_is_conclusive_v2(nxgl_launch_context_v2 context,
                                      int *conclusive) {
  if (!conclusive)
    return NXGL_ERROR_INVALID_ARGUMENT;
  switch (context) {
  case NXGL_LAUNCH_CONTEXT_V2_FRONTEND:
  case NXGL_LAUNCH_CONTEXT_V2_CONSOLE:
    *conclusive = 1;
    return NXGL_SUCCESS;
  case NXGL_LAUNCH_CONTEXT_V2_REMOTE:
  case NXGL_LAUNCH_CONTEXT_V2_UNKNOWN:
    /* Fail open for the port, closed for the report: the run simply does not
     * settle the question and must be repeated from the frontend. */
    *conclusive = 0;
    return NXGL_SUCCESS;
  default:
    return NXGL_ERROR_INVALID_ARGUMENT;
  }
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
