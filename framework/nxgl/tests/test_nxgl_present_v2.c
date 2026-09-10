/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxgl_internal.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define TEST_GL_SCISSOR_TEST 0x0C11u
#define TEST_GL_COLOR_CLEAR_VALUE 0x0C22u
#define TEST_GL_COLOR_WRITEMASK 0x0C23u
#define TEST_GL_COLOR_BUFFER_BIT 0x00004000u
#define TEST_GL_FRAMEBUFFER_BINDING 0x8CA6u
#define TEST_GL_INJECTED_ERROR 0x0502u

static int failures;
static int fake_arbiter_busy;
static unsigned emit_calls;
static char last_emit[NXGL_DETAIL_MAX];

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                             \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

int nxgl_arbiter_try_acquire(void) {
  if (fake_arbiter_busy)
    return 0;
  fake_arbiter_busy = 1;
  return 1;
}

void nxgl_arbiter_release(void) { fake_arbiter_busy = 0; }

void nxgl_emit(nxgl_status_callback callback, void *userdata,
               nxgl_status_kind kind, const char *format, ...) {
  va_list arguments;
  (void)callback;
  (void)userdata;
  (void)kind;
  ++emit_calls;
  va_start(arguments, format);
  (void)vsnprintf(last_emit, sizeof(last_emit), format, arguments);
  va_end(arguments);
}

typedef struct fake_provider {
  const nxgl_stack_handles_v2 *expected_handles;
  nxgl_context *reentry_context;
  const nxgl_present_policy *reentry_policy;
  unsigned validate_calls;
  unsigned present_calls;
  int validate_result;
  int present_result;
  int reenter_v1;
  int reentry_result;
  int reentry_saw_arbiter_held;
  int reentry_left_arbiter_held;
  int validate_unterminated_error;
  int present_unterminated_error;
  int error_has_controls;
} fake_provider;

typedef struct fake_gl {
  unsigned char scissor_enabled;
  unsigned char color_mask[4];
  float clear_color[4];
  int framebuffer;
  unsigned int pending_error;
  unsigned query_calls;
  unsigned mutation_calls;
  unsigned finish_calls;
  unsigned clear_calls;
  unsigned fail_query_at;
  unsigned fail_mutation_at;
  int fail_finish;
  int clear_saw_scissor_disabled;
  int clear_saw_alpha_only_mask;
  int clear_saw_alpha_one;
  unsigned int clear_mask;
} fake_gl;

typedef struct fake_gl_tracked_state {
  unsigned char scissor_enabled;
  unsigned char color_mask[4];
  float clear_color[4];
  int framebuffer;
} fake_gl_tracked_state;

static fake_gl gl_state;

static void fake_note_query(void) {
  ++gl_state.query_calls;
  if (gl_state.query_calls == gl_state.fail_query_at)
    gl_state.pending_error = TEST_GL_INJECTED_ERROR;
}

static void fake_note_mutation(void) {
  ++gl_state.mutation_calls;
  if (gl_state.mutation_calls == gl_state.fail_mutation_at)
    gl_state.pending_error = TEST_GL_INJECTED_ERROR;
}

static unsigned int fake_get_error(void) {
  const unsigned int error = gl_state.pending_error;
  gl_state.pending_error = 0u;
  return error;
}

static unsigned char fake_is_enabled(unsigned int capability) {
  CHECK(capability == TEST_GL_SCISSOR_TEST);
  fake_note_query();
  return gl_state.scissor_enabled;
}

static void fake_get_booleanv(unsigned int name, unsigned char *values) {
  CHECK(name == TEST_GL_COLOR_WRITEMASK);
  memcpy(values, gl_state.color_mask, sizeof(gl_state.color_mask));
  fake_note_query();
}

static void fake_get_floatv(unsigned int name, float *values) {
  CHECK(name == TEST_GL_COLOR_CLEAR_VALUE);
  memcpy(values, gl_state.clear_color, sizeof(gl_state.clear_color));
  fake_note_query();
}

static void fake_get_integerv(unsigned int name, int *value) {
  CHECK(name == TEST_GL_FRAMEBUFFER_BINDING);
  *value = gl_state.framebuffer;
  fake_note_query();
}

static void fake_disable(unsigned int capability) {
  CHECK(capability == TEST_GL_SCISSOR_TEST);
  gl_state.scissor_enabled = 0;
  fake_note_mutation();
}

static void fake_enable(unsigned int capability) {
  CHECK(capability == TEST_GL_SCISSOR_TEST);
  gl_state.scissor_enabled = 1;
  fake_note_mutation();
}

static void fake_color_mask(unsigned char red, unsigned char green,
                            unsigned char blue, unsigned char alpha) {
  gl_state.color_mask[0] = red;
  gl_state.color_mask[1] = green;
  gl_state.color_mask[2] = blue;
  gl_state.color_mask[3] = alpha;
  fake_note_mutation();
}

