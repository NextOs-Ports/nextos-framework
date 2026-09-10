/* SPDX-License-Identifier: GPL-3.0-only */
/* Corpus driver for the C3 sovereign gates. Reads files prepared by the
 * orchestrator; never touches SDL, devices or the network.
 *
 *   parse-db FILE           validate every mapping line; print counts.
 *   emit-all FILE           resolve EVERY GUID entry of the database and
 *                           print, for each one, ONLY what the runtime
 *                           consumer ended up holding after the real
 *                           setter+readback. The driver states no
 *                           expectation: the independent reference in
 *                           sovereign_corpus_gate.py decides what each entry
 *                           should be and compares.
 *   unreachable FILE GUID B A H
 *                           resolve GUID against DELIBERATELY insufficient
 *                           capabilities and print the outcome. */
#include "nxinput_sovereign.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, size_t *size_out) {
  FILE *stream = fopen(path, "rb");
  char *data;
  long size;
  if (stream == NULL) {
    return NULL;
  }
  if (fseek(stream, 0, SEEK_END) != 0 || (size = ftell(stream)) < 0 ||
      fseek(stream, 0, SEEK_SET) != 0) {
    fclose(stream);
    return NULL;
  }
  data = (char *)malloc((size_t)size + 1u);
  if (data == NULL) {
    fclose(stream);
    return NULL;
  }
  if (fread(data, 1u, (size_t)size, stream) != (size_t)size) {
    free(data);
    fclose(stream);
    return NULL;
  }
  fclose(stream);
  data[size] = '\0';
  if (size_out != NULL) {
    *size_out = (size_t)size;
  }
  return data;
}

/* The runtime consumer: it holds whatever the adapter pushed into it and
 * reports that back. It contains NO knowledge of the mapping dialect and no
 * expectation -- it is a store, so what the gate reads back is evidence of
 * what the resolver actually applied. */
struct runtime_store {
  char held[NXINPUT_SOVEREIGN_LINE_MAX];
  int has_line;
};

static int readback_store(void *userdata, const char *line, char *out,
                          size_t cap) {
  struct runtime_store *store = (struct runtime_store *)userdata;
  if (line[0] == '\0') {
    return -1; /* no built-in database in this driver */
  }
  (void)snprintf(store->held, sizeof store->held, "%s", line);
  store->has_line = 1;
  (void)snprintf(out, cap, "%s", store->held);
  return 0;
}

/* Largest ordinal + 1 referenced by a line, per class. The gate derives the
 * same numbers independently and fails when the two disagree. */
static void caps_of_line(const char *line, int *buttons, int *axes,
                         int *hats) {
  size_t i;
  size_t len = strlen(line);
  *buttons = 1;
  *axes = 1;
  *hats = 1;
  for (i = 0u; i + 1u < len; i++) {
    int *target = NULL;
    int value = 0;
    size_t j;
    if (line[i] != ':' && line[i] != '+' && line[i] != '-') {
      continue;
    }
    if (line[i + 1u] == 'b') {
      target = buttons;
    } else if (line[i + 1u] == 'a') {
      target = axes;
    } else if (line[i + 1u] == 'h') {
      target = hats;
    } else {
      continue;
    }
    j = i + 2u;
    if (j >= len || line[j] < '0' || line[j] > '9') {
      continue;
    }
    while (j < len && line[j] >= '0' && line[j] <= '9') {
      value = value * 10 + (line[j] - '0');
      j++;
    }
    if (value + 1 > *target) {
      *target = value + 1;
    }
  }
}

static int cmd_parse_db(const char *path) {
  size_t size;
  char *data = read_file(path, &size);
  char *cursor;
  unsigned int total = 0u, ok = 0u, bad = 0u, comments = 0u, foreign = 0u;
  if (data == NULL) {
    fprintf(stderr, "unreadable database\n");
    return 2;
  }
  cursor = data;
  while (*cursor != '\0') {
    char *eol = strchr(cursor, '\n');
    if (eol != NULL) {
      *eol = '\0';
    }
    {
      size_t len = strlen(cursor);
      while (len > 0u && (cursor[len - 1u] == '\r' || cursor[len - 1u] == ' ')) {
        cursor[--len] = '\0';
      }
      if (len == 0u || cursor[0] == '#') {
        comments++;
      } else {
        /* Upstream databases carry special non-GUID entries (e.g. the
         * literal `xinput,...` Windows convention). They are not our
         * SDL2/Linux dialect and can never win a GUID lookup; classify
         * them honestly as foreign instead of pretending corruption. */
        int is_guid_line = 1;
        size_t i;
        for (i = 0u; i < 32u; i++) {
          char c = cursor[i];
          if (c == '\0' ||
              !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            is_guid_line = 0;
            break;
          }
        }
        if (is_guid_line && cursor[32] != ',') {
          is_guid_line = 0;
        }
        if (!is_guid_line) {
          foreign++;
        } else {
          total++;
          if (nxinput_sovereign_line_syntax(cursor) == NXINPUT_SOVEREIGN_OK) {
            ok++;
          } else {
            bad++;
            printf("BADLINE %.60s\n", cursor);
          }
        }
      }
    }
    if (eol == NULL) {
      break;
    }
    cursor = eol + 1;
  }
  printf("PARSE-DB lines=%u ok=%u bad=%u foreign=%u comments=%u\n", total,
         ok, bad, foreign, comments);
  free(data);
  /* Report-only: the orchestrator freezes the allowed bad-line count, so a
   * real upstream typo is catalogued without hiding growth. */
  return 0;
}

