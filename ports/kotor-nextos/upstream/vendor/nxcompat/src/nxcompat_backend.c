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

typedef struct nxcompat_saved_environment {
  const char *name;
  char *value;
  int existed;
} nxcompat_saved_environment;

typedef struct nxcompat_legacy_backend {
  const nxcompat_backend_options *options;
  int last_attempt_failed;
} nxcompat_legacy_backend;

static int nxcompat_bounded_text(const char *value, size_t size) {
  return value && size != 0 && memchr(value, '\0', size) != NULL;
}

static int nxcompat_backend_kind_valid(nxcompat_backend_kind kind) {
  return kind == NXCOMPAT_BACKEND_VIDEO || kind == NXCOMPAT_BACKEND_AUDIO;
}

static int nxcompat_environment_name_valid(const char *name) {
  return !name || (!strchr(name, '=') && *name);
}

static int nxcompat_environment_names_valid(
    const char *const names[NXCOMPAT_BACKEND_ENV_MAX]) {
  size_t first;
  size_t second;
  for (first = 0; first < NXCOMPAT_BACKEND_ENV_MAX; ++first) {
    if (!nxcompat_environment_name_valid(names[first]))
      return 0;
    if (!names[first])
      continue;
    for (second = first + 1; second < NXCOMPAT_BACKEND_ENV_MAX; ++second)
      if (names[second] && strcmp(names[first], names[second]) == 0)
        return 0;
  }
  return 1;
}

static int
nxcompat_backend_options_v2_valid(const nxcompat_backend_options_v2 *options) {
  return options && options->api_version == NXCOMPAT_API_VERSION_V2 &&
         options->struct_size >= sizeof(*options) &&
         nxcompat_backend_kind_valid(options->kind) && options->attempt &&
         options->cleanup && options->accept_name &&
         nxcompat_environment_names_valid(options->environment_names);
}

static const char *nxcompat_backend_kind_name(nxcompat_backend_kind kind) {
  return kind == NXCOMPAT_BACKEND_AUDIO ? "audio" : "video";
}

const char *nxcompat_backend_state_name(nxcompat_backend_state value) {
  switch (value) {
  case NXCOMPAT_BACKEND_INHERITED_OK:
    return "inherited-ok";
  case NXCOMPAT_BACKEND_AUTODETECT_OK:
    return "autodetect-ok";
  case NXCOMPAT_BACKEND_RECOVERED_BY_AUTODETECT:
    return "recovered-by-autodetect";
  case NXCOMPAT_BACKEND_RECOVERED_BY_DISCOVERY:
    return "recovered-by-discovery";
  case NXCOMPAT_BACKEND_FAILED:
    return "failed";
  default:
    return "untested";
  }
}

static char *nxcompat_duplicate_environment_value(const char *value) {
  size_t size;
  char *copy;
  if (!value)
    return NULL;
  size = strlen(value) + 1u;
  copy = (char *)malloc(size);
  if (copy)
    memcpy(copy, value, size);
  return copy;
}

static void nxcompat_free_environment(
    nxcompat_saved_environment saved[NXCOMPAT_BACKEND_ENV_MAX]) {
  size_t index;
  for (index = 0; index < NXCOMPAT_BACKEND_ENV_MAX; ++index) {
    free(saved[index].value);
    saved[index].value = NULL;
  }
}

static int nxcompat_snapshot_environment(
    const nxcompat_backend_options_v2 *options,
    nxcompat_saved_environment saved[NXCOMPAT_BACKEND_ENV_MAX],
    unsigned *inherited_count) {
  size_t index;
  *inherited_count = 0;
  memset(saved, 0, sizeof(*saved) * NXCOMPAT_BACKEND_ENV_MAX);
  for (index = 0; index < NXCOMPAT_BACKEND_ENV_MAX; ++index) {
    const char *value;
    saved[index].name = options->environment_names[index];
    if (!saved[index].name)
      continue;
    value = getenv(saved[index].name);
    if (!value)
      continue;
    saved[index].existed = 1;
    saved[index].value = nxcompat_duplicate_environment_value(value);
    if (!saved[index].value) {
      nxcompat_free_environment(saved);
      return -1;
    }
    if (*value)
      ++*inherited_count;
  }
  return 0;
}

