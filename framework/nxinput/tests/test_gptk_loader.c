/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "nxinput_gptk_loader.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAP_NAME "NEXTOSCONTROLLERS.gptk"

static void fail(const char *expression, const char *file, int line) {
  (void)fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
  exit(1);
}

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!(expression))                                                        \
      fail(#expression, __FILE__, __LINE__);                                  \
  } while (0)

static const char default_map[] =
    "format = NEXTOS_CONTROLLERS/1\n"
    "port = loader_test\n"
    "[menu]\nA = ui.confirm\nB = ui.cancel\n"
    "[gameplay]\nA = player.jump\nB = player.action\n";

static const char owner_map[] =
    "format = NEXTOS_CONTROLLERS/1\n"
    "port = loader_test\n"
    "[menu]\nA = ui.confirm\nB = ui.cancel\n"
    "[gameplay]\nA = player.action\nB = player.jump\n";

static const char malformed_map[] =
    "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = ui.confirm\n";

static const char unknown_action_map[] =
    "format = NEXTOS_CONTROLLERS/1\n"
    "[menu]\nA = unknown.action\n"
    "[gameplay]\nA = player.jump\n";

static const char *const allowlist[] = {
    "ui.confirm", "ui.cancel", "player.jump", "player.action"};

static void write_at(int directory_fd, const char *text, size_t length) {
  int fd = openat(directory_fd, MAP_NAME,
                  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                  S_IRUSR | S_IWUSR);
  size_t used = 0u;

  CHECK(fd >= 0);
  while (used < length) {
    ssize_t wrote = write(fd, text + used, length - used);
    if (wrote < 0 && errno == EINTR) {
      continue;
    }
    CHECK(wrote > 0);
    used += (size_t)wrote;
  }
  CHECK(close(fd) == 0);
}

static void write_oversized_at(int directory_fd) {
  static char block[4096];
  size_t remaining = (size_t)NXINPUT_GPTK_MAX_BYTES + 1u;
  int fd = openat(directory_fd, MAP_NAME,
                  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                  S_IRUSR | S_IWUSR);

  CHECK(fd >= 0);
  memset(block, 'x', sizeof block);
  while (remaining > 0u) {
    size_t want = remaining < sizeof block ? remaining : sizeof block;
    ssize_t wrote = write(fd, block, want);
    if (wrote < 0 && errno == EINTR) {
      continue;
    }
    CHECK(wrote > 0);
    remaining -= (size_t)wrote;
  }
  CHECK(close(fd) == 0);
}

