/* ab_log.c — log persistente + caminhos do port.
 * O log vai pra pasta do port com O_SYNC: se o device travar, a última linha
 * escrita continua no cartão.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ab_port.h"

static int g_fd = -1;
static char g_gamedir[1024];
static char g_filesdir[1100];
static char g_cachedir[1100];
static char g_apk[1100];
static struct timespec g_t0;

int ab_env_int(const char *name, int fallback) {
  const char *v = getenv(name);
  if (!v || !*v)
    return fallback;
  return (int)strtol(v, NULL, 0);
}

static void mkdir_p(const char *path) {
  char tmp[1200];
  size_t len = strlen(path);
  if (len == 0 || len >= sizeof(tmp))
    return;
  memcpy(tmp, path, len + 1);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      mkdir(tmp, 0755);
      *p = '/';
    }
  }
  mkdir(tmp, 0755);
}

void ab_log_open(const char *gamedir) {
  char path[1200];
  snprintf(g_gamedir, sizeof(g_gamedir), "%s", gamedir);
  snprintf(g_filesdir, sizeof(g_filesdir), "%s/saves", gamedir);
  snprintf(g_cachedir, sizeof(g_cachedir), "%s/cache", gamedir);
  snprintf(g_apk, sizeof(g_apk), "%s/game.apk", gamedir);
  mkdir_p(g_filesdir);
  mkdir_p(g_cachedir);
  clock_gettime(CLOCK_MONOTONIC, &g_t0);
  snprintf(path, sizeof(path), "%s/log.txt", gamedir);
  /* APPEND, não TRUNC: o launcher único já redireciona o próprio stdout para
   * log.txt. Truncar aqui apagaria o cabeçalho dele (cfw, controles, gates). */
  g_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_SYNC, 0644);
  if (g_fd < 0)
    g_fd = 2;
}

void ab_log(const char *fmt, ...) {
  char line[2048];
  struct timespec now;
  double dt;
  int head;
  va_list ap;
  if (g_fd < 0)
    return;
  clock_gettime(CLOCK_MONOTONIC, &now);
  dt = (double)(now.tv_sec - g_t0.tv_sec) +
       (double)(now.tv_nsec - g_t0.tv_nsec) / 1e9;
  head = snprintf(line, sizeof(line), "[%8.3f] ", dt);
  va_start(ap, fmt);
  vsnprintf(line + head, sizeof(line) - (size_t)head, fmt, ap);
  va_end(ap);
  size_t len = strlen(line);
  if (len == 0 || line[len - 1] != '\n') {
    if (len < sizeof(line) - 1) {
      line[len++] = '\n';
      line[len] = 0;
    }
  }
  ssize_t written = write(g_fd, line, len);
  (void)written;
}

void ab_log_close(void) {
  if (g_fd >= 0 && g_fd != 2)
    close(g_fd);
  g_fd = -1;
}

const char *ab_gamedir(void) { return g_gamedir; }
const char *ab_filesdir(void) { return g_filesdir; }
const char *ab_cachedir(void) { return g_cachedir; }
const char *ab_apk_path(void) { return g_apk; }
