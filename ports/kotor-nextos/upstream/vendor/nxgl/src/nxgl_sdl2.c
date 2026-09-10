/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "nxgl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(NXGL_M13_TESTING)
/* The sealed M13 object redirects only SDL's in-memory hint API to its fake
 * fixture.  No SDL video/context/provider symbol is linked or called. */
const char *nxgl_test_sdl_get_hint(const char *name);
SDL_bool nxgl_test_sdl_set_hint(const char *name, const char *value);
void nxgl_test_sdl_reset_hint(const char *name);
const char *nxgl_test_sdl_current_video_driver(void);
int nxgl_test_sdl_strcasecmp(const char *left, const char *right);
#define SDL_GetHint nxgl_test_sdl_get_hint
#define SDL_SetHint nxgl_test_sdl_set_hint
#define SDL_ResetHint nxgl_test_sdl_reset_hint
#define SDL_GetCurrentVideoDriver nxgl_test_sdl_current_video_driver
#define SDL_strcasecmp nxgl_test_sdl_strcasecmp
#endif

#define NXGL_GL_VENDOR 0x1F00u
#define NXGL_GL_RENDERER 0x1F01u
#define NXGL_GL_VERSION 0x1F02u
#define NXGL_GL_EXTENSIONS 0x1F03u
#define NXGL_GL_SHADING_LANGUAGE_VERSION 0x8B8Cu
#define NXGL_EGL_CONFIG_MAX 256u
#define NXGL_EGL_OPENGL_ES2_BIT 0x0004
#define NXGL_EGL_OPENGL_ES3_BIT_KHR 0x0040
#define NXGL_EGL_WINDOW_BIT 0x0004

/* All nxgl-owned SDL, environment and hint transitions are serialized by one
 * non-blocking process-global arbiter.  Cross-component ordering remains the
 * bootstrap/adapter's responsibility; nxgl never reaches into nxcompat's
 * private arbiter. */
static volatile int nxgl_global_arbiter;
#if defined(NXGL_M13_TESTING)
static volatile int nxgl_test_fail_v2_allocation;

void nxgl_test_fail_next_v2_allocation(void) {
  nxgl_test_fail_v2_allocation = 1;
}
#endif

int nxgl_arbiter_try_acquire(void) {
#if defined(__GNUC__) || defined(__clang__)
  return __sync_lock_test_and_set(&nxgl_global_arbiter, 1) == 0;
#else
#error "nxgl requires GCC/Clang atomic builtins for its global arbiter"
#endif
}

void nxgl_arbiter_release(void) {
#if defined(__GNUC__) || defined(__clang__)
  __sync_lock_release(&nxgl_global_arbiter);
#endif
}

#if !defined(NXGL_CORE_TESTING)
static void *nxgl_v2_context_allocate(void) {
#if defined(NXGL_M13_TESTING)
  if (__sync_lock_test_and_set(&nxgl_test_fail_v2_allocation, 0) != 0)
    return NULL;
#endif
  return calloc(1, sizeof(nxgl_context));
}
#endif

#if !defined(NXGL_CORE_TESTING)
static int nxgl_options_valid(const nxgl_open_options *options) {
  size_t index;
  size_t compatible_count = 0;
  const uint32_t known_flags =
      NXGL_OPEN_INITIALIZE_VIDEO |
      NXGL_OPEN_RETRY_GLES_HINT_AFTER_DESKTOP |
      NXGL_OPEN_ALLOW_NONDISPLAY_BACKEND |
      NXGL_OPEN_RETRY_AUTODETECT_AFTER_REAL_FAILURE;
  if (!options || options->api_version != NXGL_API_VERSION ||
      options->struct_size < sizeof(*options) ||
      (options->flags & ~known_flags) != 0 || !options->requirements ||
      !options->candidates || options->candidate_count == 0 ||
      options->candidate_count > NXGL_MAX_CONFIG_CANDIDATES ||
      options->display_index < 0 || options->drawable_wait_ms > 5000 ||
      options->requirements->api_version != NXGL_API_VERSION ||
      options->requirements->struct_size < sizeof(*options->requirements))
    return 0;
  if (options->fallback_facts &&
      (options->fallback_facts->api_version != NXGL_API_VERSION ||
       options->fallback_facts->struct_size <
           sizeof(*options->fallback_facts)))
    return 0;
  if (options->context_ops &&
      (options->context_ops->api_version != NXGL_API_VERSION ||
       options->context_ops->struct_size < sizeof(*options->context_ops)))
    return 0;
  if (options->context_ops &&
      ((options->context_ops->create_context != NULL ||
        options->context_ops->make_current != NULL ||
        options->context_ops->delete_context != NULL) &&
       (!options->context_ops->create_context ||
        !options->context_ops->make_current ||
        !options->context_ops->delete_context)))
    return 0;
  for (index = 0; index < options->candidate_count; ++index)
    if (nxgl_candidate_compatible(options->requirements,
                                  &options->candidates[index]))
      ++compatible_count;
  return compatible_count != 0;
}
#endif

typedef int (*nxgl_video_environment_callback)(void *userdata);

static int nxgl_environment_value_matches(const char *name, int existed,
                                          const char *value) {
  const char *current = getenv(name);
  if (!existed)
    return current == NULL;
  return current && value && strcmp(current, value) == 0;
}

static void nxgl_free_video_environment(nxgl_context *context) {
  free(context->inherited_video_driver);
  free(context->inherited_video_driver_alias);
  context->inherited_video_driver = NULL;
  context->inherited_video_driver_alias = NULL;
}

static int nxgl_snapshot_environment_value(const char *value,
                                           char **snapshot) {
  size_t length;
  char *copy;
  *snapshot = NULL;
  if (!value)
    return 0;
  length = strnlen(value, NXGL_VIDEO_ENV_VALUE_MAX + 1u);
  if (length > NXGL_VIDEO_ENV_VALUE_MAX)
    return 1;
  copy = (char *)malloc(length + 1u);
  if (!copy)
    return -1;
  memcpy(copy, value, length + 1u);
  *snapshot = copy;
  return 0;
}

#if !defined(NXGL_CORE_TESTING)
static int nxgl_snapshot_hint(const char *name, int *existed, char **snapshot) {
  const char *value;
  int status;
  if (!name || !existed || !snapshot)
    return -1;
  *existed = 0;
  *snapshot = NULL;
  value = SDL_GetHint(name);
  if (!value)
    return 0;
  *existed = 1;
  status = nxgl_snapshot_environment_value(value, snapshot);
  return status == 0 ? 0 : -1;
}

static int nxgl_hint_matches(const char *name, int existed,
                             const char *snapshot) {
  const char *current = SDL_GetHint(name);
  if (!existed)
    return current == NULL;
  return current && snapshot && strcmp(current, snapshot) == 0;
}

static int nxgl_restore_hint(const char *name, int existed,
                             const char *snapshot) {
  int failed = 0;
  if (existed) {
    if (!snapshot || SDL_SetHint(name, snapshot) != SDL_TRUE)
      failed = 1;
  } else {
    SDL_ResetHint(name);
  }
  if (!nxgl_hint_matches(name, existed, snapshot))
    failed = 1;
  return failed ? -1 : 0;
}

static void nxgl_free_hint_snapshots(nxgl_context *context) {
  free(context->previous_gles_hint);
  free(context->previous_x11_egl_hint);
  context->previous_gles_hint = NULL;
  context->previous_x11_egl_hint = NULL;
}

static int nxgl_save_hint_snapshots(nxgl_context *context) {
  context->previous_gles_hint_preservable =
      nxgl_snapshot_hint(NXGL_HINT_OPENGL_ES_DRIVER,
                         &context->previous_gles_hint_existed,
                         &context->previous_gles_hint) == 0;
  context->previous_x11_egl_hint_preservable =
      nxgl_snapshot_hint(NXGL_HINT_VIDEO_X11_FORCE_EGL,
                         &context->previous_x11_egl_hint_existed,
                         &context->previous_x11_egl_hint) == 0;
  if (!context->previous_gles_hint_preservable ||
      !context->previous_x11_egl_hint_preservable) {
    nxgl_free_hint_snapshots(context);
    return -1;
  }
  return 0;
}

static int nxgl_restore_changed_hints(nxgl_context *context) {
  int failed = 0;
  if (context->changed_gles_hint) {
    failed |= nxgl_restore_hint(NXGL_HINT_OPENGL_ES_DRIVER,
                                context->previous_gles_hint_existed,
                                context->previous_gles_hint) != 0;
    if (!failed)
      context->changed_gles_hint = 0;
  }
  if (context->changed_x11_egl_hint) {
    int status = nxgl_restore_hint(NXGL_HINT_VIDEO_X11_FORCE_EGL,
                                   context->previous_x11_egl_hint_existed,
                                   context->previous_x11_egl_hint);
    failed |= status != 0;
    if (status == 0)
      context->changed_x11_egl_hint = 0;
  }
  return failed ? -1 : 0;
}
#endif

static int nxgl_save_video_environment(nxgl_context *context) {
  const char *value = getenv("SDL_VIDEODRIVER");
  int status;
  context->video_environment_preservable = 1;
  if (value) {
    context->inherited_video_driver_existed = 1;
    status = nxgl_snapshot_environment_value(
        value, &context->inherited_video_driver);
    if (status != 0)
      context->video_environment_preservable = 0;
  }
  value = getenv("SDL_VIDEO_DRIVER");
  if (value) {
    context->inherited_video_driver_alias_existed = 1;
    status = nxgl_snapshot_environment_value(
        value, &context->inherited_video_driver_alias);
    if (status != 0)
      context->video_environment_preservable = 0;
  }
  if (!context->video_environment_preservable) {
    nxgl_free_video_environment(context);
    return -1;
  }
  return 0;
}

static int nxgl_has_inherited_video_hint(const nxgl_context *context) {
  return context->video_environment_preservable &&
         ((context->inherited_video_driver_existed &&
           context->inherited_video_driver &&
           context->inherited_video_driver[0]) ||
          (context->inherited_video_driver_alias_existed &&
           context->inherited_video_driver_alias &&
           context->inherited_video_driver_alias[0]));
}

static int nxgl_clear_video_environment(nxgl_context *context) {
  int failed = 0;
  context->video_environment_cleared = 1;
  if (unsetenv("SDL_VIDEODRIVER") != 0)
    failed = 1;
  if (unsetenv("SDL_VIDEO_DRIVER") != 0)
    failed = 1;
  return failed ? -1 : 0;
}

static int nxgl_restore_video_environment(nxgl_context *context) {
  int failed = 0;
  if (!context->video_environment_cleared)
    return 0;
  if (context->inherited_video_driver_existed)
    failed |= setenv("SDL_VIDEODRIVER", context->inherited_video_driver, 1) !=
              0;
  else
    failed |= unsetenv("SDL_VIDEODRIVER") != 0;
  if (context->inherited_video_driver_alias_existed)
    failed |= setenv("SDL_VIDEO_DRIVER",
                     context->inherited_video_driver_alias, 1) != 0;
  else
    failed |= unsetenv("SDL_VIDEO_DRIVER") != 0;
  if (!nxgl_environment_value_matches(
          "SDL_VIDEODRIVER", context->inherited_video_driver_existed,
          context->inherited_video_driver) ||
      !nxgl_environment_value_matches(
          "SDL_VIDEO_DRIVER", context->inherited_video_driver_alias_existed,
          context->inherited_video_driver_alias))
    failed = 1;
  if (!failed)
    context->video_environment_cleared = 0;
  return failed ? -1 : 0;
}

#if !defined(NXGL_M13_TESTING)
static int nxgl_with_video_environment_cleared(
    nxgl_context *context, nxgl_video_environment_callback callback,
    void *userdata) {
  int callback_status;
  if (!context || !callback || !nxgl_has_inherited_video_hint(context))
    return NXGL_NO_ACTION;
  if (nxgl_clear_video_environment(context) != 0) {
    (void)nxgl_restore_video_environment(context);
    return NXGL_ERROR_VIDEO_UNAVAILABLE;
  }
  callback_status = callback(userdata);
  if (nxgl_restore_video_environment(context) != 0)
    return NXGL_ERROR_VIDEO_UNAVAILABLE;
  return callback_status;
}
#endif

#if defined(NXGL_CORE_TESTING)
typedef struct nxgl_test_video_environment_callback_state {
  int result;
  int create_alias;
  int *calls;
  int *environment_was_cleared;
} nxgl_test_video_environment_callback_state;

static int nxgl_test_video_environment_callback(void *userdata) {
  nxgl_test_video_environment_callback_state *state =
      (nxgl_test_video_environment_callback_state *)userdata;
  ++*state->calls;
  *state->environment_was_cleared =
      getenv("SDL_VIDEODRIVER") == NULL &&
      getenv("SDL_VIDEO_DRIVER") == NULL;
  if (state->create_alias &&
      setenv("SDL_VIDEO_DRIVER", "callback-created-alias", 1) != 0)
    return NXGL_ERROR_VIDEO_UNAVAILABLE;
  return state->result;
}

int nxgl_test_video_environment_retry(int callback_result,
                                      int create_alias_in_callback,
                                      int *callback_calls,
                                      int *environment_was_cleared) {
  nxgl_context context;
  nxgl_test_video_environment_callback_state state;
  int status;
  if (!callback_calls || !environment_was_cleared)
    return NXGL_ERROR_INVALID_ARGUMENT;
  memset(&context, 0, sizeof(context));
  *callback_calls = 0;
  *environment_was_cleared = 0;
  state.result = callback_result;
  state.create_alias = create_alias_in_callback;
  state.calls = callback_calls;
  state.environment_was_cleared = environment_was_cleared;
  (void)nxgl_save_video_environment(&context);
  status = nxgl_with_video_environment_cleared(
      &context, nxgl_test_video_environment_callback, &state);
  nxgl_free_video_environment(&context);
  return status;
}
#else

#if !defined(NXGL_M13_TESTING)

static int nxgl_backend_is_display(const char *backend) {
  if (!backend || !*backend)
    return 0;
  return SDL_strcasecmp(backend, "dummy") != 0 &&
         SDL_strcasecmp(backend, "offscreen") != 0;
}

static int nxgl_start_video(nxgl_context *context,
                            const nxgl_open_options *options) {
  if (SDL_WasInit(SDL_INIT_VIDEO) != 0)
    return NXGL_SUCCESS;
  if ((options->flags & NXGL_OPEN_INITIALIZE_VIDEO) == 0) {
    nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_ERROR,
              "SDL video was not negotiated before nxgl_open");
    return NXGL_ERROR_VIDEO_UNAVAILABLE;
  }
  SDL_ClearError();
  if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
    nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_WARNING,
              "SDL video init failed: %s", SDL_GetError());
    return NXGL_ERROR_VIDEO_UNAVAILABLE;
  }
  context->initialized_video = 1;
  return NXGL_SUCCESS;
}

