#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

// Case-insensitive fallback path resolver. Most Summertime assets are exact,
// but this keeps Android-style loose path lookups from becoming fatal.
static int ci_resolve(const char *path, char *out, size_t outsz) {
  char work[2048];
  if (snprintf(work, sizeof(work), "%s", path) >= (int)sizeof(work))
    return 0;

  char built[2048];
  size_t blen = 0;
  built[0] = '\0';

  char *p = work;
  if (*p == '/') {
    built[0] = '/';
    built[1] = '\0';
    blen = 1;
    p++;
  } else if (p[0] == '.' && p[1] == '/') {
    built[0] = '.';
    built[1] = '\0';
    blen = 1;
    p += 2;
  } else {
    built[0] = '.';
    built[1] = '\0';
    blen = 1;
  }

  char *save = NULL;
  for (char *tok = strtok_r(p, "/", &save); tok;
       tok = strtok_r(NULL, "/", &save)) {
    if (tok[0] == '\0')
      continue;
    int need_slash = (blen > 0 && built[blen - 1] != '/');
    // try exact first
    char cand[2048];
    snprintf(cand, sizeof(cand), "%s%s%s", built, need_slash ? "/" : "", tok);
    if (access(cand, F_OK) == 0) {
      snprintf(built, sizeof(built), "%s", cand);
      blen = strlen(built);
      continue;
    }
    // case-insensitive directory scan
    DIR *d = opendir(built);
    if (!d)
      return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
      if (strcasecmp(e->d_name, tok) == 0) {
        int n = snprintf(built + blen, sizeof(built) - blen, "%s%s",
                         need_slash ? "/" : "", e->d_name);
        blen += n;
        found = 1;
        break;
      }
    }
    closedir(d);
    if (!found)
      return 0;
  }
  snprintf(out, outsz, "%s", built);
  return 1;
}

#define LOG_NAME "debug.log"

int debugPrintf(const char *text, ...) {
  va_list list;

  /* File logging gated behind SUMMERTIME_LOG (per-call fopen append saturates SD).
     stdout always goes to the VT console for bring-up over SSH. */
  static int log_to_file = -1;
  if (log_to_file < 0)
    log_to_file = getenv("SUMMERTIME_LOG") ? 1 : 0;

  if (log_to_file) {
    FILE *f = fopen(LOG_NAME, "a");
    if (f) {
      va_start(list, text);
      vfprintf(f, text, list);
      va_end(list);
      fclose(f);
    }
  }

  va_start(list, text);
  vprintf(text, list);
  va_end(list);
  fflush(stdout);

  return 0;
}

uintptr_t read_tls_stack_guard(void) {
#if defined(__aarch64__)
  uintptr_t tls = 0;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tls));
  if (tls)
    return *(uintptr_t *)(tls + 0x28);
#endif
  return 0;
}

const char *resolve_android_path(const char *path) {
  static _Thread_local char alt_path[2048];
  static _Thread_local char asset_path[2048];

  static const char *app_data_prefix = "/data/data/com.kompasproductions.summertimesaga/";
  static const char *app_data_prefix_2 =
      "/data/user/0/com.kompasproductions.summertimesaga/";
  static const char *sdcard_prefix =
      "/storage/emulated/0/Android/data/com.kompasproductions.summertimesaga/";
  static const char *obb_prefix =
      "/storage/emulated/0/Android/obb/com.kompasproductions.summertimesaga/";

  if (!path || path[0] == '\0')
    return path;

  /* Android/Ren'Py builds asset paths as "<base>assets/<relative>" in several
     paths. Anchor on the "assets/" segment and resolve relative to cwd. */
  if (access(path, F_OK) != 0) {
    const char *anchor = strstr(path, "assets/");
    if (anchor) {
      snprintf(asset_path, sizeof(asset_path), "./%s", anchor);
      if (access(asset_path, F_OK) == 0)
        return asset_path;
      // case-insensitive fallback (OBB has inconsistent casing)
      char ci[2048];
      if (ci_resolve(asset_path, ci, sizeof(ci))) {
        snprintf(asset_path, sizeof(asset_path), "%s", ci);
        return asset_path;
      }
    }
  }

  if (strncmp(path, app_data_prefix, strlen(app_data_prefix)) == 0) {
    snprintf(alt_path, sizeof(alt_path), "./%s",
             path + strlen(app_data_prefix));
    return alt_path;
  }
  if (strncmp(path, app_data_prefix_2, strlen(app_data_prefix_2)) == 0) {
    snprintf(alt_path, sizeof(alt_path), "./%s",
             path + strlen(app_data_prefix_2));
    return alt_path;
  }
  if (strncmp(path, sdcard_prefix, strlen(sdcard_prefix)) == 0) {
    snprintf(alt_path, sizeof(alt_path), "./%s", path + strlen(sdcard_prefix));
    return alt_path;
  }
  if (strncmp(path, obb_prefix, strlen(obb_prefix)) == 0) {
    snprintf(alt_path, sizeof(alt_path), "./%s", path + strlen(obb_prefix));
    return alt_path;
  }
  // Keep absolute-ish app save paths in the game dir so saves persist.
  if (strncmp(path, "/snapshots/", 11) == 0) {
    snprintf(alt_path, sizeof(alt_path), ".%s", path);
    return alt_path;
  }

  if (access(path, F_OK) == 0)
    return path;

  if (path[0] == '/') {
    if (snprintf(alt_path, sizeof(alt_path), ".%s", path) <
            (int)sizeof(alt_path) &&
        access(alt_path, F_OK) == 0) {
      return alt_path;
    }
  }

  const char *basename = strrchr(path, '/');
  basename = basename ? basename + 1 : path;
  if (basename[0] != '\0') {
    if (snprintf(asset_path, sizeof(asset_path), "./assets/%s", basename) <
            (int)sizeof(asset_path) &&
        access(asset_path, F_OK) == 0) {
      return asset_path;
    }
  }

  return path;
}

int ret0(void) { return 0; }
int ret1(void) { return 1; }
int retm1(void) { return -1; }
