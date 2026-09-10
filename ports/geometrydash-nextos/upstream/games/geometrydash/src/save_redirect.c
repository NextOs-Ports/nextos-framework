/*
 * save_redirect.c -- give the guest its Android data directory.
 *
 * Geometry Dash 2.2 does not save through getCocos2dxWritablePath.  Cocos2d-x
 * 2.2.x builds the path itself, as "/data/data/" + the package name, and the
 * game opens CCGameManager.dat and CCLocalLevels.dat there directly.  On this
 * device that directory does not exist and never will, so every save silently
 * failed: the run traced six opens of the .dat files under
 * /data/data/com.robtopx.geometryjump, all returning NULL, and progress was
 * gone the moment the game closed.
 *
 * Creating that path on the device would be a port reaching outside its own
 * folder, so instead every libc call the guest makes with a path under that
 * prefix is answered against <gamedir>/userdata.  The guest keeps believing it
 * is on Android; the files land in the port's own directory, where a package
 * update or a card swap keeps them.
 *
 * Only the calls the guest actually imports are wrapped -- fopen, open, rename,
 * remove, mkdir, access, chmod, opendir and stat.  Anything else it reaches for
 * would show up as a save that does not stick, not as a crash.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

static char g_prefix[256];  /* "/data/data/<package>/" */
static char g_real[PATH_MAX];

extern void *gd_fopen_real(const char *, const char *);

void gd_save_redirect_init(const char *package, const char *real_dir) {
  snprintf(g_prefix, sizeof(g_prefix), "/data/data/%s/", package);
  snprintf(g_real, sizeof(g_real), "%s", real_dir);
  mkdir(g_real, 0755);
  debugPrintf("[save] %s* -> %s/\n", g_prefix, g_real);
}

/* Returns the rewritten path, or the original when it is not ours. */
static const char *redir(const char *p, char *buf, size_t n) {
  if (!p || !g_prefix[0])
    return p;
  size_t len = strlen(g_prefix);
  /* The bare directory, with or without its trailing slash, counts too. */
  if (strncmp(p, g_prefix, len) == 0) {
    snprintf(buf, n, "%s/%s", g_real, p + len);
    return buf;
  }
  if (strncmp(p, g_prefix, len - 1) == 0 && p[len - 1] == 0) {
    snprintf(buf, n, "%s", g_real);
    return buf;
  }
  return p;
}

/* Traced under GD_TRACE_IO=1 by bionic_shims.c's fopen; the rest stay quiet. */
static int traced(void) {
  static int on = -1;
  if (on < 0) {
    const char *e = getenv("GD_TRACE_IO");
    on = (e && *e == '1') ? 1 : 0;
  }
  return on;
}

#define REDIR(p) char _b[PATH_MAX]; const char *_p = redir((p), _b, sizeof(_b))

void *gd_fopen(const char *path, const char *mode) {
  REDIR(path);
  FILE *f = fopen(_p, mode);
  if (traced())
    debugPrintf("[io] fopen(\"%s\"%s, \"%s\") -> %p\n", path ? path : "(null)",
                _p == path ? "" : " [redir]", mode ? mode : "?", (void *)f);
  return f;
}

int gd_open(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  REDIR(path);
  int fd = open(_p, flags, mode);
  if (traced())
    debugPrintf("[io] open(\"%s\"%s, 0x%x) -> %d\n", path ? path : "(null)",
                _p == path ? "" : " [redir]", flags, fd);
  return fd;
}

int gd_rename(const char *from, const char *to) {
  char b2[PATH_MAX];
  REDIR(from);
  const char *t = redir(to, b2, sizeof(b2));
  return rename(_p, t);
}

int gd_remove(const char *path) {
  REDIR(path);
  return remove(_p);
}

int gd_mkdir(const char *path, mode_t mode) {
  REDIR(path);
  return mkdir(_p, mode);
}

int gd_access(const char *path, int how) {
  REDIR(path);
  return access(_p, how);
}

int gd_chmod(const char *path, mode_t mode) {
  REDIR(path);
  return chmod(_p, mode);
}

void *gd_opendir(const char *path) {
  REDIR(path);
  return opendir(_p);
}

int gd_stat(const char *path, void *st) {
  REDIR(path);
  return stat(_p, (struct stat *)st);
}