static void nxgl_collect_resolution_sources(
    const nxgl_open_options *options, nxgl_resolution_sources *sources) {
  SDL_DisplayMode mode;
  SDL_Rect bounds;
  nxgl_resolution_sources_init(sources);
  if (options->fallback_facts) {
    sources->drm_width = options->fallback_facts->drm_width;
    sources->drm_height = options->fallback_facts->drm_height;
    sources->fbdev_width = options->fallback_facts->fbdev_width;
    sources->fbdev_height = options->fallback_facts->fbdev_height;
  }
  memset(&mode, 0, sizeof(mode));
  if (SDL_GetDesktopDisplayMode(options->display_index, &mode) == 0) {
    sources->sdl_desktop_width = mode.w;
    sources->sdl_desktop_height = mode.h;
  }
  memset(&mode, 0, sizeof(mode));
  if (SDL_GetCurrentDisplayMode(options->display_index, &mode) == 0) {
    sources->sdl_current_width = mode.w;
    sources->sdl_current_height = mode.h;
  }
  memset(&bounds, 0, sizeof(bounds));
  if (SDL_GetDisplayBounds(options->display_index, &bounds) == 0) {
    sources->sdl_bounds_width = bounds.w;
    sources->sdl_bounds_height = bounds.h;
  }
}

static int nxgl_set_config_attributes(const nxgl_config_candidate *candidate) {
#define NXGL_SET_ATTRIBUTE(attribute, value)                                 \
  do {                                                                       \
    if (SDL_GL_SetAttribute(attribute, value) != 0)                           \
      return -1;                                                             \
  } while (0)
  NXGL_SET_ATTRIBUTE(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  NXGL_SET_ATTRIBUTE(SDL_GL_CONTEXT_MAJOR_VERSION, candidate->gles_major);
  NXGL_SET_ATTRIBUTE(SDL_GL_CONTEXT_MINOR_VERSION, candidate->gles_minor);
  NXGL_SET_ATTRIBUTE(SDL_GL_RED_SIZE, candidate->red_bits);
  NXGL_SET_ATTRIBUTE(SDL_GL_GREEN_SIZE, candidate->green_bits);
  NXGL_SET_ATTRIBUTE(SDL_GL_BLUE_SIZE, candidate->blue_bits);
  NXGL_SET_ATTRIBUTE(SDL_GL_ALPHA_SIZE, candidate->alpha_bits);
  NXGL_SET_ATTRIBUTE(SDL_GL_DEPTH_SIZE, candidate->depth_bits);
  NXGL_SET_ATTRIBUTE(SDL_GL_STENCIL_SIZE, candidate->stencil_bits);
  NXGL_SET_ATTRIBUTE(SDL_GL_DOUBLEBUFFER, candidate->double_buffer);
  NXGL_SET_ATTRIBUTE(SDL_GL_MULTISAMPLEBUFFERS, 0);
  NXGL_SET_ATTRIBUTE(SDL_GL_MULTISAMPLESAMPLES, 0);
  NXGL_SET_ATTRIBUTE(SDL_GL_STEREO, 0);
  NXGL_SET_ATTRIBUTE(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
#undef NXGL_SET_ATTRIBUTE
  return 0;
}

static SDL_GLContext nxgl_create_sdl_context(nxgl_context *context,
                                              SDL_Window *window) {
  if (context->context_ops.create_context)
    return context->context_ops.create_context(context->context_ops.userdata,
                                               window);
  return SDL_GL_CreateContext(window);
}

static int nxgl_make_sdl_context_current(nxgl_context *context,
                                         SDL_Window *window,
                                         SDL_GLContext gl_context) {
  if (context->context_ops.make_current)
    return context->context_ops.make_current(context->context_ops.userdata,
                                             window, gl_context);
  return SDL_GL_MakeCurrent(window, gl_context);
}

void nxgl_delete_context(nxgl_context *context) {
  if (!context || !context->gl_context)
    return;
  if (context->context_ops.delete_context)
    context->context_ops.delete_context(context->context_ops.userdata,
                                        context->gl_context);
  else
    SDL_GL_DeleteContext(context->gl_context);
  context->gl_context = NULL;
}

static void nxgl_drop_window_and_context(nxgl_context *context) {
  nxgl_delete_context(context);
  if (context->window) {
    SDL_DestroyWindow(context->window);
    context->window = NULL;
  }
  memset(&context->gl, 0, sizeof(context->gl));
}

int nxgl_resolve_gl_functions(nxgl_context *context) {
  if (!context)
    return NXGL_ERROR_INVALID_ARGUMENT;
#define NXGL_LOAD_GL(field, type, symbol_name)                               \
  do {                                                                       \
    context->gl.field = (type)SDL_GL_GetProcAddress(symbol_name);            \
  } while (0)
  NXGL_LOAD_GL(get_string, nxgl_gl_get_string_fn, "glGetString");
  NXGL_LOAD_GL(get_error, nxgl_gl_get_error_fn, "glGetError");
  NXGL_LOAD_GL(finish, nxgl_gl_finish_fn, "glFinish");
  NXGL_LOAD_GL(is_enabled, nxgl_gl_is_enabled_fn, "glIsEnabled");
  NXGL_LOAD_GL(get_booleanv, nxgl_gl_get_booleanv_fn, "glGetBooleanv");
  NXGL_LOAD_GL(get_floatv, nxgl_gl_get_floatv_fn, "glGetFloatv");
  NXGL_LOAD_GL(get_integerv, nxgl_gl_get_integerv_fn, "glGetIntegerv");
  NXGL_LOAD_GL(disable, nxgl_gl_disable_fn, "glDisable");
  NXGL_LOAD_GL(enable, nxgl_gl_enable_fn, "glEnable");
  NXGL_LOAD_GL(color_mask, nxgl_gl_color_mask_fn, "glColorMask");
  NXGL_LOAD_GL(clear_color, nxgl_gl_clear_color_fn, "glClearColor");
  NXGL_LOAD_GL(clear, nxgl_gl_clear_fn, "glClear");
#undef NXGL_LOAD_GL
  return context->gl.get_string ? NXGL_SUCCESS : NXGL_ERROR_NO_GLES_CONFIG;
}

static int nxgl_get_attribute(SDL_GLattr attribute, int *value) {
  *value = -1;
  return SDL_GL_GetAttribute(attribute, value);
}

static int nxgl_capture_actual_config(nxgl_context *context,
                                      nxgl_config_actual *actual) {
  const char *version;
  memset(actual, 0, sizeof(*actual));
  if (nxgl_resolve_gl_functions(context) != NXGL_SUCCESS)
    return NXGL_ERROR_NO_GLES_CONFIG;
  version = (const char *)context->gl.get_string(NXGL_GL_VERSION);
  if (nxgl_parse_gles_version(version, &actual->gles_major,
                              &actual->gles_minor) != NXGL_SUCCESS)
    return NXGL_ERROR_NO_GLES_CONFIG;
  if (nxgl_get_attribute(SDL_GL_RED_SIZE, &actual->red_bits) != 0 ||
      nxgl_get_attribute(SDL_GL_GREEN_SIZE, &actual->green_bits) != 0 ||
      nxgl_get_attribute(SDL_GL_BLUE_SIZE, &actual->blue_bits) != 0 ||
      nxgl_get_attribute(SDL_GL_ALPHA_SIZE, &actual->alpha_bits) != 0 ||
      nxgl_get_attribute(SDL_GL_DEPTH_SIZE, &actual->depth_bits) != 0 ||
      nxgl_get_attribute(SDL_GL_STENCIL_SIZE, &actual->stencil_bits) != 0)
    return NXGL_ERROR_NO_GLES_CONFIG;
  actual->double_buffer = -1;
  if (nxgl_get_attribute(SDL_GL_DOUBLEBUFFER, &actual->double_buffer) != 0)
    actual->double_buffer = -1;
  actual->profile_mask = 0;
  if (nxgl_get_attribute(SDL_GL_CONTEXT_PROFILE_MASK, &actual->profile_mask) !=
      0)
    actual->profile_mask = 0;
  return NXGL_SUCCESS;
}

static int nxgl_wait_for_drawable(SDL_Window *window, unsigned wait_ms,
                                  int expected_width, int expected_height,
                                  int *width, int *height) {
  unsigned elapsed = 0;
  int last_width = 0;
  int last_height = 0;
  *width = 0;
  *height = 0;
  for (;;) {
    nxgl_pump_events_without_consuming();
    SDL_GL_GetDrawableSize(window, width, height);
    if (*width > 0 && *height > 0 &&
        *width <= NXGL_SURFACE_DIMENSION_MAX &&
        *height <= NXGL_SURFACE_DIMENSION_MAX) {
      last_width = *width;
      last_height = *height;
      if (*width == expected_width && *height == expected_height)
        return NXGL_SUCCESS;
    }
    if (elapsed >= wait_ms) {
      *width = last_width;
      *height = last_height;
      return (last_width > 0 && last_height > 0)
                 ? NXGL_SUCCESS
                 : NXGL_ERROR_NO_GLES_CONFIG;
    }
    SDL_Delay(10);
    elapsed += 10;
  }
}

void nxgl_pump_events_without_consuming(void) {
  /* The application owns the queue. Pump window/compositor state without
   * stealing QUIT, focus, controller or lifecycle events from its main loop. */
  SDL_PumpEvents();
}

static void nxgl_capture_strings(nxgl_context *context) {
  nxgl_report *report = &context->report;
  const unsigned char *value;
  nxgl_copy_string(report->video_backend, sizeof(report->video_backend),
                   SDL_GetCurrentVideoDriver());
  value = context->gl.get_string(NXGL_GL_VENDOR);
  nxgl_copy_string(report->vendor, sizeof(report->vendor),
                   (const char *)value);
  value = context->gl.get_string(NXGL_GL_RENDERER);
  nxgl_copy_string(report->renderer, sizeof(report->renderer),
                   (const char *)value);
  value = context->gl.get_string(NXGL_GL_VERSION);
  nxgl_copy_string(report->version, sizeof(report->version),
                   (const char *)value);
  value = context->gl.get_string(NXGL_GL_SHADING_LANGUAGE_VERSION);
  nxgl_copy_string(report->shading_language_version,
                   sizeof(report->shading_language_version),
                   (const char *)value);
  value = context->gl.get_string(NXGL_GL_EXTENSIONS);
  nxgl_copy_string(report->extensions, sizeof(report->extensions),
                   (const char *)value);
}

static int nxgl_apply_gles_hint_recovery(nxgl_context *context,
                                         const nxgl_open_options *options) {
  const char *backend;
  if ((options->flags & NXGL_OPEN_RETRY_GLES_HINT_AFTER_DESKTOP) == 0 ||
      context->report.used_gles_hint_recovery ||
      !context->previous_gles_hint_preservable ||
      !context->previous_x11_egl_hint_preservable)
    return 0;
  if (SDL_SetHint(NXGL_HINT_OPENGL_ES_DRIVER, "1") != SDL_TRUE)
    return 0;
  context->changed_gles_hint = 1;
  backend = SDL_GetCurrentVideoDriver();
  if (backend && SDL_strcasecmp(backend, "x11") == 0) {
    if (SDL_SetHint(NXGL_HINT_VIDEO_X11_FORCE_EGL, "1") == SDL_TRUE)
      context->changed_x11_egl_hint = 1;
  }
  context->report.used_gles_hint_recovery = 1;
  nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_WARNING,
            "desktop GL rejected; retrying with SDL GLES/EGL hints");
  return 1;
}

static int nxgl_attempt_config(nxgl_context *context,
                               const nxgl_open_options *options,
                               const nxgl_resolution *resolution,
                               const nxgl_config_candidate *candidate,
                               size_t candidate_index) {
  char validation_error[NXGL_DETAIL_MAX];
  Uint32 window_flags = options->window_flags | SDL_WINDOW_OPENGL;
  int window_position =
      (int)SDL_WINDOWPOS_CENTERED_DISPLAY((Uint32)options->display_index);
  int hint_pass;
  for (hint_pass = 0; hint_pass < 2; ++hint_pass) {
    const char *version;
    ++context->report.attempt_count;
    nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_ATTEMPT,
              "GLES %d.%d RGBA%d/%d/%d/%d D%d S%d (attempt %u)",
              candidate->gles_major, candidate->gles_minor,
              candidate->red_bits, candidate->green_bits,
              candidate->blue_bits, candidate->alpha_bits,
              candidate->depth_bits, candidate->stencil_bits,
              context->report.attempt_count);
    SDL_ClearError();
    if (nxgl_set_config_attributes(candidate) != 0) {
      nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_WARNING,
                "SDL rejected GL attributes: %s", SDL_GetError());
      return NXGL_ERROR_NO_GLES_CONFIG;
    }
    context->window = SDL_CreateWindow(
        options->window_title ? options->window_title : "nxgl",
        window_position, window_position, resolution->width,
        resolution->height, window_flags);
    if (!context->window) {
      nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_WARNING,
                "window creation failed: %s", SDL_GetError());
      return NXGL_ERROR_NO_GLES_CONFIG;
    }
    context->gl_context = nxgl_create_sdl_context(context, context->window);
    if (!context->gl_context) {
      nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_WARNING,
                "GLES context creation failed: %s", SDL_GetError());
      nxgl_drop_window_and_context(context);
      return NXGL_ERROR_NO_GLES_CONFIG;
    }
    if (nxgl_make_sdl_context_current(context, context->window,
                                      context->gl_context) != 0) {
      nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_WARNING,
                "GLES make-current failed: %s", SDL_GetError());
      nxgl_drop_window_and_context(context);
      return NXGL_ERROR_NO_GLES_CONFIG;
    }
    if (nxgl_resolve_gl_functions(context) != NXGL_SUCCESS) {
      nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_WARNING,
                "glGetString is unavailable from the SDL GL stack");
      nxgl_drop_window_and_context(context);
      return NXGL_ERROR_NO_GLES_CONFIG;
    }
    version = (const char *)context->gl.get_string(NXGL_GL_VERSION);
    if (nxgl_parse_gles_version(version, &context->report.actual.gles_major,
                                &context->report.actual.gles_minor) !=
        NXGL_SUCCESS) {
      nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_WARNING,
                "desktop/non-GLES context rejected: %s",
                version ? version : "unknown GL version");
      nxgl_drop_window_and_context(context);
      if (hint_pass == 0 && nxgl_apply_gles_hint_recovery(context, options))
        continue;
      return NXGL_ERROR_NO_GLES_CONFIG;
    }
    if (nxgl_capture_actual_config(context, &context->report.actual) !=
        NXGL_SUCCESS) {
      nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_WARNING,
                "could not query the delivered GLES config");
      nxgl_drop_window_and_context(context);
      return NXGL_ERROR_NO_GLES_CONFIG;
    }
    if (nxgl_validate_actual(options->requirements, &context->report.actual,
                             validation_error, sizeof(validation_error)) !=
        NXGL_SUCCESS) {
      nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_WARNING,
                "delivered config rejected: %s", validation_error);
      nxgl_drop_window_and_context(context);
      return NXGL_ERROR_NO_GLES_CONFIG;
    }
    if (nxgl_wait_for_drawable(context->window, options->drawable_wait_ms,
                               resolution->width, resolution->height,
                               &context->report.drawable_width,
                               &context->report.drawable_height) !=
        NXGL_SUCCESS) {
      nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_WARNING,
                "real GL drawable did not become available");
      nxgl_drop_window_and_context(context);
      return NXGL_ERROR_NO_GLES_CONFIG;
    }
    SDL_GetWindowSize(context->window, &context->report.window_width,
                      &context->report.window_height);
    context->report.requested = *candidate;
    context->report.selected_candidate_index = candidate_index;
    nxgl_capture_strings(context);
    return NXGL_SUCCESS;
  }
  return NXGL_ERROR_NO_GLES_CONFIG;
}

