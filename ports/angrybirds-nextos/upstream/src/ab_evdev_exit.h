/* Adapter-local SELECT+START fallback for pads omitted by SDL mappings.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AB_EVDEV_EXIT_H
#define AB_EVDEV_EXIT_H

#include <stdint.h>

void ab_evdev_exit_init(uint32_t now);
int ab_evdev_exit_poll(uint32_t now);
void ab_evdev_exit_shutdown(void);

#endif
