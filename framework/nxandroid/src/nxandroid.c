/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxandroid.h"

#include <stdlib.h>
#include <string.h>

struct nxandroid_context {
  nxandroid_profile profile;
  nxandroid_module_spec *modules;
  nxandroid_step *steps;
  unsigned char *invoked;
  unsigned char *rolled_back;
  uint32_t *rollback_groups;
  unsigned char *rollback_group_closed;
  size_t rollback_group_count;
  nxandroid_ops ops;
  nxandroid_context_state state;
  size_t next_step;
  int callback_active;
  int callback_violation;
  int adapter_status;
  int rollback_status;
};

typedef struct nxandroid_validation_state {
  unsigned char *module_initialized;
  unsigned char *module_jni;
  int activity;
  int graphics_pending;
  uint32_t pending_cycle;
  int surface;
  uint32_t active_cycle;
  uint32_t last_cycle;
  int gl_ready;
  int resumed;
  uint32_t resumed_cycle;
  int focused;
  uint32_t focused_cycle;
  int entry;
  uint32_t entry_cycle;
  int entry_before_surface_callback;
  int objects_ready;
  int input;
  int run_loop;
  int shutdown_armed;
  int saved;
  int native_shutdown;
  int terminal;
  nxandroid_terminal_policy terminal_policy;
  int delegated_runtime;
  int saw_surface;
} nxandroid_validation_state;

static int nxandroid_bounded_length(const char *text, size_t maximum,
                                    size_t *length) {
  size_t index;
  if (text == NULL)
    return 0;
  for (index = 0u; index <= maximum; ++index) {
    if (text[index] == '\0') {
      if (length != NULL)
        *length = index;
      return index != 0u;
    }
  }
  return 0;
}

static int nxandroid_optional_bounded(const char *text, size_t maximum) {
  return text == NULL || nxandroid_bounded_length(text, maximum, NULL);
}

static char *nxandroid_copy_string(const char *text, size_t maximum) {
  size_t size;
  size_t length;
  char *copy;
  if (text == NULL)
    return NULL;
  if (!nxandroid_bounded_length(text, maximum, &length))
    return NULL;
  size = length + 1u;
  copy = (char *)malloc(size);
  if (copy != NULL)
    memcpy(copy, text, size);
  return copy;
}

const char *nxandroid_result_string(nxandroid_result result) {
  switch (result) {
  case NXANDROID_OK:
    return "ok";
  case NXANDROID_EINVAL:
    return "invalid argument";
  case NXANDROID_ENOMEM:
    return "out of memory";
  case NXANDROID_EPROFILE:
    return "invalid lifecycle profile";
  case NXANDROID_ESTATE:
    return "invalid context state";
  case NXANDROID_EREENTRANT:
    return "same-context callback reentrancy denied";
  case NXANDROID_ECALLBACK:
    return "adapter callback failed";
  case NXANDROID_EROLLBACK:
    return "adapter rollback failed";
  case NXANDROID_ECATALOG:
    return "invalid import catalog";
  case NXANDROID_EUNRESOLVED:
    return "unresolved import";
  case NXANDROID_ECONTRACT:
    return "import contract mismatch";
  default:
    return "unknown nxandroid result";
  }
}

const char *nxandroid_phase_name(nxandroid_phase phase) {
  switch (phase) {
  case NXANDROID_PHASE_MODULE_INITIALIZED:
    return "module-initialized";
  case NXANDROID_PHASE_MODULE_JNI:
    return "module-jni";
  case NXANDROID_PHASE_ACTIVITY_CREATE:
    return "activity-create";
  case NXANDROID_PHASE_GRAPHICS_REQUEST:
    return "graphics-request";
  case NXANDROID_PHASE_SURFACE_UP:
    return "surface-up";
  case NXANDROID_PHASE_SURFACE_CHANGED:
    return "surface-changed";
  case NXANDROID_PHASE_GL_READY:
    return "gl-ready";
  case NXANDROID_PHASE_RESUME:
    return "resume";
  case NXANDROID_PHASE_FOCUS_GAIN:
    return "focus-gain";
  case NXANDROID_PHASE_ENTRY:
    return "entry";
  case NXANDROID_PHASE_OBJECTS_READY:
    return "objects-ready";
  case NXANDROID_PHASE_INPUT_ENABLE:
    return "input-enable";
  case NXANDROID_PHASE_RUN_LOOP:
    return "run-loop";
  case NXANDROID_PHASE_INPUT_DISABLE:
    return "input-disable";
  case NXANDROID_PHASE_FOCUS_LOSS:
    return "focus-loss";
  case NXANDROID_PHASE_PAUSE:
    return "pause";
  case NXANDROID_PHASE_SURFACE_DOWN:
    return "surface-down";
  case NXANDROID_PHASE_SAVE:
    return "save";
  case NXANDROID_PHASE_NATIVE_SHUTDOWN:
    return "native-shutdown";
  case NXANDROID_PHASE_TERMINAL:
    return "terminal";
  case NXANDROID_PHASE_RUNTIME_DELEGATED:
    return "runtime-delegated";
  default:
    return "unknown";
  }
}

