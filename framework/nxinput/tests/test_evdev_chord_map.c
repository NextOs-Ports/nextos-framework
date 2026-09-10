/* Host test for nxinput_evdev_chord.h v2.
 *
 * Fixtures are REAL device tables:
 *  - Anbernic H700 family (RG40XX-H/RG34XX-SP/RG35XX-*, Knulli AND muOS):
 *    vendor gpio-keys codes from the DTS (MustardOS/internal, knulli-linux
 *    boot_package dts): A=0x130 B=0x131 X=0x132 Y=0x133 L1=0x134 R1=0x135
 *    SELECT=0x136 START=0x137 MENU=0x138 L3=0x139 L2=0x13a R2=0x13b R3=0x13c,
 *    plus low keys (volume 0x72/0x73 and one hat/direct key). Knulli's
 *    es_input.cfg (SDL_JoystickButtonEventCodeById patch) records
 *    a=id3->304, select=id9->310, start=id10->311, l2=id13->314 — which is only
 *    consistent with the Batocera SDL2 patch that walks 0..KEY_MAX ascending.
 *  - RK3326 GO-Super family (ArkOS/dArkOS, vanilla SDL): SELECT/START are
 *    BTN_TRIGGER_HAPPY1/2 and the mapping binds back=b12 start=b13.
 */
#define NXINPUT_EVDEV_CHORD_IMPLEMENTATION
#include "../include/nxinput_evdev_chord.h"

static void setb(unsigned long *b, int c) {
  b[c / (8 * (int)sizeof(unsigned long))] |= 1UL << (c % (8 * (int)sizeof(unsigned long)));
}
static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { puts("FAIL " msg); fails++; } } while (0)

