/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_godot -- see include/nxinput_godot.h. Pure. */
#include "nxinput_godot.h"

#include <stdio.h>
#include <string.h>

#define BITS_PER_LONG (8u * (unsigned int)sizeof(unsigned long))

static int bit_set(const unsigned long *bits, size_t bit_count,
                   unsigned int code) {
  if (bits == 0 || (size_t)code >= bit_count) {
    return 0;
  }
  return (int)((bits[code / BITS_PER_LONG] >> (code % BITS_PER_LONG)) & 1ul);
}

static unsigned int count_range(const unsigned long *bits, size_t bit_count,
                                unsigned int first, unsigned int limit) {
  unsigned int code;
  unsigned int total = 0u;

  if ((size_t)limit > bit_count) {
    limit = (unsigned int)bit_count;
  }
  for (code = first; code < limit; code++) {
    if (bit_set(bits, bit_count, code)) {
      total++;
    }
  }
  return total;
}

static int is_hat_code(unsigned int code) {
  return code >= NXINPUT_GODOT_ABS_HAT0X && code <= NXINPUT_GODOT_ABS_HAT3Y;
}

/* ------------------------------------------------------------- domains */
struct scan_range {
  unsigned int first;
  unsigned int limit;
};

struct domain_plan {
  struct scan_range button[2];
  unsigned int button_ranges;
  struct scan_range axis;      /* hats are skipped inside the scan */
};

/* Transcribed from the pinned upstream sources; re-verified against those
 * exact commits by tests/godot_domain_gate.py. */
static const struct domain_plan domain_plans[NXINPUT_GODOT_DOMAIN_COUNT] = {
    /* UNDECLARED */
    {{{0u, 0u}, {0u, 0u}}, 0u, {0u, 0u}},
    /* GODOT */
    {{{NXINPUT_GODOT_BTN_JOYSTICK, NXINPUT_GODOT_KEY_MAX},
      {NXINPUT_GODOT_BTN_MISC, NXINPUT_GODOT_BTN_JOYSTICK}},
     2u,
     {0u, NXINPUT_GODOT_ABS_MISC}},
    /* SDL2_EVDEV */
    {{{NXINPUT_GODOT_BTN_JOYSTICK, NXINPUT_GODOT_KEY_MAX},
      {0u, NXINPUT_GODOT_BTN_JOYSTICK}},
     2u,
     {0u, NXINPUT_GODOT_ABS_MAX}},
};

static int domain_valid(nxinput_godot_domain domain) {
  return (int)domain > (int)NXINPUT_GODOT_DOMAIN_UNDECLARED &&
         (int)domain < (int)NXINPUT_GODOT_DOMAIN_COUNT;
}

/* Both layouts are accepted: the API-2 struct simply carries no absinfo,
 * which BLOCKS axis bindings rather than silently admitting them. */
static int caps_valid(const nxinput_godot_caps *caps) {
  return caps != 0 &&
         (caps->api_version == NXINPUT_GODOT_API_VERSION ||
          caps->api_version == 2u) &&
         (caps->struct_size == sizeof(*caps) ||
          caps->struct_size == NXINPUT_GODOT_CAPS_SIZE_V2) &&
         caps->key_bits != 0 && caps->abs_bits != 0;
}

static const nxinput_godot_absinfo *caps_absinfo(
    const nxinput_godot_caps *caps, unsigned int code) {
  if (caps == 0 || caps->struct_size != sizeof(*caps) ||
      caps->abs_info == 0 || (size_t)code >= caps->abs_info_count) {
    return 0;
  }
  return caps->abs_info[code].present ? &caps->abs_info[code] : 0;
}

static int caps_has_absinfo(const nxinput_godot_caps *caps) {
  return caps != 0 && caps->struct_size == sizeof(*caps) &&
         caps->abs_info != 0 && caps->abs_info_count > 0u;
}

int nxinput_godot_button_code(nxinput_godot_domain domain,
                              const nxinput_godot_caps *caps,
                              unsigned int ordinal) {
  const struct domain_plan *plan;
  unsigned int rank = 0u;
  unsigned int r;

  if (!domain_valid(domain) || !caps_valid(caps)) {
    return -1;
  }
  plan = &domain_plans[domain];
  for (r = 0u; r < plan->button_ranges; r++) {
    unsigned int code;
    unsigned int limit = plan->button[r].limit;

    if ((size_t)limit > caps->key_bit_count) {
      limit = (unsigned int)caps->key_bit_count;
    }
    for (code = plan->button[r].first; code < limit; code++) {
      if (!bit_set(caps->key_bits, caps->key_bit_count, code)) {
        continue;
      }
      if (rank == ordinal) {
        return (int)code;
      }
      rank++;
    }
  }
  return -1;
}

