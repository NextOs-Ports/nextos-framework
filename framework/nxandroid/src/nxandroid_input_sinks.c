/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxandroid_input_sinks.h"

#include <stdio.h>
#include <string.h>

const char *nxandroid_input_result_string(nxandroid_input_result result) {
  switch (result) {
  case NXANDROID_INPUT_OK:
    return "ok";
  case NXANDROID_INPUT_EINVAL:
    return "invalid input-sink argument";
  case NXANDROID_INPUT_EFULL:
    return "input-sink registry full";
  case NXANDROID_INPUT_EDUPLICATE:
    return "duplicate input sink for action";
  default:
    return "unknown input-sink result";
  }
}

static int nxandroid_input_sink_kind_valid(nxandroid_input_sink_kind kind) {
  switch (kind) {
  case NXANDROID_SINK_ANDROID_KEY:
  case NXANDROID_SINK_ANDROID_MOTION:
  case NXANDROID_SINK_JNI_CALLBACK:
  case NXANDROID_SINK_INTERNAL_API:
  case NXANDROID_SINK_TOUCH:
    return 1;
  default:
    return 0;
  }
}

static void nxandroid_copy_bounded(char *destination, size_t capacity,
                                   const char *source) {
  size_t length = strlen(source);

  if (length >= capacity)
    length = capacity - 1u;
  memcpy(destination, source, length);
  destination[length] = '\0';
}

void nxandroid_input_sinks_init(nxandroid_input_sink_registry *registry) {
  if (registry == NULL)
    return;
  memset(registry, 0, sizeof(*registry));
}

nxandroid_input_result nxandroid_input_sinks_register(
    nxandroid_input_sink_registry *registry, const char *action,
    nxandroid_input_sink_kind kind, nxandroid_input_sink_fn callback,
    void *userdata, const char *description) {
  nxandroid_input_sink_entry *entry;
  size_t index;

  if (registry == NULL || action == NULL || action[0] == '\0' ||
      callback == NULL || !nxandroid_input_sink_kind_valid(kind))
    return NXANDROID_INPUT_EINVAL;
  if (strlen(action) >= NXANDROID_INPUT_SINK_ACTION_MAX)
    return NXANDROID_INPUT_EINVAL;
  if (registry->entry_count > NXANDROID_INPUT_SINK_MAX_ENTRIES)
    return NXANDROID_INPUT_EINVAL;
  for (index = 0u; index < registry->entry_count; ++index) {
    entry = &registry->entries[index];
    if (entry->kind == kind && entry->callback == callback &&
        strcmp(entry->action, action) == 0)
      return NXANDROID_INPUT_EDUPLICATE;
  }
  if (registry->entry_count == NXANDROID_INPUT_SINK_MAX_ENTRIES)
    return NXANDROID_INPUT_EFULL;

  entry = &registry->entries[registry->entry_count];
  memset(entry, 0, sizeof(*entry));
  nxandroid_copy_bounded(entry->action, sizeof(entry->action), action);
  entry->kind = kind;
  entry->callback = callback;
  entry->userdata = userdata;
  if (description != NULL)
    nxandroid_copy_bounded(entry->description, sizeof(entry->description),
                           description);
  registry->entry_count += 1u;
  return NXANDROID_INPUT_OK;
}

int nxandroid_input_sinks_deliver(nxandroid_input_sink_registry *registry,
                                  const char *action, int pressed,
                                  float value) {
  size_t index;
  size_t count;
  int invoked = 0;

  if (registry == NULL || action == NULL || action[0] == '\0')
    return NXANDROID_INPUT_EINVAL;
  count = registry->entry_count;
  if (count > NXANDROID_INPUT_SINK_MAX_ENTRIES)
    return NXANDROID_INPUT_EINVAL;
  for (index = 0u; index < count; ++index) {
    nxandroid_input_sink_entry *entry = &registry->entries[index];

    if (entry->callback == NULL || strcmp(entry->action, action) != 0)
      continue;
    entry->callback(entry->userdata, entry->action, pressed != 0, value);
    invoked += 1;
  }
  return invoked;
}