static int nxgl_attempt_real_output(nxgl_context *context,
                                    const nxgl_open_options *options) {
  nxgl_resolution_sources sources;
  nxgl_resolution resolution;
  const char *backend = SDL_GetCurrentVideoDriver();
  size_t index;
  if (!backend ||
      (((options->flags & NXGL_OPEN_ALLOW_NONDISPLAY_BACKEND) == 0) &&
       !nxgl_backend_is_display(backend))) {
    nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_WARNING,
              "SDL backend is not a real display output: %s",
              backend ? backend : "none");
    return NXGL_ERROR_VIDEO_UNAVAILABLE;
  }
  nxgl_collect_resolution_sources(options, &sources);
  if (nxgl_choose_resolution(&sources, &resolution) != NXGL_SUCCESS) {
    nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_WARNING,
              "no SDL, DRM, or fbdev resolution fact is available");
    return NXGL_ERROR_RESOLUTION_UNAVAILABLE;
  }
  context->report.resolution = resolution;
  nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_INFO,
            "video %s; window %dx%d from %s", backend, resolution.width,
            resolution.height, nxgl_resolution_source_name(resolution.source));
  for (index = 0; index < options->candidate_count; ++index) {
    if (!nxgl_candidate_compatible(options->requirements,
                                   &options->candidates[index]))
      continue;
    if (nxgl_attempt_config(context, options, &resolution,
                            &options->candidates[index], index) ==
        NXGL_SUCCESS)
      return NXGL_SUCCESS;
  }
  return NXGL_ERROR_NO_GLES_CONFIG;
}

static int nxgl_retry_initialize_video(void *userdata) {
  (void)userdata;
  return SDL_InitSubSystem(SDL_INIT_VIDEO);
}

static int nxgl_retry_with_video_autodetect(
    nxgl_context *context, const nxgl_open_options *options) {
  int status;
  if ((options->flags & NXGL_OPEN_RETRY_AUTODETECT_AFTER_REAL_FAILURE) == 0 ||
      !nxgl_has_inherited_video_hint(context) ||
      context->video_was_initialized_on_entry ||
      (options->flags & NXGL_OPEN_INITIALIZE_VIDEO) == 0)
    return NXGL_NO_ACTION;
  nxgl_drop_window_and_context(context);
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
  context->initialized_video = 0;
  nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_WARNING,
            "real inherited video path failed; retrying SDL autodetection");
  SDL_ClearError();
  status = nxgl_with_video_environment_cleared(
      context, nxgl_retry_initialize_video, NULL);
  if (status != 0) {
    nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_WARNING,
              "SDL video autodetection or environment rollback failed: %s",
              SDL_GetError());
    return NXGL_ERROR_VIDEO_UNAVAILABLE;
  }
  context->initialized_video = 1;
  status = nxgl_attempt_real_output(context, options);
  if (status == NXGL_SUCCESS) {
    context->report.used_video_autodetect_recovery = 1;
    return NXGL_SUCCESS;
  }
  nxgl_drop_window_and_context(context);
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
  context->initialized_video = 0;
  (void)nxgl_restore_video_environment(context);
  return status;
}

static void nxgl_close_legacy(nxgl_context *context);

static int nxgl_open_legacy_locked(const nxgl_open_options *options,
                                   nxgl_context **context_output,
                                   nxgl_report *report) {
  nxgl_context *context;
  int status;
  char profile[NXGL_DETAIL_MAX];
  *context_output = NULL;
  if (report) {
    memset(report, 0, sizeof(*report));
    report->api_version = NXGL_API_VERSION;
    report->struct_size = sizeof(*report);
    report->selected_candidate_index = (size_t)-1;
  }
  context = (nxgl_context *)calloc(1, sizeof(*context));
  if (!context)
    return NXGL_ERROR_OUT_OF_MEMORY;
  context->open_api_version = NXGL_API_VERSION_V1;
  context->status = options->status;
  context->status_userdata = options->status_userdata;
  context->report.api_version = NXGL_API_VERSION;
  context->report.struct_size = sizeof(context->report);
  context->report.selected_candidate_index = (size_t)-1;
  context->video_was_initialized_on_entry =
      SDL_WasInit(SDL_INIT_VIDEO) != 0;
  if (options->context_ops)
    context->context_ops = *options->context_ops;
  else {
    context->context_ops.api_version = NXGL_API_VERSION;
    context->context_ops.struct_size = sizeof(context->context_ops);
  }
  if (nxgl_save_video_environment(context) != 0)
    nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_WARNING,
              "video environment could not be snapshotted exactly; autodetect retry disabled");
  if (nxgl_save_hint_snapshots(context) != 0)
    nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_WARNING,
              "SDL GLES/EGL hints could not be snapshotted exactly; hint recovery disabled");
  status = nxgl_start_video(context, options);
  if (status == NXGL_SUCCESS)
    status = nxgl_attempt_real_output(context, options);
  if (status != NXGL_SUCCESS) {
    int retry_status = nxgl_retry_with_video_autodetect(context, options);
    if (retry_status != NXGL_NO_ACTION)
      status = retry_status;
  }
  if (status != NXGL_SUCCESS) {
    nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_ERROR,
              "no real GLES window/config/drawable path succeeded");
    if (report)
      *report = context->report;
    nxgl_close_legacy(context);
    return status;
  }
  if (nxgl_format_profile_line(&context->report, profile, sizeof(profile)) >=
      0)
    nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_SELECTED,
              "%s", profile);
  nxgl_emit(options->status, options->status_userdata, NXGL_STATUS_INFO,
            "vendor %s | GL %s | GLSL %s",
            context->report.vendor[0] ? context->report.vendor : "unknown",
            context->report.version[0] ? context->report.version : "unknown",
            context->report.shading_language_version[0]
                ? context->report.shading_language_version
                : "unknown");
  if (report)
    *report = context->report;
  *context_output = context;
  return NXGL_SUCCESS;
}

static void nxgl_close_legacy(nxgl_context *context) {
  if (!context)
    return;
  nxgl_drop_window_and_context(context);
  if (context->initialized_video)
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  (void)nxgl_restore_changed_hints(context);
  (void)nxgl_restore_video_environment(context);
  nxgl_free_video_environment(context);
  nxgl_free_hint_snapshots(context);
  free(context);
}

static int nxgl_make_current_legacy_locked(nxgl_context *context) {
  if (!context || context->open_api_version != NXGL_API_VERSION_V1 ||
      !context->window || !context->gl_context)
    return NXGL_ERROR_INVALID_ARGUMENT;
  if (nxgl_make_sdl_context_current(context, context->window,
                                    context->gl_context) != 0)
    return NXGL_ERROR_NO_GLES_CONFIG;
  return NXGL_SUCCESS;
}

#endif /* !NXGL_M13_TESTING */

static int nxgl_close_v2_locked(nxgl_context *context);

int nxgl_open(const nxgl_open_options *options, nxgl_context **context_output,
              nxgl_report *report) {
  int status;
  if (!context_output)
    return NXGL_ERROR_INVALID_ARGUMENT;
  /* Acquisition precedes validation that could publish an error output. */
  if (!nxgl_arbiter_try_acquire())
    return NXGL_ERROR_BUSY;
  if (!nxgl_options_valid(options)) {
    *context_output = NULL;
    nxgl_arbiter_release();
    return NXGL_ERROR_INVALID_ARGUMENT;
  }
#if defined(NXGL_M13_TESTING)
  /* The sealed fixture reaches this shell only to prove cross-API BUSY.  A
   * non-contended legacy provider is intentionally unavailable in that
   * build, preserving the no-SDL/no-GL link boundary. */
  (void)report;
  *context_output = NULL;
  status = NXGL_ERROR_VIDEO_UNAVAILABLE;
#else
  status = nxgl_open_legacy_locked(options, context_output, report);
#endif
  nxgl_arbiter_release();
  return status;
}

void nxgl_close(nxgl_context *context) {
  if (!context)
    return;
  if (!nxgl_arbiter_try_acquire())
    return;
  if (context->open_api_version == NXGL_API_VERSION_V2) {
    (void)nxgl_close_v2_locked(context);
  } else {
#if !defined(NXGL_M13_TESTING)
    nxgl_close_legacy(context);
#endif
  }
  nxgl_arbiter_release();
}

int nxgl_make_current(nxgl_context *context) {
  int status;
  if (!context)
    return NXGL_ERROR_INVALID_ARGUMENT;
  if (!nxgl_arbiter_try_acquire())
    return NXGL_ERROR_BUSY;
  if (context->open_api_version != NXGL_API_VERSION_V1) {
    nxgl_arbiter_release();
    return NXGL_ERROR_INVALID_ARGUMENT;
  }
#if defined(NXGL_M13_TESTING)
  status = NXGL_ERROR_INVALID_ARGUMENT;
#else
  status = nxgl_make_current_legacy_locked(context);
#endif
  nxgl_arbiter_release();
  return status;
}

SDL_Window *nxgl_window(nxgl_context *context) {
  return context && context->open_api_version == NXGL_API_VERSION_V1
             ? context->window
             : NULL;
}

SDL_GLContext nxgl_sdl_context(nxgl_context *context) {
  return context && context->open_api_version == NXGL_API_VERSION_V1
             ? context->gl_context
             : NULL;
}

const nxgl_report *nxgl_get_report(const nxgl_context *context) {
  return context && context->open_api_version == NXGL_API_VERSION_V1
             ? &context->report
             : NULL;
}

/* API-v2 negotiation lives below this boundary.  NXGL_M13_TESTING compiles
 * the exact same transaction with only injected stack callbacks, so the
 * sealed host test has no reachable SDL/EGL/GL provider symbols. */

static int nxgl_v2_dimensions_valid(int width, int height) {
  return width > 0 && height > 0 &&
         width <= NXGL_SURFACE_DIMENSION_MAX &&
         height <= NXGL_SURFACE_DIMENSION_MAX;
}

static int nxgl_v2_string_bounded(const char *value, size_t maximum) {
  return !value || strnlen(value, maximum + 1u) <= maximum;
}

static int nxgl_v2_backend_token_valid(const char *value, size_t maximum) {
  size_t index;
  size_t length;
  if (!value)
    return 0;
  length = strnlen(value, maximum + 1u);
  if (length == 0 || length > maximum)
    return 0;
  for (index = 0; index < length; ++index) {
    unsigned char byte = (unsigned char)value[index];
    if (!((byte >= (unsigned char)'a' && byte <= (unsigned char)'z') ||
          (byte >= (unsigned char)'A' && byte <= (unsigned char)'Z') ||
          (byte >= (unsigned char)'0' && byte <= (unsigned char)'9') ||
          byte == (unsigned char)'_' || byte == (unsigned char)'-' ||
          byte == (unsigned char)'.' || byte == (unsigned char)'+'))
      return 0;
  }
  return 1;
}

static int nxgl_v2_buffer_terminated(const char *value, size_t size) {
  return value && size > 0 && memchr(value, '\0', size) != NULL;
}

static const char *nxgl_v2_reason_detail(nxgl_open_reason_v2 reason) {
  switch (reason) {
  case NXGL_OPEN_REASON_V2_SELECTED:
    return "selected";
  case NXGL_OPEN_REASON_V2_ARBITER_BUSY:
    return "arbiter-busy";
  case NXGL_OPEN_REASON_V2_INVALID_OPTIONS:
    return "invalid-options";
  case NXGL_OPEN_REASON_V2_SNAPSHOT_TOO_LARGE:
    return "snapshot-too-large";
  case NXGL_OPEN_REASON_V2_SNAPSHOT_FAILED:
    return "snapshot-failed";
  case NXGL_OPEN_REASON_V2_VIDEO_UNAVAILABLE:
    return "video-unavailable";
  case NXGL_OPEN_REASON_V2_RESOLUTION_UNAVAILABLE:
    return "resolution-unavailable";
  case NXGL_OPEN_REASON_V2_ATTRIBUTES_REJECTED:
    return "attributes-rejected";
  case NXGL_OPEN_REASON_V2_WINDOW_FAILED:
    return "window-create-failed";
  case NXGL_OPEN_REASON_V2_CONTEXT_FAILED:
    return "context-create-failed";
  case NXGL_OPEN_REASON_V2_CURRENT_FAILED:
    return "make-current-failed";
  case NXGL_OPEN_REASON_V2_PROC_UNAVAILABLE:
    return "stack-proc-unavailable";
  case NXGL_OPEN_REASON_V2_DESKTOP_GL_REJECTED:
    return "desktop-gl-rejected";
  case NXGL_OPEN_REASON_V2_CONFIG_QUERY_FAILED:
    return "config-query-failed";
  case NXGL_OPEN_REASON_V2_CONFIG_REJECTED:
    return "delivered-config-rejected";
  case NXGL_OPEN_REASON_V2_EGL_CONFIG_UNAVAILABLE:
    return "egl-config-unavailable";
  case NXGL_OPEN_REASON_V2_DRAWABLE_INVALID:
    return "drawable-invalid";
  case NXGL_OPEN_REASON_V2_CURRENT_MISMATCH:
    return "current-stack-mismatch";
  case NXGL_OPEN_REASON_V2_CLEANUP_FAILED:
    return "attempt-cleanup-failed";
  case NXGL_OPEN_REASON_V2_RESTORE_FAILED:
    return "transaction-restore-failed";
  case NXGL_OPEN_REASON_V2_PROVIDER_CONTRACT:
    return "provider-contract-invalid";
  case NXGL_OPEN_REASON_V2_OUT_OF_MEMORY:
    return "out-of-memory";
  case NXGL_OPEN_REASON_V2_HINT_APPLY_FAILED:
    return "hint-apply-failed";
  default:
    return "none";
  }
}

static void nxgl_v2_report_init(nxgl_report_v2 *report) {
  memset(report, 0, sizeof(*report));
  report->api_version = NXGL_API_VERSION_V2;
  report->struct_size = sizeof(*report);
  report->legacy.api_version = NXGL_API_VERSION_V1;
  report->legacy.struct_size = sizeof(report->legacy);
  report->legacy.selected_candidate_index = (size_t)-1;
  report->handles.api_version = NXGL_API_VERSION_V2;
  report->handles.struct_size = sizeof(report->handles);
  report->egl.api_version = NXGL_API_VERSION_V2;
  report->egl.struct_size = sizeof(report->egl);
  report->environment_restored = 1;
  report->hints_restored = 1;
}

static void nxgl_v2_record(nxgl_context *context, unsigned round_index,
                           unsigned attempt_index, size_t candidate_index,
                           nxgl_open_stage_v2 stage,
                           nxgl_open_reason_v2 reason, int result) {
  nxgl_report_v2 *report = &context->report_v2;
  if (report->journal_count < NXGL_ATTEMPT_JOURNAL_MAX) {
    nxgl_attempt_entry_v2 *entry =
        &report->journal[report->journal_count++];
    memset(entry, 0, sizeof(*entry));
    entry->api_version = NXGL_API_VERSION_V2;
    entry->struct_size = sizeof(*entry);
    entry->round_index = round_index;
    entry->attempt_index = attempt_index;
    entry->candidate_index = candidate_index;
    entry->owner = context->stack_owner;
    entry->stage = stage;
    entry->reason = reason;
    entry->result = result;
    /* Journal text is selected only from this finite table.  Provider/SDL
     * errors may be emitted separately but cannot inject paths, controls or
     * unbounded text into the durable attempt journal. */
    (void)snprintf(entry->detail, sizeof(entry->detail), "%s",
                   nxgl_v2_reason_detail(reason));
  } else {
    ++report->journal_dropped;
  }
  report->final_stage = stage;
  report->final_reason = reason;
}

