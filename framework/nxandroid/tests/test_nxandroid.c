/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxandroid.h"
#include "nxandroid_jni_adapter.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
      return -1;                                                               \
    }                                                                          \
  } while (0)

#define STEP(phase_value, module_value, cycle_value, id_value, rollback_value, \
             terminal_value)                                                   \
  {                                                                            \
    phase_value, module_value, cycle_value, id_value, rollback_value,           \
        terminal_value, 0u, 0u                                                 \
  }
#define STEP_GROUP(phase_value, module_value, cycle_value, id_value,            \
                   rollback_value, terminal_value, group_value, close_value)   \
  {                                                                            \
    phase_value, module_value, cycle_value, id_value, rollback_value,           \
        terminal_value, group_value, close_value                               \
  }

static const nxandroid_module_spec normal_modules[] = {
    {"libsupport.so", NXANDROID_JNI_NONE},
    {"libgame.so", NXANDROID_JNI_REQUIRED},
};

static const nxandroid_step normal_steps[] = {
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 0u, 0u, "step-00", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 1u, 0u, "step-01", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_JNI, 1u, 0u, "step-02", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP_GROUP(NXANDROID_PHASE_ACTIVITY_CREATE, NXANDROID_NO_MODULE, 0u,
               "step-03", "rollback-activity", NXANDROID_TERMINAL_NONE, 1u,
               0u),
    STEP_GROUP(NXANDROID_PHASE_GRAPHICS_REQUEST, NXANDROID_NO_MODULE, 1u,
               "step-04", "rollback-graphics-request",
               NXANDROID_TERMINAL_NONE, 2u, 0u),
    STEP_GROUP(NXANDROID_PHASE_SURFACE_UP, NXANDROID_NO_MODULE, 1u, "step-05",
               "rollback-surface", NXANDROID_TERMINAL_NONE, 2u, 0u),
    STEP(NXANDROID_PHASE_SURFACE_CHANGED, NXANDROID_NO_MODULE, 1u, "step-06",
         NULL, NXANDROID_TERMINAL_NONE),
    STEP_GROUP(NXANDROID_PHASE_RESUME, NXANDROID_NO_MODULE, 1u, "step-07",
               "rollback-resume", NXANDROID_TERMINAL_NONE, 3u, 0u),
    STEP_GROUP(NXANDROID_PHASE_FOCUS_GAIN, NXANDROID_NO_MODULE, 1u, "step-08",
               "rollback-focus", NXANDROID_TERMINAL_NONE, 4u, 0u),
    STEP_GROUP(NXANDROID_PHASE_GL_READY, NXANDROID_NO_MODULE, 1u, "step-09",
               "rollback-gl", NXANDROID_TERMINAL_NONE, 2u, 0u),
    STEP(NXANDROID_PHASE_ENTRY, NXANDROID_NO_MODULE, 1u, "step-10", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_OBJECTS_READY, NXANDROID_NO_MODULE, 1u, "step-11",
         NULL, NXANDROID_TERMINAL_NONE),
    STEP_GROUP(NXANDROID_PHASE_INPUT_ENABLE, NXANDROID_NO_MODULE, 1u,
               "step-12", "rollback-input", NXANDROID_TERMINAL_NONE, 5u, 0u),
    STEP(NXANDROID_PHASE_RUN_LOOP, NXANDROID_NO_MODULE, 1u, "step-13", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP_GROUP(NXANDROID_PHASE_FOCUS_LOSS, NXANDROID_NO_MODULE, 1u, "step-14",
               NULL, NXANDROID_TERMINAL_NONE, 0u, 4u),
    STEP_GROUP(NXANDROID_PHASE_PAUSE, NXANDROID_NO_MODULE, 1u, "step-15", NULL,
               NXANDROID_TERMINAL_NONE, 0u, 3u),
    STEP_GROUP(NXANDROID_PHASE_INPUT_DISABLE, NXANDROID_NO_MODULE, 1u,
               "step-16", NULL, NXANDROID_TERMINAL_NONE, 0u, 5u),
    STEP(NXANDROID_PHASE_SAVE, NXANDROID_NO_MODULE, 0u, "step-17", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP_GROUP(NXANDROID_PHASE_SURFACE_DOWN, NXANDROID_NO_MODULE, 1u,
               "step-18", NULL, NXANDROID_TERMINAL_NONE, 0u, 2u),
    STEP_GROUP(NXANDROID_PHASE_NATIVE_SHUTDOWN, NXANDROID_NO_MODULE, 0u,
               "step-19", NULL, NXANDROID_TERMINAL_NONE, 0u, 1u),
    STEP(NXANDROID_PHASE_TERMINAL, NXANDROID_NO_MODULE, 0u, "step-20", NULL,
         NXANDROID_TERMINAL_RETURN),
};

typedef struct mock_adapter {
  const nxandroid_profile *profile;
  nxandroid_context *context;
  nxandroid_context *other_context;
  size_t invoke_count;
  size_t rollback_count;
  size_t invoked[64];
  size_t rolled_back[64];
  size_t fail_at;
  size_t rollback_fail_step;
  size_t reenter_at;
  size_t run_other_at;
  int reenter_in_rollback;
  nxandroid_result nested_run;
  nxandroid_result nested_step;
  nxandroid_result nested_abort;
  nxandroid_result nested_destroy;
  nxandroid_context_state nested_state;
  size_t nested_next;
  int nested_adapter_status;
  int nested_rollback_status;
  int nested_checked;
  nxandroid_result other_result;
  nxandroid_terminal_policy terminal_policy;
  const char *terminal_contract_id;
} mock_adapter;

static nxandroid_profile make_profile(const nxandroid_step *steps,
                                      size_t step_count, uint32_t flags) {
  nxandroid_profile profile;
  memset(&profile, 0, sizeof(profile));
  profile.api_version = NXANDROID_API_VERSION;
  profile.struct_size = sizeof(profile);
  profile.modules = normal_modules;
  profile.module_count = ARRAY_SIZE(normal_modules);
  profile.steps = steps;
  profile.step_count = step_count;
  profile.flags = flags;
  return profile;
}

static nxandroid_ops make_ops(mock_adapter *mock) {
  nxandroid_ops ops;
  memset(&ops, 0, sizeof(ops));
  ops.api_version = NXANDROID_API_VERSION;
  ops.struct_size = sizeof(ops);
  ops.userdata = mock;
  return ops;
}

static size_t find_step(const mock_adapter *mock,
                        const nxandroid_step *step) {
  size_t index;
  for (index = 0; index < mock->profile->step_count; ++index) {
    if (strcmp(mock->profile->steps[index].contract_id, step->contract_id) == 0)
      return index;
  }
  return NXANDROID_NO_MODULE;
}

static void attempt_reentrancy(mock_adapter *mock) {
  nxandroid_context *before = mock->context;
  mock->nested_run = nxandroid_context_run(mock->context);
  mock->nested_step = nxandroid_context_step(mock->context);
  mock->nested_abort = nxandroid_context_abort(mock->context);
  mock->nested_destroy = nxandroid_context_destroy(&mock->context);
  mock->nested_state = nxandroid_context_get_state(mock->context);
  mock->nested_next = nxandroid_context_get_next_step(mock->context);
  mock->nested_adapter_status =
      nxandroid_context_get_adapter_status(mock->context);
  mock->nested_rollback_status =
      nxandroid_context_get_rollback_status(mock->context);
  if (mock->context == before)
    mock->nested_checked = 1;
}

static int mock_invoke(void *userdata, const nxandroid_step *step) {
  mock_adapter *mock = (mock_adapter *)userdata;
  size_t index = find_step(mock, step);
  if (index == NXANDROID_NO_MODULE || mock->invoke_count >= ARRAY_SIZE(mock->invoked))
    return 99;
  mock->invoked[mock->invoke_count] = index;
  if (mock->invoke_count == mock->reenter_at)
    attempt_reentrancy(mock);
  if (mock->invoke_count == mock->run_other_at)
    mock->other_result = nxandroid_context_step(mock->other_context);
  ++mock->invoke_count;
  if (step->phase == NXANDROID_PHASE_TERMINAL) {
    mock->terminal_policy = step->terminal_policy;
    mock->terminal_contract_id = step->contract_id;
  }
  return index == mock->fail_at ? 71 : 0;
}

static int mock_rollback(void *userdata, const nxandroid_step *step) {
  mock_adapter *mock = (mock_adapter *)userdata;
  size_t index = find_step(mock, step);
  if (index == NXANDROID_NO_MODULE ||
      mock->rollback_count >= ARRAY_SIZE(mock->rolled_back))
    return 98;
  mock->rolled_back[mock->rollback_count++] = index;
  if (mock->reenter_in_rollback && !mock->nested_checked)
    attempt_reentrancy(mock);
  return index == mock->rollback_fail_step ? 83 : 0;
}

static void reset_mock(mock_adapter *mock, const nxandroid_profile *profile) {
  memset(mock, 0, sizeof(*mock));
  mock->profile = profile;
  mock->fail_at = NXANDROID_NO_MODULE;
  mock->rollback_fail_step = NXANDROID_NO_MODULE;
  mock->reenter_at = NXANDROID_NO_MODULE;
  mock->run_other_at = NXANDROID_NO_MODULE;
}

static int run_with_mock(const nxandroid_profile *profile, mock_adapter *mock,
                         nxandroid_context **context) {
  nxandroid_ops ops = make_ops(mock);
  ops.invoke = mock_invoke;
  ops.rollback = mock_rollback;
  CHECK(nxandroid_context_create(profile, &ops, context) == NXANDROID_OK);
  mock->context = *context;
  return 0;
}

static int test_normal(void) {
  nxandroid_profile profile =
      make_profile(normal_steps, ARRAY_SIZE(normal_steps), 0u);
  mock_adapter mock;
  nxandroid_context *context = NULL;
  size_t index;

  reset_mock(&mock, &profile);
  CHECK(nxandroid_profile_validate(&profile, NULL) == NXANDROID_OK);
  CHECK(run_with_mock(&profile, &mock, &context) == 0);
  CHECK(nxandroid_context_get_state(context) == NXANDROID_CONTEXT_READY);
  CHECK(nxandroid_context_step(context) == NXANDROID_OK);
  CHECK(nxandroid_context_get_next_step(context) == 1u);
  CHECK(nxandroid_context_run(context) == NXANDROID_OK);
  CHECK(nxandroid_context_get_state(context) == NXANDROID_CONTEXT_COMPLETE);
  CHECK(mock.invoke_count == profile.step_count);
  CHECK(mock.rollback_count == 0u);
  for (index = 0; index < profile.step_count; ++index)
    CHECK(mock.invoked[index] == index);
  CHECK(mock.terminal_policy == NXANDROID_TERMINAL_RETURN);
  CHECK(strcmp(mock.terminal_contract_id, "step-20") == 0);
  CHECK(nxandroid_context_get_adapter_status(context) == 0);
  CHECK(nxandroid_context_get_rollback_status(context) == 0);
  CHECK(nxandroid_context_run(context) == NXANDROID_ESTATE);
  CHECK(nxandroid_context_destroy(&context) == NXANDROID_OK);
  CHECK(context == NULL);
  return 0;
}

static int expect_bad_profile(nxandroid_step *steps, size_t expected_bad,
                              uint32_t flags) {
  nxandroid_profile profile =
      make_profile(steps, ARRAY_SIZE(normal_steps), flags);
  size_t bad = NXANDROID_NO_MODULE;
  CHECK(nxandroid_profile_validate(&profile, &bad) == NXANDROID_EPROFILE);
  CHECK(bad == expected_bad);
  return 0;
}

static int test_out_of_order(void) {
  nxandroid_step steps[ARRAY_SIZE(normal_steps)];

  memcpy(steps, normal_steps, sizeof(steps));
  steps[0].phase = NXANDROID_PHASE_MODULE_JNI;
  steps[0].module_index = 1u;
  CHECK(expect_bad_profile(steps, 0u, 0u) == 0);

  memcpy(steps, normal_steps, sizeof(steps));
  steps[3].phase = NXANDROID_PHASE_GRAPHICS_REQUEST;
  steps[3].cycle_id = 1u;
  CHECK(expect_bad_profile(steps, 3u, 0u) == 0);

  memcpy(steps, normal_steps, sizeof(steps));
  steps[4].phase = NXANDROID_PHASE_SURFACE_UP;
  CHECK(expect_bad_profile(steps, 4u, 0u) == 0);

  memcpy(steps, normal_steps, sizeof(steps));
  steps[4].phase = NXANDROID_PHASE_GL_READY;
  CHECK(expect_bad_profile(steps, 4u, 0u) == 0);

  memcpy(steps, normal_steps, sizeof(steps));
  steps[5].phase = NXANDROID_PHASE_ENTRY;
  CHECK(expect_bad_profile(steps, 5u, 0u) == 0);

  memcpy(steps, normal_steps, sizeof(steps));
  steps[11].phase = NXANDROID_PHASE_INPUT_ENABLE;
  CHECK(expect_bad_profile(steps, 11u, 0u) == 0);

  memcpy(steps, normal_steps, sizeof(steps));
  steps[14].phase = NXANDROID_PHASE_SAVE;
  steps[14].cycle_id = 0u;
  CHECK(expect_bad_profile(steps, 14u, 0u) == 0);

  memcpy(steps, normal_steps, sizeof(steps));
  steps[13].cycle_id = 999u;
  CHECK(expect_bad_profile(steps, 13u, 0u) == 0);

  memcpy(steps, normal_steps, sizeof(steps));
  steps[14].cycle_id = 999u;
  CHECK(expect_bad_profile(steps, 14u, 0u) == 0);

  memcpy(steps, normal_steps, sizeof(steps));
  steps[15].cycle_id = 999u;
  CHECK(expect_bad_profile(steps, 15u, 0u) == 0);

  memcpy(steps, normal_steps, sizeof(steps));
  steps[16].phase = NXANDROID_PHASE_SURFACE_CHANGED;
  CHECK(expect_bad_profile(steps, 17u, 0u) == 0);

  memcpy(steps, normal_steps, sizeof(steps));
  steps[20].terminal_policy = NXANDROID_TERMINAL_ADAPTER;
  CHECK(expect_bad_profile(steps, 20u, 0u) == 0);

  memcpy(steps, normal_steps, sizeof(steps));
  steps[2].module_index = 0u;
  CHECK(expect_bad_profile(steps, 2u, 0u) == 0);

  memcpy(steps, normal_steps, sizeof(steps));
  steps[3].rollback_group = 0u;
  CHECK(expect_bad_profile(steps, 3u, 0u) == 0);

  memcpy(steps, normal_steps, sizeof(steps));
  steps[0].closes_rollback_group = 2u;
  CHECK(expect_bad_profile(steps, 0u, 0u) == 0);

  memcpy(steps, normal_steps, sizeof(steps));
  steps[20].rollback_contract_id = "rollback-after-close-invalid";
  steps[20].rollback_group = 5u;
  CHECK(expect_bad_profile(steps, 20u, 0u) == 0);
  return 0;
}

static int test_declared_module_and_graphics_order(void) {
  nxandroid_step reordered_modules[ARRAY_SIZE(normal_steps)];
  nxandroid_step gl_before_surface[ARRAY_SIZE(normal_steps)];
  nxandroid_step lifecycle_before_surface[ARRAY_SIZE(normal_steps)];
  nxandroid_step saved_gl;
  nxandroid_profile profile;
  mock_adapter mock;
  nxandroid_context *context = NULL;

  /* Module order is adapter-owned, while every declared module must complete
   * INIT and its required JNI before Activity. */
  reordered_modules[0] = normal_steps[1];
  reordered_modules[1] = normal_steps[2];
  reordered_modules[2] = normal_steps[0];
  reordered_modules[3] = normal_steps[3];
  memcpy(&reordered_modules[4], &normal_steps[4],
         (ARRAY_SIZE(normal_steps) - 4u) * sizeof(normal_steps[0]));
  profile = make_profile(reordered_modules, ARRAY_SIZE(reordered_modules), 0u);
  CHECK(nxandroid_profile_validate(&profile, NULL) == NXANDROID_OK);
  reset_mock(&mock, &profile);
  CHECK(run_with_mock(&profile, &mock, &context) == 0);
  CHECK(nxandroid_context_run(context) == NXANDROID_OK);
  CHECK(nxandroid_context_destroy(&context) == NXANDROID_OK);

  /* Resume/focus epochs are adapter-ordered and may precede Surface. Their
   * matching loss/pause events must retain the same epoch IDs. */
  memcpy(lifecycle_before_surface, normal_steps,
         4u * sizeof(normal_steps[0]));
  lifecycle_before_surface[4] = normal_steps[7];
  lifecycle_before_surface[5] = normal_steps[8];
  lifecycle_before_surface[6] = normal_steps[4];
  lifecycle_before_surface[7] = normal_steps[5];
  lifecycle_before_surface[8] = normal_steps[6];
  lifecycle_before_surface[9] = normal_steps[9];
  memcpy(&lifecycle_before_surface[10], &normal_steps[10],
         (ARRAY_SIZE(normal_steps) - 10u) * sizeof(normal_steps[0]));
  profile = make_profile(lifecycle_before_surface,
                         ARRAY_SIZE(lifecycle_before_surface), 0u);
  CHECK(nxandroid_profile_validate(&profile, NULL) == NXANDROID_OK);
  reset_mock(&mock, &profile);
  CHECK(run_with_mock(&profile, &mock, &context) == 0);
  CHECK(nxandroid_context_run(context) == NXANDROID_OK);
  CHECK(nxandroid_context_destroy(&context) == NXANDROID_OK);

  /* Approved Bully-like trace: host GL exists before guest Surface callbacks.
   * GRAPHICS_REQUEST remains the mandatory boundary before either event. */
  memcpy(gl_before_surface, normal_steps, sizeof(gl_before_surface));
  saved_gl = gl_before_surface[9];
  memmove(&gl_before_surface[6], &gl_before_surface[5],
          4u * sizeof(gl_before_surface[0]));
  gl_before_surface[5] = saved_gl;
  profile = make_profile(gl_before_surface, ARRAY_SIZE(gl_before_surface), 0u);
  CHECK(nxandroid_profile_validate(&profile, NULL) == NXANDROID_OK);
  CHECK(profile.steps[4].phase == NXANDROID_PHASE_GRAPHICS_REQUEST);
  CHECK(profile.steps[5].phase == NXANDROID_PHASE_GL_READY);
  CHECK(profile.steps[6].phase == NXANDROID_PHASE_SURFACE_UP);
  reset_mock(&mock, &profile);
  CHECK(run_with_mock(&profile, &mock, &context) == 0);
  CHECK(nxandroid_context_run(context) == NXANDROID_OK);
  CHECK(nxandroid_context_destroy(&context) == NXANDROID_OK);

  /* The normal fixture is the opposite Unity-like ordering. */
  CHECK(normal_steps[5].phase == NXANDROID_PHASE_SURFACE_UP);
  CHECK(normal_steps[9].phase == NXANDROID_PHASE_GL_READY);
  return 0;
}

static const nxandroid_step sonic_steps[] = {
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 0u, 0u, "sonic-00", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 1u, 0u, "sonic-01", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_JNI, 1u, 0u, "sonic-02", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP_GROUP(NXANDROID_PHASE_ACTIVITY_CREATE, NXANDROID_NO_MODULE, 0u,
               "sonic-03", "sonic-rollback-activity",
               NXANDROID_TERMINAL_NONE, 1u, 0u),
    STEP_GROUP(NXANDROID_PHASE_GRAPHICS_REQUEST, NXANDROID_NO_MODULE, 1u,
               "sonic-04", "sonic-rollback-graphics",
               NXANDROID_TERMINAL_NONE, 2u, 0u),
    STEP_GROUP(NXANDROID_PHASE_GL_READY, NXANDROID_NO_MODULE, 1u, "sonic-05",
               "sonic-rollback-gl", NXANDROID_TERMINAL_NONE, 2u, 0u),
    STEP(NXANDROID_PHASE_ENTRY, NXANDROID_NO_MODULE, 1u, "sonic-06", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_OBJECTS_READY, NXANDROID_NO_MODULE, 1u, "sonic-07",
         NULL, NXANDROID_TERMINAL_NONE),
    STEP_GROUP(NXANDROID_PHASE_SURFACE_UP, NXANDROID_NO_MODULE, 1u, "sonic-08",
               "sonic-rollback-surface", NXANDROID_TERMINAL_NONE, 2u, 0u),
    STEP(NXANDROID_PHASE_SURFACE_CHANGED, NXANDROID_NO_MODULE, 1u, "sonic-09",
         NULL, NXANDROID_TERMINAL_NONE),
    STEP_GROUP(NXANDROID_PHASE_RESUME, NXANDROID_NO_MODULE, 1u, "sonic-10",
               "sonic-rollback-resume", NXANDROID_TERMINAL_NONE, 3u, 0u),
    STEP_GROUP(NXANDROID_PHASE_FOCUS_GAIN, NXANDROID_NO_MODULE, 1u,
               "sonic-11", "sonic-rollback-focus", NXANDROID_TERMINAL_NONE,
               4u, 0u),
    STEP_GROUP(NXANDROID_PHASE_INPUT_ENABLE, NXANDROID_NO_MODULE, 1u,
               "sonic-12", "sonic-rollback-input", NXANDROID_TERMINAL_NONE,
               5u, 0u),
    STEP(NXANDROID_PHASE_RUN_LOOP, NXANDROID_NO_MODULE, 1u, "sonic-13", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP_GROUP(NXANDROID_PHASE_FOCUS_LOSS, NXANDROID_NO_MODULE, 1u, "sonic-14",
               NULL, NXANDROID_TERMINAL_NONE, 0u, 4u),
    STEP_GROUP(NXANDROID_PHASE_PAUSE, NXANDROID_NO_MODULE, 1u, "sonic-15", NULL,
               NXANDROID_TERMINAL_NONE, 0u, 3u),
    STEP_GROUP(NXANDROID_PHASE_INPUT_DISABLE, NXANDROID_NO_MODULE, 1u,
               "sonic-16", NULL, NXANDROID_TERMINAL_NONE, 0u, 5u),
    STEP(NXANDROID_PHASE_SAVE, NXANDROID_NO_MODULE, 0u, "sonic-17", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP_GROUP(NXANDROID_PHASE_SURFACE_DOWN, NXANDROID_NO_MODULE, 1u,
               "sonic-18", NULL, NXANDROID_TERMINAL_NONE, 0u, 2u),
    STEP_GROUP(NXANDROID_PHASE_NATIVE_SHUTDOWN, NXANDROID_NO_MODULE, 0u,
               "sonic-19", NULL, NXANDROID_TERMINAL_NONE, 0u, 1u),
    STEP(NXANDROID_PHASE_TERMINAL, NXANDROID_NO_MODULE, 0u, "sonic-20", NULL,
         NXANDROID_TERMINAL_RETURN),
};

static int expect_bad_sonic_profile(nxandroid_step *steps,
                                    size_t expected_bad, uint32_t flags) {
  nxandroid_profile profile =
      make_profile(steps, ARRAY_SIZE(sonic_steps), flags);
  size_t bad = NXANDROID_NO_MODULE;
  CHECK(nxandroid_profile_validate(&profile, &bad) == NXANDROID_EPROFILE);
  CHECK(bad == expected_bad);
  return 0;
}

static int test_sonic_entry_before_surface_callback(void) {
  const uint32_t flag =
      NXANDROID_PROFILE_ALLOW_ENTRY_BEFORE_SURFACE_CALLBACK;
  nxandroid_step steps[ARRAY_SIZE(sonic_steps)];
  nxandroid_profile profile =
      make_profile(sonic_steps, ARRAY_SIZE(sonic_steps), flag);
  mock_adapter mock;
  nxandroid_context *context = NULL;

  /* Approved Sonic-like trace: context/GL and engine objects exist before the
   * guest Surface callback. Input and the blocking loop still wait for it. */
  CHECK(nxandroid_profile_validate(&profile, NULL) == NXANDROID_OK);
  reset_mock(&mock, &profile);
  CHECK(run_with_mock(&profile, &mock, &context) == 0);
  CHECK(nxandroid_context_run(context) == NXANDROID_OK);
  CHECK(mock.invoke_count == ARRAY_SIZE(sonic_steps));
  CHECK(nxandroid_context_destroy(&context) == NXANDROID_OK);

  memcpy(steps, sonic_steps, sizeof(steps));
  CHECK(expect_bad_sonic_profile(steps, 6u, 0u) == 0);

  memcpy(steps, sonic_steps, sizeof(steps));
  steps[5] = sonic_steps[6];
  steps[6] = sonic_steps[5];
  CHECK(expect_bad_sonic_profile(steps, 5u, flag) == 0);

  memcpy(steps, sonic_steps, sizeof(steps));
  steps[6].cycle_id = 2u;
  CHECK(expect_bad_sonic_profile(steps, 6u, flag) == 0);

  memcpy(steps, sonic_steps, sizeof(steps));
  steps[8] = sonic_steps[12];
  steps[12] = sonic_steps[8];
  CHECK(expect_bad_sonic_profile(steps, 8u, flag) == 0);

  /* Input is correctly still disabled before Surface; the moved blocking loop
   * must fail at its own step instead of treating the opt-in as runtime-ready. */
  memcpy(steps, sonic_steps, sizeof(steps));
  steps[8] = sonic_steps[13];
  steps[13] = sonic_steps[8];
  CHECK(expect_bad_sonic_profile(steps, 8u, flag) == 0);

  memcpy(steps, sonic_steps, sizeof(steps));
  steps[7] = sonic_steps[8];
  steps[8] = sonic_steps[7];
  CHECK(expect_bad_sonic_profile(steps, 7u, flag) == 0);

  /* The permission is evidence-scoped, not a harmless blanket capability. */
  memcpy(steps, normal_steps, sizeof(steps));
  CHECK(expect_bad_profile(steps, 10u, flag) == 0);
  return 0;
}

static int group_closed_before(size_t failed_at, uint32_t group) {
  size_t index;
  for (index = 0u; index < failed_at; ++index) {
    if (normal_steps[index].closes_rollback_group == group)
      return 1;
  }
  return 0;
}

static size_t expected_rollback_steps(size_t failed_at, size_t *output) {
  size_t count = 0u;
  size_t cursor = failed_at + 1u;
  while (cursor > 0u) {
    --cursor;
    if (normal_steps[cursor].rollback_contract_id != NULL &&
        normal_steps[cursor].rollback_contract_id[0] != '\0' &&
        !group_closed_before(failed_at,
                             normal_steps[cursor].rollback_group))
      output[count++] = cursor;
  }
  return count;
}

static int test_failure_each_phase(void) {
  nxandroid_profile profile =
      make_profile(normal_steps, ARRAY_SIZE(normal_steps), 0u);
  size_t failed_at;
  for (failed_at = 0u; failed_at < profile.step_count; ++failed_at) {
    mock_adapter mock;
    nxandroid_context *context = NULL;
    size_t expected[ARRAY_SIZE(normal_steps)];
    size_t expected_count;
    size_t index;
    reset_mock(&mock, &profile);
    mock.fail_at = failed_at;
    CHECK(run_with_mock(&profile, &mock, &context) == 0);
    CHECK(nxandroid_context_run(context) == NXANDROID_ECALLBACK);
    CHECK(nxandroid_context_get_state(context) == NXANDROID_CONTEXT_FAILED);
    CHECK(nxandroid_context_get_adapter_status(context) == 71);
    CHECK(mock.invoke_count == failed_at + 1u);
    expected_count = expected_rollback_steps(failed_at, expected);
    CHECK(mock.rollback_count == expected_count);
    for (index = 0u; index < expected_count; ++index)
      CHECK(mock.rolled_back[index] == expected[index]);
    if (failed_at == 17u) {
      CHECK(expected_count == 4u);
      CHECK(mock.rolled_back[0] == 9u);
      CHECK(mock.rolled_back[1] == 5u);
      CHECK(mock.rolled_back[2] == 4u);
      CHECK(mock.rolled_back[3] == 3u);
    }
    if (failed_at == 19u) {
      CHECK(expected_count == 1u);
      CHECK(mock.rolled_back[0] == 3u);
    }
    if (failed_at == 20u)
      CHECK(expected_count == 0u);
    CHECK(nxandroid_context_run(context) == NXANDROID_ESTATE);
    CHECK(nxandroid_context_step(context) == NXANDROID_ESTATE);
    CHECK(nxandroid_context_abort(context) == NXANDROID_ESTATE);
    CHECK(nxandroid_context_destroy(&context) == NXANDROID_OK);
    CHECK(context == NULL);
    CHECK(mock.rollback_count == expected_count);
  }
  return 0;
}

static int test_rollback_failure_and_abort(void) {
  nxandroid_profile profile =
      make_profile(normal_steps, ARRAY_SIZE(normal_steps), 0u);
  mock_adapter mock;
  nxandroid_context *context = NULL;
  size_t expected[ARRAY_SIZE(normal_steps)];
  size_t expected_count;

  reset_mock(&mock, &profile);
  mock.fail_at = 12u;
  mock.rollback_fail_step = 8u;
  CHECK(run_with_mock(&profile, &mock, &context) == 0);
  CHECK(nxandroid_context_run(context) == NXANDROID_EROLLBACK);
  CHECK(nxandroid_context_get_adapter_status(context) == 71);
  CHECK(nxandroid_context_get_rollback_status(context) == 83);
  expected_count = expected_rollback_steps(12u, expected);
  CHECK(mock.rollback_count == expected_count);
  CHECK(nxandroid_context_destroy(&context) == NXANDROID_OK);

  reset_mock(&mock, &profile);
  CHECK(run_with_mock(&profile, &mock, &context) == 0);
  while (nxandroid_context_get_next_step(context) < 10u)
    CHECK(nxandroid_context_step(context) == NXANDROID_OK);
  expected_count = expected_rollback_steps(9u, expected);
  CHECK(nxandroid_context_abort(context) == NXANDROID_OK);
  CHECK(nxandroid_context_get_state(context) == NXANDROID_CONTEXT_ABORTED);
  CHECK(mock.rollback_count == expected_count);
  CHECK(nxandroid_context_destroy(&context) == NXANDROID_OK);

  reset_mock(&mock, &profile);
  CHECK(run_with_mock(&profile, &mock, &context) == 0);
  while (nxandroid_context_get_next_step(context) < 6u)
    CHECK(nxandroid_context_step(context) == NXANDROID_OK);
  expected_count = expected_rollback_steps(5u, expected);
  CHECK(nxandroid_context_destroy(&context) == NXANDROID_OK);
  CHECK(context == NULL);
  CHECK(mock.rollback_count == expected_count);
  return 0;
}

static int check_reentrant_results(const mock_adapter *mock) {
  CHECK(mock->nested_checked);
  CHECK(mock->nested_run == NXANDROID_EREENTRANT);
  CHECK(mock->nested_step == NXANDROID_EREENTRANT);
  CHECK(mock->nested_abort == NXANDROID_EREENTRANT);
  CHECK(mock->nested_destroy == NXANDROID_EREENTRANT);
  CHECK(mock->nested_state == NXANDROID_CONTEXT_FAILED);
  CHECK(mock->nested_next == NXANDROID_NO_MODULE);
  CHECK(mock->nested_adapter_status == NXANDROID_EREENTRANT);
  CHECK(mock->nested_rollback_status == NXANDROID_EREENTRANT);
  return 0;
}

static int test_reentrancy(void) {
  nxandroid_profile profile =
      make_profile(normal_steps, ARRAY_SIZE(normal_steps), 0u);
  mock_adapter mock;
  nxandroid_context *context = NULL;

  reset_mock(&mock, &profile);
  mock.reenter_at = 7u;
  CHECK(run_with_mock(&profile, &mock, &context) == 0);
  CHECK(nxandroid_context_run(context) == NXANDROID_EREENTRANT);
  CHECK(check_reentrant_results(&mock) == 0);
  CHECK(nxandroid_context_get_state(context) == NXANDROID_CONTEXT_FAILED);
  CHECK(nxandroid_context_get_adapter_status(context) ==
        NXANDROID_EREENTRANT);
  context = mock.context;
  CHECK(nxandroid_context_destroy(&context) == NXANDROID_OK);

  reset_mock(&mock, &profile);
  mock.fail_at = 12u;
  mock.reenter_in_rollback = 1;
  CHECK(run_with_mock(&profile, &mock, &context) == 0);
  CHECK(nxandroid_context_run(context) == NXANDROID_EROLLBACK);
  CHECK(check_reentrant_results(&mock) == 0);
  CHECK(nxandroid_context_get_rollback_status(context) ==
        NXANDROID_EREENTRANT);
  context = mock.context;
  CHECK(nxandroid_context_destroy(&context) == NXANDROID_OK);
  return 0;
}

static int test_thousand_contexts(void) {
  nxandroid_profile profile =
      make_profile(normal_steps, ARRAY_SIZE(normal_steps), 0u);
  unsigned cycle;
  for (cycle = 0u; cycle < 1000u; ++cycle) {
    mock_adapter mock;
    nxandroid_context *context = NULL;
    reset_mock(&mock, &profile);
    CHECK(run_with_mock(&profile, &mock, &context) == 0);
    CHECK(nxandroid_context_run(context) == NXANDROID_OK);
    CHECK(mock.invoke_count == profile.step_count);
    CHECK(nxandroid_context_destroy(&context) == NXANDROID_OK);
  }
  return 0;
}

static int test_independent_contexts(void) {
  nxandroid_profile profile =
      make_profile(normal_steps, ARRAY_SIZE(normal_steps), 0u);
  mock_adapter first;
  mock_adapter second;
  nxandroid_context *first_context = NULL;
  nxandroid_context *second_context = NULL;

  reset_mock(&second, &profile);
  CHECK(run_with_mock(&profile, &second, &second_context) == 0);
  reset_mock(&first, &profile);
  first.other_context = second_context;
  first.run_other_at = 5u;
  CHECK(run_with_mock(&profile, &first, &first_context) == 0);
  CHECK(nxandroid_context_run(first_context) == NXANDROID_OK);
  CHECK(first.other_result == NXANDROID_OK);
  CHECK(nxandroid_context_get_next_step(second_context) == 1u);
  CHECK(nxandroid_context_run(second_context) == NXANDROID_OK);
  CHECK(nxandroid_context_get_state(first_context) ==
        NXANDROID_CONTEXT_COMPLETE);
  CHECK(nxandroid_context_get_state(second_context) ==
        NXANDROID_CONTEXT_COMPLETE);
  CHECK(nxandroid_context_destroy(&first_context) == NXANDROID_OK);
  CHECK(nxandroid_context_destroy(&second_context) == NXANDROID_OK);
  return 0;
}

static const nxandroid_step cycle_steps[] = {
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 0u, 0u, "cycle-00", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 1u, 0u, "cycle-01", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_JNI, 1u, 0u, "cycle-02", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_ACTIVITY_CREATE, NXANDROID_NO_MODULE, 0u, "cycle-03",
         NULL, NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_GRAPHICS_REQUEST, NXANDROID_NO_MODULE, 1u, "cycle-04",
         NULL, NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_SURFACE_UP, NXANDROID_NO_MODULE, 1u, "cycle-05", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_RESUME, NXANDROID_NO_MODULE, 1u, "cycle-06", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_FOCUS_GAIN, NXANDROID_NO_MODULE, 1u, "cycle-07", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_GL_READY, NXANDROID_NO_MODULE, 1u, "cycle-08", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_ENTRY, NXANDROID_NO_MODULE, 1u, "cycle-09", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_OBJECTS_READY, NXANDROID_NO_MODULE, 1u, "cycle-10",
         NULL, NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_FOCUS_LOSS, NXANDROID_NO_MODULE, 1u, "cycle-11", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_PAUSE, NXANDROID_NO_MODULE, 1u, "cycle-12", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_RESUME, NXANDROID_NO_MODULE, 1u, "cycle-13", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_FOCUS_GAIN, NXANDROID_NO_MODULE, 1u, "cycle-14", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_FOCUS_LOSS, NXANDROID_NO_MODULE, 1u, "cycle-15", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_PAUSE, NXANDROID_NO_MODULE, 1u, "cycle-16", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_SURFACE_DOWN, NXANDROID_NO_MODULE, 1u, "cycle-17", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_GRAPHICS_REQUEST, NXANDROID_NO_MODULE, 2u, "cycle-18",
         NULL, NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_SURFACE_UP, NXANDROID_NO_MODULE, 2u, "cycle-19", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_FOCUS_GAIN, NXANDROID_NO_MODULE, 2u, "cycle-20", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_RESUME, NXANDROID_NO_MODULE, 2u, "cycle-21", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_GL_READY, NXANDROID_NO_MODULE, 2u, "cycle-22", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_INPUT_ENABLE, NXANDROID_NO_MODULE, 2u, "cycle-23",
         NULL, NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_RUN_LOOP, NXANDROID_NO_MODULE, 2u, "cycle-24", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_FOCUS_LOSS, NXANDROID_NO_MODULE, 2u, "cycle-25", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_PAUSE, NXANDROID_NO_MODULE, 2u, "cycle-26", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_INPUT_DISABLE, NXANDROID_NO_MODULE, 2u, "cycle-27",
         NULL, NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_SAVE, NXANDROID_NO_MODULE, 0u, "cycle-28", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_SURFACE_DOWN, NXANDROID_NO_MODULE, 2u, "cycle-29", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_NATIVE_SHUTDOWN, NXANDROID_NO_MODULE, 0u, "cycle-30",
         NULL, NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_TERMINAL, NXANDROID_NO_MODULE, 0u, "cycle-31", NULL,
         NXANDROID_TERMINAL_RETURN),
};

static int test_surface_resume_cycles(void) {
  nxandroid_profile profile =
      make_profile(cycle_steps, ARRAY_SIZE(cycle_steps), 0u);
  mock_adapter mock;
  nxandroid_context *context = NULL;
  size_t index;
  unsigned graphics_requests = 0u;
  unsigned resumes = 0u;
  unsigned focus_gains = 0u;

  CHECK(nxandroid_profile_validate(&profile, NULL) == NXANDROID_OK);
  reset_mock(&mock, &profile);
  CHECK(run_with_mock(&profile, &mock, &context) == 0);
  CHECK(nxandroid_context_run(context) == NXANDROID_OK);
  for (index = 0u; index < ARRAY_SIZE(cycle_steps); ++index) {
    if (cycle_steps[index].phase == NXANDROID_PHASE_GRAPHICS_REQUEST)
      ++graphics_requests;
    if (cycle_steps[index].phase == NXANDROID_PHASE_RESUME)
      ++resumes;
    if (cycle_steps[index].phase == NXANDROID_PHASE_FOCUS_GAIN)
      ++focus_gains;
  }
  CHECK(graphics_requests == 2u);
  CHECK(resumes == 3u);
  CHECK(focus_gains == 3u);
  CHECK(nxandroid_context_destroy(&context) == NXANDROID_OK);
  return 0;
}

static int test_terminal_opt_in(void) {
  nxandroid_step steps[ARRAY_SIZE(normal_steps) - 2u];
  nxandroid_profile profile;
  mock_adapter mock;
  nxandroid_context *context = NULL;
  size_t bad = NXANDROID_NO_MODULE;

  memcpy(steps, normal_steps, 18u * sizeof(steps[0]));
  steps[18] = normal_steps[20];
  steps[18].terminal_policy = NXANDROID_TERMINAL_RETURN;
  steps[18].contract_id = "terminal-return-without-shutdown-invalid-v1";
  profile = make_profile(steps, ARRAY_SIZE(steps), 0u);
  CHECK(nxandroid_profile_validate(&profile, &bad) == NXANDROID_EPROFILE);
  CHECK(bad == 18u);

  steps[18].terminal_policy = NXANDROID_TERMINAL_ADAPTER;
  steps[18].contract_id = "terminal-proven-unsafe-destructors-v1";
  CHECK(nxandroid_profile_validate(&profile, &bad) == NXANDROID_EPROFILE);
  CHECK(bad == 18u);
  profile.flags = NXANDROID_PROFILE_ALLOW_ADAPTER_TERMINAL;
  CHECK(nxandroid_profile_validate(&profile, NULL) == NXANDROID_OK);
  reset_mock(&mock, &profile);
  CHECK(run_with_mock(&profile, &mock, &context) == 0);
  CHECK(nxandroid_context_run(context) == NXANDROID_OK);
  CHECK(mock.terminal_policy == NXANDROID_TERMINAL_ADAPTER);
  CHECK(strcmp(mock.terminal_contract_id,
               "terminal-proven-unsafe-destructors-v1") == 0);
  /* Reaching this assertion proves the core did not terminate the process. */
  CHECK(nxandroid_context_destroy(&context) == NXANDROID_OK);
  return 0;
}

static const nxandroid_module_spec kotor_modules_without_hidapi[] = {
    {"kotor-lzma", NXANDROID_JNI_NONE},
    {"kotor-miniz", NXANDROID_JNI_NONE},
    {"kotor-freetype", NXANDROID_JNI_NONE},
    {"kotor-fmod", NXANDROID_JNI_REQUIRED},
    {"kotor-android-port", NXANDROID_JNI_NONE},
    {"kotor-main", NXANDROID_JNI_NONE},
};

static const nxandroid_step kotor_steps_without_hidapi[] = {
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 0u, 0u,
         "kotor-lzma-initialized-v1", NULL, NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 1u, 0u,
         "kotor-miniz-initialized-v1", NULL, NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 2u, 0u,
         "kotor-freetype-initialized-v1", NULL, NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 3u, 0u,
         "kotor-fmod-initialized-v1", NULL, NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_JNI, 3u, 0u,
         "kotor-fmod-jni-onload-positive-v1", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 4u, 0u,
         "kotor-android-port-initialized-v1", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 5u, 0u,
         "kotor-main-initialized-v1", NULL, NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_ACTIVITY_CREATE, NXANDROID_NO_MODULE, 0u,
         "kotor-native-create-mutex-activity-v1", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_RESUME, NXANDROID_NO_MODULE, 1u,
         "kotor-native-on-resume-epoch-1-v1", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_RUNTIME_DELEGATED, NXANDROID_NO_MODULE, 0u,
         "kotor-obb-sdl-main-delegated-runtime-v1", NULL,
         NXANDROID_TERMINAL_NONE),
};

static const nxandroid_module_spec kotor_modules_with_hidapi[] = {
    {"kotor-lzma", NXANDROID_JNI_NONE},
    {"kotor-miniz", NXANDROID_JNI_NONE},
    {"kotor-freetype", NXANDROID_JNI_NONE},
    {"kotor-fmod", NXANDROID_JNI_REQUIRED},
    {"kotor-hidapi", NXANDROID_JNI_NONE},
    {"kotor-android-port", NXANDROID_JNI_NONE},
    {"kotor-main", NXANDROID_JNI_NONE},
};

static const nxandroid_step kotor_steps_with_hidapi[] = {
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 0u, 0u,
         "kotor-lzma-initialized-v1", NULL, NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 1u, 0u,
         "kotor-miniz-initialized-v1", NULL, NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 2u, 0u,
         "kotor-freetype-initialized-v1", NULL, NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 3u, 0u,
         "kotor-fmod-initialized-v1", NULL, NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_JNI, 3u, 0u,
         "kotor-fmod-jni-onload-positive-v1", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 4u, 0u,
         "kotor-hidapi-initialized-v1", NULL, NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 5u, 0u,
         "kotor-android-port-initialized-v1", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_MODULE_INITIALIZED, 6u, 0u,
         "kotor-main-initialized-v1", NULL, NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_ACTIVITY_CREATE, NXANDROID_NO_MODULE, 0u,
         "kotor-native-create-mutex-activity-v1", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_RESUME, NXANDROID_NO_MODULE, 1u,
         "kotor-native-on-resume-epoch-1-v1", NULL,
         NXANDROID_TERMINAL_NONE),
    STEP(NXANDROID_PHASE_RUNTIME_DELEGATED, NXANDROID_NO_MODULE, 0u,
         "kotor-obb-sdl-main-delegated-runtime-v1", NULL,
         NXANDROID_TERMINAL_NONE),
};

static nxandroid_profile make_kotor_profile(
    const nxandroid_module_spec *modules, size_t module_count,
    const nxandroid_step *steps, size_t step_count, uint32_t flags) {
  nxandroid_profile profile;
  memset(&profile, 0, sizeof(profile));
  profile.api_version = NXANDROID_API_VERSION;
  profile.struct_size = sizeof(profile);
  profile.modules = modules;
  profile.module_count = module_count;
  profile.steps = steps;
  profile.step_count = step_count;
  profile.flags = flags;
  return profile;
}

static int run_kotor_profile(const nxandroid_profile *profile) {
  mock_adapter mock;
  nxandroid_context *context = NULL;
  size_t index;

  CHECK(nxandroid_profile_validate(profile, NULL) == NXANDROID_OK);
  reset_mock(&mock, profile);
  CHECK(run_with_mock(profile, &mock, &context) == 0);
  CHECK(nxandroid_context_run(context) == NXANDROID_OK);
  CHECK(nxandroid_context_get_state(context) == NXANDROID_CONTEXT_COMPLETE);
  CHECK(mock.invoke_count == profile->step_count);
  for (index = 0u; index < profile->step_count; ++index)
    CHECK(mock.invoked[index] == index);
  CHECK(profile->steps[profile->step_count - 1u].phase ==
        NXANDROID_PHASE_RUNTIME_DELEGATED);
  CHECK(strcmp(profile->steps[profile->step_count - 1u].contract_id,
               "kotor-obb-sdl-main-delegated-runtime-v1") == 0);
  CHECK(nxandroid_context_destroy(&context) == NXANDROID_OK);
  return 0;
}

static int test_delegated_runtime(void) {
  static const char *const expected_without_hidapi[] = {
      "kotor-lzma", "kotor-miniz", "kotor-freetype", "kotor-fmod",
      "kotor-android-port", "kotor-main",
  };
  static const char *const expected_with_hidapi[] = {
      "kotor-lzma", "kotor-miniz", "kotor-freetype", "kotor-fmod",
      "kotor-hidapi", "kotor-android-port", "kotor-main",
  };
  nxandroid_profile without_hidapi = make_kotor_profile(
      kotor_modules_without_hidapi, ARRAY_SIZE(kotor_modules_without_hidapi),
      kotor_steps_without_hidapi, ARRAY_SIZE(kotor_steps_without_hidapi), 0u);
  nxandroid_profile with_hidapi = make_kotor_profile(
      kotor_modules_with_hidapi, ARRAY_SIZE(kotor_modules_with_hidapi),
      kotor_steps_with_hidapi, ARRAY_SIZE(kotor_steps_with_hidapi),
      NXANDROID_PROFILE_ALLOW_DELEGATED_RUNTIME);
  mock_adapter mock;
  nxandroid_context *context = NULL;
  size_t bad = NXANDROID_NO_MODULE;
  size_t index;

  CHECK(ARRAY_SIZE(kotor_modules_without_hidapi) == 6u);
  CHECK(ARRAY_SIZE(kotor_modules_with_hidapi) == 7u);
  CHECK(ARRAY_SIZE(expected_without_hidapi) ==
        ARRAY_SIZE(kotor_modules_without_hidapi));
  CHECK(ARRAY_SIZE(expected_with_hidapi) ==
        ARRAY_SIZE(kotor_modules_with_hidapi));
  for (index = 0u; index < ARRAY_SIZE(expected_without_hidapi); ++index)
    CHECK(strcmp(kotor_modules_without_hidapi[index].name,
                 expected_without_hidapi[index]) == 0);
  for (index = 0u; index < ARRAY_SIZE(expected_with_hidapi); ++index)
    CHECK(strcmp(kotor_modules_with_hidapi[index].name,
                 expected_with_hidapi[index]) == 0);
  CHECK(strcmp(kotor_modules_without_hidapi[3].name, "kotor-fmod") == 0);
  CHECK(kotor_modules_without_hidapi[3].jni_policy ==
        NXANDROID_JNI_REQUIRED);
  CHECK(kotor_steps_without_hidapi[3].phase ==
        NXANDROID_PHASE_MODULE_INITIALIZED);
  CHECK(kotor_steps_without_hidapi[4].phase == NXANDROID_PHASE_MODULE_JNI);
  CHECK(kotor_steps_without_hidapi[3].module_index == 3u);
  CHECK(kotor_steps_without_hidapi[4].module_index == 3u);
  CHECK(nxandroid_profile_validate(&without_hidapi, &bad) ==
        NXANDROID_EPROFILE);
  CHECK(bad == ARRAY_SIZE(kotor_steps_without_hidapi) - 1u);
  without_hidapi.flags = NXANDROID_PROFILE_ALLOW_DELEGATED_RUNTIME;
  CHECK(run_kotor_profile(&without_hidapi) == 0);
  CHECK(run_kotor_profile(&with_hidapi) == 0);

  for (index = 0u; index < with_hidapi.step_count; ++index) {
    CHECK(with_hidapi.steps[index].phase != NXANDROID_PHASE_SURFACE_UP);
    CHECK(with_hidapi.steps[index].phase != NXANDROID_PHASE_GL_READY);
    CHECK(with_hidapi.steps[index].phase != NXANDROID_PHASE_ENTRY);
    CHECK(with_hidapi.steps[index].phase != NXANDROID_PHASE_INPUT_ENABLE);
    CHECK(with_hidapi.steps[index].phase != NXANDROID_PHASE_RUN_LOOP);
    CHECK(with_hidapi.steps[index].phase != NXANDROID_PHASE_TERMINAL);
  }

  reset_mock(&mock, &with_hidapi);
  mock.fail_at = with_hidapi.step_count - 1u;
  CHECK(run_with_mock(&with_hidapi, &mock, &context) == 0);
  CHECK(nxandroid_context_run(context) == NXANDROID_ECALLBACK);
  CHECK(nxandroid_context_get_state(context) == NXANDROID_CONTEXT_FAILED);
  CHECK(mock.invoke_count == with_hidapi.step_count);
  CHECK(nxandroid_context_destroy(&context) == NXANDROID_OK);
  return 0;
}

static const nxandroid_catalog_entry catalog_entries[] = {
    {"GetEnv", NXANDROID_ABI_AARCH64_BIONIC, NXANDROID_IMPORT_JNI,
     NXANDROID_SYMBOL_FUNCTION, NXANDROID_IMPORT_CRITICAL,
     "jni-vm-getenv-1.4-v1", (uintptr_t)0x4000u,
     NXANDROID_PROVIDER_IMPLEMENTATION, NULL},
    {"ASensorManager_getInstance", NXANDROID_ABI_ARMV7_BIONIC,
     NXANDROID_IMPORT_NDK, NXANDROID_SYMBOL_FUNCTION,
     NXANDROID_IMPORT_OPTIONAL, "ndk-sensor-unavailable-v1",
     (uintptr_t)0x3000u, NXANDROID_PROVIDER_STUB,
     "returns NULL; leaves errno unchanged; no side effects"},
    {"__sF", NXANDROID_ABI_ARMV7_BIONIC, NXANDROID_IMPORT_BIONIC,
     NXANDROID_SYMBOL_DATA, NXANDROID_IMPORT_CRITICAL, "bionic-file84-v1",
     (uintptr_t)0x2000u, NXANDROID_PROVIDER_IMPLEMENTATION, NULL},
    {"malloc", NXANDROID_ABI_ARMV7_BIONIC, NXANDROID_IMPORT_BIONIC,
     NXANDROID_SYMBOL_FUNCTION, NXANDROID_IMPORT_CRITICAL,
     "bionic-malloc-armv7-v1", (uintptr_t)0x1000u,
     NXANDROID_PROVIDER_IMPLEMENTATION, NULL},
};

static nxandroid_import_request make_request(
    const char *name, nxandroid_abi abi, nxandroid_import_domain domain,
    nxandroid_symbol_kind symbol_kind,
    nxandroid_import_criticality criticality, nxandroid_import_binding binding,
    const char *contract_id, int allow_stub) {
  nxandroid_import_request request;
  request.name = name;
  request.abi = abi;
  request.domain = domain;
  request.symbol_kind = symbol_kind;
  request.criticality = criticality;
  request.binding = binding;
  request.contract_id = contract_id;
  request.allow_stub = allow_stub;
  return request;
}

static void set_binding_sentinel(nxandroid_import_binding_result *binding) {
  binding->resolved = 77;
  binding->address = (uintptr_t)0xdeadbeefu;
  binding->provider_kind = NXANDROID_PROVIDER_STUB;
  binding->contract_id = "unchanged-contract";
  binding->stub_semantics = "unchanged-semantics";
}

static int binding_is_sentinel(
    const nxandroid_import_binding_result *binding) {
  return binding->resolved == 77 && binding->address == (uintptr_t)0xdeadbeefu &&
         binding->provider_kind == NXANDROID_PROVIDER_STUB &&
         strcmp(binding->contract_id, "unchanged-contract") == 0 &&
         strcmp(binding->stub_semantics, "unchanged-semantics") == 0;
}

static int test_import_policy_success(void) {
  nxandroid_import_catalog *catalog = NULL;
  nxandroid_import_request requests[4];
  nxandroid_import_binding_result bindings[4];
  size_t bad = 99u;

  CHECK(nxandroid_import_catalog_create(catalog_entries,
                                        ARRAY_SIZE(catalog_entries),
                                        &catalog) == NXANDROID_OK);
  requests[0] = make_request(
      "malloc", NXANDROID_ABI_ARMV7_BIONIC, NXANDROID_IMPORT_BIONIC,
      NXANDROID_SYMBOL_FUNCTION, NXANDROID_IMPORT_CRITICAL,
      NXANDROID_BIND_STRONG, "bionic-malloc-armv7-v1", 0);
  requests[1] = make_request(
      "__sF", NXANDROID_ABI_ARMV7_BIONIC, NXANDROID_IMPORT_BIONIC,
      NXANDROID_SYMBOL_DATA, NXANDROID_IMPORT_CRITICAL, NXANDROID_BIND_STRONG,
      "bionic-file84-v1", 0);
  requests[2] = make_request(
      "ASensorManager_getInstance", NXANDROID_ABI_ARMV7_BIONIC,
      NXANDROID_IMPORT_NDK, NXANDROID_SYMBOL_FUNCTION,
      NXANDROID_IMPORT_OPTIONAL, NXANDROID_BIND_WEAK,
      "ndk-sensor-unavailable-v1", 1);
  requests[3] = make_request(
      "ANativeWindow_optionalFeature", NXANDROID_ABI_ARMV7_BIONIC,
      NXANDROID_IMPORT_NDK, NXANDROID_SYMBOL_FUNCTION,
      NXANDROID_IMPORT_OPTIONAL, NXANDROID_BIND_WEAK,
      "ndk-optional-absent-v1", 0);
  memset(bindings, 0, sizeof(bindings));
  CHECK(nxandroid_imports_resolve(catalog, requests, ARRAY_SIZE(requests),
                                  bindings, &bad) == NXANDROID_OK);
  CHECK(bad == NXANDROID_NO_MODULE);
  CHECK(bindings[0].resolved && bindings[0].address == (uintptr_t)0x1000u);
  CHECK(bindings[1].resolved && bindings[1].address == (uintptr_t)0x2000u);
  CHECK(bindings[2].resolved &&
        bindings[2].provider_kind == NXANDROID_PROVIDER_STUB);
  CHECK(strcmp(bindings[2].stub_semantics,
               "returns NULL; leaves errno unchanged; no side effects") == 0);
  CHECK(!bindings[3].resolved && bindings[3].address == 0u);
  nxandroid_import_catalog_destroy(&catalog);
  CHECK(catalog == NULL);
  return 0;
}

static int expect_resolve_failure(
    nxandroid_import_catalog *catalog, const nxandroid_import_request *request,
    nxandroid_result expected) {
  nxandroid_import_binding_result binding;
  size_t bad = NXANDROID_NO_MODULE;
  set_binding_sentinel(&binding);
  CHECK(nxandroid_imports_resolve(catalog, request, 1u, &binding, &bad) ==
        expected);
  CHECK(bad == 0u);
  CHECK(binding_is_sentinel(&binding));
  return 0;
}

static int test_import_policy_fail_closed(void) {
  nxandroid_import_catalog *catalog = NULL;
  nxandroid_import_request request;
  nxandroid_import_request pair[2];
  nxandroid_import_binding_result bindings[2];
  size_t bad = NXANDROID_NO_MODULE;

  CHECK(nxandroid_import_catalog_create(catalog_entries,
                                        ARRAY_SIZE(catalog_entries),
                                        &catalog) == NXANDROID_OK);

  request = make_request(
      "critical_missing", NXANDROID_ABI_ARMV7_BIONIC,
      NXANDROID_IMPORT_BIONIC, NXANDROID_SYMBOL_FUNCTION,
      NXANDROID_IMPORT_CRITICAL, NXANDROID_BIND_WEAK,
      "bionic-critical-missing-v1", 0);
  CHECK(expect_resolve_failure(catalog, &request, NXANDROID_EUNRESOLVED) == 0);

  request = make_request(
      "strong_optional_missing", NXANDROID_ABI_ARMV7_BIONIC,
      NXANDROID_IMPORT_BIONIC, NXANDROID_SYMBOL_FUNCTION,
      NXANDROID_IMPORT_OPTIONAL, NXANDROID_BIND_STRONG,
      "bionic-strong-optional-v1", 0);
  CHECK(expect_resolve_failure(catalog, &request, NXANDROID_EUNRESOLVED) == 0);

  request = make_request(
      "malloc", NXANDROID_ABI_ARMV7_BIONIC, NXANDROID_IMPORT_BIONIC,
      NXANDROID_SYMBOL_FUNCTION, NXANDROID_IMPORT_CRITICAL,
      NXANDROID_BIND_STRONG, "wrong-contract", 0);
  CHECK(expect_resolve_failure(catalog, &request, NXANDROID_ECONTRACT) == 0);

  request = make_request(
      "malloc", NXANDROID_ABI_ARMV7_BIONIC, NXANDROID_IMPORT_BIONIC,
      NXANDROID_SYMBOL_DATA, NXANDROID_IMPORT_CRITICAL, NXANDROID_BIND_STRONG,
      "bionic-malloc-armv7-v1", 0);
  CHECK(expect_resolve_failure(catalog, &request, NXANDROID_ECONTRACT) == 0);

  request = make_request(
      "malloc", NXANDROID_ABI_AARCH64_BIONIC, NXANDROID_IMPORT_BIONIC,
      NXANDROID_SYMBOL_FUNCTION, NXANDROID_IMPORT_CRITICAL,
      NXANDROID_BIND_STRONG, "bionic-malloc-armv7-v1", 0);
  CHECK(expect_resolve_failure(catalog, &request, NXANDROID_EUNRESOLVED) == 0);

  request = make_request(
      "ASensorManager_getInstance", NXANDROID_ABI_ARMV7_BIONIC,
      NXANDROID_IMPORT_NDK, NXANDROID_SYMBOL_FUNCTION,
      NXANDROID_IMPORT_OPTIONAL, NXANDROID_BIND_WEAK,
      "ndk-sensor-unavailable-v1", 0);
  CHECK(expect_resolve_failure(catalog, &request, NXANDROID_ECONTRACT) == 0);

  pair[0] = make_request(
      "malloc", NXANDROID_ABI_ARMV7_BIONIC, NXANDROID_IMPORT_BIONIC,
      NXANDROID_SYMBOL_FUNCTION, NXANDROID_IMPORT_CRITICAL,
      NXANDROID_BIND_STRONG, "bionic-malloc-armv7-v1", 0);
  pair[1] = make_request(
      "critical_missing", NXANDROID_ABI_ARMV7_BIONIC,
      NXANDROID_IMPORT_BIONIC, NXANDROID_SYMBOL_FUNCTION,
      NXANDROID_IMPORT_CRITICAL, NXANDROID_BIND_WEAK,
      "bionic-critical-missing-v1", 0);
  set_binding_sentinel(&bindings[0]);
  set_binding_sentinel(&bindings[1]);
  CHECK(nxandroid_imports_resolve(catalog, pair, ARRAY_SIZE(pair), bindings,
                                  &bad) == NXANDROID_EUNRESOLVED);
  CHECK(bad == 1u);
  CHECK(binding_is_sentinel(&bindings[0]));
  CHECK(binding_is_sentinel(&bindings[1]));

  nxandroid_import_catalog_destroy(&catalog);
  return 0;
}

static int test_catalog_validation(void) {
  nxandroid_import_catalog *catalog = NULL;
  nxandroid_catalog_entry entries[2];

  entries[0] = catalog_entries[0];
  entries[1] = catalog_entries[0];
  CHECK(nxandroid_import_catalog_create(entries, ARRAY_SIZE(entries), &catalog) ==
        NXANDROID_ECATALOG);
  CHECK(catalog == NULL);

  entries[0] = catalog_entries[1];
  entries[0].stub_semantics = NULL;
  CHECK(nxandroid_import_catalog_create(entries, 1u, &catalog) ==
        NXANDROID_ECATALOG);

  entries[0] = catalog_entries[0];
  entries[0].stub_semantics = "generic zero";
  CHECK(nxandroid_import_catalog_create(entries, 1u, &catalog) ==
        NXANDROID_ECATALOG);

  entries[0] = catalog_entries[0];
  entries[0].address = 0u;
  CHECK(nxandroid_import_catalog_create(entries, 1u, &catalog) ==
        NXANDROID_ECATALOG);
  return 0;
}

static int test_public_limits(void) {
  nxandroid_step steps[ARRAY_SIZE(normal_steps)];
  nxandroid_module_spec modules[ARRAY_SIZE(normal_modules)];
  nxandroid_profile profile;
  nxandroid_catalog_entry entry;
  nxandroid_import_catalog *catalog = NULL;
  nxandroid_import_request request;
  nxandroid_import_binding_result binding;
  char long_contract[NXANDROID_CONTRACT_ID_MAX + 2u];
  char long_module[NXANDROID_MODULE_NAME_MAX + 2u];
  char long_symbol[NXANDROID_SYMBOL_NAME_MAX + 2u];
  char long_semantics[NXANDROID_STUB_SEMANTICS_MAX + 2u];

  memset(long_contract, 'c', sizeof(long_contract));
  long_contract[sizeof(long_contract) - 1u] = '\0';
  memcpy(steps, normal_steps, sizeof(steps));
  steps[0].contract_id = long_contract;
  profile = make_profile(steps, ARRAY_SIZE(steps), 0u);
  CHECK(nxandroid_profile_validate(&profile, NULL) == NXANDROID_EPROFILE);

  memset(long_module, 'm', sizeof(long_module));
  long_module[sizeof(long_module) - 1u] = '\0';
  memcpy(modules, normal_modules, sizeof(modules));
  modules[0].name = long_module;
  profile = make_profile(normal_steps, ARRAY_SIZE(normal_steps), 0u);
  profile.modules = modules;
  CHECK(nxandroid_profile_validate(&profile, NULL) == NXANDROID_EPROFILE);

  profile = make_profile(normal_steps, ARRAY_SIZE(normal_steps), 0u);
  profile.module_count = NXANDROID_MAX_MODULES + 1u;
  CHECK(nxandroid_profile_validate(&profile, NULL) == NXANDROID_EINVAL);
  profile = make_profile(normal_steps, ARRAY_SIZE(normal_steps), 0u);
  profile.step_count = NXANDROID_MAX_STEPS + 1u;
  CHECK(nxandroid_profile_validate(&profile, NULL) == NXANDROID_EINVAL);

  memset(long_symbol, 's', sizeof(long_symbol));
  long_symbol[sizeof(long_symbol) - 1u] = '\0';
  entry = catalog_entries[0];
  entry.name = long_symbol;
  CHECK(nxandroid_import_catalog_create(&entry, 1u, &catalog) ==
        NXANDROID_ECATALOG);
  CHECK(nxandroid_import_catalog_create(catalog_entries,
                                        NXANDROID_MAX_CATALOG_ENTRIES + 1u,
                                        &catalog) == NXANDROID_EINVAL);

  memset(long_semantics, 'x', sizeof(long_semantics));
  long_semantics[sizeof(long_semantics) - 1u] = '\0';
  entry = catalog_entries[1];
  entry.stub_semantics = long_semantics;
  CHECK(nxandroid_import_catalog_create(&entry, 1u, &catalog) ==
        NXANDROID_ECATALOG);

  CHECK(nxandroid_import_catalog_create(catalog_entries,
                                        ARRAY_SIZE(catalog_entries),
                                        &catalog) == NXANDROID_OK);
  request = make_request(
      "malloc", NXANDROID_ABI_ARMV7_BIONIC, NXANDROID_IMPORT_BIONIC,
      NXANDROID_SYMBOL_FUNCTION, NXANDROID_IMPORT_CRITICAL,
      NXANDROID_BIND_STRONG, "bionic-malloc-armv7-v1", 0);
  set_binding_sentinel(&binding);
  CHECK(nxandroid_imports_resolve(catalog, &request,
                                  NXANDROID_MAX_IMPORT_REQUESTS + 1u,
                                  &binding, NULL) == NXANDROID_EINVAL);
  CHECK(binding_is_sentinel(&binding));
  nxandroid_import_catalog_destroy(&catalog);
  return 0;
}

static int utf16_view_matches(const nxandroid_utf16_view *view,
                              const uint16_t *units, size_t unit_count) {
  if (view == NULL || view->unit_count != unit_count)
    return 0;
  if (unit_count == 0u)
    return view->units == NULL;
  return view->units != NULL &&
         memcmp(view->units, units, unit_count * sizeof(*units)) == 0;
}

static const nxandroid_prefs_entry_view *find_snapshot_entry(
    const nxandroid_prefs_snapshot *snapshot, const uint16_t *key,
    size_t key_count) {
  size_t index;

  for (index = 0u; index < nxandroid_prefs_snapshot_count(snapshot); ++index) {
    const nxandroid_prefs_entry_view *entry =
        nxandroid_prefs_snapshot_entry_at(snapshot, index);
    if (entry != NULL && entry->key.unit_count == key_count &&
        (key_count == 0u ||
         memcmp(entry->key.units, key, key_count * sizeof(*key)) == 0))
      return entry;
  }
  return NULL;
}

static int test_utf16_region_adapter(void) {
  uint16_t source_units[] = {0x0041u, 0x00e9u, 0x4e2du,
                             0xd83du, 0xde00u, 0x005au};
  uint16_t output[] = {0xaaaau, 0xbbbbu, 0xccccu,
                       0xddddu, 0xeeeeu, 0xffffu};
  uint16_t before[ARRAY_SIZE(output)];
  uint16_t overlap[] = {1u, 2u, 3u, 4u};
  nxandroid_utf16_view source = {source_units, ARRAY_SIZE(source_units)};
  nxandroid_utf16_view empty = {NULL, 0u};
  nxandroid_utf16_view invalid = {NULL, 1u};
  nxandroid_utf16_view overlap_view = {overlap, ARRAY_SIZE(overlap)};

  CHECK(NXANDROID_JNI_SLOT_GET_STRING_REGION == 220u);
  CHECK(NXANDROID_JNI_SLOT_GET_STRING_UTF_REGION == 221u);
  CHECK(NXANDROID_JNI_SLOT_GET_STRING_REGION !=
        NXANDROID_JNI_SLOT_GET_STRING_UTF_REGION);
  CHECK(strcmp(nxandroid_jni_adapter_result_string(
                   NXANDROID_JNI_ADAPTER_EBOUNDS),
               "UTF-16 region out of bounds") == 0);

  CHECK(nxandroid_utf16_copy_region(&source, 1, 4, &output[1]) ==
        NXANDROID_JNI_ADAPTER_OK);
  CHECK(output[0] == 0xaaaau);
  CHECK(output[1] == 0x00e9u);
  CHECK(output[2] == 0x4e2du);
  CHECK(output[3] == 0xd83du);
  CHECK(output[4] == 0xde00u);
  CHECK(output[5] == 0xffffu); /* no implicit UTF-16 terminator */

  output[2] = 0x7777u;
  CHECK(nxandroid_utf16_copy_region(&source, 3, 1, &output[2]) ==
        NXANDROID_JNI_ADAPTER_OK);
  CHECK(output[2] == 0xd83du); /* a region is code-unit, not code-point based */

  CHECK(nxandroid_utf16_copy_region(&source, 6, 0, NULL) ==
        NXANDROID_JNI_ADAPTER_OK);
  CHECK(nxandroid_utf16_copy_region(&empty, 0, 0, NULL) ==
        NXANDROID_JNI_ADAPTER_OK);

  CHECK(nxandroid_utf16_copy_region(&overlap_view, 0, 3, &overlap[1]) ==
        NXANDROID_JNI_ADAPTER_OK);
  CHECK(overlap[0] == 1u && overlap[1] == 1u && overlap[2] == 2u &&
        overlap[3] == 3u);

  memcpy(before, output, sizeof(output));
  CHECK(nxandroid_utf16_copy_region(NULL, 0, 1, output) ==
        NXANDROID_JNI_ADAPTER_EINVAL);
  CHECK(memcmp(output, before, sizeof(output)) == 0);
  CHECK(nxandroid_utf16_copy_region(&invalid, 0, 1, output) ==
        NXANDROID_JNI_ADAPTER_EINVAL);
  CHECK(memcmp(output, before, sizeof(output)) == 0);
  CHECK(nxandroid_utf16_copy_region(&source, 0, 1, NULL) ==
        NXANDROID_JNI_ADAPTER_EINVAL);
  CHECK(nxandroid_utf16_copy_region(&source, -1, 1, output) ==
        NXANDROID_JNI_ADAPTER_EBOUNDS);
  CHECK(nxandroid_utf16_copy_region(&source, 0, -1, output) ==
        NXANDROID_JNI_ADAPTER_EBOUNDS);
  CHECK(nxandroid_utf16_copy_region(&source, 7, 0, output) ==
        NXANDROID_JNI_ADAPTER_EBOUNDS);
  CHECK(nxandroid_utf16_copy_region(&source, 5, 2, output) ==
        NXANDROID_JNI_ADAPTER_EBOUNDS);
  CHECK(nxandroid_utf16_copy_region(&source, INT32_MAX, INT32_MAX, output) ==
        NXANDROID_JNI_ADAPTER_EBOUNDS);
  CHECK(memcmp(output, before, sizeof(output)) == 0);
  return 0;
}

static int test_typed_preferences_snapshots(void) {
  uint16_t key_string[] = {0x006eu, 0x0061u, 0x006du, 0x0065u, 0x4e2du};
  uint16_t key_set[] = {0x0073u, 0x0065u, 0x0074u};
  uint16_t key_int32[] = {0x0069u, 0x0033u, 0x0032u};
  uint16_t key_int64[] = {0x0069u, 0x0036u, 0x0034u};
  uint16_t key_float[] = {0x0066u, 0x0033u, 0x0032u};
  uint16_t key_bool[] = {0x0062u, 0x006fu, 0x006fu, 0x006cu};
  uint16_t string_value[] = {0x006fu, 0x006cu, 0x00e1u};
  uint16_t string_value_original[ARRAY_SIZE(string_value)];
  uint16_t set_emoji[] = {0xd83du, 0xde00u};
  nxandroid_utf16_view set_members[] = {
      {NULL, 0u}, {set_emoji, ARRAY_SIZE(set_emoji)}};
  nxandroid_prefs_entry_view source[6];
  nxandroid_prefs_snapshot *first = NULL;
  nxandroid_prefs_snapshot *second = NULL;
  nxandroid_prefs_snapshot *empty = NULL;
  const nxandroid_prefs_entry_view *entry;
  uint32_t float_bits = UINT32_C(0x7fc12345);
  uint32_t observed_float_bits = 0u;

  memset(source, 0, sizeof(source));
  memcpy(string_value_original, string_value, sizeof(string_value));
  source[0].key = (nxandroid_utf16_view){key_string, ARRAY_SIZE(key_string)};
  source[0].type = NXANDROID_PREFS_STRING;
  source[0].value.string_value =
      (nxandroid_utf16_view){string_value, ARRAY_SIZE(string_value)};
  source[1].key = (nxandroid_utf16_view){key_set, ARRAY_SIZE(key_set)};
  source[1].type = NXANDROID_PREFS_STRING_SET;
  source[1].value.string_set_value =
      (nxandroid_prefs_string_set_view){set_members, ARRAY_SIZE(set_members)};
  source[2].key = (nxandroid_utf16_view){key_int32, ARRAY_SIZE(key_int32)};
  source[2].type = NXANDROID_PREFS_INT32;
  source[2].value.int32_value = INT32_C(-1234567);
  source[3].key = (nxandroid_utf16_view){key_int64, ARRAY_SIZE(key_int64)};
  source[3].type = NXANDROID_PREFS_INT64;
  source[3].value.int64_value = INT64_C(0x123456789abcdef);
  source[4].key = (nxandroid_utf16_view){key_float, ARRAY_SIZE(key_float)};
  source[4].type = NXANDROID_PREFS_FLOAT;
  memcpy(&source[4].value.float_value, &float_bits, sizeof(float_bits));
  source[5].key = (nxandroid_utf16_view){key_bool, ARRAY_SIZE(key_bool)};
  source[5].type = NXANDROID_PREFS_BOOL;
  source[5].value.bool_value = 1;

  CHECK(nxandroid_prefs_snapshot_create(NULL, 0u, &empty) ==
        NXANDROID_JNI_ADAPTER_OK);
  CHECK(empty != NULL);
  CHECK(nxandroid_prefs_snapshot_count(empty) == 0u);
  CHECK(nxandroid_prefs_snapshot_entry_at(empty, 0u) == NULL);
  nxandroid_prefs_snapshot_destroy(&empty);
  nxandroid_prefs_snapshot_destroy(&empty);
  nxandroid_prefs_snapshot_destroy(NULL);

  CHECK(nxandroid_prefs_snapshot_create(source, ARRAY_SIZE(source), &first) ==
        NXANDROID_JNI_ADAPTER_OK);
  CHECK(nxandroid_prefs_snapshot_count(first) == ARRAY_SIZE(source));
  CHECK(nxandroid_prefs_snapshot_count(NULL) == 0u);
  CHECK(nxandroid_prefs_snapshot_entry_at(NULL, 0u) == NULL);
  CHECK(nxandroid_prefs_snapshot_entry_at(first, ARRAY_SIZE(source)) == NULL);

  entry = find_snapshot_entry(first, key_string, ARRAY_SIZE(key_string));
  CHECK(entry != NULL && entry->type == NXANDROID_PREFS_STRING);
  CHECK(utf16_view_matches(&entry->value.string_value, string_value_original,
                           ARRAY_SIZE(string_value_original)));
  entry = find_snapshot_entry(first, key_set, ARRAY_SIZE(key_set));
  CHECK(entry != NULL && entry->type == NXANDROID_PREFS_STRING_SET);
  CHECK(entry->value.string_set_value.member_count == 2u);
  CHECK(utf16_view_matches(&entry->value.string_set_value.members[0], NULL,
                           0u));
  CHECK(utf16_view_matches(&entry->value.string_set_value.members[1],
                           set_emoji, ARRAY_SIZE(set_emoji)));
  entry = find_snapshot_entry(first, key_int32, ARRAY_SIZE(key_int32));
  CHECK(entry != NULL && entry->value.int32_value == INT32_C(-1234567));
  entry = find_snapshot_entry(first, key_int64, ARRAY_SIZE(key_int64));
  CHECK(entry != NULL &&
        entry->value.int64_value == INT64_C(0x123456789abcdef));
  entry = find_snapshot_entry(first, key_float, ARRAY_SIZE(key_float));
  CHECK(entry != NULL && entry->type == NXANDROID_PREFS_FLOAT);
  memcpy(&observed_float_bits, &entry->value.float_value,
         sizeof(observed_float_bits));
  CHECK(observed_float_bits == float_bits);
  entry = find_snapshot_entry(first, key_bool, ARRAY_SIZE(key_bool));
  CHECK(entry != NULL && entry->type == NXANDROID_PREFS_BOOL &&
        entry->value.bool_value == 1);

  string_value[0] = 0x0058u;
  set_emoji[0] = 0x0059u;
  source[2].value.int32_value = 42;
  source[5].value.bool_value = 0;
  CHECK(nxandroid_prefs_snapshot_create(source, ARRAY_SIZE(source), &second) ==
        NXANDROID_JNI_ADAPTER_OK);

  entry = find_snapshot_entry(first, key_string, ARRAY_SIZE(key_string));
  CHECK(utf16_view_matches(&entry->value.string_value, string_value_original,
                           ARRAY_SIZE(string_value_original)));
  entry = find_snapshot_entry(first, key_set, ARRAY_SIZE(key_set));
  CHECK(entry->value.string_set_value.members[1].units[0] == 0xd83du);
  entry = find_snapshot_entry(first, key_int32, ARRAY_SIZE(key_int32));
  CHECK(entry->value.int32_value == INT32_C(-1234567));

  nxandroid_prefs_snapshot_destroy(&first);
  entry = find_snapshot_entry(second, key_string, ARRAY_SIZE(key_string));
  CHECK(entry != NULL && entry->value.string_value.units[0] == 0x0058u);
  entry = find_snapshot_entry(second, key_set, ARRAY_SIZE(key_set));
  CHECK(entry != NULL &&
        entry->value.string_set_value.members[1].units[0] == 0x0059u);
  entry = find_snapshot_entry(second, key_int32, ARRAY_SIZE(key_int32));
  CHECK(entry != NULL && entry->value.int32_value == 42);
  entry = find_snapshot_entry(second, key_bool, ARRAY_SIZE(key_bool));
  CHECK(entry != NULL && entry->value.bool_value == 0);
  nxandroid_prefs_snapshot_destroy(&second);
  return 0;
}

static int expect_snapshot_failure(nxandroid_prefs_entry_view *entries,
                                   size_t entry_count,
                                   nxandroid_jni_adapter_result expected) {
  nxandroid_prefs_snapshot *snapshot =
      (nxandroid_prefs_snapshot *)(uintptr_t)1u;
  CHECK(nxandroid_prefs_snapshot_create(entries, entry_count, &snapshot) ==
        expected);
  CHECK(snapshot == NULL);
  return 0;
}

static int test_preferences_snapshot_validation(void) {
  uint16_t key[] = {0x006bu};
  uint16_t value[] = {0x0076u};
  nxandroid_utf16_view duplicate_members[] = {
      {value, ARRAY_SIZE(value)}, {value, ARRAY_SIZE(value)}};
  nxandroid_prefs_entry_view entries[2];
  nxandroid_prefs_snapshot *empty_values = NULL;

  memset(entries, 0, sizeof(entries));
  entries[0].key = (nxandroid_utf16_view){NULL, 0u};
  entries[0].type = NXANDROID_PREFS_STRING;
  entries[0].value.string_value = (nxandroid_utf16_view){NULL, 0u};
  entries[1].key = (nxandroid_utf16_view){key, ARRAY_SIZE(key)};
  entries[1].type = NXANDROID_PREFS_STRING_SET;
  entries[1].value.string_set_value =
      (nxandroid_prefs_string_set_view){NULL, 0u};
  CHECK(nxandroid_prefs_snapshot_create(entries, ARRAY_SIZE(entries),
                                        &empty_values) ==
        NXANDROID_JNI_ADAPTER_OK);
  CHECK(nxandroid_prefs_snapshot_count(empty_values) == 2u);
  CHECK(find_snapshot_entry(empty_values, NULL, 0u) != NULL);
  CHECK(find_snapshot_entry(empty_values, key, ARRAY_SIZE(key)) != NULL);
  nxandroid_prefs_snapshot_destroy(&empty_values);

  memset(entries, 0, sizeof(entries));
  entries[0].key = (nxandroid_utf16_view){key, ARRAY_SIZE(key)};
  entries[0].type = NXANDROID_PREFS_STRING;
  entries[0].value.string_value =
      (nxandroid_utf16_view){value, ARRAY_SIZE(value)};
  CHECK(nxandroid_prefs_snapshot_create(entries, 1u, NULL) ==
        NXANDROID_JNI_ADAPTER_EINVAL);
  CHECK(expect_snapshot_failure(NULL, 1u, NXANDROID_JNI_ADAPTER_EINVAL) == 0);
  CHECK(expect_snapshot_failure(entries, NXANDROID_PREFS_MAX_ENTRIES + 1u,
                                NXANDROID_JNI_ADAPTER_EINVAL) == 0);

  entries[0].key = (nxandroid_utf16_view){NULL, 1u};
  CHECK(expect_snapshot_failure(entries, 1u, NXANDROID_JNI_ADAPTER_EINVAL) ==
        0);
  entries[0].key = (nxandroid_utf16_view){key, ARRAY_SIZE(key)};
  entries[0].type = (nxandroid_prefs_value_type)99;
  CHECK(expect_snapshot_failure(entries, 1u, NXANDROID_JNI_ADAPTER_EINVAL) ==
        0);
  entries[0].type = NXANDROID_PREFS_STRING;
  entries[0].value.string_value = (nxandroid_utf16_view){NULL, 1u};
  CHECK(expect_snapshot_failure(entries, 1u, NXANDROID_JNI_ADAPTER_EINVAL) ==
        0);

  entries[0].type = NXANDROID_PREFS_BOOL;
  entries[0].value.bool_value = 2;
  CHECK(expect_snapshot_failure(entries, 1u, NXANDROID_JNI_ADAPTER_EINVAL) ==
        0);
  entries[0].type = NXANDROID_PREFS_STRING_SET;
  entries[0].value.string_set_value =
      (nxandroid_prefs_string_set_view){NULL, 1u};
  CHECK(expect_snapshot_failure(entries, 1u, NXANDROID_JNI_ADAPTER_EINVAL) ==
        0);
  entries[0].value.string_set_value =
      (nxandroid_prefs_string_set_view){duplicate_members,
                                        ARRAY_SIZE(duplicate_members)};
  CHECK(expect_snapshot_failure(entries, 1u,
                                NXANDROID_JNI_ADAPTER_EDUPLICATE) == 0);
  entries[0].value.string_set_value.members = duplicate_members;
  entries[0].value.string_set_value.member_count =
      NXANDROID_PREFS_MAX_STRING_SET_MEMBERS + 1u;
  CHECK(expect_snapshot_failure(entries, 1u, NXANDROID_JNI_ADAPTER_EINVAL) ==
        0);

  memset(entries, 0, sizeof(entries));
  entries[0].key = (nxandroid_utf16_view){key, ARRAY_SIZE(key)};
  entries[0].type = NXANDROID_PREFS_INT32;
  entries[1] = entries[0];
  CHECK(expect_snapshot_failure(entries, ARRAY_SIZE(entries),
                                NXANDROID_JNI_ADAPTER_EDUPLICATE) == 0);
  return 0;
}

int main(void) {
  CHECK(strcmp(nxandroid_result_string(NXANDROID_EREENTRANT),
               "same-context callback reentrancy denied") == 0);
  CHECK(strcmp(nxandroid_phase_name(NXANDROID_PHASE_GRAPHICS_REQUEST),
               "graphics-request") == 0);
  CHECK(test_normal() == 0);
  CHECK(test_out_of_order() == 0);
  CHECK(test_declared_module_and_graphics_order() == 0);
  CHECK(test_sonic_entry_before_surface_callback() == 0);
  CHECK(test_failure_each_phase() == 0);
  CHECK(test_rollback_failure_and_abort() == 0);
  CHECK(test_reentrancy() == 0);
  CHECK(test_thousand_contexts() == 0);
  CHECK(test_independent_contexts() == 0);
  CHECK(test_surface_resume_cycles() == 0);
  CHECK(test_terminal_opt_in() == 0);
  CHECK(test_delegated_runtime() == 0);
  CHECK(test_import_policy_success() == 0);
  CHECK(test_import_policy_fail_closed() == 0);
  CHECK(test_catalog_validation() == 0);
  CHECK(test_public_limits() == 0);
  CHECK(test_utf16_region_adapter() == 0);
  CHECK(test_typed_preferences_snapshots() == 0);
  CHECK(test_preferences_snapshot_validation() == 0);
  puts("nxandroid_tests=PASS");
  puts("guest_code_executed=0");
  puts("device_access=0");
  puts("network_access=0");
  puts("signals_used=0");
  puts("contexts_completed=1000");
  return 0;
}