static int nxandroid_valid_module_phase(nxandroid_phase phase) {
  return phase == NXANDROID_PHASE_MODULE_INITIALIZED ||
         phase == NXANDROID_PHASE_MODULE_JNI;
}

static int nxandroid_valid_cycle_phase(nxandroid_phase phase) {
  switch (phase) {
  case NXANDROID_PHASE_GRAPHICS_REQUEST:
  case NXANDROID_PHASE_SURFACE_UP:
  case NXANDROID_PHASE_SURFACE_CHANGED:
  case NXANDROID_PHASE_GL_READY:
  case NXANDROID_PHASE_RESUME:
  case NXANDROID_PHASE_FOCUS_GAIN:
  case NXANDROID_PHASE_ENTRY:
  case NXANDROID_PHASE_OBJECTS_READY:
  case NXANDROID_PHASE_INPUT_ENABLE:
  case NXANDROID_PHASE_RUN_LOOP:
  case NXANDROID_PHASE_INPUT_DISABLE:
  case NXANDROID_PHASE_FOCUS_LOSS:
  case NXANDROID_PHASE_PAUSE:
  case NXANDROID_PHASE_SURFACE_DOWN:
    return 1;
  default:
    return 0;
  }
}

static int nxandroid_all_modules_ready(const nxandroid_profile *profile,
                                       const nxandroid_validation_state *state) {
  size_t index;
  for (index = 0; index < profile->module_count; ++index) {
    if (!state->module_initialized[index])
      return 0;
    if (profile->modules[index].jni_policy == NXANDROID_JNI_REQUIRED &&
        !state->module_jni[index])
      return 0;
  }
  return 1;
}

static void nxandroid_sift_u32(uint32_t *values, size_t count, size_t root) {
  for (;;) {
    size_t child = root * 2u + 1u;
    size_t selected = root;
    uint32_t temporary;
    if (child < count && values[child] > values[selected])
      selected = child;
    if (child + 1u < count && values[child + 1u] > values[selected])
      selected = child + 1u;
    if (selected == root)
      return;
    temporary = values[root];
    values[root] = values[selected];
    values[selected] = temporary;
    root = selected;
  }
}

static void nxandroid_sort_u32(uint32_t *values, size_t count) {
  size_t start;
  size_t end;
  if (count < 2u)
    return;
  for (start = count / 2u; start > 0u; --start)
    nxandroid_sift_u32(values, count, start - 1u);
  for (end = count; end > 1u; --end) {
    uint32_t temporary = values[0];
    values[0] = values[end - 1u];
    values[end - 1u] = temporary;
    nxandroid_sift_u32(values, end - 1u, 0u);
  }
}

static size_t nxandroid_find_u32(const uint32_t *values, size_t count,
                                 uint32_t value) {
  size_t low = 0u;
  size_t high = count;
  while (low < high) {
    size_t middle = low + (high - low) / 2u;
    if (values[middle] == value)
      return middle;
    if (value < values[middle])
      high = middle;
    else
      low = middle + 1u;
  }
  return NXANDROID_NO_MODULE;
}

static nxandroid_result nxandroid_collect_rollback_groups(
    const nxandroid_profile *profile, uint32_t **groups, size_t *group_count) {
  uint32_t *values;
  size_t count = 0u;
  size_t index;
  size_t unique;

  *groups = NULL;
  *group_count = 0u;
  values = (uint32_t *)calloc(profile->step_count, sizeof(*values));
  if (values == NULL)
    return NXANDROID_ENOMEM;
  for (index = 0u; index < profile->step_count; ++index) {
    if (profile->steps[index].rollback_group != 0u)
      values[count++] = profile->steps[index].rollback_group;
  }
  if (count == 0u) {
    free(values);
    return NXANDROID_OK;
  }
  nxandroid_sort_u32(values, count);
  unique = 1u;
  for (index = 1u; index < count; ++index) {
    if (values[index] != values[unique - 1u])
      values[unique++] = values[index];
  }
  *groups = values;
  *group_count = unique;
  return NXANDROID_OK;
}

