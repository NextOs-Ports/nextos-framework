/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxcompat_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(NXCOMPAT_CORE_TESTING) &&                                      \
    (defined(__GNUC__) || defined(__clang__))
/* Deterministic fault injection exists only in the dedicated test target. */
extern int nxcompat_test_environment_set(const char *name, const char *value,
                                         int overwrite) __attribute__((weak));
extern int nxcompat_test_environment_unset(const char *name)
    __attribute__((weak));
#endif

static int nxcompat_environment_set(const char *name, const char *value,
                                    int overwrite) {
#if defined(NXCOMPAT_CORE_TESTING) &&                                      \
    (defined(__GNUC__) || defined(__clang__))
  if (nxcompat_test_environment_set)
    return nxcompat_test_environment_set(name, value, overwrite);
#endif
  return setenv(name, value, overwrite);
}

static int nxcompat_environment_unset(const char *name) {
#if defined(NXCOMPAT_CORE_TESTING) &&                                      \
    (defined(__GNUC__) || defined(__clang__))
  if (nxcompat_test_environment_unset)
    return nxcompat_test_environment_unset(name);
#endif
  return unsetenv(name);
}

typedef struct nxcompat_plan_saved_environment {
  const char *name;
  char *value;
  size_t action_index;
  int existed;
  int modified;
} nxcompat_plan_saved_environment;

static int nxcompat_plan_options_valid(const nxcompat_plan_options *options,
                                       uint32_t required_api) {
  const uint32_t known_policies =
      NXCOMPAT_POLICY_SESSION_RUNTIME | NXCOMPAT_POLICY_PULSE_SERVER |
      NXCOMPAT_POLICY_ARMHF_AUDIO_MODULES | NXCOMPAT_POLICY_CONTROLLER_DB |
      NXCOMPAT_POLICY_XBOX_BUTTON_LABELS |
      NXCOMPAT_POLICY_LOW_MEMORY_ARENAS;
  return options && options->api_version == required_api &&
         options->struct_size >= sizeof(*options) &&
         options->runtime_arch >= NXCOMPAT_ARCH_UNKNOWN &&
         options->runtime_arch <= NXCOMPAT_ARCH_X86_64 &&
         (options->policy_flags & ~known_policies) == 0 &&
         options->low_memory_arena_max <= 1024u;
}

static int nxcompat_bounded_text(const char *value, size_t size) {
  return value && size && memchr(value, '\0', size) != NULL;
}

static const char *nxcompat_action_expected_variable(nxcompat_action_id id) {
  switch (id) {
  case NXCOMPAT_ACTION_SESSION_RUNTIME:
    return "XDG_RUNTIME_DIR";
  case NXCOMPAT_ACTION_PULSE_SERVER:
    return "PULSE_SERVER";
  case NXCOMPAT_ACTION_PIPEWIRE_MODULE_DIR:
    return "PIPEWIRE_MODULE_DIR";
  case NXCOMPAT_ACTION_SPA_PLUGIN_DIR:
    return "SPA_PLUGIN_DIR";
  case NXCOMPAT_ACTION_ALSA_PLUGIN_DIR:
    return "ALSA_PLUGIN_DIR";
  case NXCOMPAT_ACTION_CONTROLLER_DB:
    return "SDL_GAMECONTROLLERCONFIG_FILE";
  case NXCOMPAT_ACTION_XBOX_BUTTON_LABELS:
    return "SDL_GAMECONTROLLER_USE_BUTTON_LABELS";
  case NXCOMPAT_ACTION_MALLOC_ARENAS:
    return "MALLOC_ARENA_MAX";
  default:
    return NULL;
  }
}

