/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxinput_gptk.h"
/* V3 (blocker 7): the dispatcher runs stick vectors through the kinematics,
 * so the motion API is part of this translation unit. Both sources ship in
 * the same installed static library (nxinput-gptk). */
#include "nxinput_gptk_motion.h"

#include <stdio.h>
#include <string.h>

/* nxinput_button values duplicated as plain ints so this translation unit
 * never depends on SDL headers. nxinput_gptk.h carries the compile-time
 * anchor asserts against nxinput.h; the numbering below follows the
 * nxinput_button enum in include/nxinput.h (API version 1). */
#define GPTK_NXBTN_A 0
#define GPTK_NXBTN_B 1
#define GPTK_NXBTN_X 2
#define GPTK_NXBTN_Y 3
#define GPTK_NXBTN_BACK 4
#define GPTK_NXBTN_START 6
#define GPTK_NXBTN_LEFT_STICK 7
#define GPTK_NXBTN_RIGHT_STICK 8
#define GPTK_NXBTN_LEFT_SHOULDER 9
#define GPTK_NXBTN_RIGHT_SHOULDER 10
#define GPTK_NXBTN_DPAD_UP 11
#define GPTK_NXBTN_DPAD_DOWN 12
#define GPTK_NXBTN_DPAD_LEFT 13
#define GPTK_NXBTN_DPAD_RIGHT 14

typedef struct gptk_control_entry {
  const char *name;
  int button; /* nxinput_button value, or -1 for analog-only controls */
} gptk_control_entry;

/* Order must match nxinput_gptk_control. Symbolic names ONLY: numeric evdev
 * codes are firmware-specific and rejected (Chrono Trigger regression). */
static const gptk_control_entry gptk_controls[NXINPUT_GPTK_CONTROL_COUNT] = {
  {"A", GPTK_NXBTN_A},
  {"B", GPTK_NXBTN_B},
  {"X", GPTK_NXBTN_X},
  {"Y", GPTK_NXBTN_Y},
  {"L1", GPTK_NXBTN_LEFT_SHOULDER},
  {"R1", GPTK_NXBTN_RIGHT_SHOULDER},
  {"L2", -1},
  {"R2", -1},
  {"L3", GPTK_NXBTN_LEFT_STICK},
  {"R3", GPTK_NXBTN_RIGHT_STICK},
  {"START", GPTK_NXBTN_START},
  {"SELECT", GPTK_NXBTN_BACK},
  {"UP", GPTK_NXBTN_DPAD_UP},
  {"DOWN", GPTK_NXBTN_DPAD_DOWN},
  {"LEFT", GPTK_NXBTN_DPAD_LEFT},
  {"RIGHT", GPTK_NXBTN_DPAD_RIGHT},
  {"LEFT_STICK", -1},
  {"RIGHT_STICK", -1},
};

static const char *const gptk_sections[NXINPUT_GPTK_CONTEXT_COUNT] = {
  "menu", "gameplay", "cursor"
};

static void gptk_set_error(char *error, size_t error_size, int code,
                           const char *message) {
  if (error != 0 && error_size > 0u) {
    (void)snprintf(error, error_size, "NXI%04d: %s", code, message);
  }
}

static int gptk_fail(nxinput_gptk *out, char *error, size_t error_size,
                     int code, const char *message) {
  if (out != 0) {
    memset(out, 0, sizeof *out);
  }
  gptk_set_error(error, error_size, code, message);
  return code;
}

/* Reject invalid UTF-8 and every byte below 0x20 except \n \r \t. The NUL
 * byte falls under "below 0x20", so embedded NULs are NXI1005 too. */
