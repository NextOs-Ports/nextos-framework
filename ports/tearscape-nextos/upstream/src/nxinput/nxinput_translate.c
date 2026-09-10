/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_translate -- see include/nxinput_translate.h. Pure. */
#include "nxinput_translate.h"

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

/* Candidate SOURCE domains a PortMaster/CFW line can be authored in. */
static const nxinput_sdl_domain candidates[] = {
    NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED,
    NXINPUT_SDL_DOMAIN_SDL2_EVDEV,
    NXINPUT_SDL_DOMAIN_SDL2_LEGACY_EVDEV,
};
#define NCAND (sizeof candidates / sizeof candidates[0])

typedef enum sem { SEM_META = 0, SEM_GAMEPAD, SEM_VOL_DOWN, SEM_VOL_UP, SEM_SYSBIND, SEM_DPAD } sem;

static sem semantic_of(const char *key, size_t n) {
  static const char *const gp[] = {"a", "b", "x", "y", "back", "start", "guide",
                                   "leftshoulder", "rightshoulder", "leftstick",
                                   "rightstick", "dpup", "dpdown", "dpleft",
                                   "dpright", "misc1", "paddle1", "paddle2",
                                   "paddle3", "paddle4", "touchpad",
                                   "lefttrigger", "righttrigger", "leftx",
                                   "lefty", "rightx", "righty"};
  size_t i;
  if (n == 10 && memcmp(key, "volumedown", 10) == 0) return SEM_VOL_DOWN;
  if (n == 8 && memcmp(key, "volumeup", 8) == 0) return SEM_VOL_UP;
  /* F4 (revisao 2): the three semantics a CFW really binds to a SYSTEM key
   * (below BTN_MISC) on gpio-keys boards: guide, back and start. They share
   * one class so the SHORT allow-list of is_system_key_bindable() applies to
   * all three -- the 0.11.1 comment already promised this; only `guide` had
   * it. Every other semantic (faces, shoulders, dpad, sticks) keeps the
   * strict button class, which is what discriminates an ascending line from
   * a high-first reading of the same bytes. */
  if (n == 5 && memcmp(key, "guide", 5) == 0) return SEM_SYSBIND;
  if (n == 4 && memcmp(key, "back", 4) == 0) return SEM_SYSBIND;
  if (n == 5 && memcmp(key, "start", 5) == 0) return SEM_SYSBIND;
  /* 0.11.6: a gpio-keys board publishes its D-pad as KEY_UP/DOWN/LEFT/RIGHT
   * (below BTN_MISC); the CFW line binds dpup:bN there. Refusing it made the
   * whole line incoherent in every domain -> the source yielded -> no
   * built-in mapping either -> BLOCK_AUTHORITY: mute where stock worked. */
  if (n == 4 && memcmp(key, "dpup", 4) == 0) return SEM_DPAD;
  if (n == 6 && memcmp(key, "dpdown", 6) == 0) return SEM_DPAD;
  if (n == 6 && memcmp(key, "dpleft", 6) == 0) return SEM_DPAD;
  if (n == 7 && memcmp(key, "dpright", 7) == 0) return SEM_DPAD;
  for (i = 0; i < sizeof gp / sizeof gp[0]; i++) {
    if (strlen(gp[i]) == n && memcmp(gp[i], key, n) == 0) return SEM_GAMEPAD;
  }
  return SEM_META;
}

/* Every EV_KEY in the button class is a legitimate gamepad binding target,
 * INCLUDING BTN_TRIGGER_HAPPY1 (0x2c0): gpio-keys pads (the K36S-class GO-Super:
 * back/start/L3/R3/guide = TRIGGER_HAPPY1..5) bind it. Excluding it made the
 * whole CFW line incoherent in every domain and the source yielded to the
 * provider's built-in database (field regression caught by the FP2 1.1.4
 * pilot on the K36S-class device, 2026-09-03). */
static int is_button_class(int code) {
  return code >= BTN_MISC && code < KEY_MAX;
}
/* 0.11.1 (review finding 7): capabilities prove that a code EXISTS, not the
 * legend on the plastic (mission 5.2). A CFW line may bind a gamepad
 * semantic -- typically `guide`, `back` or `start` -- to a SYSTEM key that a
 * gpio-keys/board driver publishes below BTN_MISC (KEY_MENU 0x8b on the
 * H700 family, KEY_HOMEPAGE, KEY_BACK, KEY_HOME, KEY_ESC, KEY_SELECT,
 * KEY_OK, KEY_POWER). Refusing those made the WHOLE line incoherent in every
 * domain and the source yielded to the built-in database (the K36S incident
 * had the same shape with BTN_TRIGGER_HAPPY). The allow-list is deliberately
 * SHORT: the low range is what discriminates an ascending line from a
 * high-first reading of the same bytes (a volume key read as `a` is still
 * incoherent), so the exclusion proof keeps its teeth. */
