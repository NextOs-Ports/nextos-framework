/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * corpus_replay_gptk.c -- deterministic adversarial corpus replay for the
 * NEXTOSCONTROLLERS.gptk parser (V3-HARDENING-01).
 *
 * Not a fuzzer: every file under the corpus directory (argv[1]) is replayed
 * through the real nxinput_gptk_parse.  The contract is encoded in the file
 * name: `ok-*` must parse to 0, `bad-*` must be rejected with a positive
 * NXI code, a stable "NXI####:" error string, and a fully zeroed output
 * struct (fail closed).  The replayer never crashes and never hangs; any
 * contract violation exits nonzero naming the offending file.
 *
 * stdout carries exactly one line: the number of replayed corpus files, so
 * the host gate can append `corpus=<N>` to its PASS marker.  Diagnostics go
 * to stderr.
 */
#define _POSIX_C_SOURCE 200809L

#include "nxinput_gptk.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPLAY_MAX_BYTES (128u * 1024u) /* hard read cap per corpus file */
#define REPLAY_MIN_FILES 16u            /* guard against an emptied corpus */

static int has_prefix(const char *name, const char *prefix) {
  return strncmp(name, prefix, strlen(prefix)) == 0;
}

static int replay_one(const char *directory, const char *name, int expect_ok) {
  static char buffer[REPLAY_MAX_BYTES + 1u];
  static const nxinput_gptk zeroed; /* all-bytes-zero reference */
  char path[4096];
  char error[256];
  nxinput_gptk parsed;
  FILE *stream;
  size_t length;
  int code;

  if (snprintf(path, sizeof path, "%s/%s", directory, name) >=
      (int)sizeof path) {
    fprintf(stderr, "corpus_replay_gptk: path too long: %s\n", name);
    return -1;
  }
  stream = fopen(path, "rb");
  if (stream == NULL) {
    fprintf(stderr, "corpus_replay_gptk: cannot open %s: %s\n", path,
            strerror(errno));
    return -1;
  }
  length = fread(buffer, 1u, sizeof buffer, stream);
  if (ferror(stream) != 0) {
    fprintf(stderr, "corpus_replay_gptk: read error on %s\n", path);
    fclose(stream);
    return -1;
  }
  fclose(stream);
  if (length > REPLAY_MAX_BYTES) {
    fprintf(stderr, "corpus_replay_gptk: %s exceeds the %u byte replay cap\n",
            name, (unsigned)REPLAY_MAX_BYTES);
    return -1;
  }

  /* Poison the output so "rejection leaves the struct zeroed" is a real
   * observation and not a leftover from stack luck. */
  memset(&parsed, 0xA5, sizeof parsed);
  error[0] = '\0';
  code = nxinput_gptk_parse(buffer, length, &parsed, error, sizeof error);

  if (expect_ok) {
    if (code != 0) {
      fprintf(stderr,
              "corpus_replay_gptk: %s expected ACCEPT, got NXI%04d (%s)\n",
              name, code, error);
      return -1;
    }
    if (parsed.api_version != NXINPUT_GPTK_API_VERSION) {
      fprintf(stderr, "corpus_replay_gptk: %s accepted without api_version\n",
              name);
      return -1;
    }
    return 0;
  }

  if (code <= 0) {
    fprintf(stderr, "corpus_replay_gptk: %s expected REJECT, got %d\n", name,
            code);
    return -1;
  }
  if (strncmp(error, "NXI", 3u) != 0) {
    fprintf(stderr,
            "corpus_replay_gptk: %s rejected without a stable NXI code: %s\n",
            name, error);
    return -1;
  }
  if (memcmp(&parsed, &zeroed, sizeof parsed) != 0) {
    fprintf(stderr,
            "corpus_replay_gptk: %s rejection left data in the struct\n",
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
    fprintf(stderr, "usage: corpus_replay_gptk <corpus-directory>\n");
    return 2;
  }
  directory = opendir(argv[1]);
  if (directory == NULL) {
    fprintf(stderr, "corpus_replay_gptk: cannot open %s: %s\n", argv[1],
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
            "corpus_replay_gptk: only %u corpus files (need >= %u)\n",
            replayed, (unsigned)REPLAY_MIN_FILES);
    return 1;
  }
  printf("%u\n", replayed);
  return 0;
}