static int nxgl_v2_stack_ops_valid(const nxgl_stack_ops_v2 *ops) {
  return ops && ops->api_version == NXGL_API_VERSION_V2 &&
         ops->struct_size >= sizeof(*ops) &&
         (ops->owner == NXGL_STACK_OWNER_V2_SDL_EGL ||
          ops->owner == NXGL_STACK_OWNER_V2_RAW_EGL) &&
         ops->start_video && ops->stop_video && ops->set_config &&
         ops->create_window && ops->create_context && ops->make_current &&
         ops->get_proc_address && ops->query_actual && ops->query_surface &&
         ops->validate_current && ops->release_attempt && ops->present;
}

static int nxgl_v2_options_valid(const nxgl_open_options_v2 *options) {
  const uint32_t known_flags =
      NXGL_OPEN_INITIALIZE_VIDEO |
      NXGL_OPEN_RETRY_GLES_HINT_AFTER_DESKTOP |
      NXGL_OPEN_ALLOW_NONDISPLAY_BACKEND |
      NXGL_OPEN_RETRY_AUTODETECT_AFTER_REAL_FAILURE;
  size_t index;
  size_t compatible = 0;
  if (!options || options->api_version != NXGL_API_VERSION_V2 ||
      options->struct_size < sizeof(*options) ||
      (options->flags & ~known_flags) != 0 ||
      !nxgl_v2_string_bounded(options->window_title, NXGL_DETAIL_MAX) ||
      options->display_index < 0 || options->drawable_wait_ms > 5000u ||
      !options->requirements || !options->candidates ||
      options->candidate_count == 0 ||
      options->candidate_count > NXGL_MAX_CONFIG_CANDIDATES ||
      options->requirements->api_version != NXGL_API_VERSION_V1 ||
      options->requirements->struct_size < sizeof(*options->requirements))
    return 0;
  if (options->fallback_facts &&
      (options->fallback_facts->api_version != NXGL_API_VERSION_V1 ||
       options->fallback_facts->struct_size <
           sizeof(*options->fallback_facts)))
    return 0;
  if (options->stack_ops &&
      (options->flags &
       (NXGL_OPEN_RETRY_GLES_HINT_AFTER_DESKTOP |
        NXGL_OPEN_RETRY_AUTODETECT_AFTER_REAL_FAILURE)) != 0)
    return 0;
#if defined(NXGL_M13_TESTING)
  if (!nxgl_v2_stack_ops_valid(options->stack_ops))
    return 0;
#else
  if (options->stack_ops && !nxgl_v2_stack_ops_valid(options->stack_ops))
    return 0;
#endif
  for (index = 0; index < options->candidate_count; ++index) {
    if (nxgl_candidate_compatible(options->requirements,
                                  &options->candidates[index]))
      ++compatible;
  }
  return compatible != 0;
}

static void nxgl_v2_handles_init(nxgl_stack_handles_v2 *handles,
                                 nxgl_stack_owner_v2 owner) {
  memset(handles, 0, sizeof(*handles));
  handles->api_version = NXGL_API_VERSION_V2;
  handles->struct_size = sizeof(*handles);
  handles->owner = owner;
}

static int nxgl_v2_handles_valid(const nxgl_stack_handles_v2 *handles,
                                 nxgl_stack_owner_v2 owner) {
  return handles && handles->api_version == NXGL_API_VERSION_V2 &&
         handles->struct_size >= sizeof(*handles) && handles->owner == owner;
}

static int nxgl_v2_handles_live_for_owner(
    const nxgl_stack_handles_v2 *handles, nxgl_stack_owner_v2 owner) {
  if (!nxgl_v2_handles_valid(handles, owner))
    return 0;
  if (owner == NXGL_STACK_OWNER_V2_SDL_EGL)
    return handles->sdl_window && handles->sdl_context &&
           handles->native_window != 0;
  return !handles->sdl_window && !handles->sdl_context &&
         handles->native_display != 0 && handles->native_window != 0 &&
         handles->egl_display != 0 && handles->egl_context != 0 &&
         handles->egl_surface != 0;
}

static int nxgl_v2_handles_window_for_owner(
    const nxgl_stack_handles_v2 *handles, nxgl_stack_owner_v2 owner) {
  if (!nxgl_v2_handles_valid(handles, owner))
    return 0;
  if (owner == NXGL_STACK_OWNER_V2_SDL_EGL)
    return handles->sdl_window && handles->native_window != 0;
  return !handles->sdl_window && !handles->sdl_context &&
         handles->native_display != 0 && handles->native_window != 0;
}

static int nxgl_v2_handles_empty(const nxgl_stack_handles_v2 *handles) {
  return handles && !handles->sdl_window && !handles->sdl_context &&
         handles->native_display == 0 && handles->native_window == 0 &&
         handles->egl_display == 0 && handles->egl_context == 0 &&
         handles->egl_surface == 0 && handles->egl_config == 0;
}

static int nxgl_v2_callback_contract(int callback_status, const char *error,
                                     size_t error_size) {
  if (!nxgl_v2_buffer_terminated(error, error_size))
    return NXGL_ERROR_STACK_MISMATCH;
  switch (callback_status) {
  case NXGL_SUCCESS:
  case NXGL_ERROR_INVALID_ARGUMENT:
  case NXGL_ERROR_VIDEO_UNAVAILABLE:
  case NXGL_ERROR_RESOLUTION_UNAVAILABLE:
  case NXGL_ERROR_NO_GLES_CONFIG:
  case NXGL_ERROR_PRESENT:
  case NXGL_ERROR_OUT_OF_MEMORY:
  case NXGL_ERROR_ROLLBACK:
  case NXGL_ERROR_STACK_MISMATCH:
    return callback_status;
  default:
    return NXGL_ERROR_STACK_MISMATCH;
  }
}

static int nxgl_v2_bits_valid(int value) {
  return value >= 0 && value <= 64;
}

static int nxgl_v2_actual_valid(const nxgl_config_actual *actual) {
  return actual && actual->gles_major >= 2 && actual->gles_major <= 9 &&
         actual->gles_minor >= 0 && actual->gles_minor <= 99 &&
         nxgl_v2_bits_valid(actual->red_bits) &&
         nxgl_v2_bits_valid(actual->green_bits) &&
         nxgl_v2_bits_valid(actual->blue_bits) &&
         nxgl_v2_bits_valid(actual->alpha_bits) &&
         nxgl_v2_bits_valid(actual->depth_bits) &&
         nxgl_v2_bits_valid(actual->stencil_bits) &&
         (actual->double_buffer == 0 || actual->double_buffer == 1) &&
         actual->profile_mask == SDL_GL_CONTEXT_PROFILE_ES;
}

static int nxgl_v2_egl_valid(const nxgl_egl_actual_v2 *egl,
                             const nxgl_config_actual *actual,
                             const nxgl_config_candidate *requested) {
  const int required_renderable_bit =
      requested && requested->gles_major >= 3
          ? NXGL_EGL_OPENGL_ES3_BIT_KHR
          : NXGL_EGL_OPENGL_ES2_BIT;
  return egl && actual && requested &&
         egl->api_version == NXGL_API_VERSION_V2 &&
         egl->struct_size >= sizeof(*egl) && egl->observed == 1 &&
         egl->display != 0 && egl->context != 0 && egl->surface != 0 &&
         egl->config != 0 && egl->config_id > 0 &&
         nxgl_v2_dimensions_valid(egl->surface_width, egl->surface_height) &&
         nxgl_v2_bits_valid(egl->red_bits) &&
         nxgl_v2_bits_valid(egl->green_bits) &&
         nxgl_v2_bits_valid(egl->blue_bits) &&
         nxgl_v2_bits_valid(egl->alpha_bits) &&
         nxgl_v2_bits_valid(egl->depth_bits) &&
         nxgl_v2_bits_valid(egl->stencil_bits) &&
         egl->red_bits == actual->red_bits &&
         egl->green_bits == actual->green_bits &&
         egl->blue_bits == actual->blue_bits &&
         egl->alpha_bits == actual->alpha_bits &&
         egl->depth_bits == actual->depth_bits &&
         egl->stencil_bits == actual->stencil_bits &&
         (egl->renderable_type & required_renderable_bit) != 0 &&
         (egl->surface_type & NXGL_EGL_WINDOW_BIT) != 0 &&
         memchr(egl->vendor, '\0', sizeof(egl->vendor)) != NULL &&
         memchr(egl->version, '\0', sizeof(egl->version)) != NULL &&
         memchr(egl->client_apis, '\0', sizeof(egl->client_apis)) != NULL &&
         egl->vendor[0] != '\0' && egl->version[0] != '\0' &&
         egl->client_apis[0] != '\0';
}

static nxgl_proc_v2 nxgl_v2_get_proc(nxgl_context *context,
                                     const char *name) {
  if (!context || !name || !*name)
    return NULL;
  return context->stack_ops.get_proc_address(context->stack_userdata, name);
}

static int nxgl_v2_assign_proc(void *destination, size_t destination_size,
                               nxgl_proc_v2 source) {
  if (!destination || destination_size != sizeof(source) || !source)
    return -1;
  memcpy(destination, &source, sizeof(source));
  return 0;
}

static int nxgl_v2_ascii_case_prefix(const char *value, size_t remaining,
                                     const char *prefix) {
  while (*prefix) {
    unsigned char left;
    unsigned char right = (unsigned char)*prefix++;
    if (remaining == 0 || !value || *value == '\0')
      return 0;
    left = (unsigned char)*value++;
    --remaining;
    if (left >= (unsigned char)'A' && left <= (unsigned char)'Z')
      left = (unsigned char)(left - (unsigned char)'A' + (unsigned char)'a');
    if (right >= (unsigned char)'A' && right <= (unsigned char)'Z')
      right =
          (unsigned char)(right - (unsigned char)'A' + (unsigned char)'a');
    if (left != right)
      return 0;
  }
  return 1;
}

static size_t nxgl_v2_ipv4_length(const char *value, size_t remaining) {
  size_t cursor = 0;
  unsigned group;
  if (!value || remaining == 0)
    return 0;
  for (group = 0; group < 4u; ++group) {
    unsigned digits = 0;
    unsigned number = 0;
    while (cursor < remaining && value[cursor] >= '0' &&
           value[cursor] <= '9' && digits < 3u) {
      number = number * 10u + (unsigned)(value[cursor] - '0');
      ++cursor;
      ++digits;
    }
    if (digits == 0 || number > 255u)
      return 0;
    if (group < 3u) {
      if (cursor >= remaining || value[cursor] != '.')
        return 0;
      ++cursor;
    }
  }
  if (cursor < remaining && value[cursor] >= '0' && value[cursor] <= '9')
    return 0;
  return cursor;
}

static void nxgl_v2_copy_diagnostic(char *destination,
                                    size_t destination_size,
                                    const char *source, size_t source_maximum) {
  static const char redacted[] = "[redacted]";
  static const char redacted_ip[] = "[redacted-ip]";
  static const char redacted_path[] = "[redacted-path]";
  size_t input = 0;
  size_t output = 0;
  if (!destination || destination_size == 0)
    return;
  destination[0] = '\0';
  if (!source)
    return;
  while (output + 1u < destination_size && input < source_maximum &&
         source[input] != '\0') {
    size_t ip_length =
        nxgl_v2_ipv4_length(source + input, source_maximum - input);
    const char *replacement = NULL;
    size_t replacement_size = 0;
    unsigned char byte;
    int path_like = source[input] == '/' || source[input] == '\\' ||
                    (input + 2u < source_maximum &&
                     ((source[input] >= 'a' && source[input] <= 'z') ||
                      (source[input] >= 'A' && source[input] <= 'Z')) &&
                     source[input + 1u] == ':' &&
                     (source[input + 2u] == '/' ||
                      source[input + 2u] == '\\'));
    if (path_like) {
      replacement = redacted_path;
      replacement_size = sizeof(redacted_path) - 1u;
      while (input < source_maximum && source[input] != '\0' &&
             source[input] != ' ' && source[input] != '\t' &&
             source[input] != ',' && source[input] != ';' &&
             source[input] != '|')
        ++input;
    } else if (nxgl_v2_ascii_case_prefix(source + input,
                                         source_maximum - input, "token") ||
               nxgl_v2_ascii_case_prefix(source + input,
                                         source_maximum - input, "secret") ||
               nxgl_v2_ascii_case_prefix(source + input,
                                         source_maximum - input,
                                         "password")) {
      replacement = redacted;
      replacement_size = sizeof(redacted) - 1u;
      while (input < source_maximum && source[input] != '\0' &&
             source[input] != ' ' && source[input] != '\t' &&
             source[input] != ',' && source[input] != ';' &&
             source[input] != '|')
        ++input;
    } else if (ip_length != 0) {
      replacement = redacted_ip;
      replacement_size = sizeof(redacted_ip) - 1u;
      input += ip_length;
    }
    if (replacement) {
      if (replacement_size > destination_size - output - 1u)
        replacement_size = destination_size - output - 1u;
      memcpy(destination + output, replacement, replacement_size);
      output += replacement_size;
      continue;
    }
    byte = (unsigned char)source[input++];
    if (byte < 0x20u || byte > 0x7eu)
      byte = (unsigned char)' ';
    if (byte == (unsigned char)'/' || byte == (unsigned char)'\\' ||
        byte == (unsigned char)':')
      byte = (unsigned char)'_';
    destination[output++] = (char)byte;
  }
  while (output > 0 && destination[output - 1u] == ' ')
    --output;
  destination[output] = '\0';
}

static void nxgl_v2_sanitize_egl_identity(nxgl_egl_actual_v2 *egl) {
  char vendor[NXGL_DETAIL_MAX];
  char version[NXGL_DETAIL_MAX];
  char client_apis[NXGL_DETAIL_MAX];
  memset(vendor, 0, sizeof(vendor));
  memset(version, 0, sizeof(version));
  memset(client_apis, 0, sizeof(client_apis));
  nxgl_v2_copy_diagnostic(vendor, sizeof(vendor), egl->vendor,
                          sizeof(egl->vendor));
  nxgl_v2_copy_diagnostic(version, sizeof(version), egl->version,
                          sizeof(egl->version));
  nxgl_v2_copy_diagnostic(client_apis, sizeof(client_apis), egl->client_apis,
                          sizeof(egl->client_apis));
  memcpy(egl->vendor, vendor, sizeof(vendor));
  memcpy(egl->version, version, sizeof(version));
  memcpy(egl->client_apis, client_apis, sizeof(client_apis));
}

