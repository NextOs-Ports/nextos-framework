/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_sovereign -- see include/nxinput_sovereign.h. Pure. */
#include "nxinput_sovereign.h"

#include <stdio.h>
#include <string.h>

/* Binding keys of the SDL2 dialect. `platform` and `crc` are metadata:
 * validated for syntax, ignored by the semantic comparison. `name` is the
 * second CSV field and is cosmetic. */
static const char *const binding_keys[] = {
    "a", "b", "x", "y", "back", "guide", "start",
    "leftstick", "rightstick", "leftshoulder", "rightshoulder",
    "dpup", "dpdown", "dpleft", "dpright",
    "leftx", "lefty", "rightx", "righty",
    "lefttrigger", "righttrigger",
    "misc1", "misc2", "misc3", "misc4", "misc5", "misc6",
    "paddle1", "paddle2", "paddle3", "paddle4", "touchpad",
};
#define BINDING_KEY_COUNT (sizeof(binding_keys) / sizeof(binding_keys[0]))
#define MAX_FIELDS 64u

const char *nxinput_sovereign_source_name(nxinput_sovereign_source source) {
  switch (source) {
    case NXINPUT_SOVEREIGN_ENV_GET_CONTROLS:
      return "env-get-controls";
    case NXINPUT_SOVEREIGN_CFW_DB_GUID:
      return "cfw-db-guid";
    case NXINPUT_SOVEREIGN_PORT_BUNDLE:
      return "port-bundle";
    case NXINPUT_SOVEREIGN_RUNTIME_BUILTIN:
      return "runtime-builtin";
    case NXINPUT_SOVEREIGN_RAW_PASSTHROUGH:
      return "raw-passthrough";
    case NXINPUT_SOVEREIGN_FAIL_EXPLICIT:
    default:
      return "fail-explicit";
  }
}

const char *nxinput_sovereign_reason_name(nxinput_sovereign_reason reason) {
  switch (reason) {
    case NXINPUT_SOVEREIGN_OK:
      return "ok";
    case NXINPUT_SOVEREIGN_SOURCE_EMPTY:
      return "source-empty";
    case NXINPUT_SOVEREIGN_GUID_NOT_FOUND:
      return "guid-not-found";
    case NXINPUT_SOVEREIGN_SYNTAX_INVALID:
      return "syntax-invalid";
    case NXINPUT_SOVEREIGN_DUPLICATE_DIVERGENT:
      return "duplicate-divergent";
    case NXINPUT_SOVEREIGN_UNREACHABLE:
      return "unreachable";
    case NXINPUT_SOVEREIGN_READBACK_MISMATCH:
      return "readback-mismatch";
    case NXINPUT_SOVEREIGN_BUNDLE_HEADER_INVALID:
      return "bundle-header-invalid";
    case NXINPUT_SOVEREIGN_CONSUMER_REFUSES_RAW:
      return "consumer-refuses-raw";
    case NXINPUT_SOVEREIGN_NOT_AVAILABLE:
      return "not-available";
    case NXINPUT_SOVEREIGN_REQUEST_INVALID:
    default:
      return "request-invalid";
  }
}

int nxinput_sovereign_request_init(nxinput_sovereign_request *request) {
  if (request == NULL) {
    return -1;
  }
  memset(request, 0, sizeof(*request));
  request->api_version = NXINPUT_SOVEREIGN_API_VERSION;
  request->struct_size = sizeof(*request);
  request->caps.api_version = NXINPUT_SOVEREIGN_API_VERSION;
  request->caps.struct_size = sizeof(request->caps);
  return 0;
}

int nxinput_sovereign_decision_init(nxinput_sovereign_decision *decision) {
  size_t i;
  if (decision == NULL) {
    return -1;
  }
  memset(decision, 0, sizeof(*decision));
  decision->api_version = NXINPUT_SOVEREIGN_API_VERSION;
  decision->struct_size = sizeof(*decision);
  decision->source = NXINPUT_SOVEREIGN_FAIL_EXPLICIT;
  decision->reason = NXINPUT_SOVEREIGN_NOT_AVAILABLE;
  for (i = 0u; i < 5u; i++) {
    decision->step_reason[i] = NXINPUT_SOVEREIGN_OK;
  }
  return 0;
}

