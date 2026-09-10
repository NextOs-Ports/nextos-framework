/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NX_PORT_FRAMEWORK_H
#define NX_PORT_FRAMEWORK_H

#include <stdint.h>

typedef int (*nx_port_runtime_fn)(int argc, char **argv);

int nx_port_framework_run(const char *fallback_port_id, int argc,
                          char **argv, nx_port_runtime_fn runtime);
int nx_port_framework_graphics_ready(void *sdl_window);
int nx_port_framework_input_ready(void);
void nx_port_framework_observe_event(const void *sdl_event);
int nx_port_framework_poll_input(void);
void nx_port_framework_audio_opened(uint32_t device_id, int frequency,
                                    uint32_t format, unsigned channels,
                                    unsigned samples);
void nx_port_framework_audio_failed(void);

#endif
