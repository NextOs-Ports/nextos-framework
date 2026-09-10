/* SPDX-License-Identifier: GPL-3.0-only */
/* V5 / B3-B4, C1-C2 -- physical translation source -> provider. */
#include "../../include/nxinput_translate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NL ((NXINPUT_GODOT_KEY_BITS + 63) / 64)
#define AL ((NXINPUT_GODOT_ABS_BITS + 63) / 64)
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static void setb(unsigned long *b, unsigned c) { b[c / 64] |= 1ul << (c % 64); }
static const unsigned h700[] = {0x11, 0x72, 0x73, 0x130, 0x131, 0x132, 0x133, 0x134, 0x135, 0x136, 0x137, 0x138, 0x139, 0x13a, 0x13b, 0x13c};
static const char *muos = "19000000010000000100000000010000,muOS-Keys,a:b3,b:b4,x:b6,y:b5,leftshoulder:b7,rightshoulder:b8,lefttrigger:b13,righttrigger:b14,guide:b11,start:b10,back:b9,dpup:h0.1,dpleft:h0.8,dpright:h0.2,dpdown:h0.4,volumedown:b1,volumeup:b2,leftx:a0,lefty:a1,leftstick:b12,rightx:a2,righty:a3,rightstick:b15,platform:Linux,";
static const char *knulli = "19000000010000000100000000010000,ANBERNIC-keys,b:b3,a:b4,dpdown:h0.4,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b13,leftstick:b12,dpleft:h0.8,rightshoulder:b8,leftshoulder:b7,righttrigger:b14,rightstick:b15,dpright:h0.2,back:b9,start:b10,dpup:h0.1,y:b6,x:b5,guide:b11,platform:Linux,";
static int ordinal_of(const char *m, const char *k) { char p[48]; const char *s; snprintf(p, sizeof p, ",%s:b", k); s = strstr(m, p); return s ? atoi(s + strlen(p)) : -1; }