static int gptk_bytes_valid(const unsigned char *data, size_t length) {
  size_t i = 0u;

  while (i < length) {
    unsigned char byte = data[i];
    size_t continuation;
    unsigned long code_point;
    size_t k;

    if (byte < 0x20u) {
      if (byte != (unsigned char)'\n' && byte != (unsigned char)'\r' &&
          byte != (unsigned char)'\t') {
        return 0;
      }
      i += 1u;
      continue;
    }
    if (byte < 0x80u) {
      i += 1u;
      continue;
    }
    if (byte >= 0xC2u && byte <= 0xDFu) {
      continuation = 1u;
      code_point = byte & 0x1Fu;
    } else if (byte >= 0xE0u && byte <= 0xEFu) {
      continuation = 2u;
      code_point = byte & 0x0Fu;
    } else if (byte >= 0xF0u && byte <= 0xF4u) {
      continuation = 3u;
      code_point = byte & 0x07u;
    } else {
      return 0; /* stray continuation byte or overlong lead */
    }
    if (length - i <= continuation) {
      return 0; /* truncated sequence */
    }
    for (k = 1u; k <= continuation; k++) {
      unsigned char tail = data[i + k];
      if (tail < 0x80u || tail > 0xBFu) {
        return 0;
      }
      code_point = (code_point << 6) | (unsigned long)(tail & 0x3Fu);
    }
    if (continuation == 2u && code_point < 0x800ul) {
      return 0; /* overlong */
    }
    if (continuation == 3u &&
        (code_point < 0x10000ul || code_point > 0x10FFFFul)) {
      return 0; /* overlong or beyond Unicode */
    }
    if (code_point >= 0xD800ul && code_point <= 0xDFFFul) {
      return 0; /* surrogate */
    }
    i += continuation + 1u;
  }
  return 1;
}

static int gptk_is_space(char c) {
  return c == ' ' || c == '\t';
}

static const char *gptk_skip_space(const char *p, const char *end) {
  while (p < end && gptk_is_space(*p)) {
    p++;
  }
  return p;
}

/* [a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+ with total length <= 64. */
static int gptk_action_valid(const char *action, size_t length) {
  size_t i = 0u;
  size_t dots = 0u;

  if (length == 0u || length > NXINPUT_GPTK_ACTION_MAX) {
    return 0;
  }
  while (i < length) {
    char c = action[i];
    if (c < 'a' || c > 'z') {
      return 0; /* every segment starts with a lowercase letter */
    }
    i++;
    while (i < length && ((action[i] >= 'a' && action[i] <= 'z') ||
                          (action[i] >= '0' && action[i] <= '9') ||
                          action[i] == '_')) {
      i++;
    }
    if (i == length) {
      break;
    }
    if (action[i] != '.') {
      return 0;
    }
    dots++;
    i++;
    if (i == length) {
      return 0; /* trailing dot */
    }
  }
  return dots >= 1u;
}

static int gptk_lookup_control(const char *token, size_t length) {
  size_t i;

  for (i = 0u; i < (size_t)NXINPUT_GPTK_CONTROL_COUNT; i++) {
    if (strlen(gptk_controls[i].name) == length &&
        memcmp(gptk_controls[i].name, token, length) == 0) {
      return (int)i;
    }
  }
  return -1;
}

/* ---- V3 tuning ---------------------------------------------------- */

void nxinput_gptk_cursor_tuning_defaults(nxinput_gptk_cursor_tuning *t) {
  if (t == 0) {
    return;
  }
  memset(t, 0, sizeof *t);
  t->speed = 1.0f;
  t->deadzone = 0.15f;
  t->response_curve = 1.0f;
  t->acceleration = 0.0f;
  t->smoothing_ms = 0.0f;
}

void nxinput_gptk_camera_tuning_defaults(nxinput_gptk_camera_tuning *t) {
  if (t == 0) {
    return;
  }
  memset(t, 0, sizeof *t);
  t->sensitivity_x = 1.0f;
  t->sensitivity_y = 1.0f;
  t->deadzone = 0.15f;
  t->response_curve = 1.0f;
  t->invert_x = 0u;
  t->invert_y = 0u;
  t->authority = (uint8_t)NXINPUT_GPTK_AUTHORITY_NEXTOS;
}

void nxinput_gptk_cursor_tuning_get(const nxinput_gptk *g,
                                    nxinput_gptk_cursor_tuning *out) {
  if (out == 0) {
    return;
  }
  if (g == 0 || g->api_version != NXINPUT_GPTK_API_VERSION) {
    nxinput_gptk_cursor_tuning_defaults(out);
    return;
  }
  *out = g->cursor_tuning;
}

void nxinput_gptk_camera_tuning_get(const nxinput_gptk *g,
                                    nxinput_gptk_camera_tuning *out) {
  if (out == 0) {
    return;
  }
  if (g == 0 || g->api_version != NXINPUT_GPTK_API_VERSION) {
    nxinput_gptk_camera_tuning_defaults(out);
    return;
  }
  *out = g->camera_tuning;
}

/* Strict local decimal parser: [-]digits[.digits], nothing else. No strtod
 * (locale decimal-point surprises), no hex, no exponent, no nan/inf, no
 * empty token. Length is capped so the accumulation cannot overflow. */