static int nxgl_v2_resolve_gl(nxgl_context *context) {
  nxgl_proc_v2 proc;
#define NXGL_V2_LOAD_GL(field, symbol_name)                                  \
  do {                                                                       \
    proc = nxgl_v2_get_proc(context, symbol_name);                           \
    if (proc)                                                                \
      (void)nxgl_v2_assign_proc(&context->gl.field,                          \
                                sizeof(context->gl.field), proc);            \
  } while (0)
  memset(&context->gl, 0, sizeof(context->gl));
  NXGL_V2_LOAD_GL(get_string, "glGetString");
  NXGL_V2_LOAD_GL(get_error, "glGetError");
  NXGL_V2_LOAD_GL(finish, "glFinish");
  NXGL_V2_LOAD_GL(is_enabled, "glIsEnabled");
  NXGL_V2_LOAD_GL(get_booleanv, "glGetBooleanv");
  NXGL_V2_LOAD_GL(get_floatv, "glGetFloatv");
  NXGL_V2_LOAD_GL(get_integerv, "glGetIntegerv");
  NXGL_V2_LOAD_GL(disable, "glDisable");
  NXGL_V2_LOAD_GL(enable, "glEnable");
  NXGL_V2_LOAD_GL(color_mask, "glColorMask");
  NXGL_V2_LOAD_GL(clear_color, "glClearColor");
  NXGL_V2_LOAD_GL(clear, "glClear");
#undef NXGL_V2_LOAD_GL
  return context->gl.get_string ? NXGL_SUCCESS
                                : NXGL_ERROR_NO_GLES_CONFIG;
}

static void nxgl_v2_capture_gl_strings(nxgl_context *context) {
  nxgl_report *report = &context->report_v2.legacy;
  const unsigned char *value;
  value = context->gl.get_string(NXGL_GL_VENDOR);
  nxgl_v2_copy_diagnostic(report->vendor, sizeof(report->vendor),
                          (const char *)value, sizeof(report->vendor));
  value = context->gl.get_string(NXGL_GL_RENDERER);
  nxgl_v2_copy_diagnostic(report->renderer, sizeof(report->renderer),
                          (const char *)value, sizeof(report->renderer));
  value = context->gl.get_string(NXGL_GL_VERSION);
  nxgl_v2_copy_diagnostic(report->version, sizeof(report->version),
                          (const char *)value, sizeof(report->version));
  value = context->gl.get_string(NXGL_GL_SHADING_LANGUAGE_VERSION);
  nxgl_v2_copy_diagnostic(report->shading_language_version,
                          sizeof(report->shading_language_version),
                          (const char *)value,
                          sizeof(report->shading_language_version));
  value = context->gl.get_string(NXGL_GL_EXTENSIONS);
  nxgl_v2_copy_diagnostic(report->extensions, sizeof(report->extensions),
                          (const char *)value, sizeof(report->extensions));
}

static int nxgl_v2_release_attempt(nxgl_context *context,
                                   unsigned round_index,
                                   unsigned attempt_index,
                                   size_t candidate_index) {
  char error[NXGL_DETAIL_MAX];
  int status;
  memset(error, 0, sizeof(error));
  status = context->stack_ops.release_attempt(
      context->stack_userdata, &context->stack_handles, error, sizeof(error));
  status = nxgl_v2_callback_contract(status, error, sizeof(error));
  if (status != NXGL_SUCCESS ||
      !nxgl_v2_handles_valid(&context->stack_handles, context->stack_owner) ||
      !nxgl_v2_handles_empty(&context->stack_handles)) {
    nxgl_v2_record(context, round_index, attempt_index, candidate_index,
                   NXGL_OPEN_STAGE_V2_ATTEMPT_CLEANUP,
                   NXGL_OPEN_REASON_V2_CLEANUP_FAILED,
                   NXGL_ERROR_ROLLBACK);
    context->stack_cleanup_pending = 1;
    return NXGL_ERROR_ROLLBACK;
  }
  context->stack_cleanup_pending = 0;
  context->window = NULL;
  context->gl_context = NULL;
  memset(&context->gl, 0, sizeof(context->gl));
  return NXGL_SUCCESS;
}

static int nxgl_v2_stop_video(nxgl_context *context, unsigned round_index) {
  char error[NXGL_DETAIL_MAX];
  int status;
  if (!context->stack_video_active)
    return NXGL_SUCCESS;
  memset(error, 0, sizeof(error));
  status = context->stack_ops.stop_video(
      context->stack_userdata, context->stack_video_initialized, error,
      sizeof(error));
  status = nxgl_v2_callback_contract(status, error, sizeof(error));
  if (status != NXGL_SUCCESS) {
    nxgl_v2_record(context, round_index, 0, (size_t)-1,
                   NXGL_OPEN_STAGE_V2_VIDEO_STOP,
                   NXGL_OPEN_REASON_V2_CLEANUP_FAILED,
                   NXGL_ERROR_ROLLBACK);
    return NXGL_ERROR_ROLLBACK;
  }
  context->stack_video_initialized = 0;
  context->stack_video_active = 0;
  return NXGL_SUCCESS;
}

static int nxgl_v2_fail_attempt(nxgl_context *context, unsigned round_index,
                                unsigned attempt_index,
                                size_t candidate_index,
                                nxgl_open_stage_v2 stage,
                                nxgl_open_reason_v2 reason, int status) {
  nxgl_v2_record(context, round_index, attempt_index, candidate_index, stage,
                 reason, status);
  if (nxgl_v2_release_attempt(context, round_index, attempt_index,
                              candidate_index) != NXGL_SUCCESS)
    return NXGL_ERROR_ROLLBACK;
  return status;
}

static int nxgl_v2_observe_positive_surface(
    nxgl_context *context, const nxgl_open_options_v2 *options,
    int *window_width, int *window_height, int *drawable_width,
    int *drawable_height) {
  unsigned passes = options->drawable_wait_ms / 10u + 1u;
  unsigned pass;
  char error[NXGL_DETAIL_MAX];
  int status = NXGL_ERROR_NO_GLES_CONFIG;
  for (pass = 0; pass < passes; ++pass) {
    memset(error, 0, sizeof(error));
    *window_width = 0;
    *window_height = 0;
    *drawable_width = 0;
    *drawable_height = 0;
    status = context->stack_ops.query_surface(
        context->stack_userdata, &context->stack_handles, window_width,
        window_height, drawable_width, drawable_height, error, sizeof(error));
    status = nxgl_v2_callback_contract(status, error, sizeof(error));
    if (status == NXGL_SUCCESS &&
        nxgl_v2_dimensions_valid(*window_width, *window_height) &&
        nxgl_v2_dimensions_valid(*drawable_width, *drawable_height))
      return NXGL_SUCCESS;
#if !defined(NXGL_M13_TESTING)
    if (context->stack_owner == NXGL_STACK_OWNER_V2_SDL_EGL &&
        pass + 1u < passes) {
      nxgl_pump_events_without_consuming();
      SDL_Delay(10u);
    }
#else
    (void)pass;
#endif
  }
  return status == NXGL_ERROR_STACK_MISMATCH ? status
                                             : NXGL_ERROR_NO_GLES_CONFIG;
}

static int nxgl_v2_attempt_candidate(
    nxgl_context *context, const nxgl_open_options_v2 *options,
    const nxgl_resolution *resolution, const char *backend,
    unsigned round_index, size_t candidate_index) {
  nxgl_stack_attempt_request_v2 request;
  nxgl_config_actual actual;
  nxgl_egl_actual_v2 egl;
  char validation_error[NXGL_DETAIL_MAX];
  char error[NXGL_DETAIL_MAX];
  const char *gl_version;
  unsigned attempt_index;
  int parsed_major = 0;
  int parsed_minor = 0;
  int window_width = 0;
  int window_height = 0;
  int drawable_width = 0;
  int drawable_height = 0;
  int status;

  ++context->report_v2.legacy.attempt_count;
  attempt_index = context->report_v2.legacy.attempt_count;
  nxgl_v2_handles_init(&context->stack_handles, context->stack_owner);
  context->stack_cleanup_pending = 1;
  memset(&request, 0, sizeof(request));
  request.api_version = NXGL_API_VERSION_V2;
  request.struct_size = sizeof(request);
  request.round_index = round_index;
  request.attempt_index = attempt_index;
  request.candidate_index = candidate_index;
  request.window_title = options->window_title ? options->window_title : "nxgl";
  request.window_flags = options->window_flags;
  request.display_index = options->display_index;
  request.resolution = *resolution;
  request.candidate = options->candidates[candidate_index];

  memset(error, 0, sizeof(error));
  status = context->stack_ops.set_config(context->stack_userdata,
                                         &request.candidate, error,
                                         sizeof(error));
  status = nxgl_v2_callback_contract(status, error, sizeof(error));
  if (status != NXGL_SUCCESS)
    return nxgl_v2_fail_attempt(
        context, round_index, attempt_index, candidate_index,
        NXGL_OPEN_STAGE_V2_CONFIG_ATTRIBUTES,
        status == NXGL_ERROR_STACK_MISMATCH
            ? NXGL_OPEN_REASON_V2_PROVIDER_CONTRACT
            : NXGL_OPEN_REASON_V2_ATTRIBUTES_REJECTED,
        status);

  memset(error, 0, sizeof(error));
  status = context->stack_ops.create_window(
      context->stack_userdata, &request, &context->stack_handles, error,
      sizeof(error));
  status = nxgl_v2_callback_contract(status, error, sizeof(error));
  if (status != NXGL_SUCCESS ||
      !nxgl_v2_handles_window_for_owner(&context->stack_handles,
                                        context->stack_owner))
    return nxgl_v2_fail_attempt(
        context, round_index, attempt_index, candidate_index,
        NXGL_OPEN_STAGE_V2_WINDOW_CREATE,
        status == NXGL_ERROR_STACK_MISMATCH
            ? NXGL_OPEN_REASON_V2_PROVIDER_CONTRACT
            : NXGL_OPEN_REASON_V2_WINDOW_FAILED,
        status == NXGL_SUCCESS ? NXGL_ERROR_STACK_MISMATCH : status);

  memset(error, 0, sizeof(error));
  status = context->stack_ops.create_context(
      context->stack_userdata, &context->stack_handles, error, sizeof(error));
  status = nxgl_v2_callback_contract(status, error, sizeof(error));
  if (status != NXGL_SUCCESS ||
      !nxgl_v2_handles_live_for_owner(&context->stack_handles,
                                      context->stack_owner))
    return nxgl_v2_fail_attempt(
        context, round_index, attempt_index, candidate_index,
        NXGL_OPEN_STAGE_V2_CONTEXT_CREATE,
        status == NXGL_ERROR_STACK_MISMATCH
            ? NXGL_OPEN_REASON_V2_PROVIDER_CONTRACT
            : NXGL_OPEN_REASON_V2_CONTEXT_FAILED,
        status == NXGL_SUCCESS ? NXGL_ERROR_STACK_MISMATCH : status);

  memset(error, 0, sizeof(error));
  status = context->stack_ops.make_current(
      context->stack_userdata, &context->stack_handles, error, sizeof(error));
  status = nxgl_v2_callback_contract(status, error, sizeof(error));
  if (status != NXGL_SUCCESS)
    return nxgl_v2_fail_attempt(
        context, round_index, attempt_index, candidate_index,
        NXGL_OPEN_STAGE_V2_MAKE_CURRENT,
        status == NXGL_ERROR_STACK_MISMATCH
            ? NXGL_OPEN_REASON_V2_PROVIDER_CONTRACT
            : NXGL_OPEN_REASON_V2_CURRENT_FAILED,
        status);

  /* Independently prove the expected context is current before crossing the
   * provider boundary for even the first GL/EGL entry point or query. */
  memset(error, 0, sizeof(error));
  status = context->stack_ops.validate_current(
      context->stack_userdata, &context->stack_handles, error, sizeof(error));
  status = nxgl_v2_callback_contract(status, error, sizeof(error));
  if (status != NXGL_SUCCESS)
    return nxgl_v2_fail_attempt(
        context, round_index, attempt_index, candidate_index,
        NXGL_OPEN_STAGE_V2_CURRENT_VALIDATE,
        status == NXGL_ERROR_STACK_MISMATCH
            ? NXGL_OPEN_REASON_V2_PROVIDER_CONTRACT
            : NXGL_OPEN_REASON_V2_CURRENT_MISMATCH,
        status);

  status = nxgl_v2_resolve_gl(context);
  if (status != NXGL_SUCCESS)
    return nxgl_v2_fail_attempt(context, round_index, attempt_index,
                                candidate_index,
                                NXGL_OPEN_STAGE_V2_PROC_RESOLVE,
                                NXGL_OPEN_REASON_V2_PROC_UNAVAILABLE, status);
  gl_version = (const char *)context->gl.get_string(NXGL_GL_VERSION);
  if (nxgl_parse_gles_version(gl_version, &parsed_major, &parsed_minor) !=
      NXGL_SUCCESS)
    return nxgl_v2_fail_attempt(
        context, round_index, attempt_index, candidate_index,
        NXGL_OPEN_STAGE_V2_CONFIG_QUERY,
        NXGL_OPEN_REASON_V2_DESKTOP_GL_REJECTED,
        NXGL_ERROR_NO_GLES_CONFIG);

  memset(&actual, 0, sizeof(actual));
  memset(&egl, 0, sizeof(egl));
  egl.api_version = NXGL_API_VERSION_V2;
  egl.struct_size = sizeof(egl);
  memset(error, 0, sizeof(error));
  status = context->stack_ops.query_actual(
      context->stack_userdata, &context->stack_handles, &actual, &egl, error,
      sizeof(error));
  status = nxgl_v2_callback_contract(status, error, sizeof(error));
  if (status != NXGL_SUCCESS)
    return nxgl_v2_fail_attempt(
        context, round_index, attempt_index, candidate_index,
        NXGL_OPEN_STAGE_V2_CONFIG_QUERY,
        status == NXGL_ERROR_STACK_MISMATCH
            ? NXGL_OPEN_REASON_V2_PROVIDER_CONTRACT
            : NXGL_OPEN_REASON_V2_CONFIG_QUERY_FAILED,
        status);
  if (!nxgl_v2_handles_live_for_owner(&context->stack_handles,
                                      context->stack_owner) ||
      !nxgl_v2_actual_valid(&actual) ||
      !nxgl_v2_egl_valid(&egl, &actual, &request.candidate) ||
      actual.gles_major != parsed_major ||
      actual.gles_minor != parsed_minor ||
      context->stack_handles.egl_display != egl.display ||
      context->stack_handles.egl_context != egl.context ||
      context->stack_handles.egl_surface != egl.surface ||
      context->stack_handles.egl_config != egl.config)
    return nxgl_v2_fail_attempt(
        context, round_index, attempt_index, candidate_index,
        NXGL_OPEN_STAGE_V2_CONFIG_QUERY,
        NXGL_OPEN_REASON_V2_EGL_CONFIG_UNAVAILABLE,
        NXGL_ERROR_STACK_MISMATCH);
  if (nxgl_validate_actual(options->requirements, &actual, validation_error,
                           sizeof(validation_error)) != NXGL_SUCCESS)
    return nxgl_v2_fail_attempt(
        context, round_index, attempt_index, candidate_index,
        NXGL_OPEN_STAGE_V2_CONFIG_QUERY,
        NXGL_OPEN_REASON_V2_CONFIG_REJECTED,
        NXGL_ERROR_NO_GLES_CONFIG);
  nxgl_v2_sanitize_egl_identity(&egl);

  status = nxgl_v2_observe_positive_surface(
      context, options, &window_width, &window_height, &drawable_width,
      &drawable_height);
  if (status != NXGL_SUCCESS ||
      egl.surface_width != drawable_width ||
      egl.surface_height != drawable_height)
    return nxgl_v2_fail_attempt(
        context, round_index, attempt_index, candidate_index,
        NXGL_OPEN_STAGE_V2_DRAWABLE_QUERY,
        status == NXGL_ERROR_STACK_MISMATCH
            ? NXGL_OPEN_REASON_V2_PROVIDER_CONTRACT
            : NXGL_OPEN_REASON_V2_DRAWABLE_INVALID,
        status == NXGL_SUCCESS ? NXGL_ERROR_NO_GLES_CONFIG : status);

  memset(error, 0, sizeof(error));
  status = context->stack_ops.validate_current(
      context->stack_userdata, &context->stack_handles, error, sizeof(error));
  status = nxgl_v2_callback_contract(status, error, sizeof(error));
  if (status != NXGL_SUCCESS)
    return nxgl_v2_fail_attempt(
        context, round_index, attempt_index, candidate_index,
        NXGL_OPEN_STAGE_V2_CURRENT_VALIDATE,
        status == NXGL_ERROR_STACK_MISMATCH
            ? NXGL_OPEN_REASON_V2_PROVIDER_CONTRACT
            : NXGL_OPEN_REASON_V2_CURRENT_MISMATCH,
        status);

  context->window = context->stack_handles.sdl_window;
  context->gl_context = context->stack_handles.sdl_context;
  context->report_v2.legacy.resolution = *resolution;
  context->report_v2.legacy.window_width = window_width;
  context->report_v2.legacy.window_height = window_height;
  context->report_v2.legacy.drawable_width = drawable_width;
  context->report_v2.legacy.drawable_height = drawable_height;
  context->report_v2.legacy.selected_candidate_index = candidate_index;
  context->report_v2.legacy.requested = request.candidate;
  context->report_v2.legacy.actual = actual;
  nxgl_copy_string(context->report_v2.legacy.video_backend,
                   sizeof(context->report_v2.legacy.video_backend), backend);
  nxgl_v2_capture_gl_strings(context);
  context->report_v2.handles = context->stack_handles;
  context->report_v2.egl = egl;
  nxgl_v2_record(context, round_index, attempt_index, candidate_index,
                 NXGL_OPEN_STAGE_V2_CURRENT_VALIDATE,
                 NXGL_OPEN_REASON_V2_SELECTED, NXGL_SUCCESS);
  return NXGL_SUCCESS;
}

