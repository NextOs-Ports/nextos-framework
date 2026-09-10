/* SPDX-License-Identifier: GPL-3.0-only */
/* V5 / D4, D6, E5a, E7, E13: central lifecycle, neutral gate, hot reload,
 * synchronized CAS. Mutants of 8.3 killed here: focus/unplug with a held
 * edge; unknown/loading during a hold delivering to old owner AND new
 * passthrough; synchronized publishing one half; reconnect reusing instance. */
#include "../../include/nxinput_lifecycle.h"
#include <stdio.h>
#include <string.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static char trace[2048];
static void deliver(void *u, nxinput_lc_owner o, int inst, uint32_t gen, unsigned e, int p, uint32_t ce, uint32_t mg) { char b[64]; (void)u; snprintf(b, sizeof b, "%s:%d/%u:e%u%c@c%u/m%u ", o == NXINPUT_LC_OWNER_ACTIONS ? "ACT" : o == NXINPUT_LC_OWNER_ENGINE_NATIVE ? "NAT" : "NONE", inst, (unsigned)gen, e, p ? '+' : '-', (unsigned)ce, (unsigned)mg); strcat(trace, b); }
static int parse_owner(void *u, const char *t, uint64_t *d) { (void)u; if (!t || strstr(t, "BAD")) return -1; *d = (uint64_t)strlen(t) * 7919u; return 0; }
/* fake engine for E13 */
static struct { uint64_t applied, readback_override; int fail_apply, fail_rollback, owner_cas_conflict; uint64_t owner_file; } E;
static int e_apply(void *u, uint64_t d) { (void)u; if (E.fail_apply) return -1; E.applied = d; return 0; }
static int e_read(void *u, uint64_t *d) { (void)u; *d = E.readback_override ? E.readback_override : E.applied; return 0; }
static int e_roll(void *u, uint64_t d) { (void)u; if (E.fail_rollback) return -1; E.applied = d; return 0; }
static int o_cas(void *u, uint64_t exp, uint64_t nw) { (void)u; if (E.owner_cas_conflict || E.owner_file != exp) return -2; E.owner_file = nw; return 0; }
int main(void) {
  nxinput_lifecycle lc; nxinput_lc_ops ops = { NULL, deliver, parse_owner }; uint32_t g1, g2, g3;
  nxinput_lifecycle_init(&lc, &ops, NXINPUT_LC_FALLBACK_SUPPRESS, 0x11);
  g1 = nxinput_lifecycle_admit(&lc, 0, 0xAAAA);
  CHECK(g1 == 1 && nxinput_lifecycle_open(&lc, 0, g1) == 0, "admit + open under the admitted generation");
  CHECK(nxinput_lifecycle_open(&lc, 0, 99) == -1 && lc.refused_open_race == 1, "MUTANT killed: open naming a different generation than discovery (instance race) -> quarantined");
  nxinput_lifecycle_admit(&lc, 0, 0xAAAA); g1 = lc.dev[0].generation; nxinput_lifecycle_open(&lc, 0, g1);
  trace[0] = 0;
  CHECK(nxinput_lifecycle_edge(&lc, 0, g1, 3, 1) == -2 && lc.refused_gate == 1 && !strcmp(trace, ""), "context UNKNOWN at start: gate closed, press delivered to NO owner (fallback=suppress), nothing latched");
  nxinput_lifecycle_set_context(&lc, NXINPUT_LC_CTX_MENU);
  CHECK(lc.owner == NXINPUT_LC_OWNER_NONE && lc.neutral_pending, "menu context while the refused press is still physically down: gate stays closed");
  trace[0] = 0; CHECK(nxinput_lifecycle_edge(&lc, 0, g1, 3, 0) == 0 && !strcmp(trace, ""), "release of a never-delivered press: nothing delivered");
  CHECK(lc.owner == NXINPUT_LC_OWNER_ACTIONS && lc.context_epoch == 2 && !lc.neutral_pending, "menu context: gate opened once the source is neutral, owner = actions, epoch 2");
  trace[0] = 0; nxinput_lifecycle_edge(&lc, 0, g1, 3, 1);
  CHECK(!strcmp(trace, "ACT:0/2:e3+@c2/m1 "), "press delivered exactly once to actions with (context_epoch, mapping_generation)");
  /* MUTANT: switch to loading during the hold */
  trace[0] = 0; nxinput_lifecycle_set_context(&lc, NXINPUT_LC_CTX_LOADING);
  CHECK(!strcmp(trace, "ACT:0/2:e3-@c2/m1 ") && lc.neutral_pending && lc.owner == NXINPUT_LC_OWNER_NONE && lc.context_epoch == 3, "MUTANT killed: unknown/loading during a hold: release-all of the old epoch first, no passthrough, gate closed");
  trace[0] = 0; CHECK(nxinput_lifecycle_edge(&lc, 0, g1, 4, 1) == -2 && !strcmp(trace, ""), "while the gate is closed a new press reaches nobody (no old owner, no new passthrough)");
  /* edges 3 and 4 are still physically down: only their releases open the gate */
  nxinput_lifecycle_edge(&lc, 0, g1, 3, 0);
  CHECK(lc.neutral_pending, "one source still down (the refused press 4): gate still closed");
  nxinput_lifecycle_edge(&lc, 0, g1, 4, 0);
  CHECK(!lc.neutral_pending && lc.owner == NXINPUT_LC_OWNER_NONE, "neutral gate opened after the source went neutral; loading + declared fallback=suppress => no owner");
  /* declared fallback engine-native: only after the gate */
  nxinput_lifecycle_init(&lc, &ops, NXINPUT_LC_FALLBACK_ENGINE_NATIVE, 0x11);
  g1 = nxinput_lifecycle_admit(&lc, 0, 0xAAAA); nxinput_lifecycle_open(&lc, 0, g1);
  nxinput_lifecycle_set_context(&lc, NXINPUT_LC_CTX_GAMEPLAY); nxinput_lifecycle_edge(&lc, 0, g1, 5, 1);
  trace[0] = 0; nxinput_lifecycle_set_context(&lc, NXINPUT_LC_CTX_UNKNOWN);
  CHECK(strstr(trace, "ACT:0/1:e5-") && lc.owner == NXINPUT_LC_OWNER_NONE && lc.neutral_pending, "unknown during hold with fallback=engine-native: still NOT immediate passthrough");
  nxinput_lifecycle_edge(&lc, 0, g1, 5, 0);
  CHECK(lc.owner == NXINPUT_LC_OWNER_ENGINE_NATIVE, "after neutral: the DECLARED fallback (engine-native) activates");
  trace[0] = 0; nxinput_lifecycle_edge(&lc, 0, g1, 5, 1);
  CHECK(!strcmp(trace, "NAT:0/1:e5+@c3/m1 "), "then edges go to engine-native, exactly once");
  /* MUTANT: focus lost with an edge held */
  trace[0] = 0; nxinput_lifecycle_focus_lost(&lc);
  CHECK(!strcmp(trace, "NAT:0/1:e5-@c3/m1 ") && lc.dev[0].held[0] == 0 && nxinput_lifecycle_any_held(&lc), "MUTANT killed: focus lost with a button held -> release delivered, nothing latched (source still physically down)");
  nxinput_lifecycle_edge(&lc, 0, g1, 5, 0);
  CHECK(!nxinput_lifecycle_any_held(&lc), "physical release makes the source neutral");
  /* MUTANT: unplug with an edge held; reconnect reusing the instance id => new generation */
  nxinput_lifecycle_set_context(&lc, NXINPUT_LC_CTX_GAMEPLAY);
  nxinput_lifecycle_edge(&lc, 0, g1, 7, 1);
  trace[0] = 0; nxinput_lifecycle_remove(&lc, 0);
  CHECK(!strcmp(trace, "ACT:0/1:e7-@c5/m1 "), "MUTANT killed: unplug with a button held -> release delivered once");
  g2 = nxinput_lifecycle_admit(&lc, 0, 0xAAAA);
  CHECK(g2 == 2 && nxinput_lifecycle_open(&lc, 0, g2) == 0, "reconnect reusing instance 0: NEW device_instance_generation");
  CHECK(nxinput_lifecycle_edge(&lc, 0, g1, 7, 1) == -1 && lc.refused_stale_generation == 1, "MUTANT killed: press carrying the previous generation refused");
  /* second pad, then hot reload releases both before publishing */
  g3 = nxinput_lifecycle_admit(&lc, 1, 0xBBBB); nxinput_lifecycle_open(&lc, 1, g3);
  nxinput_lifecycle_edge(&lc, 0, g2, 1, 1); nxinput_lifecycle_edge(&lc, 1, g3, 2, 1);
  trace[0] = 0;
  CHECK(nxinput_lifecycle_hot_reload(&lc, "A = action:x") == 0 && lc.mapping_generation == 2 && strstr(trace, "ACT:0/2:e1-@c5/m1") && strstr(trace, "ACT:1/3:e2-@c5/m1"), "E7 hot reload: holds of the OLD generation released (with the old generation stamp) before publishing generation 2");
  CHECK(lc.neutral_pending && lc.owner == NXINPUT_LC_OWNER_NONE && lc.context_epoch == 6, "reload published in a new epoch; gate closed until the held sources go neutral");
  nxinput_lifecycle_edge(&lc, 0, g2, 1, 0); nxinput_lifecycle_edge(&lc, 1, g3, 2, 0);
  CHECK(!lc.neutral_pending && lc.owner == NXINPUT_LC_OWNER_ACTIONS, "sources neutral: gate reopened, owner = actions");
  CHECK(nxinput_lifecycle_hot_reload(&lc, "BAD text") == -1 && lc.mapping_generation == 2 && lc.reloads_failed == 1, "MUTANT killed: invalid owner text keeps the last valid mapping_generation");
  trace[0] = 0; nxinput_lifecycle_edge(&lc, 0, g2, 1, 1);
  CHECK(!strcmp(trace, "ACT:0/2:e1+@c6/m2 "), "edges after reload carry mapping_generation 2");
  /* overflow visible */
  { unsigned i, ok = 0; for (i = 2; i < NXINPUT_LC_MAX_DEVICES + 3; i++) ok += nxinput_lifecycle_admit(&lc, (int)i, 0x1000 + i) != 0; CHECK(ok == NXINPUT_LC_MAX_DEVICES - 2, "device slots exhausted: admit returns 0 (visible), never truncates silently"); }
  /* ---------------- E13 synchronized ---------------- */
  {
    nxinput_sync s; nxinput_sync_ops so = { NULL, e_apply, e_read, e_roll, o_cas }; nxinput_sync_result r;
    nxinput_lifecycle_init(&lc, &ops, NXINPUT_LC_FALLBACK_SUPPRESS, 0x100);
    memset(&E, 0, sizeof E); E.applied = 0x100; E.owner_file = 0x100;
    g1 = nxinput_lifecycle_admit(&lc, 0, 0xAAAA); nxinput_lifecycle_open(&lc, 0, g1); nxinput_lifecycle_set_context(&lc, NXINPUT_LC_CTX_MENU);
    nxinput_sync_init(&s, &so, &lc, 0x100);
    nxinput_lifecycle_edge(&lc, 0, g1, 2, 1); trace[0] = 0;
    r = nxinput_sync_owner_reload(&s, 1, 0x200);
    CHECK(r == NXINPUT_SYNC_PUBLISHED && E.applied == 0x200 && lc.mapping_generation == 2 && strstr(trace, "e2-"), "owner_reload: holds released, engine applied, readback equal -> published generation 2");
    E.readback_override = 0x999;
    r = nxinput_sync_owner_reload(&s, 2, 0x300);
    CHECK(r == NXINPUT_SYNC_REFUSED_READBACK && E.applied == 0x200 && lc.mapping_generation == 2 && s.owner_digest == 0x200, "MUTANT killed: readback differs -> engine rolled back, generation kept, owner not rewritten (no half publish)");
    E.readback_override = 0;
    CHECK(nxinput_sync_owner_reload(&s, 1, 0x300) == NXINPUT_SYNC_REFUSED_GENERATION, "stale expected generation refused");
    /* engine rebind by the user's UI: owner CAS */
    E.owner_file = 0x200;
    r = nxinput_sync_engine_rebind(&s, 0x200, 0x400);
    CHECK(r == NXINPUT_SYNC_PUBLISHED && E.owner_file == 0x400 && E.applied == 0x400 && lc.mapping_generation == 3, "engine_rebind_ui: engine + owner CAS + generation, all or nothing");
    r = nxinput_sync_engine_rebind(&s, 0x200, 0x500);
    CHECK(r == NXINPUT_SYNC_CONFLICT && E.owner_file == 0x400 && E.applied == 0x400 && lc.mapping_generation == 3, "MUTANT killed: owner changed in parallel (digest seen != current) -> CONFLICT, nothing written or published");
    E.owner_cas_conflict = 1;
    r = nxinput_sync_engine_rebind(&s, 0x400, 0x600);
    CHECK(r == NXINPUT_SYNC_CONFLICT && E.applied == 0x400 && E.owner_file == 0x400, "MUTANT killed: engine applied but owner CAS lost -> engine rolled back, no half state");
    E.owner_cas_conflict = 0; E.readback_override = 0x777; E.fail_rollback = 1;
    r = nxinput_sync_owner_reload(&s, 3, 0x700);
    CHECK(r == NXINPUT_SYNC_ROLLBACK_FAILED_RESTART && s.restarts == 1, "rollback impossible -> instance must restart before new edges");
    s.lease_held = 0; E.fail_rollback = 0; E.readback_override = 0;
    CHECK(nxinput_sync_owner_reload(&s, 3, 0x800) == NXINPUT_SYNC_REFUSED_LEASE, "without the exclusive lease nothing is applied");
  }
    /* ---------------- 0.11.1 C5: reopen / quarantine / hotplug generation ---- */
  {
    nxinput_lifecycle L; uint32_t g, hp;
    nxinput_lifecycle_init(&L, &ops, NXINPUT_LC_FALLBACK_SUPPRESS, 0x11);
    hp = nxinput_lifecycle_hotplug_generation(&L);
    g = nxinput_lifecycle_admit(&L, 5, 0xB0B);
    CHECK(nxinput_lifecycle_hotplug_generation(&L) == hp + 1, "admit bumps the hotplug generation");
    CHECK(nxinput_lifecycle_open_checked(&L, 5, g, hp) == -1 && L.refused_hotplug_race == 1 && L.dev[0].state == 2, "MUTANT killed: open after the device list changed under the caller (instance race by hotplug generation) -> refused, quarantined");
    CHECK(nxinput_lifecycle_open(&L, 5, g) == -1 && L.refused_quarantined == 1, "MUTANT killed: a quarantined device reopened under the right generation (refused until re-admitted)");
    g = nxinput_lifecycle_admit(&L, 5, 0xB0B); hp = nxinput_lifecycle_hotplug_generation(&L);
    CHECK(nxinput_lifecycle_open_checked(&L, 5, g, hp) == 0, "re-admitted device opens with the current hotplug generation");
    CHECK(nxinput_lifecycle_open(&L, 5, g) == -1 && L.refused_reopen == 1 && L.dev[0].state == 1, "MUTANT killed: reopen of an open device accepted (refused; state kept)");
    nxinput_lifecycle_set_context(&L, NXINPUT_LC_CTX_GAMEPLAY);
    nxinput_lifecycle_edge(&L, 5, g, 2, 1);
    CHECK(nxinput_lifecycle_open(&L, 5, g) == -1 && (L.dev[0].held[0] & 4u), "reopen during a hold does not drop the held edge");
    trace[0] = 0; nxinput_lifecycle_remove(&L, 5);
    CHECK(strstr(trace, "e2-") && nxinput_lifecycle_hotplug_generation(&L) == hp + 1, "remove releases the held edge and bumps the hotplug generation");
    /* remap during an open window: the reload releases holds BEFORE publishing; the device stays open */
    g = nxinput_lifecycle_admit(&L, 6, 0xC0C); nxinput_lifecycle_open(&L, 6, g); nxinput_lifecycle_set_context(&L, NXINPUT_LC_CTX_GAMEPLAY);
    nxinput_lifecycle_edge(&L, 6, g, 3, 1); trace[0] = 0;
    { uint32_t mg = L.mapping_generation;
      CHECK(nxinput_lifecycle_hot_reload(&L, "new owner text") == 0 && strstr(trace, "e3-@") && L.mapping_generation == mg + 1 && L.dev[0].state == 1 && L.neutral_pending, "remap while an edge is held: release under the OLD generation, new generation published, device still open, gate closed until neutral"); }
    nxinput_lifecycle_edge(&L, 6, g, 3, 0);
    trace[0] = 0; nxinput_lifecycle_edge(&L, 6, g, 3, 1);
    CHECK(strstr(trace, "/m") && strstr(trace, "e3+"), "after neutral: the press is delivered under the new mapping generation");
    { unsigned i; for (i = 0; i < NXINPUT_LC_MAX_DEVICES + 2; i++) nxinput_lifecycle_admit(&L, 100 + (int)i, 0xD00 + i);
      CHECK(L.admits_overflow >= 1, "MUTANT killed: admit beyond the table truncating silently (counted)"); }
  }
  printf(fails ? "v5-lifecycle: FAIL\n" : "v5-lifecycle: OK\n"); return fails ? 1 : 0;
}
