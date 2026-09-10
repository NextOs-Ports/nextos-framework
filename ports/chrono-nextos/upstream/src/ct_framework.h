/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef CT_FRAMEWORK_H
#define CT_FRAMEWORK_H

#include <stddef.h>
#include <stdint.h>

#include <SDL2/SDL.h>

#include "imports.h"

typedef struct ct_framework ct_framework;

ct_framework *ct_framework_create(void);
void ct_framework_destroy(ct_framework *framework);

int ct_framework_preflight(ct_framework *framework, const char *game_dir);
int ct_framework_open_graphics(ct_framework *framework,
                               SDL_Window **window, int *drawable_width,
                               int *drawable_height);
int ct_framework_present(ct_framework *framework);
void ct_framework_close_graphics(ct_framework *framework);

int ct_framework_open_input(ct_framework *framework);
void ct_framework_observe_input(ct_framework *framework,
                                const SDL_Event *event);
void ct_framework_poll_input(ct_framework *framework);
int ct_framework_has_controller(const ct_framework *framework);
/* First connected pad's SDL_GameController (NULL when none). For the exit
 * chord to bind SDL and mute the evdev fallback. Never close/remap it. */
struct _SDL_GameController;
struct _SDL_GameController *ct_framework_sdl_controller(const ct_framework *framework);
int ct_framework_consume_quit(ct_framework *framework);
void ct_framework_close_input(ct_framework *framework);

/* Called by the proven OpenSL ES adapter only after a real SDL device open. */
void ct_framework_audio_device_opened(unsigned int device_id, int frequency,
                                      unsigned int format,
                                      unsigned int channels,
                                      unsigned int samples);
void ct_framework_audio_device_failed(void);

int ct_framework_load_libcxx(ct_framework *framework, const char *path,
                             const DynLibFunction *imports,
                             size_t import_count);
int ct_framework_load_game(ct_framework *framework, const char *path);
uintptr_t ct_framework_find_export(ct_framework *framework,
                                   const char *name, int required);
uintptr_t ct_framework_find_active_export(const char *name);
uintptr_t ct_framework_active_guest_base(void);
int ct_framework_active_guest_contains(uintptr_t address);
int ct_framework_install_hook(ct_framework *framework, uintptr_t target,
                              uintptr_t replacement);
int ct_framework_start_game(ct_framework *framework, void *java_vm,
                            int32_t *jni_version);

#endif