static int nxcompat_action_reason_matches_state(
    nxcompat_action_state_v2 state, nxcompat_reason_code reason) {
  switch (state) {
  case NXCOMPAT_ACTION_V2_UNAVAILABLE:
    return reason == NXCOMPAT_REASON_PLAN_CAPABILITY_UNAVAILABLE;
  case NXCOMPAT_ACTION_V2_NOT_NEEDED:
    return reason == NXCOMPAT_REASON_PLAN_INHERITED_PRESERVED ||
           reason == NXCOMPAT_REASON_PLAN_POLICY_NOT_NEEDED ||
           reason == NXCOMPAT_REASON_ENV_LATE_VALUE_PRESERVED;
  case NXCOMPAT_ACTION_V2_PLANNED:
    return reason == NXCOMPAT_REASON_PLAN_CAPABILITY_MATCHED;
  case NXCOMPAT_ACTION_V2_APPLIED:
    return reason == NXCOMPAT_REASON_ENV_APPLIED;
  case NXCOMPAT_ACTION_V2_FAILED:
    return reason == NXCOMPAT_REASON_ENV_APPLY_FAILED;
  case NXCOMPAT_ACTION_V2_ROLLED_BACK:
    return reason == NXCOMPAT_REASON_ENV_ROLLED_BACK;
  case NXCOMPAT_ACTION_V2_ROLLBACK_FAILED:
    return reason == NXCOMPAT_REASON_ENV_ROLLBACK_FAILED;
  default:
    return 0;
  }
}

static int nxcompat_plan_v2_valid(const nxcompat_plan_v2 *plan) {
  size_t first;
  size_t second;
  if (!plan || plan->api_version != NXCOMPAT_API_VERSION_V2 ||
      plan->struct_size < sizeof(*plan) ||
      plan->runtime_arch < NXCOMPAT_ARCH_UNKNOWN ||
      plan->runtime_arch > NXCOMPAT_ARCH_X86_64 ||
      plan->action_count > NXCOMPAT_MAX_ACTIONS)
    return 0;
  for (first = 0; first < plan->action_count; ++first) {
    const nxcompat_action_v2 *action = &plan->actions[first];
    const char *expected;
    if (action->id <= NXCOMPAT_ACTION_NONE ||
        action->id > NXCOMPAT_ACTION_MALLOC_ARENAS ||
        action->state < NXCOMPAT_ACTION_V2_UNAVAILABLE ||
        action->state > NXCOMPAT_ACTION_V2_ROLLBACK_FAILED ||
        !nxcompat_action_reason_matches_state(action->state,
                                              action->reason_code) ||
        !nxcompat_bounded_text(action->variable, sizeof(action->variable)) ||
        !nxcompat_bounded_text(action->value, sizeof(action->value)) ||
        !nxcompat_bounded_text(action->reason, sizeof(action->reason)))
      return 0;
    expected = nxcompat_action_expected_variable(action->id);
    if (action->state == NXCOMPAT_ACTION_V2_PLANNED &&
        (!expected || strcmp(action->variable, expected) != 0))
      return 0;
    for (second = first + 1; second < plan->action_count; ++second)
      if (plan->actions[second].id == action->id ||
          (action->state == NXCOMPAT_ACTION_V2_PLANNED &&
           plan->actions[second].state == NXCOMPAT_ACTION_V2_PLANNED &&
           strcmp(action->variable, plan->actions[second].variable) == 0))
        return 0;
  }
  return 1;
}

static int nxcompat_plan_v2_apply_ready(const nxcompat_plan_v2 *plan) {
  size_t index;
  if (!nxcompat_plan_v2_valid(plan) ||
      plan->final_reason != NXCOMPAT_REASON_PLAN_COMPLETE ||
      plan->apply_count != 0 || plan->rollback_count != 0 ||
      !plan->env_restored)
    return 0;
  for (index = 0; index < plan->action_count; ++index)
    if (plan->actions[index].state > NXCOMPAT_ACTION_V2_PLANNED)
      return 0;
  return 1;
}

static nxcompat_action_v2 *nxcompat_add_action_v2(
    nxcompat_plan_v2 *plan, nxcompat_action_id id,
    nxcompat_action_state_v2 state, nxcompat_reason_code reason_code,
    const char *variable, const char *value, const char *reason) {
  nxcompat_action_v2 *action;
  if (!plan || plan->action_count >= NXCOMPAT_MAX_ACTIONS)
    return NULL;
  action = &plan->actions[plan->action_count++];
  memset(action, 0, sizeof(*action));
  action->id = id;
  action->state = state;
  action->reason_code = reason_code;
  nxcompat_copy_string(action->variable, sizeof(action->variable), variable);
  nxcompat_copy_string(action->value, sizeof(action->value), value);
  nxcompat_copy_string(action->reason, sizeof(action->reason), reason);
  return action;
}

static int nxcompat_environment_exists(const char *name) {
  return getenv(name) != NULL;
}