static int nxgl_v2_begin_gles_hint_recovery(nxgl_context *context,
                                            const char *backend,
                                            unsigned round_index,
                                            size_t candidate_index) {
  int needs_x11_egl = backend && SDL_strcasecmp(backend, "x11") == 0;
  if (SDL_SetHint(NXGL_HINT_OPENGL_ES_DRIVER, "1") != SDL_TRUE) {
    nxgl_v2_record(context, round_index,
                   context->report_v2.legacy.attempt_count, candidate_index,
                   NXGL_OPEN_STAGE_V2_HINT_APPLY,
                   NXGL_OPEN_REASON_V2_HINT_APPLY_FAILED,
                   NXGL_ERROR_NO_GLES_CONFIG);
    return NXGL_ERROR_NO_GLES_CONFIG;
  }
  context->changed_gles_hint = 1;
  if (needs_x11_egl &&
      SDL_SetHint(NXGL_HINT_VIDEO_X11_FORCE_EGL, "1") != SDL_TRUE) {
    if (nxgl_restore_changed_hints(context) != 0) {
      context->report_v2.hints_restored = 0;
      nxgl_v2_record(context, round_index,
                     context->report_v2.legacy.attempt_count,
                     candidate_index, NXGL_OPEN_STAGE_V2_HINT_RESTORE,
                     NXGL_OPEN_REASON_V2_RESTORE_FAILED,
                     NXGL_ERROR_ROLLBACK);
      return NXGL_ERROR_ROLLBACK;
    }
    context->report_v2.hints_restored = 1;
    nxgl_v2_record(context, round_index,
                   context->report_v2.legacy.attempt_count, candidate_index,
                   NXGL_OPEN_STAGE_V2_HINT_APPLY,
                   NXGL_OPEN_REASON_V2_HINT_APPLY_FAILED,
                   NXGL_ERROR_NO_GLES_CONFIG);
    return NXGL_ERROR_NO_GLES_CONFIG;
  }
  if (needs_x11_egl)
    context->changed_x11_egl_hint = 1;
  context->report_v2.legacy.used_gles_hint_recovery = 1;
  return NXGL_SUCCESS;
}

static int nxgl_v2_run_round(nxgl_context *context,
                             const nxgl_open_options_v2 *options,
                             const nxgl_resolution_sources *sources,
                             const char *backend, unsigned round_index) {
  nxgl_resolution resolution;
  size_t candidate_index;
  int status;
  if (nxgl_choose_resolution(sources, &resolution) != NXGL_SUCCESS) {
    nxgl_v2_record(context, round_index, 0, (size_t)-1,
                   NXGL_OPEN_STAGE_V2_RESOLUTION,
                   NXGL_OPEN_REASON_V2_RESOLUTION_UNAVAILABLE,
                   NXGL_ERROR_RESOLUTION_UNAVAILABLE);
    return NXGL_ERROR_RESOLUTION_UNAVAILABLE;
  }
  for (candidate_index = 0; candidate_index < options->candidate_count;
       ++candidate_index) {
    if (!nxgl_candidate_compatible(options->requirements,
                                   &options->candidates[candidate_index]))
      continue;
    status = nxgl_v2_attempt_candidate(context, options, &resolution, backend,
                                       round_index, candidate_index);
    if (status != NXGL_SUCCESS && status != NXGL_ERROR_ROLLBACK &&
        context->stack_is_default_sdl &&
        context->report_v2.final_reason ==
            NXGL_OPEN_REASON_V2_DESKTOP_GL_REJECTED &&
        (options->flags & NXGL_OPEN_RETRY_GLES_HINT_AFTER_DESKTOP) != 0 &&
        !context->report_v2.legacy.used_gles_hint_recovery &&
        context->previous_gles_hint_preservable &&
        context->previous_x11_egl_hint_preservable) {
      const char *current_backend = SDL_GetCurrentVideoDriver();
      int hint_status = nxgl_v2_begin_gles_hint_recovery(
          context, current_backend, round_index, candidate_index);
      if (hint_status == NXGL_SUCCESS) {
        status = nxgl_v2_attempt_candidate(
            context, options, &resolution, backend, round_index,
            candidate_index);
        if (nxgl_restore_changed_hints(context) != 0) {
          context->report_v2.hints_restored = 0;
          nxgl_v2_record(context, round_index,
                         context->report_v2.legacy.attempt_count,
                         candidate_index, NXGL_OPEN_STAGE_V2_HINT_RESTORE,
                         NXGL_OPEN_REASON_V2_RESTORE_FAILED,
                         NXGL_ERROR_ROLLBACK);
          return NXGL_ERROR_ROLLBACK;
        }
        context->report_v2.hints_restored = 1;
      } else if (hint_status == NXGL_ERROR_ROLLBACK) {
        return hint_status;
      }
    }
    if (status == NXGL_SUCCESS || status == NXGL_ERROR_ROLLBACK ||
        status == NXGL_ERROR_STACK_MISMATCH ||
        status == NXGL_ERROR_OUT_OF_MEMORY ||
        status == NXGL_ERROR_INVALID_ARGUMENT)
      return status;
  }
  return NXGL_ERROR_NO_GLES_CONFIG;
}

#if defined(NXGL_M13_TESTING)
int nxgl_test_v2_hint_recovery_transaction(const char *backend,
                                            int *attempt_permitted,
                                            nxgl_report_v2 *report) {
  nxgl_context context;
  int status;
  if (!backend || !attempt_permitted || !report)
    return NXGL_ERROR_INVALID_ARGUMENT;
  memset(&context, 0, sizeof(context));
  nxgl_v2_report_init(&context.report_v2);
  context.stack_is_default_sdl = 1;
  context.previous_gles_hint_preservable = 1;
  context.previous_x11_egl_hint_preservable = 1;
  *attempt_permitted = 0;
  if (nxgl_save_hint_snapshots(&context) != 0) {
    context.report_v2.final_stage = NXGL_OPEN_STAGE_V2_HINT_SNAPSHOT;
    context.report_v2.final_reason = NXGL_OPEN_REASON_V2_SNAPSHOT_FAILED;
    *report = context.report_v2;
    nxgl_free_hint_snapshots(&context);
    return NXGL_ERROR_ROLLBACK;
  }
  status = nxgl_v2_begin_gles_hint_recovery(&context, backend, 0u, 0u);
  if (status == NXGL_SUCCESS) {
    *attempt_permitted = 1;
    if (nxgl_restore_changed_hints(&context) != 0) {
      context.report_v2.hints_restored = 0;
      nxgl_v2_record(&context, 0u, 0u, 0u,
                     NXGL_OPEN_STAGE_V2_HINT_RESTORE,
                     NXGL_OPEN_REASON_V2_RESTORE_FAILED,
                     NXGL_ERROR_ROLLBACK);
      status = NXGL_ERROR_ROLLBACK;
    } else {
      context.report_v2.hints_restored = 1;
    }
  }
  *report = context.report_v2;
  nxgl_free_hint_snapshots(&context);
  return status;
}
#endif

static int nxgl_v2_start_video(nxgl_context *context,
                               const nxgl_open_options_v2 *options,
                               nxgl_resolution_sources *sources,
                               char *backend, size_t backend_size,
                               unsigned round_index) {
  char error[NXGL_DETAIL_MAX];
  int initialized = 0;
  int status;
  memset(sources, 0, sizeof(*sources));
  sources->api_version = NXGL_API_VERSION_V1;
  sources->struct_size = sizeof(*sources);
  memset(backend, 0, backend_size);
  memset(error, 0, sizeof(error));
  status = context->stack_ops.start_video(
      context->stack_userdata,
      (options->flags & NXGL_OPEN_INITIALIZE_VIDEO) != 0,
      options->display_index, options->fallback_facts, sources, backend,
      backend_size, &initialized, error, sizeof(error));
  status = nxgl_v2_callback_contract(status, error, sizeof(error));
  context->stack_video_initialized = initialized != 0;
  /* start_video is a transactional call even when it reports that the video
   * subsystem was preowned.  A failed callback may still have touched stack
   * state, so stop_video is required exactly once after every invocation. */
  context->stack_video_active = 1;
  if (!nxgl_v2_buffer_terminated(backend, backend_size) ||
      sources->api_version != NXGL_API_VERSION_V1 ||
      sources->struct_size < sizeof(*sources) ||
      (initialized != 0 && initialized != 1))
    status = NXGL_ERROR_STACK_MISMATCH;
  if (status != NXGL_SUCCESS) {
    nxgl_v2_record(
        context, round_index, 0, (size_t)-1,
        NXGL_OPEN_STAGE_V2_VIDEO_START,
        status == NXGL_ERROR_STACK_MISMATCH
            ? NXGL_OPEN_REASON_V2_PROVIDER_CONTRACT
            : NXGL_OPEN_REASON_V2_VIDEO_UNAVAILABLE,
        status);
    if (nxgl_v2_stop_video(context, round_index) != NXGL_SUCCESS)
      return NXGL_ERROR_ROLLBACK;
    return status;
  }
  if (!nxgl_v2_backend_token_valid(backend, backend_size - 1u)) {
    nxgl_v2_record(context, round_index, 0, (size_t)-1,
                   NXGL_OPEN_STAGE_V2_VIDEO_START,
                   NXGL_OPEN_REASON_V2_PROVIDER_CONTRACT,
                   NXGL_ERROR_STACK_MISMATCH);
    if (nxgl_v2_stop_video(context, round_index) != NXGL_SUCCESS)
      return NXGL_ERROR_ROLLBACK;
    return NXGL_ERROR_STACK_MISMATCH;
  }
#if !defined(NXGL_M13_TESTING)
  if (context->stack_is_default_sdl &&
      (options->flags & NXGL_OPEN_ALLOW_NONDISPLAY_BACKEND) == 0 &&
      !nxgl_backend_is_display(backend)) {
    nxgl_v2_record(context, round_index, 0, (size_t)-1,
                   NXGL_OPEN_STAGE_V2_VIDEO_START,
                   NXGL_OPEN_REASON_V2_VIDEO_UNAVAILABLE,
                   NXGL_ERROR_VIDEO_UNAVAILABLE);
    if (nxgl_v2_stop_video(context, round_index) != NXGL_SUCCESS)
      return NXGL_ERROR_ROLLBACK;
    return NXGL_ERROR_VIDEO_UNAVAILABLE;
  }
#endif
  return NXGL_SUCCESS;
}

#if !defined(NXGL_M13_TESTING)
typedef void *(*nxgl_egl_get_current_display_fn)(void);
typedef void *(*nxgl_egl_get_current_context_fn)(void);
typedef void *(*nxgl_egl_get_current_surface_fn)(int which);
typedef int (*nxgl_egl_query_context_fn)(void *display, void *context,
                                         int attribute, int *value);
typedef int (*nxgl_egl_get_configs_fn)(void *display, void **configs,
                                       int config_size, int *count);
typedef int (*nxgl_egl_get_config_attrib_fn)(void *display, void *config,
                                             int attribute, int *value);
typedef int (*nxgl_egl_query_surface_fn)(void *display, void *surface,
                                         int attribute, int *value);
typedef const char *(*nxgl_egl_query_string_fn)(void *display, int name);

#define NXGL_EGL_DRAW 0x3059
#define NXGL_EGL_CONFIG_ID 0x3028
#define NXGL_EGL_RED_SIZE 0x3024
#define NXGL_EGL_GREEN_SIZE 0x3023
#define NXGL_EGL_BLUE_SIZE 0x3022
#define NXGL_EGL_ALPHA_SIZE 0x3021
#define NXGL_EGL_DEPTH_SIZE 0x3025
#define NXGL_EGL_STENCIL_SIZE 0x3026
#define NXGL_EGL_RENDERABLE_TYPE 0x3040
#define NXGL_EGL_SURFACE_TYPE 0x3033
#define NXGL_EGL_WIDTH 0x3057
#define NXGL_EGL_HEIGHT 0x3056
#define NXGL_EGL_RENDER_BUFFER 0x3086
#define NXGL_EGL_BACK_BUFFER 0x3084
#define NXGL_EGL_VENDOR 0x3053
#define NXGL_EGL_VERSION 0x3054
#define NXGL_EGL_CLIENT_APIS 0x308D