static int gptk_parse_number(const char *s, size_t length, double *out) {
  size_t i = 0u;
  int negative = 0;
  int digits = 0;
  double value = 0.0;

  if (s == 0 || length == 0u || length > 24u) {
    return 0;
  }
  if (s[0] == '-') {
    negative = 1;
    i = 1u;
  }
  while (i < length && s[i] >= '0' && s[i] <= '9') {
    value = value * 10.0 + (double)(s[i] - '0');
    digits++;
    i++;
  }
  if (digits == 0) {
    return 0; /* rejects "-", ".5", "nan", "inf", "0x..." at the lead */
  }
  if (i < length && s[i] == '.') {
    double place = 0.1;
    int fraction_digits = 0;

    i++;
    while (i < length && s[i] >= '0' && s[i] <= '9') {
      value += (double)(s[i] - '0') * place;
      place *= 0.1;
      fraction_digits++;
      i++;
    }
    if (fraction_digits == 0) {
      return 0; /* "1." */
    }
  }
  if (i != length) {
    return 0; /* trailing garbage: "0x1p2", "1e3", "1.0f", "inf", ... */
  }
  *out = negative ? -value : value;
  return 1;
}

/* One numeric tuning key: duplicate -> NXI1003, malformed/out-of-bounds ->
 * NXI1002. Returns 0 when the key was stored. */
static int gptk_tuning_number(nxinput_gptk *out, char *error,
                              size_t error_size, const char *value,
                              size_t value_length, double min, double max,
                              float *slot, uint8_t *set_flag) {
  double parsed;

  if (*set_flag) {
    return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_DUPLICATE,
                     "duplicate tuning key");
  }
  if (!gptk_parse_number(value, value_length, &parsed)) {
    return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_MALFORMED,
                     "malformed tuning number");
  }
  if (parsed < min || parsed > max) {
    return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_MALFORMED,
                     "tuning value out of bounds");
  }
  *slot = (float)parsed;
  *set_flag = 1u;
  return 0;
}

/* One boolean tuning key: only "true" / "false". */
static int gptk_tuning_bool(nxinput_gptk *out, char *error, size_t error_size,
                            const char *value, size_t value_length,
                            uint8_t *slot, uint8_t *set_flag) {
  if (*set_flag) {
    return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_DUPLICATE,
                     "duplicate tuning key");
  }
  if (value_length == 4u && memcmp(value, "true", 4u) == 0) {
    *slot = 1u;
  } else if (value_length == 5u && memcmp(value, "false", 5u) == 0) {
    *slot = 0u;
  } else {
    return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_MALFORMED,
                     "invalid boolean (only true|false)");
  }
  *set_flag = 1u;
  return 0;
}

static int gptk_key_is(const char *key, size_t key_length, const char *name) {
  return strlen(name) == key_length && memcmp(key, name, key_length) == 0;
}

/* [cursor] tuning keys. Returns 0 stored, -1 not a cursor tuning key,
 * else the positive NXI code (out already failed closed). */
static int gptk_cursor_tuning_line(nxinput_gptk *out, char *error,
                                   size_t error_size, const char *key,
                                   size_t key_length, const char *value,
                                   size_t value_length) {
  nxinput_gptk_cursor_tuning *t = &out->cursor_tuning;

  if (gptk_key_is(key, key_length, "speed")) {
    return gptk_tuning_number(out, error, error_size, value, value_length,
                              (double)NXINPUT_GPTK_SPEED_MIN,
                              (double)NXINPUT_GPTK_SPEED_MAX, &t->speed,
                              &t->speed_set);
  }
  if (gptk_key_is(key, key_length, "deadzone")) {
    return gptk_tuning_number(out, error, error_size, value, value_length,
                              (double)NXINPUT_GPTK_DEADZONE_MIN,
                              (double)NXINPUT_GPTK_DEADZONE_MAX, &t->deadzone,
                              &t->deadzone_set);
  }
  if (gptk_key_is(key, key_length, "response_curve")) {
    return gptk_tuning_number(out, error, error_size, value, value_length,
                              (double)NXINPUT_GPTK_CURVE_MIN,
                              (double)NXINPUT_GPTK_CURVE_MAX,
                              &t->response_curve, &t->response_curve_set);
  }
  if (gptk_key_is(key, key_length, "acceleration")) {
    return gptk_tuning_number(out, error, error_size, value, value_length,
                              (double)NXINPUT_GPTK_ACCEL_MIN,
                              (double)NXINPUT_GPTK_ACCEL_MAX,
                              &t->acceleration, &t->acceleration_set);
  }
  if (gptk_key_is(key, key_length, "smoothing_ms")) {
    return gptk_tuning_number(out, error, error_size, value, value_length,
                              (double)NXINPUT_GPTK_SMOOTHING_MIN,
                              (double)NXINPUT_GPTK_SMOOTHING_MAX,
                              &t->smoothing_ms, &t->smoothing_ms_set);
  }
  return -1;
}

