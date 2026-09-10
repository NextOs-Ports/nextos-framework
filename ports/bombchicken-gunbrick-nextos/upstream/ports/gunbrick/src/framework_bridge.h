/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef GUNBRICK_FRAMEWORK_BRIDGE_H
#define GUNBRICK_FRAMEWORK_BRIDGE_H

#include <stdint.h>

#include <SDL2/SDL.h>

#include "nxinput.h"

typedef struct gb_graphics_evidence {
    int window_width;
    int window_height;
    int drawable_width;
    int drawable_height;
    int red_bits;
    int green_bits;
    int blue_bits;
    int alpha_bits;
    int depth_bits;
    int stencil_bits;
    int double_buffer;
    int profile_mask;
    int egl_config_id;
    int egl_renderable_type;
    int egl_surface_type;
    const char *backend;
    const char *gl_vendor;
    const char *gl_renderer;
    const char *gl_version;
    const char *glsl_version;
    const char *gl_extensions;
    const char *egl_vendor;
    const char *egl_version;
    const char *egl_client_apis;
} gb_graphics_evidence;

int gb_framework_preflight(const char *game_dir);
int gb_framework_publish_graphics(const gb_graphics_evidence *evidence);
int gb_framework_publish_input(const nxinput_context *input);
int gb_framework_publish_audio(SDL_AudioDeviceID device,
                               const SDL_AudioSpec *actual);
int gb_framework_ready(void);

#endif /* GUNBRICK_FRAMEWORK_BRIDGE_H */
