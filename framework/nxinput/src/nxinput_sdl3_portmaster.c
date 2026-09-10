/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "nxinput_sdl3_portmaster.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/input.h>

#define NXINPUT_SDL3_PM_BITS_PER_LONG (8u * sizeof(unsigned long))
#define NXINPUT_SDL3_PM_KEY_COUNT ((size_t)KEY_MAX)
#define NXINPUT_SDL3_PM_KEY_WORDS                                      \
  ((NXINPUT_SDL3_PM_KEY_COUNT + NXINPUT_SDL3_PM_BITS_PER_LONG - 1u) / \
   NXINPUT_SDL3_PM_BITS_PER_LONG)

static bool nxinput_sdl3_pm_key_set(const unsigned long *bits,
                                    size_t bit_count,
                                    unsigned int code) {
  return bits != NULL && (size_t)code < bit_count &&
         ((bits[code / NXINPUT_SDL3_PM_BITS_PER_LONG] >>
           (code % NXINPUT_SDL3_PM_BITS_PER_LONG)) & 1ul) != 0ul;
}

static unsigned int nxinput_sdl3_pm_count_keys(
    const unsigned long *bits, size_t bit_count, unsigned int first,
    unsigned int limit) {
  unsigned int count = 0u;
  if ((size_t)limit > bit_count) {
    limit = bit_count > UINT_MAX ? UINT_MAX : (unsigned int)bit_count;
  }
  for (unsigned int code = first; code < limit; ++code) {
    if (nxinput_sdl3_pm_key_set(bits, bit_count, code)) {
      ++count;
    }
  }
  return count;
}

/* PortMaster/joydev mappings involved in the field incident enumerate every
 * EV_KEY capability in ascending numeric order. */
static int nxinput_sdl3_pm_legacy_code(const unsigned long *bits,
                                       size_t bit_count,
                                       unsigned int ordinal) {
  unsigned int rank = 0u;
  unsigned int limit = (unsigned int)NXINPUT_SDL3_PM_KEY_COUNT;
  if ((size_t)limit > bit_count) {
    limit = bit_count > UINT_MAX ? UINT_MAX : (unsigned int)bit_count;
  }
  for (unsigned int code = 0u; code < limit; ++code) {
    if (!nxinput_sdl3_pm_key_set(bits, bit_count, code)) {
      continue;
    }
    if (rank == ordinal) {
      return (int)code;
    }
    ++rank;
  }
  return -1;
}

/* Mirror the Linux evdev ConfigJoystick ordering in the pinned SDL3 runtime:
 * BTN_JOYSTICK..KEY_MAX first, then lower keyboard/media capabilities. */
static int nxinput_sdl3_pm_sdl3_ordinal(const unsigned long *bits,
                                        size_t bit_count,
                                        unsigned int code) {
  if (!nxinput_sdl3_pm_key_set(bits, bit_count, code) || code >= KEY_MAX) {
    return -1;
  }
  if (code >= BTN_JOYSTICK) {
    return (int)nxinput_sdl3_pm_count_keys(bits, bit_count, BTN_JOYSTICK,
                                           code);
  }
  return (int)(nxinput_sdl3_pm_count_keys(
                   bits, bit_count, BTN_JOYSTICK,
                   (unsigned int)NXINPUT_SDL3_PM_KEY_COUNT) +
               nxinput_sdl3_pm_count_keys(bits, bit_count, 0u, code));
}

static int nxinput_sdl3_pm_sdl3_code(const unsigned long *bits,
                                     size_t bit_count,
                                     unsigned int ordinal) {
  unsigned int rank = 0u;
  unsigned int limit = (unsigned int)NXINPUT_SDL3_PM_KEY_COUNT;
  if ((size_t)limit > bit_count) {
    limit = bit_count > UINT_MAX ? UINT_MAX : (unsigned int)bit_count;
  }
  for (unsigned int code = BTN_JOYSTICK; code < limit; ++code) {
    if (!nxinput_sdl3_pm_key_set(bits, bit_count, code)) {
      continue;
    }
    if (rank == ordinal) {
      return (int)code;
    }
    ++rank;
  }
  unsigned int lower_limit = BTN_JOYSTICK < limit ? BTN_JOYSTICK : limit;
  for (unsigned int code = 0u; code < lower_limit; ++code) {
    if (!nxinput_sdl3_pm_key_set(bits, bit_count, code)) {
      continue;
    }
    if (rank == ordinal) {
      return (int)code;
    }
    ++rank;
  }
  return -1;
}

static bool nxinput_sdl3_pm_find_button(const char *mapping,
                                        const char *semantic,
                                        unsigned int *ordinal) {
  const size_t semantic_length = semantic != NULL ? strlen(semantic) : 0u;
  const char *field = mapping;
  unsigned int field_index = 0u;

  if (mapping == NULL || semantic_length == 0u || ordinal == NULL) {
    return false;
  }
  while (*field != '\0') {
    const char *end = strchr(field, ',');
    if (end == NULL) {
      end = field + strlen(field);
    }
    const char *colon = memchr(field, ':', (size_t)(end - field));
    if (field_index >= 2u && colon != NULL &&
        (size_t)(colon - field) == semantic_length &&
        memcmp(field, semantic, semantic_length) == 0 &&
        colon + 2 <= end && colon[1] == 'b' &&
        isdigit((unsigned char)colon[2])) {
      char *number_end = NULL;
      errno = 0;
      unsigned long value = strtoul(colon + 2, &number_end, 10);
      if (errno == 0 && number_end == end && value <= UINT_MAX) {
        *ordinal = (unsigned int)value;
        return true;
      }
      return false;
    }
    field = *end == ',' ? end + 1 : end;
    ++field_index;
  }
  return false;
}

static bool nxinput_sdl3_pm_guid_valid(const char *guid) {
  if (guid == NULL || strlen(guid) != 32u) {
    return false;
  }
  for (size_t index = 0u; index < 32u; ++index) {
    if (!isxdigit((unsigned char)guid[index])) {
      return false;
    }
  }
  return true;
}