static int hex_lower(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

static int guid_valid(const char *guid) {
  size_t i;
  if (guid == NULL) {
    return 0;
  }
  for (i = 0u; i < 32u; i++) {
    if (!hex_lower(guid[i])) {
      return 0;
    }
  }
  return guid[32] == '\0';
}

/* Parse a non-negative decimal; returns -1 on garbage. */
static int parse_ordinal(const char *s, size_t len) {
  int value = 0;
  size_t i;
  if (len == 0u || len > 4u) {
    return -1;
  }
  for (i = 0u; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return -1;
    }
    value = value * 10 + (s[i] - '0');
  }
  return value;
}

/* One binding value: bN | aN | +aN | -aN | (any of those)~ | hN.M. Writes
 * the referenced ordinals (or -1) for the reachability check. */
static int value_valid(const char *value, size_t len, int *button,
                       int *axis, int *hat) {
  size_t start = 0u;
  *button = -1;
  *axis = -1;
  *hat = -1;
  if (len == 0u) {
    return 0;
  }
  if (value[len - 1u] == '~') {
    len--;
    if (len == 0u) {
      return 0;
    }
  }
  if (value[0] == '+' || value[0] == '-') {
    start = 1u;
    if (start >= len || value[start] != 'a') {
      return 0;
    }
  }
  if (value[start] == 'b') {
    *button = parse_ordinal(value + start + 1u, len - start - 1u);
    return *button >= 0 && start == 0u;
  }
  if (value[start] == 'a') {
    *axis = parse_ordinal(value + start + 1u, len - start - 1u);
    return *axis >= 0;
  }
  if (value[start] == 'h') {
    const char *dot = memchr(value + start, '.', len - start);
    int mask;
    if (dot == NULL || start != 0u) {
      return 0;
    }
    *hat = parse_ordinal(value + 1u, (size_t)(dot - value) - 1u);
    mask = parse_ordinal(dot + 1u, len - (size_t)(dot - value) - 1u);
    return *hat >= 0 && mask >= 0;
  }
  return 0;
}

static const char *const axis_keys[] = {
    "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger",
};

static int key_is_binding(const char *key, size_t len) {
  size_t i;
  /* SDL2 half-axis outputs: an axis key may carry a +/- prefix. */
  if (len > 1u && (key[0] == '+' || key[0] == '-')) {
    for (i = 0u; i < sizeof(axis_keys) / sizeof(axis_keys[0]); i++) {
      if (strlen(axis_keys[i]) == len - 1u &&
          memcmp(axis_keys[i], key + 1u, len - 1u) == 0) {
        return 1;
      }
    }
    return 0;
  }
  for (i = 0u; i < BINDING_KEY_COUNT; i++) {
    if (strlen(binding_keys[i]) == len &&
        memcmp(binding_keys[i], key, len) == 0) {
      return 1;
    }
  }
  return 0;
}

/* Walk the fields of a line, optionally checking reachability against caps.
 * When `pairs`/`pair_count` are given, the binding key:value pairs are
 * collected (metadata and name excluded) for the semantic comparison. */
struct pair {
  const char *key;
  size_t key_len;
  const char *value;
  size_t value_len;
};

