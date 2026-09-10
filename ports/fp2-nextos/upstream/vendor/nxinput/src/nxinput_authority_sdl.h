/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_AUTHORITY_SDL_H
#define NXINPUT_AUTHORITY_SDL_H

#include "nxinput_authority.h"

#include <SDL.h>

/* Fill `runtime` with the SDL2 effects of the sovereign adapter. */
int nxinput_authority_runtime_sdl(nxinput_authority_runtime *runtime,
                                  int consumer_accepts_raw);

#endif /* NXINPUT_AUTHORITY_SDL_H */
