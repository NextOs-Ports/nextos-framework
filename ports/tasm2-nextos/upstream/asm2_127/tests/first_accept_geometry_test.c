#include <stdio.h>
#include <stdlib.h>

#include "first_accept.h"

static void expect_position(int width, int height, int expected_x,
                            int expected_y, int expected_legacy_retry) {
  int x = -1;
  int y = -1;
  asm2_first_accept_position(width, height, &x, &y);
  int retry =
      asm2_first_accept_legacy_marker_requires_retry(width, height);
  if (x == expected_x && y == expected_y &&
      retry == expected_legacy_retry)
    return;
  fprintf(stderr,
          "first-accept geometry mismatch for %dx%d: got %d,%d retry=%d; "
          "expected %d,%d retry=%d\n",
          width, height, x, y, retry, expected_x, expected_y,
          expected_legacy_retry);
  exit(1);
}

static void expect_stage_position(int stage, int width, int height,
                                  int expected_x, int expected_y) {
  int x = -1;
  int y = -1;
  asm2_first_run_touch_position(stage, width, height, &x, &y);
  if (x == expected_x && y == expected_y)
    return;
  fprintf(stderr,
          "first-run geometry mismatch stage=%d for %dx%d: got %d,%d; "
          "expected %d,%d\n",
          stage, width, height, x, y, expected_x, expected_y);
  exit(1);
}

int main(void) {
  /* Physically validated layouts remain byte-for-byte coordinate compatible. */
  expect_position(640, 480, 320, 379, 0);
  expect_position(1280, 720, 640, 635, 0);
  expect_position(1920, 1080, 960, 953, 0);

  /* The photographed 720x720 ACCEPT center is at y ~= 67.5%. */
  expect_position(720, 720, 360, 486, 1);

  /* Keep the square migration narrowly separated from 4:3 devices. */
  expect_position(900, 800, 450, 540, 1);
  expect_position(901, 800, 450, 632, 0);

  /* Legal and update-log buttons share the bottom action row. */
  expect_stage_position(ASM2_FIRST_RUN_TOUCH_LEGAL, 640, 480, 320, 379);
  expect_stage_position(ASM2_FIRST_RUN_TOUCH_UPDATE_LOG, 720, 720, 360, 486);

  /* Cloud notices use the centered modal action row. */
  expect_stage_position(ASM2_FIRST_RUN_TOUCH_CLOUD_NOTICE,
                        640, 480, 320, 300);
  expect_stage_position(ASM2_FIRST_RUN_TOUCH_CLOUD_NOTICE,
                        1280, 720, 640, 480);
  expect_stage_position(ASM2_FIRST_RUN_TOUCH_CLOUD_NOTICE,
                        720, 720, 360, 427);

  if (asm2_first_run_resume_stage(ASM2_FIRST_RUN_TOUCH_LEGAL) != 0 ||
      asm2_first_run_resume_stage(ASM2_FIRST_RUN_TOUCH_UPDATE_LOG) != 1 ||
      asm2_first_run_resume_stage(ASM2_FIRST_RUN_TOUCH_CLOUD_NOTICE) != 2 ||
      asm2_first_run_resume_stage(ASM2_FIRST_RUN_TOUCH_COMPLETE) != 0 ||
      asm2_first_run_resume_stage(-1) != 0 ||
      asm2_first_run_resume_stage(99) != 0) {
    fputs("first-run resume policy mismatch\n", stderr);
    return 1;
  }

  puts("first-accept geometry: PASS");
  return 0;
}
