/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * V5 / A5 -- RED reproduction of the two P0 field incidents through the V4
 * pipeline (nxinput_pm_normalize_source with target = api_domain(SDL2)).
 *
 * This test PASSES when the corruption REPRODUCES: it is the fixture-backed
 * proof that V4 rewrote a Batocera-native mapping into the upstream order,
 * and it stays green forever as the documented cause. Expectations do NOT
 * come from the mapping under test: they come from the vendor DTS table
 * (position -> EV_KEY) in tests/v5/fixtures JSON files and from an independent
 * ascending table derived here from the capability bits alone.
 *
 * If the fixture does not reproduce the swap, this test FAILS.
 */
#include "../../include/nxinput_portmaster.h"
#include "../../include/nxinput_sdl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NLONG ((NXINPUT_GODOT_KEY_BITS + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long)))
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)

static void setb(unsigned long *b, unsigned c) { b[c / (8 * sizeof(unsigned long))] |= 1ul << (c % (8 * sizeof(unsigned long))); }

/* INDEPENDENT ascending table: sort the capability codes. Not nxinput_sdl. */
static int ascending_code(const unsigned *codes, unsigned n, unsigned ordinal) {
  unsigned i;
  unsigned sorted[64];
  for (i = 0; i < n; i++) sorted[i] = codes[i];
  for (i = 0; i < n; i++) { unsigned j; for (j = i + 1; j < n; j++) if (sorted[j] < sorted[i]) { unsigned t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t; } }
  return ordinal < n ? (int)sorted[ordinal] : -1;
}

static int binding_ordinal(const char *mapping, const char *key) {
  char pat[64]; const char *p;
  snprintf(pat, sizeof pat, ",%s:b", key);
  p = strstr(mapping, pat);
  return p ? atoi(p + strlen(pat)) : -1;
}

/* H700 gpio-keys table: 3 low keys + 0x130..0x13c (see fixtures). */
static const unsigned h700[] = {0x11, 0x72, 0x73, 0x130, 0x131, 0x132, 0x133, 0x134, 0x135, 0x136, 0x137, 0x138, 0x139, 0x13a, 0x13b, 0x13c};
#define NH700 (sizeof h700 / sizeof h700[0])

static void run_case(const char *id, const char *mapping, unsigned expect_bindings, unsigned expect_volume,
                     const char *a_key, unsigned phys_a, const char *start_key) {
  unsigned long bits[NLONG]; unsigned i; char out[4096];
  nxinput_pm_source_evidence ev; int r; int ord; int code;
  memset(bits, 0, sizeof bits);
  for (i = 0; i < NH700; i++) setb(bits, h700[i]);
  printf("== %s\n", id);
  r = nxinput_pm_normalize_source(mapping, bits, NXINPUT_GODOT_KEY_BITS,
                                  nxinput_sdl_api_domain(NXINPUT_SDL_API_2),
                                  "19000000010000000100000000010000", out, sizeof out, &ev);
  CHECK(r == NXINPUT_PM_REWRITTEN, "V4 rewrites the CFW-native line (result=rewritten)");
  CHECK(ev.rewritten_bindings == expect_bindings, "rewritten_bindings matches the field receipt");
  CHECK(ev.legacy_volume_markers == expect_volume, "volume_markers matches the field receipt");
  /* Physical A in the source, resolved by the independent ascending table. */
  ord = binding_ordinal(mapping, a_key);
  code = ascending_code(h700, NH700, (unsigned)ord);
  CHECK(code == (int)phys_a, "source 'a' resolves to the DTS south key under the ascending provider (source is native)");
  /* After the V4 rewrite, on the ascending provider that stayed loaded: */
  ord = binding_ordinal(out, a_key);
  code = ascending_code(h700, NH700, (unsigned)ord);
  printf("     rewritten a:b%d -> EV_KEY 0x%x on the ascending provider\n", ord, code);
  CHECK(code != (int)phys_a && code < 0x130, "RED: rewritten 'a' lands on a LOW key (volume/unknown), not the south button");
  ord = binding_ordinal(out, start_key);
  code = ascending_code(h700, NH700, (unsigned)ord);
  printf("     rewritten start:b%d -> EV_KEY 0x%x\n", ord, code);
  CHECK(code == 0x134, "RED: rewritten 'start' lands on physical L1 (field: confirm answered on L1)");
  ord = binding_ordinal(out, "back");
  code = ascending_code(h700, NH700, (unsigned)ord);
  CHECK(code == 0x133, "RED: rewritten 'back' lands on physical Y -> SELECT+START chord dead");
  ord = binding_ordinal(out, "leftstick");
  code = ascending_code(h700, NH700, (unsigned)ord);
  CHECK(code == 0x136, "RED: physical SELECT now reads as L3");
  ord = binding_ordinal(out, "lefttrigger");
  code = ascending_code(h700, NH700, (unsigned)ord);
  CHECK(code == 0x137, "RED: physical START now reads as L2");
}