static int nxcompat_environment_matches(
    const nxcompat_saved_environment saved[NXCOMPAT_BACKEND_ENV_MAX]) {
  size_t index;
  for (index = 0; index < NXCOMPAT_BACKEND_ENV_MAX; ++index) {
    const char *current;
    if (!saved[index].name)
      continue;
    current = getenv(saved[index].name);
    if (saved[index].existed) {
      if (!current || strcmp(current, saved[index].value) != 0)
        return 0;
    } else if (current) {
      return 0;
    }
  }
  return 1;
}

static int nxcompat_environment_all_absent(
    const nxcompat_saved_environment saved[NXCOMPAT_BACKEND_ENV_MAX]) {
  size_t index;
  for (index = 0; index < NXCOMPAT_BACKEND_ENV_MAX; ++index)
    if (saved[index].name && getenv(saved[index].name))
      return 0;
  return 1;
}

static int nxcompat_clear_environment(
    const nxcompat_saved_environment saved[NXCOMPAT_BACKEND_ENV_MAX]) {
  size_t index;
  int failed = 0;
  for (index = 0; index < NXCOMPAT_BACKEND_ENV_MAX; ++index)
    if (saved[index].name && nxcompat_environment_unset(saved[index].name) != 0)
      failed = 1;
  return failed || !nxcompat_environment_all_absent(saved) ? -1 : 0;
}

static int nxcompat_restore_environment(
    const nxcompat_saved_environment saved[NXCOMPAT_BACKEND_ENV_MAX]) {
  size_t index;
  int failed = 0;
  for (index = 0; index < NXCOMPAT_BACKEND_ENV_MAX; ++index) {
    if (!saved[index].name)
      continue;
    if (saved[index].existed) {
      if (nxcompat_environment_set(saved[index].name, saved[index].value, 1) !=
          0)
        failed = 1;
    } else if (nxcompat_environment_unset(saved[index].name) != 0) {
      failed = 1;
    }
  }
  return failed || !nxcompat_environment_matches(saved) ? -1 : 0;
}

static nxcompat_reason_code nxcompat_default_attempt_reason(
    nxcompat_backend_attempt_outcome outcome) {
  switch (outcome) {
  case NXCOMPAT_BACKEND_ATTEMPT_RETRYABLE_FAILURE:
    return NXCOMPAT_REASON_BACKEND_ATTEMPT_RETRYABLE;
  case NXCOMPAT_BACKEND_ATTEMPT_OWNERSHIP_BUSY:
    return NXCOMPAT_REASON_BACKEND_ATTEMPT_BUSY;
  case NXCOMPAT_BACKEND_ATTEMPT_FATAL_FAILURE:
    return NXCOMPAT_REASON_BACKEND_ATTEMPT_FATAL;
  default:
    return NXCOMPAT_REASON_NONE;
  }
}

static int nxcompat_backend_reason_valid(nxcompat_reason_code reason) {
  return reason == NXCOMPAT_REASON_NONE ||
         (reason >= NXCOMPAT_REASON_INVALID_ARGUMENT &&
          reason <= NXCOMPAT_REASON_PROVIDER_CONTRACT) ||
         (reason >= NXCOMPAT_REASON_BACKEND_INHERITED_OK &&
          reason <= NXCOMPAT_REASON_AUDIO_DEVICE_INVALID);
}

