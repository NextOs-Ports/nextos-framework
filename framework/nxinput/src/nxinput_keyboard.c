/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_keyboard -- see include/nxinput_keyboard.h. Pure. */
#include "nxinput_keyboard.h"
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

static const char *const keysyms[NXINPUT_KEYBOARD_KEYSYM_COUNT] = {"A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z",
  "0","1","2","3","4","5","6","7","8","9","SPACE","ENTER","ESCAPE","TAB","BACKSPACE","UP","DOWN","LEFT","RIGHT","LCTRL","RCTRL","LALT","RALT","LSHIFT","RSHIFT","LGUI","RGUI",
  "F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12","MINUS","EQUALS","COMMA","PERIOD","SLASH","SEMICOLON","APOSTROPHE","LEFTBRACKET","RIGHTBRACKET","BACKSLASH","GRAVE","HOME","END","PAGEUP","PAGEDOWN","INSERT","DELETE","CAPSLOCK","KP_0","KP_1","KP_2","KP_3","KP_4","KP_5","KP_6","KP_7","KP_8","KP_9","KP_ENTER","KP_PLUS","KP_MINUS"};
static const char *const mods[8] = {"LCTRL","RCTRL","LALT","RALT","LSHIFT","RSHIFT","LGUI","RGUI"};

const char *nxinput_keyboard_keysym_name(size_t index) { return index < NXINPUT_KEYBOARD_KEYSYM_COUNT ? keysyms[index] : NULL; }
int nxinput_keyboard_keysym_index(const char *keysym) { size_t i; if (!keysym) return -1; for (i = 0; i < NXINPUT_KEYBOARD_KEYSYM_COUNT; i++) if (!strcmp(keysyms[i], keysym)) return (int)i; return -1; }
static int mod_index(const char *s) { int i; for (i = 0; i < 8; i++) if (!strcmp(mods[i], s)) return i; return -1; }

const char *nxinput_keyboard_keysym_of_evdev(int key_code) {
  static const struct { int code; const char *name; } t[] = {
    {KEY_A,"A"},{KEY_B,"B"},{KEY_C,"C"},{KEY_D,"D"},{KEY_E,"E"},{KEY_F,"F"},{KEY_G,"G"},{KEY_H,"H"},{KEY_I,"I"},{KEY_J,"J"},{KEY_K,"K"},{KEY_L,"L"},{KEY_M,"M"},
    {KEY_N,"N"},{KEY_O,"O"},{KEY_P,"P"},{KEY_Q,"Q"},{KEY_R,"R"},{KEY_S,"S"},{KEY_T,"T"},{KEY_U,"U"},{KEY_V,"V"},{KEY_W,"W"},{KEY_X,"X"},{KEY_Y,"Y"},{KEY_Z,"Z"},
    {KEY_0,"0"},{KEY_1,"1"},{KEY_2,"2"},{KEY_3,"3"},{KEY_4,"4"},{KEY_5,"5"},{KEY_6,"6"},{KEY_7,"7"},{KEY_8,"8"},{KEY_9,"9"},
    {KEY_SPACE,"SPACE"},{KEY_ENTER,"ENTER"},{KEY_ESC,"ESCAPE"},{KEY_TAB,"TAB"},{KEY_BACKSPACE,"BACKSPACE"},{KEY_UP,"UP"},{KEY_DOWN,"DOWN"},{KEY_LEFT,"LEFT"},{KEY_RIGHT,"RIGHT"},
    {KEY_LEFTCTRL,"LCTRL"},{KEY_RIGHTCTRL,"RCTRL"},{KEY_LEFTALT,"LALT"},{KEY_RIGHTALT,"RALT"},{KEY_LEFTSHIFT,"LSHIFT"},{KEY_RIGHTSHIFT,"RSHIFT"},{KEY_LEFTMETA,"LGUI"},{KEY_RIGHTMETA,"RGUI"},
    {KEY_F1,"F1"},{KEY_F2,"F2"},{KEY_F3,"F3"},{KEY_F4,"F4"},{KEY_F5,"F5"},{KEY_F6,"F6"},{KEY_F7,"F7"},{KEY_F8,"F8"},{KEY_F9,"F9"},{KEY_F10,"F10"},{KEY_F11,"F11"},{KEY_F12,"F12"},
    {KEY_MINUS,"MINUS"},{KEY_EQUAL,"EQUALS"},{KEY_COMMA,"COMMA"},{KEY_DOT,"PERIOD"},{KEY_SLASH,"SLASH"},{KEY_SEMICOLON,"SEMICOLON"},{KEY_APOSTROPHE,"APOSTROPHE"},{KEY_LEFTBRACE,"LEFTBRACKET"},{KEY_RIGHTBRACE,"RIGHTBRACKET"},{KEY_BACKSLASH,"BACKSLASH"},{KEY_GRAVE,"GRAVE"},
    {KEY_HOME,"HOME"},{KEY_END,"END"},{KEY_PAGEUP,"PAGEUP"},{KEY_PAGEDOWN,"PAGEDOWN"},{KEY_INSERT,"INSERT"},{KEY_DELETE,"DELETE"},{KEY_CAPSLOCK,"CAPSLOCK"},
    {KEY_KP0,"KP_0"},{KEY_KP1,"KP_1"},{KEY_KP2,"KP_2"},{KEY_KP3,"KP_3"},{KEY_KP4,"KP_4"},{KEY_KP5,"KP_5"},{KEY_KP6,"KP_6"},{KEY_KP7,"KP_7"},{KEY_KP8,"KP_8"},{KEY_KP9,"KP_9"},{KEY_KPENTER,"KP_ENTER"},{KEY_KPPLUS,"KP_PLUS"},{KEY_KPMINUS,"KP_MINUS"}};
  size_t i; for (i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i].code == key_code) return t[i].name; return NULL;
}

