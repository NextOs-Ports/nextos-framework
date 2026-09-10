/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxgl_internal.h"

#include <stdio.h>
#include <string.h>

#define NXGL_GL_SCISSOR_TEST 0x0C11u
#define NXGL_GL_COLOR_CLEAR_VALUE 0x0C22u
#define NXGL_GL_COLOR_WRITEMASK 0x0C23u
#define NXGL_GL_COLOR_BUFFER_BIT 0x00004000u
#define NXGL_GL_FRAMEBUFFER_BINDING 0x8CA6u
#define NXGL_GL_NO_ERROR 0u

void nxgl_present_policy_init(nxgl_present_policy *policy) {
  if (!policy)
    return;
  memset(policy, 0, sizeof(*policy));
  policy->api_version = NXGL_API_VERSION;
  policy->struct_size = sizeof(*policy);
  policy->owner = NXGL_PRESENT_ENGINE_OWNED;
}

#if !defined(NXGL_PRESENT_V2_TESTING)

static int nxgl_force_backbuffer_alpha_one(nxgl_context *context) {
  unsigned char color_mask[4];
  float clear_color[4];
  unsigned char scissor_enabled;
  int framebuffer = -1;
  if (context->report.actual.alpha_bits <= 0) {
    nxgl_emit(context->status, context->status_userdata, NXGL_STATUS_ERROR,
              "opaque-alpha present policy needs a backbuffer alpha channel");
    return NXGL_ERROR_PRESENT;
  }
  if (!context->gl.is_enabled || !context->gl.get_booleanv ||
      !context->gl.get_floatv || !context->gl.get_integerv ||
      !context->gl.disable ||
      !context->gl.enable || !context->gl.color_mask ||
      !context->gl.clear_color || !context->gl.clear) {
    nxgl_emit(context->status, context->status_userdata, NXGL_STATUS_ERROR,
              "opaque-alpha present policy lacks required GLES2 functions");
    return NXGL_ERROR_PRESENT;
  }
  context->gl.get_integerv(NXGL_GL_FRAMEBUFFER_BINDING, &framebuffer);
  if (framebuffer != 0) {
    nxgl_emit(context->status, context->status_userdata, NXGL_STATUS_ERROR,
              "opaque-alpha policy requires the default backbuffer (FBO=%d)",
              framebuffer);
    return NXGL_ERROR_PRESENT;
  }
  scissor_enabled = context->gl.is_enabled(NXGL_GL_SCISSOR_TEST);
  context->gl.get_booleanv(NXGL_GL_COLOR_WRITEMASK, color_mask);
  context->gl.get_floatv(NXGL_GL_COLOR_CLEAR_VALUE, clear_color);
  if (scissor_enabled)
    context->gl.disable(NXGL_GL_SCISSOR_TEST);
  context->gl.color_mask(0, 0, 0, 1);
  context->gl.clear_color(0.0f, 0.0f, 0.0f, 1.0f);
  context->gl.clear(NXGL_GL_COLOR_BUFFER_BIT);
  context->gl.clear_color(clear_color[0], clear_color[1], clear_color[2],
                          clear_color[3]);
  context->gl.color_mask(color_mask[0], color_mask[1], color_mask[2],
                         color_mask[3]);
  if (scissor_enabled)
    context->gl.enable(NXGL_GL_SCISSOR_TEST);
  return NXGL_SUCCESS;
}
#endif /* !NXGL_PRESENT_V2_TESTING */

static int nxgl_present_policy_valid(const nxgl_present_policy *policy) {
  const uint32_t known_flags = NXGL_PRESENT_FINISH_BEFORE_SWAP |
                               NXGL_PRESENT_FORCE_BACKBUFFER_ALPHA_ONE;
  if (!policy || policy->api_version != NXGL_API_VERSION ||
      policy->struct_size < sizeof(*policy) ||
      (policy->flags & ~known_flags) != 0 ||
      policy->owner < NXGL_PRESENT_ENGINE_OWNED ||
      policy->owner > NXGL_PRESENT_ADAPTER)
    return 0;
  if (policy->owner == NXGL_PRESENT_ADAPTER && !policy->adapter)
    return 0;
  if (policy->owner != NXGL_PRESENT_ADAPTER && policy->adapter)
    return 0;
  return 1;
}