typedef struct nxinput_sdl3_pm_output {
  char *data;
  size_t capacity;
  size_t length;
} nxinput_sdl3_pm_output;

static bool nxinput_sdl3_pm_append(nxinput_sdl3_pm_output *output,
                                   const char *data, size_t length) {
  if (output == NULL || data == NULL || output->length >= output->capacity ||
      length >= output->capacity - output->length) {
    return false;
  }
  memcpy(output->data + output->length, data, length);
  output->length += length;
  output->data[output->length] = '\0';
  return true;
}

static bool nxinput_sdl3_pm_append_ordinal(nxinput_sdl3_pm_output *output,
                                           unsigned int ordinal) {
  char number[16];
  int length = snprintf(number, sizeof(number), "%u", ordinal);
  return length > 0 && (size_t)length < sizeof(number) &&
         nxinput_sdl3_pm_append(output, number, (size_t)length);
}

/* V4-CONTROLLERS-02: heterogeneous multi-entry selection. See the header for
 * the exact rule. Pure: no SDL, no environment, no device. */
int nxinput_sdl3_pm_select_mapping(const char *config, const char *target_guid,
                                   char *output, size_t output_size,
                                   unsigned int *entry_count) {
  const char *cursor;
  const char *only_entry = NULL;
  size_t only_length = 0u;
  const char *matched = NULL;
  size_t matched_length = 0u;
  unsigned int entries = 0u;
  unsigned int matches = 0u;

  if (entry_count != NULL) {
    *entry_count = 0u;
  }
  if (config == NULL || output == NULL || output_size == 0u ||
      output == config || !nxinput_sdl3_pm_guid_valid(target_guid)) {
    errno = EINVAL;
    return NXINPUT_SDL3_PM_ERROR;
  }
  output[0] = '\0';

  for (cursor = config; *cursor != '\0';) {
    const char *end = strchr(cursor, '\n');
    size_t length;
    if (end == NULL) {
      end = cursor + strlen(cursor);
    }
    length = (size_t)(end - cursor);
    /* Tolerate CRLF: the list may be edited on any host. */
    while (length > 0u && (cursor[length - 1u] == '\r' ||
                           cursor[length - 1u] == ' ' ||
                           cursor[length - 1u] == '\t')) {
      --length;
    }
    if (length == 0u || cursor[0] == '#') {
      cursor = (*end == '\n') ? end + 1 : end;
      continue;
    }
    if (length >= NXINPUT_SDL3_PM_MAPPING_MAX) {
      errno = E2BIG;
      return NXINPUT_SDL3_PM_ERROR;
    }
    /* Every entry must carry a real GUID first field and at least two commas,
     * exactly like the single-entry contract. */
    if (length < 34u || cursor[32] != ',' ||
        memchr(cursor + 33, ',', length - 33u) == NULL) {
      errno = EINVAL;
      return NXINPUT_SDL3_PM_ERROR;
    }
    {
      char guid[33];
      memcpy(guid, cursor, 32u);
      guid[32] = '\0';
      if (!nxinput_sdl3_pm_guid_valid(guid)) {
        errno = EINVAL;
        return NXINPUT_SDL3_PM_ERROR;
      }
      ++entries;
      if (entries == 1u) {
        only_entry = cursor;
        only_length = length;
      }
      if (strncmp(guid, target_guid, 32u) == 0) {
        if (matched != NULL &&
            (matched_length != length ||
             memcmp(matched, cursor, length) != 0)) {
          /* Two live entries claim the same GUID with different bytes. The
           * SDL3 mapping store is keyed by GUID, so honouring one silently
           * would depend on list order. Fail closed instead. */
          errno = EPROTO;
          return NXINPUT_SDL3_PM_ERROR;
        }
        matched = cursor;
        matched_length = length;
        ++matches;
      }
    }
    cursor = (*end == '\n') ? end + 1 : end;
  }

  if (entry_count != NULL) {
    *entry_count = entries;
  }
  if (entries == 0u) {
    return NXINPUT_SDL3_PM_NOT_APPLICABLE;
  }
  if (matched == NULL) {
    if (entries > 1u) {
      /* Several heterogeneous entries and none is for this device. Choosing
       * by position is exactly the false success this front exists to stop. */
      return NXINPUT_SDL3_PM_NOT_APPLICABLE;
    }
    matched = only_entry;
    matched_length = only_length;
  }
  (void)matches;
  if (matched_length >= output_size) {
    errno = ENOSPC;
    return NXINPUT_SDL3_PM_ERROR;
  }
  memcpy(output, matched, matched_length);
  output[matched_length] = '\0';
  return NXINPUT_SDL3_PM_REWRITTEN;
}

