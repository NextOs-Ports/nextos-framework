#include "first_accept.h"

#include <stdint.h>

static int positive_extent(int extent) {
  return extent > 0 ? extent : 1;
}

static int is_square_layout(int width, int height) {
  /* Keep the new branch deliberately narrow. Existing 4:3 and widescreen
   * coordinates have physical-device proof; only near-square panels need the
   * layout observed on the 720x720 R36 Ultra. */
  return (int64_t)width * 8 <= (int64_t)height * 9;
}

void asm2_first_accept_position(int drawable_width, int drawable_height,
                                int *x, int *y) {
  const int width = positive_extent(drawable_width);
  const int height = positive_extent(drawable_height);
  const int square = is_square_layout(width, height);
  const int compact =
      (int64_t)width * 2 <= (int64_t)height * 3;
  const int vertical_thousandths = square ? 675 : (compact ? 790 : 883);

  if (x)
    *x = width / 2;
  if (y)
    *y = (int)(((int64_t)height * vertical_thousandths) / 1000);
}

void asm2_first_run_touch_position(int stage, int drawable_width,
                                   int drawable_height, int *x, int *y) {
  const int width = positive_extent(drawable_width);
  const int height = positive_extent(drawable_height);

  if (stage == ASM2_FIRST_RUN_TOUCH_CLOUD_NOTICE) {
    /* The cloud dialog is centered in the 1280x720 virtual layout. At 4:3 it
     * is scaled to 640x360 and centered vertically: 60 + 480/2 = 300. */
    if (x)
      *x = width / 2;
    if (y)
      *y = (int)((int64_t)height / 2 + (int64_t)width * 3 / 32);
    return;
  }

  /* The update-log stage normally uses native HID. Returning its visible
   * button position here keeps the layout contract complete for diagnostics. */
  asm2_first_accept_position(width, height, x, y);
}

int asm2_first_accept_legacy_marker_requires_retry(int drawable_width,
                                                    int drawable_height) {
  return is_square_layout(positive_extent(drawable_width),
                          positive_extent(drawable_height));
}

int asm2_first_run_resume_stage(int stored_stage) {
  if (stored_stage >= ASM2_FIRST_RUN_TOUCH_LEGAL &&
      stored_stage < ASM2_FIRST_RUN_TOUCH_COMPLETE)
    return stored_stage;
  return ASM2_FIRST_RUN_TOUCH_LEGAL;
}
