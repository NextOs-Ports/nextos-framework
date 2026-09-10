/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxandroid_jni_adapter.h"

#include <stdlib.h>
#include <string.h>

typedef char nxandroid_float_must_be_32_bits
    [(sizeof(float) == sizeof(uint32_t)) ? 1 : -1];

struct nxandroid_prefs_snapshot {
  nxandroid_prefs_entry_view *entries;
  size_t entry_count;
};

const char *nxandroid_jni_adapter_result_string(
    nxandroid_jni_adapter_result result) {
  switch (result) {
  case NXANDROID_JNI_ADAPTER_OK:
    return "ok";
  case NXANDROID_JNI_ADAPTER_EINVAL:
    return "invalid adapter input";
  case NXANDROID_JNI_ADAPTER_ENOMEM:
    return "adapter allocation failed";
  case NXANDROID_JNI_ADAPTER_EBOUNDS:
    return "UTF-16 region out of bounds";
  case NXANDROID_JNI_ADAPTER_EDUPLICATE:
    return "duplicate preferences key or set member";
  default:
    return "unknown adapter result";
  }
}

static int nxandroid_utf16_view_valid(const nxandroid_utf16_view *view) {
  return view != NULL &&
         (view->unit_count == 0u || view->units != NULL) &&
         view->unit_count <= SIZE_MAX / sizeof(uint16_t);
}

static int nxandroid_utf16_equal(const nxandroid_utf16_view *left,
                                 const nxandroid_utf16_view *right) {
  if (left->unit_count != right->unit_count)
    return 0;
  if (left->unit_count == 0u)
    return 1;
  return memcmp(left->units, right->units,
                left->unit_count * sizeof(*left->units)) == 0;
}

static nxandroid_jni_adapter_result nxandroid_utf16_clone(
    const nxandroid_utf16_view *source, nxandroid_utf16_view *output) {
  uint16_t *units;

  if (!nxandroid_utf16_view_valid(source) || output == NULL)
    return NXANDROID_JNI_ADAPTER_EINVAL;
  output->units = NULL;
  output->unit_count = 0u;
  if (source->unit_count == 0u)
    return NXANDROID_JNI_ADAPTER_OK;
  units = (uint16_t *)malloc(source->unit_count * sizeof(*units));
  if (units == NULL)
    return NXANDROID_JNI_ADAPTER_ENOMEM;
  memcpy(units, source->units, source->unit_count * sizeof(*units));
  output->units = units;
  output->unit_count = source->unit_count;
  return NXANDROID_JNI_ADAPTER_OK;
}

nxandroid_jni_adapter_result nxandroid_utf16_copy_region(
    const nxandroid_utf16_view *source, int32_t start, int32_t length,
    uint16_t *output) {
  size_t start_index;
  size_t unit_length;

  if (!nxandroid_utf16_view_valid(source))
    return NXANDROID_JNI_ADAPTER_EINVAL;
  if (start < 0 || length < 0)
    return NXANDROID_JNI_ADAPTER_EBOUNDS;
  start_index = (size_t)start;
  unit_length = (size_t)length;
  if (start_index > source->unit_count ||
      unit_length > source->unit_count - start_index)
    return NXANDROID_JNI_ADAPTER_EBOUNDS;
  if (unit_length == 0u)
    return NXANDROID_JNI_ADAPTER_OK;
  if (output == NULL)
    return NXANDROID_JNI_ADAPTER_EINVAL;
  memmove(output, source->units + start_index,
          unit_length * sizeof(*output));
  return NXANDROID_JNI_ADAPTER_OK;
}

static int nxandroid_prefs_type_valid(nxandroid_prefs_value_type type) {
  return type >= NXANDROID_PREFS_STRING && type <= NXANDROID_PREFS_BOOL;
}

static int nxandroid_string_set_view_valid(
    const nxandroid_prefs_string_set_view *set) {
  size_t index;

  if (set == NULL || set->member_count > NXANDROID_PREFS_MAX_STRING_SET_MEMBERS ||
      (set->member_count != 0u && set->members == NULL))
    return 0;
  for (index = 0u; index < set->member_count; ++index) {
    if (!nxandroid_utf16_view_valid(&set->members[index]))
      return 0;
  }
  return 1;
}