int nxinput_godot_button_ordinal(nxinput_godot_domain domain,
                                 const nxinput_godot_caps *caps,
                                 unsigned int code) {
  const struct domain_plan *plan;
  unsigned int rank = 0u;
  unsigned int r;

  if (!domain_valid(domain) || !caps_valid(caps) ||
      !bit_set(caps->key_bits, caps->key_bit_count, code)) {
    return -1;
  }
  plan = &domain_plans[domain];
  for (r = 0u; r < plan->button_ranges; r++) {
    unsigned int limit = plan->button[r].limit;

    if ((size_t)limit > caps->key_bit_count) {
      limit = (unsigned int)caps->key_bit_count;
    }
    if (code >= plan->button[r].first && code < limit) {
      return (int)(rank + count_range(caps->key_bits, caps->key_bit_count,
                                      plan->button[r].first, code));
    }
    rank += count_range(caps->key_bits, caps->key_bit_count,
                        plan->button[r].first, limit);
  }
  return -1; /* the domain cannot name this code */
}

/* Axes: both domains scan ascending from 0 and skip the hat pairs; they
 * differ only in where they stop (ABS_MISC vs ABS_MAX). */
int nxinput_godot_axis_code(nxinput_godot_domain domain,
                            const nxinput_godot_caps *caps,
                            unsigned int ordinal) {
  unsigned int rank = 0u;
  unsigned int code;
  unsigned int limit;

  if (!domain_valid(domain) || !caps_valid(caps)) {
    return -1;
  }
  limit = domain_plans[domain].axis.limit;
  if ((size_t)limit > caps->abs_bit_count) {
    limit = (unsigned int)caps->abs_bit_count;
  }
  for (code = 0u; code < limit; code++) {
    if (is_hat_code(code) || !bit_set(caps->abs_bits, caps->abs_bit_count,
                                      code)) {
      continue;
    }
    if (rank == ordinal) {
      return (int)code;
    }
    rank++;
  }
  return -1;
}

int nxinput_godot_axis_ordinal(nxinput_godot_domain domain,
                               const nxinput_godot_caps *caps,
                               unsigned int code) {
  unsigned int rank = 0u;
  unsigned int scan;
  unsigned int limit;

  if (!domain_valid(domain) || !caps_valid(caps) || is_hat_code(code) ||
      !bit_set(caps->abs_bits, caps->abs_bit_count, code)) {
    return -1;
  }
  limit = domain_plans[domain].axis.limit;
  if ((size_t)limit > caps->abs_bit_count) {
    limit = (unsigned int)caps->abs_bit_count;
  }
  if (code >= limit) {
    return -1; /* outside this domain's axis window */
  }
  for (scan = 0u; scan < code; scan++) {
    if (!is_hat_code(scan) && bit_set(caps->abs_bits, caps->abs_bit_count,
                                      scan)) {
      rank++;
    }
  }
  return (int)rank;
}

/* --------------------------------------------------------------- init */
int nxinput_godot_origin_init(nxinput_godot_origin *origin) {
  if (origin == 0) {
    return -1;
  }
  memset(origin, 0, sizeof(*origin));
  origin->api_version = NXINPUT_GODOT_API_VERSION;
  origin->struct_size = sizeof(*origin);
  origin->domain = (uint8_t)NXINPUT_GODOT_DOMAIN_UNDECLARED;
  return 0;
}

int nxinput_godot_origin_declare(nxinput_godot_origin *origin,
                                 nxinput_godot_domain domain,
                                 const char *provider, const char *receipt) {
  if (origin == 0 || !domain_valid(domain) || provider == 0 ||
      provider[0] == '\0' || receipt == 0 || receipt[0] == '\0' ||
      strlen(provider) >= NXINPUT_GODOT_PROVIDER_MAX ||
      strlen(receipt) >= NXINPUT_GODOT_RECEIPT_MAX) {
    return -1;
  }
  (void)nxinput_godot_origin_init(origin);
  origin->domain = (uint8_t)domain;
  (void)snprintf(origin->provider, sizeof origin->provider, "%s", provider);
  (void)snprintf(origin->receipt, sizeof origin->receipt, "%s", receipt);
  return 0;
}

int nxinput_godot_caps_init(nxinput_godot_caps *caps,
                            const unsigned long *key_bits,
                            size_t key_bit_count,
                            const unsigned long *abs_bits,
                            size_t abs_bit_count) {
  if (caps == 0) {
    return -1;
  }
  memset(caps, 0, sizeof(*caps));
  caps->api_version = NXINPUT_GODOT_API_VERSION;
  caps->struct_size = sizeof(*caps);
  caps->key_bits = key_bits;
  caps->key_bit_count = key_bit_count;
  caps->abs_bits = abs_bits;
  caps->abs_bit_count = abs_bit_count;
  caps->abs_info = 0;
  caps->abs_info_count = 0u;
  return caps_valid(caps) ? 0 : -1;
}

int nxinput_godot_caps_set_absinfo(nxinput_godot_caps *caps,
                                   const nxinput_godot_absinfo *info,
                                   size_t info_count) {
  if (caps == 0 || caps->struct_size != sizeof(*caps) || info == 0 ||
      info_count == 0u || info_count > NXINPUT_GODOT_ABS_BITS) {
    return -1;
  }
  caps->abs_info = info;
  caps->abs_info_count = info_count;
  return 0;
}

