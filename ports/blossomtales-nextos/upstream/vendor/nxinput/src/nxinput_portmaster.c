/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxinput_portmaster.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/input.h>

#define PM_BITS_PER_LONG (8u * sizeof(unsigned long))
#define PM_ABS_WORDS \
  ((NXINPUT_GODOT_ABS_BITS + PM_BITS_PER_LONG - 1u) / PM_BITS_PER_LONG)

typedef struct pm_output {
  char *data;
  size_t capacity;
  size_t length;
} pm_output;

static int pm_guid_valid(const char *guid) {
  size_t index;
  if (guid == NULL || strlen(guid) != 32u) {
    return 0;
  }
  for (index = 0u; index < 32u; ++index) {
    if (!isdigit((unsigned char)guid[index]) &&
        (guid[index] < 'a' || guid[index] > 'f')) {
      return 0;
    }
  }
  return 1;
}

static int pm_guid_prefix_equal(const char *mapping, const char *guid) {
  if (mapping == NULL || guid == NULL || mapping[32] != ',') {
    return 0;
  }
  return memcmp(mapping, guid, 32u) == 0;
}

static int pm_bit_set(const unsigned long *bits, size_t bit_count,
                      unsigned int code) {
  return bits != NULL && (size_t)code < bit_count &&
         ((bits[code / PM_BITS_PER_LONG] >>
           (code % PM_BITS_PER_LONG)) & 1ul) != 0ul;
}

static unsigned int pm_count_keys(const unsigned long *bits,
                                  size_t bit_count, unsigned int first,
                                  unsigned int limit) {
  unsigned int count = 0u;
  if ((size_t)limit > bit_count) {
    limit = bit_count > UINT_MAX ? UINT_MAX : (unsigned int)bit_count;
  }
  for (unsigned int code = first; code < limit; ++code) {
    if (pm_bit_set(bits, bit_count, code)) {
      ++count;
    }
  }
  return count;
}

/* The legacy joydev dialect ranks every EV_KEY capability, including KEY_ESC
 * and the volume keys, in plain ascending code order. */
static int pm_legacy_code(const unsigned long *bits, size_t bit_count,
                          unsigned int ordinal) {
  unsigned int rank = 0u;
  unsigned int limit = bit_count < (size_t)KEY_MAX + 1u
                           ? (unsigned int)bit_count
                           : (unsigned int)KEY_MAX + 1u;
  for (unsigned int code = 0u; code < limit; ++code) {
    if (!pm_bit_set(bits, bit_count, code)) {
      continue;
    }
    if (rank == ordinal) {
      return (int)code;
    }
    ++rank;
  }
  return -1;
}

/* ------------------------------------------------------------------ */
/* 0.10.0 -- semantic classes of the SDL mapping vocabulary.           */
/* The class belongs to the SEMANTIC (the mapping key), never to a     */
/* device name, CFW, GUID or VID/PID.                                  */
/* ------------------------------------------------------------------ */

typedef enum pm_semantic_class {
  PM_SEM_UNCONSTRAINED = 0, /* unknown/metadata key: reachability only */
  PM_SEM_GAMEPAD,           /* must land on a BTN_* button-class code */
  PM_SEM_VOLUME_DOWN,       /* must land exactly on KEY_VOLUMEDOWN */
  PM_SEM_VOLUME_UP          /* must land exactly on KEY_VOLUMEUP */
} pm_semantic_class;