static nxinput_sovereign_reason walk_line(
    const char *line, const nxinput_sovereign_caps *caps,
    struct pair *pairs, size_t *pair_count) {
  const char *cursor;
  unsigned int field = 0u;
  size_t len = strlen(line);
  size_t count = 0u;
  /* Every binding pair is always collected, even when the caller wants no
   * output: the duplicate-key check below must see the whole line. */
  struct pair seen[MAX_FIELDS];

  if (len < 34u || len >= NXINPUT_SOVEREIGN_LINE_MAX) {
    return NXINPUT_SOVEREIGN_SYNTAX_INVALID;
  }
  {
    size_t i;
    for (i = 0u; i < 32u; i++) {
      if (!hex_lower(line[i])) {
        return NXINPUT_SOVEREIGN_SYNTAX_INVALID;
      }
    }
    if (line[32] != ',') {
      return NXINPUT_SOVEREIGN_SYNTAX_INVALID;
    }
  }
  cursor = line + 33; /* the name field */
  field = 1u;
  while (*cursor != '\0') {
    const char *comma = strchr(cursor, ',');
    size_t flen = comma ? (size_t)(comma - cursor) : strlen(cursor);
    if (field == 1u) {
      /* The name is cosmetic and may legally contain ':' (a real upstream
       * entry is literally named "idroid:con"); only emptiness is invalid. */
      if (flen == 0u) {
        return NXINPUT_SOVEREIGN_SYNTAX_INVALID;
      }
    } else if (flen > 0u) {
      const char *colon = memchr(cursor, ':', flen);
      size_t key_len;
      if (colon == NULL) {
        return NXINPUT_SOVEREIGN_SYNTAX_INVALID;
      }
      key_len = (size_t)(colon - cursor);
      if (key_is_binding(cursor, key_len)) {
        int button, axis, hat;
        size_t value_len = flen - key_len - 1u;
        if (!value_valid(colon + 1u, value_len, &button, &axis, &hat)) {
          return NXINPUT_SOVEREIGN_SYNTAX_INVALID;
        }
        if (caps != NULL) {
          if ((button >= 0 && button >= caps->buttons) ||
              (axis >= 0 && axis >= caps->axes) ||
              (hat >= 0 && hat >= caps->hats)) {
            return NXINPUT_SOVEREIGN_UNREACHABLE;
          }
        }
        {
          size_t prior;
          size_t slot = count;
          /* SDL parses the fields in order and a later assignment simply
           * overwrites an earlier one, so a duplicated binding key resolves
           * exactly the way the runtime will execute it: the LAST occurrence
           * wins. The dedup also keeps the semantic comparison well-defined
           * -- both sides compare their EFFECTIVE pairs, so a divergent
           * duplicate can never hide behind a satisfied first occurrence. */
          for (prior = 0u; prior < count; prior++) {
            if (seen[prior].key_len == key_len &&
                memcmp(seen[prior].key, cursor, key_len) == 0) {
              slot = prior;
              break;
            }
          }
          if (slot == count) {
            if (count >= MAX_FIELDS) {
              return NXINPUT_SOVEREIGN_SYNTAX_INVALID;
            }
            count++;
          }
          seen[slot].key = cursor;
          seen[slot].key_len = key_len;
          seen[slot].value = colon + 1u;
          seen[slot].value_len = value_len;
          if (pairs != NULL) {
            pairs[slot] = seen[slot];
          }
        }
      } else {
        /* Not a binding key. Real CFW data carries metadata this parser has
         * never heard of (platform, crc, hint, type, sdk>=, and whatever a
         * CFW invents next) and SDL ignores every unrecognized field; the
         * colon above already proved the field's key:value shape, so this is
         * metadata -- tolerated, never semantic, never an error. */
      }
    }
    if (field > MAX_FIELDS) {
      return NXINPUT_SOVEREIGN_SYNTAX_INVALID;
    }
    if (!comma) {
      break;
    }
    cursor = comma + 1;
    field++;
  }
  /* A line that parses but binds nothing is not a mapping: it would compare
   * "identical" to any other empty line and would authorize gameplay with no
   * reachable control at all. It fails closed. */
  if (count == 0u) {
    return NXINPUT_SOVEREIGN_SYNTAX_INVALID;
  }
  if (pair_count != NULL) {
    *pair_count = count;
  }
  return NXINPUT_SOVEREIGN_OK;
}

nxinput_sovereign_reason nxinput_sovereign_line_syntax(const char *line) {
  if (line == NULL) {
    return NXINPUT_SOVEREIGN_SYNTAX_INVALID;
  }
  return walk_line(line, NULL, NULL, NULL);
}

nxinput_sovereign_reason nxinput_sovereign_line_reachable(
    const char *line, const nxinput_sovereign_caps *caps) {
  if (line == NULL || caps == NULL ||
      caps->api_version != NXINPUT_SOVEREIGN_API_VERSION ||
      caps->struct_size != sizeof(*caps)) {
    return NXINPUT_SOVEREIGN_REQUEST_INVALID;
  }
  return walk_line(line, caps, NULL, NULL);
}