static const char *nxcompat_environment_value(const char *name) {
  const char *value = getenv(name);
  return value ? value : "";
}

static void nxcompat_plan_session_runtime(const nxcompat_host *host,
                                          nxcompat_plan_v2 *plan) {
  int supports_session =
      (host->capabilities & (NXCOMPAT_CAP_WAYLAND | NXCOMPAT_CAP_PULSE_SOCKET |
                             NXCOMPAT_CAP_PIPEWIRE_SOCKET)) != 0;
  if (nxcompat_environment_exists("XDG_RUNTIME_DIR")) {
    (void)nxcompat_add_action_v2(
        plan, NXCOMPAT_ACTION_SESSION_RUNTIME, NXCOMPAT_ACTION_V2_NOT_NEEDED,
        NXCOMPAT_REASON_PLAN_INHERITED_PRESERVED, "XDG_RUNTIME_DIR",
        nxcompat_environment_value("XDG_RUNTIME_DIR"),
        "inherited session runtime preserved");
  } else if (supports_session && host->session_runtime_dir[0]) {
    (void)nxcompat_add_action_v2(
        plan, NXCOMPAT_ACTION_SESSION_RUNTIME, NXCOMPAT_ACTION_V2_PLANNED,
        NXCOMPAT_REASON_PLAN_CAPABILITY_MATCHED, "XDG_RUNTIME_DIR",
        host->session_runtime_dir,
        "existing firmware session socket directory discovered");
  } else {
    (void)nxcompat_add_action_v2(
        plan, NXCOMPAT_ACTION_SESSION_RUNTIME, NXCOMPAT_ACTION_V2_UNAVAILABLE,
        NXCOMPAT_REASON_PLAN_CAPABILITY_UNAVAILABLE, "XDG_RUNTIME_DIR", "",
        "no usable firmware session socket discovered");
  }
}

static void nxcompat_plan_pulse(const nxcompat_host *host,
                                nxcompat_plan_v2 *plan) {
  char value[NXCOMPAT_PATH_MAX];
  if (nxcompat_environment_exists("PULSE_SERVER")) {
    (void)nxcompat_add_action_v2(
        plan, NXCOMPAT_ACTION_PULSE_SERVER, NXCOMPAT_ACTION_V2_NOT_NEEDED,
        NXCOMPAT_REASON_PLAN_INHERITED_PRESERVED, "PULSE_SERVER",
        nxcompat_environment_value("PULSE_SERVER"),
        "inherited Pulse server preserved");
  } else if ((host->capabilities & NXCOMPAT_CAP_PULSE_SOCKET) != 0 &&
             host->pulse_socket[0] &&
             snprintf(value, sizeof(value), "unix:%s", host->pulse_socket) <
                 (int)sizeof(value)) {
    (void)nxcompat_add_action_v2(
        plan, NXCOMPAT_ACTION_PULSE_SERVER, NXCOMPAT_ACTION_V2_PLANNED,
        NXCOMPAT_REASON_PLAN_CAPABILITY_MATCHED, "PULSE_SERVER", value,
        "real Pulse-compatible socket discovered");
  } else {
    (void)nxcompat_add_action_v2(
        plan, NXCOMPAT_ACTION_PULSE_SERVER, NXCOMPAT_ACTION_V2_UNAVAILABLE,
        NXCOMPAT_REASON_PLAN_CAPABILITY_UNAVAILABLE, "PULSE_SERVER", "",
        "no Pulse-compatible socket discovered");
  }
}

static void nxcompat_plan_one_module(nxcompat_plan_v2 *plan,
                                     nxcompat_action_id id,
                                     const char *variable, const char *path,
                                     const char *reason) {
  if (nxcompat_environment_exists(variable))
    (void)nxcompat_add_action_v2(
        plan, id, NXCOMPAT_ACTION_V2_NOT_NEEDED,
        NXCOMPAT_REASON_PLAN_INHERITED_PRESERVED, variable,
        nxcompat_environment_value(variable),
        "inherited runtime module path preserved");
  else if (path && *path)
    (void)nxcompat_add_action_v2(
        plan, id, NXCOMPAT_ACTION_V2_PLANNED,
        NXCOMPAT_REASON_PLAN_CAPABILITY_MATCHED, variable, path, reason);
  else
    (void)nxcompat_add_action_v2(
        plan, id, NXCOMPAT_ACTION_V2_UNAVAILABLE,
        NXCOMPAT_REASON_PLAN_CAPABILITY_UNAVAILABLE, variable, "",
        "matching ARMHF module directory not found");
}

