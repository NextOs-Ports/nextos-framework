/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_gptk4 -- see include/nxinput_gptk4.h. Pure. */
#include "nxinput_gptk4.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- names ---------------------------------------------------------- */
static const char *const slot_names[NXINPUT_GPTK4_SLOT_COUNT] = {
    "A", "B", "X", "Y", "L1", "R1", "L3", "R3", "START", "SELECT", "GUIDE",
    "UP", "DOWN", "LEFT", "RIGHT",
    "stick.left.up", "stick.left.down", "stick.left.left", "stick.left.right",
    "stick.right.up", "stick.right.down", "stick.right.left", "stick.right.right",
    "trigger.left.digital", "trigger.right.digital",
    "stick.left.vector", "stick.right.vector", "trigger.left.analog", "trigger.right.analog"};
const char *nxinput_gptk4_control_name(nxinput_gptk4_control c) { return (unsigned)c < NXINPUT_GPTK4_SLOT_COUNT ? slot_names[c] : "?"; }

static const char *const keysyms[] = {"A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z",
  "0","1","2","3","4","5","6","7","8","9","SPACE","ENTER","ESCAPE","TAB","BACKSPACE","UP","DOWN","LEFT","RIGHT","LCTRL","RCTRL","LALT","RALT","LSHIFT","RSHIFT","LGUI","RGUI",
  "F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12","MINUS","EQUALS","COMMA","PERIOD","SLASH","SEMICOLON","APOSTROPHE","LEFTBRACKET","RIGHTBRACKET","BACKSLASH","GRAVE","HOME","END","PAGEUP","PAGEDOWN","INSERT","DELETE","CAPSLOCK","KP_0","KP_1","KP_2","KP_3","KP_4","KP_5","KP_6","KP_7","KP_8","KP_9","KP_ENTER","KP_PLUS","KP_MINUS"};
static const char *const mods[8] = {"LCTRL","RCTRL","LALT","RALT","LSHIFT","RSHIFT","LGUI","RGUI"};
static int is_keysym(const char *s) { size_t i; for (i = 0; i < sizeof keysyms / sizeof keysyms[0]; i++) if (strcmp(keysyms[i], s) == 0) return 1; return 0; }
static int mod_index(const char *s) { int i; for (i = 0; i < 8; i++) if (strcmp(mods[i], s) == 0) return i; return -1; }

int nxinput_gptk4_canonical_key(const char *in, char *out, size_t cap) {
  char buf[NXINPUT_GPTK4_KEY_MAX + 1]; char *tok = buf; int have[8] = {0}; char sym[32] = ""; size_t n = 0; int i;
  if (in == NULL || strlen(in) > NXINPUT_GPTK4_KEY_MAX || cap == 0) return -1;
  strcpy(buf, in);
  while (tok && *tok) {
    char *plus = strchr(tok, '+'); int m;
    if (plus) *plus = '\0';
    m = mod_index(tok);
    if (m >= 0) { if (have[m]) return -1; have[m] = 1; }
    else { if (!is_keysym(tok) || sym[0] || strlen(tok) >= sizeof sym) return -1; strcpy(sym, tok); }
    tok = plus ? plus + 1 : NULL;
  }
  if (!sym[0]) return -1;
  out[0] = '\0';
  for (i = 0; i < 8; i++) if (have[i]) { n += (size_t)snprintf(out + n, cap - n, "%s+", mods[i]); if (n >= cap) return -1; }
  n += (size_t)snprintf(out + n, cap - n, "%s", sym);
  return n < cap ? 0 : -1;
}

/* ---- lexer ---------------------------------------------------------- */
typedef struct cur { const char *p, *end; unsigned line; } cur;
static void seterr(nxinput_gptk4_error *e, int code, unsigned line, unsigned col, const char *what) { if (e) { e->code = code; e->line = line; e->column = col; snprintf(e->what, sizeof e->what, "%s", what); } }
static void trim(char *s) { size_t n = strlen(s); while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) s[--n] = 0; }

static const nxinput_gptk4_action_decl *find_action(const nxinput_gptk4_contract *c, const char *id) { size_t i; for (i = 0; i < c->count; i++) if (strcmp(c->actions[i].id, id) == 0) return &c->actions[i]; return NULL; }
static int context_declared(const nxinput_gptk4_contract *c, const char *ctx) { size_t i; for (i = 0; i < c->context_count; i++) if (strcmp(c->contexts[i], ctx) == 0) return 1; return 0; }