static int nxgl_present_v1_commit_and_release(int result) {
  nxgl_arbiter_release();
  return result;
}

int nxgl_present(nxgl_context *context, const nxgl_present_policy *policy) {
  /* Acquire before inspecting either caller-owned pointer.  This also makes
   * reentry from a v2 provider callback fail closed without reaching SDL,
   * GL, or an adapter from the legacy entry point. */
  if (!nxgl_arbiter_try_acquire())
    return NXGL_ERROR_BUSY;
  if (!context || context->open_api_version != NXGL_API_VERSION_V1)
    return nxgl_present_v1_commit_and_release(NXGL_ERROR_INVALID_ARGUMENT);
  if (!context->window || !context->gl_context ||
      !nxgl_present_policy_valid(policy))
    return nxgl_present_v1_commit_and_release(NXGL_ERROR_INVALID_ARGUMENT);
  /* Engine-owned is deliberately a strict no-op. In particular, merely
   * linking nxgl cannot inject glFinish, alpha clears, or a second swap. */
  if (policy->owner == NXGL_PRESENT_ENGINE_OWNED)
    return nxgl_present_v1_commit_and_release(NXGL_NO_ACTION);
#if defined(NXGL_PRESENT_V2_TESTING)
  /* The hermetic provider fixture links no graphics runtime.  Its only v1
   * delegated path remains sealed after the common arbitration, version,
   * handle, and policy checks. */
  return nxgl_present_v1_commit_and_release(NXGL_ERROR_PRESENT);
#else
  char error[NXGL_DETAIL_MAX];
  if ((policy->flags & NXGL_PRESENT_FORCE_BACKBUFFER_ALPHA_ONE) != 0 &&
      nxgl_force_backbuffer_alpha_one(context) != NXGL_SUCCESS)
    return nxgl_present_v1_commit_and_release(NXGL_ERROR_PRESENT);
  if ((policy->flags & NXGL_PRESENT_FINISH_BEFORE_SWAP) != 0) {
    if (!context->gl.finish) {
      nxgl_emit(context->status, context->status_userdata, NXGL_STATUS_ERROR,
                "finish-before-swap policy lacks glFinish");
      return nxgl_present_v1_commit_and_release(NXGL_ERROR_PRESENT);
    }
    context->gl.finish();
  }
  if (policy->owner == NXGL_PRESENT_SDL) {
    SDL_GL_SwapWindow(context->window);
    return nxgl_present_v1_commit_and_release(NXGL_SUCCESS);
  }
  error[0] = '\0';
  if (policy->adapter(policy->userdata, context->window, context->gl_context,
                      error, sizeof(error)) != 0) {
    nxgl_emit(context->status, context->status_userdata, NXGL_STATUS_ERROR,
              "present adapter failed: %s", error[0] ? error : "unknown error");
    return nxgl_present_v1_commit_and_release(NXGL_ERROR_PRESENT);
  }
  return nxgl_present_v1_commit_and_release(NXGL_SUCCESS);
#endif
}

typedef struct nxgl_present_state_v2 {
  unsigned char scissor_enabled;
  unsigned char color_mask[4];
  float clear_color[4];
  int framebuffer;
} nxgl_present_state_v2;

enum nxgl_present_state_touch_v2 {
  NXGL_PRESENT_TOUCH_V2_SCISSOR = 1u << 0,
  NXGL_PRESENT_TOUCH_V2_COLOR_MASK = 1u << 1,
  NXGL_PRESENT_TOUCH_V2_CLEAR_COLOR = 1u << 2
};

void nxgl_present_policy_v2_init(nxgl_present_policy_v2 *policy) {
  if (!policy)
    return;
  memset(policy, 0, sizeof(*policy));
  policy->api_version = NXGL_API_VERSION_V2;
  policy->struct_size = sizeof(*policy);
  policy->owner = NXGL_PRESENT_ENGINE_OWNED;
  policy->quirk = NXGL_PRESENT_QUIRK_V2_NONE;
  policy->reason = NXGL_PRESENT_REASON_V2_NONE;
}

