/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_sdl -- see include/nxinput_sdl.h. Pure. */
#include "nxinput_sdl.h"

#include <string.h>

#define BITS_PER_LONG (8u * (unsigned int)sizeof(unsigned long))

static int bit_set(const unsigned long *bits, size_t bit_count,
                   unsigned int code) {
  if (bits == 0 || (size_t)code >= bit_count) {
    return 0;
  }
  return (int)((bits[code / BITS_PER_LONG] >> (code % BITS_PER_LONG)) & 1ul);
}

static int is_hat_code(unsigned int code) {
  return code >= NXINPUT_GODOT_ABS_HAT0X && code <= NXINPUT_GODOT_ABS_HAT3Y;
}

/* Does this pad really present the hat PAIR that owns `code`? The backends
 * only remove a hat pair from the axis numbering when BOTH halves exist; a
 * lone ABS_HAT0X stays an ordinary axis. */
static int hat_pair_present(const nxinput_godot_caps *caps, unsigned int code) {
  unsigned int base = NXINPUT_GODOT_ABS_HAT0X +
                      ((code - NXINPUT_GODOT_ABS_HAT0X) / 2u) * 2u;
  return bit_set(caps->abs_bits, caps->abs_bit_count, base) &&
         bit_set(caps->abs_bits, caps->abs_bit_count, base + 1u);
}

/* Does this domain's axis scan pass over `code`? The three states are real
 * backend behaviours, not degrees of strictness. */
static int axis_is_skipped(const nxinput_sdl_plan *plan,
                           const nxinput_godot_caps *caps, unsigned int code) {
  if (!is_hat_code(code)) {
    return 0;
  }
  if (plan->axis_skips_hats == NXINPUT_SDL_HATS_ALL) {
    return 1;
  }
  if (plan->axis_skips_hats == NXINPUT_SDL_HATS_DETECTED) {
    return hat_pair_present(caps, code);
  }
  return 0;
}

/* Transcribed from the pinned upstream sources. tests/c6_domain_gate.py
 * re-derives every field below from those exact files and fails if any of
 * them drifts, so this table is a claim under test, not a convention. */
static const nxinput_sdl_plan plans[NXINPUT_SDL_DOMAIN_COUNT] = {
    /* UNDECLARED -- deliberately empty; no default is ever applied. */
    {{{0u, 0u}, {0u, 0u}}, 0u, {0u, 0u}, 0},
    /* JOYDEV_LEGACY: one ascending sweep from BTN_MISC, hats numbered as
     * ordinary axes. */
    {{{NXINPUT_GODOT_BTN_MISC, NXINPUT_GODOT_KEY_MAX}, {0u, 0u}},
     1u,
     {0u, NXINPUT_GODOT_ABS_MAX},
     NXINPUT_SDL_HATS_KEPT},
    /* SDL2_LEGACY_EVDEV (2.0.10 ConfigJoystick: same button scan, but
     * `if (i == ABS_HAT0X) { i = ABS_HAT3Y; continue; }` drops the whole hat
     * range whether or not a pair was detected). */
    {{{NXINPUT_GODOT_BTN_JOYSTICK, NXINPUT_GODOT_KEY_MAX},
      {0u, NXINPUT_GODOT_BTN_JOYSTICK}},
     2u,
     {0u, NXINPUT_GODOT_ABS_MAX},
     NXINPUT_SDL_HATS_ALL},
    /* SDL2_EVDEV (SDL_sysjoystick.c: for (i = BTN_JOYSTICK; i < KEY_MAX)
     * then for (i = 0; i < BTN_JOYSTICK); axes for (i = 0; i < ABS_MAX)
     * skipping the pairs it classified as hats). */
    {{{NXINPUT_GODOT_BTN_JOYSTICK, NXINPUT_GODOT_KEY_MAX},
      {0u, NXINPUT_GODOT_BTN_JOYSTICK}},
     2u,
     {0u, NXINPUT_GODOT_ABS_MAX},
     NXINPUT_SDL_HATS_DETECTED},
    /* SDL3_EVDEV -- stated separately on purpose. That it currently equals
     * the SDL2 plan is the gate's measured finding, not this file's premise:
     * if a future pin diverges, only this row moves. */
    {{{NXINPUT_GODOT_BTN_JOYSTICK, NXINPUT_GODOT_KEY_MAX},
      {0u, NXINPUT_GODOT_BTN_JOYSTICK}},
     2u,
     {0u, NXINPUT_GODOT_ABS_MAX},
     NXINPUT_SDL_HATS_DETECTED},
};

static int domain_valid(nxinput_sdl_domain domain) {
  return (int)domain > (int)NXINPUT_SDL_DOMAIN_UNDECLARED &&
         (int)domain < (int)NXINPUT_SDL_DOMAIN_COUNT;
}

static int caps_valid(const nxinput_godot_caps *caps) {
  return caps != 0 &&
         (caps->api_version == NXINPUT_GODOT_API_VERSION ||
          caps->api_version == 2u) &&
         (caps->struct_size == sizeof(*caps) ||
          caps->struct_size == NXINPUT_GODOT_CAPS_SIZE_V2) &&
         caps->key_bits != 0 && caps->abs_bits != 0;
}

const nxinput_sdl_plan *nxinput_sdl_domain_plan(nxinput_sdl_domain domain) {
  if (!domain_valid(domain)) {
    return 0;
  }
  return &plans[domain];
}