static void fake_clear_color(float red, float green, float blue, float alpha) {
  gl_state.clear_color[0] = red;
  gl_state.clear_color[1] = green;
  gl_state.clear_color[2] = blue;
  gl_state.clear_color[3] = alpha;
  fake_note_mutation();
}

static void fake_clear(unsigned int mask) {
  ++gl_state.clear_calls;
  gl_state.clear_saw_scissor_disabled = !gl_state.scissor_enabled;
  gl_state.clear_saw_alpha_only_mask =
      gl_state.color_mask[0] == 0 && gl_state.color_mask[1] == 0 &&
      gl_state.color_mask[2] == 0 && gl_state.color_mask[3] == 1;
  gl_state.clear_saw_alpha_one = gl_state.clear_color[3] == 1.0f;
  gl_state.clear_mask = mask;
  fake_note_mutation();
}

static void fake_finish(void) {
  ++gl_state.finish_calls;
  if (gl_state.fail_finish)
    gl_state.pending_error = TEST_GL_INJECTED_ERROR;
}

static int fake_validate_current(void *userdata,
                                 const nxgl_stack_handles_v2 *handles,
                                 char *error, size_t error_size) {
  fake_provider *provider = (fake_provider *)userdata;
  ++provider->validate_calls;
  CHECK(handles == provider->expected_handles);
  if (provider->validate_unterminated_error) {
    memset(error, 'V', error_size);
  } else if (provider->error_has_controls) {
    (void)snprintf(error, error_size,
                   "wrong\n/home/test-user 192.0.2.123 token=fake\tcontext");
  }
  return provider->validate_result;
}

static int fake_present(void *userdata,
                        const nxgl_stack_handles_v2 *handles, char *error,
                        size_t error_size) {
  fake_provider *provider = (fake_provider *)userdata;
  ++provider->present_calls;
  CHECK(handles == provider->expected_handles);
  if (provider->reenter_v1) {
    provider->reentry_saw_arbiter_held = fake_arbiter_busy;
    provider->reentry_result =
        nxgl_present(provider->reentry_context, provider->reentry_policy);
    provider->reentry_left_arbiter_held = fake_arbiter_busy;
  }
  if (provider->present_unterminated_error) {
    memset(error, 'P', error_size);
  } else if (provider->error_has_controls) {
    (void)snprintf(error, error_size,
                   "swap\n/home/test-user 192.0.2.123 token=fake\tfailed");
  }
  return provider->present_result;
}

static int fake_legacy_adapter(void *userdata, SDL_Window *window,
                               SDL_GLContext context, char *error,
                               size_t error_size) {
  unsigned *calls = (unsigned *)userdata;
  (void)window;
  (void)context;
  (void)error;
  (void)error_size;
  ++*calls;
  return 0;
}

static void reset_fake_gl(void) {
  memset(&gl_state, 0, sizeof(gl_state));
  gl_state.scissor_enabled = 1;
  gl_state.color_mask[0] = 1;
  gl_state.color_mask[1] = 0;
  gl_state.color_mask[2] = 1;
  gl_state.color_mask[3] = 0;
  gl_state.clear_color[0] = 0.25f;
  gl_state.clear_color[1] = 0.5f;
  gl_state.clear_color[2] = 0.75f;
  gl_state.clear_color[3] = 0.125f;
}

static void snapshot_tracked_state(fake_gl_tracked_state *state) {
  memset(state, 0, sizeof(*state));
  state->scissor_enabled = gl_state.scissor_enabled;
  memcpy(state->color_mask, gl_state.color_mask, sizeof(state->color_mask));
  memcpy(state->clear_color, gl_state.clear_color,
         sizeof(state->clear_color));
  state->framebuffer = gl_state.framebuffer;
}

static int tracked_state_matches(const fake_gl_tracked_state *state) {
  return state->scissor_enabled == gl_state.scissor_enabled &&
         memcmp(state->color_mask, gl_state.color_mask,
                sizeof(state->color_mask)) == 0 &&
         memcmp(state->clear_color, gl_state.clear_color,
                sizeof(state->clear_color)) == 0 &&
         state->framebuffer == gl_state.framebuffer;
}

