/*
 * util.c -- logging O_SYNC + helpers. FF4 so-loader (Mali-450).
 */
#define _GNU_SOURCE
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

static int g_logfd = -1;

void log_open(const char *path) {
  if (g_logfd >= 0)
    return;
  if (!path)
    path = "ff4a.log";
  /* O_SYNC: cada write vai pro disco na hora (regra logs persistentes wedge). */
  g_logfd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, 0644);
}

void log_close(void) {
  if (g_logfd >= 0) {
    close(g_logfd);
    g_logfd = -1;
  }
}

void debugPrintf(const char *fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if (n > (int)sizeof(buf))
    n = (int)sizeof(buf);
  /* stderr p/ o stream do host */
  fwrite(buf, 1, n, stderr);
  fflush(stderr);
  /* log O_SYNC persistente */
  if (g_logfd >= 0) {
    ssize_t w = write(g_logfd, buf, n);
    (void)w;
  }
}

uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}