static pm_semantic_class pm_semantic_of(const char *key, size_t key_length) {
  static const char *const gamepad_keys[] = {
      "a",       "b",        "x",          "y",         "back",
      "guide",   "start",    "leftstick",  "rightstick", "leftshoulder",
      "rightshoulder", "dpup", "dpdown",   "dpleft",    "dpright",
      "lefttrigger", "righttrigger", "misc1", "misc2",  "misc3",
      "misc4",   "misc5",    "misc6",      "paddle1",   "paddle2",
      "paddle3", "paddle4",  "touchpad",
  };
  size_t i;

  if (key == NULL || key_length == 0u) {
    return PM_SEM_UNCONSTRAINED;
  }
  if (key_length == 10u && memcmp(key, "volumedown", 10u) == 0) {
    return PM_SEM_VOLUME_DOWN;
  }
  if (key_length == 8u && memcmp(key, "volumeup", 8u) == 0) {
    return PM_SEM_VOLUME_UP;
  }
  for (i = 0u; i < sizeof gamepad_keys / sizeof gamepad_keys[0]; ++i) {
    if (key_length == strlen(gamepad_keys[i]) &&
        memcmp(key, gamepad_keys[i], key_length) == 0) {
      return PM_SEM_GAMEPAD;
    }
  }
  return PM_SEM_UNCONSTRAINED;
}

/* Is this evdev code in one of the button (BTN_*) ranges the input
 * subsystem defines? KEY_ESC or the volume keys are provably NOT buttons. */
static int pm_code_is_button_class(int code) {
  return (code >= BTN_MISC && code <= BTN_GEAR_UP) ||
         (code >= BTN_DPAD_UP && code <= BTN_DPAD_RIGHT) ||
         (code >= BTN_TRIGGER_HAPPY && code <= BTN_TRIGGER_HAPPY40);
}

static int pm_semantic_accepts(pm_semantic_class semantic, int code) {
  switch (semantic) {
    case PM_SEM_GAMEPAD:
      return pm_code_is_button_class(code);
    case PM_SEM_VOLUME_DOWN:
      return code == KEY_VOLUMEDOWN;
    case PM_SEM_VOLUME_UP:
      return code == KEY_VOLUMEUP;
    case PM_SEM_UNCONSTRAINED:
    default:
      return code >= 0;
  }
}

/* Iterate the `key:bN` bindings of one mapping line. Returns 1 and fills the
 * cursor state while a binding exists. Fields that are not simple button
 * bindings (axes, hats, metadata, malformed values) are skipped here; the
 * sovereign syntax layer owns their verdict. */
typedef struct pm_binding_cursor {
  const char *field;        /* current scan position */
  unsigned int field_index; /* 0 = GUID, 1 = name */
  const char *key;
  size_t key_length;
  unsigned int ordinal;
} pm_binding_cursor;

static void pm_binding_cursor_init(pm_binding_cursor *cursor,
                                   const char *mapping) {
  cursor->field = mapping;
  cursor->field_index = 0u;
  cursor->key = NULL;
  cursor->key_length = 0u;
  cursor->ordinal = 0u;
}

static int pm_binding_next(pm_binding_cursor *cursor) {
  while (cursor->field != NULL && *cursor->field != '\0') {
    const char *field = cursor->field;
    const char *end = strchr(field, ',');
    const char *colon;

    if (end == NULL) {
      end = field + strlen(field);
    }
    colon = memchr(field, ':', (size_t)(end - field));
    cursor->field = *end == ',' ? end + 1 : end;
    cursor->field_index++;
    if (cursor->field_index - 1u < 2u || colon == NULL || colon == field ||
        colon + 2 > end || colon[1] != 'b' ||
        !isdigit((unsigned char)colon[2])) {
      continue;
    }
    {
      char *number_end = NULL;
      unsigned long value;

      errno = 0;
      value = strtoul(colon + 2, &number_end, 10);
      if (errno != 0 || number_end != end || value > UINT_MAX) {
        continue; /* malformed value: the sovereign syntax layer decides */
      }
      cursor->key = field;
      cursor->key_length = (size_t)(colon - field);
      cursor->ordinal = (unsigned int)value;
      return 1;
    }
  }
  return 0;
}

/* One domain interpretation of one line: coherent when every button binding
 * resolves to a code that its semantic class accepts. */
typedef struct pm_interpretation {
  int coherent;
  unsigned int bindings;
} pm_interpretation;

typedef int (*pm_code_fn)(const void *state, unsigned int ordinal);