void nxgl_present_result_v2_init(nxgl_present_result_v2 *result) {
  if (!result)
    return;
  memset(result, 0, sizeof(*result));
  result->api_version = NXGL_API_VERSION_V2;
  result->struct_size = sizeof(*result);
  result->result = NXGL_NO_ACTION;
  result->failed_stage = NXGL_PRESENT_STAGE_V2_NONE;
}

static int nxgl_present_policy_v2_valid(
    const nxgl_present_policy_v2 *policy) {
  const uint32_t known_flags = NXGL_PRESENT_FINISH_BEFORE_SWAP |
                               NXGL_PRESENT_FORCE_BACKBUFFER_ALPHA_ONE;
  const int alpha_one =
      policy &&
      (policy->flags & NXGL_PRESENT_FORCE_BACKBUFFER_ALPHA_ONE) != 0;
  if (!policy || policy->api_version != NXGL_API_VERSION_V2 ||
      policy->struct_size < sizeof(*policy) ||
      policy->owner < NXGL_PRESENT_ENGINE_OWNED ||
      policy->owner > NXGL_PRESENT_ADAPTER ||
      (policy->flags & ~known_flags) != 0)
    return 0;
  if (policy->owner == NXGL_PRESENT_ENGINE_OWNED)
    return policy->flags == 0 && policy->quirk == NXGL_PRESENT_QUIRK_V2_NONE &&
           policy->reason == NXGL_PRESENT_REASON_V2_NONE;
  if (alpha_one)
    return policy->quirk == NXGL_PRESENT_QUIRK_V2_AMLOGIC_OSD_ALPHA_ONE &&
           policy->reason ==
               NXGL_PRESENT_REASON_V2_OBSERVED_OSD_ZERO_ALPHA;
  return policy->quirk == NXGL_PRESENT_QUIRK_V2_NONE &&
         policy->reason == NXGL_PRESENT_REASON_V2_NONE;
}

static int nxgl_present_v2_handles_equal(
    const nxgl_stack_handles_v2 *left, const nxgl_stack_handles_v2 *right) {
  return left->api_version == right->api_version &&
         left->struct_size == right->struct_size &&
         left->owner == right->owner &&
         left->sdl_window == right->sdl_window &&
         left->sdl_context == right->sdl_context &&
         left->native_display == right->native_display &&
         left->native_window == right->native_window &&
         left->egl_display == right->egl_display &&
         left->egl_context == right->egl_context &&
         left->egl_surface == right->egl_surface &&
         left->egl_config == right->egl_config;
}

static int nxgl_present_v2_stack_valid(const nxgl_context *context,
                                       nxgl_present_owner owner) {
  nxgl_stack_owner_v2 expected_owner;
  if (!context || context->open_api_version != NXGL_API_VERSION_V2 ||
      context->stack_ops.api_version != NXGL_API_VERSION_V2 ||
      context->stack_ops.struct_size < sizeof(context->stack_ops) ||
      context->stack_handles.api_version != NXGL_API_VERSION_V2 ||
      context->stack_handles.struct_size < sizeof(context->stack_handles) ||
      context->report_v2.api_version != NXGL_API_VERSION_V2 ||
      context->report_v2.struct_size < sizeof(context->report_v2) ||
      !context->stack_ops.validate_current || !context->stack_ops.present)
    return 0;
  expected_owner = owner == NXGL_PRESENT_SDL
                       ? NXGL_STACK_OWNER_V2_SDL_EGL
                       : NXGL_STACK_OWNER_V2_RAW_EGL;
  return context->stack_owner == expected_owner &&
         context->stack_ops.owner == expected_owner &&
         context->stack_handles.owner == expected_owner &&
         context->report_v2.stack_owner == expected_owner &&
         context->report_v2.handles.owner == expected_owner &&
         nxgl_present_v2_handles_equal(&context->stack_handles,
                                       &context->report_v2.handles);
}

static int nxgl_present_v2_gl_error_free(nxgl_context *context) {
  return context->gl.get_error &&
         context->gl.get_error() == NXGL_GL_NO_ERROR;
}

