/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_gptk4_bridge -- see include/nxinput_gptk4_bridge.h. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "nxinput_gptk4_bridge.h"
#include "nxinput_sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void set_kind(nxinput_gptk *out, int ctx, int ctl, const nxinput_gptk4_binding *b,
                     nxinput_gptk4_bridge_receipt *r) {
  out->action[ctx][ctl][0] = '\0';
  switch (b->kind) {
    case NXINPUT_GPTK4_ACTION:
      out->kind[ctx][ctl] = NXINPUT_GPTK_BINDING_ACTION;
      snprintf(out->action[ctx][ctl], sizeof out->action[ctx][ctl], "%s", b->action);
      if (b->key[0] && r) r->key_outputs++;
      break;
    case NXINPUT_GPTK4_NULL: out->kind[ctx][ctl] = NXINPUT_GPTK_BINDING_NULL; break;
    case NXINPUT_GPTK4_NATIVE: out->kind[ctx][ctl] = NXINPUT_GPTK_BINDING_NATIVE; break;
    default: out->kind[ctx][ctl] = NXINPUT_GPTK_BINDING_NULL; break; /* unset never reaches here (parser refuses) */
  }
}

int nxinput_gptk4_project(const nxinput_gptk4 *g4, nxinput_gptk *out, nxinput_gptk4_bridge_receipt *r) {
  unsigned unsupported = 0;
  static const struct { int v3; nxinput_gptk4_control v4; } discrete[] = {
    {NXINPUT_GPTK_A, NXINPUT_GPTK4_A}, {NXINPUT_GPTK_B, NXINPUT_GPTK4_B},
    {NXINPUT_GPTK_X, NXINPUT_GPTK4_X}, {NXINPUT_GPTK_Y, NXINPUT_GPTK4_Y},
    {NXINPUT_GPTK_L1, NXINPUT_GPTK4_L1}, {NXINPUT_GPTK_R1, NXINPUT_GPTK4_R1},
    {NXINPUT_GPTK_L3, NXINPUT_GPTK4_L3}, {NXINPUT_GPTK_R3, NXINPUT_GPTK4_R3},
    {NXINPUT_GPTK_START, NXINPUT_GPTK4_START}, {NXINPUT_GPTK_SELECT, NXINPUT_GPTK4_SELECT},
    {NXINPUT_GPTK_UP, NXINPUT_GPTK4_UP}, {NXINPUT_GPTK_DOWN, NXINPUT_GPTK4_DOWN},
    {NXINPUT_GPTK_LEFT, NXINPUT_GPTK4_LEFT}, {NXINPUT_GPTK_RIGHT, NXINPUT_GPTK4_RIGHT},
  };
  static const char *const ctxname[NXINPUT_GPTK_CONTEXT_COUNT] = {"menu", "gameplay", "cursor"};
  int ctx; size_t i;
  if (!g4 || !out) return -1;
  memset(out, 0, sizeof *out);
  out->api_version = 1u;
  out->schema_version = NXINPUT_GPTK_SCHEMA_V4;
  snprintf(out->port, sizeof out->port, "%s", g4->port);
  nxinput_gptk_cursor_tuning_defaults(&out->cursor_tuning);
  nxinput_gptk_camera_tuning_defaults(&out->camera_tuning);
  out->face_layout = 0; /* AUTO: the Xbox surface is positional; the legend is evidence only */
  for (ctx = 0; ctx < NXINPUT_GPTK_CONTEXT_COUNT; ctx++) {
    const char *name = ctxname[ctx];
    unsigned o; int declared = 0;
    for (o = 0; o < g4->overrides; o++) if (!strcmp(g4->override[o].context, name)) declared = 1;
    out->context_present[ctx] = 1; /* unified: base reaches every context */
    (void)declared;
    for (i = 0; i < sizeof discrete / sizeof discrete[0]; i++)
      set_kind(out, ctx, discrete[i].v3, nxinput_gptk4_resolve(g4, name, discrete[i].v4), r);
    /* triggers */
    set_kind(out, ctx, NXINPUT_GPTK_L2, nxinput_gptk4_resolve(g4, name, g4->l2.mode == NXINPUT_GPTK4_TRIGGER_DIGITAL ? NXINPUT_GPTK4_L2_DIGITAL : NXINPUT_GPTK4_L2_ANALOG), r);
    set_kind(out, ctx, NXINPUT_GPTK_R2, nxinput_gptk4_resolve(g4, name, g4->r2.mode == NXINPUT_GPTK4_TRIGGER_DIGITAL ? NXINPUT_GPTK4_R2_DIGITAL : NXINPUT_GPTK4_R2_ANALOG), r);
    /* sticks: only the vector mode has a V3 sink */
    /* 0.11.6: the V3 runtime has no sink for the eight derived directions.
     * A stick in mode=digital|split used to be projected as NULL -- the
     * adapter then suppressed the native axis too and the stick was DEAD
     * with a receipt nobody reads. A policy this port cannot execute is
     * refused loudly (mission 7A.3: reject, never fake a fallback): the
     * file is rejected, the previous valid generation/default stays live. */
    if (g4->left.mode == NXINPUT_GPTK4_STICK_VECTOR) set_kind(out, ctx, NXINPUT_GPTK_LEFT_STICK, nxinput_gptk4_resolve(g4, name, NXINPUT_GPTK4_LS_VECTOR), r);
    else { if (ctx == 0) unsupported++; }
    if (g4->right.mode == NXINPUT_GPTK4_STICK_VECTOR) set_kind(out, ctx, NXINPUT_GPTK_RIGHT_STICK, nxinput_gptk4_resolve(g4, name, NXINPUT_GPTK4_RS_VECTOR), r);
    else { if (ctx == 0) unsupported++; }
  }
  if (unsupported) {
    if (r) { r->stick_digital_unsupported += unsupported; r->rc = -2; }
    if (r) snprintf(r->what, sizeof r->what, "stick mode digital/split has no sink in this port (V3 bridge): use mode=vector");
    memset(out, 0, sizeof *out);
    return -1;
  }
  if (r) {
    const nxinput_gptk4_binding *g = nxinput_gptk4_resolve(g4, "", NXINPUT_GPTK4_GUIDE);
    if (g && g->kind == NXINPUT_GPTK4_ACTION) r->guide_dropped++;
    r->extensions = g4->exts;
    r->digest = g4->digest;
  }
  return 0;
}

