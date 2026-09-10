/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxgl.h"
#include "nxgl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int failures;

typedef struct fake_hint_value {
  int existed;
  char value[NXGL_HINT_VALUE_MAX + 2u];
} fake_hint_value;

static fake_hint_value fake_gles_hint;
static fake_hint_value fake_x11_hint;
static int fake_fail_gles_hint_set;
static int fake_fail_x11_hint_set;
static int fake_reset_hint_available = 1;
static const char *fake_hint_backend = "x11";

static fake_hint_value *fake_hint_for_name(const char *name) {
  if (strcmp(name, NXGL_HINT_OPENGL_ES_DRIVER) == 0)
    return &fake_gles_hint;
  if (strcmp(name, NXGL_HINT_VIDEO_X11_FORCE_EGL) == 0)
    return &fake_x11_hint;
  return NULL;
}

const char *nxgl_test_sdl_get_hint(const char *name) {
  fake_hint_value *hint = fake_hint_for_name(name);
  return hint && hint->existed ? hint->value : NULL;
}

SDL_bool nxgl_test_sdl_set_hint(const char *name, const char *value) {
  fake_hint_value *hint = fake_hint_for_name(name);
  size_t length;
  if (!hint || !value)
    return SDL_FALSE;
  if ((hint == &fake_gles_hint && fake_fail_gles_hint_set) ||
      (hint == &fake_x11_hint && fake_fail_x11_hint_set))
    return SDL_FALSE;
  length = strnlen(value, sizeof(hint->value));
  if (length >= sizeof(hint->value))
    return SDL_FALSE;
  memcpy(hint->value, value, length + 1u);
  hint->existed = 1;
  return SDL_TRUE;
}

void nxgl_test_sdl_reset_hint(const char *name) {
  fake_hint_value *hint = fake_hint_for_name(name);
  if (hint) {
    memset(hint, 0, sizeof(*hint));
  }
}

int nxgl_test_sdl_reset_hint_available(void) {
  return fake_reset_hint_available;
}

const char *nxgl_test_sdl_current_video_driver(void) {
  return fake_hint_backend;
}

int nxgl_test_sdl_strcasecmp(const char *left, const char *right) {
  return strcasecmp(left, right);
}

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                    #condition);                                             \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

typedef enum fake_mode {
  FAKE_SUCCESS_LADDER = 0,
  FAKE_SUCCESS_DIRECT,
  FAKE_START_FAILURE,
  FAKE_BAD_RETURN,
  FAKE_PROVIDER_BUSY,
  FAKE_BAD_BACKEND,
  FAKE_BAD_CONFIG_ID,
  FAKE_BAD_EGL_BITS,
  FAKE_BAD_ACTUAL,
  FAKE_BAD_EGL_STRING,
  FAKE_UNTERMINATED_ERROR,
  FAKE_PRIVATE_ERROR,
  FAKE_RELEASE_FAIL_ONCE,
  FAKE_STOP_FAIL_ONCE,
  FAKE_ATTEMPT_AND_RELEASE_FAIL_ONCE,
  FAKE_CONTEXT_FAILURE,
  FAKE_PRE_CURRENT_FAIL,
  FAKE_FINAL_CURRENT_FAIL
} fake_mode;

typedef struct fake_stack {
  fake_mode mode;
  const nxgl_open_options_v2 *reentry_options;
  nxgl_config_candidate requested;
  unsigned start_calls;
  unsigned stop_calls;
  unsigned set_config_calls;
  unsigned window_calls;
  unsigned context_calls;
  unsigned current_calls;
  unsigned get_proc_calls;
  unsigned gl_string_calls;
  unsigned actual_calls;
  unsigned surface_calls;
  unsigned validate_calls;
  unsigned release_calls;
  unsigned present_calls;
  int actual_gles_major;
  int renderable_type_override;
  int current_matches;
  int reentry_checked;
  char order[128];
  size_t order_size;
} fake_stack;

static fake_stack *current_fake;

static void fake_mark(fake_stack *fake, char marker) {
  if (fake->order_size + 1u < sizeof(fake->order)) {
    fake->order[fake->order_size++] = marker;
    fake->order[fake->order_size] = '\0';
  }
}

static nxgl_proc_v2 fake_proc(nxgl_gl_get_string_fn function) {
  nxgl_proc_v2 generic = NULL;
  CHECK(sizeof(generic) == sizeof(function));
  if (sizeof(generic) == sizeof(function))
    memcpy(&generic, &function, sizeof(generic));
  return generic;
}

static const unsigned char *fake_gl_get_string(unsigned int name) {
  if (current_fake)
    ++current_fake->gl_string_calls;
  switch (name) {
  case 0x1F00u:
    return (const unsigned char *)
        "Vendor /home/alice/private token=ZXCV123 192.0.2.123\n";
  case 0x1F01u:
    return (const unsigned char *)"Fake renderer C:\\Users\\alice\\secret";
  case 0x1F02u:
    return current_fake && current_fake->actual_gles_major >= 3
               ? (const unsigned char *)"OpenGL ES 3.0 fake"
               : (const unsigned char *)"OpenGL ES 2.0 fake";
  case 0x8B8Cu:
    return (const unsigned char *)"OpenGL ES GLSL ES 1.00";
  case 0x1F03u:
    return (const unsigned char *)"GL_EXT_fake token=PRIVATE_TOKEN";
  default:
    return (const unsigned char *)"";
  }
}