static int nxgl_v2_sdl_start_video(
    void *userdata, int allow_initialize, int display_index,
    const nxgl_resolution_sources *fallback_facts,
    nxgl_resolution_sources *sources, char *backend, size_t backend_size,
    int *initialized_by_stack, char *error, size_t error_size) {
  SDL_DisplayMode mode;
  SDL_Rect bounds;
  const char *driver;
  (void)userdata;
  if (!sources || !backend || backend_size == 0 || !initialized_by_stack ||
      !error || error_size == 0)
    return NXGL_ERROR_INVALID_ARGUMENT;
  *initialized_by_stack = 0;
  if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
    if (!allow_initialize) {
      nxgl_copy_string(error, error_size, "video-not-initialized");
      return NXGL_ERROR_VIDEO_UNAVAILABLE;
    }
    SDL_ClearError();
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
      nxgl_copy_string(error, error_size, "video-init-failed");
      return NXGL_ERROR_VIDEO_UNAVAILABLE;
    }
    *initialized_by_stack = 1;
  }
  nxgl_resolution_sources_init(sources);
  if (fallback_facts) {
    sources->drm_width = fallback_facts->drm_width;
    sources->drm_height = fallback_facts->drm_height;
    sources->fbdev_width = fallback_facts->fbdev_width;
    sources->fbdev_height = fallback_facts->fbdev_height;
  }
  memset(&mode, 0, sizeof(mode));
  if (SDL_GetDesktopDisplayMode(display_index, &mode) == 0) {
    sources->sdl_desktop_width = mode.w;
    sources->sdl_desktop_height = mode.h;
  }
  memset(&mode, 0, sizeof(mode));
  if (SDL_GetCurrentDisplayMode(display_index, &mode) == 0) {
    sources->sdl_current_width = mode.w;
    sources->sdl_current_height = mode.h;
  }
  memset(&bounds, 0, sizeof(bounds));
  if (SDL_GetDisplayBounds(display_index, &bounds) == 0) {
    sources->sdl_bounds_width = bounds.w;
    sources->sdl_bounds_height = bounds.h;
  }
  driver = SDL_GetCurrentVideoDriver();
  nxgl_copy_string(backend, backend_size, driver);
  if (!driver || !*driver) {
    nxgl_copy_string(error, error_size, "video-backend-missing");
    return NXGL_ERROR_VIDEO_UNAVAILABLE;
  }
  return NXGL_SUCCESS;
}

static int nxgl_v2_sdl_stop_video(void *userdata, int initialized_by_stack,
                                  char *error, size_t error_size) {
  (void)userdata;
  if (!error || error_size == 0)
    return NXGL_ERROR_INVALID_ARGUMENT;
  if (initialized_by_stack) {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
      nxgl_copy_string(error, error_size, "video-stop-not-verified");
      return NXGL_ERROR_ROLLBACK;
    }
  }
  return NXGL_SUCCESS;
}

static int nxgl_v2_sdl_set_config(
    void *userdata, const nxgl_config_candidate *candidate, char *error,
    size_t error_size) {
  (void)userdata;
  if (!candidate || !error || error_size == 0)
    return NXGL_ERROR_INVALID_ARGUMENT;
  if (nxgl_set_config_attributes(candidate) != 0) {
    nxgl_copy_string(error, error_size, "config-attributes-rejected");
    return NXGL_ERROR_NO_GLES_CONFIG;
  }
  return NXGL_SUCCESS;
}

static int nxgl_v2_sdl_create_window(
    void *userdata, const nxgl_stack_attempt_request_v2 *request,
    nxgl_stack_handles_v2 *handles, char *error, size_t error_size) {
  int position;
  (void)userdata;
  if (!request || !handles || !error || error_size == 0)
    return NXGL_ERROR_INVALID_ARGUMENT;
  position =
      (int)SDL_WINDOWPOS_CENTERED_DISPLAY((Uint32)request->display_index);
  handles->sdl_window = SDL_CreateWindow(
      request->window_title, position, position, request->resolution.width,
      request->resolution.height, request->window_flags | SDL_WINDOW_OPENGL);
  if (!handles->sdl_window) {
    nxgl_copy_string(error, error_size, "window-create-failed");
    return NXGL_ERROR_NO_GLES_CONFIG;
  }
  handles->native_window = (uintptr_t)(void *)handles->sdl_window;
  return NXGL_SUCCESS;
}

static int nxgl_v2_sdl_create_context(void *userdata,
                                      nxgl_stack_handles_v2 *handles,
                                      char *error, size_t error_size) {
  (void)userdata;
  if (!handles || !handles->sdl_window || !error || error_size == 0)
    return NXGL_ERROR_INVALID_ARGUMENT;
  handles->sdl_context = SDL_GL_CreateContext(handles->sdl_window);
  if (!handles->sdl_context) {
    nxgl_copy_string(error, error_size, "context-create-failed");
    return NXGL_ERROR_NO_GLES_CONFIG;
  }
  return NXGL_SUCCESS;
}

static int nxgl_v2_sdl_make_current(
    void *userdata, const nxgl_stack_handles_v2 *handles, char *error,
    size_t error_size) {
  (void)userdata;
  if (!handles || !handles->sdl_window || !handles->sdl_context || !error ||
      error_size == 0)
    return NXGL_ERROR_INVALID_ARGUMENT;
  if (SDL_GL_MakeCurrent(handles->sdl_window, handles->sdl_context) != 0) {
    nxgl_copy_string(error, error_size, "make-current-failed");
    return NXGL_ERROR_NO_GLES_CONFIG;
  }
  return NXGL_SUCCESS;
}

static nxgl_proc_v2 nxgl_v2_sdl_get_proc(void *userdata, const char *name) {
  nxgl_proc_v2 result = NULL;
  void *address;
  (void)userdata;
  if (!name)
    return NULL;
  address = SDL_GL_GetProcAddress(name);
  if (sizeof(result) == sizeof(address))
    memcpy(&result, &address, sizeof(result));
  return result;
}

static int nxgl_v2_sdl_load_egl_proc(nxgl_proc_v2 generic, void *target,
                                     size_t target_size) {
  return nxgl_v2_assign_proc(target, target_size, generic);
}

static int nxgl_v2_sdl_egl_attribute(nxgl_egl_get_config_attrib_fn get_attrib,
                                     void *display, void *config,
                                     int attribute, int *value) {
  *value = -1;
  return get_attrib(display, config, attribute, value) ? 0 : -1;
}

static int nxgl_v2_sdl_query_actual(
    void *userdata, nxgl_stack_handles_v2 *handles,
    nxgl_config_actual *actual, nxgl_egl_actual_v2 *egl, char *error,
    size_t error_size) {
  nxgl_egl_get_current_display_fn get_current_display = NULL;
  nxgl_egl_get_current_context_fn get_current_context = NULL;
  nxgl_egl_get_current_surface_fn get_current_surface = NULL;
  nxgl_egl_query_context_fn query_context = NULL;
  nxgl_egl_get_configs_fn get_configs = NULL;
  nxgl_egl_get_config_attrib_fn get_config_attrib = NULL;
  nxgl_egl_query_surface_fn query_surface = NULL;
  nxgl_egl_query_string_fn query_string = NULL;
  nxgl_gl_get_string_fn get_string = NULL;
  void *configs[NXGL_EGL_CONFIG_MAX];
  void *display;
  void *context;
  void *surface;
  void *config = NULL;
  const char *gl_version;
  int config_count = 0;
  int config_id = -1;
  int index;
  int value = -1;
  const char *egl_vendor;
  const char *egl_version;
  const char *egl_client_apis;
  (void)userdata;
  if (!handles || !actual || !egl || !error || error_size == 0)
    return NXGL_ERROR_INVALID_ARGUMENT;
#define NXGL_V2_LOAD_EGL(variable, type, symbol_name)                        \
  do {                                                                       \
    if (nxgl_v2_sdl_load_egl_proc(                                           \
            nxgl_v2_sdl_get_proc(NULL, symbol_name), &(variable),           \
            sizeof(type)) != 0) {                                            \
      nxgl_copy_string(error, error_size, "egl-entry-point-unavailable");  \
      return NXGL_ERROR_NO_GLES_CONFIG;                                      \
    }                                                                        \
  } while (0)
  NXGL_V2_LOAD_EGL(get_current_display, nxgl_egl_get_current_display_fn,
                   "eglGetCurrentDisplay");
  NXGL_V2_LOAD_EGL(get_current_context, nxgl_egl_get_current_context_fn,
                   "eglGetCurrentContext");
  NXGL_V2_LOAD_EGL(get_current_surface, nxgl_egl_get_current_surface_fn,
                   "eglGetCurrentSurface");
  NXGL_V2_LOAD_EGL(query_context, nxgl_egl_query_context_fn,
                   "eglQueryContext");
  NXGL_V2_LOAD_EGL(get_configs, nxgl_egl_get_configs_fn, "eglGetConfigs");
  NXGL_V2_LOAD_EGL(get_config_attrib, nxgl_egl_get_config_attrib_fn,
                   "eglGetConfigAttrib");
  NXGL_V2_LOAD_EGL(query_surface, nxgl_egl_query_surface_fn,
                   "eglQuerySurface");
  NXGL_V2_LOAD_EGL(query_string, nxgl_egl_query_string_fn, "eglQueryString");
  NXGL_V2_LOAD_EGL(get_string, nxgl_gl_get_string_fn, "glGetString");
#undef NXGL_V2_LOAD_EGL
  display = get_current_display();
  context = get_current_context();
  surface = get_current_surface(NXGL_EGL_DRAW);
  if (!display || !context || !surface ||
      !query_context(display, context, NXGL_EGL_CONFIG_ID, &config_id) ||
      config_id <= 0 ||
      !get_configs(display, NULL, 0, &config_count) || config_count <= 0 ||
      config_count > (int)NXGL_EGL_CONFIG_MAX ||
      !get_configs(display, configs, config_count, &config_count) ||
      config_count <= 0 || config_count > (int)NXGL_EGL_CONFIG_MAX) {
    nxgl_copy_string(error, error_size, "egl-current-config-unavailable");
    return NXGL_ERROR_NO_GLES_CONFIG;
  }
  for (index = 0; index < config_count; ++index) {
    value = -1;
    if (get_config_attrib(display, configs[index], NXGL_EGL_CONFIG_ID,
                          &value) &&
        value == config_id) {
      config = configs[index];
      break;
    }
  }
  if (!config) {
    nxgl_copy_string(error, error_size, "egl-delivered-config-not-found");
    return NXGL_ERROR_NO_GLES_CONFIG;
  }
  memset(actual, 0, sizeof(*actual));
  gl_version = (const char *)get_string(NXGL_GL_VERSION);
  if (nxgl_parse_gles_version(gl_version, &actual->gles_major,
                              &actual->gles_minor) != NXGL_SUCCESS) {
    nxgl_copy_string(error, error_size, "desktop-gl-context");
    return NXGL_ERROR_STACK_MISMATCH;
  }
#define NXGL_V2_EGL_ACTUAL(field, attribute)                                \
  do {                                                                       \
    if (nxgl_v2_sdl_egl_attribute(get_config_attrib, display, config,        \
                                  attribute, &actual->field) != 0) {         \
      nxgl_copy_string(error, error_size, "egl-config-attribute-failed");  \
      return NXGL_ERROR_NO_GLES_CONFIG;                                      \
    }                                                                        \
  } while (0)
  NXGL_V2_EGL_ACTUAL(red_bits, NXGL_EGL_RED_SIZE);
  NXGL_V2_EGL_ACTUAL(green_bits, NXGL_EGL_GREEN_SIZE);
  NXGL_V2_EGL_ACTUAL(blue_bits, NXGL_EGL_BLUE_SIZE);
  NXGL_V2_EGL_ACTUAL(alpha_bits, NXGL_EGL_ALPHA_SIZE);
  NXGL_V2_EGL_ACTUAL(depth_bits, NXGL_EGL_DEPTH_SIZE);
  NXGL_V2_EGL_ACTUAL(stencil_bits, NXGL_EGL_STENCIL_SIZE);
#undef NXGL_V2_EGL_ACTUAL
  actual->profile_mask = 0;
  if (SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                          &actual->profile_mask) != 0 ||
      actual->profile_mask != SDL_GL_CONTEXT_PROFILE_ES) {
    nxgl_copy_string(error, error_size, "delivered-profile-is-not-gles");
    return NXGL_ERROR_STACK_MISMATCH;
  }
  value = -1;
  if (!query_surface(display, surface, NXGL_EGL_RENDER_BUFFER, &value)) {
    nxgl_copy_string(error, error_size, "egl-render-buffer-query-failed");
    return NXGL_ERROR_NO_GLES_CONFIG;
  }
  actual->double_buffer = value == NXGL_EGL_BACK_BUFFER ? 1 : 0;

  egl->display = (uintptr_t)display;
  egl->context = (uintptr_t)context;
  egl->surface = (uintptr_t)surface;
  egl->config = (uintptr_t)config;
  egl->config_id = config_id;
  if (!query_surface(display, surface, NXGL_EGL_WIDTH, &egl->surface_width) ||
      !query_surface(display, surface, NXGL_EGL_HEIGHT,
                     &egl->surface_height) ||
      nxgl_v2_sdl_egl_attribute(get_config_attrib, display, config,
                                NXGL_EGL_RENDERABLE_TYPE,
                                &egl->renderable_type) != 0 ||
      nxgl_v2_sdl_egl_attribute(get_config_attrib, display, config,
                                NXGL_EGL_SURFACE_TYPE,
                                &egl->surface_type) != 0) {
    nxgl_copy_string(error, error_size, "egl-surface-query-failed");
    return NXGL_ERROR_NO_GLES_CONFIG;
  }
  egl->red_bits = actual->red_bits;
  egl->green_bits = actual->green_bits;
  egl->blue_bits = actual->blue_bits;
  egl->alpha_bits = actual->alpha_bits;
  egl->depth_bits = actual->depth_bits;
  egl->stencil_bits = actual->stencil_bits;
  egl_vendor = query_string(display, NXGL_EGL_VENDOR);
  egl_version = query_string(display, NXGL_EGL_VERSION);
  egl_client_apis = query_string(display, NXGL_EGL_CLIENT_APIS);
  if (!egl_vendor || !egl_version || !egl_client_apis || !*egl_vendor ||
      !*egl_version || !*egl_client_apis ||
      !nxgl_v2_string_bounded(egl_vendor, sizeof(egl->vendor) - 1u) ||
      !nxgl_v2_string_bounded(egl_version, sizeof(egl->version) - 1u) ||
      !nxgl_v2_string_bounded(egl_client_apis,
                              sizeof(egl->client_apis) - 1u)) {
    nxgl_copy_string(error, error_size, "egl-identity-query-failed");
    return NXGL_ERROR_NO_GLES_CONFIG;
  }
  nxgl_copy_string(egl->vendor, sizeof(egl->vendor), egl_vendor);
  nxgl_copy_string(egl->version, sizeof(egl->version), egl_version);
  nxgl_copy_string(egl->client_apis, sizeof(egl->client_apis),
                   egl_client_apis);
  egl->observed = 1;
  handles->egl_display = egl->display;
  handles->egl_context = egl->context;
  handles->egl_surface = egl->surface;
  handles->egl_config = egl->config;
  return NXGL_SUCCESS;
}

static int nxgl_v2_sdl_query_surface(
    void *userdata, const nxgl_stack_handles_v2 *handles, int *window_width,
    int *window_height, int *drawable_width, int *drawable_height, char *error,
    size_t error_size) {
  (void)userdata;
  if (!handles || !handles->sdl_window || !window_width || !window_height ||
      !drawable_width || !drawable_height || !error || error_size == 0)
    return NXGL_ERROR_INVALID_ARGUMENT;
  SDL_GetWindowSize(handles->sdl_window, window_width, window_height);
  SDL_GL_GetDrawableSize(handles->sdl_window, drawable_width, drawable_height);
  return NXGL_SUCCESS;
}