static nxcompat_backend_attempt_outcome nxcompat_attempt_once(
    const nxcompat_backend_options_v2 *options,
    nxcompat_backend_result_v2 *result, nxcompat_backend_attempt_report *report) {
  nxcompat_backend_attempt_outcome outcome;
  memset(report, 0, sizeof(*report));
  report->api_version = NXCOMPAT_API_VERSION_V2;
  report->struct_size = sizeof(*report);
  ++result->attempt_count;
  outcome = options->attempt(options->userdata, report);
  if (outcome < NXCOMPAT_BACKEND_ATTEMPT_OK ||
      outcome > NXCOMPAT_BACKEND_ATTEMPT_FATAL_FAILURE ||
      report->api_version != NXCOMPAT_API_VERSION_V2 ||
      report->struct_size < sizeof(*report) ||
      !nxcompat_bounded_text(report->selected, sizeof(report->selected)) ||
      !nxcompat_bounded_text(report->error, sizeof(report->error)) ||
      !nxcompat_backend_reason_valid(report->reason)) {
    memset(report, 0, sizeof(*report));
    report->api_version = NXCOMPAT_API_VERSION_V2;
    report->struct_size = sizeof(*report);
    report->reason = NXCOMPAT_REASON_PROVIDER_CONTRACT;
    nxcompat_copy_string(report->error, sizeof(report->error),
                         "backend provider returned an invalid attempt report");
    return NXCOMPAT_BACKEND_ATTEMPT_FATAL_FAILURE;
  }
  if (outcome == NXCOMPAT_BACKEND_ATTEMPT_OK &&
      (!report->selected[0] ||
       !options->accept_name(options->userdata, report->selected))) {
    report->reason = NXCOMPAT_REASON_BACKEND_NAME_REJECTED;
    if (!report->error[0])
      nxcompat_copy_string(report->error, sizeof(report->error),
                           "backend opened but its output name was rejected");
    return NXCOMPAT_BACKEND_ATTEMPT_RETRYABLE_FAILURE;
  }
  if (report->reason == NXCOMPAT_REASON_NONE)
    report->reason = nxcompat_default_attempt_reason(outcome);
  return outcome;
}

static nxcompat_result_code nxcompat_cleanup_once(
    const nxcompat_backend_options_v2 *options,
    nxcompat_backend_result_v2 *result, char *error, size_t error_size) {
  nxcompat_result_code status;
  error[0] = '\0';
  ++result->cleanup_count;
  status = options->cleanup(options->userdata, error, error_size);
  if (!nxcompat_bounded_text(error, error_size)) {
    if (error_size)
      error[error_size - 1u] = '\0';
    return NXCOMPAT_ROLLBACK_FAILED;
  }
  return status == NXCOMPAT_OK ? NXCOMPAT_OK : NXCOMPAT_ROLLBACK_FAILED;
}

static void nxcompat_record_first(nxcompat_backend_result_v2 *result,
                                  const nxcompat_backend_attempt_report *report) {
  result->first_reason = report->reason;
  nxcompat_copy_string(result->first_error, sizeof(result->first_error),
                       report->error);
}

static void nxcompat_record_final(nxcompat_backend_result_v2 *result,
                                  const nxcompat_backend_attempt_report *report) {
  result->final_reason = report->reason;
  nxcompat_copy_string(result->final_error, sizeof(result->final_error),
                       report->error);
}

static void nxcompat_record_cleanup_failure(nxcompat_backend_result_v2 *result,
                                            const char *error) {
  result->state = NXCOMPAT_BACKEND_V2_ROLLBACK_FAILED;
  result->final_reason = NXCOMPAT_REASON_BACKEND_CLEANUP_FAILED;
  nxcompat_copy_string(result->final_error, sizeof(result->final_error),
                       error && *error ? error
                                       : "backend cleanup could not be verified");
}

static void nxcompat_emit_backend_status_v2(
    const nxcompat_backend_options_v2 *options,
    const nxcompat_backend_result_v2 *result) {
  char message[NXCOMPAT_DETAIL_MAX];
  if (!options->status)
    return;
  if (result->state == NXCOMPAT_BACKEND_V2_BUSY)
    (void)snprintf(message, sizeof(message), "%s negotiation busy",
                   nxcompat_backend_kind_name(result->kind));
  else if (result->state == NXCOMPAT_BACKEND_V2_FAILED ||
           result->state == NXCOMPAT_BACKEND_V2_ROLLBACK_FAILED)
    (void)snprintf(message, sizeof(message), "%s unavailable after %u attempt%s",
                   nxcompat_backend_kind_name(result->kind), result->attempt_count,
                   result->attempt_count == 1 ? "" : "s");
  else if (result->state == NXCOMPAT_BACKEND_V2_RECOVERED_BY_AUTODETECT)
    (void)snprintf(message, sizeof(message),
                   "%s %s; inherited hint removed after a recorded failure",
                   nxcompat_backend_kind_name(result->kind), result->selected);
  else
    (void)snprintf(message, sizeof(message), "%s %s (%s)",
                   nxcompat_backend_kind_name(result->kind), result->selected,
                   result->state == NXCOMPAT_BACKEND_V2_INHERITED_OK
                       ? "inherited-ok"
                       : "autodetect-ok");
  options->status(options->status_userdata, NXCOMPAT_STATUS_BACKEND, message);
}