static void fake_check_reentry(fake_stack *fake) {
  nxgl_context *nested = (nxgl_context *)(uintptr_t)0x1234u;
  nxgl_report_v2 report_before;
  nxgl_report_v2 report_after;
  if (!fake->reentry_options || fake->reentry_checked)
    return;
  fake->reentry_checked = 1;
  memset(&report_before, 0xa5, sizeof(report_before));
  memcpy(&report_after, &report_before, sizeof(report_after));
  CHECK(nxgl_open_v2(fake->reentry_options, &nested, &report_after) ==
        NXGL_ERROR_BUSY);
  CHECK(nested == (nxgl_context *)(uintptr_t)0x1234u);
  CHECK(memcmp(&report_before, &report_after, sizeof(report_before)) == 0);
}

static int fake_start_video(
    void *userdata, int allow_initialize, int display_index,
    const nxgl_resolution_sources *fallback_facts,
    nxgl_resolution_sources *sources, char *backend, size_t backend_size,
    int *initialized_by_stack, char *error, size_t error_size) {
  fake_stack *fake = (fake_stack *)userdata;
  (void)display_index;
  (void)fallback_facts;
  (void)error;
  (void)error_size;
  ++fake->start_calls;
  fake_mark(fake, 'S');
  fake_check_reentry(fake);
  *initialized_by_stack = 0;
  nxgl_resolution_sources_init(sources);
  sources->sdl_desktop_width = 1280;
  sources->sdl_desktop_height = 720;
  if (fake->mode == FAKE_BAD_BACKEND)
    (void)snprintf(backend, backend_size, "%s",
                   "/home/alice 192.0.2.123\ntoken=ZXCV123");
  else
    (void)snprintf(backend, backend_size, "%s", "fake-raw-egl");
  CHECK(allow_initialize == 1);
  return fake->mode == FAKE_START_FAILURE ? NXGL_ERROR_VIDEO_UNAVAILABLE
                                          : NXGL_SUCCESS;
}

static int fake_stop_video(void *userdata, int initialized_by_stack,
                           char *error, size_t error_size) {
  fake_stack *fake = (fake_stack *)userdata;
  (void)error;
  (void)error_size;
  CHECK(initialized_by_stack == 0);
  ++fake->stop_calls;
  fake_mark(fake, 'T');
  if (fake->mode == FAKE_STOP_FAIL_ONCE && fake->stop_calls == 1u)
    return NXGL_ERROR_ROLLBACK;
  return NXGL_SUCCESS;
}

static int fake_set_config(void *userdata,
                           const nxgl_config_candidate *candidate,
                           char *error, size_t error_size) {
  fake_stack *fake = (fake_stack *)userdata;
  ++fake->set_config_calls;
  fake_mark(fake, 'A');
  fake->requested = *candidate;
  if (fake->mode == FAKE_BAD_RETURN)
    return 42;
  if (fake->mode == FAKE_PROVIDER_BUSY)
    return NXGL_ERROR_BUSY;
  if (fake->mode == FAKE_UNTERMINATED_ERROR) {
    memset(error, 'X', error_size);
    return NXGL_ERROR_NO_GLES_CONFIG;
  }
  if (fake->mode == FAKE_PRIVATE_ERROR) {
    (void)snprintf(error, error_size, "%s",
                   "/home/alice/private 192.0.2.123\ntoken=ZXCV123");
    return NXGL_ERROR_NO_GLES_CONFIG;
  }
  if (fake->mode == FAKE_ATTEMPT_AND_RELEASE_FAIL_ONCE)
    return NXGL_ERROR_NO_GLES_CONFIG;
  return NXGL_SUCCESS;
}

static int fake_create_window(
    void *userdata, const nxgl_stack_attempt_request_v2 *request,
    nxgl_stack_handles_v2 *handles, char *error, size_t error_size) {
  fake_stack *fake = (fake_stack *)userdata;
  (void)error;
  (void)error_size;
  ++fake->window_calls;
  fake_mark(fake, 'W');
  CHECK(request->api_version == NXGL_API_VERSION_V2);
  CHECK(request->resolution.width == 1280);
  CHECK(request->resolution.height == 720);
  handles->native_display = (uintptr_t)0x1000u;
  handles->native_window = (uintptr_t)(0x2000u + fake->window_calls);
  return NXGL_SUCCESS;
}

static int fake_create_context(void *userdata, nxgl_stack_handles_v2 *handles,
                               char *error, size_t error_size) {
  fake_stack *fake = (fake_stack *)userdata;
  (void)error;
  (void)error_size;
  ++fake->context_calls;
  fake_mark(fake, 'C');
  if ((fake->mode == FAKE_SUCCESS_LADDER && fake->context_calls == 1u) ||
      fake->mode == FAKE_CONTEXT_FAILURE)
    return NXGL_ERROR_NO_GLES_CONFIG;
  handles->egl_display = (uintptr_t)0x3000u;
  handles->egl_context = (uintptr_t)(0x4000u + fake->context_calls);
  handles->egl_surface = (uintptr_t)(0x5000u + fake->context_calls);
  return NXGL_SUCCESS;
}

static int fake_make_current(void *userdata,
                             const nxgl_stack_handles_v2 *handles,
                             char *error, size_t error_size) {
  fake_stack *fake = (fake_stack *)userdata;
  (void)handles;
  (void)error;
  (void)error_size;
  ++fake->current_calls;
  fake_mark(fake, 'M');
  fake->current_matches = 1;
  current_fake = fake;
  return NXGL_SUCCESS;
}

static nxgl_proc_v2 fake_get_proc(void *userdata, const char *name) {
  fake_stack *fake = (fake_stack *)userdata;
  ++fake->get_proc_calls;
  if (strcmp(name, "glGetString") == 0)
    return fake_proc(fake_gl_get_string);
  return NULL;
}