static void nxcompat_plan_armhf_audio(const nxcompat_host *host,
                                      nxcompat_plan_v2 *plan) {
  if (plan->runtime_arch != NXCOMPAT_ARCH_ARMV7 ||
      host->kernel_arch != NXCOMPAT_ARCH_AARCH64)
    return;
  nxcompat_plan_one_module(
      plan, NXCOMPAT_ACTION_PIPEWIRE_MODULE_DIR, "PIPEWIRE_MODULE_DIR",
      host->armhf_pipewire_modules,
      "ARMHF runtime on AArch64 needs matching PipeWire modules");
  nxcompat_plan_one_module(
      plan, NXCOMPAT_ACTION_SPA_PLUGIN_DIR, "SPA_PLUGIN_DIR",
      host->armhf_spa_plugins,
      "ARMHF runtime on AArch64 needs matching SPA plugins");
  nxcompat_plan_one_module(
      plan, NXCOMPAT_ACTION_ALSA_PLUGIN_DIR, "ALSA_PLUGIN_DIR",
      host->armhf_alsa_plugins,
      "ARMHF runtime on AArch64 needs matching ALSA plugins");
}

static void nxcompat_plan_controller_db(const nxcompat_host *host,
                                        nxcompat_plan_v2 *plan) {
  if (nxcompat_environment_exists("SDL_GAMECONTROLLERCONFIG")) {
    (void)nxcompat_add_action_v2(
        plan, NXCOMPAT_ACTION_CONTROLLER_DB, NXCOMPAT_ACTION_V2_NOT_NEEDED,
        NXCOMPAT_REASON_PLAN_INHERITED_PRESERVED,
        "SDL_GAMECONTROLLERCONFIG", "<PortMaster mapping>",
        "inherited get_controls mapping preserved");
  } else if (nxcompat_environment_exists("SDL_GAMECONTROLLERCONFIG_FILE")) {
    (void)nxcompat_add_action_v2(
        plan, NXCOMPAT_ACTION_CONTROLLER_DB, NXCOMPAT_ACTION_V2_NOT_NEEDED,
        NXCOMPAT_REASON_PLAN_INHERITED_PRESERVED,
        "SDL_GAMECONTROLLERCONFIG_FILE",
        nxcompat_environment_value("SDL_GAMECONTROLLERCONFIG_FILE"),
        "inherited controller database preserved");
  } else if (host->controller_db[0]) {
    (void)nxcompat_add_action_v2(
        plan, NXCOMPAT_ACTION_CONTROLLER_DB, NXCOMPAT_ACTION_V2_PLANNED,
        NXCOMPAT_REASON_PLAN_CAPABILITY_MATCHED,
        "SDL_GAMECONTROLLERCONFIG_FILE", host->controller_db,
        "PortMaster SDL2 controller database discovered");
  } else {
    (void)nxcompat_add_action_v2(
        plan, NXCOMPAT_ACTION_CONTROLLER_DB, NXCOMPAT_ACTION_V2_UNAVAILABLE,
        NXCOMPAT_REASON_PLAN_CAPABILITY_UNAVAILABLE,
        "SDL_GAMECONTROLLERCONFIG_FILE", "",
        "no controller mapping or readable database discovered");
  }
}

static void nxcompat_plan_xbox_labels(nxcompat_plan_v2 *plan) {
  if (nxcompat_environment_exists("SDL_GAMECONTROLLER_USE_BUTTON_LABELS"))
    (void)nxcompat_add_action_v2(
        plan, NXCOMPAT_ACTION_XBOX_BUTTON_LABELS,
        NXCOMPAT_ACTION_V2_NOT_NEEDED,
        NXCOMPAT_REASON_PLAN_INHERITED_PRESERVED,
        "SDL_GAMECONTROLLER_USE_BUTTON_LABELS",
        nxcompat_environment_value("SDL_GAMECONTROLLER_USE_BUTTON_LABELS"),
        "inherited button semantics preserved");
  else
    (void)nxcompat_add_action_v2(
        plan, NXCOMPAT_ACTION_XBOX_BUTTON_LABELS, NXCOMPAT_ACTION_V2_PLANNED,
        NXCOMPAT_REASON_PLAN_CAPABILITY_MATCHED,
        "SDL_GAMECONTROLLER_USE_BUTTON_LABELS", "0",
        "nxinput exposes stable Xbox positions to the engine adapter");
}