static void init_context(nxgl_context *context, fake_provider *provider,
                         nxgl_stack_owner_v2 owner) {
  memset(context, 0, sizeof(*context));
  memset(provider, 0, sizeof(*provider));
  context->open_api_version = NXGL_API_VERSION_V2;
  context->stack_owner = owner;
  context->stack_ops.api_version = NXGL_API_VERSION_V2;
  context->stack_ops.struct_size = sizeof(context->stack_ops);
  context->stack_ops.owner = owner;
  context->stack_ops.validate_current = fake_validate_current;
  context->stack_ops.present = fake_present;
  context->stack_userdata = provider;
  context->stack_handles.api_version = NXGL_API_VERSION_V2;
  context->stack_handles.struct_size = sizeof(context->stack_handles);
  context->stack_handles.owner = owner;
  context->report_v2.api_version = NXGL_API_VERSION_V2;
  context->report_v2.struct_size = sizeof(context->report_v2);
  context->report_v2.stack_owner = owner;
  context->report_v2.handles.api_version = NXGL_API_VERSION_V2;
  context->report_v2.handles.struct_size =
      sizeof(context->report_v2.handles);
  context->report_v2.handles.owner = owner;
  context->report_v2.legacy.actual.alpha_bits = 8;
  context->gl.get_error = fake_get_error;
  context->gl.finish = fake_finish;
  context->gl.is_enabled = fake_is_enabled;
  context->gl.get_booleanv = fake_get_booleanv;
  context->gl.get_floatv = fake_get_floatv;
  context->gl.get_integerv = fake_get_integerv;
  context->gl.disable = fake_disable;
  context->gl.enable = fake_enable;
  context->gl.color_mask = fake_color_mask;
  context->gl.clear_color = fake_clear_color;
  context->gl.clear = fake_clear;
  context->report_v2.handles = context->stack_handles;
  provider->expected_handles = &context->stack_handles;
}

static nxgl_present_policy_v2 delegated_policy(nxgl_present_owner owner) {
  nxgl_present_policy_v2 policy;
  nxgl_present_policy_v2_init(&policy);
  policy.owner = owner;
  return policy;
}

static void enable_alpha_one(nxgl_present_policy_v2 *policy) {
  policy->flags |= NXGL_PRESENT_FORCE_BACKBUFFER_ALPHA_ONE;
  policy->quirk = NXGL_PRESENT_QUIRK_V2_AMLOGIC_OSD_ALPHA_ONE;
  policy->reason = NXGL_PRESENT_REASON_V2_OBSERVED_OSD_ZERO_ALPHA;
}

static void test_v1_abi_canary_and_v2_initializers(void) {
  typedef struct v1_present_policy_canary {
    uint32_t api_version;
    size_t struct_size;
    nxgl_present_owner owner;
    uint32_t flags;
    nxgl_present_callback adapter;
    void *userdata;
  } v1_present_policy_canary;
  nxgl_present_policy legacy_policy;
  nxgl_present_policy_v2 policy;
  nxgl_present_result_v2 result;
  CHECK(NXGL_API_VERSION == NXGL_API_VERSION_V1);
  CHECK(NXGL_API_VERSION_V1 == 1u && NXGL_API_VERSION_V2 == 2u);
  CHECK(NXGL_PRESENT_STAGE_V2_ARBITER != NXGL_PRESENT_STAGE_V2_CURRENT);
  CHECK(sizeof(nxgl_present_policy) == sizeof(v1_present_policy_canary));
  CHECK(offsetof(nxgl_present_policy, api_version) ==
        offsetof(v1_present_policy_canary, api_version));
  CHECK(offsetof(nxgl_present_policy, struct_size) ==
        offsetof(v1_present_policy_canary, struct_size));
  CHECK(offsetof(nxgl_present_policy, owner) ==
        offsetof(v1_present_policy_canary, owner));
  CHECK(offsetof(nxgl_present_policy, flags) ==
        offsetof(v1_present_policy_canary, flags));
  CHECK(offsetof(nxgl_present_policy, adapter) ==
        offsetof(v1_present_policy_canary, adapter));
  CHECK(offsetof(nxgl_present_policy, userdata) ==
        offsetof(v1_present_policy_canary, userdata));
  nxgl_present_policy_init(&legacy_policy);
  nxgl_present_policy_v2_init(&policy);
  nxgl_present_result_v2_init(&result);
  CHECK(legacy_policy.api_version == NXGL_API_VERSION_V1);
  CHECK(legacy_policy.struct_size == sizeof(legacy_policy));
  CHECK(legacy_policy.owner == NXGL_PRESENT_ENGINE_OWNED);
  CHECK(legacy_policy.flags == 0u && legacy_policy.adapter == NULL &&
        legacy_policy.userdata == NULL);
  CHECK(policy.api_version == NXGL_API_VERSION_V2);
  CHECK(policy.owner == NXGL_PRESENT_ENGINE_OWNED && policy.flags == 0u);
  CHECK(policy.quirk == NXGL_PRESENT_QUIRK_V2_NONE);
  CHECK(policy.reason == NXGL_PRESENT_REASON_V2_NONE);
  CHECK(result.api_version == NXGL_API_VERSION_V2);
  CHECK(result.result == NXGL_NO_ACTION);
}