static int fake_query_actual(
    void *userdata, nxgl_stack_handles_v2 *handles,
    nxgl_config_actual *actual, nxgl_egl_actual_v2 *egl, char *error,
    size_t error_size) {
  fake_stack *fake = (fake_stack *)userdata;
  (void)error;
  (void)error_size;
  ++fake->actual_calls;
  fake_mark(fake, 'Q');
  actual->gles_major =
      fake->actual_gles_major > 0 ? fake->actual_gles_major : 2;
  actual->gles_minor = 0;
  actual->red_bits = fake->requested.red_bits;
  actual->green_bits = fake->requested.green_bits;
  actual->blue_bits = fake->requested.blue_bits;
  actual->alpha_bits = fake->requested.alpha_bits;
  actual->depth_bits = fake->requested.depth_bits;
  actual->stencil_bits = fake->requested.stencil_bits;
  actual->double_buffer = fake->requested.double_buffer;
  actual->profile_mask = 4;
  if (fake->mode == FAKE_BAD_ACTUAL) {
    actual->red_bits = 99;
    actual->double_buffer = 2;
  }
  egl->observed = 1;
  egl->display = handles->egl_display;
  egl->context = handles->egl_context;
  egl->surface = handles->egl_surface;
  egl->config = (uintptr_t)0x6000u;
  egl->config_id = fake->mode == FAKE_BAD_CONFIG_ID ? 0 : 7;
  egl->surface_width = 1280;
  egl->surface_height = 720;
  egl->red_bits = actual->red_bits;
  egl->green_bits = actual->green_bits;
  egl->blue_bits = actual->blue_bits;
  egl->alpha_bits = actual->alpha_bits;
  egl->depth_bits = actual->depth_bits;
  egl->stencil_bits = actual->stencil_bits;
  if (fake->mode == FAKE_BAD_EGL_BITS)
    egl->renderable_type = 0x0008;
  else if (fake->renderable_type_override != 0)
    egl->renderable_type = fake->renderable_type_override;
  else
    egl->renderable_type = 0x0004;
  egl->surface_type = 4;
  if (fake->mode == FAKE_BAD_EGL_STRING)
    memset(egl->vendor, 'V', sizeof(egl->vendor));
  else
    (void)snprintf(egl->vendor, sizeof(egl->vendor), "%s",
                   "EGL /mnt/private/alice token=EGL_SECRET 192.0.2.123");
  (void)snprintf(egl->version, sizeof(egl->version), "%s", "1.4 fake");
  (void)snprintf(egl->client_apis, sizeof(egl->client_apis), "%s",
                 "OpenGL_ES");
  handles->egl_config = egl->config;
  if (fake->mode == FAKE_FINAL_CURRENT_FAIL)
    fake->current_matches = 0;
  return NXGL_SUCCESS;
}

static int fake_query_surface(
    void *userdata, const nxgl_stack_handles_v2 *handles, int *window_width,
    int *window_height, int *drawable_width, int *drawable_height, char *error,
    size_t error_size) {
  fake_stack *fake = (fake_stack *)userdata;
  (void)handles;
  (void)error;
  (void)error_size;
  ++fake->surface_calls;
  fake_mark(fake, 'D');
  *window_width = 1280;
  *window_height = 720;
  *drawable_width = 1280;
  *drawable_height = 720;
  return NXGL_SUCCESS;
}

static int fake_validate_current(void *userdata,
                                 const nxgl_stack_handles_v2 *handles,
                                 char *error, size_t error_size) {
  fake_stack *fake = (fake_stack *)userdata;
  (void)handles;
  (void)error;
  (void)error_size;
  ++fake->validate_calls;
  fake_mark(fake, 'V');
  if (fake->mode == FAKE_PRE_CURRENT_FAIL || !fake->current_matches)
    return NXGL_ERROR_STACK_MISMATCH;
  return NXGL_SUCCESS;
}

static int fake_release_attempt(void *userdata,
                                nxgl_stack_handles_v2 *handles, char *error,
                                size_t error_size) {
  fake_stack *fake = (fake_stack *)userdata;
  nxgl_stack_owner_v2 owner = handles->owner;
  (void)error;
  (void)error_size;
  ++fake->release_calls;
  fake_mark(fake, 'R');
  if ((fake->mode == FAKE_RELEASE_FAIL_ONCE && fake->release_calls == 1u) ||
      (fake->mode == FAKE_ATTEMPT_AND_RELEASE_FAIL_ONCE &&
       fake->release_calls <= 2u))
    return NXGL_ERROR_ROLLBACK;
  memset(handles, 0, sizeof(*handles));
  handles->api_version = NXGL_API_VERSION_V2;
  handles->struct_size = sizeof(*handles);
  handles->owner = owner;
  return NXGL_SUCCESS;
}

static int fake_present(void *userdata,
                        const nxgl_stack_handles_v2 *handles, char *error,
                        size_t error_size) {
  fake_stack *fake = (fake_stack *)userdata;
  (void)handles;
  (void)error;
  (void)error_size;
  ++fake->present_calls;
  return NXGL_SUCCESS;
}

static void fake_ops_init(nxgl_stack_ops_v2 *ops) {
  memset(ops, 0, sizeof(*ops));
  ops->api_version = NXGL_API_VERSION_V2;
  ops->struct_size = sizeof(*ops);
  ops->owner = NXGL_STACK_OWNER_V2_RAW_EGL;
  ops->start_video = fake_start_video;
  ops->stop_video = fake_stop_video;
  ops->set_config = fake_set_config;
  ops->create_window = fake_create_window;
  ops->create_context = fake_create_context;
  ops->make_current = fake_make_current;
  ops->get_proc_address = fake_get_proc;
  ops->query_actual = fake_query_actual;
  ops->query_surface = fake_query_surface;
  ops->validate_current = fake_validate_current;
  ops->release_attempt = fake_release_attempt;
  ops->present = fake_present;
}