int nxinput_sovereign_semantically_identical(const char *a, const char *b) {
  struct pair pa[MAX_FIELDS], pb[MAX_FIELDS];
  size_t na = 0u, nb = 0u;
  size_t i, j;

  if (a == NULL || b == NULL) {
    return 0;
  }
  if (walk_line(a, NULL, pa, &na) != NXINPUT_SOVEREIGN_OK ||
      walk_line(b, NULL, pb, &nb) != NXINPUT_SOVEREIGN_OK) {
    return 0;
  }
  if (strncmp(a, b, 32u) != 0 || na != nb) {
    return 0;
  }
  for (i = 0u; i < na; i++) {
    int found = 0;
    for (j = 0u; j < nb; j++) {
      if (pa[i].key_len == pb[j].key_len &&
          memcmp(pa[i].key, pb[j].key, pa[i].key_len) == 0) {
        /* An axis is never equal to a button: the VALUE bytes must match. */
        if (pa[i].value_len == pb[j].value_len &&
            memcmp(pa[i].value, pb[j].value, pa[i].value_len) == 0) {
          found = 1;
        }
        break;
      }
    }
    if (!found) {
      return 0;
    }
  }
  return 1;
}

static void copy_trimmed(const char *start, size_t len, char *out,
                         size_t cap) {
  while (len > 0u && (start[len - 1u] == '\r' || start[len - 1u] == ' ')) {
    len--;
  }
  if (len >= cap) {
    len = cap - 1u;
  }
  memcpy(out, start, len);
  out[len] = '\0';
}

static nxinput_sovereign_reason lookup_lines(
    const char *text, const char *guid, int skip_header, char *out,
    size_t cap, int *divergent_lastwins) {
  const char *cursor = text;
  char found[NXINPUT_SOVEREIGN_LINE_MAX];
  int matches = 0;
  int line_index = 0;

  found[0] = '\0';
  if (text == NULL || text[0] == '\0') {
    return NXINPUT_SOVEREIGN_SOURCE_EMPTY;
  }
  if (!guid_valid(guid)) {
    return NXINPUT_SOVEREIGN_REQUEST_INVALID;
  }
  while (*cursor != '\0') {
    const char *eol = strchr(cursor, '\n');
    size_t len = eol ? (size_t)(eol - cursor) : strlen(cursor);
    line_index++;
    if ((skip_header && line_index == 1) || len == 0u || cursor[0] == '#') {
      /* header/comment/blank */
    } else if (len > 33u && strncmp(cursor, guid, 32u) == 0 &&
               cursor[32] == ',') {
      char candidate[NXINPUT_SOVEREIGN_LINE_MAX];
      copy_trimmed(cursor, len, candidate, sizeof candidate);
      /* Real CFW stores legally carry the same GUID more than once, and the
       * runtime that will execute the mapping is SDL, whose AddMapping
       * REPLACES an existing entry: the LAST line wins. Mirror exactly that
       * -- refusing here would reject the CFW's own working data -- and
       * COUNT every divergent body the tolerance resolved, so the receipt
       * still shows the conflict instead of hiding it. */
      if (matches > 0 && strcmp(candidate, found) != 0 &&
          divergent_lastwins != NULL) {
        (*divergent_lastwins)++;
      }
      memcpy(found, candidate, strlen(candidate) + 1u);
      matches++;
    }
    if (!eol) {
      break;
    }
    cursor = eol + 1;
  }
  if (matches == 0) {
    return NXINPUT_SOVEREIGN_GUID_NOT_FOUND;
  }
  if (out != NULL && cap > 0u) {
    copy_trimmed(found, strlen(found), out, cap);
  }
  return NXINPUT_SOVEREIGN_OK;
}

nxinput_sovereign_reason nxinput_sovereign_db_lookup(
    const char *database, const char *guid, char *out, size_t cap) {
  return lookup_lines(database, guid, 0, out, cap, NULL);
}