static nxandroid_result nxandroid_validate_rollback_groups(
    const nxandroid_profile *profile, size_t *bad_step) {
  uint32_t *groups = NULL;
  unsigned char *states = NULL;
  size_t group_count = 0u;
  size_t index;
  nxandroid_result result;
  for (index = 0u; index < profile->step_count; ++index) {
    const nxandroid_step *step = &profile->steps[index];
    if ((step->rollback_contract_id != NULL) !=
            (step->rollback_group != 0u) ||
        (step->rollback_group != 0u &&
         step->rollback_group == step->closes_rollback_group)) {
      if (bad_step != NULL)
        *bad_step = index;
      return NXANDROID_EPROFILE;
    }
  }
  result = nxandroid_collect_rollback_groups(profile, &groups, &group_count);
  if (result != NXANDROID_OK)
    return result;
  if (group_count != 0u) {
    states = (unsigned char *)calloc(group_count, sizeof(*states));
    if (states == NULL) {
      free(groups);
      return NXANDROID_ENOMEM;
    }
  }
  for (index = 0u; index < profile->step_count; ++index) {
    const nxandroid_step *step = &profile->steps[index];
    size_t position;
    if (step->rollback_group != 0u) {
      position =
          nxandroid_find_u32(groups, group_count, step->rollback_group);
      if (states == NULL || position == NXANDROID_NO_MODULE) {
        result = NXANDROID_EPROFILE;
        goto failed;
      }
      if (states[position] == 2u) {
        result = NXANDROID_EPROFILE;
        goto failed;
      }
      states[position] = 1u;
    }
    if (step->closes_rollback_group != 0u) {
      position = nxandroid_find_u32(groups, group_count,
                                    step->closes_rollback_group);
      if (states == NULL || position == NXANDROID_NO_MODULE) {
        result = NXANDROID_EPROFILE;
        goto failed;
      }
      if (states[position] != 1u) {
        result = NXANDROID_EPROFILE;
        goto failed;
      }
      states[position] = 2u;
    }
  }
  free(states);
  free(groups);
  return NXANDROID_OK;

failed:
  if (bad_step != NULL)
    *bad_step = index;
  free(states);
  free(groups);
  return result;
}

