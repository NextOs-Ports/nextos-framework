/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-CONTROLLERS-03 / C5B: the positive control, driven by an oracle it did
 * not write.
 *
 * CLAIM CLASS: FIXTURE_HOST. This is a pure C program. It executes no engine
 * and it must never be reported as REAL_API_HOST -- the 116A audit refused
 * exactly that promotion.
 *
 * It reads the SEALED bundle itself, so the caller cannot substitute a line;
 * the caller only supplies GUID -> capability records, which come from a
 * kernel node (see build_guid_capability_corpus.py). For each GUID that has
 * an oracle it serves the official bytes EXACTLY as stored, to both engines,
 * and reports what happened. It never reauthors a line, and it never invents
 * a capability for a GUID it was not given one for. */
#include "nxinput_godot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 4096u
#define BPL (8u * (unsigned int)sizeof(unsigned long))
#define KEY_WORDS ((NXINPUT_GODOT_KEY_BITS + BPL - 1u) / BPL)
#define ABS_WORDS ((NXINPUT_GODOT_ABS_BITS + BPL - 1u) / BPL)

struct record {
  char guid[33];
  char shape[64];
  unsigned long keys[KEY_WORDS];
  unsigned long abs_bits[ABS_WORDS];
  nxinput_godot_absinfo absinfo[NXINPUT_GODOT_ABS_BITS];
};

static void set_bit(unsigned long *bits, unsigned int code) {
  bits[code / BPL] |= 1ul << (code % BPL);
}

/* "0,17,304,305" -> bits. Anything unparsable aborts the run: a corpus that
 * does not parse must not silently become an empty capability. */
static int set_codes(unsigned long *bits, unsigned int limit,
                     const char *list) {
  const char *p = list;

  if (*p == '\0') {
    return 1;
  }
  while (*p != '\0') {
    char *end;
    long value = strtol(p, &end, 10);

    if (end == p || value < 0 || (unsigned long)value >= limit) {
      return 0;
    }
    set_bit(bits, (unsigned int)value);
    p = end;
    if (*p == ';') {
      p++;
    } else if (*p != '\0') {
      return 0;
    }
  }
  return 1;
}

/* "0:-32768:32767:128;2:0:255:0" */
static int set_absinfo(nxinput_godot_absinfo *info, const char *list) {
  const char *p = list;

  while (*p != '\0') {
    char *end;
    long code = strtol(p, &end, 10);
    long lo, hi, flat;

    if (end == p || code < 0 || code >= NXINPUT_GODOT_ABS_BITS ||
        *end != ':') {
      return 0;
    }
    p = end + 1;
    lo = strtol(p, &end, 10);
    if (end == p || *end != ':') {
      return 0;
    }
    p = end + 1;
    hi = strtol(p, &end, 10);
    if (end == p || *end != ':') {
      return 0;
    }
    p = end + 1;
    flat = strtol(p, &end, 10);
    if (end == p) {
      return 0;
    }
    info[code].minimum = (int32_t)lo;
    info[code].maximum = (int32_t)hi;
    info[code].flat = (int32_t)flat;
    info[code].present = 1u;
    p = end;
    if (*p == ';') {
      p++;
    } else if (*p != '\0') {
      return 0;
    }
  }
  return 1;
}

static char *field(char *line, const char *key) {
  size_t klen = strlen(key);
  char *at = strstr(line, key);

  if (at == 0) {
    return 0;
  }
  return at + klen;
}

static int parse_record(char *line, struct record *out) {
  char *guid = field(line, "guid=");
  char *shape = field(line, " shape=");
  char *keys = field(line, " KEY=");
  char *abs_bits = field(line, " ABS=");
  char *absinfo = field(line, " ABSINFO=");
  char *cut;

  if (guid == 0 || shape == 0 || keys == 0 || abs_bits == 0 ||
      absinfo == 0) {
    return 0;
  }
  memset(out, 0, sizeof(*out));
  /* every field but the last ends at the next space */
  cut = strchr(guid, ' ');
  if (cut == 0 || (size_t)(cut - guid) != 32u) {
    return 0;
  }
  memcpy(out->guid, guid, 32u);
  cut = strchr(shape, ' ');
  if (cut == 0 || (size_t)(cut - shape) >= sizeof out->shape) {
    return 0;
  }
  memcpy(out->shape, shape, (size_t)(cut - shape));
  cut = strchr(keys, ' ');
  if (cut == 0) {
    return 0;
  }
  *cut = '\0';
  if (!set_codes(out->keys, NXINPUT_GODOT_KEY_BITS, keys)) {
    return 0;
  }
  cut = strchr(abs_bits, ' ');
  if (cut == 0) {
    return 0;
  }
  *cut = '\0';
  if (!set_codes(out->abs_bits, NXINPUT_GODOT_ABS_BITS, abs_bits)) {
    return 0;
  }
  cut = strpbrk(absinfo, " \r\n");
  if (cut != 0) {
    *cut = '\0';
  }
  return set_absinfo(out->absinfo, absinfo);
}