int nxinput_sdl3_pm_convert_mapping(
    const char *mapping, const unsigned long *key_bits, size_t key_bit_count,
    const char *target_guid, char *output, size_t output_size,
    nxinput_sdl3_pm_evidence *evidence) {
  nxinput_sdl3_pm_evidence local = {0};
  nxinput_sdl3_pm_output writer = {output, output_size, 0u};
  unsigned int volume_down = 0u;
  unsigned int volume_up = 0u;

  if (evidence != NULL) {
    memset(evidence, 0, sizeof(*evidence));
  }
  if (mapping == NULL || key_bits == NULL || output == NULL ||
      output_size == 0u || output == mapping ||
      !nxinput_sdl3_pm_guid_valid(target_guid)) {
    errno = EINVAL;
    return NXINPUT_SDL3_PM_ERROR;
  }
  output[0] = '\0';

  local.gamepad_buttons = nxinput_sdl3_pm_count_keys(
      key_bits, key_bit_count, BTN_JOYSTICK,
      (unsigned int)NXINPUT_SDL3_PM_KEY_COUNT);
  local.lower_key_buttons = nxinput_sdl3_pm_count_keys(
      key_bits, key_bit_count, 0u, BTN_JOYSTICK);
  local.key_buttons = local.gamepad_buttons + local.lower_key_buttons;

  /* Both ordinal schemes are identical without lower keys. Requiring a real
   * gamepad class prevents this opt-in from translating keyboard-only nodes. */
  if (!nxinput_sdl3_pm_key_set(key_bits, key_bit_count, BTN_GAMEPAD) ||
      local.gamepad_buttons < 4u || local.lower_key_buttons == 0u) {
    if (evidence != NULL) {
      *evidence = local;
    }
    return NXINPUT_SDL3_PM_NOT_APPLICABLE;
  }

  const char *first_comma = strchr(mapping, ',');
  if (first_comma == NULL || first_comma == mapping ||
      strchr(first_comma + 1, ',') == NULL || strchr(mapping, '\n') != NULL ||
      strchr(mapping, '\r') != NULL) {
    errno = EINVAL;
    return NXINPUT_SDL3_PM_ERROR;
  }

  /* A positive legacy proof comes from the exact node capabilities and the
   * mapping's media semantics. Native/already converted SDL3 mappings fail
   * this predicate and remain byte-for-byte untouched. */
  if (!nxinput_sdl3_pm_find_button(mapping, "volumedown", &volume_down) ||
      !nxinput_sdl3_pm_find_button(mapping, "volumeup", &volume_up) ||
      nxinput_sdl3_pm_legacy_code(key_bits, key_bit_count, volume_down) !=
          KEY_VOLUMEDOWN ||
      nxinput_sdl3_pm_legacy_code(key_bits, key_bit_count, volume_up) !=
          KEY_VOLUMEUP ||
      (nxinput_sdl3_pm_sdl3_code(key_bits, key_bit_count, volume_down) ==
           KEY_VOLUMEDOWN &&
       nxinput_sdl3_pm_sdl3_code(key_bits, key_bit_count, volume_up) ==
           KEY_VOLUMEUP)) {
    if (evidence != NULL) {
      *evidence = local;
    }
    return NXINPUT_SDL3_PM_NOT_APPLICABLE;
  }
  local.legacy_volume_markers = 2u;

  if (!nxinput_sdl3_pm_append(&writer, target_guid, 32u) ||
      !nxinput_sdl3_pm_append(&writer, first_comma, 1u)) {
    errno = ENOSPC;
    return NXINPUT_SDL3_PM_ERROR;
  }

  const char *field = first_comma + 1;
  unsigned int field_index = 1u;
  while (*field != '\0') {
    const char *end = strchr(field, ',');
    bool rewritten = false;
    if (end == NULL) {
      end = field + strlen(field);
    }
    const char *colon = memchr(field, ':', (size_t)(end - field));
    if (field_index >= 2u && colon != NULL && colon + 2 <= end &&
        colon[1] == 'b' && isdigit((unsigned char)colon[2])) {
      char *number_end = NULL;
      errno = 0;
      unsigned long old_ordinal = strtoul(colon + 2, &number_end, 10);
      if (errno == 0 && number_end == end && old_ordinal <= UINT_MAX) {
        int code = nxinput_sdl3_pm_legacy_code(
            key_bits, key_bit_count, (unsigned int)old_ordinal);
        int new_ordinal =
            code < 0 ? -1
                     : nxinput_sdl3_pm_sdl3_ordinal(
                           key_bits, key_bit_count, (unsigned int)code);
        if (new_ordinal < 0) {
          errno = ERANGE;
          return NXINPUT_SDL3_PM_ERROR;
        }
        if (!nxinput_sdl3_pm_append(
                &writer, field, (size_t)(colon + 2 - field)) ||
            !nxinput_sdl3_pm_append_ordinal(&writer,
                                            (unsigned int)new_ordinal)) {
          errno = ENOSPC;
          return NXINPUT_SDL3_PM_ERROR;
        }
        ++local.button_bindings;
        if ((unsigned int)new_ordinal != (unsigned int)old_ordinal) {
          ++local.rewritten_bindings;
        }
        rewritten = true;
      }
    }
    if (!rewritten && !nxinput_sdl3_pm_append(
                          &writer, field, (size_t)(end - field))) {
      errno = ENOSPC;
      return NXINPUT_SDL3_PM_ERROR;
    }
    if (*end == ',' && !nxinput_sdl3_pm_append(&writer, end, 1u)) {
      errno = ENOSPC;
      return NXINPUT_SDL3_PM_ERROR;
    }
    field = *end == ',' ? end + 1 : end;
    ++field_index;
  }

  if (local.button_bindings == 0u || local.rewritten_bindings == 0u) {
    if (evidence != NULL) {
      *evidence = local;
    }
    output[0] = '\0';
    return NXINPUT_SDL3_PM_NOT_APPLICABLE;
  }
  if (evidence != NULL) {
    *evidence = local;
  }
  return NXINPUT_SDL3_PM_REWRITTEN;
}

#ifndef NXINPUT_SDL3_PM_CORE_ONLY

#include <pthread.h>

typedef struct nxinput_sdl3_pm_cache_entry {
  bool occupied;
  bool stable;
  SDL_JoystickID instance_id;
  char guid[33];
  char wanted[NXINPUT_SDL3_PM_MAPPING_MAX];
  nxinput_sdl3_pm_receipt receipt;
} nxinput_sdl3_pm_cache_entry;

struct nxinput_sdl3_pm_context {
  pthread_mutex_t mutex;
  bool has_portmaster_mapping;
  char portmaster_mapping[NXINPUT_SDL3_PM_MAPPING_MAX];
  nxinput_sdl3_pm_cache_entry entries[NXINPUT_SDL3_PM_CACHE_MAX];
};

typedef struct nxinput_sdl3_pm_candidate {
  int result;
  unsigned int entry_count;
  char guid[33];
  char wanted[NXINPUT_SDL3_PM_MAPPING_MAX];
  nxinput_sdl3_pm_evidence evidence;
} nxinput_sdl3_pm_candidate;

