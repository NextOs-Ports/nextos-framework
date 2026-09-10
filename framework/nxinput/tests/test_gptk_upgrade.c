/* SPDX-License-Identifier: GPL-3.0-only */
/* V4-CONTROLLERS-03 / C4: the owner's editable NEXTOSCONTROLLERS.gptk is
 * never overwritten. An update offers a `.new` sibling and a diff; adoption
 * stays a human act. Hermetic: a private temporary directory, no device. */
#define _POSIX_C_SOURCE 200809L
#include "nxinput_gptk_upgrade.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures;

#define CHECK(cond, name)                                    \
  do {                                                       \
    if (cond) {                                              \
      printf("ok %s\n", name);                               \
    } else {                                                 \
      printf("FAIL %s (line %d)\n", name, __LINE__);         \
      g_failures++;                                          \
    }                                                        \
  } while (0)

static const char OWNER_FILE[] =
    "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = ui.confirm\n"
    "[gameplay]\nA = player.jump\nB = player.dash\n";
static const char CANDIDATE[] =
    "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = ui.confirm\nB = ui.cancel\n"
    "[gameplay]\nA = player.jump\n";

static void write_file(int dir_fd, const char *name, const char *text) {
  int fd = openat(dir_fd, name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  (void)!write(fd, text, strlen(text));
  (void)close(fd);
}

static char *read_file(int dir_fd, const char *name, size_t *length) {
  static char buffer[8192];
  ssize_t got;
  int fd = openat(dir_fd, name, O_RDONLY);

  if (fd < 0) {
    return 0;
  }
  got = read(fd, buffer, sizeof buffer - 1u);
  (void)close(fd);
  if (got < 0) {
    return 0;
  }
  buffer[got] = '\0';
  if (length != 0) {
    *length = (size_t)got;
  }
  return buffer;
}

int main(void) {
  char template[] = "/tmp/nxgptk-upgrade.XXXXXX";
  char *root = mkdtemp(template);
  nxinput_gptk_upgrade_report report;
  int dir_fd;

  if (root == 0) {
    printf("FAIL could not create a private directory\n");
    return 1;
  }
  dir_fd = open(root, O_RDONLY | O_DIRECTORY);
  if (dir_fd < 0) {
    printf("FAIL could not open the private directory\n");
    return 1;
  }

  /* 1. No owner file yet: an upgrade writes NOTHING. Materializing the first
   * copy is the launcher's job, and doing it here would be indistinguishable
   * from overwriting one. */
  CHECK(nxinput_gptk_upgrade_offer_at(dir_fd, CANDIDATE, strlen(CANDIDATE),
                                      &report) == 0 &&
            report.status == NXINPUT_GPTK_UPGRADE_OWNER_MISSING &&
            read_file(dir_fd, "NEXTOSCONTROLLERS.gptk.new", 0) == 0,
        "no owner file: nothing is written, not even a .new");

  /* 2. The owner edited theirs. The offer lands beside it and the owner's
   * bytes are untouched. */
  write_file(dir_fd, "NEXTOSCONTROLLERS.gptk", OWNER_FILE);
  CHECK(nxinput_gptk_upgrade_offer_at(dir_fd, CANDIDATE, strlen(CANDIDATE),
                                      &report) == 0 &&
            report.status == NXINPUT_GPTK_UPGRADE_OFFERED,
        "a different default is OFFERED, never applied");
  {
    size_t length = 0u;
    char *owner = read_file(dir_fd, "NEXTOSCONTROLLERS.gptk", &length);
    CHECK(owner != 0 && length == strlen(OWNER_FILE) &&
              memcmp(owner, OWNER_FILE, length) == 0,
          "the owner's file is preserved BYTE FOR BYTE");
  }
  {
    size_t length = 0u;
    char *fresh = read_file(dir_fd, "NEXTOSCONTROLLERS.gptk.new", &length);
    CHECK(fresh != 0 && length == strlen(CANDIDATE) &&
              memcmp(fresh, CANDIDATE, length) == 0,
          "the .new sibling carries the candidate byte for byte");
  }
  CHECK(report.changed > 0u && strstr(report.diff, "menu B:") != 0 &&
            strstr(report.diff, "gameplay B:") != 0,
        "the diff names context and control, by name");
  CHECK(strstr(report.diff, "/tmp/") == 0 && strstr(report.diff, "nxgptk") == 0
            && strchr(report.diff, '/') == 0,
        "the diff is path-free");
  CHECK(report.owner_sha256[0] != '\0' && report.candidate_sha256[0] != '\0' &&
            strcmp(report.owner_sha256, report.candidate_sha256) != 0,
        "the report carries both hashes and they differ");

  /* 3. No temporary residue is left behind. */
  CHECK(read_file(dir_fd, ".NEXTOSCONTROLLERS.gptk.new.tmp", 0) == 0,
        "the atomic write leaves no temporary file behind");

  /* 4. Offering the SAME bytes the owner already has is UP_TO_DATE and
   * writes nothing new. */
  (void)unlinkat(dir_fd, "NEXTOSCONTROLLERS.gptk.new", 0);
  CHECK(nxinput_gptk_upgrade_offer_at(dir_fd, OWNER_FILE, strlen(OWNER_FILE),
                                      &report) == 0 &&
            report.status == NXINPUT_GPTK_UPGRADE_UP_TO_DATE &&
            read_file(dir_fd, "NEXTOSCONTROLLERS.gptk.new", 0) == 0,
        "identical bytes are up-to-date and produce no .new");

  /* 5. A candidate that does not parse is refused; the owner keeps working. */
  {
    static const char broken[] = "format = NOT_OURS\n[menu]\nA = x\n";
    CHECK(nxinput_gptk_upgrade_offer_at(dir_fd, broken, strlen(broken),
                                        &report) == 0 &&
              report.status == NXINPUT_GPTK_UPGRADE_REFUSED &&
              report.candidate_code != 0 &&
              read_file(dir_fd, "NEXTOSCONTROLLERS.gptk.new", 0) == 0,
          "an invalid candidate is refused and offers nothing");
  }

  /* 6. Symlink safety. An owner file that is a symlink is refused, never
   * followed -- so a link pointing outside the authorized root cannot be
   * read or written through. */
  {
    char other[] = "/tmp/nxgptk-outside.XXXXXX";
    int outside_fd = mkstemp(other);
    char outside_before[512];
    size_t outside_length = 0u;
    int probe;

    (void)!write(outside_fd, "SECRET-OUTSIDE\n", 15u);
    (void)close(outside_fd);
    (void)unlinkat(dir_fd, "NEXTOSCONTROLLERS.gptk", 0);
    CHECK(symlinkat(other, dir_fd, "NEXTOSCONTROLLERS.gptk") == 0,
          "symlink: the trap link was created");
    CHECK(nxinput_gptk_upgrade_offer_at(dir_fd, CANDIDATE, strlen(CANDIDATE),
                                        &report) == 0 &&
              report.status != NXINPUT_GPTK_UPGRADE_OFFERED,
          "symlink: an owner file that is a symlink never becomes an offer");
    probe = open(other, O_RDONLY);
    outside_length = probe >= 0
                         ? (size_t)read(probe, outside_before,
                                        sizeof outside_before)
                         : 0u;
    if (probe >= 0) {
      (void)close(probe);
    }
    CHECK(outside_length == 15u &&
              memcmp(outside_before, "SECRET-OUTSIDE\n", 15u) == 0,
          "symlink: the file outside the root was never written through");
    (void)unlink(other);
    (void)unlinkat(dir_fd, "NEXTOSCONTROLLERS.gptk", 0);
  }

  /* 7. An existing `.new` that is a symlink is refused visibly, not
   * replaced silently. */
  {
    char other[] = "/tmp/nxgptk-newtrap.XXXXXX";
    int outside_fd = mkstemp(other);

    (void)close(outside_fd);
    write_file(dir_fd, "NEXTOSCONTROLLERS.gptk", OWNER_FILE);
    (void)unlinkat(dir_fd, "NEXTOSCONTROLLERS.gptk.new", 0);
    CHECK(symlinkat(other, dir_fd, "NEXTOSCONTROLLERS.gptk.new") == 0,
          ".new symlink: the trap link was created");
    CHECK(nxinput_gptk_upgrade_offer_at(dir_fd, CANDIDATE, strlen(CANDIDATE),
                                        &report) == 0 &&
              report.status == NXINPUT_GPTK_UPGRADE_REFUSED &&
              report.error[0] != '\0',
          ".new symlink: refused visibly instead of replaced silently");
    (void)unlink(other);
    (void)unlinkat(dir_fd, "NEXTOSCONTROLLERS.gptk.new", 0);
  }

  /* 8. A V1 owner offered a V2 default: still only an offer, and the diff
   * says what would change. */
  {
    static const char v2_candidate[] =
        "format = NEXTOS_CONTROLLERS/2\n"
        "[menu]\nA = ui.confirm\nB = null\nX = null\nY = null\nL1 = null\n"
        "R1 = null\nL2 = null\nR2 = null\nL3 = null\nR3 = null\n"
        "START = null\nSELECT = null\nUP = null\nDOWN = null\nLEFT = null\n"
        "RIGHT = null\nLEFT_STICK = null\nRIGHT_STICK = null\n"
        "[gameplay]\nA = player.jump\nB = null\nX = null\nY = null\n"
        "L1 = null\nR1 = null\nL2 = null\nR2 = null\nL3 = null\nR3 = null\n"
        "START = null\nSELECT = null\nUP = null\nDOWN = null\nLEFT = null\n"
        "RIGHT = null\nLEFT_STICK = null\nRIGHT_STICK = null\n";
    size_t length = 0u;
    char *owner;

    CHECK(nxinput_gptk_upgrade_offer_at(dir_fd, v2_candidate,
                                        strlen(v2_candidate), &report) == 0 &&
              report.status == NXINPUT_GPTK_UPGRADE_OFFERED &&
              report.disabled > 0u,
          "a V1 owner offered a V2 default gets an offer with a real diff");
    owner = read_file(dir_fd, "NEXTOSCONTROLLERS.gptk", &length);
    CHECK(owner != 0 && length == strlen(OWNER_FILE) &&
              memcmp(owner, OWNER_FILE, length) == 0,
          "the V1 owner file survives the V2 offer untouched");
    CHECK(strstr(report.diff, "SUPPRESS") != 0,
          "the diff spells out what the V2 default would disable");
  }

  /* 9. (0.10.0, mission case 35) A V3 default whose FACE_LAYOUT differs from
   * the owner's is reported in the diff -- and the owner's copy is still
   * never overwritten. */
  {
    static const char v3_owner[] =
        "format = NEXTOS_CONTROLLERS/3\nFACE_LAYOUT = retro\n"
        "[menu]\nA = ui.confirm\nB = null\nX = null\nY = null\nL1 = null\n"
        "R1 = null\nL2 = null\nR2 = null\nL3 = null\nR3 = null\n"
        "START = null\nSELECT = null\nUP = null\nDOWN = null\nLEFT = null\n"
        "RIGHT = null\nLEFT_STICK = null\nRIGHT_STICK = null\n"
        "[gameplay]\nA = player.jump\nB = null\nX = null\nY = null\n"
        "L1 = null\nR1 = null\nL2 = null\nR2 = null\nL3 = null\nR3 = null\n"
        "START = null\nSELECT = null\nUP = null\nDOWN = null\nLEFT = null\n"
        "RIGHT = null\nLEFT_STICK = null\nRIGHT_STICK = null\n";
    static const char v3_candidate[] =
        "format = NEXTOS_CONTROLLERS/3\nFACE_LAYOUT = auto\n"
        "[menu]\nA = ui.confirm\nB = null\nX = null\nY = null\nL1 = null\n"
        "R1 = null\nL2 = null\nR2 = null\nL3 = null\nR3 = null\n"
        "START = null\nSELECT = null\nUP = null\nDOWN = null\nLEFT = null\n"
        "RIGHT = null\nLEFT_STICK = null\nRIGHT_STICK = null\n"
        "[gameplay]\nA = player.jump\nB = null\nX = null\nY = null\n"
        "L1 = null\nR1 = null\nL2 = null\nR2 = null\nL3 = null\nR3 = null\n"
        "START = null\nSELECT = null\nUP = null\nDOWN = null\nLEFT = null\n"
        "RIGHT = null\nLEFT_STICK = null\nRIGHT_STICK = null\n";
    size_t length = 0u;
    char *owner;

    write_file(dir_fd, "NEXTOSCONTROLLERS.gptk", v3_owner);
    CHECK(nxinput_gptk_upgrade_offer_at(dir_fd, v3_candidate,
                                        strlen(v3_candidate), &report) == 0 &&
              report.status == NXINPUT_GPTK_UPGRADE_OFFERED,
          "a V3 default differing only in FACE_LAYOUT is still an offer");
    CHECK(strstr(report.diff, "FACE_LAYOUT: retro -> auto") != 0,
          "the diff names the FACE_LAYOUT difference (case 35)");
    owner = read_file(dir_fd, "NEXTOSCONTROLLERS.gptk", &length);
    CHECK(owner != 0 && length == strlen(v3_owner) &&
              memcmp(owner, v3_owner, length) == 0,
          "the owner's V3 copy is never overwritten");
    (void)unlinkat(dir_fd, "NEXTOSCONTROLLERS.gptk.new", 0);
  }

  (void)close(dir_fd);
  if (g_failures != 0) {
    printf("test_gptk_upgrade: %d FAILURES (%s)\n", g_failures, root);
    return 1;
  }
  printf("test_gptk_upgrade: ALL PASS\n");
  return 0;
}
