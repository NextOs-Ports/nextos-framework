/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "nxgl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(NXGL_SDL_HINT_TESTING)
int nxgl_test_sdl_get_num_video_drivers(void);
const char *nxgl_test_sdl_get_video_driver(int index);
Uint32 nxgl_test_sdl_was_init(Uint32 flags);
const char *nxgl_test_sdl_current_video_driver(void);
#define SDL_GetNumVideoDrivers nxgl_test_sdl_get_num_video_drivers
#define SDL_GetVideoDriver nxgl_test_sdl_get_video_driver
#define SDL_WasInit nxgl_test_sdl_was_init
#define SDL_GetCurrentVideoDriver nxgl_test_sdl_current_video_driver
#endif

static int nxgl_sdl_hint_boolean_valid(int value) {
  return value == 0 || value == 1;
}

static int nxgl_sdl_hint_ascii_space(char value) {
  return value == ' ' || value == '\t';
}

static int nxgl_sdl_hint_driver_character(char value) {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || value == '_' || value == '-' ||
         value == '.';
}

static int nxgl_sdl_hint_ascii_lower(char value) {
  if (value >= 'A' && value <= 'Z')
    return value + ('a' - 'A');
  return value;
}

static int nxgl_sdl_hint_driver_name_valid(const char *name) {
  size_t index;
  size_t length;
  if (!name)
    return 0;
  length = strnlen(name, NXGL_NAME_MAX);
  if (length == 0u || length >= NXGL_NAME_MAX)
    return 0;
  for (index = 0u; index < length; ++index)
    if (!nxgl_sdl_hint_driver_character(name[index]))
      return 0;
  return 1;
}

static int nxgl_sdl_hint_value_valid(const char *hint) {
  const char *cursor = hint;
  int token_has_character = 0;
  int token_count = 0;
  if (!hint)
    return 0;
  while (*cursor) {
    if (*cursor == ',') {
      if (!token_has_character)
        return 0;
      ++token_count;
      token_has_character = 0;
    } else if (!nxgl_sdl_hint_ascii_space(*cursor)) {
      if (!nxgl_sdl_hint_driver_character(*cursor))
        return 0;
      token_has_character = 1;
    }
    ++cursor;
  }
  if (!token_has_character)
    return 0;
  ++token_count;
  return token_count <= (int)NXGL_SDL_VIDEO_DRIVER_MAX;
}

static int nxgl_sdl_hint_token_equal(const char *begin, const char *end,
                                     const char *driver) {
  size_t token_length;
  size_t driver_length;
  while (begin < end && nxgl_sdl_hint_ascii_space(*begin))
    ++begin;
  while (end > begin && nxgl_sdl_hint_ascii_space(end[-1]))
    --end;
  token_length = (size_t)(end - begin);
  driver_length = strlen(driver);
  if (token_length != driver_length)
    return 0;
  while (begin < end) {
    if (nxgl_sdl_hint_ascii_lower(*begin) !=
        nxgl_sdl_hint_ascii_lower(*driver))
      return 0;
    ++begin;
    ++driver;
  }
  return 1;
}

static int nxgl_sdl_hint_contains_driver(const char *hint,
                                         const char *driver) {
  const char *begin = hint;
  const char *cursor = hint;
  if (!hint || !driver)
    return 0;
  for (;;) {
    if (*cursor == ',' || *cursor == '\0') {
      if (nxgl_sdl_hint_token_equal(begin, cursor, driver))
        return 1;
      if (*cursor == '\0')
        return 0;
      begin = cursor + 1;
    }
    ++cursor;
  }
}

static void nxgl_sdl_hint_copy(char *destination, size_t destination_size,
                               const char *source) {
  if (!destination || destination_size == 0u)
    return;
  if (!source)
    source = "";
  (void)snprintf(destination, destination_size, "%s", source);
}

static int nxgl_sdl_hint_receipt_strings_valid(
    const nxgl_sdl_video_hint_receipt_v2 *receipt) {
  return memchr(receipt->inherited_hint, '\0',
                sizeof(receipt->inherited_hint)) != NULL &&
         memchr(receipt->compiled_video_drivers, '\0',
                sizeof(receipt->compiled_video_drivers)) != NULL &&
         memchr(receipt->selected_video_driver, '\0',
                sizeof(receipt->selected_video_driver)) != NULL;
}

static int nxgl_sdl_hint_receipt_valid(
    const nxgl_sdl_video_hint_receipt_v2 *receipt) {
  return receipt && receipt->api_version == NXGL_API_VERSION_V2 &&
         receipt->struct_size >= sizeof(*receipt) &&
         receipt->action >= NXGL_SDL_VIDEO_HINT_V2_DISABLED &&
         receipt->action <= NXGL_SDL_VIDEO_HINT_V2_ENVIRONMENT_FAILED &&
         nxgl_sdl_hint_boolean_valid(receipt->inherited_hint_present) &&
         nxgl_sdl_hint_boolean_valid(receipt->inherited_hint_supported) &&
         nxgl_sdl_hint_boolean_valid(receipt->hint_removed) &&
         nxgl_sdl_hint_boolean_valid(receipt->selected_recorded) &&
         receipt->video_driver_count >= 0 &&
         receipt->video_driver_count <= (int)NXGL_SDL_VIDEO_DRIVER_MAX &&
         nxgl_sdl_hint_receipt_strings_valid(receipt);
}