typedef struct nxinput_sdl3_pm_binding_view {
  const char *semantic;
  size_t semantic_length;
  const char *binding;
  size_t binding_length;
} nxinput_sdl3_pm_binding_view;

static bool nxinput_sdl3_pm_stage_valid(nxinput_sdl3_pm_stage stage) {
  return stage == NXINPUT_SDL3_PM_STAGE_ENUMERATION ||
         stage == NXINPUT_SDL3_PM_STAGE_CLASSIFICATION ||
         stage == NXINPUT_SDL3_PM_STAGE_OPEN;
}

static const char *nxinput_sdl3_pm_binding_fields(const char *mapping) {
  const char *first;
  const char *second;
  if (mapping == NULL || (first = strchr(mapping, ',')) == NULL ||
      (second = strchr(first + 1, ',')) == NULL) {
    return NULL;
  }
  return second + 1;
}

static bool nxinput_sdl3_pm_decimal_span(const char **cursor,
                                         const char *end) {
  const char *start = *cursor;
  while (*cursor < end && isdigit((unsigned char)**cursor)) {
    ++*cursor;
  }
  return *cursor != start;
}

/* SDL may add or reorder metadata such as platform/crc when it renders a
 * mapping. Compare the actual input bindings instead of comparing bytes. */
static bool nxinput_sdl3_pm_binding_value(const char *value,
                                          const char *end) {
  const char *cursor = value;
  if (cursor < end && (*cursor == '+' || *cursor == '-')) {
    ++cursor;
  }
  if (cursor >= end) {
    return false;
  }
  if (*cursor == 'a' || *cursor == 'b') {
    ++cursor;
    if (!nxinput_sdl3_pm_decimal_span(&cursor, end)) {
      return false;
    }
    if (cursor < end && *cursor == '~') {
      ++cursor;
    }
    return cursor == end;
  }
  if (*cursor == 'h') {
    ++cursor;
    if (!nxinput_sdl3_pm_decimal_span(&cursor, end) || cursor >= end ||
        *cursor != '.') {
      return false;
    }
    ++cursor;
    return nxinput_sdl3_pm_decimal_span(&cursor, end) && cursor == end;
  }
  return false;
}

/* Return 1 for one binding, 0 at end and -1 for a malformed mapping. */
static int nxinput_sdl3_pm_next_binding(
    const char **cursor, nxinput_sdl3_pm_binding_view *binding) {
  if (cursor == NULL || *cursor == NULL || binding == NULL) {
    return -1;
  }
  while (**cursor != '\0') {
    const char *field = *cursor;
    const char *end = strchr(field, ',');
    if (end == NULL) {
      end = field + strlen(field);
    }
    const char *colon = memchr(field, ':', (size_t)(end - field));
    *cursor = *end == ',' ? end + 1 : end;
    if (colon == NULL || colon == field || colon + 1 == end ||
        !nxinput_sdl3_pm_binding_value(colon + 1, end)) {
      continue;
    }
    binding->semantic = field;
    binding->semantic_length = (size_t)(colon - field);
    binding->binding = colon + 1;
    binding->binding_length = (size_t)(end - (colon + 1));
    return 1;
  }
  return 0;
}

static bool nxinput_sdl3_pm_same_span(const char *left, size_t left_length,
                                      const char *right,
                                      size_t right_length) {
  return left_length == right_length &&
         memcmp(left, right, left_length) == 0;
}

static bool nxinput_sdl3_pm_effective_mapping_matches(
    const char *wanted, const char *effective) {
  const char *wanted_cursor = nxinput_sdl3_pm_binding_fields(wanted);
  const char *effective_start = nxinput_sdl3_pm_binding_fields(effective);
  nxinput_sdl3_pm_binding_view wanted_binding;
  unsigned int wanted_count = 0u;
  unsigned int effective_count = 0u;
  int next;

  if (wanted_cursor == NULL || effective_start == NULL) {
    return false;
  }
  while ((next = nxinput_sdl3_pm_next_binding(
              &wanted_cursor, &wanted_binding)) > 0) {
    const char *effective_cursor = effective_start;
    nxinput_sdl3_pm_binding_view effective_binding;
    unsigned int semantic_matches = 0u;
    int effective_next;
    ++wanted_count;
    while ((effective_next = nxinput_sdl3_pm_next_binding(
                &effective_cursor, &effective_binding)) > 0) {
      if (!nxinput_sdl3_pm_same_span(
              wanted_binding.semantic, wanted_binding.semantic_length,
              effective_binding.semantic, effective_binding.semantic_length)) {
        continue;
      }
      ++semantic_matches;
      if (!nxinput_sdl3_pm_same_span(
              wanted_binding.binding, wanted_binding.binding_length,
              effective_binding.binding, effective_binding.binding_length)) {
        return false;
      }
    }
    if (effective_next < 0 || semantic_matches != 1u) {
      return false;
    }
  }
  if (next < 0) {
    return false;
  }

  const char *effective_cursor = effective_start;
  nxinput_sdl3_pm_binding_view effective_binding;
  while ((next = nxinput_sdl3_pm_next_binding(
              &effective_cursor, &effective_binding)) > 0) {
    ++effective_count;
  }
  return next == 0 && wanted_count > 0u && wanted_count == effective_count;
}

static void nxinput_sdl3_pm_receipt_init(
    nxinput_sdl3_pm_receipt *receipt, SDL_JoystickID instance_id,
    nxinput_sdl3_pm_stage stage) {
  if (receipt == NULL) {
    return;
  }
  memset(receipt, 0, sizeof(*receipt));
  receipt->instance_id = (uint32_t)instance_id;
  receipt->stage = stage;
  receipt->result = NXINPUT_SDL3_PM_ERROR;
  receipt->reason = NXINPUT_SDL3_PM_REASON_NONE;
}

static nxinput_sdl3_pm_cache_entry *nxinput_sdl3_pm_find_entry(
    nxinput_sdl3_pm_context *context, SDL_JoystickID instance_id) {
  for (size_t index = 0u; index < NXINPUT_SDL3_PM_CACHE_MAX; ++index) {
    nxinput_sdl3_pm_cache_entry *entry = &context->entries[index];
    if (entry->occupied && entry->instance_id == instance_id) {
      return entry;
    }
  }
  return NULL;
}

