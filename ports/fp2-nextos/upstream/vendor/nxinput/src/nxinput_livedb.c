/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_livedb -- see include/nxinput_livedb.h. */
#define _POSIX_C_SOURCE 200809L

#include "nxinput_livedb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int ops_valid(const nxinput_livedb_ops *ops) {
  return ops != 0 && ops->api_version == NXINPUT_LIVEDB_API_VERSION &&
         ops->struct_size == sizeof(*ops) && ops->monotonic_ns != 0 &&
         ops->sleep_ns != 0 && ops->probe_fn != 0 && ops->snapshot_fn != 0;
}

const char *nxinput_livedb_path_class_name(int path_class) {
  switch (path_class) {
    case (int)NXINPUT_LIVEDB_PATH_DECLARED:
      return "declared";
    case (int)NXINPUT_LIVEDB_PATH_CANONICAL:
      return "canonical";
    case (int)NXINPUT_LIVEDB_PATH_NONE:
    default:
      return "none";
  }
}

/* Sanitize a symlink-target basename into the only three words a receipt
 * may carry. Never a path, never a device or CFW name. */
static void sanitize_target(const char *link_target, char *out, size_t cap) {
  const char *word = "other";

  if (cap == 0u) {
    return;
  }
  if (link_target != 0 && strstr(link_target, "modern") != 0) {
    word = "modern";
  } else if (link_target != 0 && strstr(link_target, "retro") != 0) {
    word = "retro";
  }
  {
    size_t n = strlen(word);
    if (n >= cap) {
      n = cap - 1u;
    }
    memcpy(out, word, n);
    out[n] = '\0';
  }
}

int nxinput_livedb_acquire(const nxinput_livedb_ops *ops,
                           const char *declared_path, int pointer_bits,
                           char *content_out, size_t cap,
                           nxinput_livedb_receipt *receipt) {
  const char *path;
  uint64_t start;
  unsigned int attempt;
  int probe = (int)NXINPUT_LIVEDB_PROBE_ABSENT;
  char link_target[128];

  if (receipt != 0) {
    memset(receipt, 0, sizeof *receipt);
    receipt->api_version = NXINPUT_LIVEDB_API_VERSION;
    receipt->struct_size = sizeof *receipt;
    receipt->path_class = (uint8_t)NXINPUT_LIVEDB_PATH_NONE;
  }
  if (!ops_valid(ops) || content_out == 0 || cap == 0u || receipt == 0) {
    return -1;
  }
  content_out[0] = '\0';

  /* Rule 1: a DECLARED path is used as-is. Unreadable simply yields; it
   * never authorizes a search anywhere else. No wait either: a declared
   * path is not the boot-recreated canonical link. */
  if (declared_path != 0 && declared_path[0] != '\0') {
    receipt->path_class = (uint8_t)NXINPUT_LIVEDB_PATH_DECLARED;
    receipt->attempts = 1u;
    link_target[0] = '\0';
    (void)ops->probe_fn(ops->userdata, declared_path, &probe, link_target,
                        sizeof link_target);
    sanitize_target(link_target, receipt->target, sizeof receipt->target);
    if (probe != (int)NXINPUT_LIVEDB_PROBE_READY ||
        ops->snapshot_fn(ops->userdata, declared_path, content_out, cap) !=
            0) {
      content_out[0] = '\0';
      return -1;
    }
    receipt->acquired = 1;
    return 0;
  }

  /* Rule 2: the canonical runtime path, selected by a process fact. */
  path = pointer_bits == 32 ? NXINPUT_LIVEDB_PATH_LIB32
                            : NXINPUT_LIVEDB_PATH_LIB;
  receipt->path_class = (uint8_t)NXINPUT_LIVEDB_PATH_CANONICAL;
  start = ops->monotonic_ns(ops->userdata);
  for (attempt = 1u; attempt <= NXINPUT_LIVEDB_MAX_ATTEMPTS; attempt++) {
    uint64_t now;

    receipt->attempts = attempt;
    link_target[0] = '\0';
    probe = (int)NXINPUT_LIVEDB_PROBE_ABSENT;
    if (ops->probe_fn(ops->userdata, path, &probe, link_target,
                      sizeof link_target) != 0) {
      probe = (int)NXINPUT_LIVEDB_PROBE_UNSAFE;
    }
    sanitize_target(link_target, receipt->target, sizeof receipt->target);
    if (probe == (int)NXINPUT_LIVEDB_PROBE_READY) {
      if (ops->snapshot_fn(ops->userdata, path, content_out, cap) == 0) {
        receipt->elapsed_ns = ops->monotonic_ns(ops->userdata) - start;
        receipt->acquired = 1;
        return 0;
      }
      /* The path looked ready but no coherent snapshot exists (unsafe or
       * raced beyond repair). Yield rather than loop on it. */
      content_out[0] = '\0';
      receipt->elapsed_ns = ops->monotonic_ns(ops->userdata) - start;
      return -1;
    }
    if (probe == (int)NXINPUT_LIVEDB_PROBE_UNSAFE) {
      /* FIFO, directory, device, loop, oversize target...: never wait for
       * something that can only be wrong. */
      receipt->elapsed_ns = ops->monotonic_ns(ops->userdata) - start;
      return -1;
    }
    /* ABSENT: the boot may still be recreating the link. Bounded wait. */
    now = ops->monotonic_ns(ops->userdata);
    receipt->elapsed_ns = now - start;
    if (attempt == NXINPUT_LIVEDB_MAX_ATTEMPTS ||
        now - start + NXINPUT_LIVEDB_RETRY_NS > NXINPUT_LIVEDB_BUDGET_NS) {
      break;
    }
    if (ops->sleep_ns(ops->userdata, NXINPUT_LIVEDB_RETRY_NS) != 0) {
      break;
    }
  }
  receipt->elapsed_ns = ops->monotonic_ns(ops->userdata) - start;
  return -1;
}

