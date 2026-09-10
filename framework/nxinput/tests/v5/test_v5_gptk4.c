/* SPDX-License-Identifier: GPL-3.0-only */
/* V5 / E1-E12: NEXTOS_CONTROLLERS/4 parser contract. */
#include "../../include/nxinput_gptk4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static const nxinput_gptk4_action_decl acts[] = {{"fp2.primary", 0}, {"fp2.secondary", 0}, {"fp2.special", 0}, {"fp2.guard", 0}, {"fp2.pause", 0}, {"fp2.move", 2}, {"fp2.brake", 1}, {"fp2.aim", 2}, {"menu.confirm", 0}, {"menu.cancel", 0}, {"player.jump", 0}};
static const char *const ctxs[] = {"menu", "pause", "cursor"};
static char *slurp(const char *p, size_t *n) { FILE *f = fopen(p, "rb"); char *b; if (!f) return NULL; fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET); b = malloc(*n + 1); fread(b, 1, *n, f); b[*n] = 0; fclose(f); return b; }
static int parse_str(const char *s, nxinput_gptk4_contract *c, nxinput_gptk4 *g, nxinput_gptk4_error *e) { return nxinput_gptk4_parse(s, strlen(s), c, g, e); }
/* replace one line "key = value" inside a copy of the canonical text */
static char *with(const char *base, const char *line_prefix, const char *replacement) {
  char *out = malloc(strlen(base) + strlen(replacement) + 8); const char *p = strstr(base, line_prefix); const char *eol;
  if (!p) { strcpy(out, base); return out; }
  eol = strchr(p + strlen(line_prefix) - 1, '\n'); memcpy(out, base, (size_t)(p - base)); out[p - base] = 0; strcat(out, replacement); strcat(out, eol ? eol : ""); return out;
}
int main(int argc, char **argv) {
  nxinput_gptk4_contract c = {acts, sizeof acts / sizeof acts[0], 1, ctxs, 3, NULL, 0};
  nxinput_gptk4_contract c_nokb = {acts, sizeof acts / sizeof acts[0], 0, ctxs, 3, NULL, 0};
  nxinput_gptk4 g; nxinput_gptk4_error e; size_t n; char *t; char *m;
  const char *path = argc > 1 ? argv[1] : "tests/v5/corpus/fp2-complete.gptk";
  t = slurp(path, &n); if (!t) { printf("FAIL cannot read %s\n", path); return 1; }
  CHECK(nxinput_gptk4_parse(t, n, &c, &g, &e) == 0, "canonical complete fixture parses");
  if (fails) printf("     err %d line %u col %u: %s\n", e.code, e.line, e.column, e.what);
  CHECK(strcmp(g.port, "fp2") == 0 && g.authority == NXINPUT_GPTK4_AUTH_NEXTOS && !g.glyph_style_opt_in, "header decoded; xbox glyphs are the default");
  CHECK(nxinput_gptk4_resolve(&g, "gameplay", NXINPUT_GPTK4_X)->kind == NXINPUT_GPTK4_ACTION && nxinput_gptk4_resolve(&g, "menu", NXINPUT_GPTK4_X)->kind == NXINPUT_GPTK4_NULL, "override.menu X=null is sparse: X differs only in menu");
  CHECK(nxinput_gptk4_resolve(&g, "menu", NXINPUT_GPTK4_A)->kind == NXINPUT_GPTK4_ACTION && !strcmp(nxinput_gptk4_resolve(&g, "menu", NXINPUT_GPTK4_A)->action, "fp2.primary"), "A in menu inherits the base family (edit once, all screens)");
  CHECK(nxinput_gptk4_resolve(&g, "loading-unknown", NXINPUT_GPTK4_A)->kind == NXINPUT_GPTK4_ACTION, "unknown context = base passthrough");
  CHECK(g.left.mode == NXINPUT_GPTK4_STICK_VECTOR && g.base.slot[NXINPUT_GPTK4_LS_VECTOR].kind == NXINPUT_GPTK4_ACTION, "stick.left vector action");
  /* didactic fragment of 6.2 must be REJECTED as a complete file */
  CHECK(parse_str("format = NEXTOS_CONTROLLERS/4\nport = fp2\nCONTROL_STANDARD = xbox\nGLYPH_STYLE = xbox\nAUTHORITY = nextos\nCONTEXT_POLICY = unified\n[base]\nA = action:fp2.primary\nB = action:fp2.secondary\nX = action:fp2.special\nY = action:fp2.guard\nSTART = action:fp2.pause\n[stick.left]\nmode = vector\nvector = action:fp2.move\nup = null\ndown = null\nleft = null\nright = null\n[override.menu]\nX = null\n", &c, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_OMITTED, "6.2 didactic fragment rejected: omission is an error");
  /* omission of each core/stick/trigger key fails */
  { const char *keys[] = {"L3 = ", "GUIDE = ", "RIGHT = ", "[stick.right]\nmode = ", "digital = null\n\n[trigger.right]"}; int i;
    for (i = 0; i < 3; i++) { m = with(t, keys[i], "# removed"); CHECK(parse_str(m, &c, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_OMITTED, "omitting a core control fails (NXI4001)"); free(m); } }
  m = with(t, "up = null", "up = native"); CHECK(parse_str(m, &c, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_NATIVE_DERIVED, "native on a derived direction refused"); free(m);
  m = with(t, "A = action:fp2.primary", "A = action:fp2.move"); CHECK(parse_str(m, &c, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_KIND, "digital control bound to a vector2 action refused"); free(m);
  m = with(t, "vector = action:fp2.move", "vector = action:fp2.primary"); CHECK(parse_str(m, &c, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_KIND, "vector bound to a digital action refused"); free(m);
  m = with(t, "A = action:fp2.primary", "A = action:fp2.primary@key:SPACE"); CHECK(parse_str(m, &c, &g, &e) == 0 && !strcmp(g.base.slot[NXINPUT_GPTK4_A].key, "SPACE"), "@key on a digital action accepted with keyboard backend"); CHECK(parse_str(m, &c_nokb, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_KEYBOARD, "@key refused without a proved keyboard backend"); free(m);
  m = with(t, "A = action:fp2.primary", "A = key:SPACE"); CHECK(parse_str(m, &c, &g, &e) == -1, "bare key: output invalid in schema 4"); free(m);
  m = with(t, "A = action:fp2.primary", "A = action:fp2.primary@key:LSHIFT+LCTRL+S"); CHECK(parse_str(m, &c, &g, &e) == 0 && !strcmp(g.base.slot[NXINPUT_GPTK4_A].key, "LCTRL+LSHIFT+S"), "modifiers serialize in canonical order"); free(m);
  m = with(t, "A = action:fp2.primary", "A = action:fp2.primary@key:RM"); CHECK(parse_str(m, &c, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_KEYBOARD, "key outside the allowlist refused"); free(m);
  m = with(t, "B = action:fp2.secondary", "B = action:nope.unknown"); CHECK(parse_str(m, &c, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_UNKNOWN, "undeclared action refused"); free(m);
  m = with(t, "enter_threshold = 0.55", "enter_threshold = 0.30"); CHECK(parse_str(m, &c, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_THRESHOLD, "enter <= exit refused"); free(m);
  m = with(t, "mode = analog", "mode = digital"); CHECK(parse_str(m, &c, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_MODE, "trigger digital with analog=native refused (exclusive ownership)"); free(m);
  /* normative remap A -> R2: A=null, trigger.right digital */
  { char *x = with(t, "A = action:fp2.primary", "A = null"); char *y = with(x, "[trigger.right]\nmode = analog\nanalog = native\ndigital = null", "[trigger.right]\nmode = digital\nanalog = null\ndigital = action:fp2.primary");
    CHECK(parse_str(y, &c, &g, &e) == 0 && g.r2.mode == NXINPUT_GPTK4_TRIGGER_DIGITAL && !strcmp(g.base.slot[NXINPUT_GPTK4_R2_DIGITAL].action, "fp2.primary") && g.base.slot[NXINPUT_GPTK4_A].kind == NXINPUT_GPTK4_NULL, "remap A -> R2 (A=null, R2 digital) accepted in one transaction"); free(x); free(y); }
  m = with(t, "[override.menu]\nX = null", "[override.menu]\nA = action:fp2.primary"); CHECK(parse_str(m, &c, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_DUPLICATE, "override repeating the base without divergence refused"); free(m);
  m = with(t, "[override.menu]", "[override.dialog]"); CHECK(parse_str(m, &c, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_UNKNOWN, "undeclared override context refused"); free(m);
  m = with(t, "L1 = native", "L1 = native\nL1 = null"); CHECK(parse_str(m, &c, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_DUPLICATE && e.line == 16, "duplicate control refused with the offending line"); free(m);
  m = with(t, "vector = action:fp2.move", "vector = native"); { char *y = with(m, "up = null", "up = action:player.jump"); CHECK(parse_str(y, &c, &g, &e) == -1 && (e.code == NXINPUT_GPTK4_ERR_MODE || e.code == NXINPUT_GPTK4_ERR_MIXED_OWNER), "vector mode with an action direction refused (mixed owner)"); free(y); } free(m);
  /* split: needs zones + distinct channels */
  { char *x = with(t, "mode = vector\nvector = action:fp2.move", "mode = split\nsplit_policy = zones\ndirection_enter_threshold = 0.5\ndirection_exit_threshold = 0.3\ndigital_enter_threshold = 0.9\ndigital_exit_threshold = 0.8\nvector = action:fp2.move");
    char *y = with(x, "up = null", "up = action:player.jump");
    CHECK(parse_str(y, &c, &g, &e) == 0 && g.left.mode == NXINPUT_GPTK4_STICK_SPLIT, "split with valid zones and distinct channels accepted");
    { char *z = with(y, "direction_enter_threshold = 0.5", "direction_enter_threshold = 0.7"); CHECK(parse_str(z, &c, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_THRESHOLD, "split: direction_enter > digital_exit/sqrt2 refused"); free(z); }
    free(x); free(y); }
  /* E4a: capability-gated extensions */
  { static const char *const exts[] = {"EXT.MISC1", "EXT.PADDLE1"}; nxinput_gptk4_contract cx = c; char *y; char dflt[8192]; int n;
    cx.extensions = exts; cx.extension_count = 2;
    CHECK(parse_str(t, &cx, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_OMITTED && strstr(e.what, "EXT.MISC1"), "declared extension omitted from the owner: NXI4001 (never silent null)");
    y = with(t, "GUIDE = native", "GUIDE = native\nEXT.MISC1 = action:player.jump\nEXT.PADDLE1 = null");
    CHECK(parse_str(y, &cx, &g, &e) == 0 && g.exts == 2 && nxinput_gptk4_ext(&g, "EXT.MISC1") && nxinput_gptk4_ext(&g, "EXT.MISC1")->kind == NXINPUT_GPTK4_ACTION && nxinput_gptk4_ext(&g, "EXT.PADDLE1")->kind == NXINPUT_GPTK4_NULL, "declared extensions bound in [base] like any digital control");
    CHECK(parse_str(y, &c, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_UNKNOWN, "MUTANT killed: EXT.* not declared by the adapter (improvised public name) refused");
    free(y); y = with(t, "GUIDE = native", "GUIDE = native\nEXT.misc = null\nEXT.PADDLE1 = null");
    CHECK(parse_str(y, &cx, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_UNKNOWN, "extension name outside EXT.[A-Z0-9_]+ refused");
    free(y); y = with(t, "GUIDE = native", "GUIDE = native\nEXT.MISC1 = action:fp2.move\nEXT.PADDLE1 = null");
    CHECK(parse_str(y, &cx, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_KIND, "extension is digital: vector2 action refused");
    free(y);
    n = nxinput_gptk4_default("fp2", &cx, dflt, sizeof dflt);
    CHECK(n > 0 && strstr(dflt, "EXT.MISC1 = null\n") && strstr(dflt, "EXT.PADDLE1 = null\n") && strstr(dflt, "EXT.MISC1 = null") < strstr(dflt, "[stick.left]"), "default lists every declared extension explicitly (visible null) inside [base]");
    CHECK(parse_str(dflt, &cx, &g, &e) == 0 && g.exts == 2, "generated default with extensions parses complete");
    n = nxinput_gptk4_default("fp2", &c, dflt, sizeof dflt);
    CHECK(n > 0 && !strstr(dflt, "EXT."), "no declared extension: default carries none (nothing improvised)");
  }
  /* keyboard source */
  { char buf[8192]; snprintf(buf, sizeof buf, "%s\n[keyboard.base]\nSPACE = action:player.jump\nENTER = action:menu.confirm\nS+LCTRL = action:fp2.pause\n[keyboard.override.menu]\nESCAPE = action:menu.cancel\n", t);
    CHECK(parse_str(buf, &c, &g, &e) == 0 && g.keybinds == 4 && !strcmp(g.keyboard[2].chord, "LCTRL+S") && !strcmp(g.keyboard[3].context, "menu"), "keyboard source grammar with canonical chords and overlay");
    CHECK(parse_str(buf, &c_nokb, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_KEYBOARD, "keyboard source refused without keyboard-capable adapter");
    snprintf(buf, sizeof buf, "%s\n[keyboard.base]\nSPACE = action:player.jump@key:SPACE\n", t);
    CHECK(parse_str(buf, &c, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_KEYBOARD, "physical key -> action -> @key loop refused"); }
  /* generator default is complete and parses */
  { char d[8192]; CHECK(nxinput_gptk4_default("fp2", &c, d, sizeof d) > 0 && parse_str(d, &c, &g, &e) == 0, "generated default is a complete schema-4 file"); }
  CHECK(parse_str("format = NEXTOS_CONTROLLERS/3\n", &c, &g, &e) == -1 && e.code == NXINPUT_GPTK4_ERR_MAGIC, "schema 3 magic refused by the schema-4 parser (legacy read-only path is separate)");
  free(t);
  printf(fails ? "v5-gptk4: FAIL\n" : "v5-gptk4: OK\n");
  return fails ? 1 : 0;
}