static nxinput_sdl3_pm_cache_entry *nxinput_sdl3_pm_find_empty(
    nxinput_sdl3_pm_context *context) {
  for (size_t index = 0u; index < NXINPUT_SDL3_PM_CACHE_MAX; ++index) {
    if (!context->entries[index].occupied) {
      return &context->entries[index];
    }
  }
  return NULL;
}

static bool nxinput_sdl3_pm_guid_collision(
    nxinput_sdl3_pm_context *context, SDL_JoystickID instance_id,
    const char *guid, const char *wanted) {
  for (size_t index = 0u; index < NXINPUT_SDL3_PM_CACHE_MAX; ++index) {
    const nxinput_sdl3_pm_cache_entry *entry = &context->entries[index];
    if (!entry->occupied || entry->instance_id == instance_id ||
        entry->guid[0] == '\0' || strcmp(entry->guid, guid) != 0) {
      continue;
    }
    if (strcmp(entry->wanted, wanted) != 0) {
      return true;
    }
  }
  return false;
}

static void nxinput_sdl3_pm_copy_cached_receipt(
    const nxinput_sdl3_pm_cache_entry *entry, nxinput_sdl3_pm_stage stage,
    unsigned int cache_hit, nxinput_sdl3_pm_receipt *receipt) {
  if (receipt == NULL) {
    return;
  }
  *receipt = entry->receipt;
  receipt->stage = stage;
  receipt->cache_hit = cache_hit;
}