nxcompat_result_code
nxcompat_negotiate_backend_v2(const nxcompat_backend_options_v2 *options,
                              nxcompat_backend_result_v2 *result) {
  nxcompat_saved_environment saved[NXCOMPAT_BACKEND_ENV_MAX];
  nxcompat_backend_attempt_report report;
  nxcompat_backend_attempt_outcome outcome;
  nxcompat_result_code return_code = NXCOMPAT_FAILED;
  char cleanup_error[NXCOMPAT_DETAIL_MAX];
  unsigned inherited_count = 0;
  int locked = 0;
  int snapshot_valid = 0;

  if (result) {
    memset(result, 0, sizeof(*result));
    result->api_version = NXCOMPAT_API_VERSION_V2;
    result->struct_size = sizeof(*result);
    result->env_restored = 1;
    result->final_reason = NXCOMPAT_REASON_INVALID_ARGUMENT;
    if (options && nxcompat_backend_kind_valid(options->kind))
      result->kind = options->kind;
  }
  if (!result || !nxcompat_backend_options_v2_valid(options))
    return NXCOMPAT_INVALID;

  if (nxcompat_global_arbiter_try_acquire() != NXCOMPAT_OK) {
    result->state = NXCOMPAT_BACKEND_V2_BUSY;
    result->final_reason = NXCOMPAT_REASON_ARBITER_BUSY;
    nxcompat_copy_string(result->final_error, sizeof(result->final_error),
                         "nxcompat global state is owned by another operation");
    nxcompat_emit_backend_status_v2(options, result);
    return NXCOMPAT_BUSY;
  }
  locked = 1;

  if (nxcompat_snapshot_environment(options, saved, &inherited_count) != 0) {
    result->state = NXCOMPAT_BACKEND_V2_FAILED;
    result->final_reason = NXCOMPAT_REASON_BACKEND_ATTEMPT_FATAL;
    nxcompat_copy_string(result->final_error, sizeof(result->final_error),
                         "could not snapshot inherited backend hints");
    goto out;
  }
  snapshot_valid = 1;
  result->inherited_count = inherited_count;

  outcome = nxcompat_attempt_once(options, result, &report);
  if (outcome == NXCOMPAT_BACKEND_ATTEMPT_OK) {
    nxcompat_copy_string(result->selected, sizeof(result->selected),
                         report.selected);
  } else {
    nxcompat_record_first(result, &report);
  }
  if (nxcompat_cleanup_once(options, result, cleanup_error,
                            sizeof(cleanup_error)) != NXCOMPAT_OK) {
    nxcompat_record_cleanup_failure(result, cleanup_error);
    return_code = NXCOMPAT_ROLLBACK_FAILED;
    goto restore_and_out;
  }

  if (outcome == NXCOMPAT_BACKEND_ATTEMPT_OK) {
    if (!nxcompat_environment_matches(saved)) {
      result->state = NXCOMPAT_BACKEND_V2_FAILED;
      result->final_reason = NXCOMPAT_REASON_PROVIDER_CONTRACT;
      nxcompat_copy_string(result->final_error, sizeof(result->final_error),
                           "backend provider changed a declared hint");
      return_code = NXCOMPAT_FAILED;
      goto restore_and_out;
    }
    result->state = inherited_count ? NXCOMPAT_BACKEND_V2_INHERITED_OK
                                    : NXCOMPAT_BACKEND_V2_AUTODETECT_OK;
    result->first_reason = inherited_count
                               ? NXCOMPAT_REASON_BACKEND_INHERITED_OK
                               : NXCOMPAT_REASON_BACKEND_AUTODETECT_OK;
    result->final_reason = result->first_reason;
    return_code = NXCOMPAT_OK;
    goto out;
  }

  if (outcome == NXCOMPAT_BACKEND_ATTEMPT_OWNERSHIP_BUSY) {
    result->state = NXCOMPAT_BACKEND_V2_BUSY;
    nxcompat_record_final(result, &report);
    return_code = NXCOMPAT_BUSY;
    goto restore_and_out;
  }
  if (outcome == NXCOMPAT_BACKEND_ATTEMPT_FATAL_FAILURE ||
      inherited_count == 0) {
    result->state = NXCOMPAT_BACKEND_V2_FAILED;
    nxcompat_record_final(result, &report);
    return_code = NXCOMPAT_FAILED;
    goto restore_and_out;
  }

  /* The first failure and its cleanup are now durably represented in result.
   * Only at this point may inherited hints be removed. */
  if (nxcompat_clear_environment(saved) != 0) {
    result->state = NXCOMPAT_BACKEND_V2_FAILED;
    result->final_reason = NXCOMPAT_REASON_ENV_CLEAR_FAILED;
    nxcompat_copy_string(result->final_error, sizeof(result->final_error),
                         "could not remove inherited backend hints");
    return_code = NXCOMPAT_FAILED;
    goto restore_and_out;
  }
  result->hints_removed = 1;
  result->env_restored = 0;

  outcome = nxcompat_attempt_once(options, result, &report);
  if (outcome == NXCOMPAT_BACKEND_ATTEMPT_OK)
    nxcompat_copy_string(result->selected, sizeof(result->selected),
                         report.selected);
  else
    nxcompat_record_final(result, &report);
  if (nxcompat_cleanup_once(options, result, cleanup_error,
                            sizeof(cleanup_error)) != NXCOMPAT_OK) {
    nxcompat_record_cleanup_failure(result, cleanup_error);
    return_code = NXCOMPAT_ROLLBACK_FAILED;
    goto restore_and_out;
  }

  if (outcome == NXCOMPAT_BACKEND_ATTEMPT_OK) {
    if (!nxcompat_environment_all_absent(saved)) {
      result->state = NXCOMPAT_BACKEND_V2_FAILED;
      result->final_reason = NXCOMPAT_REASON_PROVIDER_CONTRACT;
      nxcompat_copy_string(result->final_error, sizeof(result->final_error),
                           "backend provider changed an autodetect hint");
      return_code = NXCOMPAT_FAILED;
      goto restore_and_out;
    }
    result->state = NXCOMPAT_BACKEND_V2_RECOVERED_BY_AUTODETECT;
    result->final_reason = NXCOMPAT_REASON_BACKEND_AUTODETECT_OK;
    return_code = NXCOMPAT_OK;
    goto out;
  }

  result->state = outcome == NXCOMPAT_BACKEND_ATTEMPT_OWNERSHIP_BUSY
                      ? NXCOMPAT_BACKEND_V2_BUSY
                      : NXCOMPAT_BACKEND_V2_FAILED;
  result->final_reason = report.reason;
  return_code = outcome == NXCOMPAT_BACKEND_ATTEMPT_OWNERSHIP_BUSY
                    ? NXCOMPAT_BUSY
                    : NXCOMPAT_FAILED;

restore_and_out:
  if (snapshot_valid) {
    if (nxcompat_restore_environment(saved) != 0) {
      result->state = NXCOMPAT_BACKEND_V2_ROLLBACK_FAILED;
      result->final_reason = NXCOMPAT_REASON_ENV_RESTORE_FAILED;
      nxcompat_copy_string(result->final_error, sizeof(result->final_error),
                           "backend hint restoration could not be verified");
      result->env_restored = 0;
      return_code = NXCOMPAT_ROLLBACK_FAILED;
    } else {
      result->env_restored = 1;
      result->hints_removed = 0;
    }
  }

out:
  if (snapshot_valid)
    nxcompat_free_environment(saved);
  if (locked)
    nxcompat_global_arbiter_release();
  nxcompat_emit_backend_status_v2(options, result);
  return return_code;
}