static int nxgl_present_v2_snapshot_state(
    nxgl_context *context, nxgl_present_state_v2 *state) {
  nxgl_present_state_v2 captured;
  if (!context || !state || !context->gl.get_error ||
      !context->gl.is_enabled || !context->gl.get_booleanv ||
      !context->gl.get_floatv || !context->gl.get_integerv ||
      !context->gl.disable || !context->gl.enable ||
      !context->gl.color_mask || !context->gl.clear_color ||
      !context->gl.clear)
    return NXGL_ERROR_PRESENT;
  if (!nxgl_present_v2_gl_error_free(context))
    return NXGL_ERROR_PRESENT;
  memset(&captured, 0, sizeof(captured));
  captured.framebuffer = -1;
  captured.scissor_enabled =
      context->gl.is_enabled(NXGL_GL_SCISSOR_TEST);
  context->gl.get_booleanv(NXGL_GL_COLOR_WRITEMASK, captured.color_mask);
  context->gl.get_floatv(NXGL_GL_COLOR_CLEAR_VALUE, captured.clear_color);
  context->gl.get_integerv(NXGL_GL_FRAMEBUFFER_BINDING,
                           &captured.framebuffer);
  if (!nxgl_present_v2_gl_error_free(context))
    return NXGL_ERROR_PRESENT;
  *state = captured;
  return NXGL_SUCCESS;
}

static int nxgl_present_v2_state_matches(
    nxgl_context *context, const nxgl_present_state_v2 *expected) {
  nxgl_present_state_v2 observed;
  memset(&observed, 0, sizeof(observed));
  observed.framebuffer = -1;
  observed.scissor_enabled =
      context->gl.is_enabled(NXGL_GL_SCISSOR_TEST);
  context->gl.get_booleanv(NXGL_GL_COLOR_WRITEMASK, observed.color_mask);
  context->gl.get_floatv(NXGL_GL_COLOR_CLEAR_VALUE, observed.clear_color);
  context->gl.get_integerv(NXGL_GL_FRAMEBUFFER_BINDING,
                           &observed.framebuffer);
  if (!nxgl_present_v2_gl_error_free(context))
    return 0;
  return observed.scissor_enabled == expected->scissor_enabled &&
         memcmp(observed.color_mask, expected->color_mask,
                sizeof(observed.color_mask)) == 0 &&
         memcmp(observed.clear_color, expected->clear_color,
                sizeof(observed.clear_color)) == 0 &&
         observed.framebuffer == expected->framebuffer;
}

static int nxgl_present_v2_restore_state(
    nxgl_context *context, const nxgl_present_state_v2 *state,
    unsigned touched) {
  int restored = 1;
  if ((touched & NXGL_PRESENT_TOUCH_V2_CLEAR_COLOR) != 0) {
    context->gl.clear_color(state->clear_color[0], state->clear_color[1],
                            state->clear_color[2], state->clear_color[3]);
    if (!nxgl_present_v2_gl_error_free(context))
      restored = 0;
  }
  if ((touched & NXGL_PRESENT_TOUCH_V2_COLOR_MASK) != 0) {
    context->gl.color_mask(state->color_mask[0], state->color_mask[1],
                           state->color_mask[2], state->color_mask[3]);
    if (!nxgl_present_v2_gl_error_free(context))
      restored = 0;
  }
  if ((touched & NXGL_PRESENT_TOUCH_V2_SCISSOR) != 0) {
    if (state->scissor_enabled)
      context->gl.enable(NXGL_GL_SCISSOR_TEST);
    else
      context->gl.disable(NXGL_GL_SCISSOR_TEST);
    if (!nxgl_present_v2_gl_error_free(context))
      restored = 0;
  }
  if (!nxgl_present_v2_state_matches(context, state))
    restored = 0;
  return restored;
}