/* [camera] tuning keys (the section holds ONLY tuning keys). Returns 0
 * stored, else the positive NXI code. */
static int gptk_camera_tuning_line(nxinput_gptk *out, char *error,
                                   size_t error_size, const char *key,
                                   size_t key_length, const char *value,
                                   size_t value_length) {
  nxinput_gptk_camera_tuning *t = &out->camera_tuning;

  if (gptk_key_is(key, key_length, "sensitivity_x")) {
    return gptk_tuning_number(out, error, error_size, value, value_length,
                              (double)NXINPUT_GPTK_SENSITIVITY_MIN,
                              (double)NXINPUT_GPTK_SENSITIVITY_MAX,
                              &t->sensitivity_x, &t->sensitivity_x_set);
  }
  if (gptk_key_is(key, key_length, "sensitivity_y")) {
    return gptk_tuning_number(out, error, error_size, value, value_length,
                              (double)NXINPUT_GPTK_SENSITIVITY_MIN,
                              (double)NXINPUT_GPTK_SENSITIVITY_MAX,
                              &t->sensitivity_y, &t->sensitivity_y_set);
  }
  if (gptk_key_is(key, key_length, "deadzone")) {
    return gptk_tuning_number(out, error, error_size, value, value_length,
                              (double)NXINPUT_GPTK_DEADZONE_MIN,
                              (double)NXINPUT_GPTK_DEADZONE_MAX, &t->deadzone,
                              &t->deadzone_set);
  }
  if (gptk_key_is(key, key_length, "response_curve")) {
    return gptk_tuning_number(out, error, error_size, value, value_length,
                              (double)NXINPUT_GPTK_CURVE_MIN,
                              (double)NXINPUT_GPTK_CURVE_MAX,
                              &t->response_curve, &t->response_curve_set);
  }
  if (gptk_key_is(key, key_length, "invert_x")) {
    return gptk_tuning_bool(out, error, error_size, value, value_length,
                            &t->invert_x, &t->invert_x_set);
  }
  if (gptk_key_is(key, key_length, "invert_y")) {
    return gptk_tuning_bool(out, error, error_size, value, value_length,
                            &t->invert_y, &t->invert_y_set);
  }
  if (gptk_key_is(key, key_length, "authority")) {
    if (t->authority_set) {
      return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_DUPLICATE,
                       "duplicate tuning key");
    }
    if (value_length == 6u && memcmp(value, "nextos", 6u) == 0) { /* authority value */
      t->authority = (uint8_t)NXINPUT_GPTK_AUTHORITY_NEXTOS;
    } else if (value_length == 6u && memcmp(value, "native", 6u) == 0) {
      t->authority = (uint8_t)NXINPUT_GPTK_AUTHORITY_NATIVE;
    } else {
      return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_MALFORMED,
                       "invalid authority (only nextos|native)");
    }
    t->authority_set = 1u;
    return 0;
  }
  return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_UNKNOWN_NAME,
                   "unknown tuning key");
}

static int gptk_lookup_section(const char *token, size_t length) {
  size_t i;

  for (i = 0u; i < (size_t)NXINPUT_GPTK_CONTEXT_COUNT; i++) {
    if (strlen(gptk_sections[i]) == length &&
        memcmp(gptk_sections[i], token, length) == 0) {
      return (int)i;
    }
  }
  return -1;
}