static void check_file_at(int directory_fd, const char *expected,
                          size_t expected_length) {
  char buffer[512];
  ssize_t got;
  int fd = openat(directory_fd, MAP_NAME, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

  CHECK(fd >= 0);
  got = read(fd, buffer, sizeof buffer);
  CHECK(got == (ssize_t)expected_length);
  CHECK(memcmp(buffer, expected, expected_length) == 0);
  CHECK(close(fd) == 0);
}

int main(void) {
  char root[] = "/tmp/nxinput-gptk-loader-XXXXXX";
  char owner_path[256];
  char defaults_path[256];
  char outside_path[256];
  char json[1400];
  nxinput_gptk_load_receipt receipt;
  nxinput_gptk map;
  struct stat before;
  struct stat after;
  int root_fd;
  int owner_fd;
  int defaults_fd;

  CHECK(mkdtemp(root) != 0);
  CHECK(snprintf(owner_path, sizeof owner_path, "%s/owner", root) > 0);
  CHECK(snprintf(defaults_path, sizeof defaults_path, "%s/defaults", root) >
        0);
  CHECK(snprintf(outside_path, sizeof outside_path, "%s/outside", root) > 0);
  CHECK(mkdir(owner_path, S_IRWXU) == 0);
  CHECK(mkdir(defaults_path, S_IRWXU) == 0);
  root_fd = open(root, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  owner_fd = open(owner_path, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  defaults_fd =
      open(defaults_path, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  CHECK(root_fd >= 0 && owner_fd >= 0 && defaults_fd >= 0);
  write_at(defaults_fd, default_map, strlen(default_map));
  write_at(owner_fd, owner_map, strlen(owner_map));

  /* Valid editable owner wins and the receipt binds its exact bytes. */
  CHECK(nxinput_gptk_load_at(owner_fd, defaults_fd, allowlist,
                             sizeof allowlist / sizeof allowlist[0], &map,
                             &receipt) == 0);
  CHECK(receipt.source == (uint8_t)NXINPUT_GPTK_LOAD_OWNER);
  CHECK(receipt.owner_present == 1u && receipt.owner_error_code == 0);
  CHECK(receipt.owner_bytes == strlen(owner_map));
  CHECK(strlen(receipt.owner_sha256) == 64u);
  CHECK(strcmp(receipt.owner_sha256,
               "e11e4375b1879cdb796e334f442a16bda7d69cf35e0c2609faa0eae2e54e07eb") ==
        0);
  CHECK(strcmp(receipt.default_sha256,
               "6285b6a3ca15d2e935b058cf65d44bd68a6c8dbb631b2ab2d21e4c5a8d24140d") ==
        0);
  CHECK(strcmp(receipt.owner_sha256, receipt.selected_sha256) == 0);
  CHECK(strcmp(nxinput_gptk_action(&map, NXINPUT_GPTK_CONTEXT_GAMEPLAY,
                                   NXINPUT_GPTK_A),
               "player.action") == 0);
  CHECK(nxinput_gptk_load_receipt_json(&receipt, json, sizeof json) == 0);
  CHECK(strstr(json, "nxinput-gptk-load-evidence/1") != 0);
  CHECK(strstr(json, "\"source\":\"owner\"") != 0);
  CHECK(strstr(json, receipt.selected_sha256) != 0);
  /* C4: the receipt names the FORMAT of the map that won, and the JSON stays
   * path-free -- hash, schema and sanitized state, never the owner's path. */
  CHECK(receipt.selected_gptk_schema == NXINPUT_GPTK_SCHEMA_V1);
  CHECK(strstr(json, "\"selected_gptk_schema\":1") != 0);
  CHECK(strchr(json, '/') == 0 || strstr(json, "gptk-load-evidence/1") != 0);
  CHECK(strstr(json, "/tmp/") == 0);
  CHECK(strstr(json, "NEXTOSCONTROLLERS") == 0);
  CHECK(nxinput_gptk_load_receipt_json(&receipt, json, 8u) == -1);
  CHECK(json[0] == '\0');

  /* C4: a V2 owner file goes through the SAME loader and the receipt names
   * schema 2. `null` binds nothing, so it never has to be in the allowlist. */
  {
    static const char v2_owner[] =
        "format = NEXTOS_CONTROLLERS/2\n"
        "[menu]\nA = ui.confirm\nB = ui.cancel\nX = null\nY = null\n"
        "L1 = null\nR1 = null\nL2 = null\nR2 = null\nL3 = null\n"
        "R3 = null\nSTART = null\nSELECT = null\nUP = null\nDOWN = null\n"
        "LEFT = null\nRIGHT = null\nLEFT_STICK = null\n"
        "RIGHT_STICK = null\n"
        "[gameplay]\nA = player.jump\nB = native\nX = null\nY = null\n"
        "L1 = null\nR1 = null\nL2 = null\nR2 = null\nL3 = null\n"
        "R3 = null\nSTART = null\nSELECT = null\nUP = null\nDOWN = null\n"
        "LEFT = null\nRIGHT = null\nLEFT_STICK = null\n"
        "RIGHT_STICK = null\n";

    write_at(owner_fd, v2_owner, strlen(v2_owner));
    CHECK(nxinput_gptk_load_at(owner_fd, defaults_fd, allowlist,
                               sizeof allowlist / sizeof allowlist[0], &map,
                               &receipt) == 0);
    CHECK(receipt.source == (uint8_t)NXINPUT_GPTK_LOAD_OWNER);
    CHECK(receipt.selected_gptk_schema == NXINPUT_GPTK_SCHEMA_V2);
    CHECK(nxinput_gptk_decide(&map, NXINPUT_GPTK_CONTEXT_GAMEPLAY,
                              NXINPUT_GPTK_L3, 0) ==
          NXINPUT_GPTK_DECIDE_SUPPRESS);
    CHECK(nxinput_gptk_decide(&map, NXINPUT_GPTK_CONTEXT_GAMEPLAY,
                              NXINPUT_GPTK_B, 0) ==
          NXINPUT_GPTK_DECIDE_NATIVE);
    CHECK(nxinput_gptk_load_receipt_json(&receipt, json, sizeof json) == 0);
    CHECK(strstr(json, "\"selected_gptk_schema\":2") != 0);
  }

  /* Invalid owner is preserved byte-for-byte; default is session-only. */
  write_at(owner_fd, malformed_map, strlen(malformed_map));
  CHECK(fstatat(owner_fd, MAP_NAME, &before, AT_SYMLINK_NOFOLLOW) == 0);
  CHECK(nxinput_gptk_load_at(owner_fd, defaults_fd, allowlist,
                             sizeof allowlist / sizeof allowlist[0], &map,
                             &receipt) == 0);
  CHECK(receipt.source ==
        (uint8_t)NXINPUT_GPTK_LOAD_DEFAULT_OWNER_REJECTED);
  CHECK(receipt.owner_error_code == NXINPUT_GPTK_ERR_MALFORMED);
  CHECK(strlen(receipt.owner_sha256) == 64u);
  CHECK(strcmp(receipt.default_sha256, receipt.selected_sha256) == 0);
  CHECK(fstatat(owner_fd, MAP_NAME, &after, AT_SYMLINK_NOFOLLOW) == 0);
  CHECK(before.st_ino == after.st_ino && before.st_size == after.st_size);
  check_file_at(owner_fd, malformed_map, strlen(malformed_map));
  CHECK(strcmp(nxinput_gptk_action(&map, NXINPUT_GPTK_CONTEXT_GAMEPLAY,
                                   NXINPUT_GPTK_A),
               "player.jump") == 0);

  /* Missing owner also selects the default without materializing a file. */
  CHECK(unlinkat(owner_fd, MAP_NAME, 0) == 0);
  CHECK(nxinput_gptk_load_at(owner_fd, defaults_fd, allowlist,
                             sizeof allowlist / sizeof allowlist[0], &map,
                             &receipt) == 0);
  CHECK(receipt.source ==
        (uint8_t)NXINPUT_GPTK_LOAD_DEFAULT_OWNER_MISSING);
  CHECK(receipt.owner_present == 0u && receipt.owner_error_code == 1007);
  CHECK(fstatat(owner_fd, MAP_NAME, &after, AT_SYMLINK_NOFOLLOW) != 0 &&
        errno == ENOENT);

  /* A symlink is never followed and the outside target stays untouched. */
  write_at(root_fd, default_map, strlen(default_map));
  CHECK(renameat(root_fd, MAP_NAME, root_fd, "outside") == 0);
  {
    int non_directory_fd = open(outside_path, O_RDONLY | O_CLOEXEC);

    CHECK(non_directory_fd >= 0);
    CHECK(nxinput_gptk_load_at(non_directory_fd, defaults_fd, allowlist,
                               sizeof allowlist / sizeof allowlist[0], &map,
                               &receipt) == 0);
    CHECK(receipt.source ==
          (uint8_t)NXINPUT_GPTK_LOAD_DEFAULT_OWNER_REJECTED);
    CHECK(receipt.owner_present == 0u &&
          receipt.owner_error_code == NXINPUT_GPTK_ERR_IO);
    CHECK(close(non_directory_fd) == 0);
  }
  CHECK(symlinkat(outside_path, owner_fd, MAP_NAME) == 0);
  CHECK(stat(outside_path, &before) == 0);
  CHECK(nxinput_gptk_load_at(owner_fd, defaults_fd, allowlist,
                             sizeof allowlist / sizeof allowlist[0], &map,
                             &receipt) == 0);
  CHECK(receipt.source ==
        (uint8_t)NXINPUT_GPTK_LOAD_DEFAULT_OWNER_REJECTED);
  CHECK(receipt.owner_error_code == NXINPUT_GPTK_ERR_IO);
  CHECK(fstatat(owner_fd, MAP_NAME, &after, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISLNK(after.st_mode));
  CHECK(stat(outside_path, &after) == 0 && before.st_size == after.st_size);
  CHECK(unlinkat(owner_fd, MAP_NAME, 0) == 0);

  /* FIFO/non-regular owners are opened nonblocking, rejected and preserved. */
  CHECK(mkfifoat(owner_fd, MAP_NAME, S_IRUSR | S_IWUSR) == 0);
  CHECK(nxinput_gptk_load_at(owner_fd, defaults_fd, allowlist,
                             sizeof allowlist / sizeof allowlist[0], &map,
                             &receipt) == 0);
  CHECK(receipt.source ==
        (uint8_t)NXINPUT_GPTK_LOAD_DEFAULT_OWNER_REJECTED);
  CHECK(receipt.owner_error_code == NXINPUT_GPTK_ERR_IO);
  CHECK(fstatat(owner_fd, MAP_NAME, &after, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISFIFO(after.st_mode));
  CHECK(unlinkat(owner_fd, MAP_NAME, 0) == 0);

  /* Bounded heap read and semantic allowlist fail closed to the default. */
  write_oversized_at(owner_fd);
  CHECK(nxinput_gptk_load_at(owner_fd, defaults_fd, allowlist,
                             sizeof allowlist / sizeof allowlist[0], &map,
                             &receipt) == 0);
  CHECK(receipt.owner_error_code == NXINPUT_GPTK_ERR_TOO_LARGE);
  CHECK(receipt.owner_sha256[0] == '\0');
  write_at(owner_fd, unknown_action_map, strlen(unknown_action_map));
  CHECK(nxinput_gptk_load_at(owner_fd, defaults_fd, allowlist,
                             sizeof allowlist / sizeof allowlist[0], &map,
                             &receipt) == 0);
  CHECK(receipt.owner_error_code == NXINPUT_GPTK_ERR_UNKNOWN_NAME);
  check_file_at(owner_fd, unknown_action_map, strlen(unknown_action_map));

  /* Without a valid immutable default there is no safe session fallback. */
  write_at(defaults_fd, malformed_map, strlen(malformed_map));
  memset(&map, 0x7f, sizeof map);
  CHECK(nxinput_gptk_load_at(owner_fd, defaults_fd, allowlist,
                             sizeof allowlist / sizeof allowlist[0], &map,
                             &receipt) == NXINPUT_GPTK_ERR_MALFORMED);
  CHECK(receipt.source == (uint8_t)NXINPUT_GPTK_LOAD_NONE);
  CHECK(map.api_version == 0u);

  CHECK(unlinkat(owner_fd, MAP_NAME, 0) == 0);
  CHECK(unlinkat(defaults_fd, MAP_NAME, 0) == 0);
  CHECK(unlink(outside_path) == 0);
  CHECK(close(defaults_fd) == 0);
  CHECK(close(owner_fd) == 0);
  CHECK(close(root_fd) == 0);
  CHECK(rmdir(defaults_path) == 0);
  CHECK(rmdir(owner_path) == 0);
  CHECK(rmdir(root) == 0);
  (void)puts("gptk loader tests: ok (owner preserved, fallback session-only)");
  return 0;
}
