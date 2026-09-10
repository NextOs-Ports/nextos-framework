#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "util.h"

static char g_gamedir[PATH_MAX] = ".";

int debugPrintf(const char *text, ...) {
  static int verbose = -1;
  if (verbose < 0) {
    const char *e = getenv("BB_VERBOSE");
    verbose = (e && *e && *e != '0');
  }
  if (!verbose) return 0;
  va_list list;
  va_start(list, text);
  int ret = vprintf(text, list);
  va_end(list);
  fflush(stdout);
  return ret;
}

int logPrintf(const char *text, ...) {
  va_list list;
  va_start(list, text);
  int ret = vprintf(text, list);
  va_end(list);
  fflush(stdout);
  return ret;
}

const char *bb_game_dir(void) {
  const char *env = getenv("BB_GAMEDIR");
  if (env && *env) return env;
  return g_gamedir;
}

int bb_game_path(char *dst, size_t dst_size, const char *relative) {
  if (!dst || dst_size == 0) return -1;
  const char *base = bb_game_dir();
  if (!relative || !*relative) {
    snprintf(dst, dst_size, "%s", base);
  } else if (relative[0] == '/') {
    snprintf(dst, dst_size, "%s", relative);
  } else {
    snprintf(dst, dst_size, "%s/%s", base, relative);
  }
  return 0;
}

int ret0(void) { return 0; }
int ret1(void) { return 1; }
int retm1(void) { return -1; }
