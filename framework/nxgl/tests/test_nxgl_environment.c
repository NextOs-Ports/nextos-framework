/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxgl.h"
#include "nxgl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                             \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

typedef struct saved_environment {
  int video_existed;
  int alias_existed;
  char *video;
  char *alias;
} saved_environment;

static char *duplicate_text(const char *value) {
  size_t size;
  char *copy;
  if (!value)
    return NULL;
  size = strlen(value) + 1u;
  copy = (char *)malloc(size);
  if (copy)
    memcpy(copy, value, size);
  return copy;
}

static saved_environment save_environment(void) {
  saved_environment saved;
  const char *value;
  memset(&saved, 0, sizeof(saved));
  value = getenv("SDL_VIDEODRIVER");
  saved.video_existed = value != NULL;
  saved.video = duplicate_text(value);
  value = getenv("SDL_VIDEO_DRIVER");
  saved.alias_existed = value != NULL;
  saved.alias = duplicate_text(value);
  CHECK(!saved.video_existed || saved.video != NULL);
  CHECK(!saved.alias_existed || saved.alias != NULL);
  return saved;
}

static void restore_environment(saved_environment *saved) {
  if (saved->video_existed)
    CHECK(setenv("SDL_VIDEODRIVER", saved->video, 1) == 0);
  else
    CHECK(unsetenv("SDL_VIDEODRIVER") == 0);
  if (saved->alias_existed)
    CHECK(setenv("SDL_VIDEO_DRIVER", saved->alias, 1) == 0);
  else
    CHECK(unsetenv("SDL_VIDEO_DRIVER") == 0);
  free(saved->video);
  free(saved->alias);
}

static void set_environment(const char *video, const char *alias) {
  if (video)
    CHECK(setenv("SDL_VIDEODRIVER", video, 1) == 0);
  else
    CHECK(unsetenv("SDL_VIDEODRIVER") == 0);
  if (alias)
    CHECK(setenv("SDL_VIDEO_DRIVER", alias, 1) == 0);
  else
    CHECK(unsetenv("SDL_VIDEO_DRIVER") == 0);
}

static void check_environment(const char *video, const char *alias) {
  const char *current = getenv("SDL_VIDEODRIVER");
  CHECK((!video && !current) ||
        (video && current && strcmp(video, current) == 0));
  current = getenv("SDL_VIDEO_DRIVER");
  CHECK((!alias && !current) ||
        (alias && current && strcmp(alias, current) == 0));
}

static char *make_long_value(size_t length, char byte) {
  char *value = (char *)malloc(length + 1u);
  CHECK(value != NULL);
  if (!value)
    return NULL;
  memset(value, byte, length);
  value[length] = '\0';
  return value;
}

static void run_retry_case(const char *video, const char *alias,
                           int callback_result, int create_alias_in_callback,
                           int expected_status, int expected_calls,
                           int expected_cleared) {
  int calls = -1;
  int cleared = -1;
  set_environment(video, alias);
  CHECK(nxgl_test_video_environment_retry(callback_result,
                                          create_alias_in_callback, &calls,
                                          &cleared) == expected_status);
  CHECK(calls == expected_calls);
  CHECK(cleared == expected_cleared);
  check_environment(video, alias);
}

static void test_exact_video_environment_transaction(void) {
  saved_environment saved = save_environment();
  char *long_video = make_long_value(NXGL_VIDEO_ENV_VALUE_MAX, 'v');
  char *long_alias = make_long_value(2048u, 'a');
  char *over_limit = make_long_value(NXGL_VIDEO_ENV_VALUE_MAX + 1u, 'x');

  run_retry_case(NULL, NULL, 0, 0, NXGL_NO_ACTION, 0, 0);
  run_retry_case("", NULL, 0, 0, NXGL_NO_ACTION, 0, 0);
  run_retry_case("inherited-video", "inherited-alias", 0, 0, 0, 1, 1);
  run_retry_case("inherited-video", "", 0, 0, 0, 1, 1);
  run_retry_case("inherited-video", NULL, 0, 1, 0, 1, 1);
  if (long_video && long_alias)
    run_retry_case(long_video, long_alias, -77, 0, -77, 1, 1);
  if (over_limit)
    run_retry_case(over_limit, "bounded-alias", 0, 0, NXGL_NO_ACTION, 0, 0);

  CHECK(nxgl_test_video_environment_retry(0, 0, NULL, NULL) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  free(long_video);
  free(long_alias);
  free(over_limit);
  restore_environment(&saved);
}

int main(void) {
  test_exact_video_environment_transaction();
  if (failures) {
    (void)fprintf(stderr, "%d nxgl environment test(s) failed\n", failures);
    return 1;
  }
  (void)fprintf(stdout,
                "nxgl exact synthetic video environment tests passed\n");
  return 0;
}