/* Resolve one GUID and print, with no verdict of its own, the outcome and
 * exactly what the runtime consumer holds afterwards. */
static void emit_one(const char *db, const char *guid, int buttons, int axes,
                     int hats) {
  nxinput_sovereign_request request;
  nxinput_sovereign_decision decision;
  struct runtime_store store;

  memset(&store, 0, sizeof store);
  (void)nxinput_sovereign_request_init(&request);
  (void)snprintf(request.guid, sizeof request.guid, "%s", guid);
  request.caps.buttons = buttons;
  request.caps.axes = axes;
  request.caps.hats = hats;
  if (nxinput_sovereign_resolve(&request, NULL, db, NULL, readback_store,
                                &store, &decision) != 0) {
    printf("EMIT guid=%s caps=%d/%d/%d source=none reason=request-invalid "
           "readback=\n",
           guid, buttons, axes, hats);
    return;
  }
  printf("EMIT guid=%s caps=%d/%d/%d source=%s reason=%s readback=%s\n",
         guid, buttons, axes, hats,
         nxinput_sovereign_source_name(decision.source),
         nxinput_sovereign_reason_name(
             decision.source == NXINPUT_SOVEREIGN_FAIL_EXPLICIT
                 ? decision.step_reason[NXINPUT_SOVEREIGN_CFW_DB_GUID]
                 : NXINPUT_SOVEREIGN_OK),
         store.has_line ? store.held : "");
}

static int cmd_emit_all(const char *path) {
  size_t size;
  char *data = read_file(path, &size);
  char *scan;
  char *db;
  unsigned int emitted = 0u;

  if (data == NULL) {
    fprintf(stderr, "unreadable database\n");
    return 2;
  }
  db = read_file(path, NULL);
  if (db == NULL) {
    free(data);
    return 2;
  }
  scan = data;
  while (*scan != '\0') {
    char *eol = strchr(scan, '\n');
    size_t len = eol ? (size_t)(eol - scan) : strlen(scan);
    int is_guid = len > 33u;
    size_t i;
    for (i = 0u; is_guid && i < 32u; i++) {
      char c = scan[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
        is_guid = 0;
      }
    }
    if (is_guid && scan[32] != ',') {
      is_guid = 0;
    }
    if (is_guid) {
      char guid[NXINPUT_SOVEREIGN_GUID_MAX];
      char line[NXINPUT_SOVEREIGN_LINE_MAX];
      int buttons, axes, hats;
      size_t copy = len < sizeof line ? len : sizeof line - 1u;
      memcpy(guid, scan, 32u);
      guid[32] = '\0';
      memcpy(line, scan, copy);
      line[copy] = '\0';
      caps_of_line(line, &buttons, &axes, &hats);
      emit_one(db, guid, buttons, axes, hats);
      emitted++;
    }
    if (!eol) {
      break;
    }
    scan = eol + 1;
  }
  printf("EMIT-ALL entries=%u\n", emitted);
  free(db);
  free(data);
  return 0;
}

static int cmd_unreachable(const char *path, const char *guid, int buttons,
                           int axes, int hats) {
  char *db = read_file(path, NULL);
  if (db == NULL) {
    fprintf(stderr, "unreadable database\n");
    return 2;
  }
  emit_one(db, guid, buttons, axes, hats);
  free(db);
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "parse-db") == 0) {
    return cmd_parse_db(argv[2]);
  }
  if (argc == 3 && strcmp(argv[1], "emit-all") == 0) {
    return cmd_emit_all(argv[2]);
  }
  if (argc == 7 && strcmp(argv[1], "unreachable") == 0) {
    return cmd_unreachable(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]),
                           atoi(argv[6]));
  }
  fprintf(stderr,
          "usage: sovereign_corpus_driver parse-db FILE | emit-all FILE | "
          "unreachable FILE GUID BUTTONS AXES HATS\n");
  return 2;
}