static int is_system_key_bindable(int code) {
  switch (code) {
    case KEY_MENU: case KEY_HOMEPAGE: case KEY_HOME: case KEY_BACK:
    case KEY_ESC: case KEY_SELECT: case KEY_OK: case KEY_POWER:
    case KEY_ENTER: case KEY_SPACE:
      return 1;
    default:
      return 0;
  }
}
/* 0.11.6: the four arrow codes a gpio-keys D-pad publishes (KEY_UP 0x67,
 * KEY_LEFT 0x69, KEY_RIGHT 0x6a, KEY_DOWN 0x6c). Only the dpad semantics
 * accept them: a face on an arrow key stays incoherent (exclusion proof). */
static int is_dpad_key(int code) {
  return code == KEY_UP || code == KEY_DOWN || code == KEY_LEFT || code == KEY_RIGHT;
}
static int accepts(sem s, int code) {
  switch (s) {
    case SEM_GAMEPAD: return is_button_class(code);
    case SEM_SYSBIND: return is_button_class(code) || is_system_key_bindable(code);
    case SEM_DPAD: return is_button_class(code) || is_dpad_key(code);
    case SEM_VOL_DOWN: return code == KEY_VOLUMEDOWN;
    case SEM_VOL_UP: return code == KEY_VOLUMEUP;
    default: return code >= 0;
  }
}

/* A cursor over "key:value" fields after guid,name. */
typedef struct field { const char *key; size_t klen; const char *val; size_t vlen; } field;

static const char *skip_guid_name(const char *line) {
  const char *p = strchr(line, ',');
  if (p == NULL) return NULL;
  p = strchr(p + 1, ',');
  return p != NULL ? p + 1 : NULL;
}

static int next_field(const char **cursor, field *f) {
  const char *p = *cursor;
  const char *end, *colon;
  if (p == NULL || *p == '\0') return 0;
  end = strchr(p, ',');
  if (end == NULL) end = p + strlen(p);
  colon = memchr(p, ':', (size_t)(end - p));
  if (colon == NULL) { *cursor = *end ? end + 1 : end; f->key = p; f->klen = 0; f->val = p; f->vlen = 0; return 1; }
  f->key = p; f->klen = (size_t)(colon - p);
  f->val = colon + 1; f->vlen = (size_t)(end - colon - 1);
  *cursor = *end ? end + 1 : end;
  return 1;
}

/* Parse one binding value: [+-]?(b|a|h)N(.M)?(~)?  */
typedef struct binding { char prefix; char kind; unsigned n; unsigned hatmask; int has_hat; char suffix; } binding;

static int parse_binding(const char *v, size_t n, binding *b) {
  size_t i = 0; unsigned num = 0; int digits = 0;
  memset(b, 0, sizeof *b);
  if (n == 0) return 0;
  if (v[i] == '+' || v[i] == '-') { b->prefix = v[i]; i++; }
  if (i >= n || (v[i] != 'b' && v[i] != 'a' && v[i] != 'h')) return 0;
  b->kind = v[i++];
  while (i < n && v[i] >= '0' && v[i] <= '9') { num = num * 10u + (unsigned)(v[i] - '0'); i++; digits++; }
  if (!digits) return 0;
  b->n = num;
  if (b->kind == 'h') {
    unsigned m = 0; int d2 = 0;
    if (i >= n || v[i] != '.') return 0;
    i++;
    while (i < n && v[i] >= '0' && v[i] <= '9') { m = m * 10u + (unsigned)(v[i] - '0'); i++; d2++; }
    if (!d2) return 0;
    b->hatmask = m; b->has_hat = 1;
  }
  if (i < n && v[i] == '~') { b->suffix = '~'; i++; }
  return i == n;
}

typedef struct reading { int coherent; unsigned buttons, axes, hats; unsigned half_axis, inversion, trigger_as_button; } reading;