/* Find this GUID's line in the SEALED bundle and hand back its exact bytes,
 * delimiters included. The caller never gets to supply the line. */
static int bundle_line(const char *bundle_path, const char *guid, char *out,
                       size_t out_size) {
  FILE *f = fopen(bundle_path, "rb");
  char line[MAX_LINE];
  int found = 0;

  if (f == 0) {
    return 0;
  }
  while (fgets(line, (int)sizeof line, f) != 0) {
    size_t len = strlen(line);

    while (len > 0u && (line[len - 1u] == '\n' || line[len - 1u] == '\r')) {
      line[--len] = '\0';
    }
    if (len < 33u || strncmp(line, guid, 32u) != 0 || line[32] != ',') {
      continue;
    }
    if (len + 1u > out_size) {
      break;
    }
    memcpy(out, line, len + 1u);
    found = 1;
    break;
  }
  (void)fclose(f);
  return found;
}

static const char *serve_one(nxinput_godot_engine engine,
                             const struct record *rec, const char *line,
                             int *bytes_equal,
                             nxinput_godot_evidence *evidence) {
  nxinput_godot_origin origin;
  nxinput_godot_caps caps;
  char served[NXINPUT_GODOT_LINE_MAX];
  nxinput_godot_result result;

  /* The bundle IS the sovereign source and it is written in the SDL2
   * dialect: that is what the sealed header declares, not something this
   * program inferred from the line's contents. */
  if (nxinput_godot_origin_declare(&origin, NXINPUT_GODOT_DOMAIN_SDL2_EVDEV,
                                   "nxinput-authority-c3",
                                   "sealed-nxcontroller-profiles-1") != 0 ||
      nxinput_godot_caps_init(&caps, rec->keys, NXINPUT_GODOT_KEY_BITS,
                              rec->abs_bits, NXINPUT_GODOT_ABS_BITS) != 0 ||
      nxinput_godot_caps_set_absinfo(&caps, rec->absinfo,
                                     NXINPUT_GODOT_ABS_BITS) != 0) {
    *bytes_equal = 0;
    return "harness-error";
  }
  result = nxinput_godot_serve(engine, &origin, &caps, line, served,
                               sizeof served, evidence);
  *bytes_equal = strcmp(served, line) == 0;
  return nxinput_godot_result_name(result);
}

int main(int argc, char **argv) {
  char line[MAX_LINE];
  char official[NXINPUT_GODOT_LINE_MAX];
  FILE *corpus;
  unsigned int rows = 0u;

  if (argc != 3) {
    fprintf(stderr, "usage: %s <bundle> <corpus-records>\n", argv[0]);
    return 2;
  }
  corpus = fopen(argv[2], "r");
  if (corpus == 0) {
    fprintf(stderr, "cannot read the corpus records\n");
    return 2;
  }
  while (fgets(line, (int)sizeof line, corpus) != 0) {
    struct record rec;
    nxinput_godot_evidence ev3;
    nxinput_godot_evidence ev4;
    const char *r3;
    const char *r4;
    int eq3 = 0;
    int eq4 = 0;

    if (line[0] != 'R') {
      continue;
    }
    if (!parse_record(line, &rec)) {
      printf("CORPUS-ERROR unparsable record\n");
      (void)fclose(corpus);
      return 1;
    }
    if (!bundle_line(argv[1], rec.guid, official, sizeof official)) {
      printf("CORPUS guid=%s shape=%s verdict=NO_SUCH_LINE\n", rec.guid,
             rec.shape);
      continue;
    }
    r3 = serve_one(NXINPUT_GODOT_ENGINE_3, &rec, official, &eq3, &ev3);
    r4 = serve_one(NXINPUT_GODOT_ENGINE_4, &rec, official, &eq4, &ev4);
    printf("CORPUS guid=%s shape=%s bytes=%u godot3=%s intact3=%d "
           "godot4=%s intact4=%d buttons=%u axes=%u hats=%u meta=%u "
           "unreachable=%u reason3=%s\n",
           rec.guid, rec.shape, (unsigned int)strlen(official), r3, eq3, r4,
           eq4, ev3.button_bindings, ev3.axis_bindings, ev3.hat_bindings,
           ev3.metadata_fields, ev3.unreachable_bindings, ev3.reason);
    rows++;
  }
  (void)fclose(corpus);
  printf("CORPUS-TOTAL rows=%u\n", rows);
  return 0;
}