static nxcompat_backend_attempt_outcome
nxcompat_legacy_attempt(void *userdata, nxcompat_backend_attempt_report *report) {
  nxcompat_legacy_backend *legacy = (nxcompat_legacy_backend *)userdata;
  const char *selected;
  int status;
  report->api_version = NXCOMPAT_API_VERSION_V2;
  report->struct_size = sizeof(*report);
  report->error[0] = '\0';
  status = legacy->options->attempt(legacy->options->userdata, report->error,
                                    sizeof(report->error));
  legacy->last_attempt_failed = status != 0;
  if (status != 0) {
    report->reason = NXCOMPAT_REASON_BACKEND_ATTEMPT_RETRYABLE;
    return NXCOMPAT_BACKEND_ATTEMPT_RETRYABLE_FAILURE;
  }
  selected = legacy->options->current_name(legacy->options->userdata);
  if (!selected || !*selected ||
      (legacy->options->accept_name &&
       !legacy->options->accept_name(legacy->options->userdata, selected))) {
    legacy->last_attempt_failed = 1;
    report->reason = NXCOMPAT_REASON_BACKEND_NAME_REJECTED;
    nxcompat_copy_string(report->error, sizeof(report->error),
                         "legacy backend output name was rejected");
    return NXCOMPAT_BACKEND_ATTEMPT_RETRYABLE_FAILURE;
  }
  nxcompat_copy_string(report->selected, sizeof(report->selected), selected);
  return NXCOMPAT_BACKEND_ATTEMPT_OK;
}