int nxinput_godot_evidence_init(nxinput_godot_evidence *evidence) {
  if (evidence == 0) {
    return -1;
  }
  memset(evidence, 0, sizeof(*evidence));
  evidence->api_version = NXINPUT_GODOT_API_VERSION;
  evidence->struct_size = sizeof(*evidence);
  evidence->result = (uint8_t)NXINPUT_GODOT_INVALID;
  return 0;
}

const char *nxinput_godot_domain_name(nxinput_godot_domain domain) {
  switch (domain) {
    case NXINPUT_GODOT_DOMAIN_GODOT:
      return "godot";
    case NXINPUT_GODOT_DOMAIN_SDL2_EVDEV:
      return "sdl2-evdev";
    case NXINPUT_GODOT_DOMAIN_UNDECLARED:
    default:
      return "undeclared";
  }
}

const char *nxinput_godot_result_name(nxinput_godot_result result) {
  switch (result) {
    case NXINPUT_GODOT_BYTE_INTACT:
      return "byte-intact";
    case NXINPUT_GODOT_CONVERTED:
      return "converted";
    case NXINPUT_GODOT_ORIGIN_UNDECLARED:
      return "origin-undeclared-blocked";
    case NXINPUT_GODOT_UNREACHABLE:
      return "unreachable-blocked";
    case NXINPUT_GODOT_DUPLICATE:
      return "duplicate-blocked";
    case NXINPUT_GODOT_SYNTAX:
      return "syntax-blocked";
    case NXINPUT_GODOT_HAT:
      return "hat-blocked";
    case NXINPUT_GODOT_ABSINFO:
      return "absinfo-blocked";
    case NXINPUT_GODOT_INVALID:
    default:
      return "invalid";
  }
}

const char *nxinput_godot_engine_name(nxinput_godot_engine engine) {
  return engine == NXINPUT_GODOT_ENGINE_4 ? "godot4" : "godot3";
}

/* ------------------------------------------------------------- parsing */
enum binding_kind { BIND_BUTTON, BIND_AXIS, BIND_HAT, BIND_METADATA };

/* Why a field was refused. The reason is reported verbatim: an auditor must
 * be able to tell a bad hat mask from an out-of-range button. */
enum collect_status {
  COLLECT_OK = 0,
  COLLECT_TOO_MANY,
  COLLECT_EMPTY_FIELD,
  COLLECT_NO_COLON,
  COLLECT_EMPTY_VALUE,
  COLLECT_BAD_BUTTON,
  COLLECT_BAD_AXIS,
  COLLECT_BAD_HAT,
  COLLECT_BAD_HAT_MASK,
  COLLECT_TILDE_ON_NON_AXIS,
  COLLECT_UNKNOWN_FIELD,
  COLLECT_UNKNOWN_KEY,
  COLLECT_SIGN_ON_NON_AXIS
};

/* Metadata keys carry no binding. Anything NOT on this list must parse as a
 * binding: an unknown key with unknown syntax can no longer slip through. */
static int is_metadata_key(const char *name, size_t length) {
  static const char *const keys[] = {"platform", "crc",    "type",
                                     "hint",     "sdk",    "hidapi",
                                     "sdl2",     "sdl3"};
  size_t i;

  if (length > 0u && name[0] == '!') { /* SDL negates a hint with `!` */
    name++;
    length--;
  }
  for (i = 0u; i < sizeof keys / sizeof keys[0]; i++) {
    if (strlen(keys[i]) == length && memcmp(keys[i], name, length) == 0) {
      return 1;
    }
  }
  return 0;
}

/* The control names the pinned SDL2 line defines. An unknown key used to be
 * accepted as long as its value happened to parse; it now blocks, so a typo
 * cannot silently bind nothing. A leading `+`/`-` selects one half of an
 * analog output and is part of the grammar. */
static int is_control_key(const char *name, size_t length) {
  static const char *const names[] = {
      "a",           "b",            "x",             "y",
      "back",        "guide",        "start",         "leftstick",
      "rightstick",  "leftshoulder", "rightshoulder", "dpup",
      "dpdown",      "dpleft",       "dpright",       "misc1",
      "paddle1",     "paddle2",      "paddle3",       "paddle4",
      "touchpad",    "leftx",        "lefty",         "rightx",
      "righty",      "lefttrigger",  "righttrigger"};
  size_t i;

  if (length > 0u && (name[0] == '+' || name[0] == '-')) {
    name++;
    length--;
  }
  for (i = 0u; i < sizeof names / sizeof names[0]; i++) {
    if (strlen(names[i]) == length && memcmp(names[i], name, length) == 0) {
      return 1;
    }
  }
  return 0;
}

static int hat_mask_valid(unsigned int mask) {
  return mask == NXINPUT_GODOT_HAT_UP || mask == NXINPUT_GODOT_HAT_RIGHT ||
         mask == NXINPUT_GODOT_HAT_DOWN || mask == NXINPUT_GODOT_HAT_LEFT;
}

