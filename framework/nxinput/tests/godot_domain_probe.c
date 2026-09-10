/* SPDX-License-Identifier: GPL-3.0-only */
/* Probe for the C5A domain gate. Prints, for each ordinal domain the adapter
 * implements, the ordinal a set of probe evdev codes actually receives. The
 * gate re-derives the same signature from the PINNED upstream sources and
 * compares behaviour, not tables.
 *
 * CLASS: SOURCE_AUDIT support. This is our own code answering; it is not an
 * executed engine. */
#include "nxinput_godot.h"

#include <stdio.h>
#include <string.h>

#define BPL (8u * (unsigned int)sizeof(unsigned long))
#define KEY_WORDS ((NXINPUT_GODOT_KEY_BITS + BPL - 1u) / BPL)
#define ABS_WORDS ((NXINPUT_GODOT_ABS_BITS + BPL - 1u) / BPL)

static const unsigned int probes[] = {0x040u, 0x110u, 0x130u};

int main(void) {
  static unsigned long keys[KEY_WORDS];
  static unsigned long abs_bits[ABS_WORDS];
  nxinput_godot_caps caps;
  int domain;
  size_t i;

  for (i = 0u; i < sizeof(probes) / sizeof(probes[0]); i++) {
    keys[probes[i] / BPL] |= 1ul << (probes[i] % BPL);
  }
  abs_bits[0] |= 1ul; /* one axis so the caps are structurally complete */
  if (nxinput_godot_caps_init(&caps, keys, NXINPUT_GODOT_KEY_BITS, abs_bits,
                              NXINPUT_GODOT_ABS_BITS) != 0) {
    return 1;
  }
  for (domain = (int)NXINPUT_GODOT_DOMAIN_UNDECLARED + 1;
       domain < (int)NXINPUT_GODOT_DOMAIN_COUNT; domain++) {
    printf("DOMAIN %s",
           nxinput_godot_domain_name((nxinput_godot_domain)domain));
    for (i = 0u; i < sizeof(probes) / sizeof(probes[0]); i++) {
      printf(" 0x%03x=%d", probes[i],
             nxinput_godot_button_ordinal((nxinput_godot_domain)domain,
                                          &caps, probes[i]));
    }
    printf("\n");
  }
  return 0;
}
