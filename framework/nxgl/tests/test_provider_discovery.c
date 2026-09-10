/* SPDX-License-Identifier: GPL-3.0-only */
/* Sonda do gate de descoberta/reparo de provedor. */
#include "nxgl_provider_discovery_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_teardown_calls;
static void count_teardown(void) { ++g_teardown_calls; }

int main(int argc, char **argv) {
  static const char *const k_symbols[] = {"glOrthof", "glDrawArrays", "glClear"};
  const char *mode = argc > 1 ? argv[1] : "";
  const char *dir = argc > 2 ? argv[2] : "";
  const char *renderer = argc > 3 ? argv[3] : "";
  const char *dirs[1];
  char found[NXGL_PROVIDER_RECOVERY_PATH_MAX];

  dirs[0] = dir;

  if (strcmp(mode, "broken") == 0) {
    printf("broken=%d\n", nxgl_provider_renderer_is_broken(
                              argc > 2 && argv[2][0] ? argv[2] : NULL));
    return 0;
  }
  if (strcmp(mode, "discover") == 0) {
    int ok = nxgl_provider_discover_unified(k_symbols, 3u, dirs, 1u, found,
                                            sizeof found);
    printf("discover=%d name=%s\n", ok,
           ok ? (strrchr(found, '/') ? strrchr(found, '/') + 1 : found) : "-");
    return 0;
  }
  if (strcmp(mode, "repair-hint") == 0) {
    nxgl_provider_repair_options options;
    nxgl_provider_repair_receipt receipt;
    int outcome;

    nxgl_provider_repair_options_init(&options);
    options.renderer = renderer[0] != '\0' ? renderer : NULL;
    options.video_backend = "kmsdrm";
    options.required_gles_symbols = k_symbols;
    options.required_gles_symbol_count = 3u;
    options.extra_library_dirs = dirs;
    options.extra_library_dir_count = 1u;
    options.teardown = count_teardown;
    options.argv = NULL;
    options.window_opened = 1;
    options.context_current = 1;
    options.drawable_positive = 1;
    outcome = nxgl_provider_repair_if_renderer_broken(&options, &receipt);
    printf("outcome=%d teardown=%d egl=%s gl=%s\n", outcome, g_teardown_calls,
           getenv("SDL_VIDEO_EGL_DRIVER") ? getenv("SDL_VIDEO_EGL_DRIVER")
                                          : "-",
           getenv("SDL_VIDEO_GL_DRIVER") ? getenv("SDL_VIDEO_GL_DRIVER")
                                         : "-");
    return 0;
  }
  if (strcmp(mode, "rollback") == 0) {
    nxgl_provider_repair_receipt receipt;
    int first = nxgl_provider_precontext_rollback(&receipt);
    int second = nxgl_provider_precontext_rollback(&receipt);
    printf("rollback=%d again=%d egl=%s gl=%s\n", first, second,
           getenv("SDL_VIDEO_EGL_DRIVER") ? getenv("SDL_VIDEO_EGL_DRIVER")
                                          : "-",
           getenv("SDL_VIDEO_GL_DRIVER") ? getenv("SDL_VIDEO_GL_DRIVER")
                                         : "-");
    return 0;
  }
  if (strcmp(mode, "precontext") == 0) {
    nxgl_provider_repair_options options;
    nxgl_provider_repair_receipt receipt;
    static const char *const k_syms[] = {"glOrthof", "glDrawArrays", "glClear"};
    int outcome;

    nxgl_provider_repair_options_init(&options);
    options.video_backend = "kmsdrm";
    options.required_gles_symbols = k_syms;
    options.required_gles_symbol_count = 3u;
    options.extra_library_dirs = dirs;
    options.extra_library_dir_count = 1u;
    options.teardown = count_teardown;
    options.argv = NULL;
    outcome = nxgl_provider_repair_precontext(
        &options, NXGL_OPEN_STAGE_V2_WINDOW_CREATE,
        NXGL_OPEN_REASON_V2_WINDOW_FAILED, &receipt);
    printf("outcome=%d teardown=%d\n", outcome, g_teardown_calls);
    return 0;
  }
  {
    nxgl_provider_repair_options options;
    nxgl_provider_repair_receipt receipt;
    int outcome;

    nxgl_provider_repair_options_init(&options);
    options.renderer = renderer[0] != '\0' ? renderer : NULL;
    options.video_backend = "kmsdrm";
    options.window_opened = 1;
    options.context_current = 1;
    options.drawable_positive = 1;
    options.required_gles_symbols = k_symbols;
    options.required_gles_symbol_count = 3u;
    options.extra_library_dirs = dirs;
    options.extra_library_dir_count = 1u;
    options.teardown = count_teardown;
    options.argv = NULL; /* sem argv o re-exec nao acontece: o gate nao troca
                          * o proprio processo. */
    outcome = nxgl_provider_repair_if_renderer_broken(&options, &receipt);
    printf("outcome=%d teardown=%d\n", outcome, g_teardown_calls);
    return 0;
  }
}