struct binding {
  const char *field;
  size_t field_length;
  const char *name;
  size_t name_length;
  const char *digits;      /* first digit of the ordinal */
  size_t digits_length;
  unsigned int ordinal;
  unsigned int hat_mask;   /* BIND_HAT only */
  int half;                /* -1, 0 or +1 for a half-range axis binding */
  int inverted;            /* `~` seen on an axis binding */
  enum binding_kind kind;
  int keep;                /* 0 once the engine cannot name it */
};

static int parse_uint(const char *text, size_t length, unsigned int *out) {
  unsigned int value = 0u;
  size_t i;

  if (length == 0u || length > 4u) {
    return 0;
  }
  for (i = 0u; i < length; i++) {
    if (text[i] < '0' || text[i] > '9') {
      return 0;
    }
    value = value * 10u + (unsigned int)(text[i] - '0');
  }
  *out = value;
  return 1;
}

/* Collect every field of the line and CLOSE the grammar. A field is either a
 * declared metadata key or a binding that parses completely; there is no
 * third outcome any more, so nothing unparsed can reach the setter. */
static enum collect_status collect(const char *mapping, struct binding *out,
                                   unsigned int *count,
                                   const char **failing_field,
                                   size_t *failing_length) {
  const char *field = mapping;
  unsigned int index = 0u;

  *count = 0u;
  *failing_field = mapping;
  *failing_length = 0u;
  while (*field != '\0') {
    const char *end = strchr(field, ',');
    size_t flen;
    const char *colon;
    const char *value;
    size_t vlen;
    struct binding *slot;
    size_t skip;
    const char *digits;
    size_t dlen;

    if (end == 0) {
      end = field + strlen(field);
    }
    flen = (size_t)(end - field);
    if (index < 2u) { /* GUID and name are not bindings */
      goto next;
    }
    *failing_field = field;
    *failing_length = flen;
    if (flen == 0u) {
      /* A trailing comma closes the line; an empty field anywhere else is
       * malformed and must not be skipped silently. */
      if (*end == '\0') {
        break;
      }
      return COLLECT_EMPTY_FIELD;
    }
    colon = (const char *)memchr(field, ':', flen);
    if (colon == 0) {
      return COLLECT_NO_COLON;
    }
    if (*count >= NXINPUT_GODOT_MAX_BINDINGS) {
      return COLLECT_TOO_MANY;
    }
    slot = &out[*count];
    memset(slot, 0, sizeof(*slot));
    slot->field = field;
    slot->field_length = flen;
    slot->name = field;
    slot->name_length = (size_t)(colon - field);
    slot->keep = 1;
    value = colon + 1;
    vlen = flen - (size_t)(colon - field) - 1u;
    if (slot->name_length == 0u) {
      return COLLECT_UNKNOWN_FIELD;
    }
    if (is_metadata_key(slot->name, slot->name_length)) {
      slot->kind = BIND_METADATA;
      (*count)++;
      goto next;
    }
    if (!is_control_key(slot->name, slot->name_length)) {
      return COLLECT_UNKNOWN_KEY;
    }
    if (vlen == 0u) {
      return COLLECT_EMPTY_VALUE;
    }
    skip = (value[0] == '+' || value[0] == '-') ? 1u : 0u;
    if (vlen <= skip) {
      return COLLECT_EMPTY_VALUE;
    }
    switch (value[skip]) {
      case 'b':
        if (skip != 0u) {
          return COLLECT_SIGN_ON_NON_AXIS;
        }
        digits = value + skip + 1u;
        dlen = vlen - skip - 1u;
        if (dlen > 0u && digits[dlen - 1u] == '~') {
          /* `~` inverts an AXIS. On a button it is meaningless, and the
           * 116A parser stripped it and carried on. */
          return COLLECT_TILDE_ON_NON_AXIS;
        }
        if (!parse_uint(digits, dlen, &slot->ordinal)) {
          return COLLECT_BAD_BUTTON;
        }
        slot->kind = BIND_BUTTON;
        slot->digits = digits;
        slot->digits_length = dlen;
        break;
      case 'a':
        digits = value + skip + 1u;
        dlen = vlen - skip - 1u;
        if (dlen > 0u && digits[dlen - 1u] == '~') {
          slot->inverted = 1;
          dlen--;
        }
        if (!parse_uint(digits, dlen, &slot->ordinal)) {
          return COLLECT_BAD_AXIS;
        }
        slot->kind = BIND_AXIS;
        slot->half = skip == 0u ? 0 : (value[0] == '+' ? 1 : -1);
        slot->digits = digits;
        slot->digits_length = dlen;
        break;
      case 'h': {
        const char *dot;
        const char *mask_digits;
        size_t mask_length;

        if (skip != 0u) {
          return COLLECT_SIGN_ON_NON_AXIS;
        }
        dot = (const char *)memchr(value + skip, '.', vlen - skip);
        if (dot == 0) {
          return COLLECT_BAD_HAT;
        }
        digits = value + skip + 1u;
        dlen = (size_t)(dot - digits);
        mask_digits = dot + 1;
        mask_length = (size_t)((value + vlen) - mask_digits);
        if (!parse_uint(digits, dlen, &slot->ordinal) ||
            !parse_uint(mask_digits, mask_length, &slot->hat_mask)) {
          return COLLECT_BAD_HAT;
        }
        if (!hat_mask_valid(slot->hat_mask)) {
          return COLLECT_BAD_HAT_MASK;
        }
        slot->kind = BIND_HAT;
        slot->digits = digits;
        slot->digits_length = dlen;
        break;
      }
      default:
        return COLLECT_UNKNOWN_FIELD;
    }
    (*count)++;
  next:
    if (*end == '\0') {
      break;
    }
    field = end + 1;
    index++;
  }
  *failing_field = mapping;
  *failing_length = 0u;
  return COLLECT_OK;
}