static nxcompat_result_code nxcompat_legacy_cleanup(void *userdata, char *error,
                                                    size_t error_size) {
  nxcompat_legacy_backend *legacy = (nxcompat_legacy_backend *)userdata;
  if (error_size)
    error[0] = '\0';
  if (legacy->last_attempt_failed)
    legacy->options->reset(legacy->options->userdata);
  return NXCOMPAT_OK;
}

static int nxcompat_legacy_accept(void *userdata, const char *name) {
  (void)userdata;
  return name && *name;
}

int nxcompat_negotiate_backend(const nxcompat_backend_options *options,
                               nxcompat_backend_result *result) {
  nxcompat_backend_options_v2 options_v2;
  nxcompat_backend_result_v2 result_v2;
  nxcompat_legacy_backend legacy;
  nxcompat_result_code status;
  if (!options || !result ||
      !nxcompat_api_version_supported(options->api_version) ||
      options->struct_size < sizeof(*options) ||
      !nxcompat_backend_kind_valid(options->kind) || !options->attempt ||
      !options->reset || !options->current_name ||
      !nxcompat_environment_names_valid(options->environment_names))
    return -1;

  memset(&legacy, 0, sizeof(legacy));
  legacy.options = options;
  memset(&options_v2, 0, sizeof(options_v2));
  options_v2.api_version = NXCOMPAT_API_VERSION_V2;
  options_v2.struct_size = sizeof(options_v2);
  options_v2.kind = options->kind;
  memcpy(options_v2.environment_names, options->environment_names,
         sizeof(options_v2.environment_names));
  options_v2.attempt = nxcompat_legacy_attempt;
  options_v2.cleanup = nxcompat_legacy_cleanup;
  options_v2.accept_name = nxcompat_legacy_accept;
  options_v2.status = options->status;
  options_v2.userdata = &legacy;
  options_v2.status_userdata = options->status_userdata;
  status = nxcompat_negotiate_backend_v2(&options_v2, &result_v2);

  memset(result, 0, sizeof(*result));
  result->api_version = options->api_version;
  result->struct_size = sizeof(*result);
  result->kind = options->kind;
  result->attempt_count = result_v2.attempt_count;
  result->inherited_count = result_v2.inherited_count;
  nxcompat_copy_string(result->selected, sizeof(result->selected),
                       result_v2.selected);
  nxcompat_copy_string(result->first_error, sizeof(result->first_error),
                       result_v2.first_error);
  nxcompat_copy_string(result->final_error, sizeof(result->final_error),
                       result_v2.final_error);
  switch (result_v2.state) {
  case NXCOMPAT_BACKEND_V2_INHERITED_OK:
    result->state = NXCOMPAT_BACKEND_INHERITED_OK;
    break;
  case NXCOMPAT_BACKEND_V2_AUTODETECT_OK:
    result->state = NXCOMPAT_BACKEND_AUTODETECT_OK;
    break;
  case NXCOMPAT_BACKEND_V2_RECOVERED_BY_AUTODETECT:
    result->state = NXCOMPAT_BACKEND_RECOVERED_BY_AUTODETECT;
    break;
  default:
    result->state = NXCOMPAT_BACKEND_FAILED;
    break;
  }
  return status == NXCOMPAT_OK ? 0 : -1;
}