int nxinput_gptk_parse(const char *text, size_t length, nxinput_gptk *out,
                       char *error, size_t error_size) {
  const char *cursor;
  const char *end;
  size_t line_count = 0u;
  int saw_magic = 0;
  int saw_port = 0;
  int section = -1;
  int in_camera = 0; /* [camera] is a tuning-only section, not a context */

  if (out == 0) {
    gptk_set_error(error, error_size, NXINPUT_GPTK_ERR_MALFORMED,
                   "no output structure");
    return NXINPUT_GPTK_ERR_MALFORMED;
  }
  memset(out, 0, sizeof *out);
  out->api_version = NXINPUT_GPTK_API_VERSION;
  nxinput_gptk_cursor_tuning_defaults(&out->cursor_tuning);
  nxinput_gptk_camera_tuning_defaults(&out->camera_tuning);

  if (text == 0) {
    return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_MALFORMED,
                     "no input buffer");
  }
  if (length > (size_t)NXINPUT_GPTK_MAX_BYTES) {
    return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_TOO_LARGE,
                     "input exceeds 65536 bytes");
  }
  if (!gptk_bytes_valid((const unsigned char *)text, length)) {
    return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_BAD_BYTES,
                     "invalid UTF-8 or forbidden control byte");
  }

  cursor = text;
  end = text + length;
  while (cursor < end) {
    const char *line_start = cursor;
    const char *line_end = memchr(cursor, '\n', (size_t)(end - cursor));
    const char *trimmed_start;
    const char *trimmed_end;
    size_t line_length;

    if (line_end == 0) {
      line_end = end;
      cursor = end;
    } else {
      cursor = line_end + 1;
    }
    line_count++;
    if (line_count > (size_t)NXINPUT_GPTK_MAX_LINES) {
      return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_TOO_LARGE,
                       "input exceeds 512 lines");
    }

    /* Trim spaces/tabs and a trailing carriage return. */
    if (line_end > line_start && line_end[-1] == '\r') {
      line_end--;
    }
    trimmed_start = gptk_skip_space(line_start, line_end);
    trimmed_end = line_end;
    while (trimmed_end > trimmed_start && gptk_is_space(trimmed_end[-1])) {
      trimmed_end--;
    }
    line_length = (size_t)(trimmed_end - trimmed_start);

    if (line_length == 0u || trimmed_start[0] == '#') {
      continue; /* blank line or comment */
    }

    if (!saw_magic) {
      /* First non-comment line must be exactly the magic, no variants. */
      if (line_length != strlen(NXINPUT_GPTK_MAGIC) ||
          memcmp(trimmed_start, NXINPUT_GPTK_MAGIC, line_length) != 0) {
        return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_BAD_MAGIC,
                         "missing or wrong format magic");
      }
      saw_magic = 1;
      continue;
    }

    if (trimmed_start[0] == '[') {
      int found;

      if (line_length < 3u || trimmed_start[line_length - 1u] != ']') {
        return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_MALFORMED,
                         "malformed section header");
      }
      if (line_length - 2u == 6u &&
          memcmp(trimmed_start + 1, "camera", 6u) == 0) {
        /* Tuning-only section: no control->action lines here. */
        in_camera = 1;
        section = -1;
        out->camera_present = 1;
        continue;
      }
      found = gptk_lookup_section(trimmed_start + 1, line_length - 2u);
      if (found < 0) {
        return gptk_fail(out, error, error_size,
                         NXINPUT_GPTK_ERR_UNKNOWN_NAME, "unknown section");
      }
      in_camera = 0;
      section = found;
      out->context_present[found] = 1;
      continue;
    }

    /* key = value */
    {
      const char *equals = memchr(trimmed_start, '=', line_length);
      const char *key_end;
      const char *value_start;
      size_t key_length;
      size_t value_length;

      if (equals == 0 || equals == trimmed_start) {
        return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_MALFORMED,
                         "malformed line (expected CONTROL = action)");
      }
      key_end = equals;
      while (key_end > trimmed_start && gptk_is_space(key_end[-1])) {
        key_end--;
      }
      value_start = gptk_skip_space(equals + 1, trimmed_end);
      key_length = (size_t)(key_end - trimmed_start);
      value_length = (size_t)(trimmed_end - value_start);
      if (key_length == 0u || value_length == 0u) {
        return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_MALFORMED,
                         "malformed line (empty key or value)");
      }

      if (in_camera) {
        int tuning = gptk_camera_tuning_line(out, error, error_size,
                                             trimmed_start, key_length,
                                             value_start, value_length);

        if (tuning != 0) {
          return tuning; /* out already failed closed */
        }
        continue;
      }

      if (section < 0) {
        /* Only "port = <id>" may appear between magic and first section. */
        if (key_length == 4u && memcmp(trimmed_start, "port", 4u) == 0) {
          size_t i;

          if (saw_port) {
            return gptk_fail(out, error, error_size,
                             NXINPUT_GPTK_ERR_DUPLICATE,
                             "duplicate port line");
          }
          if (value_length > (size_t)NXINPUT_GPTK_PORT_MAX) {
            return gptk_fail(out, error, error_size,
                             NXINPUT_GPTK_ERR_MALFORMED, "port id too long");
          }
          for (i = 0u; i < value_length; i++) {
            char c = value_start[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '.')) {
              return gptk_fail(out, error, error_size,
                               NXINPUT_GPTK_ERR_MALFORMED,
                               "invalid port id character");
            }
          }
          memcpy(out->port, value_start, value_length);
          out->port[value_length] = '\0';
          saw_port = 1;
          continue;
        }
        return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_MALFORMED,
                         "mapping line before any section");
      }

      {
        int control = gptk_lookup_control(trimmed_start, key_length);

        if (control < 0) {
          if (section == (int)NXINPUT_GPTK_CONTEXT_CURSOR) {
            /* [cursor] also accepts the numeric tuning keys. */
            int tuning = gptk_cursor_tuning_line(out, error, error_size,
                                                 trimmed_start, key_length,
                                                 value_start, value_length);

            if (tuning == 0) {
              continue;
            }
            if (tuning > 0) {
              return tuning; /* out already failed closed */
            }
            return gptk_fail(out, error, error_size,
                             NXINPUT_GPTK_ERR_UNKNOWN_NAME,
                             "unknown tuning key");
          }
          /* Digits-only tokens land here as well: numeric evdev codes are
           * rejected by design (Chrono Trigger L2/R2 regression). Tuning
           * keys outside [cursor]/[camera] are unknown names too. */
          return gptk_fail(out, error, error_size,
                           NXINPUT_GPTK_ERR_UNKNOWN_NAME,
                           "unknown control name (symbolic names only)");
        }
        if (out->action[section][control][0] != '\0') {
          return gptk_fail(out, error, error_size,
                           NXINPUT_GPTK_ERR_DUPLICATE,
                           "duplicate control in section");
        }
        if (!gptk_action_valid(value_start, value_length)) {
          return gptk_fail(out, error, error_size,
                           NXINPUT_GPTK_ERR_MALFORMED,
                           "invalid action name");
        }
        memcpy(out->action[section][control], value_start, value_length);
        out->action[section][control][value_length] = '\0';
      }
    }
  }

  if (!saw_magic) {
    return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_BAD_MAGIC,
                     "missing or wrong format magic");
  }
  if (!out->context_present[NXINPUT_GPTK_CONTEXT_MENU] ||
      !out->context_present[NXINPUT_GPTK_CONTEXT_GAMEPLAY]) {
    return gptk_fail(out, error, error_size, NXINPUT_GPTK_ERR_MALFORMED,
                     "missing required [menu] or [gameplay] section");
  }
  return 0;
}