static void fake_options_init(nxgl_open_options_v2 *options,
                              nxgl_stack_ops_v2 *ops, fake_stack *fake,
                              nxgl_engine_requirements *requirements,
                              nxgl_config_candidate candidates[2]) {
  nxgl_open_options_v2_init(options);
  options->flags = NXGL_OPEN_INITIALIZE_VIDEO;
  nxgl_engine_requirements_init(requirements);
  requirements->minimum_alpha_bits = 8;
  requirements->minimum_depth_bits = 16;
  requirements->minimum_stencil_bits = 0;
  candidates[0] = (nxgl_config_candidate){2, 0, 8, 8, 8, 8, 24, 8, 1};
  candidates[1] = (nxgl_config_candidate){2, 0, 8, 8, 8, 8, 16, 0, 1};
  options->requirements = requirements;
  options->candidates = candidates;
  options->candidate_count = 2;
  options->stack_ops = ops;
  options->stack_userdata = fake;
}

static int text_private(const char *value) {
  return value &&
         (strstr(value, "alice") || strstr(value, "ZXCV123") ||
          strstr(value, "EGL_SECRET") || strstr(value, "PRIVATE_TOKEN") ||
          strstr(value, "192.0.2.123") || strchr(value, '\n'));
}

typedef struct fake_terminal_status {
  const nxgl_open_options_v2 *reentry_options;
  nxgl_context **context_slot;
  nxgl_report_v2 *report_slot;
  nxgl_context *context_canary;
  nxgl_context *close_probe;
  nxgl_report_v2 report_canary;
  nxgl_status_kind expected_kind;
  int calls;
} fake_terminal_status;

static void fake_terminal_status_locked(void *userdata,
                                        nxgl_status_kind kind,
                                        const char *message) {
  fake_terminal_status *terminal = (fake_terminal_status *)userdata;
  nxgl_open_options_v2 invalid_v2_options;
  nxgl_open_options legacy_options;
  nxgl_engine_requirements legacy_requirements;
  nxgl_config_candidate legacy_candidate;
  nxgl_report legacy_report_before;
  nxgl_report legacy_report_after;
  int acquired;
  CHECK(terminal != NULL);
  if (!terminal)
    return;
  ++terminal->calls;
  CHECK(kind == terminal->expected_kind);
  CHECK(message != NULL && strstr(message, "nxgl-v2") != NULL);
  CHECK(terminal->reentry_options != NULL &&
        terminal->context_slot != NULL && terminal->report_slot != NULL);
  if (!terminal->reentry_options || !terminal->context_slot ||
      !terminal->report_slot)
    return;
  CHECK(*terminal->context_slot == terminal->context_canary);
  CHECK(memcmp(terminal->report_slot, &terminal->report_canary,
               sizeof(terminal->report_canary)) == 0);
  acquired = nxgl_arbiter_try_acquire();
  CHECK(acquired == 0);
  if (acquired) {
    nxgl_arbiter_release();
    return;
  }
  CHECK(nxgl_open_v2(terminal->reentry_options, terminal->context_slot,
                     terminal->report_slot) == NXGL_ERROR_BUSY);
  CHECK(*terminal->context_slot == terminal->context_canary);
  CHECK(memcmp(terminal->report_slot, &terminal->report_canary,
               sizeof(terminal->report_canary)) == 0);
  invalid_v2_options = *terminal->reentry_options;
  invalid_v2_options.api_version = 0u;
  CHECK(nxgl_open_v2(&invalid_v2_options, terminal->context_slot,
                     terminal->report_slot) == NXGL_ERROR_BUSY);
  CHECK(*terminal->context_slot == terminal->context_canary);
  CHECK(memcmp(terminal->report_slot, &terminal->report_canary,
               sizeof(terminal->report_canary)) == 0);
  nxgl_open_options_init(&legacy_options);
  nxgl_engine_requirements_init(&legacy_requirements);
  legacy_candidate =
      (nxgl_config_candidate){2, 0, 8, 8, 8, 8, 16, 0, 1};
  legacy_options.requirements = &legacy_requirements;
  legacy_options.candidates = &legacy_candidate;
  legacy_options.candidate_count = 1u;
  memset(&legacy_report_before, 0x3c, sizeof(legacy_report_before));
  memcpy(&legacy_report_after, &legacy_report_before,
         sizeof(legacy_report_after));
  CHECK(nxgl_open(&legacy_options, terminal->context_slot,
                  &legacy_report_after) == NXGL_ERROR_BUSY);
  CHECK(*terminal->context_slot == terminal->context_canary);
  CHECK(memcmp(&legacy_report_before, &legacy_report_after,
               sizeof(legacy_report_before)) == 0);
  legacy_options.api_version = 0u;
  CHECK(nxgl_open(&legacy_options, terminal->context_slot,
                  &legacy_report_after) == NXGL_ERROR_BUSY);
  CHECK(*terminal->context_slot == terminal->context_canary);
  CHECK(memcmp(&legacy_report_before, &legacy_report_after,
               sizeof(legacy_report_before)) == 0);
  if (terminal->close_probe) {
    CHECK(nxgl_window(terminal->close_probe) == NULL);
    CHECK(nxgl_sdl_context(terminal->close_probe) == NULL);
    CHECK(nxgl_get_report(terminal->close_probe) == NULL);
    CHECK(nxgl_make_current(terminal->close_probe) == NXGL_ERROR_BUSY);
    nxgl_close(terminal->close_probe);
    CHECK(nxgl_stack_owner(terminal->close_probe) ==
          NXGL_STACK_OWNER_V2_RAW_EGL);
    CHECK(nxgl_close_v2(terminal->close_probe) == NXGL_ERROR_BUSY);
  }
}

