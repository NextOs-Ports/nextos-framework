#include <SDL3/SDL.h>

#include <stdio.h>

int main(void)
{
    if (!SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    printf("gamepads=%d driver=%s\n", count, SDL_GetCurrentVideoDriver());
    for (int i = 0; i < count; ++i) {
        SDL_Gamepad *pad = SDL_OpenGamepad(ids[i]);
        if (pad == NULL) {
            printf("id=%u open failed: %s\n", (unsigned)ids[i], SDL_GetError());
            continue;
        }
        printf("id=%u name=%s player=%d\n", (unsigned)ids[i],
            SDL_GetGamepadName(pad), SDL_GetGamepadPlayerIndex(pad));
        fflush(stdout);
        for (int sample = 0; sample < 60; ++sample) {
            SDL_PumpEvents();
            for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
                if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)button)) {
                    printf("sample=%d button=%d name=%s\n", sample, button,
                        SDL_GetGamepadStringForButton((SDL_GamepadButton)button));
                    fflush(stdout);
                }
            }
            SDL_Delay(100);
        }
        SDL_CloseGamepad(pad);
    }
    SDL_free(ids);
    SDL_Quit();
    return 0;
}