struct pm_joydev_state {
  const unsigned long *bits;
  size_t bit_count;
};

static int pm_joydev_code_of(const void *state, unsigned int ordinal) {
  const struct pm_joydev_state *s = (const struct pm_joydev_state *)state;
  return pm_legacy_code(s->bits, s->bit_count, ordinal);
}

struct pm_evdev_state {
  nxinput_sdl_domain domain;
  const nxinput_godot_caps *caps;
};

static int pm_evdev_code_of(const void *state, unsigned int ordinal) {
  const struct pm_evdev_state *s = (const struct pm_evdev_state *)state;
  return nxinput_sdl_button_code(s->domain, s->caps, ordinal);
}

static void pm_interpret(const char *mapping, pm_code_fn code_of,
                         const void *state, pm_interpretation *out) {
  pm_binding_cursor cursor;

  out->coherent = 1;
  out->bindings = 0u;
  pm_binding_cursor_init(&cursor, mapping);
  while (pm_binding_next(&cursor)) {
    pm_semantic_class semantic =
        pm_semantic_of(cursor.key, cursor.key_length);
    int code = code_of(state, cursor.ordinal);

    out->bindings++;
    if (code < 0 || !pm_semantic_accepts(semantic, code)) {
      out->coherent = 0;
    }
  }
}

static int pm_codes_identical(const char *mapping, pm_code_fn a_of,
                              const void *a_state, pm_code_fn b_of,
                              const void *b_state) {
  pm_binding_cursor cursor;

  pm_binding_cursor_init(&cursor, mapping);
  while (pm_binding_next(&cursor)) {
    if (a_of(a_state, cursor.ordinal) != b_of(b_state, cursor.ordinal)) {
      return 0;
    }
  }
  return 1;
}

static void pm_caps_from_key_bits(nxinput_godot_caps *caps,
                                  const unsigned long *key_bits,
                                  size_t key_bit_count,
                                  const unsigned long *empty_abs) {
  /* Keep this representation-only module link-independent from the Godot
   * parser library. nxinput_sdl consumes the public measured-capabilities
   * layout directly; no parser or engine policy is needed here. */
  memset(caps, 0, sizeof *caps);
  caps->api_version = NXINPUT_GODOT_API_VERSION;
  caps->struct_size = sizeof *caps;
  caps->key_bits = key_bits;
  caps->key_bit_count = key_bit_count;
  caps->abs_bits = empty_abs;
  caps->abs_bit_count = NXINPUT_GODOT_ABS_BITS;
}

static int pm_find_single_button(const char *mapping, const char *semantic,
                                 unsigned int *ordinal) {
  const size_t semantic_length = semantic != NULL ? strlen(semantic) : 0u;
  const char *field = mapping;
  unsigned int field_index = 0u;
  int found = 0;

  if (mapping == NULL || semantic_length == 0u || ordinal == NULL) {
    return 0;
  }
  while (*field != '\0') {
    const char *end = strchr(field, ',');
    const char *colon;
    char *number_end = NULL;
    unsigned long value;
    if (end == NULL) {
      end = field + strlen(field);
    }
    colon = memchr(field, ':', (size_t)(end - field));
    if (field_index >= 2u && colon != NULL &&
        (size_t)(colon - field) == semantic_length &&
        memcmp(field, semantic, semantic_length) == 0) {
      if (found || colon + 2 > end || colon[1] != 'b' ||
          !isdigit((unsigned char)colon[2])) {
        errno = EINVAL;
        return -1;
      }
      errno = 0;
      value = strtoul(colon + 2, &number_end, 10);
      if (errno != 0 || number_end != end || value > UINT_MAX) {
        errno = EINVAL;
        return -1;
      }
      *ordinal = (unsigned int)value;
      found = 1;
    }
    field = *end == ',' ? end + 1 : end;
    ++field_index;
  }
  return found;
}

