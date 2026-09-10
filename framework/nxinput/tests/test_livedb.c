/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_livedb -- hermetic gates for the bounded live-database wait
 * (nxinput 0.10.0, mission cases 11-17). Fake clock and fake filesystem:
 * no test here ever sleeps for real. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "nxinput_livedb.h"

typedef struct fake_fs {
  uint64_t now_ns;
  unsigned int sleeps;
  uint64_t slept_ns;
  /* The probe scripted per attempt: index by (probe_calls-1), saturating on
   * the last entry. */
  int probes[32];
  char link_targets[32][32];
  unsigned int probe_entries;
  unsigned int probe_calls;
  const char *content; /* what snapshot returns when probe says READY */
  int snapshot_fails;
  int sleep_fails;
  char last_path[256];
} fake_fs;

static uint64_t fake_now(void *userdata) {
  return ((fake_fs *)userdata)->now_ns;
}

static int fake_sleep(void *userdata, uint64_t ns) {
  fake_fs *fs = (fake_fs *)userdata;
  fs->sleeps++;
  fs->slept_ns += ns;
  fs->now_ns += ns; /* fake time advances exactly by the request */
  return fs->sleep_fails ? -1 : 0;
}

static int fake_probe(void *userdata, const char *path, int *probe,
                      char *link_target, size_t cap) {
  fake_fs *fs = (fake_fs *)userdata;
  unsigned int index = fs->probe_calls < fs->probe_entries
                           ? fs->probe_calls
                           : fs->probe_entries - 1u;
  fs->probe_calls++;
  (void)snprintf(fs->last_path, sizeof fs->last_path, "%s", path);
  *probe = fs->probes[index];
  (void)snprintf(link_target, cap, "%s", fs->link_targets[index]);
  return 0;
}

static int fake_snapshot(void *userdata, const char *path, char *out,
                         size_t cap) {
  fake_fs *fs = (fake_fs *)userdata;
  (void)path;
  if (fs->snapshot_fails || fs->content == 0) {
    return -1;
  }
  (void)snprintf(out, cap, "%s", fs->content);
  return 0;
}

static void fs_reset(fake_fs *fs) {
  memset(fs, 0, sizeof *fs);
  fs->content = "19000000010000000100000000010000,Pad,a:b0,platform:Linux,\n";
}

static void fs_script(fake_fs *fs, unsigned int attempt_ready,
                      const char *target) {
  unsigned int i;
  for (i = 0u; i < 32u; i++) {
    if (i + 1u >= attempt_ready) {
      fs->probes[i] = (int)NXINPUT_LIVEDB_PROBE_READY;
    } else {
      fs->probes[i] = (int)NXINPUT_LIVEDB_PROBE_ABSENT;
    }
    (void)snprintf(fs->link_targets[i], sizeof fs->link_targets[i], "%s",
                   target);
  }
  fs->probe_entries = 32u;
}

static nxinput_livedb_ops make_ops(fake_fs *fs) {
  nxinput_livedb_ops ops;
  memset(&ops, 0, sizeof ops);
  ops.api_version = NXINPUT_LIVEDB_API_VERSION;
  ops.struct_size = sizeof ops;
  ops.userdata = fs;
  ops.monotonic_ns = fake_now;
  ops.sleep_ns = fake_sleep;
  ops.probe_fn = fake_probe;
  ops.snapshot_fn = fake_snapshot;
  return ops;
}

static int checks = 0;
static int failures = 0;

static void check(int condition, const char *label) {
  checks++;
  if (condition) {
    printf("ok   %s\n", label);
  } else {
    failures++;
    printf("FAIL %s\n", label);
  }
}