static void nxcompat_plan_arenas(const nxcompat_host *host,
                                 const nxcompat_plan_options *options,
                                 nxcompat_plan_v2 *plan) {
  char value[32];
  unsigned arenas = options->low_memory_arena_max;
  if (arenas == 0)
    arenas = 2;
  if (nxcompat_environment_exists("MALLOC_ARENA_MAX"))
    (void)nxcompat_add_action_v2(
        plan, NXCOMPAT_ACTION_MALLOC_ARENAS, NXCOMPAT_ACTION_V2_NOT_NEEDED,
        NXCOMPAT_REASON_PLAN_INHERITED_PRESERVED, "MALLOC_ARENA_MAX",
        nxcompat_environment_value("MALLOC_ARENA_MAX"),
        "inherited allocator policy preserved");
  else if (host->memory_class == NXCOMPAT_MEMORY_SHORT) {
    (void)snprintf(value, sizeof(value), "%u", arenas);
    (void)nxcompat_add_action_v2(
        plan, NXCOMPAT_ACTION_MALLOC_ARENAS, NXCOMPAT_ACTION_V2_PLANNED,
        NXCOMPAT_REASON_PLAN_CAPABILITY_MATCHED, "MALLOC_ARENA_MAX", value,
        "port enabled the measured short-memory allocator policy");
  } else
    (void)nxcompat_add_action_v2(
        plan, NXCOMPAT_ACTION_MALLOC_ARENAS, NXCOMPAT_ACTION_V2_NOT_NEEDED,
        NXCOMPAT_REASON_PLAN_POLICY_NOT_NEEDED, "MALLOC_ARENA_MAX", "",
        "host is not in the short-memory class");
}

static nxcompat_result_code nxcompat_plan_environment_core(
    const nxcompat_host *host, const nxcompat_plan_options *options,
    nxcompat_plan_v2 *plan) {
  uint32_t policies = options->policy_flags;
  plan->runtime_arch = options->runtime_arch == NXCOMPAT_ARCH_UNKNOWN
                           ? host->process_arch
                           : options->runtime_arch;
  if ((policies & NXCOMPAT_POLICY_SESSION_RUNTIME) != 0)
    nxcompat_plan_session_runtime(host, plan);
  if ((policies & NXCOMPAT_POLICY_PULSE_SERVER) != 0)
    nxcompat_plan_pulse(host, plan);
  if ((policies & NXCOMPAT_POLICY_ARMHF_AUDIO_MODULES) != 0)
    nxcompat_plan_armhf_audio(host, plan);
  if ((policies & NXCOMPAT_POLICY_CONTROLLER_DB) != 0)
    nxcompat_plan_controller_db(host, plan);
  if ((policies & NXCOMPAT_POLICY_XBOX_BUTTON_LABELS) != 0)
    nxcompat_plan_xbox_labels(plan);
  if ((policies & NXCOMPAT_POLICY_LOW_MEMORY_ARENAS) != 0)
    nxcompat_plan_arenas(host, options, plan);
  plan->final_reason = NXCOMPAT_REASON_PLAN_COMPLETE;
  plan->env_restored = 1;
  return NXCOMPAT_OK;
}

nxcompat_result_code nxcompat_plan_environment_v2(
    const nxcompat_host *host, const nxcompat_plan_options *options,
    nxcompat_plan_v2 *plan) {
  if (plan) {
    memset(plan, 0, sizeof(*plan));
    plan->api_version = NXCOMPAT_API_VERSION_V2;
    plan->struct_size = sizeof(*plan);
    plan->final_reason = NXCOMPAT_REASON_INVALID_ARGUMENT;
    plan->env_restored = 1;
  }
  if (!plan || !nxcompat_host_instance_valid(host) ||
      !nxcompat_plan_options_valid(options, NXCOMPAT_API_VERSION_V2))
    return NXCOMPAT_INVALID;
  if (nxcompat_global_arbiter_try_acquire() != NXCOMPAT_OK) {
    plan->final_reason = NXCOMPAT_REASON_ARBITER_BUSY;
    return NXCOMPAT_BUSY;
  }
  (void)nxcompat_plan_environment_core(host, options, plan);
  nxcompat_global_arbiter_release();
  return NXCOMPAT_OK;
}

