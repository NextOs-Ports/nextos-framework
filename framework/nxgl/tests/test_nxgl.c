/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxgl.h"
#include "nxgl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int present_adapter_calls;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                             \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static void test_initializers_do_not_touch_backend(void) {
  nxgl_open_options options;
  nxgl_engine_requirements requirements;
  nxgl_present_policy present;
  const char *before;
  (void)setenv("SDL_VIDEODRIVER", "inherited-test-driver", 1);
  before = getenv("SDL_VIDEODRIVER");
  nxgl_open_options_init(&options);
  nxgl_engine_requirements_init(&requirements);
  nxgl_present_policy_init(&present);
  CHECK(before != NULL);
  CHECK(strcmp(getenv("SDL_VIDEODRIVER"), "inherited-test-driver") == 0);
  CHECK(options.api_version == NXGL_API_VERSION);
  CHECK((options.flags & NXGL_OPEN_RETRY_AUTODETECT_AFTER_REAL_FAILURE) != 0);
  CHECK(requirements.minimum_gles_major == 2);
  CHECK(requirements.maximum_gles_major == 0);
  CHECK(requirements.minimum_red_bits == 8);
  CHECK(requirements.require_double_buffer == 0);
  CHECK(present.owner == NXGL_PRESENT_ENGINE_OWNED);
  CHECK(present.flags == 0);
  (void)unsetenv("SDL_VIDEODRIVER");
}

static void test_resolution_order(void) {
  nxgl_resolution_sources sources;
  nxgl_resolution resolution;
  nxgl_resolution_sources_init(&sources);
  sources.sdl_desktop_width = 640;
  sources.sdl_desktop_height = 480;
  sources.sdl_current_width = 800;
  sources.sdl_current_height = 600;
  sources.sdl_bounds_width = 1024;
  sources.sdl_bounds_height = 768;
  sources.drm_width = 1280;
  sources.drm_height = 720;
  sources.fbdev_width = 1920;
  sources.fbdev_height = 1080;
  CHECK(nxgl_choose_resolution(&sources, &resolution) == NXGL_SUCCESS);
  CHECK(resolution.source == NXGL_RESOLUTION_SDL_DESKTOP);
  CHECK(resolution.width == 640 && resolution.height == 480);

  sources.sdl_desktop_width = 0;
  sources.sdl_desktop_height = 0;
  CHECK(nxgl_choose_resolution(&sources, &resolution) == NXGL_SUCCESS);
  CHECK(resolution.source == NXGL_RESOLUTION_SDL_CURRENT);
  sources.sdl_current_width = 0;
  sources.sdl_current_height = 0;
  CHECK(nxgl_choose_resolution(&sources, &resolution) == NXGL_SUCCESS);
  CHECK(resolution.source == NXGL_RESOLUTION_SDL_BOUNDS);
  sources.sdl_bounds_width = 0;
  sources.sdl_bounds_height = 0;
  CHECK(nxgl_choose_resolution(&sources, &resolution) == NXGL_SUCCESS);
  CHECK(resolution.source == NXGL_RESOLUTION_DRM_FACT);
  sources.drm_width = 0;
  sources.drm_height = 0;
  CHECK(nxgl_choose_resolution(&sources, &resolution) == NXGL_SUCCESS);
  CHECK(resolution.source == NXGL_RESOLUTION_FBDEV_FACT);
  sources.fbdev_width = 0;
  sources.fbdev_height = 0;
  CHECK(nxgl_choose_resolution(&sources, &resolution) ==
        NXGL_ERROR_RESOLUTION_UNAVAILABLE);
}

static void test_gles_and_ladder_policy(void) {
  nxgl_engine_requirements requirements;
  nxgl_config_candidate gles2 = {2, 0, 8, 8, 8, 8, 24, 8, 1};
  nxgl_config_candidate no_alpha = {2, 0, 8, 8, 8, 0, 24, 8, 1};
  nxgl_config_candidate gles3 = {3, 0, 8, 8, 8, 8, 24, 8, 1};
  nxgl_config_actual actual;
  char error[NXGL_DETAIL_MAX];
  int major = 0;
  int minor = 0;
  nxgl_engine_requirements_init(&requirements);
  requirements.maximum_gles_major = 2;
  requirements.minimum_alpha_bits = 8;
  requirements.minimum_depth_bits = 16;
  CHECK(nxgl_parse_gles_version("OpenGL ES 2.0 Mali-450 MP", &major, &minor) ==
        NXGL_SUCCESS);
  CHECK(major == 2 && minor == 0);
  CHECK(nxgl_parse_gles_version("OpenGL ES-CM 1.1", &major, &minor) ==
        NXGL_SUCCESS);
  CHECK(major == 1 && minor == 1);
  CHECK(nxgl_parse_gles_version("4.6 (Core Profile) Mesa", &major, &minor) ==
        NXGL_ERROR_NO_GLES_CONFIG);
  CHECK(nxgl_candidate_compatible(&requirements, &gles2));
  CHECK(!nxgl_candidate_compatible(&requirements, &no_alpha));
  CHECK(!nxgl_candidate_compatible(&requirements, &gles3));

  memset(&actual, 0, sizeof(actual));
  actual.gles_major = 2;
  actual.red_bits = 8;
  actual.green_bits = 8;
  actual.blue_bits = 8;
  actual.alpha_bits = 8;
  actual.depth_bits = 24;
  actual.stencil_bits = 8;
  actual.double_buffer = 1;
  CHECK(nxgl_validate_actual(&requirements, &actual, error, sizeof(error)) ==
        NXGL_SUCCESS);
  actual.double_buffer = 0;
  CHECK(nxgl_validate_actual(&requirements, &actual, error, sizeof(error)) ==
        NXGL_SUCCESS);
  actual.alpha_bits = 0;
  CHECK(nxgl_validate_actual(&requirements, &actual, error, sizeof(error)) ==
        NXGL_ERROR_NO_GLES_CONFIG);
  CHECK(strstr(error, "alpha bits") != NULL);
  actual.alpha_bits = 8;
  requirements.require_double_buffer = 1;
  actual.double_buffer = 0;
  CHECK(nxgl_validate_actual(&requirements, &actual, error, sizeof(error)) ==
        NXGL_ERROR_NO_GLES_CONFIG);
  CHECK(strstr(error, "single-buffer") != NULL);
}

