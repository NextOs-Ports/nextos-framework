/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef KOTOR_FRAMEWORK_H
#define KOTOR_FRAMEWORK_H

#include <stddef.h>

#include <SDL2/SDL.h>

int kotor_framework_preflight(const char *game_dir);
int kotor_framework_android_module_initialized(size_t module_index);
int kotor_framework_android_module_jni(size_t module_index);
int kotor_framework_android_activity_created(void);
int kotor_framework_android_resumed(void);
int kotor_framework_run_delegated(int (*entry)(int, char **), int argc,
                                  char **argv, int *result);

void kotor_framework_observe_event(const SDL_Event *event);
void kotor_framework_poll_input(void);
void kotor_framework_observe_swap(SDL_Window *window);
void kotor_framework_observe_audio(SDL_AudioDeviceID device,
                                   const SDL_AudioSpec *obtained);
void kotor_framework_finish(void);

#endif
