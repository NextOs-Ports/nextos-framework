/* SPDX-License-Identifier: GPL-3.0-only */
/* Este port NÃO linka -lSDL2: a SDL de execução é a do firmware, aberta por
 * dlopen(RTLD_GLOBAL) pelo bridge e resolvida por dlsym. A costura C6 do
 * framework (nxc6_glue.c, código compartilhado com ports que linkam a SDL)
 * chama entradas SDL pelo nome: aqui elas viram trampolins de
 * visibilidade OCULTA (não saem no dynsym, logo dlsym(RTLD_DEFAULT,"SDL_free")
 * continua achando a SDL real) que repassam ao símbolo da biblioteca já
 * carregada. Falha fechada com log se a SDL não estiver carregada. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Sem SDL.h de propósito: o cabeçalho força visibilidade default nessas
 * declarações e elas sairiam no dynsym. O GUID é o layout ABI da SDL2. */
typedef struct { unsigned char data[16]; } SDL_JoystickGUID;
typedef struct SDL_Joystick SDL_Joystick;

#define BT_HIDDEN __attribute__((visibility("hidden")))

static void *bt_sdl_lib_sym(const char *name)
{
    static void *lib;
    static int tried;
    if (!lib && !tried) {
        tried = 1;
        static const char *const names[] = { "libSDL2-2.0.so.0", "libSDL2.so", "libSDL2-2.0.so", NULL };
        for (int i = 0; names[i] && !lib; i++)
            lib = dlopen(names[i], RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);
        if (!lib)
            fprintf(stderr, "[bt/sdl] system SDL2 not loaded yet; C6 seam helpers unavailable\n");
    }
    void *sym = lib ? dlsym(lib, name) : NULL;
    if (!sym)
        fprintf(stderr, "[bt/sdl] missing SDL symbol %s\n", name);
    return sym;
}

BT_HIDDEN SDL_JoystickGUID SDL_JoystickGetGUIDFromString(const char *pchGUID)
{
    SDL_JoystickGUID (*fn)(const char *) = (SDL_JoystickGUID (*)(const char *))bt_sdl_lib_sym("SDL_JoystickGetGUIDFromString");
    SDL_JoystickGUID zero;
    memset(&zero, 0, sizeof zero);
    return fn ? fn(pchGUID) : zero;
}

BT_HIDDEN char *SDL_GameControllerMappingForGUID(SDL_JoystickGUID guid)
{
    char *(*fn)(SDL_JoystickGUID) = (char *(*)(SDL_JoystickGUID))bt_sdl_lib_sym("SDL_GameControllerMappingForGUID");
    return fn ? fn(guid) : NULL;
}

BT_HIDDEN int SDL_GameControllerAddMapping(const char *mappingString)
{
    int (*fn)(const char *) = (int (*)(const char *))bt_sdl_lib_sym("SDL_GameControllerAddMapping");
    return fn ? fn(mappingString) : -1;
}

BT_HIDDEN void SDL_free(void *mem)
{
    void (*fn)(void *) = (void (*)(void *))bt_sdl_lib_sym("SDL_free");
    if (fn)
        fn(mem);
}

/* nxinput 0.11.8 measures the provider table per admitted instance and owns
 * the precise subsystem staging predicate. These hidden trampolines let its
 * shared glue call the already-loaded firmware SDL without linking or
 * exporting a private SDL ABI from this executable. */
BT_HIDDEN int SDL_NumJoysticks(void)
{
    int (*fn)(void) = (int (*)(void))bt_sdl_lib_sym("SDL_NumJoysticks");
    return fn ? fn() : 0;
}

BT_HIDDEN SDL_Joystick *SDL_JoystickOpen(int device_index)
{
    SDL_Joystick *(*fn)(int) =
        (SDL_Joystick *(*)(int))bt_sdl_lib_sym("SDL_JoystickOpen");
    return fn ? fn(device_index) : NULL;
}

BT_HIDDEN void SDL_JoystickClose(SDL_Joystick *joystick)
{
    void (*fn)(SDL_Joystick *) =
        (void (*)(SDL_Joystick *))bt_sdl_lib_sym("SDL_JoystickClose");
    if (fn)
        fn(joystick);
}

BT_HIDDEN uint32_t SDL_WasInit(uint32_t flags)
{
    uint32_t (*fn)(uint32_t) =
        (uint32_t (*)(uint32_t))bt_sdl_lib_sym("SDL_WasInit");
    return fn ? fn(flags) : 0u;
}