/* Physical resolution of a whole line under `domain`. */
static void read_line(const char *line, nxinput_sdl_domain domain,
                      const nxinput_godot_caps *caps, reading *r) {
  const char *cur = skip_guid_name(line);
  field f;
  memset(r, 0, sizeof *r);
  r->coherent = cur != NULL;
  while (cur != NULL && next_field(&cur, &f)) {
    binding b; sem s;
    if (f.klen == 0 || (f.klen == 8 && memcmp(f.key, "platform", 8) == 0)) continue;
    s = semantic_of(f.key, f.klen);
    if (!parse_binding(f.val, f.vlen, &b)) { if (s != SEM_META) r->coherent = 0; continue; }
    if (b.kind == 'b') {
      int code = nxinput_sdl_button_code(domain, caps, b.n);
      r->buttons++;
      if (code < 0 || !accepts(s, code)) r->coherent = 0;
      if ((f.klen == 11 && memcmp(f.key, "lefttrigger", 11) == 0) ||
          (f.klen == 12 && memcmp(f.key, "righttrigger", 12) == 0)) r->trigger_as_button++;
    } else if (b.kind == 'a') {
      int code = nxinput_sdl_axis_code(domain, caps, b.n);
      r->axes++;
      if (code < 0) r->coherent = 0;
      if (b.prefix) r->half_axis++;
      if (b.suffix) r->inversion++;
    } else {
      /* hats: numbered by DETECTED hats, and detection is per domain
       * (0.11.1: the pinned p009 patch makes a lone half a hat on the
       * ascending provider; upstream needs the pair). It must EXIST. */
      r->hats++;
      if (!nxinput_sdl_hat_present(domain, caps, b.n)) r->coherent = 0;
    }
  }
}

static int same_codes(const char *line, nxinput_sdl_domain a, nxinput_sdl_domain b,
                      const nxinput_godot_caps *caps) {
  const char *cur = skip_guid_name(line);
  field f;
  while (cur != NULL && next_field(&cur, &f)) {
    binding bd;
    if (f.klen == 0 || !parse_binding(f.val, f.vlen, &bd)) continue;
    if (bd.kind == 'b' && nxinput_sdl_button_code(a, caps, bd.n) != nxinput_sdl_button_code(b, caps, bd.n)) return 0;
    if (bd.kind == 'a' && nxinput_sdl_axis_code(a, caps, bd.n) != nxinput_sdl_axis_code(b, caps, bd.n)) return 0;
  }
  return 1;
}

static int emit(char *out, size_t cap, size_t *len, const char *s, size_t n) {
  if (*len + n >= cap) return 0;
  memcpy(out + *len, s, n);
  *len += n;
  out[*len] = '\0';
  return 1;
}

static nxinput_translate_result rewrite(const char *line, nxinput_sdl_domain from,
                                        nxinput_sdl_domain to, const nxinput_godot_caps *caps,
                                        char *out, size_t cap, unsigned *rewritten) {
  const char *head_end = skip_guid_name(line);
  const char *cur = head_end;
  field f; size_t len = 0;
  *rewritten = 0;
  if (head_end == NULL || !emit(out, cap, &len, line, (size_t)(head_end - line))) return NXINPUT_TRANSLATE_ERROR;
  while (cur != NULL && next_field(&cur, &f)) {
    binding b; char buf[64]; int n;
    const char *fend = f.val + f.vlen;
    if (f.klen != 0 && parse_binding(f.val, f.vlen, &b) && (b.kind == 'b' || b.kind == 'a')) {
      int code = b.kind == 'b' ? nxinput_sdl_button_code(from, caps, b.n) : nxinput_sdl_axis_code(from, caps, b.n);
      int ord = code < 0 ? -1 : (b.kind == 'b' ? nxinput_sdl_button_ordinal(to, caps, (unsigned)code) : nxinput_sdl_axis_ordinal(to, caps, (unsigned)code));
      if (ord < 0) return NXINPUT_TRANSLATE_ERROR;
      n = snprintf(buf, sizeof buf, "%.*s:%s%c%d%s", (int)f.klen, f.key, b.prefix ? (b.prefix == '+' ? "+" : "-") : "", b.kind, ord, b.suffix ? "~" : "");
      if (n < 0 || !emit(out, cap, &len, buf, (size_t)n)) return NXINPUT_TRANSLATE_ERROR;
      if ((unsigned)ord != b.n) (*rewritten)++;
    } else {
      if (!emit(out, cap, &len, f.key, (size_t)(fend - f.key))) return NXINPUT_TRANSLATE_ERROR;
    }
    if (*fend == ',' && !emit(out, cap, &len, ",", 1)) return NXINPUT_TRANSLATE_ERROR;
  }
  return NXINPUT_TRANSLATE_REWRITTEN;
}