const char *nxinput_gptk_action(const nxinput_gptk *g,
                                nxinput_gptk_context c, int control) {
  if (g == 0 || (int)c < 0 || (int)c >= (int)NXINPUT_GPTK_CONTEXT_COUNT ||
      control < 0 || control >= (int)NXINPUT_GPTK_CONTROL_COUNT) {
    return 0;
  }
  if (g->action[c][control][0] == '\0') {
    return 0;
  }
  return g->action[c][control];
}

int nxinput_gptk_validate_actions(const nxinput_gptk *g,
                                  const char *const *allowed,
                                  size_t allowed_count, char *error,
                                  size_t error_size) {
  int c;
  int k;

  if (g == 0) {
    gptk_set_error(error, error_size, NXINPUT_GPTK_ERR_MALFORMED,
                   "no mapping to validate");
    return NXINPUT_GPTK_ERR_MALFORMED;
  }
  for (c = 0; c < (int)NXINPUT_GPTK_CONTEXT_COUNT; c++) {
    for (k = 0; k < (int)NXINPUT_GPTK_CONTROL_COUNT; k++) {
      const char *action = g->action[c][k];
      size_t i;
      int known = 0;

      if (action[0] == '\0') {
        continue;
      }
      for (i = 0u; allowed != 0 && i < allowed_count; i++) {
        if (allowed[i] != 0 && strcmp(allowed[i], action) == 0) {
          known = 1;
          break;
        }
      }
      if (!known) {
        if (error != 0 && error_size > 0u) {
          (void)snprintf(error, error_size,
                         "NXI%04d: action not in adapter allowlist: %s",
                         NXINPUT_GPTK_ERR_UNKNOWN_NAME, action);
        }
        return NXINPUT_GPTK_ERR_UNKNOWN_NAME;
      }
    }
  }
  return 0;
}