static int read_bounded_at(int dir_fd, const char *name, char *buf, size_t cap, size_t *len) {
  int fd = openat(dir_fd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat st; ssize_t got; size_t used = 0;
  if (fd < 0) return -1;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (size_t)st.st_size >= cap) { close(fd); return -2; }
  while (used < cap - 1) {
    got = read(fd, buf + used, cap - 1 - used);
    if (got < 0) { if (errno == EINTR) continue; close(fd); return -1; }
    if (got == 0) break;
    used += (size_t)got;
  }
  close(fd); buf[used] = '\0'; *len = used;
  return 0;
}

static int parse_one(const char *text, size_t len, const nxinput_gptk4_contract *c, nxinput_gptk4 *g4, nxinput_gptk4_bridge_receipt *r) {
  nxinput_gptk4_error e; memset(&e, 0, sizeof e);
  if (nxinput_gptk4_parse(text, len, c, g4, &e) != 0) {
    if (r) { r->rc = e.code; r->line = e.line; r->column = e.column; snprintf(r->what, sizeof r->what, "%s", e.what); }
    return -1;
  }
  if (r) { nxinput_sha256 ctx; uint8_t d[32]; nxinput_sha256_init(&ctx); nxinput_sha256_update(&ctx, text, len); nxinput_sha256_final(&ctx, d); nxinput_sha256_hex(d, r->sha256); }
  return 0;
}