static char *nxcompat_duplicate_value(const char *value) {
  size_t size = strlen(value) + 1u;
  char *copy = (char *)malloc(size);
  if (copy)
    memcpy(copy, value, size);
  return copy;
}

static void nxcompat_free_apply_snapshot(nxcompat_plan_saved_environment *saved,
                                         size_t count) {
  size_t index;
  for (index = 0; index < count; ++index)
    free(saved[index].value);
}

static int nxcompat_snapshot_apply(const nxcompat_plan_v2 *plan,
                                   nxcompat_plan_saved_environment *saved,
                                   size_t *saved_count) {
  size_t index;
  memset(saved, 0,
         sizeof(*saved) * (size_t)NXCOMPAT_MAX_ACTIONS);
  *saved_count = plan->action_count;
  for (index = 0; index < plan->action_count; ++index) {
    const nxcompat_action_v2 *action = &plan->actions[index];
    const char *value;
    nxcompat_plan_saved_environment *entry;
    if (action->state != NXCOMPAT_ACTION_V2_PLANNED)
      continue;
    entry = &saved[index];
    entry->name = action->variable;
    entry->action_index = index;
    value = getenv(entry->name);
    if (value) {
      entry->existed = 1;
      entry->value = nxcompat_duplicate_value(value);
      if (!entry->value) {
        nxcompat_free_apply_snapshot(saved, *saved_count);
        *saved_count = 0;
        return -1;
      }
    }
  }
  return 0;
}

static int nxcompat_apply_entry_matches(
    const nxcompat_plan_saved_environment *entry) {
  const char *current = getenv(entry->name);
  if (entry->existed)
    return current && strcmp(current, entry->value) == 0;
  return current == NULL;
}

static int nxcompat_apply_value_matches(const nxcompat_action_v2 *action) {
  const char *current = getenv(action->variable);
  return current && strcmp(current, action->value) == 0;
}

static int nxcompat_restore_apply_entry(
    const nxcompat_plan_saved_environment *entry) {
  int status = entry->existed
                   ? nxcompat_environment_set(entry->name, entry->value, 1)
                   : nxcompat_environment_unset(entry->name);
  return status == 0 && nxcompat_apply_entry_matches(entry) ? 0 : -1;
}