static void test_present_v2_callback_cannot_reenter_v1(void) {
  nxgl_context context;
  fake_provider provider;
  nxgl_present_policy legacy_policy;
  nxgl_present_policy_v2 policy;
  nxgl_present_result_v2 result;
  fake_gl_tracked_state before;

  reset_fake_gl();
  snapshot_tracked_state(&before);
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  context.window = (SDL_Window *)(uintptr_t)0x101u;
  context.gl_context = (SDL_GLContext)(uintptr_t)0x202u;
  context.report.actual.alpha_bits = 8;
  nxgl_present_policy_init(&legacy_policy);
  legacy_policy.owner = NXGL_PRESENT_SDL;
  legacy_policy.flags = NXGL_PRESENT_FINISH_BEFORE_SWAP |
                        NXGL_PRESENT_FORCE_BACKBUFFER_ALPHA_ONE;
  provider.reentry_context = &context;
  provider.reentry_policy = &legacy_policy;
  provider.reenter_v1 = 1;
  policy = delegated_policy(NXGL_PRESENT_SDL);

  CHECK(nxgl_present_v2(&context, &policy, &result) == NXGL_SUCCESS);
  CHECK(result.result == NXGL_SUCCESS);
  CHECK(provider.validate_calls == 1u && provider.present_calls == 1u);
  CHECK(provider.reentry_saw_arbiter_held == 1);
  CHECK(provider.reentry_result == NXGL_ERROR_BUSY);
  CHECK(provider.reentry_left_arbiter_held == 1);
  CHECK(gl_state.query_calls == 0u && gl_state.mutation_calls == 0u);
  CHECK(gl_state.finish_calls == 0u && gl_state.clear_calls == 0u);
  CHECK(tracked_state_matches(&before));
  CHECK(fake_arbiter_busy == 0);
}

static void test_v1_rejects_v2_context_when_uncontended(void) {
  nxgl_context context;
  fake_provider provider;
  nxgl_present_policy legacy_policy;
  fake_gl_tracked_state before;
  unsigned adapter_calls = 0u;

  fake_arbiter_busy = 0;
  reset_fake_gl();
  snapshot_tracked_state(&before);
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  context.window = (SDL_Window *)(uintptr_t)0x303u;
  context.gl_context = (SDL_GLContext)(uintptr_t)0x404u;
  context.report.actual.alpha_bits = 8;
  nxgl_present_policy_init(&legacy_policy);
  legacy_policy.owner = NXGL_PRESENT_ADAPTER;
  legacy_policy.flags = NXGL_PRESENT_FINISH_BEFORE_SWAP |
                        NXGL_PRESENT_FORCE_BACKBUFFER_ALPHA_ONE;
  legacy_policy.adapter = fake_legacy_adapter;
  legacy_policy.userdata = &adapter_calls;

  CHECK(nxgl_present(&context, &legacy_policy) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(adapter_calls == 0u);
  CHECK(provider.validate_calls == 0u && provider.present_calls == 0u);
  CHECK(gl_state.query_calls == 0u && gl_state.mutation_calls == 0u);
  CHECK(gl_state.finish_calls == 0u && gl_state.clear_calls == 0u);
  CHECK(tracked_state_matches(&before));
  CHECK(fake_arbiter_busy == 0);
}

static void test_engine_owned_is_strict_no_action(void) {
  nxgl_context context;
  fake_provider provider;
  nxgl_present_policy_v2 policy;
  nxgl_present_result_v2 result;
  reset_fake_gl();
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  nxgl_present_policy_v2_init(&policy);
  CHECK(nxgl_present_v2(&context, &policy, &result) == NXGL_NO_ACTION);
  CHECK(result.result == NXGL_NO_ACTION);
  CHECK(provider.validate_calls == 0u && provider.present_calls == 0u);
  CHECK(gl_state.query_calls == 0u && gl_state.mutation_calls == 0u);
  CHECK(fake_arbiter_busy == 0);
}

static void test_owner_is_strictly_associated_with_open_stack(void) {
  nxgl_context context;
  fake_provider provider;
  nxgl_present_policy_v2 policy;
  nxgl_present_result_v2 result;
  reset_fake_gl();
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  policy = delegated_policy(NXGL_PRESENT_ADAPTER);
  CHECK(nxgl_present_v2(&context, &policy, &result) ==
        NXGL_ERROR_STACK_MISMATCH);
  CHECK(result.failed_stage == NXGL_PRESENT_STAGE_V2_CURRENT);
  CHECK(provider.validate_calls == 0u && provider.present_calls == 0u);
  CHECK(gl_state.query_calls == 0u && gl_state.mutation_calls == 0u);

  context.report_v2.handles.egl_context = (uintptr_t)1u;
  policy = delegated_policy(NXGL_PRESENT_SDL);
  CHECK(nxgl_present_v2(&context, &policy, &result) ==
        NXGL_ERROR_STACK_MISMATCH);
  CHECK(provider.validate_calls == 0u && provider.present_calls == 0u);

  reset_fake_gl();
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_RAW_EGL);
  policy = delegated_policy(NXGL_PRESENT_ADAPTER);
  CHECK(nxgl_present_v2(&context, &policy, &result) == NXGL_SUCCESS);
  CHECK(provider.validate_calls == 1u && provider.present_calls == 1u);
  CHECK(gl_state.query_calls == 0u && gl_state.mutation_calls == 0u);
}