static nxandroid_result
nxandroid_validate_step(const nxandroid_profile *profile,
                        nxandroid_validation_state *state, size_t index) {
  const nxandroid_step *step = &profile->steps[index];

  if (!nxandroid_bounded_length(step->contract_id,
                                NXANDROID_CONTRACT_ID_MAX, NULL) ||
      !nxandroid_optional_bounded(step->rollback_contract_id,
                                  NXANDROID_CONTRACT_ID_MAX))
    return NXANDROID_EPROFILE;
  if ((step->rollback_contract_id != NULL) !=
          (step->rollback_group != 0u) ||
      (step->rollback_group != 0u &&
       step->rollback_group == step->closes_rollback_group))
    return NXANDROID_EPROFILE;
  if (step->phase < NXANDROID_PHASE_MODULE_INITIALIZED ||
      step->phase > NXANDROID_PHASE_RUNTIME_DELEGATED)
    return NXANDROID_EPROFILE;

  if (nxandroid_valid_module_phase(step->phase)) {
    if (step->module_index >= profile->module_count || step->cycle_id != 0u)
      return NXANDROID_EPROFILE;
  } else if (step->module_index != NXANDROID_NO_MODULE) {
    return NXANDROID_EPROFILE;
  }

  if (nxandroid_valid_cycle_phase(step->phase)) {
    if (step->cycle_id == 0u)
      return NXANDROID_EPROFILE;
  } else if (step->cycle_id != 0u) {
    return NXANDROID_EPROFILE;
  }

  if (step->phase == NXANDROID_PHASE_TERMINAL) {
    if (step->terminal_policy != NXANDROID_TERMINAL_RETURN &&
        step->terminal_policy != NXANDROID_TERMINAL_ADAPTER)
      return NXANDROID_EPROFILE;
    if (step->terminal_policy == NXANDROID_TERMINAL_ADAPTER &&
        !(profile->flags & NXANDROID_PROFILE_ALLOW_ADAPTER_TERMINAL))
      return NXANDROID_EPROFILE;
  } else if (step->terminal_policy != NXANDROID_TERMINAL_NONE) {
    return NXANDROID_EPROFILE;
  }

  switch (step->phase) {
  case NXANDROID_PHASE_MODULE_INITIALIZED:
    if (state->activity || state->saved ||
        state->module_initialized[step->module_index])
      return NXANDROID_EPROFILE;
    state->module_initialized[step->module_index] = 1u;
    break;

  case NXANDROID_PHASE_MODULE_JNI:
    if (state->activity || state->saved ||
        !state->module_initialized[step->module_index] ||
        state->module_jni[step->module_index] ||
        profile->modules[step->module_index].jni_policy !=
            NXANDROID_JNI_REQUIRED)
      return NXANDROID_EPROFILE;
    state->module_jni[step->module_index] = 1u;
    break;

  case NXANDROID_PHASE_ACTIVITY_CREATE:
    if (state->activity || state->saved ||
        !nxandroid_all_modules_ready(profile, state))
      return NXANDROID_EPROFILE;
    state->activity = 1;
    break;

  case NXANDROID_PHASE_GRAPHICS_REQUEST:
    if (!state->activity || state->surface || state->graphics_pending ||
        state->input || state->saved || state->last_cycle == UINT32_MAX ||
        step->cycle_id != state->last_cycle + 1u)
      return NXANDROID_EPROFILE;
    state->graphics_pending = 1;
    state->pending_cycle = step->cycle_id;
    break;

  case NXANDROID_PHASE_SURFACE_UP:
    if (!state->activity || state->surface || !state->graphics_pending ||
        step->cycle_id != state->pending_cycle || state->saved ||
        (state->entry_before_surface_callback && !state->objects_ready))
      return NXANDROID_EPROFILE;
    state->graphics_pending = 0;
    state->pending_cycle = 0u;
    state->surface = 1;
    state->active_cycle = step->cycle_id;
    state->last_cycle = step->cycle_id;
    state->saw_surface = 1;
    break;

  case NXANDROID_PHASE_SURFACE_CHANGED:
    if (!state->surface || step->cycle_id != state->active_cycle ||
        state->saved)
      return NXANDROID_EPROFILE;
    break;

  case NXANDROID_PHASE_GL_READY:
    if ((!state->surface && !state->graphics_pending) ||
        (state->surface && step->cycle_id != state->active_cycle) ||
        (state->graphics_pending && step->cycle_id != state->pending_cycle) ||
        state->gl_ready || state->saved)
      return NXANDROID_EPROFILE;
    state->gl_ready = 1;
    break;

  case NXANDROID_PHASE_RESUME:
    if (!state->activity || state->resumed || state->saved)
      return NXANDROID_EPROFILE;
    state->resumed = 1;
    state->resumed_cycle = step->cycle_id;
    state->shutdown_armed = 0;
    break;

  case NXANDROID_PHASE_FOCUS_GAIN:
    if (!state->activity || state->focused || state->saved)
      return NXANDROID_EPROFILE;
    state->focused = 1;
    state->focused_cycle = step->cycle_id;
    state->shutdown_armed = 0;
    break;

  case NXANDROID_PHASE_ENTRY:
    if (state->entry || state->saved)
      return NXANDROID_EPROFILE;
    if (profile->flags &
        NXANDROID_PROFILE_ALLOW_ENTRY_BEFORE_SURFACE_CALLBACK) {
      if (state->surface || !state->graphics_pending || !state->gl_ready ||
          step->cycle_id != state->pending_cycle)
        return NXANDROID_EPROFILE;
    } else if (!state->surface || step->cycle_id != state->active_cycle) {
      return NXANDROID_EPROFILE;
    }
    state->entry = 1;
    state->entry_cycle = step->cycle_id;
    state->entry_before_surface_callback = !state->surface;
    break;

  case NXANDROID_PHASE_OBJECTS_READY:
    if (!state->entry || step->cycle_id != state->entry_cycle ||
        state->objects_ready || state->saved)
      return NXANDROID_EPROFILE;
    if (state->entry_before_surface_callback) {
      if (!(profile->flags &
            NXANDROID_PROFILE_ALLOW_ENTRY_BEFORE_SURFACE_CALLBACK) ||
          state->surface || !state->graphics_pending || !state->gl_ready ||
          step->cycle_id != state->pending_cycle)
        return NXANDROID_EPROFILE;
    } else if (!state->surface || step->cycle_id != state->active_cycle) {
      return NXANDROID_EPROFILE;
    }
    state->objects_ready = 1;
    break;

  case NXANDROID_PHASE_INPUT_ENABLE:
    if (!state->surface || step->cycle_id != state->active_cycle ||
        !state->objects_ready || state->input || state->run_loop ||
        state->saved)
      return NXANDROID_EPROFILE;
    state->input = 1;
    break;

  case NXANDROID_PHASE_RUN_LOOP:
    if (!state->surface || step->cycle_id != state->active_cycle ||
        !state->entry || !state->objects_ready || !state->input ||
        state->run_loop || state->saved)
      return NXANDROID_EPROFILE;
    state->run_loop = 1;
    break;

  case NXANDROID_PHASE_INPUT_DISABLE:
    if (!state->surface || step->cycle_id != state->active_cycle ||
        !state->input || !state->run_loop)
      return NXANDROID_EPROFILE;
    state->input = 0;
    break;

  case NXANDROID_PHASE_FOCUS_LOSS:
    if (!state->activity || !state->focused ||
        step->cycle_id != state->focused_cycle || state->saved)
      return NXANDROID_EPROFILE;
    state->focused = 0;
    state->focused_cycle = 0u;
    state->shutdown_armed = 0;
    break;

  case NXANDROID_PHASE_PAUSE:
    if (!state->activity || !state->resumed ||
        step->cycle_id != state->resumed_cycle || state->focused ||
        state->saved)
      return NXANDROID_EPROFILE;
    state->resumed = 0;
    state->resumed_cycle = 0u;
    state->shutdown_armed = 1;
    break;

  case NXANDROID_PHASE_SURFACE_DOWN:
    if (!state->surface || step->cycle_id != state->active_cycle ||
        state->input || state->native_shutdown)
      return NXANDROID_EPROFILE;
    state->surface = 0;
    state->active_cycle = 0u;
    state->gl_ready = 0;
    break;

  case NXANDROID_PHASE_SAVE:
    if (!state->entry || !state->objects_ready || state->focused ||
        state->resumed || state->input || !state->shutdown_armed ||
        state->graphics_pending || !state->run_loop || state->saved)
      return NXANDROID_EPROFILE;
    state->saved = 1;
    break;

  case NXANDROID_PHASE_NATIVE_SHUTDOWN:
    if (!state->saved || state->native_shutdown || state->input ||
        state->surface || state->graphics_pending || state->gl_ready)
      return NXANDROID_EPROFILE;
    state->native_shutdown = 1;
    break;

  case NXANDROID_PHASE_TERMINAL:
    if (!state->saved || state->input || state->terminal ||
        index + 1u != profile->step_count)
      return NXANDROID_EPROFILE;
    if (step->terminal_policy == NXANDROID_TERMINAL_RETURN &&
        (!state->native_shutdown || index == 0u ||
         profile->steps[index - 1u].phase !=
             NXANDROID_PHASE_NATIVE_SHUTDOWN))
      return NXANDROID_EPROFILE;
    if (step->terminal_policy == NXANDROID_TERMINAL_ADAPTER &&
        state->native_shutdown &&
        (index == 0u || profile->steps[index - 1u].phase !=
                            NXANDROID_PHASE_NATIVE_SHUTDOWN))
      return NXANDROID_EPROFILE;
    state->terminal = 1;
    state->terminal_policy = step->terminal_policy;
    break;

  case NXANDROID_PHASE_RUNTIME_DELEGATED:
    if (!(profile->flags & NXANDROID_PROFILE_ALLOW_DELEGATED_RUNTIME) ||
        !state->activity || index + 1u != profile->step_count ||
        state->graphics_pending || state->surface || state->gl_ready ||
        state->entry || state->objects_ready || state->input ||
        state->run_loop || state->saved || state->native_shutdown ||
        state->terminal || state->delegated_runtime)
      return NXANDROID_EPROFILE;
    state->delegated_runtime = 1;
    break;

  default:
    return NXANDROID_EPROFILE;
  }
  return NXANDROID_OK;
}