/* parse "native|null|action:id[@key:chord]" for a slot of value kind vk */
static int parse_binding(const char *v, uint8_t vk, int derived_direction, const nxinput_gptk4_contract *c, nxinput_gptk4_binding *b, unsigned line, unsigned col, nxinput_gptk4_error *e) {
  memset(b, 0, sizeof *b); b->line = (uint16_t)line;
  if (strcmp(v, "native") == 0) {
    if (derived_direction) { seterr(e, NXINPUT_GPTK4_ERR_NATIVE_DERIVED, line, col, "native is invalid on a derived stick direction (use vector=native)"); return -1; }
    b->kind = NXINPUT_GPTK4_NATIVE; return 0;
  }
  if (strcmp(v, "null") == 0) { b->kind = NXINPUT_GPTK4_NULL; return 0; }
  if (strncmp(v, "action:", 7) == 0) {
    const char *id = v + 7; const char *at = strchr(id, '@'); char idbuf[NXINPUT_GPTK4_ACTION_MAX + 1]; size_t idlen = at ? (size_t)(at - id) : strlen(id);
    const nxinput_gptk4_action_decl *d;
    if (idlen == 0 || idlen > NXINPUT_GPTK4_ACTION_MAX) { seterr(e, NXINPUT_GPTK4_ERR_MALFORMED, line, col, "action id length"); return -1; }
    memcpy(idbuf, id, idlen); idbuf[idlen] = 0;
    { size_t i; for (i = 0; i < idlen; i++) if (!(islower((unsigned char)idbuf[i]) || isdigit((unsigned char)idbuf[i]) || idbuf[i] == '.' || idbuf[i] == '_')) { seterr(e, NXINPUT_GPTK4_ERR_MALFORMED, line, col, "action id charset [a-z0-9._]"); return -1; } }
    d = find_action(c, idbuf);
    if (!d) { seterr(e, NXINPUT_GPTK4_ERR_UNKNOWN, line, col, "action not declared by the adapter contract"); return -1; }
    if (d->value_kind != vk) { seterr(e, NXINPUT_GPTK4_ERR_KIND, line, col, "binding kind does not match the action value_kind"); return -1; }
    b->kind = NXINPUT_GPTK4_ACTION; strcpy(b->action, idbuf);
    if (at) {
      if (vk != NXINPUT_GPTK4_V_DIGITAL) { seterr(e, NXINPUT_GPTK4_ERR_KIND, line, col, "@key only on a digital action"); return -1; }
      if (strncmp(at, "@key:", 5) != 0) { seterr(e, NXINPUT_GPTK4_ERR_MALFORMED, line, col, "expected @key:<chord>"); return -1; }
      if (!c->keyboard_backend) { seterr(e, NXINPUT_GPTK4_ERR_KEYBOARD, line, col, "@key without a proved keyboard backend"); return -1; }
      if (nxinput_gptk4_canonical_key(at + 5, b->key, sizeof b->key) != 0) { seterr(e, NXINPUT_GPTK4_ERR_KEYBOARD, line, col, "key chord outside the allowlist"); return -1; }
    }
    return 0;
  }
  if (strncmp(v, "key:", 4) == 0) { seterr(e, NXINPUT_GPTK4_ERR_MALFORMED, line, col, "bare key: output is invalid in schema 4 (use action:<id>@key:)"); return -1; }
  seterr(e, NXINPUT_GPTK4_ERR_MALFORMED, line, col, "expected native | null | action:<id>[@key:<chord>]"); return -1;
}

static int slot_of_discrete(const char *k) { int i; for (i = 0; i <= NXINPUT_GPTK4_RIGHT; i++) if (strcmp(slot_names[i], k) == 0) return i; return -1; }
static int slot_of_any(const char *k) { int i; for (i = 0; i < NXINPUT_GPTK4_SLOT_COUNT; i++) if (strcmp(slot_names[i], k) == 0) return i; return -1; }
static uint8_t vk_of_slot(int s) { return s == NXINPUT_GPTK4_LS_VECTOR || s == NXINPUT_GPTK4_RS_VECTOR ? NXINPUT_GPTK4_V_VECTOR2 : (s == NXINPUT_GPTK4_L2_ANALOG || s == NXINPUT_GPTK4_R2_ANALOG) ? NXINPUT_GPTK4_V_SCALAR : NXINPUT_GPTK4_V_DIGITAL; }
static int is_derived(int s) { return s >= NXINPUT_GPTK4_LS_UP && s <= NXINPUT_GPTK4_RS_RIGHT; }
static int parse_float(const char *v, float *f) { char *end; double d = strtod(v, &end); if (end == v || *end) return -1; if (d != d) return -1; *f = (float)d; return 0; }

static uint64_t fnv(const char *s, size_t n) { uint64_t h = 1469598103934665603ull; size_t i; for (i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ull; } return h; }