static int nxandroid_string_set_has_duplicates(
    const nxandroid_prefs_string_set_view *set) {
  size_t left;
  size_t right;

  for (left = 0u; left < set->member_count; ++left) {
    for (right = left + 1u; right < set->member_count; ++right) {
      if (nxandroid_utf16_equal(&set->members[left], &set->members[right]))
        return 1;
    }
  }
  return 0;
}

static void nxandroid_utf16_free(nxandroid_utf16_view *view) {
  if (view == NULL)
    return;
  free((void *)view->units);
  view->units = NULL;
  view->unit_count = 0u;
}

static void nxandroid_string_set_free(
    nxandroid_prefs_string_set_view *set) {
  nxandroid_utf16_view *members;
  size_t index;

  if (set == NULL)
    return;
  members = (nxandroid_utf16_view *)set->members;
  for (index = 0u; index < set->member_count; ++index)
    nxandroid_utf16_free(&members[index]);
  free(members);
  set->members = NULL;
  set->member_count = 0u;
}

static void nxandroid_prefs_entry_free(nxandroid_prefs_entry_view *entry) {
  if (entry == NULL)
    return;
  nxandroid_utf16_free(&entry->key);
  if (entry->type == NXANDROID_PREFS_STRING)
    nxandroid_utf16_free(&entry->value.string_value);
  else if (entry->type == NXANDROID_PREFS_STRING_SET)
    nxandroid_string_set_free(&entry->value.string_set_value);
  memset(entry, 0, sizeof(*entry));
}

static nxandroid_jni_adapter_result nxandroid_string_set_clone(
    const nxandroid_prefs_string_set_view *source,
    nxandroid_prefs_string_set_view *output) {
  nxandroid_utf16_view *members;
  size_t index;
  nxandroid_jni_adapter_result result;

  output->members = NULL;
  output->member_count = 0u;
  if (!nxandroid_string_set_view_valid(source))
    return NXANDROID_JNI_ADAPTER_EINVAL;
  if (nxandroid_string_set_has_duplicates(source))
    return NXANDROID_JNI_ADAPTER_EDUPLICATE;
  if (source->member_count == 0u)
    return NXANDROID_JNI_ADAPTER_OK;
  if (source->member_count > SIZE_MAX / sizeof(*members))
    return NXANDROID_JNI_ADAPTER_EINVAL;
  members = (nxandroid_utf16_view *)calloc(source->member_count,
                                           sizeof(*members));
  if (members == NULL)
    return NXANDROID_JNI_ADAPTER_ENOMEM;
  for (index = 0u; index < source->member_count; ++index) {
    result = nxandroid_utf16_clone(&source->members[index], &members[index]);
    if (result != NXANDROID_JNI_ADAPTER_OK) {
      nxandroid_prefs_string_set_view partial;
      partial.members = members;
      partial.member_count = source->member_count;
      nxandroid_string_set_free(&partial);
      return result;
    }
  }
  output->members = members;
  output->member_count = source->member_count;
  return NXANDROID_JNI_ADAPTER_OK;
}

static nxandroid_jni_adapter_result nxandroid_prefs_entry_clone(
    const nxandroid_prefs_entry_view *source,
    nxandroid_prefs_entry_view *output) {
  nxandroid_jni_adapter_result result;

  memset(output, 0, sizeof(*output));
  if (source == NULL || !nxandroid_utf16_view_valid(&source->key) ||
      !nxandroid_prefs_type_valid(source->type))
    return NXANDROID_JNI_ADAPTER_EINVAL;
  result = nxandroid_utf16_clone(&source->key, &output->key);
  if (result != NXANDROID_JNI_ADAPTER_OK)
    return result;
  output->type = source->type;
  switch (source->type) {
  case NXANDROID_PREFS_STRING:
    result = nxandroid_utf16_clone(&source->value.string_value,
                                   &output->value.string_value);
    break;
  case NXANDROID_PREFS_STRING_SET:
    result = nxandroid_string_set_clone(&source->value.string_set_value,
                                        &output->value.string_set_value);
    break;
  case NXANDROID_PREFS_INT32:
    output->value.int32_value = source->value.int32_value;
    result = NXANDROID_JNI_ADAPTER_OK;
    break;
  case NXANDROID_PREFS_INT64:
    output->value.int64_value = source->value.int64_value;
    result = NXANDROID_JNI_ADAPTER_OK;
    break;
  case NXANDROID_PREFS_FLOAT:
    memcpy(&output->value.float_value, &source->value.float_value,
           sizeof(output->value.float_value));
    result = NXANDROID_JNI_ADAPTER_OK;
    break;
  case NXANDROID_PREFS_BOOL:
    if (source->value.bool_value != 0 && source->value.bool_value != 1)
      result = NXANDROID_JNI_ADAPTER_EINVAL;
    else {
      output->value.bool_value = source->value.bool_value;
      result = NXANDROID_JNI_ADAPTER_OK;
    }
    break;
  default:
    result = NXANDROID_JNI_ADAPTER_EINVAL;
    break;
  }
  if (result != NXANDROID_JNI_ADAPTER_OK)
    nxandroid_prefs_entry_free(output);
  return result;
}