static nxinput_sovereign_reason bundle_lookup_counted(
    const char *bundle, const char *guid, char *out, size_t cap,
    int *divergent_lastwins) {
  static const char header[] = "NXCONTROLLER_PROFILES/1";
  size_t header_len = sizeof(header) - 1u;
  if (bundle == NULL || bundle[0] == '\0') {
    return NXINPUT_SOVEREIGN_SOURCE_EMPTY;
  }
  if (strncmp(bundle, header, header_len) != 0 ||
      (bundle[header_len] != '\n' && bundle[header_len] != '\r' &&
       bundle[header_len] != '\0')) {
    return NXINPUT_SOVEREIGN_BUNDLE_HEADER_INVALID;
  }
  return lookup_lines(bundle, guid, 1, out, cap, divergent_lastwins);
}

nxinput_sovereign_reason nxinput_sovereign_bundle_lookup(
    const char *bundle, const char *guid, char *out, size_t cap) {
  return bundle_lookup_counted(bundle, guid, out, cap, NULL);
}

/* Validate one candidate line through the whole ladder. On success the line
 * is installed in the decision byte-intact. */
static nxinput_sovereign_reason try_candidate(
    const nxinput_sovereign_request *request, const char *line,
    nxinput_sovereign_readback_fn readback, void *userdata,
    nxinput_sovereign_decision *decision) {
  nxinput_sovereign_reason reason;

  reason = nxinput_sovereign_line_syntax(line);
  if (reason != NXINPUT_SOVEREIGN_OK) {
    return reason;
  }
  if (strncmp(line, request->guid, 32u) != 0) {
    return NXINPUT_SOVEREIGN_GUID_NOT_FOUND;
  }
  reason = walk_line(line, &request->caps, NULL, NULL);
  if (reason != NXINPUT_SOVEREIGN_OK) {
    return reason;
  }
  if (readback != NULL) {
    char effective[NXINPUT_SOVEREIGN_LINE_MAX];
    if (readback(userdata, line, effective, sizeof effective) != 0) {
      return NXINPUT_SOVEREIGN_READBACK_MISMATCH;
    }
    if (!nxinput_sovereign_semantically_identical(line, effective)) {
      return NXINPUT_SOVEREIGN_READBACK_MISMATCH;
    }
    decision->readback_checked = 1;
  } else {
    decision->readback_checked = 0;
  }
  (void)snprintf(decision->line, sizeof decision->line, "%s", line);
  return NXINPUT_SOVEREIGN_OK;
}