static const char *collect_reason(enum collect_status status) {
  switch (status) {
    case COLLECT_TOO_MANY:
      return "more bindings than the contract admits";
    case COLLECT_EMPTY_FIELD:
      return "an empty field sits between two commas";
    case COLLECT_NO_COLON:
      return "a field carries no `key:value` separator";
    case COLLECT_EMPTY_VALUE:
      return "a binding key carries no value";
    case COLLECT_BAD_BUTTON:
      return "a `bN` binding does not parse completely";
    case COLLECT_BAD_AXIS:
      return "an `aN` binding does not parse completely";
    case COLLECT_BAD_HAT:
      return "an `hN.M` binding is missing its dot or does not parse";
    case COLLECT_BAD_HAT_MASK:
      return "a hat mask is not exactly one of up/right/down/left";
    case COLLECT_TILDE_ON_NON_AXIS:
      return "`~` inverts an axis; it is meaningless on this binding";
    case COLLECT_UNKNOWN_FIELD:
      return "a field is neither declared metadata nor a known binding";
    case COLLECT_UNKNOWN_KEY:
      return "a control name is not one this contract defines";
    case COLLECT_SIGN_ON_NON_AXIS:
      return "`+`/`-` selects half of an axis; it is meaningless here";
    case COLLECT_OK:
    default:
      return "ok";
  }
}

/* A key bound twice is ambiguous whatever the second value says: order would
 * decide. `a:b0,a:b0` is refused exactly like `a:b0,a:b1`. */
static int has_duplicate_key(const struct binding *bindings,
                             unsigned int count) {
  unsigned int i;
  unsigned int j;

  for (i = 0u; i < count; i++) {
    for (j = i + 1u; j < count; j++) {
      if (bindings[i].name_length == bindings[j].name_length &&
          memcmp(bindings[i].name, bindings[j].name,
                 bindings[i].name_length) == 0) {
        return 1;
      }
    }
  }
  return 0;
}

static void set_reason(nxinput_godot_evidence *evidence, const char *text) {
  (void)snprintf(evidence->reason, sizeof evidence->reason, "%s", text);
}

/* Bounded append that can never walk past the buffer, whatever snprintf
 * would have wanted to write (116A robustness finding). */
static int append(char *out, size_t cap, size_t *used, const char *data,
                  size_t length) {
  if (out == 0 || *used > cap || length > cap - *used - 1u) {
    return -1;
  }
  memcpy(out + *used, data, length);
  *used += length;
  out[*used] = '\0';
  return 0;
}