static int pm_append(pm_output *output, const char *data, size_t length) {
  if (output == NULL || data == NULL || output->length >= output->capacity ||
      length >= output->capacity - output->length) {
    return 0;
  }
  memcpy(output->data + output->length, data, length);
  output->length += length;
  output->data[output->length] = '\0';
  return 1;
}

static int pm_append_ordinal(pm_output *output, unsigned int ordinal) {
  char number[16];
  int length = snprintf(number, sizeof number, "%u", ordinal);
  return length > 0 && (size_t)length < sizeof number &&
         pm_append(output, number, (size_t)length);
}

const char *nxinput_pm_domain_class_name(nxinput_pm_domain_class value) {
  switch (value) {
    case NXINPUT_PM_CLASS_CURRENT_NATIVE:
      return "current-native";
    case NXINPUT_PM_CLASS_LEGACY_JOYDEV_REWRITE:
      return "legacy-joydev-rewrite";
    case NXINPUT_PM_CLASS_IDENTICAL_IN_BOTH:
      return "identical-in-both";
    case NXINPUT_PM_CLASS_AMBIGUOUS:
      return "ambiguous";
    case NXINPUT_PM_CLASS_INVALID:
    default:
      return "invalid";
  }
}

/* Structural evidence about the pad, common to classify and convert. */
static void pm_fill_capability_evidence(nxinput_pm_evidence *local,
                                        const unsigned long *key_bits,
                                        size_t key_bit_count) {
  local->gamepad_buttons = pm_count_keys(key_bits, key_bit_count,
                                         BTN_JOYSTICK, KEY_MAX + 1u);
  local->lower_key_buttons = pm_count_keys(key_bits, key_bit_count, 0u,
                                           BTN_JOYSTICK);
  local->key_buttons = local->gamepad_buttons + local->lower_key_buttons;
}

nxinput_pm_domain_class nxinput_pm_classify_mapping(
    const char *mapping, const unsigned long *key_bits, size_t key_bit_count,
    nxinput_sdl_domain target_domain, nxinput_pm_evidence *evidence) {
  unsigned long empty_abs[PM_ABS_WORDS] = {0};
  nxinput_godot_caps caps;
  nxinput_pm_evidence local = {0};
  struct pm_joydev_state joydev_state;
  struct pm_evdev_state evdev_state;
  pm_interpretation joydev;
  pm_interpretation evdev;
  unsigned int volume_down = 0u;
  unsigned int volume_up = 0u;
  nxinput_pm_domain_class verdict;

  if (mapping == NULL || key_bits == NULL ||
      (target_domain != NXINPUT_SDL_DOMAIN_SDL2_EVDEV &&
       target_domain != NXINPUT_SDL_DOMAIN_SDL3_EVDEV)) {
    if (evidence != NULL) {
      memset(evidence, 0, sizeof *evidence);
      evidence->domain_class = (unsigned int)NXINPUT_PM_CLASS_INVALID;
    }
    return NXINPUT_PM_CLASS_INVALID;
  }
  pm_fill_capability_evidence(&local, key_bits, key_bit_count);
  pm_caps_from_key_bits(&caps, key_bits, key_bit_count, empty_abs);

  joydev_state.bits = key_bits;
  joydev_state.bit_count = key_bit_count;
  evdev_state.domain = target_domain;
  evdev_state.caps = &caps;
  pm_interpret(mapping, pm_joydev_code_of, &joydev_state, &joydev);
  pm_interpret(mapping, pm_evdev_code_of, &evdev_state, &evdev);
  local.button_bindings = joydev.bindings;
  if (pm_find_single_button(mapping, "volumedown", &volume_down) > 0 &&
      pm_find_single_button(mapping, "volumeup", &volume_up) > 0) {
    /* Volume markers stay strong POSITIVE evidence for the legacy dialect
     * when both keys really rank on the volume codes -- but presence alone
     * never decides; the class check above already consumed them. */
    if (pm_legacy_code(key_bits, key_bit_count, volume_down) ==
            KEY_VOLUMEDOWN &&
        pm_legacy_code(key_bits, key_bit_count, volume_up) == KEY_VOLUMEUP) {
      local.legacy_volume_markers = 2u;
    }
  }

  if (joydev.coherent && evdev.coherent) {
    if (pm_codes_identical(mapping, pm_joydev_code_of, &joydev_state,
                           pm_evdev_code_of, &evdev_state)) {
      verdict = NXINPUT_PM_CLASS_IDENTICAL_IN_BOTH;
    } else {
      /* Never NOT_APPLICABLE: silently passing the original line would let
       * the wrong domain through. The source must yield. */
      verdict = NXINPUT_PM_CLASS_AMBIGUOUS;
    }
  } else if (joydev.coherent) {
    verdict = NXINPUT_PM_CLASS_LEGACY_JOYDEV_REWRITE;
  } else if (evdev.coherent) {
    verdict = NXINPUT_PM_CLASS_CURRENT_NATIVE;
  } else {
    verdict = NXINPUT_PM_CLASS_INVALID;
  }
  local.domain_class = (unsigned int)verdict;
  if (evidence != NULL) {
    *evidence = local;
  }
  return verdict;
}