static void test_event_pump_preserves_application_queue(void) {
  SDL_Event sent;
  SDL_Event received;
  CHECK(SDL_InitSubSystem(SDL_INIT_EVENTS) == 0);
  SDL_FlushEvent(SDL_USEREVENT);
  memset(&sent, 0, sizeof(sent));
  sent.type = SDL_USEREVENT;
  sent.user.code = 0x4e58474c;
  CHECK(SDL_PushEvent(&sent) == 1);
  nxgl_pump_events_without_consuming();
  memset(&received, 0, sizeof(received));
  CHECK(SDL_PeepEvents(&received, 1, SDL_GETEVENT, SDL_USEREVENT,
                       SDL_USEREVENT) == 1);
  CHECK(received.user.code == sent.user.code);
  SDL_QuitSubSystem(SDL_INIT_EVENTS);
}

static SDL_GLContext partial_create_context(void *userdata,
                                             SDL_Window *window) {
  (void)userdata;
  (void)window;
  return NULL;
}

static void test_context_ops_are_atomic(void) {
  nxgl_config_candidate candidate = {2, 0, 8, 8, 8, 0, 0, 0, 1};
  nxgl_engine_requirements requirements;
  nxgl_context_ops context_ops;
  nxgl_open_options options;
  nxgl_context *context = NULL;
  nxgl_engine_requirements_init(&requirements);
  nxgl_open_options_init(&options);
  memset(&context_ops, 0, sizeof(context_ops));
  context_ops.api_version = NXGL_API_VERSION;
  context_ops.struct_size = sizeof(context_ops);
  context_ops.create_context = partial_create_context;
  options.requirements = &requirements;
  options.candidates = &candidate;
  options.candidate_count = 1;
  options.context_ops = &context_ops;
  CHECK(nxgl_open(&options, &context, NULL) == NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(context == NULL);
}

static void test_profile_format(void) {
  nxgl_report report;
  char line[512];
  memset(&report, 0, sizeof(report));
  report.api_version = NXGL_API_VERSION;
  report.struct_size = sizeof(report);
  report.actual.gles_major = 2;
  report.actual.gles_minor = 0;
  report.actual.red_bits = 8;
  report.actual.green_bits = 8;
  report.actual.blue_bits = 8;
  report.actual.alpha_bits = 8;
  report.actual.depth_bits = 24;
  report.actual.stencil_bits = 8;
  report.drawable_width = 1280;
  report.drawable_height = 720;
  (void)snprintf(report.video_backend, sizeof(report.video_backend), "%s",
                 "mali");
  (void)snprintf(report.renderer, sizeof(report.renderer), "%s",
                 "Mali-450 MP");
  CHECK(nxgl_format_profile_line(&report, line, sizeof(line)) > 0);
  CHECK(strstr(line, "video mali") != NULL);
  CHECK(strstr(line, "GLES 2.0") != NULL);
  CHECK(strstr(line, "drawable 1280x720") != NULL);
  CHECK(strstr(line, "rgba 8/8/8/8 d24 s8") != NULL);
}

static int test_present_adapter(void *userdata, SDL_Window *window,
                                SDL_GLContext context, char *error,
                                size_t error_size) {
  int *marker = (int *)userdata;
  (void)window;
  (void)context;
  (void)error;
  (void)error_size;
  ++*marker;
  return 0;
}

static void test_present_is_opt_in(void) {
  nxgl_context context;
  nxgl_present_policy policy;
  int handle_marker = 1;
  memset(&context, 0, sizeof(context));
  context.window = (SDL_Window *)(void *)&handle_marker;
  context.gl_context = (SDL_GLContext)(void *)&handle_marker;
  nxgl_present_policy_init(&policy);
  CHECK(nxgl_present(&context, &policy) == NXGL_NO_ACTION);
  CHECK(present_adapter_calls == 0);
  policy.owner = NXGL_PRESENT_ADAPTER;
  policy.adapter = test_present_adapter;
  policy.userdata = &present_adapter_calls;
  CHECK(nxgl_present(&context, &policy) == NXGL_SUCCESS);
  CHECK(present_adapter_calls == 1);
  policy.adapter = NULL;
  CHECK(nxgl_present(&context, &policy) == NXGL_ERROR_INVALID_ARGUMENT);
}

int main(void) {
  test_initializers_do_not_touch_backend();
  test_resolution_order();
  test_gles_and_ladder_policy();
  test_event_pump_preserves_application_queue();
  test_context_ops_are_atomic();
  test_profile_format();
  test_present_is_opt_in();
  if (failures) {
    (void)fprintf(stderr, "%d nxgl test(s) failed\n", failures);
    return 1;
  }
  (void)fprintf(stdout, "nxgl pure policy tests passed\n");
  return 0;
}