nxinput_godot_result nxinput_godot_serve(nxinput_godot_engine engine,
                                         const nxinput_godot_origin *origin,
                                         const nxinput_godot_caps *caps,
                                         const char *mapping, char *out,
                                         size_t out_size,
                                         nxinput_godot_evidence *evidence) {
  nxinput_godot_evidence local;
  struct binding bindings[NXINPUT_GODOT_MAX_BINDINGS];
  unsigned int count = 0u;
  unsigned int i;
  enum collect_status status;
  const char *bad_field = mapping;
  size_t bad_length = 0u;
  char detail[128];
  nxinput_godot_domain source;
  size_t mapping_length;
  size_t used = 0u;

  (void)nxinput_godot_evidence_init(&local);
  local.engine = (uint8_t)engine;
  if (out != 0 && out_size > 0u) {
    out[0] = '\0';
  }
  if (mapping == 0 || out == 0 || out_size == 0u || (int)engine < 0 ||
      (int)engine >= (int)NXINPUT_GODOT_ENGINE_COUNT || !caps_valid(caps)) {
    set_reason(&local, "structurally invalid request");
    goto invalid;
  }

  /* THE ORIGIN IS DECLARED, NEVER INFERRED. */
  if (origin == 0 || origin->api_version != NXINPUT_GODOT_API_VERSION ||
      origin->struct_size != sizeof(*origin) ||
      !domain_valid((nxinput_godot_domain)origin->domain) ||
      origin->provider[0] == '\0' || origin->receipt[0] == '\0') {
    local.result = (uint8_t)NXINPUT_GODOT_ORIGIN_UNDECLARED;
    set_reason(&local,
               "no declared origin authorizes an ordinal domain; refusing "
               "to infer one from the mapping's contents");
    if (evidence != 0) {
      *evidence = local;
    }
    return NXINPUT_GODOT_ORIGIN_UNDECLARED;
  }
  source = (nxinput_godot_domain)origin->domain;
  local.source_domain = (uint8_t)source;
  (void)snprintf(local.provider, sizeof local.provider, "%s",
                 origin->provider);
  (void)snprintf(local.receipt, sizeof local.receipt, "%s", origin->receipt);

  mapping_length = strlen(mapping);
  if (mapping_length < 34u || mapping_length >= NXINPUT_GODOT_LINE_MAX ||
      memchr(mapping, '\n', mapping_length) != 0 ||
      memchr(mapping, '\r', mapping_length) != 0) {
    set_reason(&local, "mapping is not a single well-formed line");
    goto invalid;
  }

  local.keys = count_range(caps->key_bits, caps->key_bit_count, 0u,
                           NXINPUT_GODOT_KEY_MAX);
  local.godot_buttons =
      count_range(caps->key_bits, caps->key_bit_count,
                  NXINPUT_GODOT_BTN_JOYSTICK, NXINPUT_GODOT_KEY_MAX) +
      count_range(caps->key_bits, caps->key_bit_count,
                  NXINPUT_GODOT_BTN_MISC, NXINPUT_GODOT_BTN_JOYSTICK);
  local.ignored_low = count_range(caps->key_bits, caps->key_bit_count, 0u,
                                  NXINPUT_GODOT_BTN_MISC);
  for (i = 0u; i < NXINPUT_GODOT_ABS_MAX; i++) {
    if (is_hat_code(i) || !bit_set(caps->abs_bits, caps->abs_bit_count, i)) {
      continue;
    }
    local.axes++;
    if (i < NXINPUT_GODOT_ABS_MISC) {
      local.godot_axes++;
    }
  }

  status = collect(mapping, bindings, &count, &bad_field, &bad_length);
  if (status != COLLECT_OK) {
    size_t shown = bad_length < 40u ? bad_length : 40u;

    (void)snprintf(detail, sizeof detail, "%s: `%.*s`",
                   collect_reason(status), (int)shown, bad_field);
    local.result = (uint8_t)NXINPUT_GODOT_SYNTAX;
    set_reason(&local, detail);
    out[0] = '\0';
    if (evidence != 0) {
      *evidence = local;
    }
    return NXINPUT_GODOT_SYNTAX;
  }
  if (count == 0u) {
    set_reason(&local, "mapping declares no binding");
    goto invalid;
  }
  if (has_duplicate_key(bindings, count)) {
    local.result = (uint8_t)NXINPUT_GODOT_DUPLICATE;
    set_reason(&local,
               "a key is bound twice; order would decide, so it fails closed");
    if (evidence != 0) {
      *evidence = local;
    }
    return NXINPUT_GODOT_DUPLICATE;
  }

  /* Which bindings can the ENGINE name? Ordinals are absolute, so removing a
   * binding shifts nothing -- but losing one is still recorded, never
   * silent. */
  for (i = 0u; i < count; i++) {
    int code;
    int engine_ordinal = -1;

    switch (bindings[i].kind) {
      case BIND_BUTTON:
        local.button_bindings++;
        code = nxinput_godot_button_code(source, caps, bindings[i].ordinal);
        if (code >= 0) {
          engine_ordinal = nxinput_godot_button_ordinal(
              NXINPUT_GODOT_DOMAIN_GODOT, caps, (unsigned int)code);
        }
        break;
      case BIND_AXIS: {
        const nxinput_godot_absinfo *info;

        local.axis_bindings++;
        if (bindings[i].half != 0) {
          local.half_axis_bindings++;
        }
        if (bindings[i].inverted) {
          local.inverted_axis_bindings++;
        }
        code = nxinput_godot_axis_code(source, caps, bindings[i].ordinal);
        if (code >= 0) {
          engine_ordinal = nxinput_godot_axis_ordinal(
              NXINPUT_GODOT_DOMAIN_GODOT, caps, (unsigned int)code);
        }
        if (code < 0) {
          break; /* handled below as unreachable */
        }
        if (!caps_has_absinfo(caps)) {
          local.result = (uint8_t)NXINPUT_GODOT_ABSINFO;
          set_reason(&local,
                     "an axis is bound but no measured absinfo was supplied; "
                     "SDL's range decision cannot be reproduced");
          out[0] = '\0';
          if (evidence != 0) {
            *evidence = local;
          }
          return NXINPUT_GODOT_ABSINFO;
        }
        info = caps_absinfo(caps, (unsigned int)code);
        if (info == 0 || info->maximum <= info->minimum ||
            (bindings[i].half > 0 && info->maximum <= 0) ||
            (bindings[i].half < 0 && info->minimum >= 0)) {
          local.result = (uint8_t)NXINPUT_GODOT_ABSINFO;
          (void)snprintf(detail, sizeof detail,
                         "axis ordinal %u -> ABS 0x%x has no usable %s range",
                         bindings[i].ordinal, (unsigned int)code,
                         bindings[i].half == 0 ? "full"
                                               : (bindings[i].half > 0
                                                      ? "positive"
                                                      : "negative"));
          set_reason(&local, detail);
          out[0] = '\0';
          if (evidence != 0) {
            *evidence = local;
          }
          return NXINPUT_GODOT_ABSINFO;
        }
        break;
      }
      case BIND_HAT: {
        /* Both domains index hats by pair order over ABS_HAT0X..ABS_HAT3Y,
         * so a hat ordinal is domain-independent -- but the hat has to
         * EXIST. 116A counted hats and never looked. */
        unsigned int hx;

        local.hat_bindings++;
        if (bindings[i].ordinal >= NXINPUT_GODOT_HAT_COUNT) {
          local.result = (uint8_t)NXINPUT_GODOT_HAT;
          (void)snprintf(detail, sizeof detail,
                         "hat ordinal %u is past ABS_HAT3Y",
                         bindings[i].ordinal);
          set_reason(&local, detail);
          out[0] = '\0';
          if (evidence != 0) {
            *evidence = local;
          }
          return NXINPUT_GODOT_HAT;
        }
        hx = NXINPUT_GODOT_ABS_HAT0X + 2u * bindings[i].ordinal;
        if (!bit_set(caps->abs_bits, caps->abs_bit_count, hx) ||
            !bit_set(caps->abs_bits, caps->abs_bit_count, hx + 1u)) {
          local.result = (uint8_t)NXINPUT_GODOT_HAT;
          (void)snprintf(detail, sizeof detail,
                         "hat %u is bound but ABS_HAT%uX/Y is not present",
                         bindings[i].ordinal, bindings[i].ordinal);
          set_reason(&local, detail);
          out[0] = '\0';
          if (evidence != 0) {
            *evidence = local;
          }
          return NXINPUT_GODOT_HAT;
        }
        continue;
      }
      case BIND_METADATA:
      default:
        local.metadata_fields++;
        continue;
    }
    if (engine_ordinal < 0) {
      bindings[i].keep = 0;
      local.unreachable_bindings++;
    }
  }
  if (local.unreachable_bindings > 0u) {
    local.result = (uint8_t)NXINPUT_GODOT_UNREACHABLE;
    set_reason(&local,
               "a binding names something this engine cannot enumerate");
    out[0] = '\0';
    if (evidence != 0) {
      *evidence = local;
    }
    return NXINPUT_GODOT_UNREACHABLE;
  }

  if (source == NXINPUT_GODOT_DOMAIN_GODOT) {
    if (mapping_length + 1u > out_size) {
      set_reason(&local, "output buffer too small");
      goto invalid;
    }
    memcpy(out, mapping, mapping_length + 1u);
    local.result = (uint8_t)NXINPUT_GODOT_BYTE_INTACT;
    local.internal_consistency = 1u;
    set_reason(&local, "declared origin already is the engine's domain");
    if (evidence != 0) {
      *evidence = local;
    }
    return NXINPUT_GODOT_BYTE_INTACT;
  }

  /* Convert once: only the ordinals move, every other byte is preserved. */
  {
    const char *cursor = mapping;
    unsigned int next = 0u;

    while (*cursor != '\0') {
      const char *end = strchr(cursor, ',');
      size_t chunk;

      if (end == 0) {
        end = cursor + strlen(cursor);
      }
      chunk = (size_t)(end - cursor);
      if (next < count && bindings[next].field == cursor) {
        struct binding *b = &bindings[next];

        next++;
        if (b->kind == BIND_BUTTON || b->kind == BIND_AXIS) {
          size_t head = (size_t)(b->digits - cursor);
          size_t tail_offset = head + b->digits_length;
          int code = b->kind == BIND_BUTTON
                         ? nxinput_godot_button_code(source, caps, b->ordinal)
                         : nxinput_godot_axis_code(source, caps, b->ordinal);
          int fresh = code < 0 ? -1
                     : b->kind == BIND_BUTTON
                         ? nxinput_godot_button_ordinal(
                               NXINPUT_GODOT_DOMAIN_GODOT, caps,
                               (unsigned int)code)
                         : nxinput_godot_axis_ordinal(
                               NXINPUT_GODOT_DOMAIN_GODOT, caps,
                               (unsigned int)code);
          char number[8];
          int written;

          if (fresh < 0) {
            set_reason(&local, "a binding lost its code during conversion");
            goto invalid;
          }
          written = snprintf(number, sizeof number, "%d", fresh);
          if (written <= 0 || (size_t)written >= sizeof number) {
            set_reason(&local, "converted ordinal does not fit");
            goto invalid;
          }
          if (append(out, out_size, &used, cursor, head) != 0 ||
              append(out, out_size, &used, number, (size_t)written) != 0 ||
              append(out, out_size, &used, cursor + tail_offset,
                     chunk - tail_offset) != 0) {
            set_reason(&local, "output buffer too small");
            goto invalid;
          }
          if ((unsigned int)fresh != b->ordinal) {
            local.rewritten_bindings++;
          }
        } else if (append(out, out_size, &used, cursor, chunk) != 0) {
          set_reason(&local, "output buffer too small");
          goto invalid;
        }
      } else if (append(out, out_size, &used, cursor, chunk) != 0) {
        set_reason(&local, "output buffer too small");
        goto invalid;
      }
      if (*end == ',') {
        if (append(out, out_size, &used, ",", 1u) != 0) {
          set_reason(&local, "output buffer too small");
          goto invalid;
        }
        cursor = end + 1;
      } else {
        cursor = end;
      }
    }
  }

  /* INTERNAL CONSISTENCY ONLY. Re-deriving our own output with our own
   * functions proves the converter agrees with itself -- nothing more. The
   * readback that authorizes announcing a joypad comes from the ENGINE, in
   * nxinput_godot_consumer.h; this flag must never be read as one. */
  {
    struct binding after[NXINPUT_GODOT_MAX_BINDINGS];
    unsigned int after_count = 0u;
    unsigned int checked = 0u;

    if (collect(out, after, &after_count, &bad_field, &bad_length) !=
            COLLECT_OK ||
        after_count != count) {
      set_reason(&local, "self-check found a different binding set");
      goto invalid;
    }
    for (i = 0u; i < count; i++) {
      int before;
      int now;

      if (bindings[i].kind == BIND_BUTTON) {
        before = nxinput_godot_button_code(source, caps, bindings[i].ordinal);
        now = nxinput_godot_button_code(NXINPUT_GODOT_DOMAIN_GODOT, caps,
                                        after[i].ordinal);
      } else if (bindings[i].kind == BIND_AXIS) {
        before = nxinput_godot_axis_code(source, caps, bindings[i].ordinal);
        now = nxinput_godot_axis_code(NXINPUT_GODOT_DOMAIN_GODOT, caps,
                                      after[i].ordinal);
      } else {
        continue;
      }
      if (before < 0 || before != now) {
        set_reason(&local, "self-check disagreed with the source binding");
        goto invalid;
      }
      checked++;
    }
    local.internal_consistency =
        (uint8_t)(checked == local.button_bindings + local.axis_bindings
                      ? 1u : 0u);
    if (!local.internal_consistency) {
      set_reason(&local, "self-check did not cover every ordinal binding");
      goto invalid;
    }
  }

  /* If the declared origins differ but this pad ranks every binding the
   * same way, nothing moved: the mapping is byte-intact in fact, and saying
   * "converted" would overstate what happened. */
  if (strcmp(out, mapping) == 0) {
    local.result = (uint8_t)NXINPUT_GODOT_BYTE_INTACT;
    set_reason(&local,
               "declared origins differ but this pad ranks every binding "
               "identically; nothing was rewritten");
    if (evidence != 0) {
      *evidence = local;
    }
    return NXINPUT_GODOT_BYTE_INTACT;
  }
  local.result = (uint8_t)NXINPUT_GODOT_CONVERTED;
  set_reason(&local, "declared origin differs from the engine; converted once");
  if (evidence != 0) {
    *evidence = local;
  }
  return NXINPUT_GODOT_CONVERTED;

invalid:
  local.result = (uint8_t)NXINPUT_GODOT_INVALID;
  if (out != 0 && out_size > 0u) {
    out[0] = '\0';
  }
  if (evidence != 0) {
    *evidence = local;
  }
  return NXINPUT_GODOT_INVALID;
}