/* Pure ordinal rewrite of a line PROVEN legacy: every bN becomes the ordinal
 * of the same evdev code in the target domain. Never called on an unproven
 * line. */
static int pm_rewrite_line(const char *mapping, const unsigned long *key_bits,
                           size_t key_bit_count,
                           nxinput_sdl_domain target_domain,
                           const nxinput_godot_caps *caps, pm_output *writer,
                           nxinput_pm_evidence *local) {
  const char *first_comma = strchr(mapping, ',');
  const char *field;
  unsigned int field_index = 1u;

  if (first_comma == NULL ||
      !pm_append(writer, mapping, (size_t)(first_comma - mapping)) ||
      !pm_append(writer, first_comma, 1u)) {
    errno = ENOSPC;
    return NXINPUT_PM_ERROR;
  }
  field = first_comma + 1;
  while (*field != '\0') {
    const char *end = strchr(field, ',');
    const char *colon;
    int rewritten = 0;
    if (end == NULL) {
      end = field + strlen(field);
    }
    colon = memchr(field, ':', (size_t)(end - field));
    if (field_index >= 2u && colon != NULL && colon + 2 <= end &&
        colon[1] == 'b' && isdigit((unsigned char)colon[2])) {
      char *number_end = NULL;
      unsigned long old_ordinal;
      errno = 0;
      old_ordinal = strtoul(colon + 2, &number_end, 10);
      if (errno == 0 && number_end == end && old_ordinal <= UINT_MAX) {
        int code = pm_legacy_code(key_bits, key_bit_count,
                                  (unsigned int)old_ordinal);
        int new_ordinal = code < 0
                              ? -1
                              : nxinput_sdl_button_ordinal(
                                    target_domain, caps,
                                    (unsigned int)code);
        if (new_ordinal < 0) {
          errno = ERANGE;
          return NXINPUT_PM_ERROR;
        }
        if (!pm_append(writer, field, (size_t)(colon + 2 - field)) ||
            !pm_append_ordinal(writer, (unsigned int)new_ordinal)) {
          errno = ENOSPC;
          return NXINPUT_PM_ERROR;
        }
        ++local->button_bindings;
        if ((unsigned int)new_ordinal != (unsigned int)old_ordinal) {
          ++local->rewritten_bindings;
        }
        rewritten = 1;
      }
    }
    if (!rewritten &&
        !pm_append(writer, field, (size_t)(end - field))) {
      errno = ENOSPC;
      return NXINPUT_PM_ERROR;
    }
    if (*end == ',' && !pm_append(writer, end, 1u)) {
      errno = ENOSPC;
      return NXINPUT_PM_ERROR;
    }
    field = *end == ',' ? end + 1 : end;
    ++field_index;
  }
  return NXINPUT_PM_REWRITTEN;
}

