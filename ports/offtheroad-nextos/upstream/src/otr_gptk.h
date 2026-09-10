/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef OTR_GPTK_H
#define OTR_GPTK_H

#include <stddef.h>

/* Port-owned bridge between the framework's semantic GPTK dispatcher and the
 * real Off The Road JNI/cocos input entry points.  The framework owns parsing,
 * edge latching, context changes, cursor motion and per-stick suppression; the
 * callbacks below are the game's concrete sinks. */
typedef struct otr_gptk_hooks {
  void (*digital)(const char *action, int pressed, float value);
  void (*vector)(const char *action, float x, float y);
} otr_gptk_hooks;

int otr_gptk_init(const char *game_dir, int drawable_w, int drawable_h,
                  const otr_gptk_hooks *hooks, char *error,
                  size_t error_size);
int otr_gptk_ready(void);
void otr_gptk_set_gameplay(int gameplay);
void otr_gptk_set_primary_mask(unsigned int control_mask);
int otr_gptk_button_mapped(int control);
int otr_gptk_stick_owned(int control);
void otr_gptk_feed_button(int control, int pressed, float value);
void otr_gptk_feed_stick(int control, float x, float y, float dt_seconds);
void otr_gptk_note_terminal_chord(void);
void otr_gptk_note_terminal_receipt(const char *source,
                                    unsigned int delivery_count,
                                    unsigned int poll_count,
                                    int independent_guest_loop);
void otr_gptk_release_all(void);
int otr_gptk_write_receipt(const char *path);
int otr_gptk_write_load_receipt(const char *path);
const char *otr_gptk_context_name(void);

#endif /* OTR_GPTK_H */
