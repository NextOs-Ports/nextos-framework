/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_SDL_H
#define NXINPUT_SDL_H

/*
 * nxinput_sdl -- V4-CONTROLLERS-03 / C6: the ordinal domains of the SDL
 * family, and nothing else.
 *
 * WHY A SEPARATE MODULE, AND WHAT IT IS NOT
 * -----------------------------------------
 * C5B proved Godot. Its pure core (nxinput_godot.c) already carries a
 * `sdl2-evdev` domain, transcribed from SDL 2.32.10 and re-verified by a
 * gate. C6 does NOT re-implement the mapping grammar, the serve or the
 * sovereign order: the grammar stays in nxinput_godot.c and the priority
 * stays literally in nxinput_sovereign/nxinput_authority (C3). Reinterpreting
 * either one locally is exactly what the mission forbids.
 *
 * What C6 genuinely needs, and nothing more, is the answer to one question:
 * DO SDL2 AND SDL3 NUMBER THE SAME PAD THE SAME WAY? A PortMaster mapping is
 * a list of ordinals. If the two majors enumerate identically, a mapping is
 * BYTE-INTACT across them and any conversion would corrupt it; if they
 * diverge, a conversion is mandatory. The answer must be MEASURED from the
 * pinned sources, never assumed in either direction -- so this module states
 * each domain explicitly and tests/c6_domain_gate.py checks every one of them
 * against the pinned upstream files.
 *
 * MEASURED RESULT for the pins C6 fixes. SDL 2.28.5, SDL 2.32.10 and
 * SDL 3.2.30 enumerate a pad IDENTICALLY, so a mapping already authored in a
 * current SDL evdev domain crosses that major boundary byte-intact. Two things
 * genuinely do differ and are modelled as their own domains rather than
 * smoothed over: the legacy joydev dialect found in real PortMaster data,
 * which walks all key codes in plain ascending order and needs positive
 * capability proof before projection, and SDL2 2.0.10, whose axis scan skips
 * the entire ABS_HAT range instead of only the pairs it detected.
 *
 * PURE: no SDL headers, no I/O, no environment.
 */

#include "nxinput_godot.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_SDL_API_VERSION 1u

/* Which SDL major a binary is. Not a version test: only the two API shapes
 * the seam has real glue for. */
typedef enum nxinput_sdl_api {
  NXINPUT_SDL_API_2 = 0,
  NXINPUT_SDL_API_3,
  NXINPUT_SDL_API_COUNT
} nxinput_sdl_api;

/* The ordinal domains C6 knows about. */
typedef enum nxinput_sdl_domain {
  NXINPUT_SDL_DOMAIN_UNDECLARED = 0,
  /* The legacy kernel joydev dialect a lot of PortMaster material is written
   * in: every key code in plain ascending order, hats numbered as axes. */
  NXINPUT_SDL_DOMAIN_JOYDEV_LEGACY,
  /* SDL2's Linux evdev backend as it stood in 2.0.10 and around it: the same
   * button scan, but the axis scan jumps over the WHOLE ABS_HAT0X..ABS_HAT3Y
   * range unconditionally (`if (i == ABS_HAT0X) { i = ABS_HAT3Y; continue; }`)
   * instead of removing only the pairs it actually classified as hats.
   *
   * For an ordinary pad with complete hat pairs the two produce identical
   * numbering. They diverge on a pad that exposes an ABS_HAT code WITHOUT its
   * partner: the modern backend keeps it as an ordinary axis and the old one
   * drops it, so every axis after it shifts by one. That is a real ordinal
   * difference and it is why this is a separate domain rather than a footnote
   * on the next one. */
  NXINPUT_SDL_DOMAIN_SDL2_LEGACY_EVDEV,
  /* SDL2's Linux evdev backend, current: the high button range first, then
   * the low one; axes ascending with DETECTED hat pairs removed. */
  NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
  /* SDL3's Linux evdev backend. Stated separately ON PURPOSE: whether it
   * equals SDL2's is a measured fact, not a definition. */
  NXINPUT_SDL_DOMAIN_SDL3_EVDEV,
  NXINPUT_SDL_DOMAIN_COUNT
} nxinput_sdl_domain;

/* One scan range, half-open, exactly as the backend loop writes it. */
#define NXINPUT_SDL_HATS_KEPT 0
#define NXINPUT_SDL_HATS_DETECTED 1
#define NXINPUT_SDL_HATS_ALL 2

typedef struct nxinput_sdl_range {
  unsigned int first;
  unsigned int limit;
} nxinput_sdl_range;

/* The enumeration plan of one domain, in the form a gate can compare against
 * the loops in the pinned upstream source. */
typedef struct nxinput_sdl_plan {
  nxinput_sdl_range button[2];
  unsigned int button_ranges;
  nxinput_sdl_range axis;
  /* How the axis scan treats the ABS_HAT range:
   *   NXINPUT_SDL_HATS_KEPT      hats are numbered as ordinary axes (joydev)
   *   NXINPUT_SDL_HATS_DETECTED  only complete, detected pairs are removed
   *   NXINPUT_SDL_HATS_ALL       the whole ABS_HAT range is skipped, pair or
   *                              not (SDL2 2.0.10 and around it) */
  int axis_skips_hats;
} nxinput_sdl_plan;

/* The plan of a domain. NULL for UNDECLARED or an out-of-range value: no
 * default is ever applied. */
const nxinput_sdl_plan *nxinput_sdl_domain_plan(nxinput_sdl_domain domain);

/* Do these two domains number every pad identically? Compares the whole
 * plan, not a version number. */
int nxinput_sdl_domains_equal(nxinput_sdl_domain a, nxinput_sdl_domain b);

/* The domain an SDL major's evdev backend uses. */
nxinput_sdl_domain nxinput_sdl_api_domain(nxinput_sdl_api api);

/* Ordinal <-> evdev code inside one domain, driven by MEASURED capability.
 * Negative means the domain cannot name that ordinal/code at all. */
int nxinput_sdl_button_code(nxinput_sdl_domain domain,
                            const nxinput_godot_caps *caps,
                            unsigned int ordinal);
int nxinput_sdl_button_ordinal(nxinput_sdl_domain domain,
                               const nxinput_godot_caps *caps,
                               unsigned int code);
int nxinput_sdl_axis_code(nxinput_sdl_domain domain,
                          const nxinput_godot_caps *caps,
                          unsigned int ordinal);
int nxinput_sdl_axis_ordinal(nxinput_sdl_domain domain,
                             const nxinput_godot_caps *caps,
                             unsigned int code);

const char *nxinput_sdl_domain_name(nxinput_sdl_domain domain);
const char *nxinput_sdl_api_name(nxinput_sdl_api api);

#ifdef __cplusplus
}
#endif

#endif /* NXINPUT_SDL_H */