nxandroid_result nxandroid_profile_validate(const nxandroid_profile *profile,
                                             size_t *bad_step) {
  nxandroid_validation_state state;
  nxandroid_result result = NXANDROID_OK;
  size_t index;

  if (bad_step != NULL)
    *bad_step = NXANDROID_NO_MODULE;
  if (profile == NULL || profile->api_version != NXANDROID_API_VERSION ||
      profile->struct_size < sizeof(*profile) || profile->modules == NULL ||
      profile->module_count == 0u ||
      profile->module_count > NXANDROID_MAX_MODULES || profile->steps == NULL ||
      profile->step_count == 0u || profile->step_count > NXANDROID_MAX_STEPS ||
      (profile->flags & ~(NXANDROID_PROFILE_ALLOW_ADAPTER_TERMINAL |
                          NXANDROID_PROFILE_ALLOW_DELEGATED_RUNTIME |
                          NXANDROID_PROFILE_ALLOW_ENTRY_BEFORE_SURFACE_CALLBACK)) !=
          0u)
    return NXANDROID_EINVAL;

  for (index = 0; index < profile->module_count; ++index) {
    size_t other;
    if (!nxandroid_bounded_length(profile->modules[index].name,
                                  NXANDROID_MODULE_NAME_MAX, NULL) ||
        (profile->modules[index].jni_policy != NXANDROID_JNI_NONE &&
         profile->modules[index].jni_policy != NXANDROID_JNI_REQUIRED))
      return NXANDROID_EPROFILE;
    for (other = 0; other < index; ++other) {
      if (strcmp(profile->modules[index].name,
                 profile->modules[other].name) == 0)
        return NXANDROID_EPROFILE;
    }
  }

  result = nxandroid_validate_rollback_groups(profile, bad_step);
  if (result != NXANDROID_OK)
    return result;

  memset(&state, 0, sizeof(state));
  state.module_initialized =
      (unsigned char *)calloc(profile->module_count, sizeof(unsigned char));
  state.module_jni =
      (unsigned char *)calloc(profile->module_count, sizeof(unsigned char));
  if (state.module_initialized == NULL || state.module_jni == NULL) {
    result = NXANDROID_ENOMEM;
    goto done;
  }

  for (index = 0; index < profile->step_count; ++index) {
    result = nxandroid_validate_step(profile, &state, index);
    if (result != NXANDROID_OK) {
      if (bad_step != NULL)
        *bad_step = index;
      goto done;
    }
  }

  if (!state.activity || !nxandroid_all_modules_ready(profile, &state) ||
      ((profile->flags & NXANDROID_PROFILE_ALLOW_DELEGATED_RUNTIME) != 0u) !=
          (state.delegated_runtime != 0) ||
      ((profile->flags & NXANDROID_PROFILE_ALLOW_ADAPTER_TERMINAL) != 0u) !=
          (state.terminal_policy == NXANDROID_TERMINAL_ADAPTER) ||
      ((profile->flags &
        NXANDROID_PROFILE_ALLOW_ENTRY_BEFORE_SURFACE_CALLBACK) != 0u) !=
          (state.entry_before_surface_callback != 0) ||
      (state.delegated_runtime &&
       (state.saw_surface || state.entry || state.objects_ready ||
        state.run_loop || state.saved || state.terminal)) ||
      (!state.delegated_runtime &&
       (!state.saw_surface || !state.entry || !state.objects_ready ||
        !state.run_loop || !state.saved || !state.terminal ||
        state.graphics_pending ||
        (state.terminal_policy == NXANDROID_TERMINAL_RETURN &&
         !state.native_shutdown)))) {
    result = NXANDROID_EPROFILE;
    if (bad_step != NULL)
      *bad_step = profile->step_count;
  }

done:
  free(state.module_initialized);
  free(state.module_jni);
  return result;
}