int nxinput_godot_evidence_line(const nxinput_godot_evidence *evidence,
                                char *out, size_t out_size) {
  int written;

  if (evidence == 0 || out == 0 || out_size == 0u ||
      evidence->api_version != NXINPUT_GODOT_API_VERSION) {
    return -1;
  }
  written = snprintf(
      out, out_size,
      "NXINPUT-GODOT-MAPPING-EVIDENCE: engine=%s result=%s "
      "declared_origin=%s provider=%s receipt=%s keys=%u godot_buttons=%u "
      "ignored_low=%u axes=%u godot_axes=%u buttons=%u axis=%u hats=%u "
      "meta=%u half_axis=%u inverted_axis=%u "
      "rewritten=%u unreachable=%u internal_consistency=%u reason=%s",
      nxinput_godot_engine_name((nxinput_godot_engine)evidence->engine),
      nxinput_godot_result_name((nxinput_godot_result)evidence->result),
      nxinput_godot_domain_name(
          (nxinput_godot_domain)evidence->source_domain),
      evidence->provider[0] ? evidence->provider : "-",
      evidence->receipt[0] ? evidence->receipt : "-", evidence->keys,
      evidence->godot_buttons, evidence->ignored_low, evidence->axes,
      evidence->godot_axes, evidence->button_bindings,
      evidence->axis_bindings, evidence->hat_bindings,
      evidence->metadata_fields, evidence->half_axis_bindings,
      evidence->inverted_axis_bindings, evidence->rewritten_bindings, evidence->unreachable_bindings,
      (unsigned int)evidence->internal_consistency, evidence->reason);
  return written > 0 && (size_t)written < out_size ? written : -1;
}
