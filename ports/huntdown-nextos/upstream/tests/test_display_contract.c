#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "huntdown_display.h"

static int expect_size(const char *label, int expected_w, int expected_h,
                       const char *expected_source) {
  int width = 0, height = 0;
  const char *source = NULL;
  if (!hd_display_size_detect(&width, &height, &source) ||
      width != expected_w || height != expected_h || !source ||
      strcmp(source, expected_source) != 0) {
    fprintf(stderr, "%s: got %dx%d from %s, expected %dx%d from %s\n",
            label, width, height, source ? source : "none", expected_w,
            expected_h, expected_source);
    return 0;
  }
  return 1;
}

static void clear_display_env(void) {
  unsetenv("HD_SCREEN_W");
  unsetenv("HD_SCREEN_H");
  unsetenv("HD_SCREEN_WIDTH");
  unsetenv("HD_SCREEN_HEIGHT");
  unsetenv("DISPLAY_WIDTH");
  unsetenv("DISPLAY_HEIGHT");
}

int main(void) {
  clear_display_env();
  setenv("DISPLAY_WIDTH", "640", 1);
  setenv("DISPLAY_HEIGHT", "480", 1);
  if (!expect_size("4:3 launcher", 640, 480,
                   "launcher display contract"))
    return 1;

  setenv("DISPLAY_WIDTH", "1920", 1);
  setenv("DISPLAY_HEIGHT", "1080", 1);
  if (!expect_size("16:9 launcher", 1920, 1080,
                   "launcher display contract"))
    return 1;

  setenv("HD_SCREEN_W", "1280", 1);
  setenv("HD_SCREEN_H", "720", 1);
  if (!expect_size("diagnostic override", 1280, 720,
                   "HD_SCREEN override"))
    return 1;

  unsetenv("HD_SCREEN_H");
  if (!expect_size("incomplete override", 1920, 1080,
                   "launcher display contract"))
    return 1;

  puts("huntdown display contract: PASS");
  return 0;
}