static void test_v1_contract_is_literal(void) {
  nxgl_open_options options;
  CHECK(NXGL_API_VERSION == 1u);
  CHECK(NXGL_API_VERSION_V1 == 1u);
  CHECK(NXGL_API_VERSION_V2 == 2u);
  nxgl_open_options_init(&options);
  CHECK(options.api_version == 1u);
  CHECK(options.struct_size == sizeof(nxgl_open_options));
  CHECK(offsetof(nxgl_open_options, status_userdata) + sizeof(void *) ==
        sizeof(nxgl_open_options));
}

static void test_success_ladder_reentry_privacy_and_close(void) {
  nxgl_open_options_v2 options;
  nxgl_open_options_v2 probe_options;
  nxgl_stack_ops_v2 ops;
  nxgl_stack_ops_v2 probe_ops;
  nxgl_engine_requirements requirements;
  nxgl_engine_requirements probe_requirements;
  nxgl_config_candidate candidates[2];
  nxgl_config_candidate probe_candidates[2];
  nxgl_report_v2 report;
  nxgl_report_v2 probe_report;
  nxgl_context *context;
  nxgl_context *probe_context = NULL;
  fake_stack fake;
  fake_stack probe_fake;
  fake_terminal_status terminal;
  const char *video_before;
  const char *alias_before;
  const char *video_after;
  const char *alias_after;
  memset(&fake, 0, sizeof(fake));
  memset(&probe_fake, 0, sizeof(probe_fake));
  probe_fake.mode = FAKE_SUCCESS_DIRECT;
  fake_ops_init(&probe_ops);
  fake_options_init(&probe_options, &probe_ops, &probe_fake,
                    &probe_requirements, probe_candidates);
  CHECK(nxgl_open_v2(&probe_options, &probe_context, &probe_report) ==
        NXGL_SUCCESS);
  CHECK(probe_context != NULL);
  fake.mode = FAKE_SUCCESS_LADDER;
  fake_ops_init(&ops);
  fake_options_init(&options, &ops, &fake, &requirements, candidates);
  fake.reentry_options = &options;
  context = probe_context;
  memset(&report, 0xa5, sizeof(report));
  memset(&terminal, 0, sizeof(terminal));
  terminal.reentry_options = &options;
  terminal.context_slot = &context;
  terminal.report_slot = &report;
  terminal.context_canary = probe_context;
  terminal.close_probe = probe_context;
  terminal.expected_kind = NXGL_STATUS_SELECTED;
  memcpy(&terminal.report_canary, &report, sizeof(report));
  options.status = fake_terminal_status_locked;
  options.status_userdata = &terminal;
  CHECK(setenv("SDL_VIDEODRIVER", "raw-inherited", 1) == 0);
  CHECK(setenv("SDL_VIDEO_DRIVER", "", 1) == 0);
  video_before = getenv("SDL_VIDEODRIVER");
  alias_before = getenv("SDL_VIDEO_DRIVER");
  CHECK(nxgl_open_v2(&options, &context, &report) == NXGL_SUCCESS);
  CHECK(context != NULL && context != probe_context);
  CHECK(terminal.calls == 1);
  CHECK(fake.reentry_checked == 1);
  video_after = getenv("SDL_VIDEODRIVER");
  alias_after = getenv("SDL_VIDEO_DRIVER");
  if (!video_before || !alias_before || !video_after || !alias_after) {
    CHECK(0 && "raw stack changed environment presence");
  } else {
    CHECK(strcmp(video_after, video_before) == 0);
    CHECK(strcmp(alias_after, alias_before) == 0);
  }
  CHECK(report.api_version == 2u);
  CHECK(report.legacy.api_version == 1u);
  CHECK(report.legacy.attempt_count == 2u);
  CHECK(report.legacy.selected_candidate_index == 1u);
  CHECK(report.stack_owner == NXGL_STACK_OWNER_V2_RAW_EGL);
  CHECK(report.egl.observed == 1 && report.egl.config_id == 7);
  CHECK(report.legacy.drawable_width == 1280);
  CHECK(report.legacy.drawable_height == 720);
  CHECK(fake.window_calls == 2u);
  CHECK(fake.context_calls == 2u);
  CHECK(fake.release_calls == 1u);
  CHECK(fake.stop_calls == 0u);
  CHECK(report.journal_count == 2u);
  CHECK(report.journal[0].stage == NXGL_OPEN_STAGE_V2_CONTEXT_CREATE);
  CHECK(report.journal[1].reason == NXGL_OPEN_REASON_V2_SELECTED);
  CHECK(!text_private(report.legacy.vendor));
  CHECK(!text_private(report.legacy.renderer));
  CHECK(!text_private(report.legacy.extensions));
  CHECK(!text_private(report.egl.vendor));
  CHECK(strstr(report.legacy.vendor, "[redacted-path]") != NULL);
  CHECK(strstr(report.legacy.vendor, "[redacted-ip]") != NULL);
  CHECK(nxgl_stack_owner(context) == NXGL_STACK_OWNER_V2_RAW_EGL);
  CHECK(nxgl_get_report_v2(context) != NULL);
  CHECK(nxgl_window(context) == NULL);
  CHECK(nxgl_sdl_context(context) == NULL);
  CHECK(nxgl_get_report(context) == NULL);
  CHECK(nxgl_make_current(context) == NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(nxgl_close_v2(context) == NXGL_SUCCESS);
  CHECK(fake.release_calls == 2u);
  CHECK(fake.stop_calls == 1u);
  CHECK(fake.order_size >= 2u);
  CHECK(fake.order[fake.order_size - 2u] == 'R');
  CHECK(fake.order[fake.order_size - 1u] == 'T');
  CHECK(nxgl_close_v2(probe_context) == NXGL_SUCCESS);
  CHECK(probe_fake.release_calls == 1u);
  CHECK(probe_fake.stop_calls == 1u);
  CHECK(unsetenv("SDL_VIDEODRIVER") == 0);
  CHECK(unsetenv("SDL_VIDEO_DRIVER") == 0);
}

static void run_failure_case(fake_mode mode, int expected_status,
                             unsigned expected_release_calls) {
  nxgl_open_options_v2 options;
  nxgl_stack_ops_v2 ops;
  nxgl_engine_requirements requirements;
  nxgl_config_candidate candidates[2];
  nxgl_report_v2 report;
  nxgl_context *context = NULL;
  fake_stack fake;
  unsigned index;
  int actual_status;
  memset(&fake, 0, sizeof(fake));
  fake.mode = mode;
  fake_ops_init(&ops);
  fake_options_init(&options, &ops, &fake, &requirements, candidates);
  actual_status = nxgl_open_v2(&options, &context, &report);
  if (actual_status != expected_status)
    (void)fprintf(stderr, "mode %d status %d expected %d\n", (int)mode,
                  actual_status, expected_status);
  CHECK(actual_status == expected_status);
  CHECK(context == NULL);
  CHECK(fake.stop_calls == 1u);
  CHECK(fake.release_calls == expected_release_calls);
  CHECK(report.journal_count > 0u);
  CHECK(!text_private(report.legacy.video_backend));
  if (mode != FAKE_START_FAILURE && mode != FAKE_BAD_BACKEND)
    CHECK(strcmp(report.legacy.video_backend, "fake-raw-egl") == 0);
  if (mode == FAKE_CONTEXT_FAILURE) {
    CHECK(report.final_stage == NXGL_OPEN_STAGE_V2_CONTEXT_CREATE);
    CHECK(report.final_reason == NXGL_OPEN_REASON_V2_CONTEXT_FAILED);
  }
  for (index = 0; index < report.journal_count; ++index)
    CHECK(!text_private(report.journal[index].detail));
}

static void test_fail_closed_provider_contracts(void) {
  run_failure_case(FAKE_START_FAILURE, NXGL_ERROR_VIDEO_UNAVAILABLE, 0u);
  run_failure_case(FAKE_BAD_BACKEND, NXGL_ERROR_STACK_MISMATCH, 0u);
  run_failure_case(FAKE_BAD_RETURN, NXGL_ERROR_STACK_MISMATCH, 1u);
  run_failure_case(FAKE_PROVIDER_BUSY, NXGL_ERROR_STACK_MISMATCH, 1u);
  run_failure_case(FAKE_BAD_CONFIG_ID, NXGL_ERROR_STACK_MISMATCH, 1u);
  run_failure_case(FAKE_BAD_EGL_BITS, NXGL_ERROR_STACK_MISMATCH, 1u);
  run_failure_case(FAKE_BAD_ACTUAL, NXGL_ERROR_STACK_MISMATCH, 1u);
  run_failure_case(FAKE_BAD_EGL_STRING, NXGL_ERROR_STACK_MISMATCH, 1u);
  run_failure_case(FAKE_UNTERMINATED_ERROR, NXGL_ERROR_STACK_MISMATCH, 1u);
  run_failure_case(FAKE_PRIVATE_ERROR, NXGL_ERROR_NO_GLES_CONFIG, 2u);
  run_failure_case(FAKE_CONTEXT_FAILURE, NXGL_ERROR_NO_GLES_CONFIG, 2u);
}

static void test_current_is_validated_before_and_after_provider_queries(void) {
  fake_mode modes[2] = {FAKE_PRE_CURRENT_FAIL, FAKE_FINAL_CURRENT_FAIL};
  size_t mode_index;
  for (mode_index = 0; mode_index < 2u; ++mode_index) {
    nxgl_open_options_v2 options;
    nxgl_stack_ops_v2 ops;
    nxgl_engine_requirements requirements;
    nxgl_config_candidate candidates[2];
    nxgl_report_v2 report;
    nxgl_context *context = NULL;
    fake_stack fake;
    memset(&fake, 0, sizeof(fake));
    fake.mode = modes[mode_index];
    fake_ops_init(&ops);
    fake_options_init(&options, &ops, &fake, &requirements, candidates);
    CHECK(nxgl_open_v2(&options, &context, &report) ==
          NXGL_ERROR_STACK_MISMATCH);
    CHECK(context == NULL);
    CHECK(fake.release_calls == 1u);
    CHECK(fake.stop_calls == 1u);
    if (fake.mode == FAKE_PRE_CURRENT_FAIL) {
      CHECK(fake.validate_calls == 1u);
      CHECK(fake.get_proc_calls == 0u);
      CHECK(fake.gl_string_calls == 0u);
      CHECK(fake.actual_calls == 0u);
      CHECK(fake.surface_calls == 0u);
    } else {
      CHECK(fake.validate_calls == 2u);
      CHECK(fake.get_proc_calls > 0u);
      CHECK(fake.gl_string_calls == 1u);
      CHECK(fake.actual_calls == 1u);
      CHECK(fake.surface_calls == 1u);
    }
  }
}

static void run_renderable_request_case(int requested_major,
                                        int actual_major,
                                        int renderable_type,
                                        int expected_status) {
  nxgl_open_options_v2 options;
  nxgl_stack_ops_v2 ops;
  nxgl_engine_requirements requirements;
  nxgl_config_candidate candidates[2];
  nxgl_report_v2 report;
  nxgl_context *context = NULL;
  fake_stack fake;
  memset(&fake, 0, sizeof(fake));
  fake.mode = FAKE_SUCCESS_DIRECT;
  fake.actual_gles_major = actual_major;
  fake.renderable_type_override = renderable_type;
  fake_ops_init(&ops);
  fake_options_init(&options, &ops, &fake, &requirements, candidates);
  requirements.minimum_gles_major = requested_major;
  requirements.minimum_gles_minor = 0;
  candidates[0].gles_major = requested_major;
  candidates[0].gles_minor = 0;
  options.candidate_count = 1u;
  CHECK(nxgl_open_v2(&options, &context, &report) == expected_status);
  if (expected_status == NXGL_SUCCESS) {
    CHECK(context != NULL);
    CHECK(report.legacy.requested.gles_major == requested_major);
    CHECK(report.legacy.actual.gles_major == actual_major);
    CHECK(report.egl.renderable_type == renderable_type);
    CHECK(nxgl_close_v2(context) == NXGL_SUCCESS);
  } else {
    CHECK(context == NULL);
    CHECK(fake.release_calls == 1u);
    CHECK(fake.stop_calls == 1u);
  }
}

static void test_egl_renderable_bit_tracks_requested_api(void) {
  run_renderable_request_case(2, 3, 0x0004, NXGL_SUCCESS);
  run_renderable_request_case(3, 3, 0x0040, NXGL_SUCCESS);
  run_renderable_request_case(3, 3, 0x0004,
                              NXGL_ERROR_STACK_MISMATCH);
  run_renderable_request_case(2, 3, 0x0040,
                              NXGL_ERROR_STACK_MISMATCH);
}

static void test_close_retry_preserves_release_stop_order(void) {
  fake_mode modes[2] = {FAKE_RELEASE_FAIL_ONCE, FAKE_STOP_FAIL_ONCE};
  size_t mode_index;
  for (mode_index = 0; mode_index < 2u; ++mode_index) {
    nxgl_open_options_v2 options;
    nxgl_stack_ops_v2 ops;
    nxgl_engine_requirements requirements;
    nxgl_config_candidate candidates[2];
    nxgl_report_v2 report;
    nxgl_context *context = NULL;
    fake_stack fake;
    memset(&fake, 0, sizeof(fake));
    fake.mode = modes[mode_index];
    fake_ops_init(&ops);
    fake_options_init(&options, &ops, &fake, &requirements, candidates);
    CHECK(nxgl_open_v2(&options, &context, &report) == NXGL_SUCCESS);
    CHECK(context != NULL);
    CHECK(nxgl_close_v2(context) == NXGL_ERROR_ROLLBACK);
    if (fake.mode == FAKE_RELEASE_FAIL_ONCE) {
      CHECK(fake.release_calls == 1u);
      CHECK(fake.stop_calls == 0u);
    } else {
      CHECK(fake.release_calls == 1u);
      CHECK(fake.stop_calls == 1u);
    }
    CHECK(nxgl_close_v2(context) == NXGL_SUCCESS);
    if (fake.mode == FAKE_RELEASE_FAIL_ONCE)
      CHECK(fake.release_calls == 2u);
    else
      CHECK(fake.release_calls == 1u);
    CHECK(fake.stop_calls ==
          (fake.mode == FAKE_STOP_FAIL_ONCE ? 2u : 1u));
  }
}

static void test_allocation_reason_is_exact(void) {
  nxgl_open_options_v2 options;
  nxgl_stack_ops_v2 ops;
  nxgl_engine_requirements requirements;
  nxgl_config_candidate candidates[2];
  nxgl_report_v2 report;
  nxgl_context *context = (nxgl_context *)(uintptr_t)0x1234u;
  fake_stack fake;
  memset(&fake, 0, sizeof(fake));
  fake_ops_init(&ops);
  fake_options_init(&options, &ops, &fake, &requirements, candidates);
  nxgl_test_fail_next_v2_allocation();
  CHECK(nxgl_open_v2(&options, &context, &report) ==
        NXGL_ERROR_OUT_OF_MEMORY);
  CHECK(context == NULL);
  CHECK(report.api_version == NXGL_API_VERSION_V2);
  CHECK(report.final_reason == NXGL_OPEN_REASON_V2_OUT_OF_MEMORY);
  CHECK(fake.start_calls == 0u);
  CHECK(fake.stop_calls == 0u);
}

static void test_failed_open_retains_cleanup_handle_until_retry(void) {
  nxgl_open_options_v2 options;
  nxgl_stack_ops_v2 ops;
  nxgl_engine_requirements requirements;
  nxgl_config_candidate candidates[2];
  nxgl_report_v2 report;
  nxgl_context *context = (nxgl_context *)(uintptr_t)0x4321u;
  fake_stack fake;
  fake_terminal_status terminal;
  memset(&fake, 0, sizeof(fake));
  fake.mode = FAKE_ATTEMPT_AND_RELEASE_FAIL_ONCE;
  fake_ops_init(&ops);
  fake_options_init(&options, &ops, &fake, &requirements, candidates);
  memset(&report, 0x5a, sizeof(report));
  memset(&terminal, 0, sizeof(terminal));
  terminal.reentry_options = &options;
  terminal.context_slot = &context;
  terminal.report_slot = &report;
  terminal.context_canary = context;
  terminal.expected_kind = NXGL_STATUS_ERROR;
  memcpy(&terminal.report_canary, &report, sizeof(report));
  options.status = fake_terminal_status_locked;
  options.status_userdata = &terminal;
  CHECK(nxgl_open_v2(&options, &context, &report) == NXGL_ERROR_ROLLBACK);
  CHECK(context != NULL && context != terminal.context_canary);
  CHECK(terminal.calls == 1);
  CHECK(memcmp(&report, &terminal.report_canary, sizeof(report)) != 0);
  CHECK(nxgl_get_report(context) == NULL);
  CHECK(fake.release_calls == 2u);
  CHECK(fake.stop_calls == 0u);
  CHECK(nxgl_close_v2(context) == NXGL_SUCCESS);
  CHECK(fake.release_calls == 3u);
  CHECK(fake.stop_calls == 1u);
}

static void fake_hint_assign(fake_hint_value *hint, int existed,
                             const char *value, size_t length) {
  memset(hint, 0, sizeof(*hint));
  hint->existed = existed;
  if (existed) {
    CHECK(value != NULL);
    CHECK(length + 1u <= sizeof(hint->value));
    if (value && length + 1u <= sizeof(hint->value)) {
      memcpy(hint->value, value, length);
      hint->value[length] = '\0';
    }
  }
}

static void test_exact_hint_retry_transaction(void) {
  nxgl_report_v2 report;
  int attempt = -1;
  char long_hint[NXGL_HINT_VALUE_MAX + 2u];
  memset(long_hint, 'h', sizeof(long_hint));
  long_hint[sizeof(long_hint) - 1u] = '\0';
  fake_fail_gles_hint_set = 0;
  fake_fail_x11_hint_set = 0;
  fake_hint_assign(&fake_gles_hint, 0, NULL, 0u);
  fake_hint_assign(&fake_x11_hint, 0, NULL, 0u);
  CHECK(nxgl_test_v2_hint_recovery_transaction("x11", &attempt, &report) ==
        NXGL_SUCCESS);
  CHECK(attempt == 1);
  CHECK(!fake_gles_hint.existed && !fake_x11_hint.existed);
  CHECK(report.hints_restored == 1);

  fake_hint_assign(&fake_gles_hint, 1, "", 0u);
  fake_hint_assign(&fake_x11_hint, 1, long_hint, NXGL_HINT_VALUE_MAX);
  CHECK(nxgl_test_v2_hint_recovery_transaction("x11", &attempt, &report) ==
        NXGL_SUCCESS);
  CHECK(attempt == 1);
  CHECK(fake_gles_hint.existed && fake_gles_hint.value[0] == '\0');
  CHECK(fake_x11_hint.existed);
  CHECK(strlen(fake_x11_hint.value) == NXGL_HINT_VALUE_MAX);
  CHECK(memcmp(fake_x11_hint.value, long_hint, NXGL_HINT_VALUE_MAX) == 0);
  CHECK(fake_x11_hint.value[NXGL_HINT_VALUE_MAX] == '\0');

  fake_hint_assign(&fake_gles_hint, 1, "gles-old", 8u);
  fake_hint_assign(&fake_x11_hint, 1, "egl-old", 7u);
  fake_fail_x11_hint_set = 1;
  CHECK(nxgl_test_v2_hint_recovery_transaction("x11", &attempt, &report) ==
        NXGL_ERROR_NO_GLES_CONFIG);
  CHECK(attempt == 0);
  CHECK(strcmp(fake_gles_hint.value, "gles-old") == 0);
  CHECK(strcmp(fake_x11_hint.value, "egl-old") == 0);
  CHECK(report.final_reason == NXGL_OPEN_REASON_V2_HINT_APPLY_FAILED);
  fake_fail_x11_hint_set = 0;

  fake_hint_assign(&fake_gles_hint, 1, long_hint,
                   NXGL_HINT_VALUE_MAX + 1u);
  fake_hint_assign(&fake_x11_hint, 0, NULL, 0u);
  CHECK(nxgl_test_v2_hint_recovery_transaction("x11", &attempt, &report) ==
        NXGL_ERROR_ROLLBACK);
  CHECK(attempt == 0);
  CHECK(report.final_reason == NXGL_OPEN_REASON_V2_SNAPSHOT_FAILED);
}

static void test_old_sdl_without_reset_keeps_normal_round(void) {
  nxgl_report_v2 report;
  int normal_round = -1;
  int hint_recovery = -1;

  fake_reset_hint_available = 0;
  fake_hint_assign(&fake_gles_hint, 0, NULL, 0u);
  fake_hint_assign(&fake_x11_hint, 0, NULL, 0u);
  CHECK(nxgl_test_v2_hint_snapshot_policy(&normal_round, &hint_recovery,
                                          &report) == NXGL_SUCCESS);
  CHECK(normal_round == 1);
  CHECK(hint_recovery == 0);
  CHECK(!fake_gles_hint.existed && !fake_x11_hint.existed);
  CHECK(report.hints_restored == 1);

  fake_reset_hint_available = 1;
}

int main(void) {
  test_v1_contract_is_literal();
  test_success_ladder_reentry_privacy_and_close();
  test_fail_closed_provider_contracts();
  test_current_is_validated_before_and_after_provider_queries();
  test_egl_renderable_bit_tracks_requested_api();
  test_close_retry_preserves_release_stop_order();
  test_allocation_reason_is_exact();
  test_failed_open_retains_cleanup_handle_until_retry();
  test_exact_hint_retry_transaction();
  test_old_sdl_without_reset_keeps_normal_round();
  if (failures) {
    (void)fprintf(stderr, "%d nxgl open-v2 test(s) failed\n", failures);
    return 1;
  }
  (void)fprintf(stdout,
                "nxgl hermetic open-v2 ownership/transaction tests passed\n");
  return 0;
}