int nxinput_gptk4_load_project_at(int owner_dir_fd, int defaults_dir_fd, const nxinput_gptk4_contract *contract,
                                  nxinput_gptk4 *g4_out, nxinput_gptk *live_out, nxinput_gptk4_bridge_receipt *r) {
  static char text[NXINPUT_GPTK4_MAX_BYTES + 1];
  nxinput_gptk4_bridge_receipt local; size_t len = 0; int have_default = 0;
  nxinput_gptk4 g4;
  if (!r) r = &local;
  memset(r, 0, sizeof *r);
  if (!contract || !g4_out || !live_out) { r->rc = -1; snprintf(r->what, sizeof r->what, "invalid arguments"); return -1; }
  memset(g4_out, 0, sizeof *g4_out); memset(live_out, 0, sizeof *live_out);
  /* 1. the immutable default */
  if (read_bounded_at(defaults_dir_fd, "NEXTOSCONTROLLERS.gptk", text, sizeof text, &len) == 0 &&
      parse_one(text, len, contract, &g4, r) == 0) {
    have_default = 1; *g4_out = g4; r->source = NXINPUT_GPTK4_SRC_DEFAULT;
  } else {
    r->default_rejected = 1;
  }
  /* 2. the owner's editable copy: wins when valid; never rewritten */
  if (read_bounded_at(owner_dir_fd, "NEXTOSCONTROLLERS.gptk", text, sizeof text, &len) == 0) {
    nxinput_gptk4_bridge_receipt keep = *r;
    r->owner_present = 1;
    if (parse_one(text, len, contract, &g4, r) == 0 && nxinput_gptk4_project(&g4, live_out, r) == 0) {
      *g4_out = g4; r->source = NXINPUT_GPTK4_SRC_OWNER; r->rc = 0; r->line = 0; r->column = 0; r->what[0] = '\0';
    } else {
      /* keep the default (or nothing); remember the owner diagnostics */
      int rc = r->rc; unsigned ln = r->line, col = r->column; char what[96]; memcpy(what, r->what, sizeof what);
      *r = keep; r->owner_present = 1; r->owner_rejected = 1; r->rc = rc; r->line = ln; r->column = col; memcpy(r->what, what, sizeof what);
    }
  }
  if (r->source == NXINPUT_GPTK4_SRC_NONE) { memset(live_out, 0, sizeof *live_out); return -1; }
  (void)have_default;
  return nxinput_gptk4_project(g4_out, live_out, r);
}

int nxinput_gptk4_bridge_receipt_json(const nxinput_gptk4_bridge_receipt *r, char *out, size_t cap) {
  static const char *const src[] = {"none", "default", "owner"};
  int n; size_t i; char what[96 * 2 + 1]; size_t w = 0;
  if (!r || !out || cap == 0) return -1;
  for (i = 0; r->what[i] && w + 2 < sizeof what; i++) { if (r->what[i] == '"' || r->what[i] == '\\') what[w++] = '\\'; what[w++] = r->what[i]; }
  what[w] = '\0';
  n = snprintf(out, cap, "{\"schema\":\"nxinput-gptk4-bridge/1\",\"marker\":\"%s\",\"source\":\"%s\",\"owner_present\":%u,\"owner_rejected\":%u,\"default_rejected\":%u,\"rc\":%d,\"line\":%u,\"column\":%u,\"what\":\"%s\",\"digest\":\"%016llx\",\"sha256\":\"%s\",\"guide_dropped\":%u,\"stick_digital_unsupported\":%u,\"key_outputs\":%u,\"extensions\":%u}",
               NXINPUT_GPTK4_RUNTIME_MARKER, src[r->source < 3 ? r->source : 0], r->owner_present, r->owner_rejected, r->default_rejected,
               r->rc, r->line, r->column, what, (unsigned long long)r->digest, r->sha256, r->guide_dropped, r->stick_digital_unsupported, r->key_outputs, r->extensions);
  return n < 0 || (size_t)n >= cap ? -1 : 0;
}