uint32_t nxinput_keyboard_action_code(const char *action) {
  uint32_t h = 2166136261u; const unsigned char *p = (const unsigned char *)action;
  while (p && *p) { h ^= *p++; h *= 16777619u; }
  return h ? h : 1u;
}

int nxinput_keyboard_init(nxinput_keyboard *kb, const nxinput_gptk4 *map, nxinput_router *router, uint64_t source_identity) {
  if (!kb || !map || !router || source_identity == router->identity_token) return -1;
  memset(kb, 0, sizeof *kb); kb->map = map; kb->router = router; kb->source_identity = source_identity; kb->next_edge_id = 0x4b00000001ull;
  return 0;
}

static void chord_text(uint8_t mods_mask, const char *keysym, char *out, size_t cap) {
  size_t n = 0; int i; out[0] = '\0';
  for (i = 0; i < 8; i++) if (mods_mask & (1u << i)) n += (size_t)snprintf(out + n, cap > n ? cap - n : 0, "%s+", mods[i]);
  (void)snprintf(out + n, cap > n ? cap - n : 0, "%s", keysym);
}

/* [keyboard.override.<ctx>] first, then [keyboard.base] */
static const nxinput_gptk4_keybind *lookup(const nxinput_keyboard *kb, const char *chord) {
  unsigned i; const nxinput_gptk4_keybind *base = NULL;
  for (i = 0; i < kb->map->keybinds; i++) {
    const nxinput_gptk4_keybind *k = &kb->map->keyboard[i];
    if (strcmp(k->chord, chord) != 0) continue;
    if (k->context[0] == '\0') { if (!base) base = k; }
    else if (kb->context[0] && !strcmp(k->context, kb->context)) return k;
  }
  return base;
}

/* does the action's own @key output name this chord? (loop) */
static int action_key_loops(const nxinput_keyboard *kb, const char *action, const char *chord) {
  int s; unsigned o;
  for (s = 0; s < (int)NXINPUT_GPTK4_SLOT_COUNT; s++) {
    const nxinput_gptk4_binding *b = &kb->map->base.slot[s];
    if (b->kind == NXINPUT_GPTK4_ACTION && !strcmp(b->action, action) && b->key[0] && !strcmp(b->key, chord)) return 1;
    for (o = 0; o < kb->map->overrides; o++) {
      const nxinput_gptk4_binding *ob = &kb->map->override[o].slot[s];
      if (ob->kind == NXINPUT_GPTK4_ACTION && !strcmp(ob->action, action) && ob->key[0] && !strcmp(ob->key, chord)) return 1;
    }
  }
  return 0;
}

static int release_held(nxinput_keyboard *kb, nxinput_keyboard_held *h) {
  int n = nxinput_router_release(kb->router, h->edge_id, &h->out, NULL, 0);
  memset(h, 0, sizeof *h); kb->releases++;
  return n;
}