nxinput_translate_result nxinput_translate_line(
    const char *line, const unsigned long *key_bits, size_t key_bit_count,
    const unsigned long *abs_bits, size_t abs_bit_count,
    nxinput_sdl_domain provider_domain, const nxinput_source_descriptor *source,
    char *out, size_t cap, nxinput_translate_evidence *evidence) {
  nxinput_godot_caps caps;
  nxinput_translate_evidence ev;
  reading rd[NCAND];
  size_t i, n = strlen(line == NULL ? "" : line);
  nxinput_sdl_domain src = NXINPUT_SDL_DOMAIN_UNDECLARED;
  int coherent = 0;

  memset(&ev, 0, sizeof ev);
  if (evidence) *evidence = ev;
  if (line == NULL || out == NULL || cap == 0 || key_bits == NULL || abs_bits == NULL || n + 1 > cap) return NXINPUT_TRANSLATE_ERROR;
  if (nxinput_godot_caps_init(&caps, key_bits, key_bit_count, abs_bits, abs_bit_count) != 0) return NXINPUT_TRANSLATE_ERROR;
  memcpy(out, line, n + 1);
  ev.provider_domain = (uint8_t)provider_domain;

  /* Provider UNKNOWN: nothing is rewritten, ever. */
  if (nxinput_sdl_domain_plan(provider_domain) == NULL) {
    if (evidence) *evidence = ev;
    return NXINPUT_TRANSLATE_BYTE_INTACT_UNPROVEN;
  }
  for (i = 0; i < NCAND; i++) {
    read_line(line, candidates[i], &caps, &rd[i]);
    if (rd[i].coherent) { coherent++; }
  }
  ev.coherent_domains = (unsigned)coherent;
  {
    reading any; read_line(line, provider_domain, &caps, &any);
    ev.button_bindings = any.buttons; ev.axis_bindings = any.axes; ev.hat_bindings = any.hats;
    ev.half_axis = any.half_axis > 0u; ev.inversion = any.inversion > 0u; ev.trigger_as_button = any.trigger_as_button > 0u;
  }
  /* 1. A declared source domain wins when it reads the line coherently. */
  if (source != NULL && source->provenance != NXINPUT_SOURCE_UNDECLARED &&
      nxinput_sdl_domain_plan((nxinput_sdl_domain)source->domain) != NULL) {
    reading r; read_line(line, (nxinput_sdl_domain)source->domain, &caps, &r);
    if (r.coherent) { src = (nxinput_sdl_domain)source->domain; ev.source_proved_by = 1; }
  }
  /* 2. Otherwise by exclusion: exactly one candidate reads it, or all the
   * coherent candidates resolve to identical codes. */
  if (src == NXINPUT_SDL_DOMAIN_UNDECLARED) {
    if (coherent == 0) { if (evidence) *evidence = ev; return NXINPUT_TRANSLATE_REJECTED; }
    if (coherent == 1) {
      for (i = 0; i < NCAND; i++) if (rd[i].coherent) src = candidates[i];
      ev.source_proved_by = 2;
    } else {
      nxinput_sdl_domain first = NXINPUT_SDL_DOMAIN_UNDECLARED; int identical = 1;
      for (i = 0; i < NCAND; i++) {
        if (!rd[i].coherent) continue;
        if (first == NXINPUT_SDL_DOMAIN_UNDECLARED) first = candidates[i];
        else if (!same_codes(line, first, candidates[i], &caps)) identical = 0;
      }
      if (identical) { src = first; ev.source_proved_by = 3; }
    }
  }
  ev.source_domain = (uint8_t)src;
  if (src == NXINPUT_SDL_DOMAIN_UNDECLARED) { if (evidence) *evidence = ev; return NXINPUT_TRANSLATE_BYTE_INTACT_UNPROVEN; }
  /* 3. Same physical codes in both domains => byte-intact native. */
  if (nxinput_sdl_domains_equal(src, provider_domain) || same_codes(line, src, provider_domain, &caps)) {
    if (evidence) *evidence = ev;
    return NXINPUT_TRANSLATE_BYTE_INTACT_NATIVE;
  }
  /* 4. Proved different: rewrite through the physical codes. */
  {
    unsigned rw = 0;
    nxinput_translate_result r = rewrite(line, src, provider_domain, &caps, out, cap, &rw);
    if (r == NXINPUT_TRANSLATE_ERROR) { memcpy(out, line, n + 1); if (evidence) *evidence = ev; return NXINPUT_TRANSLATE_ERROR; }
    ev.rewritten_bindings = rw;
    if (evidence) *evidence = ev;
    return rw == 0 ? NXINPUT_TRANSLATE_BYTE_INTACT_NATIVE : NXINPUT_TRANSLATE_REWRITTEN;
  }
}

const char *nxinput_translate_result_name(nxinput_translate_result r) {
  switch (r) {
    case NXINPUT_TRANSLATE_BYTE_INTACT_NATIVE: return "byte-intact-native";
    case NXINPUT_TRANSLATE_REWRITTEN: return "rewritten";
    case NXINPUT_TRANSLATE_BYTE_INTACT_UNPROVEN: return "byte-intact-unproven";
    case NXINPUT_TRANSLATE_REJECTED: return "rejected";
    default: return "error";
  }
}