int nxinput_sovereign_resolve(
    const nxinput_sovereign_request *request,
    const char *env_get_controls_mapping,
    const char *cfw_database,
    const char *port_bundle,
    nxinput_sovereign_readback_fn readback, void *userdata,
    nxinput_sovereign_decision *decision) {
  char candidate[NXINPUT_SOVEREIGN_LINE_MAX];
  nxinput_sovereign_reason reason;

  if (decision == NULL) {
    return -1;
  }
  (void)nxinput_sovereign_decision_init(decision);
  if (request == NULL ||
      request->api_version != NXINPUT_SOVEREIGN_API_VERSION ||
      request->struct_size != sizeof(*request) ||
      !guid_valid(request->guid) ||
      request->caps.api_version != NXINPUT_SOVEREIGN_API_VERSION ||
      request->caps.struct_size != sizeof(request->caps) ||
      request->caps.buttons < 0 || request->caps.axes < 0 ||
      request->caps.hats < 0) {
    decision->reason = NXINPUT_SOVEREIGN_REQUEST_INVALID;
    return -1;
  }

  /* 1. The mapping the CFW's control.txt/get_controls already delivered. */
  reason = lookup_lines(env_get_controls_mapping, request->guid, 0,
                        candidate, sizeof candidate,
                        &decision->duplicate_lastwins);
  if (reason == NXINPUT_SOVEREIGN_OK) {
    reason = try_candidate(request, candidate, readback, userdata, decision);
  }
  decision->step_reason[NXINPUT_SOVEREIGN_ENV_GET_CONTROLS] = reason;
  if (reason == NXINPUT_SOVEREIGN_OK) {
    decision->source = NXINPUT_SOVEREIGN_ENV_GET_CONTROLS;
    decision->reason = NXINPUT_SOVEREIGN_OK;
    return 0;
  }

  /* 2. The exact GUID entry in the CFW's official database. */
  reason = lookup_lines(cfw_database, request->guid, 0, candidate,
                        sizeof candidate, &decision->duplicate_lastwins);
  if (reason == NXINPUT_SOVEREIGN_OK) {
    reason = try_candidate(request, candidate, readback, userdata, decision);
  }
  decision->step_reason[NXINPUT_SOVEREIGN_CFW_DB_GUID] = reason;
  if (reason == NXINPUT_SOVEREIGN_OK) {
    decision->source = NXINPUT_SOVEREIGN_CFW_DB_GUID;
    decision->reason = NXINPUT_SOVEREIGN_OK;
    return 0;
  }

  /* 3. The NXCONTROLLER_PROFILES/1 bundle pinned inside the port ZIP. */
  reason = bundle_lookup_counted(port_bundle, request->guid, candidate,
                                 sizeof candidate,
                                 &decision->duplicate_lastwins);
  if (reason == NXINPUT_SOVEREIGN_OK) {
    reason = try_candidate(request, candidate, readback, userdata, decision);
  }
  decision->step_reason[NXINPUT_SOVEREIGN_PORT_BUNDLE] = reason;
  if (reason == NXINPUT_SOVEREIGN_OK) {
    decision->source = NXINPUT_SOVEREIGN_PORT_BUNDLE;
    decision->reason = NXINPUT_SOVEREIGN_OK;
    return 0;
  }

  /* 4. The runtime's built-in database: ask the runtime (through the same
   * injected effect, line="") what it would effectively use, then validate
   * that answer under the same rules. */
  if (request->runtime_has_builtin && readback != NULL) {
    char effective[NXINPUT_SOVEREIGN_LINE_MAX];
    if (readback(userdata, "", effective, sizeof effective) == 0 &&
        nxinput_sovereign_line_syntax(effective) == NXINPUT_SOVEREIGN_OK &&
        strncmp(effective, request->guid, 32u) == 0 &&
        walk_line(effective, &request->caps, NULL, NULL) ==
            NXINPUT_SOVEREIGN_OK) {
      (void)snprintf(decision->line, sizeof decision->line, "%s", effective);
      decision->readback_checked = 1;
      decision->source = NXINPUT_SOVEREIGN_RUNTIME_BUILTIN;
      decision->reason = NXINPUT_SOVEREIGN_OK;
      decision->step_reason[NXINPUT_SOVEREIGN_RUNTIME_BUILTIN] =
          NXINPUT_SOVEREIGN_OK;
      return 0;
    }
    decision->step_reason[NXINPUT_SOVEREIGN_RUNTIME_BUILTIN] =
        NXINPUT_SOVEREIGN_NOT_AVAILABLE;
  } else {
    decision->step_reason[NXINPUT_SOVEREIGN_RUNTIME_BUILTIN] =
        NXINPUT_SOVEREIGN_NOT_AVAILABLE;
  }

  /* 5. Raw passthrough: only when the consumer declared it understands it. */
  if (request->consumer_accepts_raw) {
    decision->line[0] = '\0';
    decision->readback_checked = 0;
    decision->source = NXINPUT_SOVEREIGN_RAW_PASSTHROUGH;
    decision->reason = NXINPUT_SOVEREIGN_OK;
    decision->step_reason[NXINPUT_SOVEREIGN_RAW_PASSTHROUGH] =
        NXINPUT_SOVEREIGN_OK;
    return 0;
  }
  decision->step_reason[NXINPUT_SOVEREIGN_RAW_PASSTHROUGH] =
      NXINPUT_SOVEREIGN_CONSUMER_REFUSES_RAW;

  /* 6. Explicit failure before gameplay. */
  decision->source = NXINPUT_SOVEREIGN_FAIL_EXPLICIT;
  decision->reason = NXINPUT_SOVEREIGN_NOT_AVAILABLE;
  decision->line[0] = '\0';
  return 0;
}