int nxinput_sdl_domains_equal(nxinput_sdl_domain a, nxinput_sdl_domain b) {
  const nxinput_sdl_plan *pa = nxinput_sdl_domain_plan(a);
  const nxinput_sdl_plan *pb = nxinput_sdl_domain_plan(b);
  unsigned int i;

  if (pa == 0 || pb == 0) {
    return 0;
  }
  if (pa->button_ranges != pb->button_ranges ||
      pa->axis.first != pb->axis.first || pa->axis.limit != pb->axis.limit ||
      pa->axis_skips_hats != pb->axis_skips_hats) {
    return 0;
  }
  for (i = 0u; i < pa->button_ranges; i++) {
    if (pa->button[i].first != pb->button[i].first ||
        pa->button[i].limit != pb->button[i].limit) {
      return 0;
    }
  }
  return 1;
}

nxinput_sdl_domain nxinput_sdl_api_domain(nxinput_sdl_api api) {
  switch (api) {
    case NXINPUT_SDL_API_2: return NXINPUT_SDL_DOMAIN_SDL2_EVDEV;
    case NXINPUT_SDL_API_3: return NXINPUT_SDL_DOMAIN_SDL3_EVDEV;
    default: return NXINPUT_SDL_DOMAIN_UNDECLARED;
  }
}

int nxinput_sdl_button_code(nxinput_sdl_domain domain,
                            const nxinput_godot_caps *caps,
                            unsigned int ordinal) {
  const nxinput_sdl_plan *plan = nxinput_sdl_domain_plan(domain);
  unsigned int seen = 0u;
  unsigned int r;

  if (plan == 0 || !caps_valid(caps)) {
    return -1;
  }
  for (r = 0u; r < plan->button_ranges; r++) {
    unsigned int code;
    unsigned int limit = plan->button[r].limit;

    if ((size_t)limit > caps->key_bit_count) {
      limit = (unsigned int)caps->key_bit_count;
    }
    for (code = plan->button[r].first; code < limit; code++) {
      if (!bit_set(caps->key_bits, caps->key_bit_count, code)) {
        continue;
      }
      if (seen == ordinal) {
        return (int)code;
      }
      seen++;
    }
  }
  return -1;
}

int nxinput_sdl_button_ordinal(nxinput_sdl_domain domain,
                               const nxinput_godot_caps *caps,
                               unsigned int code) {
  const nxinput_sdl_plan *plan = nxinput_sdl_domain_plan(domain);
  unsigned int seen = 0u;
  unsigned int r;

  if (plan == 0 || !caps_valid(caps)) {
    return -1;
  }
  for (r = 0u; r < plan->button_ranges; r++) {
    unsigned int scan;
    unsigned int limit = plan->button[r].limit;

    if ((size_t)limit > caps->key_bit_count) {
      limit = (unsigned int)caps->key_bit_count;
    }
    for (scan = plan->button[r].first; scan < limit; scan++) {
      if (!bit_set(caps->key_bits, caps->key_bit_count, scan)) {
        continue;
      }
      if (scan == code) {
        return (int)seen;
      }
      seen++;
    }
  }
  return -1;
}

int nxinput_sdl_axis_code(nxinput_sdl_domain domain,
                          const nxinput_godot_caps *caps,
                          unsigned int ordinal) {
  const nxinput_sdl_plan *plan = nxinput_sdl_domain_plan(domain);
  unsigned int seen = 0u;
  unsigned int code;
  unsigned int limit;

  if (plan == 0 || !caps_valid(caps)) {
    return -1;
  }
  limit = plan->axis.limit;
  if ((size_t)limit > caps->abs_bit_count) {
    limit = (unsigned int)caps->abs_bit_count;
  }
  for (code = plan->axis.first; code < limit; code++) {
    if (!bit_set(caps->abs_bits, caps->abs_bit_count, code)) {
      continue;
    }
    if (axis_is_skipped(plan, caps, code)) {
      continue;
    }
    if (seen == ordinal) {
      return (int)code;
    }
    seen++;
  }
  return -1;
}

int nxinput_sdl_axis_ordinal(nxinput_sdl_domain domain,
                             const nxinput_godot_caps *caps,
                             unsigned int code) {
  const nxinput_sdl_plan *plan = nxinput_sdl_domain_plan(domain);
  unsigned int seen = 0u;
  unsigned int scan;
  unsigned int limit;

  if (plan == 0 || !caps_valid(caps)) {
    return -1;
  }
  limit = plan->axis.limit;
  if ((size_t)limit > caps->abs_bit_count) {
    limit = (unsigned int)caps->abs_bit_count;
  }
  for (scan = plan->axis.first; scan < limit; scan++) {
    if (!bit_set(caps->abs_bits, caps->abs_bit_count, scan)) {
      continue;
    }
    if (axis_is_skipped(plan, caps, scan)) {
      continue;
    }
    if (scan == code) {
      return (int)seen;
    }
    seen++;
  }
  return -1;
}

const char *nxinput_sdl_domain_name(nxinput_sdl_domain domain) {
  switch (domain) {
    case NXINPUT_SDL_DOMAIN_JOYDEV_LEGACY: return "joydev-legacy";
    case NXINPUT_SDL_DOMAIN_SDL2_LEGACY_EVDEV: return "sdl2-legacy-evdev";
    case NXINPUT_SDL_DOMAIN_SDL2_EVDEV: return "sdl2-evdev";
    case NXINPUT_SDL_DOMAIN_SDL3_EVDEV: return "sdl3-evdev";
    case NXINPUT_SDL_DOMAIN_UNDECLARED:
    default: return "undeclared";
  }
}

const char *nxinput_sdl_api_name(nxinput_sdl_api api) {
  switch (api) {
    case NXINPUT_SDL_API_2: return "sdl2";
    case NXINPUT_SDL_API_3: return "sdl3";
    default: return "unknown";
  }
}
