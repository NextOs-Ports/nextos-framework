/*
 * util.c -- misc utility functions
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 * Adapted for Syberia ARM64 port
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

#define LOG_NAME "debug.log"

const char *ter_game_dir(void) {
  static char dir[4096];
  static int initialized;
  if (initialized) return dir;

  const char *configured = getenv("TER_GAMEDIR");
  if (configured && configured[0] == '/') {
    snprintf(dir, sizeof(dir), "%s", configured);
  } else if (!getcwd(dir, sizeof(dir))) {
    snprintf(dir, sizeof(dir), ".");
  }

  size_t len = strlen(dir);
  while (len > 1 && dir[len - 1] == '/') dir[--len] = '\0';
  initialized = 1;
  return dir;
}

int ter_game_path(char *out, size_t out_size, const char *relative) {
  if (!out || !out_size || !relative || relative[0] == '/') return -1;

  /* All callers use project-owned relative paths.  Refuse traversal anyway so
   * future JNI redirects cannot escape the selected game directory. */
  for (const char *p = relative; *p;) {
    while (*p == '/') p++;
    const char *start = p;
    while (*p && *p != '/') p++;
    if ((p - start) == 2 && start[0] == '.' && start[1] == '.') return -1;
  }

  int n = snprintf(out, out_size, "%s%s%s", ter_game_dir(),
                   relative[0] ? "/" : "", relative);
  return (n >= 0 && (size_t)n < out_size) ? 0 : -1;
}

int debugPrintf(const char *text, ...) {
  va_list list;

  const char *internal_log = getenv("TER_INTERNAL_LOG");
  FILE *f = NULL;
  char log_path[4096];
  if (internal_log && *internal_log) {
    if (internal_log[0] == '/') {
      snprintf(log_path, sizeof(log_path), "%s", internal_log);
    } else if (ter_game_path(log_path, sizeof(log_path), LOG_NAME) != 0) {
      log_path[0] = '\0';
    }
    if (log_path[0]) f = fopen(log_path, "a");
  }
  if (f) {
    va_start(list, text);
    vfprintf(f, text, list);
    va_end(list);
    fclose(f);
  }

  va_start(list, text);
  vfprintf(stderr, text, list);
  va_end(list);

  return 0;
}

int ret0(void) { return 0; }
int ret1(void) { return 1; }
int retm1(void) { return -1; }