nxandroid_input_result nxandroid_input_sinks_set_exclusive(
    nxandroid_input_sink_registry *registry, int exclusive) {
  if (registry == NULL)
    return NXANDROID_INPUT_EINVAL;
  registry->exclusive = exclusive != 0;
  return NXANDROID_INPUT_OK;
}

int nxandroid_input_sinks_exclusive(
    const nxandroid_input_sink_registry *registry) {
  if (registry == NULL)
    return 0;
  return registry->exclusive != 0;
}

static nxandroid_input_result nxandroid_touch_fail(char *error,
                                                   size_t error_size,
                                                   const char *message) {
  if (error != NULL && error_size > 0u)
    snprintf(error, error_size, "%s", message);
  return NXANDROID_INPUT_EINVAL;
}

static float nxandroid_clamp_unit(float value) {
  if (!(value > 0.0f))
    return 0.0f;
  if (value > 1.0f)
    return 1.0f;
  return value;
}

nxandroid_input_result nxandroid_touch_resolve(
    const nxandroid_touch_geometry *geometry, float norm_x, float norm_y,
    int *out_px, int *out_py, char *error, size_t error_size) {
  float rotated_x;
  float rotated_y;
  int usable_width;
  int usable_height;
  int pixel_x;
  int pixel_y;

  if (geometry == NULL || out_px == NULL || out_py == NULL)
    return nxandroid_touch_fail(error, error_size,
                                "touch resolve: NULL geometry or output");
  if (geometry->width <= 0 || geometry->height <= 0)
    return nxandroid_touch_fail(error, error_size,
                                "touch resolve: non-positive drawable size");
  if (geometry->safe_left < 0 || geometry->safe_top < 0 ||
      geometry->safe_right < 0 || geometry->safe_bottom < 0)
    return nxandroid_touch_fail(error, error_size,
                                "touch resolve: negative safe-area inset");
  if (geometry->safe_left + geometry->safe_right >= geometry->width ||
      geometry->safe_top + geometry->safe_bottom >= geometry->height)
    return nxandroid_touch_fail(
        error, error_size,
        "touch resolve: safe area consumes the whole drawable");

  norm_x = nxandroid_clamp_unit(norm_x);
  norm_y = nxandroid_clamp_unit(norm_y);
  switch (geometry->rotation_degrees) {
  case 0:
    rotated_x = norm_x;
    rotated_y = norm_y;
    break;
  case 90:
    rotated_x = 1.0f - norm_y;
    rotated_y = norm_x;
    break;
  case 180:
    rotated_x = 1.0f - norm_x;
    rotated_y = 1.0f - norm_y;
    break;
  case 270:
    rotated_x = norm_y;
    rotated_y = 1.0f - norm_x;
    break;
  default:
    return nxandroid_touch_fail(
        error, error_size,
        "touch resolve: rotation must be 0, 90, 180 or 270");
  }

  usable_width = geometry->width - geometry->safe_left - geometry->safe_right;
  usable_height = geometry->height - geometry->safe_top - geometry->safe_bottom;
  pixel_x = geometry->safe_left + (int)(rotated_x * (float)usable_width);
  pixel_y = geometry->safe_top + (int)(rotated_y * (float)usable_height);
  if (pixel_x > geometry->width - geometry->safe_right - 1)
    pixel_x = geometry->width - geometry->safe_right - 1;
  if (pixel_y > geometry->height - geometry->safe_bottom - 1)
    pixel_y = geometry->height - geometry->safe_bottom - 1;
  if (pixel_x < geometry->safe_left)
    pixel_x = geometry->safe_left;
  if (pixel_y < geometry->safe_top)
    pixel_y = geometry->safe_top;

  *out_px = pixel_x;
  *out_py = pixel_y;
  return NXANDROID_INPUT_OK;
}
