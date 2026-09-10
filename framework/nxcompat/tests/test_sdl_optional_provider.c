/* SPDX-License-Identifier: GPL-3.0-only */
/* Real-provider proof and the ELF audited by run-sdl-optional-audit.sh: this
 * executable links the system SDL2 but must carry NO direct ELF import of
 * SDL_JoystickGetVendor/SDL_JoystickGetProduct (both SDL 2.0.6); every
 * baseline import stays at or below the universal 2.0.4 floor.  The optional
 * metadata is reached only through the nxcompat resolver, anchored on the
 * really loaded SDL, and — when the runtime supports virtual joysticks — is
 * proven by value against a virtual pad with a known VID/PID. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* dladdr */
#endif

#include "nxcompat_sdl_optional.h"

#include <SDL.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__,             \
              #condition, SDL_GetError());                                     \
      return 1;                                                                \
    }                                                                          \
  } while (0)

#define VIRTUAL_VENDOR 0x1209u
#define VIRTUAL_PRODUCT 0x4004u

static int provider_module_matches(const void *anchor, const void *symbol) {
  Dl_info anchor_info;
  Dl_info symbol_info;
  memset(&anchor_info, 0, sizeof(anchor_info));
  memset(&symbol_info, 0, sizeof(symbol_info));
  if (dladdr((void *)(uintptr_t)anchor, &anchor_info) == 0 ||
      dladdr((void *)(uintptr_t)symbol, &symbol_info) == 0)
    return 0;
  return anchor_info.dli_fbase && anchor_info.dli_fbase == symbol_info.dli_fbase;
}

int main(void) {
  nxcompat_sdl_optional_options options;
  nxcompat_sdl_optional_joystick_api api;
  nxcompat_sdl_joystick_ids ids;
  SDL_version runtime;
  int post_206;

  SDL_GetVersion(&runtime);
  post_206 = runtime.major > 2 ||
             (runtime.major == 2 &&
              (runtime.minor > 0 || runtime.patch >= 6));

  memset(&options, 0, sizeof(options));
  options.api_version = NXCOMPAT_API_VERSION_V2;
  options.struct_size = sizeof(options);
  options.provider_anchor = (const void *)SDL_GetVersion;

  CHECK(nxcompat_sdl_optional_resolve_joystick_api(&options, &api) ==
        NXCOMPAT_OK);
  if (!post_206) {
    /* A genuine floor-era SDL: absence must read as unknown metadata. */
    CHECK(api.get_vendor == NULL && api.get_product == NULL);
    printf("nxcompat-sdl-optional-provider: PASS (SDL %u.%u.%u predates "
           "2.0.6; optional metadata absent)\n",
           runtime.major, runtime.minor, runtime.patch);
    return 0;
  }

  CHECK(api.get_vendor != NULL && api.get_product != NULL);
  CHECK(provider_module_matches(options.provider_anchor,
                                (const void *)(uintptr_t)api.get_vendor));
  CHECK(provider_module_matches(options.provider_anchor,
                                (const void *)(uintptr_t)api.get_product));

#if SDL_VERSION_ATLEAST(2, 24, 0)
  if (runtime.major > 2 || runtime.minor >= 24) {
    /* Value proof on a virtual pad.  The virtual-joystick entry points are
     * themselves post-floor, so they are reached through the same resolver
     * and never imported directly. */
    typedef int (*attach_virtual_ex_fn)(const SDL_VirtualJoystickDesc *desc);
    typedef int (*detach_virtual_fn)(int device_index);
    union {
      void *object;
      attach_virtual_ex_fn attach;
    } attach_cast;
    union {
      void *object;
      detach_virtual_fn detach;
    } detach_cast;
    SDL_VirtualJoystickDesc desc;
    SDL_Joystick *joystick;
    int device_index;

    CHECK(nxcompat_sdl_optional_symbol(&options, "SDL_JoystickAttachVirtualEx",
                                       &attach_cast.object) == NXCOMPAT_OK);
    CHECK(nxcompat_sdl_optional_symbol(&options, "SDL_JoystickDetachVirtual",
                                       &detach_cast.object) == NXCOMPAT_OK);
    CHECK(attach_cast.object != NULL && detach_cast.object != NULL);
    CHECK(SDL_InitSubSystem(SDL_INIT_JOYSTICK) == 0);

    SDL_memset(&desc, 0, sizeof(desc));
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = 2;
    desc.nbuttons = 4;
    desc.vendor_id = VIRTUAL_VENDOR;
    desc.product_id = VIRTUAL_PRODUCT;
    device_index = attach_cast.attach(&desc);
    CHECK(device_index >= 0);
    joystick = SDL_JoystickOpen(device_index);
    CHECK(joystick != NULL);

    CHECK(nxcompat_sdl_optional_joystick_ids(&api, joystick, &ids) ==
          NXCOMPAT_OK);
    CHECK(ids.vendor_known == 1 && ids.vendor == VIRTUAL_VENDOR);
    CHECK(ids.product_known == 1 && ids.product == VIRTUAL_PRODUCT);

    SDL_JoystickClose(joystick);
    CHECK(detach_cast.detach(device_index) == 0);
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    printf("nxcompat-sdl-optional-provider: PASS (virtual pad value proof)\n");
    return 0;
  }
#endif

  printf("nxcompat-sdl-optional-provider: PASS (resolution and provider "
         "identity only; runtime lacks virtual joysticks)\n");
  return 0;
}