int main(void) {
  run_case("incident-fp2-1.1.3-muos-rg40xx-h (retro line)",
           "19000000010000000100000000010000,muOS-Keys,a:b3,b:b4,x:b6,y:b5,leftshoulder:b7,rightshoulder:b8,lefttrigger:b13,righttrigger:b14,guide:b11,start:b10,back:b9,dpup:h0.1,dpleft:h0.8,dpright:h0.2,dpdown:h0.4,volumedown:b1,volumeup:b2,leftx:a0,lefty:a1,leftstick:b12,rightx:a2,righty:a3,rightstick:b15,platform:Linux,",
           15u, 2u, "a", 0x130, "start");
  run_case("incident-blossomtales-1.4.0-knulli-rg-cubexx",
           "19000000010000000100000000010000,ANBERNIC-keys,b:b3,a:b4,dpdown:h0.4,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b13,leftstick:b12,dpleft:h0.8,rightshoulder:b8,leftshoulder:b7,righttrigger:b14,rightstick:b15,dpright:h0.2,back:b9,start:b10,dpup:h0.1,y:b6,x:b5,guide:b11,platform:Linux,",
           13u, 0u, "b", 0x130, "start");
  /* Blossom: PortMaster line binds b:b3 -> physical south (0x130); 'a' is
   * b4 -> 0x131. The V4 rewrite sends 'a' to b1 -> 0x72 VOLUMEDOWN. */
  /* Third incident: Nameless Cat 1.2.7 / muOS, env source with TWO
   * byte-identical lines (matching=2). Hats/axes keep numbering (movement
   * survives), faces/back/start shift: the asymmetric field state. */
  {
    unsigned long bits[NLONG]; unsigned i; char out[8192]; nxinput_pm_source_evidence ev; int r;
    const char *two = "19000000010000000100000000010000,Deeplay-keys,a:b3,b:b4,x:b6,y:b5,leftshoulder:b7,rightshoulder:b8,lefttrigger:b13,righttrigger:b14,guide:b11,start:b10,back:b9,dpup:h0.1,dpleft:h0.8,dpright:h0.2,dpdown:h0.4,volumedown:b1,volumeup:b2,leftx:a0,lefty:a1,leftstick:b12,rightx:a2,righty:a3,rightstick:b15,platform:Linux,\n19000000010000000100000000010000,muOS-Keys,a:b3,b:b4,x:b6,y:b5,leftshoulder:b7,rightshoulder:b8,lefttrigger:b13,righttrigger:b14,guide:b11,start:b10,back:b9,dpup:h0.1,dpleft:h0.8,dpright:h0.2,dpdown:h0.4,volumedown:b1,volumeup:b2,leftx:a0,lefty:a1,leftstick:b12,rightx:a2,righty:a3,rightstick:b15,platform:Linux,\n";
    memset(bits, 0, sizeof bits); for (i = 0; i < NH700; i++) setb(bits, h700[i]);
    printf("== namelesscat-1.2.7-rg40xxh-muos-20260902 (matching=2)\n");
    r = nxinput_pm_normalize_source(two, bits, NXINPUT_GODOT_KEY_BITS, nxinput_sdl_api_domain(NXINPUT_SDL_API_2), "19000000010000000100000000010000", out, sizeof out, &ev);
    CHECK(r == NXINPUT_PM_REWRITTEN && ev.matching_lines == 2 && ev.rewritten_lines == 2, "V4 rewrites both duplicate lines (matching=2)");
    CHECK(strstr(out, "Deeplay-keys,a:b0,b:b1,x:b3,y:b2,") && strstr(out, "start:b7,back:b6"), "post-rewrite bytes equal the field controller: readback (a:b0,b:b1,x:b3,y:b2 ... back:b6,start:b7)");
    CHECK(strstr(out, "dpup:h0.1") && strstr(out, "leftx:a0,lefty:a1"), "hat and axes untouched: movement survives (the field asymmetry)");
    CHECK(ascending_code(h700, NH700, (unsigned)binding_ordinal(out, "a")) < 0x130 && ascending_code(h700, NH700, (unsigned)binding_ordinal(out, "start")) == 0x134, "RED: faces dead, START on physical L1");
  }
  printf(fails ? "v5-incident-red: FAIL (fixture did not reproduce)\n" : "v5-incident-red: REPRODUCED (V4 corrupts the three incidents)\n");
  return fails ? 1 : 0;
}