int nxinput_gptk_control_button(int control) {
  if (control < 0 || control >= (int)NXINPUT_GPTK_CONTROL_COUNT) {
    return -1;
  }
  return gptk_controls[control].button;
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                          */
/* ------------------------------------------------------------------ */

void nxinput_gptk_dispatcher_init(nxinput_gptk_dispatcher *d,
                                  const nxinput_gptk *g) {
  if (d == 0) {
    return;
  }
  memset(d, 0, sizeof *d);
  d->map = g;
  d->context = NXINPUT_GPTK_CONTEXT_MENU;
}

int nxinput_gptk_dispatcher_register(nxinput_gptk_dispatcher *d,
                                     const char *action,
                                     nxinput_gptk_sink_fn fn, void *user) {
  size_t length;

  if (d == 0 || action == 0 || fn == 0) {
    return -1;
  }
  length = strlen(action);
  if (length == 0u || length > NXINPUT_GPTK_ACTION_MAX) {
    return -1;
  }
  if (d->sink_count >= (size_t)NXINPUT_GPTK_MAX_SINKS) {
    return -1;
  }
  memcpy(d->sinks[d->sink_count].action, action, length + 1u);
  d->sinks[d->sink_count].fn = fn;
  d->sinks[d->sink_count].user = user;
  d->sink_count++;
  return 0;
}

/* One logical action event fans out to every sink registered for it. */
static void gptk_emit(nxinput_gptk_dispatcher *d, const char *action,
                      int pressed, float value) {
  size_t i;

  for (i = 0u; i < d->sink_count; i++) {
    if (strcmp(d->sinks[i].action, action) == 0) {
      d->sinks[i].fn(d->sinks[i].user, action, pressed, value);
    }
  }
}

void nxinput_gptk_dispatcher_set_context(nxinput_gptk_dispatcher *d,
                                         nxinput_gptk_context c) {
  int control;

  if (d == 0 || (int)c < 0 || (int)c >= (int)NXINPUT_GPTK_CONTEXT_COUNT) {
    return;
  }
  /* Release every latched control into the OLD context before switching, so
   * no sink is left holding a press whose release would otherwise land in a
   * different action namespace. */
  for (control = 0; control < (int)NXINPUT_GPTK_CONTROL_COUNT; control++) {
    if ((d->latched & (UINT32_C(1) << (unsigned int)control)) != 0u) {
      const char *action = nxinput_gptk_action(d->map, d->context, control);

      if (action != 0) {
        gptk_emit(d, action, 0, 0.0f);
      }
    }
  }
  d->latched = 0u;
  d->context = c;
  /* V3 (blocker 7): a context switch also drops accumulated cursor velocity
   * and smoothing so no residual motion leaks across the boundary (position
   * is preserved). */
  d->cursor_vel_x = 0.0f;
  d->cursor_vel_y = 0.0f;
}

int nxinput_gptk_dispatcher_configure_motion(nxinput_gptk_dispatcher *d,
                                             int drawable_w, int drawable_h) {
  if (d == 0 || drawable_w <= 0 || drawable_h <= 0) {
    return -1;
  }
  d->drawable_w = drawable_w;
  d->drawable_h = drawable_h;
  d->cursor_x = (float)drawable_w * 0.5f;
  d->cursor_y = (float)drawable_h * 0.5f;
  d->cursor_vel_x = 0.0f;
  d->cursor_vel_y = 0.0f;
  d->motion_configured = 1;
  return 0;
}

int nxinput_gptk_dispatcher_register_vector(nxinput_gptk_dispatcher *d,
                                            const char *action,
                                            nxinput_gptk_vector_sink_fn fn,
                                            void *user) {
  size_t length;

  if (d == 0 || action == 0 || fn == 0) {
    return -1;
  }
  length = strlen(action);
  if (length == 0u || length > NXINPUT_GPTK_ACTION_MAX) {
    return -1;
  }
  if (d->vector_sink_count >= (size_t)NXINPUT_GPTK_MAX_SINKS) {
    return -1;
  }
  memcpy(d->vector_sinks[d->vector_sink_count].action, action, length + 1u);
  d->vector_sinks[d->vector_sink_count].fn = fn;
  d->vector_sinks[d->vector_sink_count].user = user;
  d->vector_sink_count++;
  return 0;
}

static void gptk_emit_vector(nxinput_gptk_dispatcher *d, const char *action,
                             float ax, float ay) {
  size_t i;

  for (i = 0u; i < d->vector_sink_count; i++) {
    if (strcmp(d->vector_sinks[i].action, action) == 0) {
      d->vector_sinks[i].fn(d->vector_sinks[i].user, action, ax, ay);
    }
  }
}

static int gptk_is_stick(int control) {
  return control == (int)NXINPUT_GPTK_LEFT_STICK ||
         control == (int)NXINPUT_GPTK_RIGHT_STICK;
}

void nxinput_gptk_dispatcher_feed_stick(nxinput_gptk_dispatcher *d, int control,
                                        float x, float y, float dt_seconds) {
  const char *action;

  if (d == 0 || !gptk_is_stick(control)) {
    return;
  }
  action = nxinput_gptk_action(d->map, d->context, control);
  if (action == 0) {
    return;
  }
  if (strncmp(action, "cursor.", 7u) == 0) {
    nxinput_gptk_cursor_tuning tuning;
    nxinput_gptk_cursor_state state;

    if (!d->motion_configured) {
      return;
    }
    nxinput_gptk_cursor_tuning_get(d->map, &tuning);
    state.x = d->cursor_x;
    state.y = d->cursor_y;
    state.vel_x = d->cursor_vel_x;
    state.vel_y = d->cursor_vel_y;
    if (nxinput_gptk_cursor_step(&tuning, x, y, dt_seconds, d->drawable_w,
                                 d->drawable_h, &state) == 0) {
      d->cursor_x = state.x;
      d->cursor_y = state.y;
      d->cursor_vel_x = state.vel_x;
      d->cursor_vel_y = state.vel_y;
      gptk_emit_vector(d, action, state.x, state.y);
    }
  } else if (strncmp(action, "camera.", 7u) == 0) {
    nxinput_gptk_camera_tuning tuning;
    float out_x = 0.0f;
    float out_y = 0.0f;

    nxinput_gptk_camera_tuning_get(d->map, &tuning);
    if (nxinput_gptk_camera_transform(&tuning, x, y, &out_x, &out_y) == 0) {
      gptk_emit_vector(d, action, out_x, out_y);
    }
  }
}

uint32_t nxinput_gptk_dispatcher_suppressed_mask(
    const nxinput_gptk_dispatcher *d) {
  int i;
  uint32_t mask = 0u;
  static const int sticks[2] = {(int)NXINPUT_GPTK_LEFT_STICK,
                                (int)NXINPUT_GPTK_RIGHT_STICK};

  if (d == 0) {
    return 0u;
  }
  /* Set a bit ONLY for the specific stick the live context maps to a cursor.*
   * or camera.* action. A game that hands only the right stick to the camera
   * gets exactly bit RIGHT_STICK -- its native left stick is never in the mask
   * and the adapter must not suppress it. */
  for (i = 0; i < 2; i++) {
    const char *action = nxinput_gptk_action(d->map, d->context, sticks[i]);

    if (action != 0 && (strncmp(action, "cursor.", 7u) == 0 ||
                        strncmp(action, "camera.", 7u) == 0)) {
      mask |= (uint32_t)1u << (unsigned)sticks[i];
    }
  }
  return mask;
}

int nxinput_gptk_dispatcher_control_suppressed(
    const nxinput_gptk_dispatcher *d, int control) {
  if (control < 0 || control >= (int)NXINPUT_GPTK_CONTROL_COUNT) {
    return 0;
  }
  return (nxinput_gptk_dispatcher_suppressed_mask(d) &
          ((uint32_t)1u << (unsigned)control)) != 0u
             ? 1
             : 0;
}

int nxinput_gptk_dispatcher_physical_suppressed(
    const nxinput_gptk_dispatcher *d) {
  return nxinput_gptk_dispatcher_suppressed_mask(d) != 0u ? 1 : 0;
}

void nxinput_gptk_dispatcher_feed(nxinput_gptk_dispatcher *d, int control,
                                  int pressed, float value) {
  uint32_t bit;
  const char *action;

  if (d == 0 || control < 0 ||
      control >= (int)NXINPUT_GPTK_CONTROL_COUNT) {
    return;
  }
  if (value < 0.0f) {
    value = 0.0f;
  } else if (value > 1.0f) {
    value = 1.0f;
  }
  bit = UINT32_C(1) << (unsigned int)control;
  action = nxinput_gptk_action(d->map, d->context, control);

  if (pressed) {
    if ((d->latched & bit) != 0u) {
      return; /* edge-triggered: repeats of an active press emit nothing */
    }
    d->latched |= bit;
    if (action != 0) {
      gptk_emit(d, action, 1, value);
    }
  } else {
    if ((d->latched & bit) == 0u) {
      return; /* release without a latched press emits nothing */
    }
    d->latched &= ~bit;
    if (action != 0) {
      gptk_emit(d, action, 0, value);
    }
  }
}