static void nxandroid_free_profile(nxandroid_context *context) {
  size_t index;
  if (context == NULL)
    return;
  if (context->modules != NULL) {
    for (index = 0; index < context->profile.module_count; ++index)
      free((char *)context->modules[index].name);
  }
  if (context->steps != NULL) {
    for (index = 0; index < context->profile.step_count; ++index) {
      free((char *)context->steps[index].contract_id);
      free((char *)context->steps[index].rollback_contract_id);
    }
  }
  free(context->modules);
  free(context->steps);
  free(context->invoked);
  free(context->rolled_back);
  free(context->rollback_groups);
  free(context->rollback_group_closed);
}

static nxandroid_result nxandroid_clone_profile(
    nxandroid_context *context, const nxandroid_profile *profile) {
  size_t index;

  context->modules = (nxandroid_module_spec *)calloc(
      profile->module_count, sizeof(*context->modules));
  context->steps =
      (nxandroid_step *)calloc(profile->step_count, sizeof(*context->steps));
  context->invoked =
      (unsigned char *)calloc(profile->step_count, sizeof(unsigned char));
  context->rolled_back =
      (unsigned char *)calloc(profile->step_count, sizeof(unsigned char));
  if (context->modules == NULL || context->steps == NULL ||
      context->invoked == NULL || context->rolled_back == NULL)
    return NXANDROID_ENOMEM;

  context->profile = *profile;
  context->profile.modules = context->modules;
  context->profile.steps = context->steps;

  for (index = 0; index < profile->module_count; ++index) {
    context->modules[index] = profile->modules[index];
    context->modules[index].name =
        nxandroid_copy_string(profile->modules[index].name,
                              NXANDROID_MODULE_NAME_MAX);
    if (context->modules[index].name == NULL)
      return NXANDROID_ENOMEM;
  }
  for (index = 0; index < profile->step_count; ++index) {
    context->steps[index] = profile->steps[index];
    context->steps[index].contract_id =
        nxandroid_copy_string(profile->steps[index].contract_id,
                              NXANDROID_CONTRACT_ID_MAX);
    if (context->steps[index].contract_id == NULL)
      return NXANDROID_ENOMEM;
    if (profile->steps[index].rollback_contract_id != NULL) {
      context->steps[index].rollback_contract_id =
          nxandroid_copy_string(profile->steps[index].rollback_contract_id,
                                NXANDROID_CONTRACT_ID_MAX);
      if (context->steps[index].rollback_contract_id == NULL)
        return NXANDROID_ENOMEM;
    }
  }
  return NXANDROID_OK;
}