nxcompat_result_code nxcompat_apply_environment_v2(nxcompat_plan_v2 *plan) {
  nxcompat_plan_saved_environment saved[NXCOMPAT_MAX_ACTIONS];
  size_t saved_count = 0;
  size_t index;
  size_t failed_at = (size_t)-1;
  int rollback_failed = 0;
  if (!nxcompat_plan_v2_apply_ready(plan)) {
    if (plan && plan->api_version == NXCOMPAT_API_VERSION_V2 &&
        plan->struct_size >= sizeof(*plan))
      plan->final_reason = NXCOMPAT_REASON_INVALID_ARGUMENT;
    return NXCOMPAT_INVALID;
  }
  plan->apply_count = 0;
  plan->rollback_count = 0;
  plan->env_restored = 1;
  if (nxcompat_global_arbiter_try_acquire() != NXCOMPAT_OK) {
    plan->final_reason = NXCOMPAT_REASON_ARBITER_BUSY;
    return NXCOMPAT_BUSY;
  }
  if (nxcompat_snapshot_apply(plan, saved, &saved_count) != 0) {
    plan->final_reason = NXCOMPAT_REASON_ENV_APPLY_FAILED;
    nxcompat_global_arbiter_release();
    return NXCOMPAT_FAILED;
  }

  for (index = 0; index < saved_count; ++index) {
    nxcompat_plan_saved_environment *entry = &saved[index];
    nxcompat_action_v2 *action;
    const char *late;
    if (!entry->name)
      continue;
    action = &plan->actions[entry->action_index];
    late = getenv(entry->name);
    if (late) {
      action->state = NXCOMPAT_ACTION_V2_NOT_NEEDED;
      action->reason_code = NXCOMPAT_REASON_ENV_LATE_VALUE_PRESERVED;
      nxcompat_copy_string(action->value, sizeof(action->value), late);
      nxcompat_copy_string(action->reason, sizeof(action->reason),
                           "environment changed after planning; preserved it");
      continue;
    }
    if (nxcompat_environment_set(action->variable, action->value, 0) != 0 ||
        !nxcompat_apply_value_matches(action)) {
      /* A failing provider may have mutated the process environment before
       * returning an error.  Include that partial write in the transaction. */
      entry->modified = !nxcompat_apply_entry_matches(entry);
      action->state = NXCOMPAT_ACTION_V2_FAILED;
      action->reason_code = NXCOMPAT_REASON_ENV_APPLY_FAILED;
      nxcompat_copy_string(action->reason, sizeof(action->reason),
                           "environment apply failed; starting rollback");
      failed_at = index;
      break;
    }
    entry->modified = 1;
    ++plan->apply_count;
    action->state = NXCOMPAT_ACTION_V2_APPLIED;
    action->reason_code = NXCOMPAT_REASON_ENV_APPLIED;
    nxcompat_copy_string(action->reason, sizeof(action->reason),
                         "process-local environment action applied");
  }

  if (failed_at == (size_t)-1) {
    plan->final_reason = NXCOMPAT_REASON_ENV_APPLY_COMPLETE;
    plan->env_restored = plan->apply_count == 0;
    nxcompat_free_apply_snapshot(saved, saved_count);
    nxcompat_global_arbiter_release();
    return NXCOMPAT_OK;
  }

  index = failed_at + 1u;
  while (index > 0) {
    nxcompat_plan_saved_environment *entry;
    nxcompat_action_v2 *action;
    --index;
    entry = &saved[index];
    if (!entry->name || !entry->modified)
      continue;
    action = &plan->actions[entry->action_index];
    ++plan->rollback_count;
    if (nxcompat_restore_apply_entry(entry) == 0) {
      action->state = NXCOMPAT_ACTION_V2_ROLLED_BACK;
      action->reason_code = NXCOMPAT_REASON_ENV_ROLLED_BACK;
      nxcompat_copy_string(action->reason, sizeof(action->reason),
                           "environment action rolled back and verified");
    } else {
      action->state = NXCOMPAT_ACTION_V2_ROLLBACK_FAILED;
      action->reason_code = NXCOMPAT_REASON_ENV_ROLLBACK_FAILED;
      nxcompat_copy_string(action->reason, sizeof(action->reason),
                           "environment rollback could not be verified");
      rollback_failed = 1;
    }
  }
  for (index = 0; index < saved_count; ++index)
    if (saved[index].name && !nxcompat_apply_entry_matches(&saved[index]))
      rollback_failed = 1;
  plan->env_restored = !rollback_failed;
  plan->final_reason = rollback_failed
                           ? NXCOMPAT_REASON_ENV_ROLLBACK_FAILED
                           : NXCOMPAT_REASON_ENV_APPLY_FAILED;
  nxcompat_free_apply_snapshot(saved, saved_count);
  nxcompat_global_arbiter_release();
  return rollback_failed ? NXCOMPAT_ROLLBACK_FAILED : NXCOMPAT_FAILED;
}

static nxcompat_action_state nxcompat_legacy_action_state(
    nxcompat_action_state_v2 state) {
  switch (state) {
  case NXCOMPAT_ACTION_V2_NOT_NEEDED:
    return NXCOMPAT_ACTION_NOT_NEEDED;
  case NXCOMPAT_ACTION_V2_PLANNED:
    return NXCOMPAT_ACTION_PLANNED;
  case NXCOMPAT_ACTION_V2_APPLIED:
    return NXCOMPAT_ACTION_APPLIED;
  case NXCOMPAT_ACTION_V2_FAILED:
  case NXCOMPAT_ACTION_V2_ROLLBACK_FAILED:
    return NXCOMPAT_ACTION_FAILED;
  case NXCOMPAT_ACTION_V2_ROLLED_BACK:
    return NXCOMPAT_ACTION_PLANNED;
  default:
    return NXCOMPAT_ACTION_UNAVAILABLE;
  }
}