int nxinput_pm_convert_joydev_mapping(
    const char *mapping, const unsigned long *key_bits, size_t key_bit_count,
    nxinput_sdl_domain target_domain, const char *target_guid, char *output,
    size_t output_size, nxinput_pm_evidence *evidence) {
  unsigned long empty_abs[PM_ABS_WORDS] = {0};
  nxinput_godot_caps caps;
  nxinput_pm_evidence local = {0};
  nxinput_pm_evidence classify_evidence = {0};
  pm_output writer = {output, output_size, 0u};
  nxinput_pm_domain_class verdict;
  const char *first_comma;
  int result;

  if (evidence != NULL) {
    memset(evidence, 0, sizeof *evidence);
  }
  if (mapping == NULL || key_bits == NULL || output == NULL ||
      output_size == 0u || output == mapping ||
      !pm_guid_valid(target_guid) ||
      (target_domain != NXINPUT_SDL_DOMAIN_SDL2_EVDEV &&
       target_domain != NXINPUT_SDL_DOMAIN_SDL3_EVDEV)) {
    errno = EINVAL;
    return NXINPUT_PM_ERROR;
  }
  output[0] = '\0';

  first_comma = strchr(mapping, ',');
  if (first_comma == NULL || (size_t)(first_comma - mapping) != 32u ||
      !pm_guid_prefix_equal(mapping, target_guid) ||
      strchr(first_comma + 1, ',') == NULL || strchr(mapping, '\n') != NULL ||
      strchr(mapping, '\r') != NULL) {
    errno = EINVAL;
    return NXINPUT_PM_ERROR;
  }

  verdict = nxinput_pm_classify_mapping(mapping, key_bits, key_bit_count,
                                        target_domain, &classify_evidence);
  pm_fill_capability_evidence(&local, key_bits, key_bit_count);
  local.legacy_volume_markers = classify_evidence.legacy_volume_markers;
  local.domain_class = (unsigned int)verdict;

  switch (verdict) {
    case NXINPUT_PM_CLASS_CURRENT_NATIVE:
    case NXINPUT_PM_CLASS_IDENTICAL_IN_BOTH:
      /* Byte-intact by proof; nothing to rewrite. */
      local.button_bindings = classify_evidence.button_bindings;
      if (evidence != NULL) {
        *evidence = local;
      }
      return NXINPUT_PM_NOT_APPLICABLE;
    case NXINPUT_PM_CLASS_AMBIGUOUS:
    case NXINPUT_PM_CLASS_INVALID:
      /* The source must yield. Ambiguity is never a silent pass-through. */
      if (evidence != NULL) {
        *evidence = local;
      }
      return NXINPUT_PM_SOURCE_YIELDS;
    case NXINPUT_PM_CLASS_LEGACY_JOYDEV_REWRITE:
      break;
  }

  pm_caps_from_key_bits(&caps, key_bits, key_bit_count, empty_abs);
  result = pm_rewrite_line(mapping, key_bits, key_bit_count, target_domain,
                           &caps, &writer, &local);
  if (result != NXINPUT_PM_REWRITTEN) {
    return result;
  }
  if (local.rewritten_bindings == 0u) {
    /* A proven-legacy line whose ordinals all coincide is byte-equal to its
     * projection; report it as untouched rather than pretending a rewrite. */
    output[0] = '\0';
    if (evidence != NULL) {
      *evidence = local;
    }
    return NXINPUT_PM_NOT_APPLICABLE;
  }
  if (evidence != NULL) {
    *evidence = local;
  }
  return NXINPUT_PM_REWRITTEN;
}