static void test_current_validation_precedes_every_graphics_touch(void) {
  nxgl_context context;
  fake_provider provider;
  nxgl_present_policy_v2 policy;
  nxgl_present_result_v2 result;
  fake_gl_tracked_state before;
  reset_fake_gl();
  snapshot_tracked_state(&before);
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  provider.validate_result = NXGL_ERROR_STACK_MISMATCH;
  provider.error_has_controls = 1;
  policy = delegated_policy(NXGL_PRESENT_SDL);
  enable_alpha_one(&policy);
  policy.flags |= NXGL_PRESENT_FINISH_BEFORE_SWAP;
  CHECK(nxgl_present_v2(&context, &policy, &result) ==
        NXGL_ERROR_STACK_MISMATCH);
  CHECK(result.failed_stage == NXGL_PRESENT_STAGE_V2_CURRENT);
  CHECK(result.expected_context_current == 0);
  CHECK(provider.validate_calls == 1u && provider.present_calls == 0u);
  CHECK(gl_state.query_calls == 0u && gl_state.mutation_calls == 0u);
  CHECK(gl_state.finish_calls == 0u);
  CHECK(tracked_state_matches(&before));
  CHECK(strchr(last_emit, '\n') == NULL && strchr(last_emit, '\t') == NULL);
  CHECK(strstr(last_emit, "/home/") == NULL);
  CHECK(strstr(last_emit, "192.0.2.") == NULL);
  CHECK(strstr(last_emit, "token=") == NULL);
}

static void test_busy_is_byte_atomic_and_callback_free(void) {
  nxgl_context context;
  fake_provider provider;
  nxgl_present_policy_v2 policy;
  nxgl_present_result_v2 result;
  nxgl_present_result_v2 before_result;
  fake_gl_tracked_state before_state;
  reset_fake_gl();
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  policy = delegated_policy(NXGL_PRESENT_SDL);
  enable_alpha_one(&policy);
  memset(&result, 0xa5, sizeof(result));
  memcpy(&before_result, &result, sizeof(before_result));
  snapshot_tracked_state(&before_state);
  fake_arbiter_busy = 1;
  CHECK(nxgl_present_v2(&context, &policy, &result) == NXGL_ERROR_BUSY);
  CHECK(memcmp(&result, &before_result, sizeof(result)) == 0);
  CHECK(provider.validate_calls == 0u && provider.present_calls == 0u);
  CHECK(gl_state.query_calls == 0u && gl_state.mutation_calls == 0u);
  CHECK(tracked_state_matches(&before_state));
  CHECK(fake_arbiter_busy == 1);
  fake_arbiter_busy = 0;
}