int nxinput_keyboard_event(nxinput_keyboard *kb, const char *keysym, int value) {
  int m; unsigned i;
  if (!kb || !keysym || nxinput_keyboard_keysym_index(keysym) < 0 || value < 0 || value > 2) return -1;
  if (value == 2) { kb->repeats_ignored++; return 0; }
  m = mod_index(keysym);
  if (m >= 0) {
    if (value) kb->mods |= (uint8_t)(1u << m);
    else {
      kb->mods &= (uint8_t)~(1u << m);
      /* a chord whose modifier released no longer holds */
      for (i = 0; i < NXINPUT_KEYBOARD_MAX_HELD; i++) if (kb->held[i].used && (kb->held[i].mods & (1u << m))) release_held(kb, &kb->held[i]);
    }
    return 0;
  }
  if (!value) {
    int n = 0;
    for (i = 0; i < NXINPUT_KEYBOARD_MAX_HELD; i++) if (kb->held[i].used && !strcmp(kb->held[i].keysym, keysym)) n += release_held(kb, &kb->held[i]) >= 0;
    return n ? 1 : 0;
  }
  {
    char chord[NXINPUT_GPTK4_KEY_MAX + 1]; const nxinput_gptk4_keybind *k; nxinput_keyboard_held *slot = NULL;
    for (i = 0; i < NXINPUT_KEYBOARD_MAX_HELD; i++) if (kb->held[i].used && !strcmp(kb->held[i].keysym, keysym)) return 0; /* already held: no double press */
    chord_text(kb->mods, keysym, chord, sizeof chord);
    k = lookup(kb, chord);
    if (!k || k->binding.kind != NXINPUT_GPTK4_ACTION) { kb->refused_unbound++; return 0; }
    if (action_key_loops(kb, k->binding.action, chord)) { kb->refused_loop++; return 0; }
    for (i = 0; i < NXINPUT_KEYBOARD_MAX_HELD; i++) if (!kb->held[i].used) { slot = &kb->held[i]; break; }
    if (!slot) return 0;
    memset(slot, 0, sizeof *slot);
    slot->out.kind = NXINPUT_ROUTE_ENGINE_DIRECT; slot->out.code = nxinput_keyboard_action_code(k->binding.action); slot->out.player = 0;
    slot->edge_id = kb->next_edge_id++; slot->mods = kb->mods;
    snprintf(slot->keysym, sizeof slot->keysym, "%s", keysym); snprintf(slot->chord, sizeof slot->chord, "%s", chord);
    if (nxinput_router_press(kb->router, slot->edge_id, kb->source_identity, &slot->out, NULL, 0) < 0) { kb->refused_router++; memset(slot, 0, sizeof *slot); return 0; }
    slot->used = 1; kb->presses++;
    return 1;
  }
}

int nxinput_keyboard_poll(nxinput_keyboard *kb, const uint8_t *down, size_t count) {
  size_t i; int n = 0;
  if (!kb || !down) return -1;
  if (count > NXINPUT_KEYBOARD_KEYSYM_COUNT) count = NXINPUT_KEYBOARD_KEYSYM_COUNT;
  /* modifiers first, so a chord pressed in the same frame sees them (a key
   * and its modifier arriving together count as the chord, like an event
   * stream that delivers the modifier first) */
  for (i = 0; i < count; i++) if (mod_index(keysyms[i]) >= 0 && (down[i] != 0) != (kb->prev_snapshot[i] != 0) && down[i]) nxinput_keyboard_event(kb, keysyms[i], 1);
  for (i = 0; i < count; i++) if (mod_index(keysyms[i]) < 0 && (down[i] != 0) != (kb->prev_snapshot[i] != 0)) n += nxinput_keyboard_event(kb, keysyms[i], down[i] ? 1 : 0) == 1;
  for (i = 0; i < count; i++) if (mod_index(keysyms[i]) >= 0 && (down[i] != 0) != (kb->prev_snapshot[i] != 0) && !down[i]) nxinput_keyboard_event(kb, keysyms[i], 0);
  for (i = 0; i < count; i++) kb->prev_snapshot[i] = down[i] ? 1u : 0u;
  return n;
}

unsigned nxinput_keyboard_release_all(nxinput_keyboard *kb) {
  unsigned i, n = 0;
  if (!kb) return 0;
  for (i = 0; i < NXINPUT_KEYBOARD_MAX_HELD; i++) if (kb->held[i].used) { release_held(kb, &kb->held[i]); n++; }
  kb->mods = 0; memset(kb->prev_snapshot, 0, sizeof kb->prev_snapshot); kb->released_all++;
  return n;
}

void nxinput_keyboard_set_context(nxinput_keyboard *kb, const char *context) {
  if (!kb) return;
  if (context && !strcmp(kb->context, context)) return;
  nxinput_keyboard_release_all(kb);
  snprintf(kb->context, sizeof kb->context, "%s", context ? context : "");
}
