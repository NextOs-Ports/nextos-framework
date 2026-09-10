#ifndef BBRACING2_CURSOR_BRIDGE_H
#define BBRACING2_CURSOR_BRIDGE_H

#include <stdbool.h>

#include <SDL3/SDL.h>

/* Touch-only menu fallback. All gamepad input is observed, never consumed. */
void cursor_bridge_handle_event(const SDL_Event *event);
void cursor_bridge_track_axis(SDL_GamepadAxis axis, Sint16 value);
void cursor_bridge_track_button(SDL_GamepadButton button, bool pressed);
void cursor_bridge_frame(SDL_Window *window);

#endif
