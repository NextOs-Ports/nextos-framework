/* SPDX-License-Identifier: GPL-3.0-only */
/* V5 / D7 (5.6): sovereign START/SELECT pre-router. Chord consumes both
 * without letting START pause first; taps/holds forward exactly once;
 * cross-pad, stale generation, auto-repeat, focus/unplug never latch. */
#include "../../include/nxinput_prerouter.h"
#include "../../include/nxinput_decision.h"
#include <stdio.h>
#include <string.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static char trace[1024];
static void fwd(void *u, int inst, uint32_t gen, nxinput_prerouter_edge e, int pressed, uint64_t id) { char b[64]; (void)u; (void)id; snprintf(b, sizeof b, "%s%d/%u%c ", e == NXINPUT_PREROUTER_START ? "START" : "SELECT", inst, (unsigned)gen, pressed ? '+' : '-'); strcat(trace, b); }
static void sysexit(void *u, int inst, uint32_t gen, uint64_t id) { char b[64]; (void)u; (void)id; snprintf(b, sizeof b, "EXIT%d/%u ", inst, (unsigned)gen); strcat(trace, b); }
#define MS(x) ((uint64_t)(x) * 1000000ull)
static nxinput_prerouter P; static nxinput_prerouter_ops OPS = { NULL, fwd, sysexit };
#define SEL NXINPUT_PREROUTER_SELECT
#define STA NXINPUT_PREROUTER_START
static void reset(void) { nxinput_prerouter_init(&P, &OPS, MS(180)); trace[0] = 0; }
int main(void) {
  CHECK(nxinput_prerouter_init(&P, &OPS, MS(5000)) == -1, "window above 1 s refused (bounded)");
  /* 1. START tap alone: forwarded exactly once (press then release), after the window */
  reset();
  nxinput_prerouter_event(&P, 1, 1, STA, 1, MS(0));
  CHECK(!strcmp(trace, "") && nxinput_prerouter_holding(&P, 1, STA), "MUTANT killed: START leaking pause before the pre-router saw SELECT (press retained, owner=SUPPRESSED)");
  CHECK(nxinput_decision_edge_owner(NXINPUT_AUTHORITY_NEXTOS, NXINPUT_EDGE_ACTION, nxinput_prerouter_holding(&P, 1, STA)) == NXINPUT_OWNER_SUPPRESSED, "edge owner during hold = suppressed");
  nxinput_prerouter_event(&P, 1, 1, STA, 0, MS(50));
  nxinput_prerouter_tick(&P, MS(100));
  CHECK(!strcmp(trace, ""), "tap inside the window still retained (SELECT may still arrive)");
  nxinput_prerouter_tick(&P, MS(200));
  /* 0.11.5 (field, FP2 03/09): the press goes out when the window ends and
   * the release only on the NEXT tick -- a per-frame sampler must see the
   * button down in at least one sample; before, press+release left in one
   * tick and every tap shorter than the window was invisible to the game. */
  CHECK(!strcmp(trace, "START1/1+ ") && P.forwarded_press == 1 && P.forwarded_release == 0, "MUTANT killed: tap's press and release flushed in the SAME tick (no sample ever sees the button down)");
  nxinput_prerouter_tick(&P, MS(216));
  CHECK(!strcmp(trace, "START1/1+ START1/1- ") && P.forwarded_press == 1 && P.forwarded_release == 1, "MUTANT killed: tap lost or duplicated after timeout (forwarded exactly once, press then release, one tick apart)");
  nxinput_prerouter_tick(&P, MS(900));
  CHECK(!strcmp(trace, "START1/1+ START1/1- "), "no duplicate after later ticks (no latch, slot freed)");
  /* fast double tap: second press arrives while the first release is deferred */
  reset();
  nxinput_prerouter_event(&P, 1, 1, STA, 1, MS(0)); nxinput_prerouter_event(&P, 1, 1, STA, 0, MS(50));
  nxinput_prerouter_tick(&P, MS(200));
  nxinput_prerouter_event(&P, 1, 1, STA, 1, MS(205)); nxinput_prerouter_event(&P, 1, 1, STA, 0, MS(260));
  nxinput_prerouter_tick(&P, MS(216)); nxinput_prerouter_tick(&P, MS(400)); nxinput_prerouter_tick(&P, MS(416));
  CHECK(!strcmp(trace, "START1/1+ START1/1- START1/1+ START1/1- ") && P.forwarded_press == 2 && P.forwarded_release == 2, "MUTANT killed: second tap during the deferred release lost (both taps forwarded, in order)");
  /* 2. START hold: forwarded once at window end, release forwarded once when it comes */
  reset();
  nxinput_prerouter_event(&P, 1, 1, STA, 1, MS(0));
  nxinput_prerouter_tick(&P, MS(181));
  CHECK(!strcmp(trace, "START1/1+ "), "hold: press forwarded once when window ends");
  nxinput_prerouter_event(&P, 1, 1, STA, 2, MS(300)); /* auto-repeat */
  CHECK(!strcmp(trace, "START1/1+ ") && P.repeats_ignored == 1, "auto-repeat ignored");
  nxinput_prerouter_event(&P, 1, 1, STA, 0, MS(500));
  CHECK(!strcmp(trace, "START1/1+ START1/1- "), "hold release forwarded exactly once");
  /* 3. chord SELECT then START inside window: both consumed, one exit, START never pauses */
  reset();
  nxinput_prerouter_event(&P, 1, 1, SEL, 1, MS(0));
  nxinput_prerouter_event(&P, 1, 1, STA, 1, MS(90));
  CHECK(!strcmp(trace, "EXIT1/1 ") && P.exits == 1, "SELECT+START same instance: exactly one exit, no START forwarded");
  nxinput_prerouter_event(&P, 1, 1, STA, 0, MS(150)); nxinput_prerouter_event(&P, 1, 1, SEL, 0, MS(160));
  nxinput_prerouter_tick(&P, MS(1000));
  CHECK(!strcmp(trace, "EXIT1/1 ") && P.forwarded_press == 0 && P.forwarded_release == 0, "chord releases swallowed; nothing else forwarded");
  /* 4. chord START then SELECT (other order) */
  reset();
  nxinput_prerouter_event(&P, 1, 1, STA, 1, MS(0));
  nxinput_prerouter_event(&P, 1, 1, SEL, 1, MS(170));
  CHECK(!strcmp(trace, "EXIT1/1 ") && P.exits == 1, "START then SELECT inside window: one exit");
  /* 5. simultaneous press (same timestamp) */
  reset();
  nxinput_prerouter_event(&P, 1, 1, SEL, 1, MS(10)); nxinput_prerouter_event(&P, 1, 1, STA, 1, MS(10));
  CHECK(!strcmp(trace, "EXIT1/1 ") && P.exits == 1, "simultaneous press: one exit");
  /* 6. second edge AFTER window: first is forwarded (in order), no chord */
  reset();
  nxinput_prerouter_event(&P, 1, 1, SEL, 1, MS(0));
  nxinput_prerouter_event(&P, 1, 1, STA, 1, MS(400));
  CHECK(!strcmp(trace, "SELECT1/1+ ") && P.exits == 0, "START after SELECT's window: SELECT forwarded first, START retained, no exit");
  nxinput_prerouter_tick(&P, MS(600));
  CHECK(!strcmp(trace, "SELECT1/1+ START1/1+ "), "then START forwarded once; order coherent");
  nxinput_prerouter_event(&P, 1, 1, SEL, 0, MS(700)); nxinput_prerouter_event(&P, 1, 1, STA, 0, MS(710));
  CHECK(!strcmp(trace, "SELECT1/1+ START1/1+ SELECT1/1- START1/1- "), "both releases forwarded exactly once");
  /* 7. cross-pad: SELECT on 1 + START on 2 never chords */
  reset();
  nxinput_prerouter_event(&P, 1, 1, SEL, 1, MS(0)); nxinput_prerouter_event(&P, 2, 7, STA, 1, MS(50));
  nxinput_prerouter_tick(&P, MS(500));
  CHECK(P.exits == 0 && strstr(trace, "SELECT1/1+") && strstr(trace, "START2/7+"), "MUTANT killed: SELECT of one pad + START of another forming a chord (no exit; each edge to its own instance)");
  /* 8. stale generation: instance 1 reconnected (gen 2) while SELECT of gen 1 was retained */
  reset();
  nxinput_prerouter_event(&P, 1, 1, SEL, 1, MS(0));
  nxinput_prerouter_event(&P, 1, 2, STA, 1, MS(50));
  CHECK(P.exits == 0 && P.stale_generation_refused == 1 && P.dropped == 1 && !strcmp(trace, ""), "MUTANT killed: press of a previous device_instance_generation completing a chord after reconnect (dropped)");
  nxinput_prerouter_tick(&P, MS(300));
  CHECK(!strcmp(trace, "START1/2+ "), "new generation's START forwarded on its own");
  /* 9. focus lost while START retained: dropped, never forwarded; while forwarded: release forwarded */
  reset();
  nxinput_prerouter_event(&P, 1, 1, STA, 1, MS(0));
  nxinput_prerouter_release_all(&P, MS(10));
  CHECK(!strcmp(trace, "") && P.dropped == 1 && !nxinput_prerouter_holding(&P, 1, STA), "focus lost during retention: press dropped, no latch");
  reset();
  nxinput_prerouter_event(&P, 1, 1, STA, 1, MS(0)); nxinput_prerouter_tick(&P, MS(200));
  nxinput_prerouter_unplug(&P, 1, MS(250));
  CHECK(!strcmp(trace, "START1/1+ START1/1- "), "unplug after forwarded press: release forwarded once");
  nxinput_prerouter_event(&P, 1, 1, STA, 0, MS(300));
  CHECK(!strcmp(trace, "START1/1+ START1/1- "), "late release after unplug: nothing (slot gone)");
  /* 10. press from a previous generation + chord attempt after reconnect within window */
  reset();
  nxinput_prerouter_event(&P, 3, 1, STA, 1, MS(0));
  nxinput_prerouter_unplug(&P, 3, MS(20));
  nxinput_prerouter_event(&P, 3, 2, SEL, 1, MS(40));
  nxinput_prerouter_tick(&P, MS(400));
  CHECK(P.exits == 0 && !strcmp(trace, "SELECT3/2+ "), "START of the unplugged generation cannot chord with SELECT of the new one");
  /* 11. L2+R2 is not this router's business: only SELECT/START edges exist here (compile-time) */
  /* 12. (0.11.1, review finding 8) SELECT tap then START press inside the window: NO chord */
  reset();
  nxinput_prerouter_event(&P, 1, 1, SEL, 1, MS(0));
  nxinput_prerouter_event(&P, 1, 1, SEL, 0, MS(40));
  nxinput_prerouter_event(&P, 1, 1, STA, 1, MS(90));
  /* 0.11.5: the completed SELECT tap is flushed at once (press now, release
   * on a later tick) and START stays retained -- no chord. */
  CHECK(P.exits == 0 && !strcmp(trace, "SELECT1/1+ ") && nxinput_prerouter_holding(&P, 1, STA), "MUTANT killed: SELECT tap + START press forming a chord (no exit; the tap is forwarded, START retained)");
  /* 0.11.6 (field, FP2 03/09): the padset ticks in the SAME sample as the
   * event that flushed the press; that tick must not pay the release yet,
   * or the sample sees press+release and the SELECT tap is invisible. */
  nxinput_prerouter_tick(&P, MS(100));
  CHECK(!strcmp(trace, "SELECT1/1+ ") && P.forwarded_release == 0, "MUTANT killed: release owed by an event-flushed tap paid by the tick of the same sample (SELECT tap invisible to a per-frame consumer)");
  nxinput_prerouter_tick(&P, MS(116));
  CHECK(!strcmp(trace, "SELECT1/1+ SELECT1/1- "), "the owed release lands on the following tick");
  nxinput_prerouter_event(&P, 1, 1, STA, 0, MS(120)); nxinput_prerouter_tick(&P, MS(400));
  CHECK(!strcmp(trace, "SELECT1/1+ SELECT1/1- START1/1+ ") && P.exits == 0, "then the START tap is forwarded once (release deferred one tick); still no exit");
  nxinput_prerouter_tick(&P, MS(416));
  CHECK(!strcmp(trace, "SELECT1/1+ SELECT1/1- START1/1+ START1/1- ") && P.exits == 0, "both taps complete, in order, exactly once");
  /* 13. (0.11.1) double tap of START inside the window: BOTH taps forwarded */
  reset();
  nxinput_prerouter_event(&P, 1, 1, STA, 1, MS(0)); nxinput_prerouter_event(&P, 1, 1, STA, 0, MS(50));
  nxinput_prerouter_event(&P, 1, 1, STA, 1, MS(100));
  nxinput_prerouter_tick(&P, MS(100)); /* the padset ticks right after the event, same sample */
  CHECK(!strcmp(trace, "START1/1+ ") && P.forwarded_release == 0, "MUTANT killed: first tap of a double tap flushed press+release inside one sample (invisible to the game)");
  nxinput_prerouter_event(&P, 1, 1, STA, 0, MS(150));
  nxinput_prerouter_tick(&P, MS(1000)); nxinput_prerouter_tick(&P, MS(1016));
  CHECK(!strcmp(trace, "START1/1+ START1/1- START1/1+ START1/1- ") && P.forwarded_press == 2 && P.forwarded_release == 2, "MUTANT killed: second tap of a double tap lost (both taps forwarded, in order)");
  /* 14. (0.11.1) START then SELECT both held past the window: flushed in press order */
  reset();
  nxinput_prerouter_event(&P, 1, 1, STA, 1, MS(0));
  nxinput_prerouter_tick(&P, MS(190)); /* START's window over: forwarded */
  nxinput_prerouter_event(&P, 1, 1, SEL, 1, MS(200));
  nxinput_prerouter_tick(&P, MS(400));
  CHECK(!strcmp(trace, "START1/1+ SELECT1/1+ "), "START forwarded before SELECT when pressed first (coherent order)");
  reset();
  nxinput_prerouter_event(&P, 2, 1, STA, 1, MS(0)); nxinput_prerouter_event(&P, 2, 1, SEL, 1, MS(300));
  CHECK(!strcmp(trace, "START2/1+ "), "second edge after the first's window: first flushed first");
  reset();
  nxinput_prerouter_event(&P, 1, 1, STA, 1, MS(0)); nxinput_prerouter_event(&P, 1, 1, SEL, 1, MS(500));
  nxinput_prerouter_tick(&P, MS(1000));
  CHECK(!strcmp(trace, "START1/1+ SELECT1/1+ "), "MUTANT killed: expired edges flushed SELECT-first regardless of press time (press-time order kept)");
  /* 15. (0.11.1) instances beyond the slot table: forwarded individually, counted, never chord */
  reset();
  { int i; for (i = 0; i < (int)NXINPUT_PREROUTER_MAX_INSTANCES; i++) nxinput_prerouter_event(&P, 100 + i, 1, STA, 1, MS(0)); }
  trace[0] = 0; nxinput_prerouter_event(&P, 999, 1, SEL, 1, MS(10)); nxinput_prerouter_event(&P, 999, 1, STA, 1, MS(20));
  CHECK(P.overflow_refused == 2 && !strcmp(trace, "SELECT999/1+ START999/1+ ") && P.exits == 0, "MUTANT killed: silent truncation beyond the slot table (edges forwarded, refusal counted, no chord)");
  /* ---------------- 0.11.1 D5: the physical graph ---------------------------- */
  /* 16. SDL2 and raw evdev both see the SAME physical pad: one owner (declared
   * priority), the other observes; SELECT+START through BOTH routes = ONE exit. */
  reset();
  CHECK(nxinput_prerouter_bind_physical(&P, 10, 1, 0xABC, NXINPUT_PREROUTER_PATH_SDL2, 2, 1) == 1, "SDL2 path bound: owner of physical 0xABC (priority 2)");
  CHECK(nxinput_prerouter_bind_physical(&P, 20, 1, 0xABC, NXINPUT_PREROUTER_PATH_RAW_EVDEV, 1, 1) == 0 && nxinput_prerouter_owner_of(&P, 0xABC) == 10, "raw evdev path bound to the same pad: observer (lower priority)");
  nxinput_prerouter_event(&P, 20, 1, SEL, 1, MS(0)); nxinput_prerouter_event(&P, 20, 1, STA, 1, MS(10)); /* raw route sees the chord */
  nxinput_prerouter_event(&P, 10, 1, SEL, 1, MS(2)); nxinput_prerouter_event(&P, 10, 1, STA, 1, MS(12)); /* SDL route sees it too */
  CHECK(P.exits == 1 && !strcmp(trace, "EXIT10/1 ") && P.observer_dropped == 2, "MUTANT killed: two routes generating two shutdowns (exactly ONE exit, from the owner; observer edges dropped)");
  nxinput_prerouter_event(&P, 20, 1, SEL, 0, MS(50)); nxinput_prerouter_event(&P, 20, 1, STA, 0, MS(50));
  nxinput_prerouter_event(&P, 10, 1, SEL, 0, MS(52)); nxinput_prerouter_event(&P, 10, 1, STA, 0, MS(52));
  nxinput_prerouter_tick(&P, MS(1000));
  CHECK(P.exits == 1 && P.forwarded_press == 0, "releases through both routes: nothing forwarded, no second exit");
  /* 17. observer START alone never pauses the game through the second route */
  trace[0] = 0; nxinput_prerouter_event(&P, 20, 1, STA, 1, MS(1100)); nxinput_prerouter_tick(&P, MS(1400));
  CHECK(!strcmp(trace, "") && P.observer_dropped == 5, "observer route's lone START: dropped (the owner route delivers it)");
  nxinput_prerouter_event(&P, 20, 1, STA, 0, MS(1450));
  /* 18. two DIFFERENT physical pads: independent owners, cross-pad still no chord */
  CHECK(nxinput_prerouter_bind_physical(&P, 30, 1, 0xDEF, NXINPUT_PREROUTER_PATH_SDL2, 2, 1) == 1, "second physical pad: its own owner");
  trace[0] = 0; nxinput_prerouter_event(&P, 10, 1, SEL, 1, MS(2000)); nxinput_prerouter_event(&P, 30, 1, STA, 1, MS(2010)); nxinput_prerouter_tick(&P, MS(2500));
  CHECK(P.exits == 1 && strstr(trace, "SELECT10/1+") && strstr(trace, "START30/1+"), "SELECT on pad A + START on pad B (both owners): no chord, each forwarded");
  nxinput_prerouter_event(&P, 10, 1, SEL, 0, MS(2600)); nxinput_prerouter_event(&P, 30, 1, STA, 0, MS(2600));
  /* 19. uncertified SELECT (unknown provider, no physical profile) never chords */
  reset();
  CHECK(nxinput_prerouter_bind_physical(&P, 40, 1, 0x111, NXINPUT_PREROUTER_PATH_SDL2, 1, 0) == 1, "path bound with SELECT NOT certified");
  nxinput_prerouter_event(&P, 40, 1, SEL, 1, MS(0)); nxinput_prerouter_event(&P, 40, 1, STA, 1, MS(20)); nxinput_prerouter_tick(&P, MS(400));
  CHECK(P.exits == 0 && P.uncertified_select_refused == 1 && strstr(trace, "SELECT40/1+") && strstr(trace, "START40/1+"), "MUTANT killed: chord formed from a SELECT the physical profile never certified (no exit; individual presses forwarded)");
  /* 20. reconnect of the owner under a new generation: election re-run; a higher-priority late binder takes over and the old owner's retained edge is dropped */
  reset();
  nxinput_prerouter_bind_physical(&P, 50, 1, 0x222, NXINPUT_PREROUTER_PATH_RAW_EVDEV, 1, 1);
  nxinput_prerouter_event(&P, 50, 1, STA, 1, MS(0));
  CHECK(nxinput_prerouter_bind_physical(&P, 51, 1, 0x222, NXINPUT_PREROUTER_PATH_SDL2, 5, 1) == 1 && nxinput_prerouter_owner_of(&P, 0x222) == 51 && P.dropped == 1, "higher-priority path elected later becomes owner; the old owner's retained START is dropped (never delivered twice)");
  nxinput_prerouter_unplug(&P, 51, MS(100));
  CHECK(nxinput_prerouter_owner_of(&P, 0x222) == -1, "unplug of the owner leaves the physical pad without owner until re-election");
  CHECK(nxinput_prerouter_bind_physical(&P, 50, 2, 0x222, NXINPUT_PREROUTER_PATH_RAW_EVDEV, 1, 1) == 1, "the remaining path re-elected under its new generation");
  /* 21. tie between two paths of equal priority: first bound stays owner, counted as ambiguous */
  reset();
  nxinput_prerouter_bind_physical(&P, 60, 1, 0x333, NXINPUT_PREROUTER_PATH_SDL2, 1, 1);
  CHECK(nxinput_prerouter_bind_physical(&P, 61, 1, 0x333, NXINPUT_PREROUTER_PATH_SDL3, 1, 1) == 0 && P.election_ambiguous == 1 && nxinput_prerouter_owner_of(&P, 0x333) == 60, "equal priority tie: first bound stays owner, ambiguity counted (fail-closed evidence, no double delivery)");
  printf(fails ? "v5-prerouter: FAIL\n" : "v5-prerouter: OK\n"); return fails ? 1 : 0;
}