int main(void) {
  unsigned long bits[NX_EVC_NLONGS];
  int c;

  /* ---- H700 (Knulli/muOS) ---- */
  memset(bits, 0, sizeof bits);
  setb(bits, 0x11); setb(bits, 0x72); setb(bits, 0x73); /* 3 low keys */
  for (c = 0x130; c <= 0x13c; ++c) setb(bits, c);
  /* Batocera-ascending SDL (what Knulli/muOS actually run) == es_input.cfg */
  CHECK(nx_evc_code_for_sdl_index_ascending(bits, 3) == 0x130, "h700 asc a=b3->304");
  CHECK(nx_evc_code_for_sdl_index_ascending(bits, 9) == 0x136, "h700 asc back=b9->310 (BTN_TL)");
  CHECK(nx_evc_code_for_sdl_index_ascending(bits, 10) == 0x137, "h700 asc start=b10->311 (BTN_TR)");
  CHECK(nx_evc_code_for_sdl_index_ascending(bits, 13) == 0x13a, "h700 asc l2=b13->314 (BTN_SELECT)");
  CHECK(nx_evc_code_for_sdl_index_ascending(bits, 14) == 0x13b, "h700 asc r2=b14->315 (BTN_START)");
  /* Vanilla-order derivation on the same device is WRONG by 3 (this is the
   * bug that turned SELECT+START into L3+L2 in v1): must never drive the chord. */
  CHECK(nx_evc_code_for_sdl_index(bits, 9) == 0x139, "h700 vanilla b9 -> L3 (documented mismatch)");
  CHECK(nx_evc_code_for_sdl_index(bits, 10) == 0x13a, "h700 vanilla b10 -> L2 (documented mismatch)");
  /* 0.4.2: raw fallback on this device MUST pick the vendor pair 0x136/0x137
   * (real select/start); the literal BTN_SELECT/START here are physical L2/R2
   * and would close the game on L2+R2. */
  {
    nx_evc_pad pad; memset(&pad, 0, sizeof pad);
    memcpy(pad.keybits, bits, sizeof bits);
    nx_evc_apply_raw_fallback(&pad);
    CHECK(pad.code_select == 0x136 && pad.code_start == 0x137 &&
          strcmp(pad.source, "raw-gpio-base-pair") == 0, "h700 raw fallback = vendor select/start pair");
  }

  /* ---- RK3326 GO-Super (vanilla SDL) ---- */
  memset(bits, 0, sizeof bits);
  for (c = 0x130; c <= 0x13b; ++c) setb(bits, c); /* 12 face/shoulder codes */
  setb(bits, 0x2c0); setb(bits, 0x2c1); setb(bits, 0x2c2); setb(bits, 0x2c3);
  CHECK(nx_evc_code_for_sdl_index(bits, 12) == 0x2c0, "gosuper vanilla back=b12->TRIGGER_HAPPY1");
  CHECK(nx_evc_code_for_sdl_index(bits, 13) == 0x2c1, "gosuper vanilla start=b13->TRIGGER_HAPPY2");
  {
    nx_evc_pad pad; memset(&pad, 0, sizeof pad);
    memcpy(pad.keybits, bits, sizeof bits);
    nx_evc_apply_raw_fallback(&pad);
    CHECK(pad.code_select == NX_EVC_BTN_TRIGGER_HAPPY1 && strcmp(pad.source, "raw-trigger-happy") == 0,
          "gosuper raw fallback = trigger-happy");
  }

  /* ---- 0.4.2: H700 no fallback CRU (mapping bindless + evdev ativo) ----
   * A tabela gpio-keys tem os DOIS pares: 0x136/0x137 (select/start reais) e
   * 0x13a/0x13b (L2/R2). O fallback tem que escolher 0x136/0x137 — escolher
   * os codigos "oficiais" fecharia o jogo com L2+R2. */
  memset(bits, 0, sizeof bits);
  for (c = 0x130; c <= 0x13c; ++c) setb(bits, c);
  setb(bits, 0x72); setb(bits, 0x73);
  {
    nx_evc_pad pad; memset(&pad, 0, sizeof pad);
    memcpy(pad.keybits, bits, sizeof bits);
    nx_evc_apply_raw_fallback(&pad);
    CHECK(pad.code_select == 0x136 && pad.code_start == 0x137 &&
          strcmp(pad.source, "raw-gpio-base-pair") == 0,
          "h700 raw fallback picks 0x136/0x137, never L2/R2");
  }
  /* pad que SO tem os codigos oficiais continua raw-select-start */
  memset(bits, 0, sizeof bits);
  setb(bits, 0x130); setb(bits, 0x13a); setb(bits, 0x13b);
  {
    nx_evc_pad pad; memset(&pad, 0, sizeof pad);
    memcpy(pad.keybits, bits, sizeof bits);
    nx_evc_apply_raw_fallback(&pad);
    CHECK(pad.code_select == NX_EVC_BTN_SELECT && strcmp(pad.source, "raw-select-start") == 0,
          "official BTN_SELECT/START pair still honoured when alone");
  }

  /* ---- ordering primitives ---- */
  memset(bits, 0, sizeof bits);
  setb(bits, 0x100); setb(bits, 0x130);
  CHECK(nx_evc_code_for_sdl_index(bits, 0) == 0x130 && nx_evc_code_for_sdl_index(bits, 1) == 0x100, "vanilla order");
  CHECK(nx_evc_code_for_sdl_index_ascending(bits, 0) == 0x100 && nx_evc_code_for_sdl_index_ascending(bits, 1) == 0x130, "ascending order");
  CHECK(nx_evc_code_for_sdl_index(bits, 2) == -1 && nx_evc_code_for_sdl_index_ascending(bits, 2) == -1, "oob");

  /* ---- policy state machine (no fds needed) ----
   * Simulate one evdev pad with both raw keys down: fires while unbound,
   * muted while an SDL pad is bound. */
  {
    g_nx_evc_count = 1;
    memset(&g_nx_evc_pads[0], 0, sizeof g_nx_evc_pads[0]);
    g_nx_evc_pads[0].fd = -1; /* read() fails: state stays as we set it */
    g_nx_evc_pads[0].code_select = NX_EVC_BTN_SELECT;
    g_nx_evc_pads[0].code_start = NX_EVC_BTN_START;
    g_nx_evc_pads[0].source = "raw-select-start";
    g_nx_evc_sdl_bound = 0; g_nx_evc_fired = 0;
    g_nx_evc_pads[0].down_select = 1; g_nx_evc_pads[0].down_start = 1;
    CHECK(nx_evdev_chord_poll() == 1, "evdev fallback fires when unbound");
    CHECK(nx_evdev_chord_poll() == 0, "evdev fallback is edge-triggered");
    g_nx_evc_sdl_bound = 1; g_nx_evc_fired = 0;
    g_nx_evc_pads[0].down_select = 1; g_nx_evc_pads[0].down_start = 1;
    CHECK(nx_evdev_chord_poll() == 0, "evdev fallback muted while SDL bound (no L2+R2 on H700)");
    /* SDL3 has no SDL_GameController pointer. Its wrapper uses the neutral
     * authority hand-off after proving logical BACK+START bindings. */
    nx_evdev_chord_set_primary_active(0);
    CHECK(g_nx_evc_sdl_bound == 0, "neutral wrapper enables evdev fallback");
    nx_evdev_chord_set_primary_active(1);
    CHECK(g_nx_evc_sdl_bound == 1, "neutral wrapper mutes evdev fallback");
    /* SDL primary with no pads never fires and keeps hold at 0 */
    CHECK(nx_exit_chord_poll_sdl(NULL, 0) == 0 && g_nx_evc_sdl_hold == 0, "sdl poll without pads");
    g_nx_evc_count = -1;
  }

  /* ---- 0.4.1 field case (Magic Rampage v1.1.5 / dArkOSRE via ES) ----
   * A REAL SDL pad whose mapping lacks back/start must NOT mute the evdev
   * fallback: the SDL path can never fire, so muting kills SELECT+START.
   * Virtual joysticks reproduce both mappings exactly. */
  if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) == 0) {
    int di;
    SDL_GameController *pad;
    g_nx_evc_count = 0; /* no evdev fds: only the mute decision is under test */

    /* mapping crc:3ebb do campo: 12 botoes, SEM back/start */
    di = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 4, 12, 0);
    CHECK(di >= 0, "virtual joystick (bindless) attaches");
    if (di >= 0) {
      char guid[64]; char mapping[512];
      SDL_JoystickGetGUIDString(
          SDL_JoystickGetDeviceGUID(di), guid, sizeof guid);
      snprintf(mapping, sizeof mapping,
               "%s,Field Pad,a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,"
               "rightshoulder:b5,dpup:b8,dpdown:b9,dpleft:b10,dpright:b11,"
               "leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux",
               guid);
      SDL_GameControllerAddMapping(mapping);
      pad = SDL_GameControllerOpen(di);
      CHECK(pad != NULL, "bindless pad opens as GameController");
      if (pad) {
        CHECK(nx_evc_sdl_can_chord(pad) == 0,
              "mapping without back/start cannot chord via SDL");
        g_nx_evc_sdl_bound = 1; /* estado herdado de um pad anterior valido */
        nx_evdev_chord_bind_sdl(pad);
        CHECK(g_nx_evc_sdl_bound == 0,
              "0.4.1: bindless SDL pad keeps the evdev fallback ACTIVE");
        SDL_GameControllerClose(pad);
      }
      SDL_JoystickDetachVirtual(di);
    }

    /* mapping completo COM back/start -> SDL assume e o evdev muta */
    di = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 4, 14, 0);
    CHECK(di >= 0, "virtual joystick (full) attaches");
    if (di >= 0) {
      char guid[64]; char mapping[512];
      SDL_JoystickGetGUIDString(
          SDL_JoystickGetDeviceGUID(di), guid, sizeof guid);
      snprintf(mapping, sizeof mapping,
               "%s,Full Pad,a:b0,b:b1,x:b2,y:b3,back:b12,start:b13,"
               "leftx:a0,lefty:a1,platform:Linux", guid);
      SDL_GameControllerAddMapping(mapping);
      pad = SDL_GameControllerOpen(di);
      CHECK(pad != NULL, "full pad opens as GameController");
      if (pad) {
        CHECK(nx_evc_sdl_can_chord(pad) == 1,
              "mapping with back/start chords via SDL");
        g_nx_evc_sdl_bound = 0;
        nx_evdev_chord_bind_sdl(pad);
        CHECK(g_nx_evc_sdl_bound == 1,
              "full SDL pad mutes the evdev fallback");
        SDL_GameControllerClose(pad);
      }
      SDL_JoystickDetachVirtual(di);
    }
    g_nx_evc_count = -1;
    SDL_Quit();
  } else {
    puts("SKIP sdl-virtual (SDL_Init failed on this host)");
  }

  puts(fails ? "evdev-chord: FAIL" : "evdev-chord: OK");
  return fails;
}