static void test_alpha_one_requires_exact_quirk_and_reason(void) {
  nxgl_context context;
  fake_provider provider;
  nxgl_present_policy_v2 policy;
  nxgl_present_result_v2 result;
  reset_fake_gl();
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  policy = delegated_policy(NXGL_PRESENT_SDL);
  policy.flags = NXGL_PRESENT_FORCE_BACKBUFFER_ALPHA_ONE;
  CHECK(nxgl_present_v2(&context, &policy, &result) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(provider.validate_calls == 0u && gl_state.query_calls == 0u);

  policy = delegated_policy(NXGL_PRESENT_SDL);
  policy.quirk = NXGL_PRESENT_QUIRK_V2_AMLOGIC_OSD_ALPHA_ONE;
  policy.reason = NXGL_PRESENT_REASON_V2_OBSERVED_OSD_ZERO_ALPHA;
  CHECK(nxgl_present_v2(&context, &policy, &result) ==
        NXGL_ERROR_INVALID_ARGUMENT);

  policy = delegated_policy(NXGL_PRESENT_SDL);
  enable_alpha_one(&policy);
  policy.reason = NXGL_PRESENT_REASON_V2_NONE;
  CHECK(nxgl_present_v2(&context, &policy, &result) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(provider.validate_calls == 0u && provider.present_calls == 0u);
  CHECK(fake_arbiter_busy == 0);
}

static void test_alpha_one_success_preserves_all_tracked_state(void) {
  nxgl_context context;
  fake_provider provider;
  nxgl_present_policy_v2 policy;
  nxgl_present_result_v2 result;
  fake_gl_tracked_state before;
  reset_fake_gl();
  snapshot_tracked_state(&before);
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  policy = delegated_policy(NXGL_PRESENT_SDL);
  enable_alpha_one(&policy);
  CHECK(nxgl_present_v2(&context, &policy, &result) == NXGL_SUCCESS);
  CHECK(result.expected_context_current == 1);
  CHECK(result.state_snapshotted == 1 && result.state_restored == 1);
  CHECK(provider.validate_calls == 1u && provider.present_calls == 1u);
  CHECK(gl_state.clear_calls == 1u);
  CHECK(gl_state.clear_saw_scissor_disabled);
  CHECK(gl_state.clear_saw_alpha_only_mask);
  CHECK(gl_state.clear_saw_alpha_one);
  CHECK(gl_state.clear_mask == TEST_GL_COLOR_BUFFER_BIT);
  CHECK(gl_state.finish_calls == 0u);
  CHECK(tracked_state_matches(&before));
  CHECK(fake_arbiter_busy == 0);
}

static void test_alpha_one_rejects_nondefault_fbo_without_mutation(void) {
  nxgl_context context;
  fake_provider provider;
  nxgl_present_policy_v2 policy;
  nxgl_present_result_v2 result;
  fake_gl_tracked_state before;
  reset_fake_gl();
  gl_state.framebuffer = 7;
  snapshot_tracked_state(&before);
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  policy = delegated_policy(NXGL_PRESENT_SDL);
  enable_alpha_one(&policy);
  CHECK(nxgl_present_v2(&context, &policy, &result) == NXGL_ERROR_PRESENT);
  CHECK(result.failed_stage == NXGL_PRESENT_STAGE_V2_ALPHA_ONE);
  CHECK(result.state_snapshotted == 1 && result.state_restored == 1);
  CHECK(gl_state.mutation_calls == 0u && gl_state.clear_calls == 0u);
  CHECK(provider.present_calls == 0u);
  CHECK(tracked_state_matches(&before));
}

static void test_alpha_one_preconditions_and_disabled_scissor(void) {
  nxgl_context context;
  fake_provider provider;
  nxgl_present_policy_v2 policy;
  nxgl_present_result_v2 result;
  fake_gl_tracked_state before;

  reset_fake_gl();
  snapshot_tracked_state(&before);
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  context.report_v2.legacy.actual.alpha_bits = 0;
  policy = delegated_policy(NXGL_PRESENT_SDL);
  enable_alpha_one(&policy);
  CHECK(nxgl_present_v2(&context, &policy, &result) == NXGL_ERROR_PRESENT);
  CHECK(result.failed_stage == NXGL_PRESENT_STAGE_V2_ALPHA_ONE);
  CHECK(gl_state.query_calls == 0u && gl_state.mutation_calls == 0u);
  CHECK(provider.present_calls == 0u && tracked_state_matches(&before));

  reset_fake_gl();
  snapshot_tracked_state(&before);
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  context.gl.clear = NULL;
  policy = delegated_policy(NXGL_PRESENT_SDL);
  enable_alpha_one(&policy);
  CHECK(nxgl_present_v2(&context, &policy, &result) == NXGL_ERROR_PRESENT);
  CHECK(result.failed_stage == NXGL_PRESENT_STAGE_V2_SNAPSHOT);
  CHECK(gl_state.query_calls == 0u && gl_state.mutation_calls == 0u);
  CHECK(provider.present_calls == 0u && tracked_state_matches(&before));

  reset_fake_gl();
  gl_state.scissor_enabled = 0;
  snapshot_tracked_state(&before);
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  policy = delegated_policy(NXGL_PRESENT_SDL);
  enable_alpha_one(&policy);
  CHECK(nxgl_present_v2(&context, &policy, &result) == NXGL_SUCCESS);
  CHECK(result.state_restored == 1);
  CHECK(gl_state.clear_saw_scissor_disabled);
  CHECK(tracked_state_matches(&before));
}

static void test_snapshot_failure_is_pre_mutation_and_atomic(void) {
  unsigned failure_at;
  for (failure_at = 1u; failure_at <= 4u; ++failure_at) {
    nxgl_context context;
    fake_provider provider;
    nxgl_present_policy_v2 policy;
    nxgl_present_result_v2 result;
    fake_gl_tracked_state before;
    reset_fake_gl();
    gl_state.fail_query_at = failure_at;
    snapshot_tracked_state(&before);
    init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
    policy = delegated_policy(NXGL_PRESENT_SDL);
    enable_alpha_one(&policy);
    CHECK(nxgl_present_v2(&context, &policy, &result) == NXGL_ERROR_PRESENT);
    CHECK(result.failed_stage == NXGL_PRESENT_STAGE_V2_SNAPSHOT);
    CHECK(result.state_snapshotted == 0 && result.state_restored == 0);
    CHECK(gl_state.mutation_calls == 0u && provider.present_calls == 0u);
    CHECK(tracked_state_matches(&before));
    CHECK(fake_arbiter_busy == 0);
  }
}

static void test_each_alpha_mutation_failure_rolls_back(void) {
  unsigned failure_at;
  for (failure_at = 1u; failure_at <= 4u; ++failure_at) {
    nxgl_context context;
    fake_provider provider;
    nxgl_present_policy_v2 policy;
    nxgl_present_result_v2 result;
    fake_gl_tracked_state before;
    reset_fake_gl();
    gl_state.fail_mutation_at = failure_at;
    snapshot_tracked_state(&before);
    init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
    policy = delegated_policy(NXGL_PRESENT_SDL);
    enable_alpha_one(&policy);
    CHECK(nxgl_present_v2(&context, &policy, &result) == NXGL_ERROR_PRESENT);
    CHECK(result.failed_stage == NXGL_PRESENT_STAGE_V2_ALPHA_ONE);
    CHECK(result.state_snapshotted == 1 && result.state_restored == 1);
    CHECK(provider.present_calls == 0u);
    CHECK(tracked_state_matches(&before));
    CHECK(fake_arbiter_busy == 0);
  }
}

static void test_restore_and_verification_failures_are_detected(void) {
  unsigned failure_at;
  for (failure_at = 5u; failure_at <= 7u; ++failure_at) {
    nxgl_context context;
    fake_provider provider;
    nxgl_present_policy_v2 policy;
    nxgl_present_result_v2 result;
    fake_gl_tracked_state before;
    reset_fake_gl();
    gl_state.fail_mutation_at = failure_at;
    snapshot_tracked_state(&before);
    init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
    policy = delegated_policy(NXGL_PRESENT_SDL);
    enable_alpha_one(&policy);
    CHECK(nxgl_present_v2(&context, &policy, &result) == NXGL_ERROR_ROLLBACK);
    CHECK(result.failed_stage == NXGL_PRESENT_STAGE_V2_RESTORE);
    CHECK(result.state_snapshotted == 1 && result.state_restored == 0);
    CHECK(provider.present_calls == 0u);
    CHECK(tracked_state_matches(&before));
    CHECK(fake_arbiter_busy == 0);
  }
  for (failure_at = 5u; failure_at <= 8u; ++failure_at) {
    nxgl_context context;
    fake_provider provider;
    nxgl_present_policy_v2 policy;
    nxgl_present_result_v2 result;
    fake_gl_tracked_state before;
    reset_fake_gl();
    gl_state.fail_query_at = failure_at;
    snapshot_tracked_state(&before);
    init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
    policy = delegated_policy(NXGL_PRESENT_SDL);
    enable_alpha_one(&policy);
    CHECK(nxgl_present_v2(&context, &policy, &result) == NXGL_ERROR_ROLLBACK);
    CHECK(result.failed_stage == NXGL_PRESENT_STAGE_V2_RESTORE);
    CHECK(provider.present_calls == 0u);
    CHECK(tracked_state_matches(&before));
    CHECK(fake_arbiter_busy == 0);
  }
}

static void test_present_failure_occurs_after_state_restore(void) {
  nxgl_context context;
  fake_provider provider;
  nxgl_present_policy_v2 policy;
  nxgl_present_result_v2 result;
  fake_gl_tracked_state before;
  reset_fake_gl();
  snapshot_tracked_state(&before);
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  provider.present_result = NXGL_ERROR_PRESENT;
  policy = delegated_policy(NXGL_PRESENT_SDL);
  enable_alpha_one(&policy);
  CHECK(nxgl_present_v2(&context, &policy, &result) == NXGL_ERROR_PRESENT);
  CHECK(result.failed_stage == NXGL_PRESENT_STAGE_V2_PRESENT);
  CHECK(result.state_restored == 1);
  CHECK(provider.present_calls == 1u);
  CHECK(tracked_state_matches(&before));
}

static void test_glfinish_is_only_an_explicit_policy(void) {
  nxgl_context context;
  fake_provider provider;
  nxgl_present_policy_v2 policy;
  nxgl_present_result_v2 result;
  reset_fake_gl();
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  policy = delegated_policy(NXGL_PRESENT_SDL);
  CHECK(nxgl_present_v2(&context, &policy, &result) == NXGL_SUCCESS);
  CHECK(gl_state.finish_calls == 0u);

  policy.flags = NXGL_PRESENT_FINISH_BEFORE_SWAP;
  CHECK(nxgl_present_v2(&context, &policy, &result) == NXGL_SUCCESS);
  CHECK(gl_state.finish_calls == 1u);

  gl_state.fail_finish = 1;
  CHECK(nxgl_present_v2(&context, &policy, &result) == NXGL_ERROR_PRESENT);
  CHECK(result.failed_stage == NXGL_PRESENT_STAGE_V2_FINISH);
  CHECK(gl_state.finish_calls == 2u);
  CHECK(provider.present_calls == 2u);
  CHECK(fake_arbiter_busy == 0);
}

static void test_unterminated_provider_errors_never_overread(void) {
  nxgl_context context;
  fake_provider provider;
  nxgl_present_policy_v2 policy;
  nxgl_present_result_v2 result;
  reset_fake_gl();
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  provider.validate_result = NXGL_ERROR_PRESENT;
  provider.validate_unterminated_error = 1;
  policy = delegated_policy(NXGL_PRESENT_SDL);
  CHECK(nxgl_present_v2(&context, &policy, &result) ==
        NXGL_ERROR_STACK_MISMATCH);
  CHECK(strstr(last_emit, "present-v2-provider-contract") != NULL);
  CHECK(provider.present_calls == 0u);

  reset_fake_gl();
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  provider.present_result = NXGL_ERROR_PRESENT;
  provider.present_unterminated_error = 1;
  policy = delegated_policy(NXGL_PRESENT_SDL);
  CHECK(nxgl_present_v2(&context, &policy, &result) ==
        NXGL_ERROR_STACK_MISMATCH);
  CHECK(strstr(last_emit, "present-v2-provider-contract") != NULL);
  CHECK(fake_arbiter_busy == 0);

  reset_fake_gl();
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  provider.validate_result = 42;
  policy = delegated_policy(NXGL_PRESENT_SDL);
  CHECK(nxgl_present_v2(&context, &policy, &result) ==
        NXGL_ERROR_STACK_MISMATCH);
  CHECK(provider.present_calls == 0u);

  reset_fake_gl();
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  provider.present_result = 42;
  policy = delegated_policy(NXGL_PRESENT_SDL);
  CHECK(nxgl_present_v2(&context, &policy, &result) ==
        NXGL_ERROR_STACK_MISMATCH);
  CHECK(fake_arbiter_busy == 0);

  reset_fake_gl();
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  provider.validate_result = NXGL_ERROR_BUSY;
  policy = delegated_policy(NXGL_PRESENT_SDL);
  CHECK(nxgl_present_v2(&context, &policy, &result) ==
        NXGL_ERROR_STACK_MISMATCH);
  CHECK(provider.present_calls == 0u);

  reset_fake_gl();
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  provider.present_result = NXGL_ERROR_BUSY;
  policy = delegated_policy(NXGL_PRESENT_SDL);
  CHECK(nxgl_present_v2(&context, &policy, &result) ==
        NXGL_ERROR_STACK_MISMATCH);
  CHECK(fake_arbiter_busy == 0);

  reset_fake_gl();
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  provider.validate_result = NXGL_SUCCESS;
  provider.validate_unterminated_error = 1;
  policy = delegated_policy(NXGL_PRESENT_SDL);
  CHECK(nxgl_present_v2(&context, &policy, &result) ==
        NXGL_ERROR_STACK_MISMATCH);
  CHECK(provider.present_calls == 0u);
  CHECK(gl_state.query_calls == 0u && gl_state.mutation_calls == 0u);

  reset_fake_gl();
  init_context(&context, &provider, NXGL_STACK_OWNER_V2_SDL_EGL);
  provider.present_result = NXGL_SUCCESS;
  provider.present_unterminated_error = 1;
  policy = delegated_policy(NXGL_PRESENT_SDL);
  CHECK(nxgl_present_v2(&context, &policy, &result) ==
        NXGL_ERROR_STACK_MISMATCH);
  CHECK(provider.present_calls == 1u);
  CHECK(fake_arbiter_busy == 0);
}

int main(void) {
  test_v1_abi_canary_and_v2_initializers();
  test_present_v2_callback_cannot_reenter_v1();
  test_v1_rejects_v2_context_when_uncontended();
  test_engine_owned_is_strict_no_action();
  test_owner_is_strictly_associated_with_open_stack();
  test_current_validation_precedes_every_graphics_touch();
  test_busy_is_byte_atomic_and_callback_free();
  test_alpha_one_requires_exact_quirk_and_reason();
  test_alpha_one_success_preserves_all_tracked_state();
  test_alpha_one_rejects_nondefault_fbo_without_mutation();
  test_alpha_one_preconditions_and_disabled_scissor();
  test_snapshot_failure_is_pre_mutation_and_atomic();
  test_each_alpha_mutation_failure_rolls_back();
  test_restore_and_verification_failures_are_detected();
  test_present_failure_occurs_after_state_restore();
  test_glfinish_is_only_an_explicit_policy();
  test_unterminated_provider_errors_never_overread();
  if (failures) {
    (void)fprintf(stderr, "%d nxgl present-v2 test(s) failed\n", failures);
    return 1;
  }
  (void)fprintf(stdout, "nxgl fake-provider present-v2 tests passed\n");
  return 0;
}
