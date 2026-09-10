/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * corpus_replay_settings.c -- deterministic adversarial corpus replay for
 * the NEXTOSSETTINGS.txt parser (V3-HARDENING-01).
 *
 * Not a fuzzer: every `ok-*` / `bad-*` file under the corpus directory
 * (argv[1]) is replayed through the real nxcompat_settings_parse.  The
 * verdict must match the filename prefix: ok-* parse to 0; bad-* return
 * nonzero AND leave the output struct at the documented safe defaults
 * (language "auto", quality "auto", unknown_key_count 0).  unknown keys are
 * rejected fail-closed (V3-SETTINGS-01), so they live under bad-* (kept
 * non-fatal by contract).  Never crashes, never hangs; stdout is exactly
 * the replayed-file count for the host gate's `corpus=<N>` marker.
 */
#define _POSIX_C_SOURCE 200809L

#include "nxcompat_settings.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPLAY_MAX_BYTES (128u * 1024u) /* hard read cap per corpus file */
#define REPLAY_MIN_FILES 10u            /* guard against an emptied corpus */

static int has_prefix(const char *name, const char *prefix) {
  return strncmp(name, prefix, strlen(prefix)) == 0;
}

static unsigned unknown_seen;

static void count_unknown(const char *key, size_t key_len, void *user_data) {
  (void)key;
  (void)key_len;
  (void)user_data;
  unknown_seen++;
}

static int settings_is_default(const nxcompat_settings *s) {
  return s->api_version == NXCOMPAT_SETTINGS_API_VERSION &&
         strcmp(s->language, "auto") == 0 && strcmp(s->quality, "auto") == 0 &&
         s->unknown_key_count == 0u;
}

static int replay_one(const char *directory, const char *name, int expect_ok) {
  static char buffer[REPLAY_MAX_BYTES + 1u];
  char path[4096];
  nxcompat_settings parsed;
  FILE *stream;
  size_t length;
  int code;

  if (snprintf(path, sizeof path, "%s/%s", directory, name) >=
      (int)sizeof path) {
    fprintf(stderr, "corpus_replay_settings: path too long: %s\n", name);
    return -1;
  }
  stream = fopen(path, "rb");
  if (stream == NULL) {
    fprintf(stderr, "corpus_replay_settings: cannot open %s: %s\n", path,
            strerror(errno));
    return -1;
  }
  length = fread(buffer, 1u, sizeof buffer, stream);
  if (ferror(stream) != 0) {
    fprintf(stderr, "corpus_replay_settings: read error on %s\n", path);
    fclose(stream);
    return -1;
  }
  fclose(stream);
  if (length > REPLAY_MAX_BYTES) {
    fprintf(stderr,
            "corpus_replay_settings: %s exceeds the %u byte replay cap\n",
            name, (unsigned)REPLAY_MAX_BYTES);
    return -1;
  }

  /* Poison the output so "failure resets to defaults" is a real
   * observation, not leftover stack contents. */
  memset(&parsed, 0xA5, sizeof parsed);
  unknown_seen = 0u;
  code = nxcompat_settings_parse(buffer, length, &parsed, count_unknown, NULL);

  if (expect_ok) {
    if (code != 0) {
      fprintf(stderr,
              "corpus_replay_settings: %s expected ACCEPT, got %d\n", name,
              code);
      return -1;
    }
    if (parsed.api_version != NXCOMPAT_SETTINGS_API_VERSION ||
        parsed.language[0] == '\0' || parsed.quality[0] == '\0') {
      fprintf(stderr,
              "corpus_replay_settings: %s accepted with unfilled output\n",
              name);
      return -1;
    }
    if (0 &&
        (parsed.unknown_key_count == 0u || unknown_seen == 0u)) {
      fprintf(stderr,
              "corpus_replay_settings: %s must report unknown keys "
              "(count=%u callback=%u)\n",
              name, parsed.unknown_key_count, unknown_seen);
      return -1;
    }
    return 0;
  }

  if (code == 0) {
    fprintf(stderr, "corpus_replay_settings: %s expected REJECT, got 0\n",
            name);
    return -1;
  }
  if (!settings_is_default(&parsed)) {
    fprintf(stderr,
            "corpus_replay_settings: %s rejection did not reset the output "
            "to the safe defaults\n",
            name);
    return -1;
  }
  return 0;
}

int main(int argc, char **argv) {
  DIR *directory;
  struct dirent *entry;
  unsigned replayed = 0u;

  if (argc != 2) {
    fprintf(stderr, "usage: corpus_replay_settings <corpus-directory>\n");
    return 2;
  }
  directory = opendir(argv[1]);
  if (directory == NULL) {
    fprintf(stderr, "corpus_replay_settings: cannot open %s: %s\n", argv[1],
            strerror(errno));
    return 2;
  }
  while ((entry = readdir(directory)) != NULL) {
    int expect_ok;

    if (has_prefix(entry->d_name, "ok-")) {
      expect_ok = 1;
    } else if (has_prefix(entry->d_name, "bad-")) {
      expect_ok = 0;
    } else {
      continue; /* ".", "..", stray editor files */
    }
    if (replay_one(argv[1], entry->d_name, expect_ok) != 0) {
      closedir(directory);
      return 1;
    }
    replayed++;
  }
  closedir(directory);
  if (replayed < REPLAY_MIN_FILES) {
    fprintf(stderr,
            "corpus_replay_settings: only %u corpus files (need >= %u)\n",
            replayed, (unsigned)REPLAY_MIN_FILES);
    return 1;
  }
  printf("%u\n", replayed);
  return 0;
}