static int nxgl_v2_sdl_validate_current(
    void *userdata, const nxgl_stack_handles_v2 *handles, char *error,
    size_t error_size) {
  (void)userdata;
  if (!handles || !error || error_size == 0)
    return NXGL_ERROR_INVALID_ARGUMENT;
  if (SDL_GL_GetCurrentWindow() != handles->sdl_window ||
      SDL_GL_GetCurrentContext() != handles->sdl_context) {
    nxgl_copy_string(error, error_size, "current-context-mismatch");
    return NXGL_ERROR_STACK_MISMATCH;
  }
  return NXGL_SUCCESS;
}

static int nxgl_v2_sdl_release_attempt(
    void *userdata, nxgl_stack_handles_v2 *handles, char *error,
    size_t error_size) {
  nxgl_stack_owner_v2 owner;
  (void)userdata;
  if (!handles || !error || error_size == 0)
    return NXGL_ERROR_INVALID_ARGUMENT;
  owner = handles->owner;
  if (handles->sdl_context)
    SDL_GL_DeleteContext(handles->sdl_context);
  if (handles->sdl_window)
    SDL_DestroyWindow(handles->sdl_window);
  nxgl_v2_handles_init(handles, owner);
  return NXGL_SUCCESS;
}

static int nxgl_v2_sdl_present(void *userdata,
                               const nxgl_stack_handles_v2 *handles,
                               char *error, size_t error_size) {
  (void)userdata;
  if (!handles || !handles->sdl_window || !error || error_size == 0)
    return NXGL_ERROR_INVALID_ARGUMENT;
  SDL_GL_SwapWindow(handles->sdl_window);
  return NXGL_SUCCESS;
}

void nxgl_stack_ops_v2_init_sdl(nxgl_stack_ops_v2 *ops) {
  if (!ops)
    return;
  memset(ops, 0, sizeof(*ops));
  ops->api_version = NXGL_API_VERSION_V2;
  ops->struct_size = sizeof(*ops);
  ops->owner = NXGL_STACK_OWNER_V2_SDL_EGL;
  ops->start_video = nxgl_v2_sdl_start_video;
  ops->stop_video = nxgl_v2_sdl_stop_video;
  ops->set_config = nxgl_v2_sdl_set_config;
  ops->create_window = nxgl_v2_sdl_create_window;
  ops->create_context = nxgl_v2_sdl_create_context;
  ops->make_current = nxgl_v2_sdl_make_current;
  ops->get_proc_address = nxgl_v2_sdl_get_proc;
  ops->query_actual = nxgl_v2_sdl_query_actual;
  ops->query_surface = nxgl_v2_sdl_query_surface;
  ops->validate_current = nxgl_v2_sdl_validate_current;
  ops->release_attempt = nxgl_v2_sdl_release_attempt;
  ops->present = nxgl_v2_sdl_present;
}
#endif /* !NXGL_M13_TESTING */

void nxgl_open_options_v2_init(nxgl_open_options_v2 *options) {
  if (!options)
    return;
  memset(options, 0, sizeof(*options));
  options->api_version = NXGL_API_VERSION_V2;
  options->struct_size = sizeof(*options);
  options->flags = NXGL_OPEN_INITIALIZE_VIDEO |
                   NXGL_OPEN_RETRY_GLES_HINT_AFTER_DESKTOP |
                   NXGL_OPEN_RETRY_AUTODETECT_AFTER_REAL_FAILURE;
  options->window_title = "nxgl";
  options->window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN |
                          SDL_WINDOW_FULLSCREEN_DESKTOP;
  options->display_index = 0;
  options->drawable_wait_ms = 300u;
}

static int nxgl_v2_restore_transaction(nxgl_context *context,
                                       unsigned round_index) {
  int failed = 0;
  if (context->stack_is_default_sdl &&
      nxgl_restore_changed_hints(context) != 0) {
    context->report_v2.hints_restored = 0;
    nxgl_v2_record(context, round_index, 0, (size_t)-1,
                   NXGL_OPEN_STAGE_V2_HINT_RESTORE,
                   NXGL_OPEN_REASON_V2_RESTORE_FAILED,
                   NXGL_ERROR_ROLLBACK);
    failed = 1;
  }
  if (context->video_environment_cleared &&
      nxgl_restore_video_environment(context) != 0) {
    context->report_v2.environment_restored = 0;
    nxgl_v2_record(context, round_index, 0, (size_t)-1,
                   NXGL_OPEN_STAGE_V2_ENVIRONMENT_RESTORE,
                   NXGL_OPEN_REASON_V2_RESTORE_FAILED,
                   NXGL_ERROR_ROLLBACK);
    failed = 1;
  } else {
    context->report_v2.environment_restored = 1;
  }
  return failed ? NXGL_ERROR_ROLLBACK : NXGL_SUCCESS;
}

static void nxgl_v2_free_snapshots(nxgl_context *context) {
  nxgl_free_video_environment(context);
  nxgl_free_hint_snapshots(context);
}

static int nxgl_v2_cleanup_resources(nxgl_context *context,
                                     unsigned round_index) {
  if ((context->stack_cleanup_pending ||
       !nxgl_v2_handles_empty(&context->stack_handles)) &&
      nxgl_v2_release_attempt(context, round_index, 0, (size_t)-1) !=
          NXGL_SUCCESS)
    return NXGL_ERROR_ROLLBACK;
  if (nxgl_v2_stop_video(context, round_index) != NXGL_SUCCESS)
    return NXGL_ERROR_ROLLBACK;
  if (nxgl_v2_restore_transaction(context, round_index) != NXGL_SUCCESS)
    return NXGL_ERROR_ROLLBACK;
  return NXGL_SUCCESS;
}

int nxgl_open_v2(const nxgl_open_options_v2 *options,
                 nxgl_context **context_output, nxgl_report_v2 *report) {
  nxgl_context *context = NULL;
  nxgl_report_v2 result_report;
  nxgl_resolution_sources sources;
  nxgl_status_callback status_callback = NULL;
  void *status_userdata = NULL;
  char backend[NXGL_NAME_MAX];
  int inherited_retry_available = 0;
  int initial_video_owned = 0;
  int status;
  int cleanup_incomplete = 0;
  int retry = 0;
  unsigned round_index = 0;

  /* Acquire before validating any caller-owned object that could cause an
   * error output to be published.  Reentry is therefore unconditionally
   * byte-atomic, including malformed nested options. */
  if (!nxgl_arbiter_try_acquire())
    return NXGL_ERROR_BUSY;
  if (!context_output || !nxgl_v2_options_valid(options)) {
    if (context_output)
      *context_output = NULL;
    if (report) {
      nxgl_v2_report_init(report);
      report->final_stage = NXGL_OPEN_STAGE_V2_NONE;
      report->final_reason = NXGL_OPEN_REASON_V2_INVALID_OPTIONS;
    }
    nxgl_arbiter_release();
    return NXGL_ERROR_INVALID_ARGUMENT;
  }

  context = (nxgl_context *)nxgl_v2_context_allocate();
  if (!context) {
    *context_output = NULL;
    if (report) {
      nxgl_v2_report_init(report);
      report->final_reason = NXGL_OPEN_REASON_V2_OUT_OF_MEMORY;
    }
    nxgl_arbiter_release();
    return NXGL_ERROR_OUT_OF_MEMORY;
  }
  context->open_api_version = NXGL_API_VERSION_V2;
  context->status = options->status;
  context->status_userdata = options->status_userdata;
  status_callback = options->status;
  status_userdata = options->status_userdata;
  nxgl_v2_report_init(&context->report_v2);
  nxgl_v2_handles_init(&context->stack_handles,
                       NXGL_STACK_OWNER_V2_NONE);

  if (options->stack_ops) {
    context->stack_ops = *options->stack_ops;
    context->stack_is_default_sdl = 0;
  } else {
#if defined(NXGL_M13_TESTING)
    /* The sealed fake build rejects implicit production providers. */
    status = NXGL_ERROR_INVALID_ARGUMENT;
    nxgl_v2_record(context, 0, 0, (size_t)-1, NXGL_OPEN_STAGE_V2_NONE,
                   NXGL_OPEN_REASON_V2_INVALID_OPTIONS, status);
    goto fail;
#else
    nxgl_stack_ops_v2_init_sdl(&context->stack_ops);
    context->stack_is_default_sdl = 1;
#endif
  }
  context->stack_owner = context->stack_ops.owner;
  context->stack_userdata = options->stack_userdata;
  nxgl_v2_handles_init(&context->stack_handles, context->stack_owner);
  context->report_v2.stack_owner = context->stack_owner;
  context->report_v2.handles.owner = context->stack_owner;

  if (context->stack_is_default_sdl) {
    if (nxgl_save_video_environment(context) != 0) {
      status = NXGL_ERROR_ROLLBACK;
      context->report_v2.environment_restored = 0;
      nxgl_v2_record(context, 0, 0, (size_t)-1,
                     NXGL_OPEN_STAGE_V2_ENVIRONMENT_SNAPSHOT,
                     NXGL_OPEN_REASON_V2_SNAPSHOT_FAILED, status);
      goto fail;
    }
    inherited_retry_available = nxgl_has_inherited_video_hint(context);
#if !defined(NXGL_M13_TESTING)
    if (nxgl_save_hint_snapshots(context) != 0) {
      status = NXGL_ERROR_ROLLBACK;
      context->report_v2.hints_restored = 0;
      nxgl_v2_record(context, 0, 0, (size_t)-1,
                     NXGL_OPEN_STAGE_V2_HINT_SNAPSHOT,
                     NXGL_OPEN_REASON_V2_SNAPSHOT_FAILED, status);
      goto fail;
    }
#endif
  }

  status = nxgl_v2_start_video(context, options, &sources, backend,
                               sizeof(backend), round_index);
  initial_video_owned = context->stack_video_initialized;
  if (status == NXGL_SUCCESS)
    status = nxgl_v2_run_round(context, options, &sources, backend,
                               round_index);
  if (status == NXGL_SUCCESS)
    goto selected;
  if (status == NXGL_ERROR_ROLLBACK)
    goto fail;
  if (nxgl_v2_stop_video(context, round_index) != NXGL_SUCCESS) {
    status = NXGL_ERROR_ROLLBACK;
    goto fail;
  }

  retry = context->stack_is_default_sdl && initial_video_owned &&
          inherited_retry_available &&
          (options->flags &
           NXGL_OPEN_RETRY_AUTODETECT_AFTER_REAL_FAILURE) != 0;
  if (!retry)
    goto fail;
  round_index = 1u;
  if (nxgl_clear_video_environment(context) != 0) {
    status = NXGL_ERROR_ROLLBACK;
    context->report_v2.environment_restored = 0;
    nxgl_v2_record(context, round_index, 0, (size_t)-1,
                   NXGL_OPEN_STAGE_V2_ENVIRONMENT_RESTORE,
                   NXGL_OPEN_REASON_V2_RESTORE_FAILED, status);
    goto fail;
  }
  context->report_v2.environment_restored = 0;
  status = nxgl_v2_start_video(context, options, &sources, backend,
                               sizeof(backend), round_index);
  if (nxgl_restore_video_environment(context) != 0) {
    context->report_v2.environment_restored = 0;
    nxgl_v2_record(context, round_index, 0, (size_t)-1,
                   NXGL_OPEN_STAGE_V2_ENVIRONMENT_RESTORE,
                   NXGL_OPEN_REASON_V2_RESTORE_FAILED,
                   NXGL_ERROR_ROLLBACK);
    status = NXGL_ERROR_ROLLBACK;
    goto fail;
  }
  context->report_v2.environment_restored = 1;
  if (status == NXGL_SUCCESS)
    status = nxgl_v2_run_round(context, options, &sources, backend,
                               round_index);
  if (status == NXGL_SUCCESS) {
    context->report_v2.legacy.used_video_autodetect_recovery = 1;
    goto selected;
  }
  if (status != NXGL_ERROR_ROLLBACK &&
      nxgl_v2_stop_video(context, round_index) != NXGL_SUCCESS)
    status = NXGL_ERROR_ROLLBACK;
  goto fail;

selected:
  if (nxgl_v2_restore_transaction(context, round_index) != NXGL_SUCCESS) {
    status = NXGL_ERROR_ROLLBACK;
    goto fail;
  }
  context->report_v2.handles = context->stack_handles;
  context->report_v2.stack_owner = context->stack_owner;
  context->report = context->report_v2.legacy;
  result_report = context->report_v2;
  nxgl_v2_free_snapshots(context);
  /* The terminal callback is part of the open transaction.  Keep the
   * process arbiter held and both caller outputs unpublished so a callback
   * cannot close the selected context, replace an aliased output through a
   * nested open, or observe a cleanup handle before open returns. */
  if (status_callback)
    status_callback(status_userdata, NXGL_STATUS_SELECTED,
                    "nxgl-v2 graphics stack selected");
  *context_output = context;
  if (report)
    *report = result_report;
  nxgl_arbiter_release();
  return NXGL_SUCCESS;

fail:
  if (context && nxgl_v2_cleanup_resources(context, round_index) !=
                     NXGL_SUCCESS) {
    status = NXGL_ERROR_ROLLBACK;
    cleanup_incomplete = 1;
  }
  if (context)
    result_report = context->report_v2;
  else
    nxgl_v2_report_init(&result_report);
  if (!cleanup_incomplete) {
    if (context) {
      nxgl_v2_free_snapshots(context);
      free(context);
      context = NULL;
    }
  }
  if (status_callback)
    status_callback(status_userdata, NXGL_STATUS_ERROR,
                    "nxgl-v2 graphics stack unavailable");
  if (cleanup_incomplete) {
    /* A rollback failure retains the opaque context so the caller can retry
     * nxgl_close_v2; silently freeing provider-owned handles would be unsafe. */
    *context_output = context;
  } else {
    *context_output = NULL;
  }
  if (report)
    *report = result_report;
  nxgl_arbiter_release();
  return status;
}

static int nxgl_close_v2_locked(nxgl_context *context) {
  int status;
  if (context->open_api_version != NXGL_API_VERSION_V2)
    return NXGL_ERROR_INVALID_ARGUMENT;
  status = nxgl_v2_cleanup_resources(context, 0u);
  if (status == NXGL_SUCCESS) {
    nxgl_v2_free_snapshots(context);
    free(context);
  }
  return status;
}

int nxgl_close_v2(nxgl_context *context) {
  int status;
  if (!context)
    return NXGL_ERROR_INVALID_ARGUMENT;
  if (!nxgl_arbiter_try_acquire())
    return NXGL_ERROR_BUSY;
  status = nxgl_close_v2_locked(context);
  nxgl_arbiter_release();
  return status;
}

nxgl_stack_owner_v2 nxgl_stack_owner(const nxgl_context *context) {
  if (!context || context->open_api_version != NXGL_API_VERSION_V2)
    return NXGL_STACK_OWNER_V2_NONE;
  return context->stack_owner;
}

const nxgl_report_v2 *nxgl_get_report_v2(const nxgl_context *context) {
  if (!context || context->open_api_version != NXGL_API_VERSION_V2)
    return NULL;
  return &context->report_v2;
}


#endif /* NXGL_CORE_TESTING */
