/* Sally Face adapter for the framework NEXTOSCONTROLLERS.gptk contract. */
#ifndef SF_GPTK_ADAPTER_H
#define SF_GPTK_ADAPTER_H

#include <stddef.h>

#include "nxinput_gptk.h"

typedef void (*sf_gptk_button_delivery_fn)(void *user, int android_keycode,
                                           int pressed, int physical_control);

int sf_gptk_init(const char *game_dir, sf_gptk_button_delivery_fn delivery,
                 void *user);
int sf_gptk_init_text(const char *text, size_t length,
                      sf_gptk_button_delivery_fn delivery, void *user,
                      const char *source_label);
void sf_gptk_close(void);

int sf_gptk_control_owned(int control);
const char *sf_gptk_control_action(int control);
void sf_gptk_feed_button(int control, int pressed, float value);

/* Returns 1 when the GPTK map owns this stick. For sf.move, writes the
 * semantic movement vector exactly once; unsupported mappings fail at load. */
int sf_gptk_route_stick(int control, float x, float y,
                        float *move_x, float *move_y);

/* Runtime accounting used by the no-double-input receipt. Native delivery
 * sites call these only for the path that actually reaches the game. */
void sf_gptk_note_native_button_delivery(int control);
void sf_gptk_note_native_stick_delivery(int control);
void sf_gptk_periodic_receipt(unsigned long frame);
unsigned long sf_gptk_raw_duplicate_count(void);

#endif
