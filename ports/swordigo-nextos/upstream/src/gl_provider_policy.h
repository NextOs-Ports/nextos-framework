/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SWORDIGO_GL_PROVIDER_POLICY_H
#define SWORDIGO_GL_PROVIDER_POLICY_H

/* Conservative filename prefilter only.  A positive result still requires
 * symbol validation and a real EGL initialization against the live kernel. */
int gl_provider_name_compatible(const char *video_backend,
                                const char *provider_name);

/* Vendored policy synchronized with nxgl 0.2.3. The caller owns all effects. */
typedef enum gl_provider_sdl_pair_plan {
  GL_PROVIDER_SDL_PAIR_INVALID = -1,
  GL_PROVIDER_SDL_PAIR_NO_ACTION = 0,
  GL_PROVIDER_SDL_PAIR_BIND_COHERENT = 1
} gl_provider_sdl_pair_plan;

gl_provider_sdl_pair_plan gl_provider_plan_sdl_pair(
    const char *video_backend, const char *renderer,
    const char *provider_name, int window_opened, int context_current,
    int drawable_positive, int exports_egl, int exports_engine_gles);

#endif