static int nxgl_sdl_hint_build_driver_list(
    nxgl_sdl_video_hint_receipt_v2 *receipt) {
  int count;
  int index;
  size_t used = 0u;
  count = SDL_GetNumVideoDrivers();
  if (count <= 0 || count > (int)NXGL_SDL_VIDEO_DRIVER_MAX)
    return -1;
  for (index = 0; index < count; ++index) {
    const char *driver = SDL_GetVideoDriver(index);
    size_t length;
    if (!nxgl_sdl_hint_driver_name_valid(driver))
      return -1;
    length = strlen(driver);
    if (used + (used ? 1u : 0u) + length + 1u >
        sizeof(receipt->compiled_video_drivers))
      return -1;
    if (used)
      receipt->compiled_video_drivers[used++] = ',';
    memcpy(receipt->compiled_video_drivers + used, driver, length);
    used += length;
    receipt->compiled_video_drivers[used] = '\0';
  }
  receipt->video_driver_count = count;
  return 0;
}

void nxgl_sdl_video_hint_options_v2_init(
    nxgl_sdl_video_hint_options_v2 *options) {
  if (!options)
    return;
  memset(options, 0, sizeof(*options));
  options->api_version = NXGL_API_VERSION_V2;
  options->struct_size = sizeof(*options);
}

void nxgl_sdl_video_hint_receipt_v2_init(
    nxgl_sdl_video_hint_receipt_v2 *receipt) {
  if (!receipt)
    return;
  memset(receipt, 0, sizeof(*receipt));
  receipt->api_version = NXGL_API_VERSION_V2;
  receipt->struct_size = sizeof(*receipt);
  receipt->action = NXGL_SDL_VIDEO_HINT_V2_DISABLED;
}

int nxgl_sanitize_sdl_video_hint_v2(
    const nxgl_sdl_video_hint_options_v2 *options,
    nxgl_sdl_video_hint_receipt_v2 *receipt) {
  nxgl_sdl_video_hint_receipt_v2 local;
  const char *inherited;
  size_t inherited_length;
  int index;
  int supported = 0;
  int result = NXGL_SUCCESS;

  if (!options || options->api_version != NXGL_API_VERSION_V2 ||
      options->struct_size < sizeof(*options) ||
      !nxgl_sdl_hint_boolean_valid(options->enabled) || !receipt)
    return NXGL_ERROR_INVALID_ARGUMENT;
  if (!nxgl_arbiter_try_acquire())
    return NXGL_ERROR_BUSY;

  nxgl_sdl_video_hint_receipt_v2_init(&local);
  if (!options->enabled) {
    *receipt = local;
    nxgl_arbiter_release();
    return NXGL_NO_ACTION;
  }

  inherited = getenv("SDL_VIDEODRIVER");
  if (inherited && inherited[0]) {
    local.inherited_hint_present = 1;
    inherited_length = strnlen(inherited, NXGL_SDL_VIDEO_HINT_MAX);
    if (inherited_length >= NXGL_SDL_VIDEO_HINT_MAX ||
        !nxgl_sdl_hint_value_valid(inherited)) {
      local.action = NXGL_SDL_VIDEO_HINT_V2_INVALID_HINT;
      nxgl_sdl_hint_copy(local.inherited_hint,
                         sizeof(local.inherited_hint), "<invalid>");
      *receipt = local;
      nxgl_arbiter_release();
      return NXGL_ERROR_INVALID_ARGUMENT;
    }
    nxgl_sdl_hint_copy(local.inherited_hint, sizeof(local.inherited_hint),
                       inherited);
  }
  if (SDL_WasInit(SDL_INIT_VIDEO) != 0u) {
    local.action = NXGL_SDL_VIDEO_HINT_V2_REFUSED_VIDEO_ACTIVE;
    *receipt = local;
    nxgl_arbiter_release();
    return NXGL_ERROR_INVALID_STATE;
  }
  if (nxgl_sdl_hint_build_driver_list(&local) != 0) {
    local.action = NXGL_SDL_VIDEO_HINT_V2_DRIVER_QUERY_FAILED;
    *receipt = local;
    nxgl_arbiter_release();
    return NXGL_ERROR_VIDEO_UNAVAILABLE;
  }

  if (!local.inherited_hint_present) {
    local.action = NXGL_SDL_VIDEO_HINT_V2_NO_HINT;
    *receipt = local;
    nxgl_arbiter_release();
    return NXGL_SUCCESS;
  }
  for (index = 0; index < local.video_driver_count; ++index) {
    const char *driver = SDL_GetVideoDriver(index);
    if (driver &&
        nxgl_sdl_hint_contains_driver(local.inherited_hint, driver)) {
      supported = 1;
      break;
    }
  }
  if (supported) {
    local.inherited_hint_supported = 1;
    local.action = NXGL_SDL_VIDEO_HINT_V2_PRESERVED_SUPPORTED;
  } else if (unsetenv("SDL_VIDEODRIVER") == 0 &&
             getenv("SDL_VIDEODRIVER") == NULL) {
    local.hint_removed = 1;
    local.action = NXGL_SDL_VIDEO_HINT_V2_CLEARED_UNSUPPORTED;
  } else {
    if (getenv("SDL_VIDEODRIVER") == NULL)
      (void)setenv("SDL_VIDEODRIVER", local.inherited_hint, 1);
    local.action = NXGL_SDL_VIDEO_HINT_V2_ENVIRONMENT_FAILED;
    result = NXGL_ERROR_ROLLBACK;
  }

  *receipt = local;
  nxgl_arbiter_release();
  return result;
}

