/* SPDX-License-Identifier: GPL-3.0-only */
/* Prints what nxinput_sdl CLAIMS each domain is, so the gate can hold the
 * claim against the pinned upstream sources. Prints nothing it derives from
 * those sources: the whole point is that the two are independent. */
#include "nxinput_sdl.h"

#include <stdio.h>

int main(void) {
  nxinput_sdl_domain domains[] = {NXINPUT_SDL_DOMAIN_JOYDEV_LEGACY,
                                  NXINPUT_SDL_DOMAIN_SDL2_LEGACY_EVDEV,
                                  NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
                                  NXINPUT_SDL_DOMAIN_SDL3_EVDEV};
  unsigned int i;

  for (i = 0u; i < sizeof(domains) / sizeof(domains[0]); i++) {
    const nxinput_sdl_plan *p = nxinput_sdl_domain_plan(domains[i]);
    unsigned int r;
    printf("domain %s buttons=", nxinput_sdl_domain_name(domains[i]));
    for (r = 0u; r < p->button_ranges; r++) {
      printf("%s[0x%x,0x%x)", r ? "+" : "", p->button[r].first,
             p->button[r].limit);
    }
    printf(" axes=[0x%x,0x%x) skip_hats=%d\n", p->axis.first, p->axis.limit,
           p->axis_skips_hats);
  }
  printf("equal sdl2_sdl3=%d\n",
         nxinput_sdl_domains_equal(NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
                                   NXINPUT_SDL_DOMAIN_SDL3_EVDEV));
  printf("equal legacy_sdl2=%d\n",
         nxinput_sdl_domains_equal(NXINPUT_SDL_DOMAIN_SDL2_LEGACY_EVDEV,
                                   NXINPUT_SDL_DOMAIN_SDL2_EVDEV));
  printf("equal joydev_sdl2=%d\n",
         nxinput_sdl_domains_equal(NXINPUT_SDL_DOMAIN_JOYDEV_LEGACY,
                                   NXINPUT_SDL_DOMAIN_SDL2_EVDEV));
  printf("api sdl2=%s api sdl3=%s\n",
         nxinput_sdl_domain_name(nxinput_sdl_api_domain(NXINPUT_SDL_API_2)),
         nxinput_sdl_domain_name(nxinput_sdl_api_domain(NXINPUT_SDL_API_3)));
  return 0;
}
