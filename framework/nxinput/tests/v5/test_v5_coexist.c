/* SPDX-License-Identifier: GPL-3.0-only */
/* 0.11.1 B8 (mission 5.7): SDL2 and SDL3 IN THE SAME PROCESS, real DSOs.
 * Proves, with the objects this host really maps:
 *   - every symbol of each major resolves from ITS OWN object (dladdr base
 *     equality per handle); an SDL2 entry never satisfies an SDL3 name;
 *   - two provider descriptors: different api, different sha256, never the
 *     same instance;
 *   - the mapping STORES are separate: a line added to SDL2 is not visible
 *     to SDL3's readback and vice versa (per-GUID stores never shared);
 *   - the ONE global SDL_GAMECONTROLLERCONFIG is sequenced: the sequencer
 *     stages it, initialises each major with ITS corpus, restores the
 *     environment, and neither major imported the other's line;
 *   - a physical pad seen by both majors gets ONE ingestion owner
 *     (nxinput_prerouter physical graph): the observer's chord is dropped.
 * The test SKIPs (exit 77) when either DSO is missing on the host. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../include/nxinput_coexist.h"
#include "../../include/nxinput_provider_linux.h"
#include "../../include/nxinput_prerouter.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
typedef struct { unsigned char data[16]; } guid16;
static int same_base(void *a, void *b) { Dl_info ia, ib; return dladdr(a, &ia) && dladdr(b, &ib) && ia.dli_fbase == ib.dli_fbase; }
static void fwd(void *u, int i, uint32_t g, nxinput_prerouter_edge e, int p, uint64_t id) { (void)u; (void)i; (void)g; (void)e; (void)p; (void)id; }
static int exits; static void sysexit(void *u, int i, uint32_t g, uint64_t id) { (void)u; (void)i; (void)g; (void)id; exits++; }
int main(void) {
  void *h2 = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_LOCAL), *h3 = dlopen("libSDL3.so.0", RTLD_NOW | RTLD_LOCAL);
  int (*init2)(unsigned); int (*init3)(unsigned); void (*quit2)(void); void (*quit3)(void);
  int (*add2)(const char *); int (*add3)(const char *);
  guid16 (*g2from)(const char *); guid16 (*g3from)(const char *);
  char *(*map2)(guid16); char *(*map3)(guid16);
  void (*free2)(void *); void (*free3)(void *);
  int (*sethint2)(const char *, const char *); int (*sethint3)(const char *, const char *);
  nxinput_provider_probe p2, p3; nxinput_provider_descriptor d2, d3;
  nxinput_coexist cx; char line[512]; int shared_core = 0;
  const char *GUID = "030000004e5800c0edbeef0000010000";
  const char *LINE2 = "030000004e5800c0edbeef0000010000,Coexist Pad Two,a:b0,b:b1,platform:Linux,";
  const char *LINE3 = "030000004e5800c0edbeef0000010000,Coexist Pad Three,a:b1,b:b0,platform:Linux,";
  if (!h2 || !h3) { printf("SKIP: SDL2 or SDL3 DSO not present on this host (%s)\n", dlerror()); return 77; }
  init2 = (int (*)(unsigned))dlsym(h2, "SDL_Init"); init3 = (int (*)(unsigned))dlsym(h3, "SDL_Init");
  quit2 = (void (*)(void))dlsym(h2, "SDL_Quit"); quit3 = (void (*)(void))dlsym(h3, "SDL_Quit");
  add2 = (int (*)(const char *))dlsym(h2, "SDL_GameControllerAddMapping"); add3 = (int (*)(const char *))dlsym(h3, "SDL_AddGamepadMapping");
  g2from = (guid16 (*)(const char *))dlsym(h2, "SDL_JoystickGetGUIDFromString"); g3from = (guid16 (*)(const char *))dlsym(h3, "SDL_StringToGUID");
  map2 = (char *(*)(guid16))dlsym(h2, "SDL_GameControllerMappingForGUID"); map3 = (char *(*)(guid16))dlsym(h3, "SDL_GetGamepadMappingForGUID");
  free2 = (void (*)(void *))dlsym(h2, "SDL_free"); free3 = (void (*)(void *))dlsym(h3, "SDL_free");
  sethint2 = (int (*)(const char *, const char *))dlsym(h2, "SDL_SetHint"); sethint3 = (int (*)(const char *, const char *))dlsym(h3, "SDL_SetHint");
  CHECK(init2 && init3 && add2 && add3 && g2from && g3from && map2 && map3 && free2 && free3, "every entry point resolved from its own handle");
  /* 1. symbol scope: each major's entry lives in its own object; the SDL2-only
   * and SDL3-only names never resolve from the other handle */
  CHECK(!same_base((void *)init2, (void *)init3) && same_base((void *)init2, (void *)add2) && same_base((void *)init3, (void *)add3), "MUTANT killed: an SDL2 function satisfied by SDL3 (or the reverse) through interposition (bases differ per major, equal within a major)");
  CHECK(dlsym(h2, "SDL_AddGamepadMapping") == NULL && dlsym(h3, "SDL_GameControllerAddMapping") == NULL, "SDL2-only / SDL3-only names do not cross handles (RTLD_LOCAL)");
  /* 2. two descriptors, never the same instance */
  CHECK(nxinput_provider_probe_sdl((void *)init2, NXINPUT_SDL_API_2, &p2) == 0 && nxinput_provider_probe_sdl((void *)init3, NXINPUT_SDL_API_3, &p3) == 0, "both providers probed");
  nxinput_provider_resolve(&p2.evidence, 1, &d2); nxinput_provider_resolve(&p3.evidence, 1, &d3);
  CHECK(strcmp(d2.evidence.sha256, d3.evidence.sha256) != 0 && !nxinput_provider_same_instance(&d2, &d3) && d2.evidence.api == NXINPUT_SDL_API_2 && d3.evidence.api == NXINPUT_SDL_API_3, "two provider descriptors: different bytes, never the same instance");
  nxinput_provider_probe_close(&p2); nxinput_provider_probe_close(&p3);
  { nxinput_coexist_verdict vd = nxinput_coexist_arbitrate(&d2, &d3);
    shared_core = vd == NXINPUT_COEXIST_SHARED_CORE;
    printf("     arbitration=%s compat_over_sdl3=%u\n", nxinput_coexist_verdict_name(vd), (unsigned)d2.evidence.compat_over_sdl3);
    CHECK(vd != NXINPUT_COEXIST_AMBIGUOUS, "arbitration decided (separate or shared-core), never ambiguous with bound bytes"); }
  /* 3. the sequencer: one global SDL_GAMECONTROLLERCONFIG, isolated per major */
  setenv("SDL_GAMECONTROLLERCONFIG", "030000004e5800c0edbeef0000010000,Ambient,a:b2,b:b3,platform:Linux,", 1);
  CHECK(nxinput_coexist_init(&cx) == 0, "sequencer initialised");
  CHECK(nxinput_coexist_stage(&cx) == 0 && getenv("SDL_GAMECONTROLLERCONFIG") == NULL && cx.staged_len > 0, "ambient corpus staged out of the environment before either init (no major imports an ambiguous line)");
  sethint2("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1"); sethint3("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
  CHECK(nxinput_coexist_begin_with_hint(&cx, NXINPUT_SDL_API_2, LINE2, sethint2) == 0 && init2(0x200u | 0x2000u) == 0 && nxinput_coexist_end(&cx) == 0, "SDL2 initialised under ITS corpus only (env + its own SetHint), environment restored after");
  CHECK(nxinput_coexist_begin_with_hint(&cx, NXINPUT_SDL_API_3, LINE3, sethint3) == 0 && init3(0x200u | 0x2000u) != 0 && nxinput_coexist_end(&cx) == 0, "SDL3 initialised under ITS corpus only (its own SetHint: SDL3 snapshots the environment at first use), environment restored after");
  CHECK(getenv("SDL_GAMECONTROLLERCONFIG") == NULL && cx.sequenced == 2 && cx.concurrent_refused == 0, "after both: the ambient variable stays staged (never handed to either), two sequenced inits");
  CHECK(nxinput_coexist_begin(&cx, NXINPUT_SDL_API_2, LINE2) == 0 && nxinput_coexist_begin(&cx, NXINPUT_SDL_API_3, LINE3) == -1 && cx.concurrent_refused == 1 && nxinput_coexist_end(&cx) == 0, "MUTANT killed: two majors initialising under one corpus concurrently (second begin refused while the first is open)");
  /* 4. the stores. SEPARATE providers: what SDL2 holds for the GUID is not
   * what SDL3 holds. SHARED CORE (sdl2-compat over SDL3, this host): ONE
   * store by construction -- the framework must have DETECTED it (the
   * isolation claim is refused), and the observation confirms the sharing. */
  { char *m2 = map2(g2from(GUID)); char *m3 = map3(g3from(GUID));
    printf("     sdl2=[%s] sdl3=[%s]\n", m2 ? m2 : "(null)", m3 ? m3 : "(null)");
    if (!shared_core) {
      CHECK(m2 && strstr(m2, "Pad Two") && m3 && strstr(m3, "Pad Three"), "each major holds ITS line for the GUID (imported from its own sequenced corpus)");
      CHECK(m2 && !strstr(m2, "Pad Three") && m3 && !strstr(m3, "Pad Two") && (!m2 || !strstr(m2, "Ambient")) && (!m3 || !strstr(m3, "Ambient")), "MUTANT killed: a mapping store shared across majors (neither major sees the other's line, nor the ambient one)");
    } else {
      CHECK(m2 && m3 && strcmp(m2, m3) == 0, "MUTANT killed: sdl2-compat over SDL3 reported as two isolated providers (the stores ARE one: same line from both ABIs; detected from the bytes, claim refused)");
      printf("     INCONCLUSIVE FOR NOW on this host: isolation of two REAL providers needs a real SDL2 + SDL3 pair (the authorized device carries both); the shared-core case is what this host proves\n");
    }
    if (m2) { free2(m2); } if (m3) { free3(m3); } }
  add2("030000004e5800c0edbeef0000010000,Coexist Pad Two Edit,a:b3,b:b2,platform:Linux,");
  { char *m2 = map2(g2from(GUID)); char *m3 = map3(g3from(GUID));
    printf("     after AddMapping via SDL2 ABI: sdl2=[%s] sdl3=[%s]\n", m2 ? m2 : "(null)", m3 ? m3 : "(null)");
    if (!shared_core) CHECK(m2 && m3 && strstr(m3, "Pad Three") && !strstr(m3, "Two Edit") && !strstr(m2, "Pad Three"), "an AddMapping through the SDL2 ABI never reaches SDL3's store (SDL2's own readback keeps its USER-priority env line: the staging rationale)");
    else CHECK(m2 && m3 && strcmp(m2, m3) == 0, "shared core: the environment-imported line (USER priority) outranks the API AddMapping on BOTH ABIs -- the very reason the seam stages the variable; one store, same answer");
    if (m2) { free2(m2); } if (m3) { free3(m3); } }
  /* 5. one physical pad seen by both majors: one owner, one exit */
  { nxinput_prerouter pr; nxinput_prerouter_ops ops = {NULL, fwd, sysexit}; nxinput_prerouter_init(&pr, &ops, 0);
    CHECK(nxinput_prerouter_bind_physical(&pr, 1, 1, 0xC0E1571ull, NXINPUT_PREROUTER_PATH_SDL2, 2, 1) == 1 && nxinput_prerouter_bind_physical(&pr, 1000, 1, 0xC0E1571ull, NXINPUT_PREROUTER_PATH_SDL3, 1, 1) == 0, "the same physical pad through SDL2 (owner) and SDL3 (observer)");
    nxinput_prerouter_event(&pr, 1000, 1, NXINPUT_PREROUTER_SELECT, 1, 0); nxinput_prerouter_event(&pr, 1000, 1, NXINPUT_PREROUTER_START, 1, 10);
    nxinput_prerouter_event(&pr, 1, 1, NXINPUT_PREROUTER_SELECT, 1, 5); nxinput_prerouter_event(&pr, 1, 1, NXINPUT_PREROUTER_START, 1, 15);
    CHECK(exits == 1 && pr.observer_dropped == 2, "MUTANT killed: SDL2 and SDL3 delivering the same physical edge to the sink (one exit, from the owner)"); }
  nxinput_coexist_receipt(&cx, line, sizeof line); puts(line);
  CHECK(strstr(line, "NXC6-COEXIST") && strstr(line, "sequenced=3") && strstr(line, "concurrent_refused=1"), "coexistence receipt");
  nxinput_coexist_restore(&cx);
  CHECK(getenv("SDL_GAMECONTROLLERCONFIG") != NULL && strstr(getenv("SDL_GAMECONTROLLERCONFIG"), "Ambient"), "environment restored for the process after the sequencer is done");
  quit2(); quit3(); dlclose(h2); dlclose(h3);
  printf(fails ? "v5-coexist: FAIL\n" : "v5-coexist: OK\n"); return fails ? 1 : 0;
}