static nxandroid_result
nxandroid_context_init_groups(nxandroid_context *context) {
  nxandroid_result result = nxandroid_collect_rollback_groups(
      &context->profile, &context->rollback_groups,
      &context->rollback_group_count);
  if (result != NXANDROID_OK || context->rollback_group_count == 0u)
    return result;
  context->rollback_group_closed = (unsigned char *)calloc(
      context->rollback_group_count, sizeof(*context->rollback_group_closed));
  return context->rollback_group_closed != NULL ? NXANDROID_OK
                                                 : NXANDROID_ENOMEM;
}

static int nxandroid_profile_has_rollback(const nxandroid_profile *profile) {
  size_t index;
  for (index = 0; index < profile->step_count; ++index) {
    if (profile->steps[index].rollback_contract_id != NULL &&
        profile->steps[index].rollback_contract_id[0] != '\0')
      return 1;
  }
  return 0;
}

static int nxandroid_callback_reentry(nxandroid_context *context) {
  if (context != NULL && context->callback_active) {
    context->callback_violation = 1;
    return 1;
  }
  return 0;
}

nxandroid_result nxandroid_context_create(const nxandroid_profile *profile,
                                           const nxandroid_ops *ops,
                                           nxandroid_context **output) {
  nxandroid_context *context;
  nxandroid_result result;

  if (output == NULL)
    return NXANDROID_EINVAL;
  *output = NULL;
  result = nxandroid_profile_validate(profile, NULL);
  if (result != NXANDROID_OK)
    return result;
  if (ops == NULL || ops->api_version != NXANDROID_API_VERSION ||
      ops->struct_size < sizeof(*ops) || ops->invoke == NULL ||
      (nxandroid_profile_has_rollback(profile) && ops->rollback == NULL))
    return NXANDROID_EINVAL;

  context = (nxandroid_context *)calloc(1u, sizeof(*context));
  if (context == NULL)
    return NXANDROID_ENOMEM;
  context->ops = *ops;
  context->state = NXANDROID_CONTEXT_READY;
  result = nxandroid_clone_profile(context, profile);
  if (result == NXANDROID_OK)
    result = nxandroid_context_init_groups(context);
  if (result != NXANDROID_OK) {
    nxandroid_free_profile(context);
    free(context);
    return result;
  }
  *output = context;
  return NXANDROID_OK;
}

static nxandroid_result nxandroid_rollback_from(nxandroid_context *context,
                                                size_t end) {
  nxandroid_result result = NXANDROID_OK;
  size_t index = end + 1u;

  while (index > 0u) {
    const nxandroid_step *step;
    int status;
    --index;
    step = &context->steps[index];
    if (!context->invoked[index] || context->rolled_back[index] ||
        step->rollback_contract_id == NULL ||
        step->rollback_contract_id[0] == '\0')
      continue;
    if (step->rollback_group != 0u) {
      size_t group_index = nxandroid_find_u32(
          context->rollback_groups, context->rollback_group_count,
          step->rollback_group);
      if (group_index != NXANDROID_NO_MODULE &&
          context->rollback_group_closed[group_index])
        continue;
    }
    context->callback_violation = 0;
    context->callback_active = 1;
    status = context->ops.rollback(context->ops.userdata, step);
    context->callback_active = 0;
    if (context->callback_violation && status == 0)
      status = NXANDROID_EREENTRANT;
    context->rolled_back[index] = 1u;
    if (status != 0 && result == NXANDROID_OK) {
      context->rollback_status = status;
      result = NXANDROID_EROLLBACK;
    }
  }
  return result;
}

