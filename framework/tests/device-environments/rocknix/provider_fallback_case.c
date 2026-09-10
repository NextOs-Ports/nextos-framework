/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "nxgl.h"
#include "nxgl_provider_recovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    static const char *const symbols[] = {"glCreateShader"};
    nxgl_sdl_provider_probe_options_v2 options;
    nxgl_sdl_provider_probe_receipt_v2 receipt;
    const char *preload;
    int result;

    (void)unsetenv("SDL_VIDEO_EGL_DRIVER");
    (void)unsetenv("SDL_VIDEO_GL_DRIVER");
    if (setenv("LD_PRELOAD", "rocknix-owner-sentinel", 1) != 0)
        return 2;

    nxgl_sdl_provider_probe_options_v2_init(&options);
    nxgl_sdl_provider_probe_receipt_v2_init(&receipt);
    options.enabled = 1;
    options.mode = NXGL_PROVIDER_PROBE_V2_SYMBOLS_ONLY;
    options.video_backend = "wayland";
    options.provider = "/nxgl/rocknix-provider-does-not-exist.so";
    options.required_engine_gles_symbols = symbols;
    options.required_engine_gles_symbol_count = 1u;

    result = nxgl_probe_sdl_provider_v2(&options, &receipt);
    preload = getenv("LD_PRELOAD");
    if (result != NXGL_NO_ACTION ||
        receipt.reason != NXGL_PROVIDER_RECOVERY_V2_CANDIDATE_UNAVAILABLE ||
        getenv("SDL_VIDEO_EGL_DRIVER") != NULL ||
        getenv("SDL_VIDEO_GL_DRIVER") != NULL || !preload ||
        strcmp(preload, "rocknix-owner-sentinel") != 0)
        return 1;

    (void)printf("provider-fallback=candidate-unavailable action=no-action "
                 "egl_override=absent gles_override=absent "
                 "ld_preload=preserved\n");
    return 0;
}