int nxinput_gptk4_parse(const char *text, size_t len, const nxinput_gptk4_contract *c, nxinput_gptk4 *g, nxinput_gptk4_error *e) {
  cur cu; char line[512]; enum { S_HEAD, S_BASE, S_OVERRIDE, S_STICK_L, S_STICK_R, S_TRIG_L, S_TRIG_R, S_KEYBOARD, S_KB_OVERRIDE } sec = S_HEAD;
  nxinput_gptk4_layer *layer = NULL; char kbctx[24] = "";
  int seen_stick[2][2] = {{0}}, seen_trig[2] = {0}; /* mode seen, vector/analog seen */
  int have_header[5] = {0};
  if (!text || !c || !g) return -1;
  if (len > NXINPUT_GPTK4_MAX_BYTES) { seterr(e, NXINPUT_GPTK4_ERR_TOO_LARGE, 0, 0, "file too large"); return -1; }
  memset(g, 0, sizeof *g);
  g->left.enter = g->right.enter = 0.55f; g->left.exit = g->right.exit = 0.40f; g->left.eight_way = g->right.eight_way = 1; g->left.tie_horizontal = g->right.tie_horizontal = 1;
  g->l2.enter = g->r2.enter = 0.55f; g->l2.exit = g->r2.exit = 0.40f;
  cu.p = text; cu.end = text + len; cu.line = 0;
  { /* the magic is the FIRST non-blank, non-comment line (the generated
     * default carries a bilingual comment header before it, as V1-V3 did) */
    const char *q = text; const char *end = text + len; unsigned ln = 1; int found = 0;
    while (q < end) {
      const char *nl = memchr(q, '\n', (size_t)(end - q)); size_t n = nl ? (size_t)(nl - q) : (size_t)(end - q);
      size_t k = 0; while (k < n && (q[k] == ' ' || q[k] == '\t' || q[k] == '\r')) k++;
      if (k < n && q[k] != '#') {
        if (n - k >= strlen(NXINPUT_GPTK4_MAGIC) && strncmp(q + k, NXINPUT_GPTK4_MAGIC, strlen(NXINPUT_GPTK4_MAGIC)) == 0) found = 1;
        else { seterr(e, NXINPUT_GPTK4_ERR_MAGIC, ln, (unsigned)k + 1, "first line must be `format = NEXTOS_CONTROLLERS/4`"); return -1; }
        break;
      }
      q = nl ? nl + 1 : end; ln++;
    }
    if (!found) { seterr(e, NXINPUT_GPTK4_ERR_MAGIC, 1, 1, "first line must be `format = NEXTOS_CONTROLLERS/4`"); return -1; }
  }
  while (cu.p < cu.end) {
    const char *nl = memchr(cu.p, '\n', (size_t)(cu.end - cu.p)); size_t n = nl ? (size_t)(nl - cu.p) : (size_t)(cu.end - cu.p);
    char *hash, *eq, *k, *v; unsigned col;
    cu.line++;
    if (n >= sizeof line) { seterr(e, NXINPUT_GPTK4_ERR_TOO_LARGE, cu.line, 1, "line too long"); return -1; }
    memcpy(line, cu.p, n); line[n] = 0; cu.p = nl ? nl + 1 : cu.end;
    hash = strchr(line, '#'); if (hash) *hash = 0;
    trim(line); k = line; while (*k == ' ' || *k == '\t') k++;
    if (!*k) continue;
    col = (unsigned)(k - line) + 1;
    if (*k == '[') {
      char *close = strchr(k, ']'); if (!close) { seterr(e, NXINPUT_GPTK4_ERR_MALFORMED, cu.line, col, "unterminated section"); return -1; }
      *close = 0; k++;
      if (strcmp(k, "base") == 0) { sec = S_BASE; layer = &g->base; }
      else if (strncmp(k, "override.", 9) == 0) {
        unsigned i; if (!context_declared(c, k + 9)) { seterr(e, NXINPUT_GPTK4_ERR_UNKNOWN, cu.line, col, "override context not declared by the adapter"); return -1; }
        for (i = 0; i < g->overrides; i++) if (strcmp(g->override[i].context, k + 9) == 0) { seterr(e, NXINPUT_GPTK4_ERR_DUPLICATE, cu.line, col, "duplicate override section"); return -1; }
        if (g->overrides >= NXINPUT_GPTK4_MAX_OVERRIDES) { seterr(e, NXINPUT_GPTK4_ERR_TOO_LARGE, cu.line, col, "too many overrides"); return -1; }
        layer = &g->override[g->overrides++]; snprintf(layer->context, sizeof layer->context, "%s", k + 9); sec = S_OVERRIDE;
      }
      else if (strcmp(k, "stick.left") == 0) sec = S_STICK_L; else if (strcmp(k, "stick.right") == 0) sec = S_STICK_R;
      else if (strcmp(k, "trigger.left") == 0) sec = S_TRIG_L; else if (strcmp(k, "trigger.right") == 0) sec = S_TRIG_R;
      else if (strcmp(k, "keyboard.base") == 0) { sec = S_KEYBOARD; kbctx[0] = 0; }
      else if (strncmp(k, "keyboard.override.", 18) == 0) { if (!context_declared(c, k + 18)) { seterr(e, NXINPUT_GPTK4_ERR_UNKNOWN, cu.line, col, "keyboard override context not declared"); return -1; } sec = S_KB_OVERRIDE; snprintf(kbctx, sizeof kbctx, "%s", k + 18); }
      else { seterr(e, NXINPUT_GPTK4_ERR_UNKNOWN, cu.line, col, "unknown section"); return -1; }
      continue;
    }
    eq = strchr(k, '='); if (!eq) { seterr(e, NXINPUT_GPTK4_ERR_MALFORMED, cu.line, col, "expected key = value"); return -1; }
    *eq = 0; v = eq + 1; trim(k); while (*v == ' ' || *v == '\t') v++; trim(v);
    if (!*v) { seterr(e, NXINPUT_GPTK4_ERR_MALFORMED, cu.line, col, "empty value"); return -1; }
    switch (sec) {
      case S_HEAD:
        if (strcmp(k, "format") == 0) { have_header[0] = 1; }
        else if (strcmp(k, "port") == 0) { if (strlen(v) > 64) { seterr(e, NXINPUT_GPTK4_ERR_HEADER, cu.line, col, "port too long"); return -1; } strcpy(g->port, v); have_header[1] = 1; }
        else if (strcmp(k, "CONTROL_STANDARD") == 0) { if (strcmp(v, "xbox") != 0) { seterr(e, NXINPUT_GPTK4_ERR_HEADER, cu.line, col, "CONTROL_STANDARD must be xbox (positional)"); return -1; } have_header[2] = 1; }
        else if (strcmp(k, "GLYPH_STYLE") == 0) { snprintf(g->glyph_style, sizeof g->glyph_style, "%s", v); g->glyph_style_opt_in = strcmp(v, "xbox") != 0; }
        else if (strcmp(k, "AUTHORITY") == 0) { if (!strcmp(v, "nextos")) g->authority = NXINPUT_GPTK4_AUTH_NEXTOS; else if (!strcmp(v, "engine")) g->authority = NXINPUT_GPTK4_AUTH_ENGINE; else if (!strcmp(v, "synchronized")) g->authority = NXINPUT_GPTK4_AUTH_SYNCHRONIZED; else { seterr(e, NXINPUT_GPTK4_ERR_HEADER, cu.line, col, "AUTHORITY nextos|engine|synchronized"); return -1; } have_header[3] = 1; }
        else if (strcmp(k, "CONTEXT_POLICY") == 0) { if (strcmp(v, "unified") != 0) { seterr(e, NXINPUT_GPTK4_ERR_HEADER, cu.line, col, "CONTEXT_POLICY must be unified"); return -1; } have_header[4] = 1; }
        else { seterr(e, NXINPUT_GPTK4_ERR_UNKNOWN, cu.line, col, "unknown header key"); return -1; }
        break;
      case S_BASE: case S_OVERRIDE: {
        int s = sec == S_BASE ? slot_of_discrete(k) : slot_of_any(k);
        if (s < 0 && sec == S_BASE && strncmp(k, "EXT.", 4) == 0) {
          size_t i; int decl = -1;
          if (!nxinput_gptk4_ext_name_valid(k)) { seterr(e, NXINPUT_GPTK4_ERR_UNKNOWN, cu.line, col, "extension name must be EXT.[A-Z0-9_]+"); return -1; }
          for (i = 0; c && c->extensions && i < c->extension_count; i++) if (strcmp(c->extensions[i], k) == 0) decl = (int)i;
          if (decl < 0) { seterr(e, NXINPUT_GPTK4_ERR_UNKNOWN, cu.line, col, "extension not declared by the adapter capability descriptor"); return -1; }
          for (i = 0; i < g->exts; i++) if (strcmp(g->ext[i].name, k) == 0) { seterr(e, NXINPUT_GPTK4_ERR_DUPLICATE, cu.line, col, "extension declared twice"); return -1; }
          if (g->exts >= NXINPUT_GPTK4_MAX_EXT) { seterr(e, NXINPUT_GPTK4_ERR_TOO_LARGE, cu.line, col, "too many extensions"); return -1; }
          memcpy(g->ext[g->exts].name, k, strlen(k) + 1u); /* length already bounded by ext_name_valid */
          if (parse_binding(v, NXINPUT_GPTK4_V_DIGITAL, 0, c, &g->ext[g->exts].binding, cu.line, col, e)) return -1;
          g->exts++;
          break;
        }
        if (s < 0) { seterr(e, NXINPUT_GPTK4_ERR_UNKNOWN, cu.line, col, sec == S_BASE ? "[base] owns only the discrete controls" : "unknown control in override"); return -1; }
        if (layer->slot[s].kind) { seterr(e, NXINPUT_GPTK4_ERR_DUPLICATE, cu.line, col, "control declared twice"); return -1; }
        if (parse_binding(v, vk_of_slot(s), is_derived(s), c, &layer->slot[s], cu.line, col, e)) return -1;
        break; }
      case S_STICK_L: case S_STICK_R: {
        int which = sec == S_STICK_R; nxinput_gptk4_stick *st = which ? &g->right : &g->left; int base = which ? NXINPUT_GPTK4_RS_UP : NXINPUT_GPTK4_LS_UP; int vec = which ? NXINPUT_GPTK4_RS_VECTOR : NXINPUT_GPTK4_LS_VECTOR;
        if (!strcmp(k, "mode")) { if (!strcmp(v, "vector")) st->mode = NXINPUT_GPTK4_STICK_VECTOR; else if (!strcmp(v, "digital")) st->mode = NXINPUT_GPTK4_STICK_DIGITAL; else if (!strcmp(v, "split")) st->mode = NXINPUT_GPTK4_STICK_SPLIT; else { seterr(e, NXINPUT_GPTK4_ERR_MODE, cu.line, col, "mode vector|digital|split"); return -1; } seen_stick[which][0] = 1; }
        else if (!strcmp(k, "vector")) { if (g->base.slot[vec].kind) { seterr(e, NXINPUT_GPTK4_ERR_DUPLICATE, cu.line, col, "vector twice"); return -1; } if (parse_binding(v, NXINPUT_GPTK4_V_VECTOR2, 0, c, &g->base.slot[vec], cu.line, col, e)) return -1; seen_stick[which][1] = 1; }
        else if (!strcmp(k, "up") || !strcmp(k, "down") || !strcmp(k, "left") || !strcmp(k, "right")) { int d = !strcmp(k, "up") ? 0 : !strcmp(k, "down") ? 1 : !strcmp(k, "left") ? 2 : 3; if (g->base.slot[base + d].kind) { seterr(e, NXINPUT_GPTK4_ERR_DUPLICATE, cu.line, col, "direction twice"); return -1; } if (parse_binding(v, NXINPUT_GPTK4_V_DIGITAL, 1, c, &g->base.slot[base + d], cu.line, col, e)) return -1; }
        else if (!strcmp(k, "enter_threshold")) { if (parse_float(v, &st->enter)) goto badf; }
        else if (!strcmp(k, "exit_threshold")) { if (parse_float(v, &st->exit)) goto badf; }
        else if (!strcmp(k, "direction_enter_threshold")) { if (parse_float(v, &st->dir_enter)) goto badf; }
        else if (!strcmp(k, "direction_exit_threshold")) { if (parse_float(v, &st->dir_exit)) goto badf; }
        else if (!strcmp(k, "digital_enter_threshold")) { if (parse_float(v, &st->dig_enter)) goto badf; }
        else if (!strcmp(k, "digital_exit_threshold")) { if (parse_float(v, &st->dig_exit)) goto badf; }
        else if (!strcmp(k, "diagonal")) { if (!strcmp(v, "8way")) st->eight_way = 1; else if (!strcmp(v, "4way-dominant")) st->eight_way = 0; else { seterr(e, NXINPUT_GPTK4_ERR_MODE, cu.line, col, "diagonal 8way|4way-dominant"); return -1; } }
        else if (!strcmp(k, "tie_break")) { if (!strcmp(v, "horizontal")) st->tie_horizontal = 1; else if (!strcmp(v, "vertical")) st->tie_horizontal = 0; else { seterr(e, NXINPUT_GPTK4_ERR_MODE, cu.line, col, "tie_break horizontal|vertical"); return -1; } }
        else if (!strcmp(k, "split_policy")) { if (strcmp(v, "zones")) { seterr(e, NXINPUT_GPTK4_ERR_MODE, cu.line, col, "split_policy must be zones"); return -1; } }
        else { seterr(e, NXINPUT_GPTK4_ERR_UNKNOWN, cu.line, col, "unknown stick key"); return -1; }
        break;
      badf: seterr(e, NXINPUT_GPTK4_ERR_THRESHOLD, cu.line, col, "threshold must be a finite number"); return -1; }
      case S_TRIG_L: case S_TRIG_R: {
        int which = sec == S_TRIG_R; nxinput_gptk4_trigger *t = which ? &g->r2 : &g->l2; int an = which ? NXINPUT_GPTK4_R2_ANALOG : NXINPUT_GPTK4_L2_ANALOG; int dg = which ? NXINPUT_GPTK4_R2_DIGITAL : NXINPUT_GPTK4_L2_DIGITAL;
        if (!strcmp(k, "mode")) { if (!strcmp(v, "analog")) t->mode = NXINPUT_GPTK4_TRIGGER_ANALOG; else if (!strcmp(v, "digital")) t->mode = NXINPUT_GPTK4_TRIGGER_DIGITAL; else { seterr(e, NXINPUT_GPTK4_ERR_MODE, cu.line, col, "mode analog|digital"); return -1; } seen_trig[which] |= 1; }
        else if (!strcmp(k, "analog")) { if (g->base.slot[an].kind) { seterr(e, NXINPUT_GPTK4_ERR_DUPLICATE, cu.line, col, "analog twice"); return -1; } if (parse_binding(v, NXINPUT_GPTK4_V_SCALAR, 0, c, &g->base.slot[an], cu.line, col, e)) return -1; seen_trig[which] |= 2; }
        else if (!strcmp(k, "digital")) { if (g->base.slot[dg].kind) { seterr(e, NXINPUT_GPTK4_ERR_DUPLICATE, cu.line, col, "digital twice"); return -1; } if (parse_binding(v, NXINPUT_GPTK4_V_DIGITAL, 1, c, &g->base.slot[dg], cu.line, col, e)) return -1; seen_trig[which] |= 4; }
        else if (!strcmp(k, "enter_threshold")) { if (parse_float(v, &t->enter)) { seterr(e, NXINPUT_GPTK4_ERR_THRESHOLD, cu.line, col, "threshold"); return -1; } }
        else if (!strcmp(k, "exit_threshold")) { if (parse_float(v, &t->exit)) { seterr(e, NXINPUT_GPTK4_ERR_THRESHOLD, cu.line, col, "threshold"); return -1; } }
        else { seterr(e, NXINPUT_GPTK4_ERR_UNKNOWN, cu.line, col, "unknown trigger key"); return -1; }
        break; }
      case S_KEYBOARD: case S_KB_OVERRIDE: {
        nxinput_gptk4_keybind *kb; unsigned i; char canon[NXINPUT_GPTK4_KEY_MAX + 1];
        if (!c->keyboard_backend) { seterr(e, NXINPUT_GPTK4_ERR_KEYBOARD, cu.line, col, "keyboard source without a keyboard-capable adapter"); return -1; }
        if (nxinput_gptk4_canonical_key(k, canon, sizeof canon)) { seterr(e, NXINPUT_GPTK4_ERR_KEYBOARD, cu.line, col, "key chord outside the allowlist"); return -1; }
        for (i = 0; i < g->keybinds; i++) if (!strcmp(g->keyboard[i].chord, canon) && !strcmp(g->keyboard[i].context, kbctx)) { seterr(e, NXINPUT_GPTK4_ERR_DUPLICATE, cu.line, col, "key bound twice in this context"); return -1; }
        if (g->keybinds >= NXINPUT_GPTK4_MAX_KEYBOARD) { seterr(e, NXINPUT_GPTK4_ERR_TOO_LARGE, cu.line, col, "too many key bindings"); return -1; }
        kb = &g->keyboard[g->keybinds]; memset(kb, 0, sizeof *kb); strcpy(kb->chord, canon); snprintf(kb->context, sizeof kb->context, "%s", kbctx);
        if (parse_binding(v, NXINPUT_GPTK4_V_DIGITAL, 1, c, &kb->binding, cu.line, col, e)) return -1;
        if (kb->binding.kind == NXINPUT_GPTK4_ACTION && kb->binding.key[0]) { seterr(e, NXINPUT_GPTK4_ERR_KEYBOARD, cu.line, col, "physical key -> action -> @key would loop"); return -1; }
        g->keybinds++;
        break; }
    }
  }
  /* completeness + contracts */
  { int i; for (i = 0; i < 5; i++) if (!have_header[i]) { seterr(e, NXINPUT_GPTK4_ERR_HEADER, 1, 1, "header incomplete (format/port/CONTROL_STANDARD/AUTHORITY/CONTEXT_POLICY)"); return -1; } }
  { int s; for (s = 0; s < NXINPUT_GPTK4_SLOT_COUNT; s++) if (!g->base.slot[s].kind) { char what[96]; snprintf(what, sizeof what, "control omitted: %s (omission is an error, never null)", slot_names[s]); seterr(e, NXINPUT_GPTK4_ERR_OMITTED, 0, 0, what); return -1; } }
  /* E4a: every DECLARED extension must be listed (visible), never silently null */
  { size_t i, j; for (i = 0; c && c->extensions && i < c->extension_count; i++) { int found = 0; if (!nxinput_gptk4_ext_name_valid(c->extensions[i])) { seterr(e, NXINPUT_GPTK4_ERR_UNKNOWN, 0, 0, "adapter declared an invalid extension name"); return -1; } for (j = 0; j < g->exts; j++) if (strcmp(g->ext[j].name, c->extensions[i]) == 0) found = 1; if (!found) { char what[96]; snprintf(what, sizeof what, "extension omitted: %s (declared by the adapter; omission is an error, never null)", c->extensions[i]); seterr(e, NXINPUT_GPTK4_ERR_OMITTED, 0, 0, what); return -1; } } }
  { int w; for (w = 0; w < 2; w++) {
      nxinput_gptk4_stick *st = w ? &g->right : &g->left; int base = w ? NXINPUT_GPTK4_RS_UP : NXINPUT_GPTK4_LS_UP; int vec = w ? NXINPUT_GPTK4_RS_VECTOR : NXINPUT_GPTK4_LS_VECTOR; int d, anydir = 0, anyaction = 0, anynative = 0;
      if (!seen_stick[w][0]) { seterr(e, NXINPUT_GPTK4_ERR_OMITTED, 0, 0, w ? "stick.right mode omitted" : "stick.left mode omitted"); return -1; }
      for (d = 0; d < 4; d++) { if (g->base.slot[base + d].kind != NXINPUT_GPTK4_NULL) anydir = 1; if (g->base.slot[base + d].kind == NXINPUT_GPTK4_ACTION) anyaction = 1; }
      if (g->base.slot[vec].kind == NXINPUT_GPTK4_NATIVE) anynative = 1;
      if (st->mode == NXINPUT_GPTK4_STICK_VECTOR && anydir) { seterr(e, NXINPUT_GPTK4_ERR_MODE, 0, 0, "mode=vector requires the four directions = null"); return -1; }
      if (st->mode == NXINPUT_GPTK4_STICK_DIGITAL && g->base.slot[vec].kind != NXINPUT_GPTK4_NULL) { seterr(e, NXINPUT_GPTK4_ERR_MODE, 0, 0, "mode=digital requires vector = null"); return -1; }
      if (anynative && anyaction) { seterr(e, NXINPUT_GPTK4_ERR_MIXED_OWNER, 0, 0, "mixed-owner-stick: native vector with action directions"); return -1; }
      if (!(st->exit >= 0.0f && st->exit < st->enter && st->enter <= 1.0f)) { seterr(e, NXINPUT_GPTK4_ERR_THRESHOLD, 0, 0, "0 <= exit_threshold < enter_threshold <= 1"); return -1; }
      if (st->mode == NXINPUT_GPTK4_STICK_SPLIT) {
        const nxinput_gptk4_binding *vb = &g->base.slot[vec]; int distinct = 1;
        if (!(st->dig_exit >= 0.0f && st->dig_exit < st->dig_enter && st->dig_enter <= 1.0f) || !(st->dir_exit >= 0.0f && st->dir_exit < st->dir_enter && st->dir_enter <= 1.0f) || !(st->dir_enter <= st->dig_exit / 1.41421356f)) { seterr(e, NXINPUT_GPTK4_ERR_THRESHOLD, 0, 0, "split zones: digital/direction thresholds invalid or direction_enter > digital_exit/sqrt2"); return -1; }
        if (vb->kind != NXINPUT_GPTK4_ACTION || !anyaction) { seterr(e, NXINPUT_GPTK4_ERR_MODE, 0, 0, "split needs an action vector AND action directions"); return -1; }
        for (d = 0; d < 4; d++) if (g->base.slot[base + d].kind == NXINPUT_GPTK4_ACTION && !strcmp(g->base.slot[base + d].action, vb->action)) distinct = 0;
        if (!distinct) { seterr(e, NXINPUT_GPTK4_ERR_MODE, 0, 0, "split needs two distinct semantic channels"); return -1; }
      }
  } }
  { int w; for (w = 0; w < 2; w++) { nxinput_gptk4_trigger *t = w ? &g->r2 : &g->l2; int an = w ? NXINPUT_GPTK4_R2_ANALOG : NXINPUT_GPTK4_L2_ANALOG; int dg = w ? NXINPUT_GPTK4_R2_DIGITAL : NXINPUT_GPTK4_L2_DIGITAL;
      if (seen_trig[w] != 7) { seterr(e, NXINPUT_GPTK4_ERR_OMITTED, 0, 0, "trigger needs mode, analog and digital"); return -1; }
      if (t->mode == NXINPUT_GPTK4_TRIGGER_ANALOG && g->base.slot[dg].kind != NXINPUT_GPTK4_NULL) { seterr(e, NXINPUT_GPTK4_ERR_MODE, 0, 0, "mode=analog requires digital = null"); return -1; }
      if (t->mode == NXINPUT_GPTK4_TRIGGER_DIGITAL && g->base.slot[an].kind != NXINPUT_GPTK4_NULL) { seterr(e, NXINPUT_GPTK4_ERR_MODE, 0, 0, "mode=digital requires analog = null"); return -1; }
      if (!(t->exit >= 0.0f && t->exit < t->enter && t->enter <= 1.0f)) { seterr(e, NXINPUT_GPTK4_ERR_THRESHOLD, 0, 0, "trigger thresholds"); return -1; }
  } }
  /* overrides must be sparse: an override equal to the whole base is refused */
  { unsigned o; for (o = 0; o < g->overrides; o++) { int s, n = 0, diverge = 0; for (s = 0; s < NXINPUT_GPTK4_SLOT_COUNT; s++) if (g->override[o].slot[s].kind) { n++; if (g->override[o].slot[s].kind != g->base.slot[s].kind || strcmp(g->override[o].slot[s].action, g->base.slot[s].action)) diverge = 1; }
      if (n == 0) { seterr(e, NXINPUT_GPTK4_ERR_MALFORMED, 0, 0, "empty override section"); return -1; }
      if (!diverge) { seterr(e, NXINPUT_GPTK4_ERR_DUPLICATE, 0, 0, "override repeats the base without divergence (contexts are exceptions only)"); return -1; } } }
  /* 0.11.1 (F2): the CROSS-SECTION loop -- a [keyboard.*] chord feeding an
   * action whose own gamepad binding emits @key of that very chord. The
   * in-line check above only saw a keyboard line carrying @key itself. */
  { unsigned kb, o; int sl;
    for (kb = 0; kb < g->keybinds; kb++) {
      const nxinput_gptk4_keybind *k = &g->keyboard[kb];
      if (k->binding.kind != NXINPUT_GPTK4_ACTION) continue;
      for (sl = 0; sl < (int)NXINPUT_GPTK4_SLOT_COUNT; sl++) {
        const nxinput_gptk4_binding *b = &g->base.slot[sl];
        if (b->kind == NXINPUT_GPTK4_ACTION && b->key[0] && !strcmp(b->action, k->binding.action) && !strcmp(b->key, k->chord)) { seterr(e, NXINPUT_GPTK4_ERR_KEYBOARD, k->binding.line, 1, "physical key -> action -> @key of the same chord would loop"); return -1; }
        for (o = 0; o < g->overrides; o++) {
          const nxinput_gptk4_binding *ob = &g->override[o].slot[sl];
          if (ob->kind == NXINPUT_GPTK4_ACTION && ob->key[0] && !strcmp(ob->action, k->binding.action) && !strcmp(ob->key, k->chord)) { seterr(e, NXINPUT_GPTK4_ERR_KEYBOARD, k->binding.line, 1, "physical key -> action -> @key of the same chord would loop"); return -1; }
        }
      }
    }
  }
  g->digest = fnv(text, len);
  return 0;
}