/* ------------------------------------------------ the production ops */

static uint64_t default_monotonic(void *userdata) {
  struct timespec ts;
  (void)userdata;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0u;
  }
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int default_sleep(void *userdata, uint64_t ns) {
  struct timespec request;
  struct timespec remaining;
  (void)userdata;
  request.tv_sec = (time_t)(ns / 1000000000ull);
  request.tv_nsec = (long)(ns % 1000000000ull);
  /* EINTR-safe: resume with the remainder, never restart the full period,
   * so a signal storm cannot stretch the absolute ceiling. */
  while (nanosleep(&request, &remaining) != 0) {
    if (errno != EINTR) {
      return -1;
    }
    request = remaining;
  }
  return 0;
}

static int default_probe(void *userdata, const char *path, int *probe,
                         char *link_target, size_t cap) {
  struct stat link_status;
  struct stat target_status;

  (void)userdata;
  if (path == 0 || probe == 0 || link_target == 0 || cap == 0u) {
    return -1;
  }
  link_target[0] = '\0';
  *probe = (int)NXINPUT_LIVEDB_PROBE_ABSENT;
  if (lstat(path, &link_status) != 0) {
    return 0; /* missing entirely: ABSENT */
  }
  if (S_ISLNK(link_status.st_mode)) {
    char raw[512];
    ssize_t got = readlink(path, raw, sizeof raw - 1u);

    if (got > 0) {
      const char *base;
      raw[got] = '\0';
      base = strrchr(raw, '/');
      base = base != 0 ? base + 1 : raw;
      (void)snprintf(link_target, cap, "%s", base);
    }
    if (stat(path, &target_status) != 0) {
      /* ELOOP/excessive depth are UNSAFE, a dead target is ABSENT. */
      *probe = (errno == ELOOP || errno == ENAMETOOLONG)
                   ? (int)NXINPUT_LIVEDB_PROBE_UNSAFE
                   : (int)NXINPUT_LIVEDB_PROBE_ABSENT;
      return 0;
    }
    *probe = S_ISREG(target_status.st_mode)
                 ? (int)NXINPUT_LIVEDB_PROBE_READY
                 : (int)NXINPUT_LIVEDB_PROBE_UNSAFE;
    return 0;
  }
  *probe = S_ISREG(link_status.st_mode) ? (int)NXINPUT_LIVEDB_PROBE_READY
                                        : (int)NXINPUT_LIVEDB_PROBE_UNSAFE;
  return 0;
}

static int default_snapshot(void *userdata, const char *path, char *out,
                            size_t cap) {
  struct stat before;
  struct stat after;
  size_t used = 0u;
  int fd;
  int failed = 0;

  (void)userdata;
  if (path == 0 || out == 0 || cap == 0u) {
    return -1;
  }
  out[0] = '\0';
  /* O_NONBLOCK so a FIFO can never park the admission. The open follows the
   * symlink ON PURPOSE: from here on every read uses THIS fd, so a swap of
   * the link mid-read cannot mix two databases. */
  fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    return -1;
  }
  if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) ||
      before.st_size < 0 || (uintmax_t)before.st_size >= (uintmax_t)cap) {
    (void)close(fd);
    return -1;
  }
  while (used < cap - 1u) {
    ssize_t got = read(fd, out + used, cap - 1u - used);

    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      failed = 1;
      break;
    }
    if (got == 0) {
      break;
    }
    used += (size_t)got;
  }
  /* Identity re-verified AFTER the read: an in-place truncation or rewrite
   * during the read is a race, not a snapshot. */
  if (failed || fstat(fd, &after) != 0 ||
      before.st_ino != after.st_ino || before.st_dev != after.st_dev ||
      before.st_size != after.st_size ||
      (uintmax_t)after.st_size != (uintmax_t)used ||
      memchr(out, '\0', used) != 0) {
    (void)close(fd);
    out[0] = '\0';
    return -1;
  }
  (void)close(fd);
  out[used] = '\0';
  return 0;
}

const nxinput_livedb_ops *nxinput_livedb_default_ops(void) {
  static nxinput_livedb_ops ops;

  ops.api_version = NXINPUT_LIVEDB_API_VERSION;
  ops.struct_size = sizeof ops;
  ops.userdata = 0;
  ops.monotonic_ns = default_monotonic;
  ops.sleep_ns = default_sleep;
  ops.probe_fn = default_probe;
  ops.snapshot_fn = default_snapshot;
  return &ops;
}