static int nxinput_sdl3_pm_build_candidate(
    nxinput_sdl3_pm_context *context, SDL_JoystickID instance_id,
    nxinput_sdl3_pm_candidate *candidate, nxinput_sdl3_pm_reason *reason) {
  unsigned long key_bits[NXINPUT_SDL3_PM_KEY_WORDS];
  char rewritten[NXINPUT_SDL3_PM_MAPPING_MAX];
  char selected_entry[NXINPUT_SDL3_PM_MAPPING_MAX];
  const char *path;
  struct stat metadata;
  int flags = O_RDONLY | O_NONBLOCK | O_CLOEXEC;

  memset(candidate, 0, sizeof(*candidate));
  *reason = NXINPUT_SDL3_PM_REASON_NONE;
  errno = 0;

  path = SDL_GetJoystickPathForID(instance_id);
  if (path == NULL || *path == '\0') {
    errno = ENOENT;
    *reason = NXINPUT_SDL3_PM_REASON_NO_DEVICE_PATH;
    return NXINPUT_SDL3_PM_ERROR;
  }
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  int descriptor = open(path, flags);
  if (descriptor < 0) {
    *reason = NXINPUT_SDL3_PM_REASON_DEVICE_OPEN_FAILED;
    return NXINPUT_SDL3_PM_ERROR;
  }
  if (fstat(descriptor, &metadata) != 0) {
    const int saved_errno = errno;
    close(descriptor);
    errno = saved_errno;
    *reason = NXINPUT_SDL3_PM_REASON_DEVICE_STAT_FAILED;
    return NXINPUT_SDL3_PM_ERROR;
  }
  if (!S_ISCHR(metadata.st_mode)) {
    close(descriptor);
    errno = ENOTTY;
    *reason = NXINPUT_SDL3_PM_REASON_NOT_CHARACTER_DEVICE;
    return NXINPUT_SDL3_PM_ERROR;
  }
  memset(key_bits, 0, sizeof(key_bits));
  if (ioctl(descriptor, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
    const int saved_errno = errno;
    close(descriptor);
    errno = saved_errno;
    *reason = NXINPUT_SDL3_PM_REASON_CAPABILITY_QUERY_FAILED;
    return NXINPUT_SDL3_PM_ERROR;
  }
  close(descriptor);

  SDL_GUIDToString(SDL_GetJoystickGUIDForID(instance_id), candidate->guid,
                   (int)sizeof(candidate->guid));
  if (!nxinput_sdl3_pm_guid_valid(candidate->guid)) {
    errno = EINVAL;
    *reason = NXINPUT_SDL3_PM_REASON_CONVERSION_FAILED;
    return NXINPUT_SDL3_PM_ERROR;
  }
  /* V4-CONTROLLERS-02: the staged config may be a heterogeneous list. Pick the
   * entry that belongs to THIS device before any conversion; never guess. */
  {
    const int selected = nxinput_sdl3_pm_select_mapping(
        context->portmaster_mapping, candidate->guid, selected_entry,
        sizeof(selected_entry), &candidate->entry_count);
    if (selected == NXINPUT_SDL3_PM_ERROR) {
      *reason = errno == EPROTO ? NXINPUT_SDL3_PM_REASON_GUID_COLLISION
                                : NXINPUT_SDL3_PM_REASON_CONVERSION_FAILED;
      return NXINPUT_SDL3_PM_ERROR;
    }
    if (selected == NXINPUT_SDL3_PM_NOT_APPLICABLE) {
      candidate->result = NXINPUT_SDL3_PM_NOT_APPLICABLE;
      *reason = NXINPUT_SDL3_PM_REASON_NO_MAPPING;
      candidate->wanted[0] = '\0';
      return NXINPUT_SDL3_PM_NOT_APPLICABLE;
    }
  }
  candidate->result = nxinput_sdl3_pm_convert_mapping(
      selected_entry, key_bits, NXINPUT_SDL3_PM_KEY_COUNT,
      candidate->guid,
      rewritten, sizeof(rewritten), &candidate->evidence);
  if (candidate->result == NXINPUT_SDL3_PM_ERROR) {
    *reason = NXINPUT_SDL3_PM_REASON_CONVERSION_FAILED;
    return NXINPUT_SDL3_PM_ERROR;
  }

  const char *wanted = candidate->result == NXINPUT_SDL3_PM_REWRITTEN
                           ? rewritten
                           : selected_entry;
  size_t wanted_length = strlen(wanted);
  if (wanted_length >= sizeof(candidate->wanted)) {
    errno = ENOSPC;
    *reason = NXINPUT_SDL3_PM_REASON_CONVERSION_FAILED;
    return NXINPUT_SDL3_PM_ERROR;
  }
  memcpy(candidate->wanted, wanted, wanted_length + 1u);
  *reason = candidate->result == NXINPUT_SDL3_PM_REWRITTEN
                ? NXINPUT_SDL3_PM_REASON_REWRITTEN
                : NXINPUT_SDL3_PM_REASON_NATIVE_OR_UNSUPPORTED;
  return candidate->result;
}

static nxinput_sdl3_pm_context *nxinput_sdl3_pm_create_copy(
    const char *portmaster_mapping) {
  nxinput_sdl3_pm_context *context =
      (nxinput_sdl3_pm_context *)calloc(1u, sizeof(*context));
  if (context == NULL) {
    errno = ENOMEM;
    return NULL;
  }
  if (portmaster_mapping != NULL && *portmaster_mapping != '\0') {
    size_t length = strlen(portmaster_mapping);
    if (length >= sizeof(context->portmaster_mapping)) {
      free(context);
      errno = E2BIG;
      return NULL;
    }
    memcpy(context->portmaster_mapping, portmaster_mapping, length + 1u);
    context->has_portmaster_mapping = true;
  }
  if (pthread_mutex_init(&context->mutex, NULL) != 0) {
    free(context);
    errno = EBUSY;
    return NULL;
  }
  return context;
}

nxinput_sdl3_pm_context *nxinput_sdl3_pm_stage_before_sdl_init(void) {
  const char *environment_mapping;
  nxinput_sdl3_pm_context *context;
  bool environment_present;

  /* Staging after any SDL subsystem is active cannot prove that the gamepad
   * hint was not already imported at USER priority. Fail before touching the
   * environment so the caller can report the original launch state. */
  if (SDL_WasInit(0) != 0) {
    errno = EBUSY;
    return NULL;
  }
  environment_mapping = getenv("SDL_GAMECONTROLLERCONFIG");
  environment_present = environment_mapping != NULL;
  context = nxinput_sdl3_pm_create_copy(
      environment_mapping != NULL && *environment_mapping != '\0'
          ? environment_mapping
          : NULL);
  if (context == NULL) {
    return NULL;
  }
  if (environment_present && unsetenv("SDL_GAMECONTROLLERCONFIG") != 0) {
    const int saved_errno = errno != 0 ? errno : EIO;
    nxinput_sdl3_pm_destroy(context);
    errno = saved_errno;
    return NULL;
  }
  return context;
}

void nxinput_sdl3_pm_destroy(nxinput_sdl3_pm_context *context) {
  if (context == NULL) {
    return;
  }
  (void)pthread_mutex_destroy(&context->mutex);
  memset(context, 0, sizeof(*context));
  free(context);
}

void nxinput_sdl3_pm_reset(nxinput_sdl3_pm_context *context) {
  if (context == NULL || pthread_mutex_lock(&context->mutex) != 0) {
    return;
  }
  memset(context->entries, 0, sizeof(context->entries));
  (void)pthread_mutex_unlock(&context->mutex);
}

void nxinput_sdl3_pm_remove(nxinput_sdl3_pm_context *context,
                            SDL_JoystickID instance_id) {
  if (context == NULL || pthread_mutex_lock(&context->mutex) != 0) {
    return;
  }
  for (size_t index = 0u; index < NXINPUT_SDL3_PM_CACHE_MAX; ++index) {
    nxinput_sdl3_pm_cache_entry *entry = &context->entries[index];
    if (entry->occupied && entry->instance_id == instance_id) {
      memset(entry, 0, sizeof(*entry));
    }
  }
  (void)pthread_mutex_unlock(&context->mutex);
}

int nxinput_sdl3_pm_prepare(nxinput_sdl3_pm_context *context,
                            SDL_JoystickID instance_id,
                            nxinput_sdl3_pm_stage stage,
                            nxinput_sdl3_pm_receipt *receipt) {
  nxinput_sdl3_pm_candidate candidate;
  nxinput_sdl3_pm_reason reason = NXINPUT_SDL3_PM_REASON_NONE;
  nxinput_sdl3_pm_receipt local;
  nxinput_sdl3_pm_cache_entry *entry;
  int result;

  nxinput_sdl3_pm_receipt_init(&local, instance_id, stage);
  if (context == NULL || !nxinput_sdl3_pm_stage_valid(stage)) {
    errno = EINVAL;
    local.reason = NXINPUT_SDL3_PM_REASON_INVALID_ARGUMENT;
    local.error_number = errno;
    if (receipt != NULL) {
      *receipt = local;
    }
    return NXINPUT_SDL3_PM_ERROR;
  }

  /* No PortMaster mapping means pure passthrough. Do not consume one of the
   * sixteen rewrite-cache slots and do not impose a synthetic device limit on
   * an adapter that has not opted into translation. */
  if (!context->has_portmaster_mapping) {
    local.result = NXINPUT_SDL3_PM_NOT_APPLICABLE;
    local.reason = NXINPUT_SDL3_PM_REASON_NO_MAPPING;
    if (receipt != NULL) {
      *receipt = local;
    }
    return NXINPUT_SDL3_PM_NOT_APPLICABLE;
  }

  if (pthread_mutex_lock(&context->mutex) != 0) {
    errno = EBUSY;
    local.reason = NXINPUT_SDL3_PM_REASON_INVALID_ARGUMENT;
    local.error_number = errno;
    if (receipt != NULL) {
      *receipt = local;
    }
    return NXINPUT_SDL3_PM_ERROR;
  }
  entry = nxinput_sdl3_pm_find_entry(context, instance_id);
  if (entry != NULL && entry->stable) {
    result = entry->receipt.result;
    nxinput_sdl3_pm_copy_cached_receipt(entry, stage, 1u, &local);
    (void)pthread_mutex_unlock(&context->mutex);
    if (receipt != NULL) {
      *receipt = local;
    }
    return result;
  }
  if (entry != NULL) {
    (void)pthread_mutex_unlock(&context->mutex);
    errno = EAGAIN;
    local.reason = NXINPUT_SDL3_PM_REASON_INVALID_ARGUMENT;
    local.error_number = errno;
    if (receipt != NULL) {
      *receipt = local;
    }
    return NXINPUT_SDL3_PM_ERROR;
  }

  (void)pthread_mutex_unlock(&context->mutex);

  result = nxinput_sdl3_pm_build_candidate(context, instance_id, &candidate,
                                            &reason);
  /* V4-CONTROLLERS-02: a staged list that carries no entry for this device is
   * indistinguishable, for this device, from no mapping at all. Registering
   * anything here would install a foreign pad's ordinals. Stay a passthrough:
   * no SDL call, no cache, and the guest's own flow is untouched. */
  if (result == NXINPUT_SDL3_PM_NOT_APPLICABLE &&
      reason == NXINPUT_SDL3_PM_REASON_NO_MAPPING) {
    local.result = result;
    local.reason = reason;
    local.error_number = 0;
    local.evidence = candidate.evidence;
    local.staged_entry_count = candidate.entry_count;
    if (receipt != NULL) {
      *receipt = local;
    }
    return result;
  }
  if (result == NXINPUT_SDL3_PM_ERROR) {
    local.reason = reason;
    local.error_number = errno;
    local.evidence = candidate.evidence;
    local.staged_entry_count = candidate.entry_count;
    if (receipt != NULL) {
      *receipt = local;
    }
    return result;
  }

  if (pthread_mutex_lock(&context->mutex) != 0) {
    errno = EBUSY;
    local.reason = NXINPUT_SDL3_PM_REASON_INVALID_ARGUMENT;
    local.error_number = errno;
    if (receipt != NULL) {
      *receipt = local;
    }
    return NXINPUT_SDL3_PM_ERROR;
  }
  entry = nxinput_sdl3_pm_find_entry(context, instance_id);
  if (entry != NULL && entry->stable) {
    result = entry->receipt.result;
    nxinput_sdl3_pm_copy_cached_receipt(entry, stage, 1u, &local);
    (void)pthread_mutex_unlock(&context->mutex);
    if (receipt != NULL) {
      *receipt = local;
    }
    return result;
  }

  if (entry != NULL || nxinput_sdl3_pm_guid_collision(
                           context, instance_id, candidate.guid,
                           candidate.wanted)) {
    (void)pthread_mutex_unlock(&context->mutex);
    errno = entry != NULL ? EAGAIN : EEXIST;
    local.reason = entry != NULL ? NXINPUT_SDL3_PM_REASON_INVALID_ARGUMENT
                                 : NXINPUT_SDL3_PM_REASON_GUID_COLLISION;
    local.error_number = errno;
    local.evidence = candidate.evidence;
    local.staged_entry_count = candidate.entry_count;
    if (receipt != NULL) {
      *receipt = local;
    }
    return NXINPUT_SDL3_PM_ERROR;
  }
  entry = nxinput_sdl3_pm_find_empty(context);
  if (entry == NULL) {
    (void)pthread_mutex_unlock(&context->mutex);
    errno = ENOSPC;
    local.reason = NXINPUT_SDL3_PM_REASON_CACHE_FULL;
    local.error_number = errno;
    local.evidence = candidate.evidence;
    local.staged_entry_count = candidate.entry_count;
    if (receipt != NULL) {
      *receipt = local;
    }
    return NXINPUT_SDL3_PM_ERROR;
  }

  /* Reserve this instance/GUID pair before calling SDL. The reservation is
   * not a cached verdict and is removed on every transient failure. It only
   * serializes same-GUID decisions without holding our mutex across SDL. */
  memset(entry, 0, sizeof(*entry));
  entry->occupied = true;
  entry->instance_id = instance_id;
  entry->receipt.result = result;
  memcpy(entry->guid, candidate.guid, sizeof(entry->guid));
  memcpy(entry->wanted, candidate.wanted,
         strlen(candidate.wanted) + 1u);
  (void)pthread_mutex_unlock(&context->mutex);

  if (!SDL_SetGamepadMapping(instance_id, candidate.wanted)) {
    const int saved_errno = errno != 0 ? errno : EIO;
    nxinput_sdl3_pm_remove(context, instance_id);
    errno = saved_errno;
    local.reason = NXINPUT_SDL3_PM_REASON_REGISTRATION_FAILED;
    local.error_number = errno;
    local.evidence = candidate.evidence;
    local.staged_entry_count = candidate.entry_count;
    if (receipt != NULL) {
      *receipt = local;
    }
    return NXINPUT_SDL3_PM_ERROR;
  }

  char *effective = SDL_GetGamepadMappingForID(instance_id);
  if (effective == NULL || !nxinput_sdl3_pm_effective_mapping_matches(
                               candidate.wanted, effective)) {
    SDL_free(effective);
    nxinput_sdl3_pm_remove(context, instance_id);
    errno = EPROTO;
    local.reason = NXINPUT_SDL3_PM_REASON_EFFECTIVE_MAPPING_MISMATCH;
    local.error_number = errno;
    local.evidence = candidate.evidence;
    local.staged_entry_count = candidate.entry_count;
    if (receipt != NULL) {
      *receipt = local;
    }
    return NXINPUT_SDL3_PM_ERROR;
  }
  SDL_free(effective);

  local.result = candidate.result;
  local.reason = reason;
  local.evidence = candidate.evidence;
    local.staged_entry_count = candidate.entry_count;
  local.error_number = 0;
  local.effective_mapping_verified = 1u;

  if (pthread_mutex_lock(&context->mutex) != 0) {
    nxinput_sdl3_pm_remove(context, instance_id);
    errno = EBUSY;
    local.result = NXINPUT_SDL3_PM_ERROR;
    local.reason = NXINPUT_SDL3_PM_REASON_INVALID_ARGUMENT;
    local.error_number = errno;
    if (receipt != NULL) {
      *receipt = local;
    }
    return NXINPUT_SDL3_PM_ERROR;
  }
  entry = nxinput_sdl3_pm_find_entry(context, instance_id);
  if (entry == NULL || entry->stable ||
      nxinput_sdl3_pm_guid_collision(context, instance_id, candidate.guid,
                                     candidate.wanted)) {
    if (entry != NULL) {
      memset(entry, 0, sizeof(*entry));
    }
    (void)pthread_mutex_unlock(&context->mutex);
    errno = EEXIST;
    local.result = NXINPUT_SDL3_PM_ERROR;
    local.reason = NXINPUT_SDL3_PM_REASON_GUID_COLLISION;
    local.error_number = errno;
    if (receipt != NULL) {
      *receipt = local;
    }
    return NXINPUT_SDL3_PM_ERROR;
  }
  entry->stable = true;
  entry->receipt = local;
  (void)pthread_mutex_unlock(&context->mutex);

  if (receipt != NULL) {
    *receipt = local;
  }
  return result;
}

SDL_JoystickID *
nxinput_sdl3_pm_get_joysticks(nxinput_sdl3_pm_context *context, int *count) {
  SDL_JoystickID *ids = SDL_GetJoysticks(count);
  int total = 0;
  if (ids != NULL) {
    if (count != NULL && *count > 0) {
      total = *count;
    } else if (count == NULL) {
      while (ids[total] != 0) {
        ++total;
      }
    }
  }
  for (int index = 0; index < total; ++index) {
    (void)nxinput_sdl3_pm_prepare(context, ids[index],
                                  NXINPUT_SDL3_PM_STAGE_ENUMERATION, NULL);
  }
  return ids;
}

bool nxinput_sdl3_pm_is_gamepad(nxinput_sdl3_pm_context *context,
                                SDL_JoystickID instance_id) {
  (void)nxinput_sdl3_pm_prepare(
      context, instance_id, NXINPUT_SDL3_PM_STAGE_CLASSIFICATION, NULL);
  bool result = SDL_IsGamepad(instance_id);

  if (context != NULL && pthread_mutex_lock(&context->mutex) == 0) {
    nxinput_sdl3_pm_cache_entry *entry =
        nxinput_sdl3_pm_find_entry(context, instance_id);
    if (entry != NULL && entry->stable) {
      entry->receipt.classification_observed = 1u;
      entry->receipt.classification_result = result ? 1u : 0u;
    }
    (void)pthread_mutex_unlock(&context->mutex);
  }
  return result;
}

SDL_Gamepad *nxinput_sdl3_pm_open_gamepad(nxinput_sdl3_pm_context *context,
                                          SDL_JoystickID instance_id) {
  (void)nxinput_sdl3_pm_prepare(
      context, instance_id, NXINPUT_SDL3_PM_STAGE_OPEN, NULL);
  SDL_Gamepad *gamepad = SDL_OpenGamepad(instance_id);

  if (context != NULL && pthread_mutex_lock(&context->mutex) == 0) {
    nxinput_sdl3_pm_cache_entry *entry =
        nxinput_sdl3_pm_find_entry(context, instance_id);
    if (entry != NULL && entry->stable) {
      entry->receipt.open_observed = 1u;
      entry->receipt.open_result = gamepad != NULL ? 1u : 0u;
    }
    (void)pthread_mutex_unlock(&context->mutex);
  }
  return gamepad;
}

int nxinput_sdl3_pm_record_consumer_delivery(
    nxinput_sdl3_pm_context *context, SDL_JoystickID instance_id,
    unsigned int button_index, bool pressed,
    nxinput_sdl3_pm_receipt *receipt) {
  if (context == NULL ||
      button_index >= NXINPUT_SDL3_PM_CONSUMER_BUTTON_MAX) {
    errno = EINVAL;
    return -1;
  }
  if (pthread_mutex_lock(&context->mutex) != 0) {
    errno = EBUSY;
    return -1;
  }
  nxinput_sdl3_pm_cache_entry *entry =
      nxinput_sdl3_pm_find_entry(context, instance_id);
  /* Only a state delivered after a real successful SDL_OpenGamepad can be a
   * consumer receipt. Parser, dispatcher or pre-open adapter observations do
   * not satisfy this boundary. */
  if (entry == NULL || !entry->stable || !entry->receipt.open_observed ||
      !entry->receipt.open_result) {
    (void)pthread_mutex_unlock(&context->mutex);
    errno = EPERM;
    return -1;
  }
  uint32_t bit = UINT32_C(1) << button_index;
  ++entry->receipt.consumer_delivery_count;
  if (pressed) {
    entry->receipt.consumer_pressed_mask |= bit;
  } else {
    entry->receipt.consumer_released_mask |= bit;
  }
  if (receipt != NULL) {
    *receipt = entry->receipt;
  }
  (void)pthread_mutex_unlock(&context->mutex);
  return 0;
}

int nxinput_sdl3_pm_get_receipt(nxinput_sdl3_pm_context *context,
                                SDL_JoystickID instance_id,
                                nxinput_sdl3_pm_receipt *receipt) {
  if (context == NULL || receipt == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (pthread_mutex_lock(&context->mutex) != 0) {
    errno = EBUSY;
    return -1;
  }
  nxinput_sdl3_pm_cache_entry *entry =
      nxinput_sdl3_pm_find_entry(context, instance_id);
  if (entry == NULL || !entry->stable) {
    (void)pthread_mutex_unlock(&context->mutex);
    errno = ENOENT;
    return -1;
  }
  *receipt = entry->receipt;
  (void)pthread_mutex_unlock(&context->mutex);
  return 0;
}

#endif /* NXINPUT_SDL3_PM_CORE_ONLY */