static int nxgl_present_v2_apply_alpha_one(
    nxgl_context *context, const nxgl_present_state_v2 *state,
    int *state_restored) {
  unsigned touched = 0;
  int mutation_failed = 0;
  int restored;

  if (state->scissor_enabled) {
    touched |= NXGL_PRESENT_TOUCH_V2_SCISSOR;
    context->gl.disable(NXGL_GL_SCISSOR_TEST);
    if (!nxgl_present_v2_gl_error_free(context))
      mutation_failed = 1;
  }
  if (!mutation_failed) {
    touched |= NXGL_PRESENT_TOUCH_V2_COLOR_MASK;
    context->gl.color_mask(0, 0, 0, 1);
    if (!nxgl_present_v2_gl_error_free(context))
      mutation_failed = 1;
  }
  if (!mutation_failed) {
    touched |= NXGL_PRESENT_TOUCH_V2_CLEAR_COLOR;
    context->gl.clear_color(0.0f, 0.0f, 0.0f, 1.0f);
    if (!nxgl_present_v2_gl_error_free(context))
      mutation_failed = 1;
  }
  if (!mutation_failed) {
    context->gl.clear(NXGL_GL_COLOR_BUFFER_BIT);
    if (!nxgl_present_v2_gl_error_free(context))
      mutation_failed = 1;
  }

  restored = nxgl_present_v2_restore_state(context, state, touched);
  *state_restored = restored;
  if (!restored)
    return NXGL_ERROR_ROLLBACK;
  return mutation_failed ? NXGL_ERROR_PRESENT : NXGL_SUCCESS;
}

static int nxgl_present_v2_error_terminated(const char *error, size_t size) {
  return error && size > 0 && memchr(error, '\0', size) != NULL;
}

static int nxgl_present_v2_current_result_allowed(int provider_result) {
  return provider_result == NXGL_ERROR_STACK_MISMATCH;
}

static int nxgl_present_v2_present_result_allowed(int provider_result) {
  return provider_result == NXGL_ERROR_STACK_MISMATCH ||
         provider_result == NXGL_ERROR_PRESENT;
}

static int nxgl_present_v2_commit_and_release(
    nxgl_present_result_v2 *result,
    const nxgl_present_result_v2 *computed) {
  const int return_code = computed->result;
  *result = *computed;
  nxgl_arbiter_release();
  return return_code;
}