const nxinput_gptk4_binding *nxinput_gptk4_resolve(const nxinput_gptk4 *g, const char *context, nxinput_gptk4_control slot) {
  unsigned o;
  if (!g || (unsigned)slot >= NXINPUT_GPTK4_SLOT_COUNT) return NULL;
  if (context && *context) for (o = 0; o < g->overrides; o++) if (!strcmp(g->override[o].context, context) && g->override[o].slot[slot].kind) return &g->override[o].slot[slot];
  return &g->base.slot[slot];
}

int nxinput_gptk4_default(const char *port, const nxinput_gptk4_contract *c, char *out, size_t cap) {
  int n = snprintf(out, cap,
    "format = NEXTOS_CONTROLLERS/4\nport = %s\nCONTROL_STANDARD = xbox\nGLYPH_STYLE = xbox\nAUTHORITY = nextos\nCONTEXT_POLICY = unified\n\n"
    "# Positional Xbox surface: A = south, B = east, X = west, Y = north.\n# Every control is listed; edit here, once, for title/menu/gameplay.\n"
    "[base]\nA = native\nB = native\nX = native\nY = native\nL1 = native\nR1 = native\nL3 = native\nR3 = native\nSTART = native\nSELECT = native\nGUIDE = native\nUP = native\nDOWN = native\nLEFT = native\nRIGHT = native\n\n"
    "[stick.left]\nmode = vector\nvector = native\nup = null\ndown = null\nleft = null\nright = null\nenter_threshold = 0.55\nexit_threshold = 0.40\ndiagonal = 8way\ntie_break = horizontal\n\n"
    "[stick.right]\nmode = vector\nvector = native\nup = null\ndown = null\nleft = null\nright = null\nenter_threshold = 0.55\nexit_threshold = 0.40\ndiagonal = 8way\ntie_break = horizontal\n\n"
    "[trigger.left]\nmode = analog\nanalog = native\ndigital = null\nenter_threshold = 0.55\nexit_threshold = 0.40\n\n"
    "[trigger.right]\nmode = analog\nanalog = native\ndigital = null\nenter_threshold = 0.55\nexit_threshold = 0.40\n", port);
  if (n < 0 || (size_t)n >= cap) return -1;
  if (c && c->extensions && c->extension_count) {
    /* E4a: declared extensions are emitted into [base], explicitly and visibly
     * unbound (`null`) -- the owner sees every bindable control the device has. */
    size_t i; char block[1024]; int m = snprintf(block, sizeof block, "# Extension controls declared by this device/adapter (capability-gated); unbound until you edit them.\n");
    char *at = strstr(out, "RIGHT = native\n\n"); size_t head, tail;
    if (m < 0 || !at) return -1;
    for (i = 0; i < c->extension_count; i++) { int w = snprintf(block + m, sizeof block - (size_t)m, "%s = null\n", c->extensions[i]); if (w < 0 || (size_t)w >= sizeof block - (size_t)m) return -1; m += w; }
    at += strlen("RIGHT = native\n");
    head = (size_t)(at - out); tail = (size_t)n - head;
    if ((size_t)n + (size_t)m >= cap) return -1;
    memmove(at + m, at, tail + 1); memcpy(at, block, (size_t)m); n += m;
  }
  return n;
}

int nxinput_gptk4_ext_name_valid(const char *name) {
  size_t i, n;
  if (!name || strncmp(name, "EXT.", 4) != 0) return 0;
  n = strlen(name);
  if (n < 5 || n > NXINPUT_GPTK4_EXT_NAME_MAX) return 0;
  for (i = 4; i < n; i++) { char ch = name[i]; if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')) return 0; }
  return 1;
}
const nxinput_gptk4_binding *nxinput_gptk4_ext(const nxinput_gptk4 *g, const char *name) {
  unsigned i; if (!g || !name) return NULL;
  for (i = 0; i < g->exts; i++) if (strcmp(g->ext[i].name, name) == 0) return &g->ext[i].binding;
  return NULL;
}