int nxgl_complete_sdl_video_hint_receipt_v2(
    nxgl_sdl_video_hint_receipt_v2 *receipt) {
  nxgl_sdl_video_hint_receipt_v2 local;
  const char *selected;
  if (!nxgl_sdl_hint_receipt_valid(receipt))
    return NXGL_ERROR_INVALID_ARGUMENT;
  if (receipt->action == NXGL_SDL_VIDEO_HINT_V2_DISABLED)
    return NXGL_NO_ACTION;
  if (receipt->action != NXGL_SDL_VIDEO_HINT_V2_NO_HINT &&
      receipt->action != NXGL_SDL_VIDEO_HINT_V2_PRESERVED_SUPPORTED &&
      receipt->action != NXGL_SDL_VIDEO_HINT_V2_CLEARED_UNSUPPORTED)
    return NXGL_ERROR_INVALID_STATE;
  if (!nxgl_arbiter_try_acquire())
    return NXGL_ERROR_BUSY;
  if (SDL_WasInit(SDL_INIT_VIDEO) == 0u) {
    nxgl_arbiter_release();
    return NXGL_ERROR_INVALID_STATE;
  }
  selected = SDL_GetCurrentVideoDriver();
  if (!nxgl_sdl_hint_driver_name_valid(selected) ||
      !nxgl_sdl_hint_contains_driver(receipt->compiled_video_drivers,
                                     selected)) {
    nxgl_arbiter_release();
    return NXGL_ERROR_STACK_MISMATCH;
  }
  local = *receipt;
  nxgl_sdl_hint_copy(local.selected_video_driver,
                     sizeof(local.selected_video_driver), selected);
  local.selected_recorded = 1;
  *receipt = local;
  nxgl_arbiter_release();
  return NXGL_SUCCESS;
}

const char *nxgl_sdl_video_hint_action_name_v2(
    nxgl_sdl_video_hint_action_v2 action) {
  switch (action) {
  case NXGL_SDL_VIDEO_HINT_V2_DISABLED:
    return "disabled";
  case NXGL_SDL_VIDEO_HINT_V2_NO_HINT:
    return "no-hint";
  case NXGL_SDL_VIDEO_HINT_V2_PRESERVED_SUPPORTED:
    return "preserved-supported";
  case NXGL_SDL_VIDEO_HINT_V2_CLEARED_UNSUPPORTED:
    return "cleared-unsupported";
  case NXGL_SDL_VIDEO_HINT_V2_REFUSED_VIDEO_ACTIVE:
    return "refused-video-active";
  case NXGL_SDL_VIDEO_HINT_V2_DRIVER_QUERY_FAILED:
    return "driver-query-failed";
  case NXGL_SDL_VIDEO_HINT_V2_INVALID_HINT:
    return "invalid-hint";
  case NXGL_SDL_VIDEO_HINT_V2_ENVIRONMENT_FAILED:
    return "environment-failed";
  default:
    return "unknown";
  }
}

int nxgl_format_sdl_video_hint_receipt_v2(
    const nxgl_sdl_video_hint_receipt_v2 *receipt, char *output,
    size_t output_size) {
  const char *inherited;
  const char *available;
  const char *selected;
  int written;
  if (!nxgl_sdl_hint_receipt_valid(receipt) || !output || output_size == 0u)
    return NXGL_ERROR_INVALID_ARGUMENT;
  inherited = receipt->inherited_hint_present ? receipt->inherited_hint
                                               : "<none>";
  available = receipt->video_driver_count > 0
                  ? receipt->compiled_video_drivers
                  : "<unavailable>";
  selected = receipt->selected_recorded ? receipt->selected_video_driver
                                        : "<pending>";
  written = snprintf(
      output, output_size,
      "SDL VIDEO HINT RECEIPT: inherited=%s available=[%s] action=%s "
      "selected=%s",
      inherited, available,
      nxgl_sdl_video_hint_action_name_v2(receipt->action), selected);
  if (written < 0 || (size_t)written >= output_size)
    return NXGL_ERROR_INVALID_ARGUMENT;
  return NXGL_SUCCESS;
}
