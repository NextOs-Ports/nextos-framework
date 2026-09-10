/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nx-gptk4-host-gate -- nxinput 0.11.1 (M1c item 1): the UNIVERSAL host gate
 * for a NEXTOS_CONTROLLERS/4 owner.
 *
 *   nx-gptk4-host-gate --contract adapter-contract.json --owner-dir DIR
 *                      --contexts menu,gameplay [--closure closure.tsv]
 *
 * Given the port's adapter contract (actions with kinds and sinks, and the
 * declared contexts with their control -> action closure) and the game
 * directory (owner NEXTOSCONTROLLERS.gptk + defaults/), it proves on the
 * host what every schema-4 port needs proved and what the Tearscape harness
 * used to do with the V3 loader (and therefore failed with NXI1006 on every
 * schema-4 owner):
 *   (a) the owner/default loads through nxinput_gptk4_preinit_load with the
 *       contract's actions (kinds) and overlay contexts;
 *   (b) recorder sinks are registered for every action, the runtime seals;
 *   (c) per DECLARED context: every closure case decides exactly as expected
 *       (ACTION with that action), every other control decides NONE/
 *       SUPPRESS/NATIVE -- ONLY in declared contexts (a V3 context the port
 *       never declares inherits [base] by contract and does not count);
 *   (d) live delivery: passthrough before any context is proven; each case
 *       delivered exactly once to its sink, then released;
 *   (e) the NXGPTK_PROOF\tCONTEXT|CASE|SAFETY lines the generator's
 *       make_input_proof already parses.
 * The expected closure comes from the CONTRACT (the project's declaration),
 * never from the map under test; --closure overrides/extends it with a TSV:
 *   context<TAB>control<TAB>action<TAB>sink<TAB>press|motion<TAB>1
 * Exit 0 = PASS, 1 = FAIL (first failure named), 2 = usage.
 */
#define _POSIX_C_SOURCE 200809L
#include "nxinput_gptk4_preinit.h"
#include "nxinput_gptk_live.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ACTIONS 96
#define MAX_CASES 256
typedef struct { char id[65]; uint8_t kind; char sink[96]; } action_t;
typedef struct { int ctx; int control; char action[65]; char sink[96]; int vector; } case_t;
static action_t actions[MAX_ACTIONS]; static size_t n_actions;
static const char *action_ids[MAX_ACTIONS];
static nxinput_gptk4_action_decl decls[MAX_ACTIONS];
static char overlay_names[8][24]; static const char *overlay_ptrs[8]; static size_t n_overlays;
static case_t cases[MAX_CASES]; static size_t n_cases;
static int declared_ctx[NXINPUT_GPTK_CONTEXT_COUNT];
static nxinput_gptk4_preinit_result R;
static nxinput_gptk_live live;
static char events[64][160]; static int n_events;
static int fails; static char first_fail[200];

static void fail(const char *why) { if (!fails) snprintf(first_fail, sizeof first_fail, "%s", why); fails++; printf("FAIL: %s\n", why); }

/* ---- a tiny tolerant JSON scanner (enough for adapter-contract.json) ---- */
typedef struct { const char *p; } js;
static void ws(js *j) { while (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t') j->p++; }
static int str(js *j, char *out, size_t cap) { size_t n = 0; ws(j); if (*j->p != '"') return 0; j->p++; while (*j->p && *j->p != '"') { if (*j->p == '\\' && j->p[1]) j->p++; if (n + 1 < cap) out[n++] = *j->p; j->p++; } if (*j->p == '"') j->p++; out[n] = 0; return 1; }
static void skip_value(js *j);
static void skip_object(js *j) { int depth = 0; do { if (*j->p == '{' || *j->p == '[') depth++; else if (*j->p == '}' || *j->p == ']') depth--; else if (*j->p == '"') { char t[8]; str(j, t, sizeof t); continue; } if (*j->p) j->p++; } while (*j->p && depth > 0); }
static void skip_value(js *j) { ws(j); if (*j->p == '{' || *j->p == '[') skip_object(j); else if (*j->p == '"') { char t[8]; str(j, t, sizeof t); } else while (*j->p && *j->p != ',' && *j->p != '}' && *j->p != ']') j->p++; }
/* position j at the value of key `key` inside the object starting at j->p ('{'); 1 when found */
static int find_key(js *j, const char *key) { char k[96]; ws(j); if (*j->p != '{') return 0; j->p++; for (;;) { ws(j); if (*j->p == '}') return 0; if (!str(j, k, sizeof k)) return 0; ws(j); if (*j->p == ':') j->p++; ws(j); if (strcmp(k, key) == 0) return 1; skip_value(j); ws(j); if (*j->p == ',') j->p++; } }

static uint8_t kind_of(const char *k) { if (!strcmp(k, "vector") || !strcmp(k, "vector2")) return NXINPUT_GPTK4_V_VECTOR2; if (!strcmp(k, "axis") || !strcmp(k, "scalar")) return NXINPUT_GPTK4_V_SCALAR; return NXINPUT_GPTK4_V_DIGITAL; }
static int control_of(const char *name) { int c; for (c = 0; c < (int)NXINPUT_GPTK_CONTROL_COUNT; c++) if (!strcmp(nxinput_gptk_control_name(c), name)) return c; return -1; }
static int ctx_of(const char *name) { if (!strcmp(name, "menu")) return NXINPUT_GPTK_CONTEXT_MENU; if (!strcmp(name, "gameplay")) return NXINPUT_GPTK_CONTEXT_GAMEPLAY; if (!strcmp(name, "cursor")) return NXINPUT_GPTK_CONTEXT_CURSOR; return -1; }
static const char *ctx_name(int c) { return c == NXINPUT_GPTK_CONTEXT_MENU ? "menu" : c == NXINPUT_GPTK_CONTEXT_GAMEPLAY ? "gameplay" : "cursor"; }
static const action_t *action_by_id(const char *id) { size_t i; for (i = 0; i < n_actions; i++) if (!strcmp(actions[i].id, id)) return &actions[i]; return NULL; }
static void add_case(int ctx, int control, const char *action, const char *sink, int vector) {
  size_t i; for (i = 0; i < n_cases; i++) if (cases[i].ctx == ctx && cases[i].control == control) { snprintf(cases[i].action, sizeof cases[i].action, "%s", action); snprintf(cases[i].sink, sizeof cases[i].sink, "%s", sink); cases[i].vector = vector; return; }
  if (n_cases >= MAX_CASES) return;
  cases[n_cases].ctx = ctx; cases[n_cases].control = control; snprintf(cases[n_cases].action, sizeof cases[n_cases].action, "%s", action); snprintf(cases[n_cases].sink, sizeof cases[n_cases].sink, "%s", sink); cases[n_cases].vector = vector; n_cases++;
}

static char *slurp(const char *path) { FILE *f = fopen(path, "rb"); long n; char *b; if (!f) return NULL; fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); if (n < 0 || n > (1 << 20)) { fclose(f); return NULL; } b = malloc((size_t)n + 1); if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; } b[n] = 0; fclose(f); return b; }

static int load_contract(const char *path) {
  char *text = slurp(path); js j; js top; char key[96], val[160], id[65], kind[24], sink[96];
  if (!text) { fail("contract file unreadable"); return -1; }
  j.p = text; if (!find_key(&j, "input")) { free(text); fail("contract has no input object"); return -1; }
  top = j;
  /* actions */
  j = top;
  if (find_key(&j, "actions")) {
    ws(&j); if (*j.p == '[') j.p++;
    for (;;) { js o; ws(&j); if (*j.p == ']' || !*j.p) { break; } if (*j.p == ',') { j.p++; continue; }
      o = j; id[0] = kind[0] = sink[0] = 0;
      { js t = o; if (find_key(&t, "id")) str(&t, id, sizeof id); }
      { js t = o; if (find_key(&t, "kind")) str(&t, kind, sizeof kind); }
      { js t = o; if (find_key(&t, "sinks")) { ws(&t); if (*t.p == '[') { t.p++; str(&t, sink, sizeof sink); } } }
      if (id[0] && n_actions < MAX_ACTIONS) { snprintf(actions[n_actions].id, sizeof actions[n_actions].id, "%s", id); actions[n_actions].kind = kind_of(kind); snprintf(actions[n_actions].sink, sizeof actions[n_actions].sink, "%s", sink); n_actions++; }
      skip_value(&j); }
  }
  /* contexts: control -> action|native|null */
  j = top;
  if (find_key(&j, "contexts")) {
    ws(&j); if (*j.p == '{') j.p++;
    for (;;) { int ctx; ws(&j); if (*j.p == '}' || !*j.p) { break; } if (*j.p == ',') { j.p++; continue; }
      if (!str(&j, key, sizeof key)) { break; }
      ws(&j); if (*j.p == ':') { j.p++; } ws(&j);
      ctx = ctx_of(key);
      if (ctx >= 0 && strcmp(key, "gameplay") != 0 && n_overlays < 8) { snprintf(overlay_names[n_overlays], sizeof overlay_names[0], "%s", key); overlay_ptrs[n_overlays] = overlay_names[n_overlays]; n_overlays++; }
      if (*j.p == '{') { j.p++;
        for (;;) { int control; ws(&j); if (*j.p == '}' || !*j.p) { break; } if (*j.p == ',') { j.p++; continue; }
          if (!str(&j, key, sizeof key)) { break; }
          ws(&j); if (*j.p == ':') { j.p++; }
          if (!str(&j, val, sizeof val)) { skip_value(&j); continue; }
          control = control_of(key);
          if (ctx >= 0 && control >= 0 && strcmp(val, "native") != 0 && strcmp(val, "null") != 0) {
            const action_t *a = action_by_id(val);
            add_case(ctx, control, val, a ? a->sink : "", a ? a->kind == NXINPUT_GPTK4_V_VECTOR2 : (control == NXINPUT_GPTK_LEFT_STICK || control == NXINPUT_GPTK_RIGHT_STICK));
          }
        }
        if (*j.p == '}') j.p++;
      } else skip_value(&j);
    }
  }
  free(text);
  return n_actions ? 0 : -1;
}

static int load_closure(const char *path) {
  FILE *f = fopen(path, "r"); char line[512];
  if (!f) { fail("closure file unreadable"); return -1; }
  while (fgets(line, sizeof line, f)) {
    char ctx[24], control[24], action[65], sink[96], event[16]; int count = 0;
    if (line[0] == '#' || line[0] == '\n') continue;
    if (sscanf(line, "%23[^\t]\t%23[^\t]\t%64[^\t]\t%95[^\t]\t%15[^\t]\t%d", ctx, control, action, sink, event, &count) < 5) { fclose(f); fail("closure line malformed"); return -1; }
    if (ctx_of(ctx) < 0 || control_of(control) < 0) { fclose(f); fail("closure names an unknown context/control"); return -1; }
    add_case(ctx_of(ctx), control_of(control), action, sink, strcmp(event, "motion") == 0);
  }
  fclose(f); return 0;
}

static int rec_sink(void *u, const char *action, int pressed, float v) { (void)u; (void)v; if (n_events < 64) snprintf(events[n_events++], 160, "sink:%s:%d", action, pressed); return 0; }
static int rec_vec(void *u, const char *action, float x, float y) { (void)u; if (n_events < 64) snprintf(events[n_events++], 160, "vector:%s:%.2f,%.2f", action, (double)x, (double)y); return 0; }

int main(int argc, char **argv) {
  const char *contract = NULL, *owner_dir = NULL, *contexts = "menu,gameplay", *closure = NULL; int i; size_t k; char err[160];
  nxinput_gptk4_contract c; char ctxbuf[64];
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--contract") && i + 1 < argc) contract = argv[++i];
    else if (!strcmp(argv[i], "--owner-dir") && i + 1 < argc) owner_dir = argv[++i];
    else if (!strcmp(argv[i], "--contexts") && i + 1 < argc) contexts = argv[++i];
    else if (!strcmp(argv[i], "--closure") && i + 1 < argc) closure = argv[++i];
    else { fprintf(stderr, "usage: %s --contract adapter-contract.json --owner-dir DIR [--contexts menu,gameplay] [--closure closure.tsv]\n", argv[0]); return 2; }
  }
  if (!contract || !owner_dir) { fprintf(stderr, "usage: --contract and --owner-dir are required\n"); return 2; }
  if (load_contract(contract) != 0) { printf("gptk4-host-gate: FAIL reason=%s\n", first_fail[0] ? first_fail : "contract"); return 1; }
  if (closure && load_closure(closure) != 0) { printf("gptk4-host-gate: FAIL reason=%s\n", first_fail); return 1; }
  snprintf(ctxbuf, sizeof ctxbuf, "%s", contexts);
  { char *tok = strtok(ctxbuf, ","); while (tok) { int cx = ctx_of(tok); if (cx < 0) { printf("gptk4-host-gate: FAIL reason=unknown-context-%s\n", tok); return 1; } declared_ctx[cx] = 1; tok = strtok(NULL, ","); } }
  for (k = 0; k < n_actions; k++) { action_ids[k] = actions[k].id; decls[k].id = actions[k].id; decls[k].value_kind = actions[k].kind; }
  memset(&c, 0, sizeof c); c.actions = decls; c.count = n_actions; c.contexts = overlay_ptrs; c.context_count = n_overlays; c.keyboard_backend = 0;
  /* (a) load through the universal pre-init */
  if (nxinput_gptk4_preinit_load(owner_dir, &c, &R) != 0 || !R.v3.loaded) {
    printf("%s\n", R.receipt_json);
    printf("gptk4-host-gate: FAIL reason=owner-not-loaded rc=NXI%04d line=%u col=%u what=%s\n", R.bridge.rc, R.bridge.line, R.bridge.column, R.bridge.what);
    return 1;
  }
  printf("%s\n", R.receipt_json);
  printf("gptk4-host-gate: loaded source=%s sha256=%s actions=%lu overlays=%lu cases=%lu\n", R.bridge.source == NXINPUT_GPTK4_SRC_OWNER ? "owner" : "default", R.bridge.sha256, (unsigned long)n_actions, (unsigned long)n_overlays, (unsigned long)n_cases);
  /* (b) recorder sinks + seal */
  nxinput_gptk_live_init(&live, &R.v3.map);
  for (k = 0; k < n_actions; k++) {
    if (actions[k].kind == NXINPUT_GPTK4_V_VECTOR2) nxinput_gptk_live_register_vector(&live, actions[k].id, rec_vec, NULL);
    else nxinput_gptk_live_register(&live, actions[k].id, rec_sink, NULL);
  }
  if (nxinput_gptk_live_seal(&live, err, sizeof err) != 0) { printf("gptk4-host-gate: FAIL reason=seal: %s\n", err); return 1; }
  /* (d1) passthrough before any context */
  n_events = 0;
  if (nxinput_gptk_live_feed(&live, NXINPUT_GPTK_A, 1, 1.0f) != NXINPUT_GPTK_LIVE_PASSTHROUGH || n_events != 0) fail("input consumed before a context was proven");
  else printf("NXGPTK_PROOF\tSAFETY\tunknown_context\tPASSTHROUGH\t0\n");
  /* (c) decisions per declared context */
  for (i = 0; i < (int)NXINPUT_GPTK_CONTEXT_COUNT; i++) {
    int control;
    if (!declared_ctx[i]) continue;
    if (!R.v3.map.context_present[i]) { char w[120]; snprintf(w, sizeof w, "declared context %s absent from the projected map", ctx_name(i)); fail(w); continue; }
    for (control = 0; control < (int)NXINPUT_GPTK_CONTROL_COUNT; control++) {
      const char *action = NULL; nxinput_gptk_decision d = nxinput_gptk_decide(&R.v3.map, (nxinput_gptk_context)i, control, &action);
      const case_t *cs = NULL; size_t q; for (q = 0; q < n_cases; q++) if (cases[q].ctx == i && cases[q].control == control) cs = &cases[q];
      if (cs) {
        if (d != NXINPUT_GPTK_DECIDE_ACTION || !action || strcmp(action, cs->action) != 0) { char w[200]; snprintf(w, sizeof w, "%s %s: expected action %s, owner decides %s%s%s", ctx_name(i), nxinput_gptk_control_name(control), cs->action, d == NXINPUT_GPTK_DECIDE_ACTION ? "action " : d == NXINPUT_GPTK_DECIDE_SUPPRESS ? "null" : d == NXINPUT_GPTK_DECIDE_NATIVE ? "native" : "none", d == NXINPUT_GPTK_DECIDE_ACTION ? (action ? action : "") : "", ""); fail(w); }
      } else if (d == NXINPUT_GPTK_DECIDE_ACTION) { char w[200]; snprintf(w, sizeof w, "%s %s: undeclared extra binding action %s in a declared context", ctx_name(i), nxinput_gptk_control_name(control), action ? action : "?"); fail(w); }
    }
  }
  /* (d2) live delivery of every case, once, in context order */
  for (i = 0; i < (int)NXINPUT_GPTK_CONTEXT_COUNT; i++) {
    size_t q; char source[40];
    if (!declared_ctx[i] || !R.v3.map.context_present[i]) continue;
    snprintf(source, sizeof source, "scene:%s", ctx_name(i));
    if (nxinput_gptk_live_set_context(&live, (nxinput_gptk_context)i, source) != 0) { char w[120]; snprintf(w, sizeof w, "context %s could not be proven", ctx_name(i)); fail(w); continue; }
    printf("NXGPTK_PROOF\tCONTEXT\t%s\t%s\n", ctx_name(i), source);
    for (q = 0; q < n_cases; q++) {
      const case_t *cs = &cases[q]; char expected[200];
      if (cs->ctx != i) continue;
      n_events = 0;
      if (cs->vector) {
        snprintf(expected, sizeof expected, "vector:%s:", cs->action);
        if (nxinput_gptk_live_feed_vector(&live, cs->control, 0.75f, -0.25f) != NXINPUT_GPTK_LIVE_DELIVERED || n_events != 1 || strncmp(events[0], expected, strlen(expected)) != 0) { char w[200]; snprintf(w, sizeof w, "%s %s: vector not delivered exactly once to %s", ctx_name(i), nxinput_gptk_control_name(cs->control), cs->action); fail(w); }
        else printf("NXGPTK_PROOF\tCASE\t%s\t%s\t%s\tmotion\t%s\t%s\t1\n", ctx_name(i), source, nxinput_gptk_control_name(cs->control), cs->action, cs->sink[0] ? cs->sink : "-");
        (void)nxinput_gptk_live_feed_vector(&live, cs->control, 0.0f, 0.0f);
      } else {
        snprintf(expected, sizeof expected, "sink:%s:1", cs->action);
        if (nxinput_gptk_live_feed(&live, cs->control, 1, 1.0f) != NXINPUT_GPTK_LIVE_DELIVERED || n_events != 1 || strcmp(events[0], expected) != 0) { char w[200]; snprintf(w, sizeof w, "%s %s: press not delivered exactly once to %s", ctx_name(i), nxinput_gptk_control_name(cs->control), cs->action); fail(w); }
        else printf("NXGPTK_PROOF\tCASE\t%s\t%s\t%s\tpress\t%s\t%s\t1\n", ctx_name(i), source, nxinput_gptk_control_name(cs->control), cs->action, cs->sink[0] ? cs->sink : "-");
        n_events = 0; (void)nxinput_gptk_live_feed(&live, cs->control, 0, 0.0f);
        snprintf(expected, sizeof expected, "sink:%s:0", cs->action);
        if (n_events != 1 || strcmp(events[0], expected) != 0) { char w[200]; snprintf(w, sizeof w, "%s %s: release not delivered exactly once", ctx_name(i), nxinput_gptk_control_name(cs->control)); fail(w); }
      }
    }
  }
  if (fails) { printf("gptk4-host-gate: FAIL reason=%s failures=%d\n", first_fail, fails); return 1; }
  printf("gptk4-host-gate: PASS cases=%lu contexts=%s owner_sha256=%s source=%s\n", (unsigned long)n_cases, contexts, R.bridge.sha256, R.bridge.source == NXINPUT_GPTK4_SRC_OWNER ? "owner" : "default");
  return 0;
}
