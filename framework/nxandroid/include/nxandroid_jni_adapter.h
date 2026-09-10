/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXANDROID_JNI_ADAPTER_H
#define NXANDROID_JNI_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This is an opt-in adapter-support API. It does not create a JavaVM/JNIEnv,
 * install a vtable, dispatch Java methods or raise Java exceptions. */
#define NXANDROID_JNI_ADAPTER_API_VERSION 1u
#define NXANDROID_JNI_SLOT_GET_STRING_REGION 220u
#define NXANDROID_JNI_SLOT_GET_STRING_UTF_REGION 221u
#define NXANDROID_PREFS_MAX_ENTRIES 4096u
#define NXANDROID_PREFS_MAX_STRING_SET_MEMBERS 4096u

typedef enum nxandroid_jni_adapter_result {
  NXANDROID_JNI_ADAPTER_OK = 0,
  NXANDROID_JNI_ADAPTER_EINVAL = -1,
  NXANDROID_JNI_ADAPTER_ENOMEM = -2,
  NXANDROID_JNI_ADAPTER_EBOUNDS = -3,
  NXANDROID_JNI_ADAPTER_EDUPLICATE = -4
} nxandroid_jni_adapter_result;

const char *nxandroid_jni_adapter_result_string(
    nxandroid_jni_adapter_result result);

/* A UTF-16 view is counted in 16-bit code units, not Unicode code points or
 * UTF-8 bytes. NULL units are valid only for an empty view. */
typedef struct nxandroid_utf16_view {
  const uint16_t *units;
  size_t unit_count;
} nxandroid_utf16_view;

/* Passive substrate for JNI GetStringRegion. start/length have jsize shape.
 * Exactly length UTF-16 code units are copied; no terminator is appended and
 * surrogate pairs are neither interpreted nor normalized. Regions may split a
 * surrogate pair. An empty region at the end is valid and may use output=NULL.
 * On any error output is untouched. Overlapping source/output is supported.
 * EBOUNDS is intended for adapter mapping to
 * StringIndexOutOfBoundsException; EINVAL denotes an invalid view/output. */
nxandroid_jni_adapter_result nxandroid_utf16_copy_region(
    const nxandroid_utf16_view *source, int32_t start, int32_t length,
    uint16_t *output);

typedef enum nxandroid_prefs_value_type {
  NXANDROID_PREFS_STRING = 1,
  NXANDROID_PREFS_STRING_SET,
  NXANDROID_PREFS_INT32,
  NXANDROID_PREFS_INT64,
  NXANDROID_PREFS_FLOAT,
  NXANDROID_PREFS_BOOL
} nxandroid_prefs_value_type;

typedef struct nxandroid_prefs_string_set_view {
  const nxandroid_utf16_view *members;
  size_t member_count;
} nxandroid_prefs_string_set_view;

typedef struct nxandroid_prefs_entry_view {
  nxandroid_utf16_view key;
  nxandroid_prefs_value_type type;
  union {
    nxandroid_utf16_view string_value;
    nxandroid_prefs_string_set_view string_set_value;
    int32_t int32_value;
    int64_t int64_value;
    float float_value;
    int bool_value;
  } value;
} nxandroid_prefs_entry_view;

typedef struct nxandroid_prefs_snapshot nxandroid_prefs_snapshot;

/* Creates the immutable, fully deep-copied typed substrate for an
 * adapter-owned SharedPreferences.getAll(). Input is borrowed only for this
 * call. entries=NULL is valid only when entry_count is zero. Keys must be
 * unique; StringSet members must be unique; bool_value must be exactly 0 or 1.
 * The adapter must hold its store lock for the duration of this call. The
 * helper provides no synchronization and implements no Map/Set/Iterator,
 * boxing, Java signatures or exception policy. On failure *output is NULL and
 * no partial snapshot is published. */
nxandroid_jni_adapter_result nxandroid_prefs_snapshot_create(
    const nxandroid_prefs_entry_view *entries, size_t entry_count,
    nxandroid_prefs_snapshot **output);

/* A real destroy always NULLs *snapshot. It must not race with readers. */
void nxandroid_prefs_snapshot_destroy(nxandroid_prefs_snapshot **snapshot);

/* NULL snapshots report zero/no entry. Returned views and their nested storage
 * are snapshot-owned and remain valid only until destroy. Iteration order is an
 * implementation detail and must not be exposed as a Java Map/Set guarantee. */
size_t nxandroid_prefs_snapshot_count(
    const nxandroid_prefs_snapshot *snapshot);
const nxandroid_prefs_entry_view *nxandroid_prefs_snapshot_entry_at(
    const nxandroid_prefs_snapshot *snapshot, size_t index);

#ifdef __cplusplus
}
#endif

#endif