int nxinput_pm_normalize_source(
    const char *source, const unsigned long *key_bits, size_t key_bit_count,
    nxinput_sdl_domain target_domain, const char *target_guid, char *output,
    size_t output_size, nxinput_pm_source_evidence *evidence) {
  nxinput_pm_source_evidence local = {0};
  pm_output writer = {output, output_size, 0u};
  const char *cursor;

  if (evidence != NULL) {
    memset(evidence, 0, sizeof *evidence);
  }
  if (source == NULL || key_bits == NULL || output == NULL ||
      output_size == 0u || output == source || !pm_guid_valid(target_guid)) {
    errno = EINVAL;
    return NXINPUT_PM_ERROR;
  }
  output[0] = '\0';
  cursor = source;
  while (*cursor != '\0') {
    const char *eol = strchr(cursor, '\n');
    size_t length = eol != NULL ? (size_t)(eol - cursor) : strlen(cursor);
    size_t trimmed = length;
    int handled = 0;

    while (trimmed > 0u &&
           (cursor[trimmed - 1u] == '\r' || cursor[trimmed - 1u] == ' ' ||
            cursor[trimmed - 1u] == '\t')) {
      --trimmed;
    }
    if (trimmed > 33u && pm_guid_prefix_equal(cursor, target_guid)) {
      char input[NXINPUT_PM_MAPPING_MAX];
      char converted[NXINPUT_PM_MAPPING_MAX];
      nxinput_pm_evidence line_evidence;
      int result;
      ++local.matching_lines;
      if (trimmed >= sizeof input) {
        errno = E2BIG;
        return NXINPUT_PM_ERROR;
      }
      memcpy(input, cursor, trimmed);
      input[trimmed] = '\0';
      result = nxinput_pm_convert_joydev_mapping(
          input, key_bits, key_bit_count, target_domain, target_guid,
          converted, sizeof converted, &line_evidence);
      if (result == NXINPUT_PM_ERROR) {
        return NXINPUT_PM_ERROR;
      }
      if (result == NXINPUT_PM_SOURCE_YIELDS) {
        /* One ambiguous or invalid target line poisons the WHOLE source:
         * the caller must fall through to the next authority instead of
         * admitting any part of it in an unproved domain. */
        if (line_evidence.domain_class ==
            (unsigned int)NXINPUT_PM_CLASS_AMBIGUOUS) {
          ++local.ambiguous_lines;
        } else {
          ++local.invalid_lines;
        }
        output[0] = '\0';
        if (evidence != NULL) {
          local.legacy_volume_markers += line_evidence.legacy_volume_markers;
          *evidence = local;
        }
        return NXINPUT_PM_SOURCE_YIELDS;
      }
      if (result == NXINPUT_PM_REWRITTEN) {
        if (!pm_append(&writer, converted, strlen(converted)) ||
            !pm_append(&writer, cursor + trimmed, length - trimmed)) {
          errno = ENOSPC;
          return NXINPUT_PM_ERROR;
        }
        ++local.rewritten_lines;
        local.rewritten_bindings += line_evidence.rewritten_bindings;
        local.legacy_volume_markers +=
            line_evidence.legacy_volume_markers;
        handled = 1;
      } else {
        /* NOT_APPLICABLE: byte-intact, and now positively classified. */
        if (line_evidence.domain_class ==
            (unsigned int)NXINPUT_PM_CLASS_IDENTICAL_IN_BOTH) {
          ++local.identical_lines;
        } else if (line_evidence.domain_class ==
                   (unsigned int)NXINPUT_PM_CLASS_CURRENT_NATIVE) {
          ++local.native_lines;
        } else {
          /* proven legacy whose projection is byte-identical */
          ++local.identical_lines;
        }
        local.legacy_volume_markers += line_evidence.legacy_volume_markers;
      }
    }
    if (!handled && !pm_append(&writer, cursor, length)) {
      errno = ENOSPC;
      return NXINPUT_PM_ERROR;
    }
    if (eol != NULL && !pm_append(&writer, eol, 1u)) {
      errno = ENOSPC;
      return NXINPUT_PM_ERROR;
    }
    cursor = eol != NULL ? eol + 1 : cursor + length;
  }
  if (evidence != NULL) {
    *evidence = local;
  }
  return local.rewritten_lines > 0u ? NXINPUT_PM_REWRITTEN
                                    : NXINPUT_PM_NOT_APPLICABLE;
}