nxandroid_result nxandroid_context_step(nxandroid_context *context) {
  const nxandroid_step *step;
  nxandroid_result rollback_result;
  int status;

  if (context == NULL)
    return NXANDROID_EINVAL;
  if (nxandroid_callback_reentry(context))
    return NXANDROID_EREENTRANT;
  if (context->state != NXANDROID_CONTEXT_READY &&
      context->state != NXANDROID_CONTEXT_RUNNING)
    return NXANDROID_ESTATE;
  if (context->next_step >= context->profile.step_count)
    return NXANDROID_ESTATE;

  context->state = NXANDROID_CONTEXT_RUNNING;
  step = &context->steps[context->next_step];
  context->callback_violation = 0;
  context->callback_active = 1;
  status = context->ops.invoke(context->ops.userdata, step);
  context->callback_active = 0;
  context->invoked[context->next_step] = 1u;
  if (status != 0 || context->callback_violation) {
    int reentrant = context->callback_violation;
    context->adapter_status = status != 0 ? status : NXANDROID_EREENTRANT;
    rollback_result =
        nxandroid_rollback_from(context, context->next_step);
    context->state = NXANDROID_CONTEXT_FAILED;
    if (rollback_result != NXANDROID_OK)
      return rollback_result;
    return reentrant ? NXANDROID_EREENTRANT : NXANDROID_ECALLBACK;
  }

  if (step->closes_rollback_group != 0u) {
    size_t group_index = nxandroid_find_u32(
        context->rollback_groups, context->rollback_group_count,
        step->closes_rollback_group);
    if (group_index == NXANDROID_NO_MODULE) {
      context->state = NXANDROID_CONTEXT_FAILED;
      return NXANDROID_EPROFILE;
    }
    context->rollback_group_closed[group_index] = 1u;
  }

  ++context->next_step;
  if (context->next_step == context->profile.step_count)
    context->state = NXANDROID_CONTEXT_COMPLETE;
  return NXANDROID_OK;
}

nxandroid_result nxandroid_context_run(nxandroid_context *context) {
  nxandroid_result result;
  if (context == NULL)
    return NXANDROID_EINVAL;
  if (nxandroid_callback_reentry(context))
    return NXANDROID_EREENTRANT;
  if (context->state != NXANDROID_CONTEXT_READY &&
      context->state != NXANDROID_CONTEXT_RUNNING)
    return NXANDROID_ESTATE;
  do {
    result = nxandroid_context_step(context);
    if (result != NXANDROID_OK)
      return result;
  } while (context->state == NXANDROID_CONTEXT_RUNNING);
  return NXANDROID_OK;
}

nxandroid_result nxandroid_context_abort(nxandroid_context *context) {
  nxandroid_result result = NXANDROID_OK;
  if (context == NULL)
    return NXANDROID_EINVAL;
  if (nxandroid_callback_reentry(context))
    return NXANDROID_EREENTRANT;
  if (context->state != NXANDROID_CONTEXT_READY &&
      context->state != NXANDROID_CONTEXT_RUNNING)
    return NXANDROID_ESTATE;
  if (context->next_step > 0u)
    result = nxandroid_rollback_from(context, context->next_step - 1u);
  context->state = NXANDROID_CONTEXT_ABORTED;
  return result;
}

nxandroid_result nxandroid_context_destroy(nxandroid_context **context) {
  nxandroid_context *current;
  nxandroid_result result = NXANDROID_OK;
  if (context == NULL || *context == NULL)
    return NXANDROID_EINVAL;
  current = *context;
  if (nxandroid_callback_reentry(current))
    return NXANDROID_EREENTRANT;
  if (current->state == NXANDROID_CONTEXT_READY ||
      current->state == NXANDROID_CONTEXT_RUNNING)
    result = nxandroid_context_abort(current);
  nxandroid_free_profile(current);
  free(current);
  *context = NULL;
  return result;
}

nxandroid_context_state
nxandroid_context_get_state(nxandroid_context *context) {
  if (context == NULL || nxandroid_callback_reentry(context))
    return NXANDROID_CONTEXT_FAILED;
  return context->state;
}

size_t nxandroid_context_get_next_step(nxandroid_context *context) {
  if (context == NULL || nxandroid_callback_reentry(context))
    return NXANDROID_NO_MODULE;
  return context->next_step;
}

int nxandroid_context_get_adapter_status(nxandroid_context *context) {
  if (context == NULL || nxandroid_callback_reentry(context))
    return NXANDROID_EREENTRANT;
  return context->adapter_status;
}

int nxandroid_context_get_rollback_status(nxandroid_context *context) {
  if (context == NULL || nxandroid_callback_reentry(context))
    return NXANDROID_EREENTRANT;
  return context->rollback_status;
}