static void nxcompat_copy_plan_to_legacy(const nxcompat_plan_v2 *source,
                                         nxcompat_plan *destination,
                                         uint32_t api_version) {
  size_t index;
  memset(destination, 0, sizeof(*destination));
  destination->api_version = api_version;
  destination->struct_size = sizeof(*destination);
  destination->runtime_arch = source->runtime_arch;
  destination->action_count = source->action_count;
  for (index = 0; index < source->action_count; ++index) {
    const nxcompat_action_v2 *from = &source->actions[index];
    nxcompat_action *to = &destination->actions[index];
    to->id = from->id;
    to->state = nxcompat_legacy_action_state(from->state);
    nxcompat_copy_string(to->variable, sizeof(to->variable), from->variable);
    nxcompat_copy_string(to->value, sizeof(to->value), from->value);
    nxcompat_copy_string(to->reason, sizeof(to->reason), from->reason);
  }
}

int nxcompat_plan_environment(const nxcompat_host *host,
                              const nxcompat_plan_options *options,
                              nxcompat_plan *plan) {
  nxcompat_plan_options options_v2;
  nxcompat_plan_v2 plan_v2;
  nxcompat_result_code status;
  if (!options || !plan || !nxcompat_host_instance_valid(host) ||
      !nxcompat_api_version_supported(options->api_version) ||
      options->struct_size < sizeof(*options))
    return -1;
  options_v2 = *options;
  options_v2.api_version = NXCOMPAT_API_VERSION_V2;
  status = nxcompat_plan_environment_v2(host, &options_v2, &plan_v2);
  nxcompat_copy_plan_to_legacy(&plan_v2, plan, options->api_version);
  return status == NXCOMPAT_OK ? 0 : -1;
}

int nxcompat_apply_environment(nxcompat_plan *plan) {
  nxcompat_plan_v2 plan_v2;
  nxcompat_result_code status;
  size_t index;
  uint32_t legacy_api_version;
  if (!nxcompat_plan_instance_valid(plan))
    return -1;
  legacy_api_version = plan->api_version;
  memset(&plan_v2, 0, sizeof(plan_v2));
  plan_v2.api_version = NXCOMPAT_API_VERSION_V2;
  plan_v2.struct_size = sizeof(plan_v2);
  plan_v2.runtime_arch = plan->runtime_arch;
  plan_v2.action_count = plan->action_count;
  plan_v2.final_reason = NXCOMPAT_REASON_PLAN_COMPLETE;
  plan_v2.env_restored = 1;
  for (index = 0; index < plan->action_count; ++index) {
    plan_v2.actions[index].id = plan->actions[index].id;
    plan_v2.actions[index].state =
        (nxcompat_action_state_v2)plan->actions[index].state;
    switch (plan_v2.actions[index].state) {
    case NXCOMPAT_ACTION_V2_UNAVAILABLE:
      plan_v2.actions[index].reason_code =
          NXCOMPAT_REASON_PLAN_CAPABILITY_UNAVAILABLE;
      break;
    case NXCOMPAT_ACTION_V2_NOT_NEEDED:
      plan_v2.actions[index].reason_code =
          NXCOMPAT_REASON_PLAN_INHERITED_PRESERVED;
      break;
    case NXCOMPAT_ACTION_V2_PLANNED:
      plan_v2.actions[index].reason_code =
          NXCOMPAT_REASON_PLAN_CAPABILITY_MATCHED;
      break;
    case NXCOMPAT_ACTION_V2_APPLIED:
      plan_v2.actions[index].reason_code = NXCOMPAT_REASON_ENV_APPLIED;
      break;
    default:
      plan_v2.actions[index].reason_code = NXCOMPAT_REASON_ENV_APPLY_FAILED;
      break;
    }
    nxcompat_copy_string(plan_v2.actions[index].variable,
                         sizeof(plan_v2.actions[index].variable),
                         plan->actions[index].variable);
    nxcompat_copy_string(plan_v2.actions[index].value,
                         sizeof(plan_v2.actions[index].value),
                         plan->actions[index].value);
    nxcompat_copy_string(plan_v2.actions[index].reason,
                         sizeof(plan_v2.actions[index].reason),
                         plan->actions[index].reason);
  }
  status = nxcompat_apply_environment_v2(&plan_v2);
  nxcompat_copy_plan_to_legacy(&plan_v2, plan, legacy_api_version);
  return status == NXCOMPAT_OK ? 0 : -1;
}