static int nxandroid_prefs_has_duplicate_key(
    const nxandroid_prefs_entry_view *entries, size_t entry_count,
    const nxandroid_utf16_view *key) {
  size_t index;

  for (index = 0u; index < entry_count; ++index) {
    if (nxandroid_utf16_equal(&entries[index].key, key))
      return 1;
  }
  return 0;
}

nxandroid_jni_adapter_result nxandroid_prefs_snapshot_create(
    const nxandroid_prefs_entry_view *entries, size_t entry_count,
    nxandroid_prefs_snapshot **output) {
  nxandroid_prefs_snapshot *snapshot;
  size_t index;
  nxandroid_jni_adapter_result result;

  if (output == NULL)
    return NXANDROID_JNI_ADAPTER_EINVAL;
  *output = NULL;
  if (entry_count > NXANDROID_PREFS_MAX_ENTRIES ||
      (entry_count != 0u && entries == NULL) ||
      entry_count > SIZE_MAX / sizeof(*snapshot->entries))
    return NXANDROID_JNI_ADAPTER_EINVAL;
  snapshot = (nxandroid_prefs_snapshot *)calloc(1u, sizeof(*snapshot));
  if (snapshot == NULL)
    return NXANDROID_JNI_ADAPTER_ENOMEM;
  if (entry_count != 0u) {
    snapshot->entries = (nxandroid_prefs_entry_view *)calloc(
        entry_count, sizeof(*snapshot->entries));
    if (snapshot->entries == NULL) {
      free(snapshot);
      return NXANDROID_JNI_ADAPTER_ENOMEM;
    }
  }
  for (index = 0u; index < entry_count; ++index) {
    if (!nxandroid_utf16_view_valid(&entries[index].key)) {
      result = NXANDROID_JNI_ADAPTER_EINVAL;
      goto fail;
    }
    if (nxandroid_prefs_has_duplicate_key(snapshot->entries, index,
                                           &entries[index].key)) {
      result = NXANDROID_JNI_ADAPTER_EDUPLICATE;
      goto fail;
    }
    result = nxandroid_prefs_entry_clone(&entries[index],
                                         &snapshot->entries[index]);
    if (result != NXANDROID_JNI_ADAPTER_OK)
      goto fail;
    snapshot->entry_count = index + 1u;
  }
  *output = snapshot;
  return NXANDROID_JNI_ADAPTER_OK;

fail:
  nxandroid_prefs_snapshot_destroy(&snapshot);
  return result;
}

void nxandroid_prefs_snapshot_destroy(nxandroid_prefs_snapshot **snapshot) {
  size_t index;

  if (snapshot == NULL || *snapshot == NULL)
    return;
  for (index = 0u; index < (*snapshot)->entry_count; ++index)
    nxandroid_prefs_entry_free(&(*snapshot)->entries[index]);
  free((*snapshot)->entries);
  free(*snapshot);
  *snapshot = NULL;
}

size_t nxandroid_prefs_snapshot_count(
    const nxandroid_prefs_snapshot *snapshot) {
  return snapshot != NULL ? snapshot->entry_count : 0u;
}

const nxandroid_prefs_entry_view *nxandroid_prefs_snapshot_entry_at(
    const nxandroid_prefs_snapshot *snapshot, size_t index) {
  if (snapshot == NULL || index >= snapshot->entry_count)
    return NULL;
  return &snapshot->entries[index];
}