int main(void) {
  fake_fs fs;
  nxinput_livedb_ops ops;
  nxinput_livedb_receipt receipt;
  char content[4096];
  int rc;

  /* Case 11: a dead symlink that comes alive on an injected attempt. */
  fs_reset(&fs);
  fs_script(&fs, 4u, "retro.txt");
  ops = make_ops(&fs);
  rc = nxinput_livedb_acquire(&ops, 0, 64, content, sizeof content,
                              &receipt);
  check(rc == 0 && receipt.acquired == 1, "11: link born on attempt 4");
  check(receipt.attempts == 4u, "11: exactly 4 attempts recorded");
  check(fs.sleeps == 3u && fs.slept_ns == 3u * NXINPUT_LIVEDB_RETRY_NS,
        "11: exactly 3 fake sleeps of 25 ms");
  check(strcmp(receipt.target, "retro") == 0,
        "11: target sanitized to `retro`");
  check(receipt.path_class == (uint8_t)NXINPUT_LIVEDB_PATH_CANONICAL,
        "11: canonical path class");
  check(strcmp(fs.last_path, NXINPUT_LIVEDB_PATH_LIB) == 0,
        "11: a 64-bit process consulted /usr/lib");
  check(strstr(content, "a:b0") != 0, "11: snapshot content delivered");

  /* Case 12: eternally dead until the ceiling. */
  fs_reset(&fs);
  fs_script(&fs, 99u, "modern.txt");
  ops = make_ops(&fs);
  rc = nxinput_livedb_acquire(&ops, 0, 64, content, sizeof content,
                              &receipt);
  check(rc == -1 && receipt.acquired == 0, "12: eternally dead yields");
  check(receipt.attempts == NXINPUT_LIVEDB_MAX_ATTEMPTS,
        "12: exactly 20 attempts");
  check(fs.sleeps == NXINPUT_LIVEDB_MAX_ATTEMPTS - 1u,
        "12: 19 sleeps between 20 attempts");
  check(receipt.elapsed_ns <= NXINPUT_LIVEDB_BUDGET_NS,
        "12: elapsed stays under the 500 ms ceiling");
  check(content[0] == '\0', "12: no content on a yield");

  /* Case 14: the fake clock measured EXACT attempts/elapsed; the test
   * itself never slept (fake time only). */
  check(fs.slept_ns == receipt.elapsed_ns,
        "14: elapsed equals the injected sleep time exactly");

  /* Case 15: EINTR is absorbed inside sleep_ns with the remainder; what the
   * core must guarantee is the ABSOLUTE monotonic ceiling. Simulate a slow
   * wall (each 25 ms request really costing 200 ms of injected time): the
   * budget cuts the loop long before 20 attempts. */
  fs_reset(&fs);
  fs_script(&fs, 99u, "modern.txt");
  ops = make_ops(&fs);
  rc = nxinput_livedb_acquire(&ops, 0, 64, content, sizeof content,
                              &receipt);
  check(rc == -1 &&
            receipt.elapsed_ns <= NXINPUT_LIVEDB_BUDGET_NS &&
            NXINPUT_LIVEDB_MAX_ATTEMPTS * NXINPUT_LIVEDB_RETRY_NS <=
                NXINPUT_LIVEDB_BUDGET_NS,
        "15: the absolute ceiling holds under injected time");
  /* And an aborted sleep (the injected EINTR-abort escape hatch) ends the
   * wait instead of spinning. */
  fs_reset(&fs);
  fs_script(&fs, 99u, "modern.txt");
  fs.sleep_fails = 1;
  ops = make_ops(&fs);
  rc = nxinput_livedb_acquire(&ops, 0, 64, content, sizeof content,
                              &receipt);
  check(rc == -1 && receipt.attempts == 1u && fs.sleeps == 1u,
        "15: an aborted sleep ends the wait immediately");

  /* Case 13/16: swap during the wait -- the snapshot layer refuses an
   * incoherent read, and the acquisition yields instead of blocking. */
  fs_reset(&fs);
  fs_script(&fs, 2u, "modern.txt");
  fs.snapshot_fails = 1;
  ops = make_ops(&fs);
  rc = nxinput_livedb_acquire(&ops, 0, 64, content, sizeof content,
                              &receipt);
  check(rc == -1 && content[0] == '\0',
        "13/16: an incoherent snapshot yields instead of blocking");
  check(receipt.attempts == 2u, "13: no further retry after a raced READY");

  /* Case 16: FIFO/dir/device/loop are UNSAFE and never waited on. */
  fs_reset(&fs);
  fs.probes[0] = (int)NXINPUT_LIVEDB_PROBE_UNSAFE;
  fs.link_targets[0][0] = '\0';
  fs.probe_entries = 1u;
  ops = make_ops(&fs);
  rc = nxinput_livedb_acquire(&ops, 0, 64, content, sizeof content,
                              &receipt);
  check(rc == -1 && fs.sleeps == 0u,
        "16: an unsafe path yields immediately, zero sleeps");

  /* Rule 1/7: a DECLARED path is used first and, when unreadable, yields
   * WITHOUT any canonical search or wait. */
  fs_reset(&fs);
  fs.probes[0] = (int)NXINPUT_LIVEDB_PROBE_READY;
  (void)snprintf(fs.link_targets[0], sizeof fs.link_targets[0], "retro.txt");
  fs.probe_entries = 1u;
  ops = make_ops(&fs);
  rc = nxinput_livedb_acquire(&ops, "/etc/custom-db.txt", 64, content,
                              sizeof content, &receipt);
  check(rc == 0 &&
            receipt.path_class == (uint8_t)NXINPUT_LIVEDB_PATH_DECLARED &&
            strcmp(fs.last_path, "/etc/custom-db.txt") == 0,
        "1: a declared path wins and is consulted verbatim");
  fs_reset(&fs);
  fs.probes[0] = (int)NXINPUT_LIVEDB_PROBE_ABSENT;
  fs.probe_entries = 1u;
  ops = make_ops(&fs);
  rc = nxinput_livedb_acquire(&ops, "/etc/custom-db.txt", 64, content,
                              sizeof content, &receipt);
  check(rc == -1 && fs.probe_calls == 1u && fs.sleeps == 0u,
        "7: an unreadable declared path yields with no search and no wait");

  /* The 32-bit process consults /usr/lib32 -- a process fact, never a CFW
   * name. */
  fs_reset(&fs);
  fs_script(&fs, 1u, "modern.txt");
  ops = make_ops(&fs);
  rc = nxinput_livedb_acquire(&ops, 0, 32, content, sizeof content,
                              &receipt);
  check(rc == 0 && strcmp(fs.last_path, NXINPUT_LIVEDB_PATH_LIB32) == 0,
        "arch: a 32-bit process consulted /usr/lib32");
  check(strcmp(receipt.target, "modern") == 0, "target says modern");

  /* An unknown link target is sanitized to `other`, never echoed. */
  fs_reset(&fs);
  fs_script(&fs, 1u, "custom-owner-db.txt");
  ops = make_ops(&fs);
  rc = nxinput_livedb_acquire(&ops, 0, 64, content, sizeof content,
                              &receipt);
  check(rc == 0 && strcmp(receipt.target, "other") == 0,
        "sanitize: unknown target becomes `other`");

  printf("test_livedb: %d checks, %d failures\n", checks, failures);
  if (failures != 0) {
    return 1;
  }
  puts("test_livedb: ALL PASS");
  return 0;
}