int nxgl_present_v2(nxgl_context *context,
                    const nxgl_present_policy_v2 *policy,
                    nxgl_present_result_v2 *result) {
  nxgl_present_state_v2 state;
  nxgl_present_result_v2 computed;
  char error[NXGL_DETAIL_MAX];
  int provider_result;
  int operation_result;

  if (!result)
    return NXGL_ERROR_INVALID_ARGUMENT;
  /* The shared nxgl arbiter serializes open/close/present without calling
   * SDL, EGL, or GL.  BUSY is deliberately byte-atomic for the caller's
   * result and cannot reach a provider callback. */
  if (!nxgl_arbiter_try_acquire())
    return NXGL_ERROR_BUSY;
  nxgl_present_result_v2_init(&computed);
  if (!context || !nxgl_present_policy_v2_valid(policy)) {
    computed.result = NXGL_ERROR_INVALID_ARGUMENT;
    return nxgl_present_v2_commit_and_release(result, &computed);
  }
  if (policy->owner == NXGL_PRESENT_ENGINE_OWNED)
    return nxgl_present_v2_commit_and_release(result, &computed);
  if (!nxgl_present_v2_stack_valid(context, policy->owner)) {
    computed.result = NXGL_ERROR_STACK_MISMATCH;
    computed.failed_stage = NXGL_PRESENT_STAGE_V2_CURRENT;
    return nxgl_present_v2_commit_and_release(result, &computed);
  }

  memset(error, 0, sizeof(error));
  provider_result = context->stack_ops.validate_current(
      context->stack_userdata, &context->stack_handles, error, sizeof(error));
  if (provider_result != NXGL_SUCCESS ||
      !nxgl_present_v2_error_terminated(error, sizeof(error))) {
    const int provider_contract_valid =
        nxgl_present_v2_error_terminated(error, sizeof(error)) &&
        provider_result != NXGL_SUCCESS &&
        nxgl_present_v2_current_result_allowed(provider_result);
    computed.result = provider_contract_valid
                          ? provider_result
                          : NXGL_ERROR_STACK_MISMATCH;
    computed.failed_stage = NXGL_PRESENT_STAGE_V2_CURRENT;
    nxgl_emit(context->status, context->status_userdata, NXGL_STATUS_ERROR,
              "%s", provider_contract_valid
                        ? "present-v2-current-rejected"
                        : "present-v2-provider-contract");
    return nxgl_present_v2_commit_and_release(result, &computed);
  }
  computed.expected_context_current = 1;

  if ((policy->flags & NXGL_PRESENT_FORCE_BACKBUFFER_ALPHA_ONE) != 0) {
    if (context->report_v2.legacy.actual.alpha_bits <= 0) {
      computed.result = NXGL_ERROR_PRESENT;
      computed.failed_stage = NXGL_PRESENT_STAGE_V2_ALPHA_ONE;
      return nxgl_present_v2_commit_and_release(result, &computed);
    }
    operation_result = nxgl_present_v2_snapshot_state(context, &state);
    if (operation_result != NXGL_SUCCESS) {
      computed.result = operation_result;
      computed.failed_stage = NXGL_PRESENT_STAGE_V2_SNAPSHOT;
      return nxgl_present_v2_commit_and_release(result, &computed);
    }
    computed.state_snapshotted = 1;
    if (state.framebuffer != 0) {
      computed.result = NXGL_ERROR_PRESENT;
      computed.failed_stage = NXGL_PRESENT_STAGE_V2_ALPHA_ONE;
      computed.state_restored = 1;
      return nxgl_present_v2_commit_and_release(result, &computed);
    }
    operation_result = nxgl_present_v2_apply_alpha_one(
        context, &state, &computed.state_restored);
    if (operation_result != NXGL_SUCCESS) {
      computed.result = operation_result;
      computed.failed_stage = operation_result == NXGL_ERROR_ROLLBACK
                                  ? NXGL_PRESENT_STAGE_V2_RESTORE
                                  : NXGL_PRESENT_STAGE_V2_ALPHA_ONE;
      return nxgl_present_v2_commit_and_release(result, &computed);
    }
  }

  if ((policy->flags & NXGL_PRESENT_FINISH_BEFORE_SWAP) != 0) {
    if (!context->gl.finish || !context->gl.get_error ||
        !nxgl_present_v2_gl_error_free(context)) {
      computed.result = NXGL_ERROR_PRESENT;
      computed.failed_stage = NXGL_PRESENT_STAGE_V2_FINISH;
      return nxgl_present_v2_commit_and_release(result, &computed);
    }
    context->gl.finish();
    if (!nxgl_present_v2_gl_error_free(context)) {
      computed.result = NXGL_ERROR_PRESENT;
      computed.failed_stage = NXGL_PRESENT_STAGE_V2_FINISH;
      return nxgl_present_v2_commit_and_release(result, &computed);
    }
  }

  memset(error, 0, sizeof(error));
  provider_result = context->stack_ops.present(
      context->stack_userdata, &context->stack_handles, error, sizeof(error));
  if (provider_result != NXGL_SUCCESS ||
      !nxgl_present_v2_error_terminated(error, sizeof(error))) {
    const int provider_contract_valid =
        nxgl_present_v2_error_terminated(error, sizeof(error)) &&
        provider_result != NXGL_SUCCESS &&
        nxgl_present_v2_present_result_allowed(provider_result);
    computed.result = provider_contract_valid
                          ? provider_result
                          : NXGL_ERROR_STACK_MISMATCH;
    computed.failed_stage = NXGL_PRESENT_STAGE_V2_PRESENT;
    nxgl_emit(context->status, context->status_userdata, NXGL_STATUS_ERROR,
              "%s", provider_contract_valid
                        ? "present-v2-provider-failed"
                        : "present-v2-provider-contract");
    return nxgl_present_v2_commit_and_release(result, &computed);
  }
  computed.result = NXGL_SUCCESS;
  computed.failed_stage = NXGL_PRESENT_STAGE_V2_NONE;
  return nxgl_present_v2_commit_and_release(result, &computed);
}
