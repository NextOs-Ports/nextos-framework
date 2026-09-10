#ifndef ASM2_FIRST_ACCEPT_H
#define ASM2_FIRST_ACCEPT_H

enum asm2_first_run_touch_stage {
  ASM2_FIRST_RUN_TOUCH_LEGAL = 0,
  ASM2_FIRST_RUN_TOUCH_UPDATE_LOG = 1,
  ASM2_FIRST_RUN_TOUCH_CLOUD_NOTICE = 2,
  ASM2_FIRST_RUN_TOUCH_COMPLETE = 3,
};

/* Returns the drawable-space center for each first-run modal. The update-log
 * stage uses native HID at runtime, but exposes its button center here for
 * geometry diagnostics. */
void asm2_first_run_touch_position(int stage, int drawable_width,
                                   int drawable_height, int *x, int *y);

/* Kept public so geometry regressions can pin the legal-dialog layouts. */
void asm2_first_accept_position(int drawable_width, int drawable_height,
                                int *x, int *y);

/* Version-1 markers were written after the old coordinate was tried. They are
 * reliable on the physically validated 4:3 and widescreen layouts, but not on
 * square layouts, where that coordinate falls below the ACCEPT button. */
int asm2_first_accept_legacy_marker_requires_retry(int drawable_width,
                                                    int drawable_height);

/* A persisted COMPLETE stage is only an attempted sequence. Until the game
 * creates ud_Control.sav, a restart must offer the recovery sequence again. */
int asm2_first_run_resume_stage(int stored_stage);

#endif
