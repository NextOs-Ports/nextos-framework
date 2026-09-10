/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef DEADTRIGGER_FRAMEWORK_BRIDGE_H
#define DEADTRIGGER_FRAMEWORK_BRIDGE_H

#include <stdint.h>

#include <SDL2/SDL.h>

typedef struct dt_graphics_evidence {
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
} dt_graphics_evidence;

int dt_framework_preflight(const char *game_dir);
int dt_framework_open_input(void);
void dt_framework_observe_input(const SDL_Event *event);
void dt_framework_poll_input(void);
void dt_framework_audio_opened(SDL_AudioDeviceID device,
                               const SDL_AudioSpec *actual);
void dt_framework_audio_failed(void);
int dt_framework_publish_graphics(const dt_graphics_evidence *evidence);
int dt_framework_publish_current_graphics(void);

#endif /* DEADTRIGGER_FRAMEWORK_BRIDGE_H */