int main(void) {
  unsigned long kb[NL], ab[AL]; char out[2048]; nxinput_translate_evidence ev; nxinput_translate_result r; unsigned i;
  nxinput_godot_caps caps;
  memset(kb, 0, sizeof kb); memset(ab, 0, sizeof ab);
  for (i = 0; i < sizeof h700 / sizeof h700[0]; i++) setb(kb, h700[i]);
  setb(ab, 0); setb(ab, 1); setb(ab, 2); setb(ab, 3); setb(ab, 0x10); setb(ab, 0x11);
  nxinput_godot_caps_init(&caps, kb, NXINPUT_GODOT_KEY_BITS, ab, NXINPUT_GODOT_ABS_BITS);

  /* Incident 1: muOS line on the ascending provider => byte-intact native. */
  r = nxinput_translate_line(muos, kb, NXINPUT_GODOT_KEY_BITS, ab, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, NULL, out, sizeof out, &ev);
  CHECK(r == NXINPUT_TRANSLATE_BYTE_INTACT_NATIVE && strcmp(out, muos) == 0, "muOS line on ascending provider: byte-intact native (GREEN for FP2/muOS)");
  CHECK(ev.source_domain == NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED && ev.source_proved_by == 2, "source proved ascending by exclusion");
  /* Incident 2: Knulli line on ascending provider => byte-intact. */
  r = nxinput_translate_line(knulli, kb, NXINPUT_GODOT_KEY_BITS, ab, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, NULL, out, sizeof out, &ev);
  CHECK(r == NXINPUT_TRANSLATE_BYTE_INTACT_NATIVE && strcmp(out, knulli) == 0, "Knulli line on ascending provider: byte-intact (GREEN for Blossom/Knulli)");
  /* Same line on the upstream provider (dArkOS): rewritten, physical codes preserved. */
  r = nxinput_translate_line(muos, kb, NXINPUT_GODOT_KEY_BITS, ab, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_EVDEV, NULL, out, sizeof out, &ev);
  CHECK(r == NXINPUT_TRANSLATE_REWRITTEN && ev.rewritten_bindings == 15, "muOS line on high-first provider: rewritten (15 bindings)");
  CHECK(nxinput_sdl_button_code(NXINPUT_SDL_DOMAIN_SDL2_EVDEV, &caps, (unsigned)ordinal_of(out, "a")) == 0x130, "rewritten 'a' still names EV_KEY 0x130 (physical south)");
  CHECK(nxinput_sdl_button_code(NXINPUT_SDL_DOMAIN_SDL2_EVDEV, &caps, (unsigned)ordinal_of(out, "back")) == 0x136 && nxinput_sdl_button_code(NXINPUT_SDL_DOMAIN_SDL2_EVDEV, &caps, (unsigned)ordinal_of(out, "start")) == 0x137, "rewritten back/start name the physical SELECT/START pair");
  CHECK(nxinput_sdl_button_code(NXINPUT_SDL_DOMAIN_SDL2_EVDEV, &caps, (unsigned)ordinal_of(out, "volumedown")) == 0x72, "volumedown follows its EV_KEY");
  CHECK(strstr(out, "dpup:h0.1") && strstr(out, "leftx:a0") && strstr(out, "platform:Linux,"), "hats, axes and metadata preserved");
  /* Reverse: a high-first-authored line (dArkOS style) on the ascending provider. */
  {
    char hf[2048]; strcpy(hf, out);
    r = nxinput_translate_line(hf, kb, NXINPUT_GODOT_KEY_BITS, ab, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, NULL, out, sizeof out, &ev);
    CHECK(r == NXINPUT_TRANSLATE_REWRITTEN && strcmp(out, muos) == 0, "round trip high-first -> ascending restores the exact muOS bytes");
  }
  /* Provider UNKNOWN: untouched, unproven. */
  r = nxinput_translate_line(muos, kb, NXINPUT_GODOT_KEY_BITS, ab, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_UNDECLARED, NULL, out, sizeof out, &ev);
  CHECK(r == NXINPUT_TRANSLATE_BYTE_INTACT_UNPROVEN && strcmp(out, muos) == 0, "provider UNKNOWN: byte-intact, unproven, never rewritten");
  /* Declared source domain wins over exclusion. */
  {
    nxinput_source_descriptor sd = {NXINPUT_SOURCE_DECLARED_BY_PRODUCER, NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED};
    r = nxinput_translate_line(muos, kb, NXINPUT_GODOT_KEY_BITS, ab, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_EVDEV, &sd, out, sizeof out, &ev);
    CHECK(r == NXINPUT_TRANSLATE_REWRITTEN && ev.source_proved_by == 1, "declared source descriptor used");
  }
  /* Ambiguous: a pad with ONLY gamepad codes (no low keys) reads the same in
   * every domain -> identical -> byte-intact native on both providers. */
  {
    unsigned long kb2[NL]; memset(kb2, 0, sizeof kb2); for (i = 0x130; i <= 0x13c; i++) setb(kb2, i);
    r = nxinput_translate_line("19000000010000000100000000010000,Plain,a:b0,b:b1,back:b6,start:b7,leftx:a0,lefty:a1,platform:Linux,", kb2, NXINPUT_GODOT_KEY_BITS, ab, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_EVDEV, NULL, out, sizeof out, &ev);
    CHECK(r == NXINPUT_TRANSLATE_BYTE_INTACT_NATIVE && ev.source_proved_by == 3, "no low keys: identical in all domains, byte-intact");
  }
  /* Incoherent everywhere: ordinal beyond the pad -> rejected. */
  r = nxinput_translate_line("19000000010000000100000000010000,Bad,a:b40,platform:Linux,", kb, NXINPUT_GODOT_KEY_BITS, ab, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_EVDEV, NULL, out, sizeof out, &ev);
  CHECK(r == NXINPUT_TRANSLATE_REJECTED, "ordinal beyond capabilities: rejected");
  /* Axis beyond ABS_MISC: high-first numbers ABS 0x28 as an axis, ascending
   * does not; a line authored high-first with that axis cannot land. */
  {
    unsigned long ab2[AL]; memset(ab2, 0, sizeof ab2); setb(ab2, 0); setb(ab2, 1); setb(ab2, 0x28);
    unsigned long kb2[NL]; memset(kb2, 0, sizeof kb2); for (i = 0x130; i <= 0x13c; i++) setb(kb2, i);
    r = nxinput_translate_line("19000000010000000100000000010000,Misc,a:b0,leftx:a0,lefty:a1,rightx:a2,platform:Linux,", kb2, NXINPUT_GODOT_KEY_BITS, ab2, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, NULL, out, sizeof out, &ev);
    CHECK(r == NXINPUT_TRANSLATE_REWRITTEN || r == NXINPUT_TRANSLATE_ERROR || r == NXINPUT_TRANSLATE_BYTE_INTACT_UNPROVEN, "axis past ABS_MISC handled without inventing an ordinal");
    CHECK(strstr(out, "rightx:a2") == NULL || r != NXINPUT_TRANSLATE_REWRITTEN, "a2 (ABS 0x28) is never presented as reachable on the ascending provider");
  }
  /* Signs/inversion preserved. */
  r = nxinput_translate_line("19000000010000000100000000010000,Half,a:b3,lefttrigger:+a4,righttrigger:-a5~,leftx:a0~,platform:Linux,", kb, NXINPUT_GODOT_KEY_BITS, ab, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_EVDEV, NULL, out, sizeof out, &ev);
  CHECK(r != NXINPUT_TRANSLATE_ERROR && (strstr(out, "lefttrigger:+a") != NULL) && strstr(out, "righttrigger:-a") && strstr(out, "leftx:a0~"), "half-axis sign and ~ inversion survive translation");
  /* Field regression (FP2 1.1.4 pilot on the dArkOS K36S, 2026-09-03): the
   * GO-Super gpio-keys pad binds back/start/L3/R3/guide to TRIGGER_HAPPY1..5
   * (0x2c0..0x2c4) and its D-pad to BTN_DPAD_*; the CFW line was byte-intact
   * for V4 and must be byte-intact native on the high-first provider here. */
  {
    static const unsigned k36s[] = {0x130,0x131,0x133,0x134,0x136,0x137,0x138,0x139,0x220,0x221,0x222,0x223,0x2c0,0x2c1,0x2c2,0x2c3,0x2c4};
    static const char *gosuper = "1900bb3e4b4800000011000000010000,GO-Super Gamepad,a:b1,b:b0,back:b12,dpdown:b9,dpleft:b10,dpright:b11,dpup:b8,guide:b16,leftshoulder:b4,leftstick:b14,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b15,righttrigger:b7,rightx:a2,righty:a3,start:b13,x:b2,y:b3,platform:Linux,";
    unsigned long kb2[NL], ab2[AL];
    memset(kb2, 0, sizeof kb2); memset(ab2, 0, sizeof ab2);
    for (i = 0; i < sizeof k36s / sizeof k36s[0]; i++) setb(kb2, k36s[i]);
    setb(ab2, 0); setb(ab2, 1); setb(ab2, 3); setb(ab2, 4);
    r = nxinput_translate_line(gosuper, kb2, NXINPUT_GODOT_KEY_BITS, ab2, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_EVDEV, NULL, out, sizeof out, &ev);
    CHECK(r == NXINPUT_TRANSLATE_BYTE_INTACT_NATIVE && strcmp(out, gosuper) == 0, "MUTANT killed: K36S GO-Super line (back=TRIGGER_HAPPY1, BTN_DPAD d-pad) is byte-intact native on the high-first provider, never rejected");
    CHECK(ev.coherent_domains >= 1 && ev.button_bindings == 17 && ev.axis_bindings == 4, "K36S line: every binding coherent");
  }
  /* 0.11.1 (review finding 7): guide bound to KEY_MENU (0x8b), a system key
   * below BTN_MISC, is a legitimate binding -- the whole line must stay
   * coherent instead of yielding to the built-in database. */
  {
    unsigned long kb2[NXINPUT_GODOT_KEY_BITS / 64 + 1] = {0}, ab2[NXINPUT_GODOT_ABS_BITS / 64 + 1] = {0};
    static const unsigned keys2[] = {0x8b, 0x130, 0x131, 0x133, 0x134, 0x136, 0x137, 0x138, 0x139};
    static const char *menu_guide = "19000000010000000100000000010000,Board,guide:b0,a:b1,b:b2,x:b3,y:b4,leftshoulder:b5,rightshoulder:b6,back:b7,start:b8,leftx:a0,lefty:a1,platform:Linux,";
    nxinput_translate_evidence ev2; char out2[600]; nxinput_translate_result r2; size_t k;
    for (k = 0; k < sizeof keys2 / sizeof keys2[0]; k++) kb2[keys2[k] / 64] |= 1ul << (keys2[k] % 64);
    ab2[0] |= 3ul;
    r2 = nxinput_translate_line(menu_guide, kb2, NXINPUT_GODOT_KEY_BITS, ab2, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, NULL, out2, sizeof out2, &ev2);
    /* 0.11.4 (F4): with back/start also bindable on system keys the low key no
     * longer discriminates start:b8 either, so the line is coherent in BOTH
     * domains: byte-intact (native or unproven), NEVER rejected/rewritten. */
    CHECK((r2 == NXINPUT_TRANSLATE_BYTE_INTACT_NATIVE || r2 == NXINPUT_TRANSLATE_BYTE_INTACT_UNPROVEN) && strcmp(out2, menu_guide) == 0, "MUTANT killed: guide on KEY_MENU rejected -- the ascending line is coherent and byte-intact on the ascending provider");
    /* the same bytes read high-first would put `guide` on BTN_SOUTH and `a` on
     * BTN_EAST -- and the volume-class discrimination is gone here; so the
     * exclusion proof relies on the remaining low key: KEY_MENU as `start`
     * (b8 -> 0x8b under high-first) is NOT in the allow-list for `start`?
     * It is (system key). The line is then coherent in BOTH domains with
     * different codes: on a high-first provider it is UNPROVEN, never
     * rewritten by guess. */
    r2 = nxinput_translate_line(menu_guide, kb2, NXINPUT_GODOT_KEY_BITS, ab2, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_EVDEV, NULL, out2, sizeof out2, &ev2);
    CHECK(r2 == NXINPUT_TRANSLATE_BYTE_INTACT_UNPROVEN || r2 == NXINPUT_TRANSLATE_REWRITTEN, "ambiguous low-key line on a high-first provider: unproven (no guess) or rewritten by a declared source, never rejected");
    { nxinput_source_descriptor sd = {NXINPUT_SOURCE_DECLARED_BY_PRODUCER, NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED};
      r2 = nxinput_translate_line(menu_guide, kb2, NXINPUT_GODOT_KEY_BITS, ab2, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_EVDEV, &sd, out2, sizeof out2, &ev2);
      CHECK(r2 == NXINPUT_TRANSLATE_REWRITTEN && ev2.source_proved_by == 1 && nxinput_sdl_button_code(NXINPUT_SDL_DOMAIN_SDL2_EVDEV, &caps, 0) >= 0, "declared producer (the CFW for its SDL): the same line translates to the high-first provider"); }
  }
  /* 0.11.1 (review finding 6): a LONE ABS_HAT0X is a hat on the ascending
   * provider (p009), an ordinary axis upstream. */
  {
    unsigned long kb3[NXINPUT_GODOT_KEY_BITS / 64 + 1] = {0}, ab3[NXINPUT_GODOT_ABS_BITS / 64 + 1] = {0};
    static const unsigned keys3[] = {0x130, 0x131, 0x133, 0x134, 0x136, 0x137};
    static const char *half = "19000000010000000100000000010000,HalfHat,a:b0,b:b1,x:b2,y:b3,back:b4,start:b5,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,rightx:a2,platform:Linux,";
    nxinput_translate_evidence ev3; char out3[600]; nxinput_translate_result r3; size_t k; nxinput_godot_caps caps3;
    for (k = 0; k < sizeof keys3 / sizeof keys3[0]; k++) kb3[keys3[k] / 64] |= 1ul << (keys3[k] % 64);
    ab3[0] |= 1ul << 0; ab3[0] |= 1ul << 1; ab3[0] |= 1ul << 3; ab3[0] |= 1ul << 0x10; /* X, Y, RX, HAT0X alone */
    nxinput_godot_caps_init(&caps3, kb3, NXINPUT_GODOT_KEY_BITS, ab3, NXINPUT_GODOT_ABS_BITS);
    CHECK(nxinput_sdl_hat_present(NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, &caps3, 0) == 1 && nxinput_sdl_hat_present(NXINPUT_SDL_DOMAIN_SDL2_EVDEV, &caps3, 0) == 0, "lone ABS_HAT0X: hat on the ascending provider (p009), not upstream");
    CHECK(nxinput_sdl_axis_code(NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, &caps3, 2) == 3 && nxinput_sdl_axis_code(NXINPUT_SDL_DOMAIN_SDL2_EVDEV, &caps3, 3) == 0x10, "axis numbering: ascending skips the lone hat code, upstream numbers it as a3");
    r3 = nxinput_translate_line(half, kb3, NXINPUT_GODOT_KEY_BITS, ab3, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, NULL, out3, sizeof out3, &ev3);
    CHECK(r3 == NXINPUT_TRANSLATE_BYTE_INTACT_NATIVE && ev3.hat_bindings == 2, "MUTANT killed: half-hat line rejected on the ascending provider (coherent, byte-intact)");
  }
  /* F4 (revisao 2, 03/09): the 0.11.1 comment promised `guide`, `back` OR
   * `start` on a system key; the code only allowed `guide`. A CFW line that
   * binds back/start to the gpio-keys system codes (KEY_BACK 0x9e,
   * KEY_HOMEPAGE 0xac) was incoherent in EVERY domain -> the whole source
   * yielded to the provider's built-in database. The allow-list is the same
   * SHORT one: only the semantics change. */
  {
    unsigned long kb4[NXINPUT_GODOT_KEY_BITS / 64 + 1] = {0}, ab4[NXINPUT_GODOT_ABS_BITS / 64 + 1] = {0};
    static const unsigned keys4[] = {0x9e, 0xac, 0x130, 0x131, 0x133, 0x134, 0x136, 0x137};
    static const char *sysline = "19000000010000000100000000010000,Board,a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,back:b6,start:b7,leftx:a0,lefty:a1,platform:Linux,";
    nxinput_translate_evidence ev4; char out4[600]; nxinput_translate_result r4; size_t k;
    for (k = 0; k < sizeof keys4 / sizeof keys4[0]; k++) kb4[keys4[k] / 64] |= 1ul << (keys4[k] % 64);
    ab4[0] |= 3ul;
    r4 = nxinput_translate_line(sysline, kb4, NXINPUT_GODOT_KEY_BITS, ab4, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, NULL, out4, sizeof out4, &ev4);
    CHECK(r4 != NXINPUT_TRANSLATE_REJECTED, "MUTANT killed: back/start bound to a system key (KEY_BACK/KEY_HOMEPAGE) rejected -- the whole CFW line fell to the built-in database");
    /* The fixture is authored HIGH-FIRST (a:b0 = BTN_SOUTH there); on the
     * ascending provider every coherent reading agrees on the physical
     * codes, so the line is rewritten by exclusion (never by guess). */
    CHECK(r4 == NXINPUT_TRANSLATE_REWRITTEN && ev4.button_bindings == 8 && ev4.rewritten_bindings == 8 &&
          strcmp(out4, "19000000010000000100000000010000,Board,a:b2,b:b3,x:b4,y:b5,leftshoulder:b6,rightshoulder:b7,back:b0,start:b1,leftx:a0,lefty:a1,platform:Linux,") == 0,
          "back/start on system keys: the high-first line is rewritten to the ascending provider through the physical codes (KEY_BACK->b0, KEY_HOMEPAGE->b1)");
    /* The same pad's line as the ascending CFW writes it: byte-intact native. */
    { static const char *ascline = "19000000010000000100000000010000,Board,back:b0,start:b1,a:b2,b:b3,x:b4,y:b5,leftshoulder:b6,rightshoulder:b7,leftx:a0,lefty:a1,platform:Linux,";
      nxinput_translate_result r6 = nxinput_translate_line(ascline, kb4, NXINPUT_GODOT_KEY_BITS, ab4, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, NULL, out4, sizeof out4, &ev4);
      CHECK(r6 == NXINPUT_TRANSLATE_BYTE_INTACT_NATIVE && strcmp(out4, ascline) == 0, "MUTANT killed: the ascending CFW line with back/start on system keys rejected or rewritten on its own provider"); }
    /* Teeth kept: a system key is bindable only for the three semantics the
     * CFWs really bind there. A FACE button on KEY_HOMEPAGE stays incoherent. */
    { static const char *faceline = "19000000010000000100000000010000,Board,a:b6,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,back:b0,start:b7,leftx:a0,lefty:a1,platform:Linux,";
      nxinput_translate_result r5 = nxinput_translate_line(faceline, kb4, NXINPUT_GODOT_KEY_BITS, ab4, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, NULL, out4, sizeof out4, &ev4);
      CHECK(r5 == NXINPUT_TRANSLATE_REJECTED, "MUTANT killed: allow-list widened to every semantic -- `a` on KEY_BACK must stay incoherent"); }
  }
  /* 0.11.6: a gpio-keys D-pad published as KEY_UP/LEFT/RIGHT/DOWN (0x67,
   * 0x69, 0x6a, 0x6c). The CFW binds dpup:b0.. there; refusing it made the
   * whole line incoherent -> source yielded -> BLOCK_AUTHORITY (mute). */
  {
    unsigned long kb7[NXINPUT_GODOT_KEY_BITS / 64 + 1] = {0}, ab7[NXINPUT_GODOT_ABS_BITS / 64 + 1] = {0};
    static const unsigned keys7[] = {0x67, 0x69, 0x6a, 0x6c, 0x130, 0x131, 0x133, 0x134, 0x136, 0x137, 0x13a, 0x13b};
    static const char *dpadline = "19000000010000000100000000010000,Board,dpup:b0,dpleft:b1,dpright:b2,dpdown:b3,a:b4,b:b5,x:b6,y:b7,leftshoulder:b8,rightshoulder:b9,back:b10,start:b11,leftx:a0,lefty:a1,platform:Linux,";
    static const char *faceline7 = "19000000010000000100000000010000,Board,a:b0,dpleft:b1,dpright:b2,dpdown:b3,dpup:b4,b:b5,x:b6,y:b7,leftshoulder:b8,rightshoulder:b9,back:b10,start:b11,leftx:a0,lefty:a1,platform:Linux,";
    char out7[512]; nxinput_translate_evidence ev7; nxinput_translate_result r7; size_t k;
    for (k = 0; k < sizeof keys7 / sizeof keys7[0]; k++) kb7[keys7[k] / 64] |= 1ul << (keys7[k] % 64);
    ab7[0] |= 3ul;
    r7 = nxinput_translate_line(dpadline, kb7, NXINPUT_GODOT_KEY_BITS, ab7, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, NULL, out7, sizeof out7, &ev7);
    CHECK(r7 != NXINPUT_TRANSLATE_REJECTED && strcmp(out7, dpadline) == 0, "MUTANT killed: D-pad bound to the gpio-keys arrow codes rejected -- the whole CFW line fell to the built-in database (mute)");
    r7 = nxinput_translate_line(faceline7, kb7, NXINPUT_GODOT_KEY_BITS, ab7, NXINPUT_GODOT_ABS_BITS, NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, NULL, out7, sizeof out7, &ev7);
    CHECK(r7 == NXINPUT_TRANSLATE_REJECTED, "MUTANT killed: arrow keys accepted for every semantic -- `a` on KEY_UP must stay incoherent");
  }
  printf(fails ? "v5-translate: FAIL\n" : "v5-translate: OK\n");
  return fails ? 1 : 0;
}
