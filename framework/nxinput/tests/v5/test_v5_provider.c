/* SPDX-License-Identifier: GPL-3.0-only */
/* V5 / B1-B4 -- the provider descriptor: pure decisions + host probe. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../../include/nxinput_provider.h"
#include "../../include/nxinput_provider_linux.h"
#include "../../include/nxinput_sha256.h"
#include "../../include/nxinput_godot.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)

static void evidence_init(nxinput_provider_evidence *e, uint8_t api) {
  memset(e, 0, sizeof *e);
  e->api_version = NXINPUT_PROVIDER_API_VERSION;
  e->struct_size = sizeof *e;
  e->api = api;
}

int main(int argc, char **argv) {
  nxinput_provider_evidence e;
  nxinput_provider_descriptor d, d2;
  char line[512];

  /* sha256 KAT (FIPS 180-4 "abc"). */
  {
    nxinput_sha256 ctx; uint8_t dg[32]; char hex[65];
    nxinput_sha256_init(&ctx); nxinput_sha256_update(&ctx, "abc", 3); nxinput_sha256_final(&ctx, dg); nxinput_sha256_hex(dg, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "sha256 KAT abc");
    nxinput_sha256_init(&ctx); nxinput_sha256_final(&ctx, dg); nxinput_sha256_hex(dg, hex);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0, "sha256 KAT empty");
  }

  /* 1. (0.11.1, review finding 1b) Exported ById API with an unknown sha:
   * the API is a measurement instrument, NOT a table. Domain stays
   * UNDECLARED (no rewrite, stock) until nxinput_provider_apply_measurement. */
  evidence_init(&e, NXINPUT_SDL_API_2);
  e.has_exported_bytable = 1;
  strcpy(e.sha256, "0000000000000000000000000000000000000000000000000000000000000000");
  CHECK(nxinput_provider_resolve(&e, 1, &d) == 0, "resolve ok");
  CHECK(d.method == NXINPUT_PROVIDER_METHOD_EXPORTED_API && d.domain == NXINPUT_SDL_DOMAIN_UNDECLARED && d.table_api_available && !nxinput_provider_allows_rewrite(&d), "MUTANT killed: domain inferred from the PRESENCE of the ById symbols (exported API alone => undeclared, no rewrite)");
  /* 1b. the measurement decides: the table the provider really built. */
  {
    unsigned long kb[NXINPUT_GODOT_KEY_BITS / 64 + 1] = {0}, ab[NXINPUT_GODOT_ABS_BITS / 64 + 1] = {0};
    static const unsigned keys[] = {0x11, 0x72, 0x73, 0x130, 0x131, 0x132, 0x133, 0x134, 0x135, 0x136, 0x137, 0x138, 0x139, 0x13a, 0x13b, 0x13c};
    nxinput_godot_caps caps; nxinput_provider_measurement m; uint8_t matched = 99, amb = 99; size_t k;
    for (k = 0; k < sizeof keys / sizeof keys[0]; k++) kb[keys[k] / 64] |= 1ul << (keys[k] % 64);
    ab[0] |= 1ul << 0; ab[0] |= 1ul << 1; ab[0] |= 1ul << 2; ab[0] |= 1ul << 5; ab[0] |= 1ul << 0x10; ab[0] |= 1ul << 0x11;
    nxinput_godot_caps_init(&caps, kb, NXINPUT_GODOT_KEY_BITS, ab, NXINPUT_GODOT_ABS_BITS);
    /* the ascending sweep (as the CFW 2.30.12 probe measured: 0x11->b0, 0x72->b1, 0x73->b2, 0x130->b3 ...) */
    memset(&m, 0, sizeof m);
    for (k = 0; k < sizeof keys / sizeof keys[0]; k++) m.button_code[k] = (int)keys[k];
    m.buttons = (unsigned)(sizeof keys / sizeof keys[0]);
    m.axis_code[0] = 0; m.axis_code[1] = 1; m.axis_code[2] = 2; m.axis_code[3] = 5; m.axes = 4;
    m.hat_code[0] = 0x10; m.hats = 1;
    CHECK(nxinput_provider_measurement_match(&m, &caps, NXINPUT_SDL_API_2, &matched, &amb) == 0 && matched == NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED && amb == 1, "measured ascending table matches exactly one plan: sdl2-ascending-patched");
    CHECK(nxinput_provider_apply_measurement(&d, &m, matched, amb) == 0 && d.method == NXINPUT_PROVIDER_METHOD_MEASURED_INPROCESS && d.domain == NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED && d.measured && !d.measurement_conflict && nxinput_provider_allows_rewrite(&d), "measurement applied: MEASURED_INPROCESS, rewrite allowed");
    /* the upstream high-first sweep of the same pad */
    memset(&m, 0, sizeof m);
    for (k = 0; k < 13; k++) m.button_code[k] = (int)keys[3 + k];
    m.button_code[13] = 0x11; m.button_code[14] = 0x72; m.button_code[15] = 0x73; m.buttons = 16;
    m.axis_code[0] = 0; m.axis_code[1] = 1; m.axis_code[2] = 2; m.axis_code[3] = 5; m.axes = 4; m.hat_code[0] = 0x10; m.hats = 1;
    CHECK(nxinput_provider_measurement_match(&m, &caps, NXINPUT_SDL_API_2, &matched, &amb) == 0 && matched == NXINPUT_SDL_DOMAIN_SDL2_EVDEV, "measured high-first table => sdl2-evdev");
    /* MUTANT: a table matching no plan (A/B swapped in the provider) */
    m.button_code[0] = 0x131; m.button_code[1] = 0x130;
    nxinput_provider_resolve(&e, 1, &d);
    CHECK(nxinput_provider_measurement_match(&m, &caps, NXINPUT_SDL_API_2, &matched, &amb) == 0 && matched == NXINPUT_SDL_DOMAIN_UNDECLARED && amb == 0, "MUTANT killed: a table no plan reproduces matches nothing");
    CHECK(nxinput_provider_apply_measurement(&d, &m, matched, amb) == 0 && d.method == NXINPUT_PROVIDER_METHOD_UNKNOWN && d.measurement_conflict && !nxinput_provider_allows_rewrite(&d), "conflicting measurement => UNKNOWN (exported API claim does not survive), no rewrite");
    /* SDL3 never matches an SDL2 plan */
    memset(&m, 0, sizeof m); for (k = 0; k < sizeof keys / sizeof keys[0]; k++) m.button_code[k] = (int)keys[k]; m.buttons = 16; m.axes = 4; m.axis_code[0] = 0; m.axis_code[1] = 1; m.axis_code[2] = 2; m.axis_code[3] = 5; m.hat_code[0] = 0x10; m.hats = 1;
    CHECK(nxinput_provider_measurement_match(&m, &caps, NXINPUT_SDL_API_3, &matched, &amb) == 0 && matched == NXINPUT_SDL_DOMAIN_UNDECLARED, "an ascending table measured on SDL3 matches no SDL3 plan (5.7: majors never share)");
    /* runtime pins: a harness-declared sha decides by PINNED_ELF */
    { static const nxinput_provider_pin rp[1] = {{"2222222222222222222222222222222222222222222222222222222222222222", "file:harness-2.32.10", NXINPUT_SDL_API_2, NXINPUT_SDL_DOMAIN_SDL2_EVDEV}};
      nxinput_provider_set_runtime_pins(rp, 1);
      evidence_init(&e, NXINPUT_SDL_API_2); strcpy(e.sha256, rp[0].sha256); e.sha_bound_to_mapping = 1;
      nxinput_provider_resolve(&e, 1, &d);
      CHECK(d.method == NXINPUT_PROVIDER_METHOD_PINNED_ELF && d.domain == NXINPUT_SDL_DOMAIN_SDL2_EVDEV && d.pin_id && strncmp(d.pin_id, "file:", 5) == 0, "runtime pin (harness file) decides by PINNED_ELF with a file: id");
      nxinput_provider_set_runtime_pins(NULL, 0);
      nxinput_provider_resolve(&e, 1, &d);
      CHECK(d.method == NXINPUT_PROVIDER_METHOD_UNKNOWN, "runtime pins cleared: unknown again"); }
  }

  /* 0.11.4 (review 2, N1): measurement against the SET of plans.
   * A pad with ONLY BTN_* codes is numbered identically by the upstream
   * high-first plan and the ascending plan. Such a pad decides NOTHING; a
   * decided descriptor (pin) is confirmed by it, never replaced; and the
   * next pad (gpio-keys, discriminating) decides on its own. */
  {
    unsigned long kbA[NXINPUT_GODOT_KEY_BITS / 64 + 1] = {0}, abA[NXINPUT_GODOT_ABS_BITS / 64 + 1] = {0};
    unsigned long kbB[NXINPUT_GODOT_KEY_BITS / 64 + 1] = {0};
    static const unsigned keysA[] = {0x130, 0x131, 0x133, 0x134, 0x136, 0x137, 0x13a, 0x13b, 0x13d, 0x13e};
    static const unsigned keysB[] = {0x11, 0x72, 0x73, 0x130, 0x131, 0x132, 0x133, 0x134, 0x135, 0x136, 0x137, 0x138, 0x139, 0x13a, 0x13b, 0x13c};
    nxinput_godot_caps capsA, capsB; nxinput_provider_measurement mA, mB; uint32_t mask = 0; uint8_t matched = 0, amb = 0; size_t k;
    for (k = 0; k < sizeof keysA / sizeof keysA[0]; k++) kbA[keysA[k] / 64] |= 1ul << (keysA[k] % 64);
    for (k = 0; k < sizeof keysB / sizeof keysB[0]; k++) kbB[keysB[k] / 64] |= 1ul << (keysB[k] % 64);
    abA[0] |= 1ul << 0; abA[0] |= 1ul << 1; abA[0] |= 1ul << 0x10; abA[0] |= 1ul << 0x11;
    nxinput_godot_caps_init(&capsA, kbA, NXINPUT_GODOT_KEY_BITS, abA, NXINPUT_GODOT_ABS_BITS);
    nxinput_godot_caps_init(&capsB, kbB, NXINPUT_GODOT_KEY_BITS, abA, NXINPUT_GODOT_ABS_BITS);
    memset(&mA, 0, sizeof mA); for (k = 0; k < sizeof keysA / sizeof keysA[0]; k++) mA.button_code[k] = (int)keysA[k]; mA.buttons = (unsigned)(sizeof keysA / sizeof keysA[0]);
    mA.axis_code[0] = 0; mA.axis_code[1] = 1; mA.axes = 2; mA.hat_code[0] = 0x10; mA.hats = 1;
    CHECK(nxinput_provider_measurement_match(&mA, &capsA, NXINPUT_SDL_API_2, &matched, &amb) == 0 && amb >= 2, "a BTN_*-only pad is reproduced by more than one plan (ambiguous)");
    CHECK(nxinput_provider_measurement_plans(&mA, &capsA, NXINPUT_SDL_API_2, &mask) == 0 && (mask & (1u << NXINPUT_SDL_DOMAIN_SDL2_EVDEV)) && (mask & (1u << NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED)), "the plan SET names both sdl2-evdev and ascending");
    evidence_init(&e, NXINPUT_SDL_API_2); e.has_exported_bytable = 1; strcpy(e.sha256, "0000000000000000000000000000000000000000000000000000000000000000");
    nxinput_provider_resolve(&e, 1, &d);
    CHECK(nxinput_provider_apply_measurement_set(&d, &mA, mask) == 0 && d.domain == NXINPUT_SDL_DOMAIN_UNDECLARED && !d.measured && !nxinput_provider_allows_rewrite(&d) && d.measured_ambiguous >= 2, "MUTANT killed: ambiguous first pad decided the domain by plan order (must stay undecided = stock)");
    CHECK(nxinput_provider_apply_measurement(&d, &mA, matched, amb) == 0 && d.domain == NXINPUT_SDL_DOMAIN_UNDECLARED && !nxinput_provider_allows_rewrite(&d), "compat entry with ambiguous>1 never decides either");
    memset(&mB, 0, sizeof mB); for (k = 0; k < sizeof keysB / sizeof keysB[0]; k++) mB.button_code[k] = (int)keysB[k]; mB.buttons = (unsigned)(sizeof keysB / sizeof keysB[0]);
    mB.axis_code[0] = 0; mB.axis_code[1] = 1; mB.axes = 2; mB.hat_code[0] = 0x10; mB.hats = 1;
    CHECK(nxinput_provider_measurement_plans(&mB, &capsB, NXINPUT_SDL_API_2, &mask) == 0 && mask == (1u << NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED), "the second pad is reproduced by exactly the ascending plan");
    CHECK(nxinput_provider_apply_measurement_set(&d, &mB, mask) == 0 && d.method == NXINPUT_PROVIDER_METHOD_MEASURED_INPROCESS && d.domain == NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED && nxinput_provider_allows_rewrite(&d), "the discriminating pad decides: measured-inprocess ascending (its CFW line will be translated, not dropped)");
    evidence_init(&e, NXINPUT_SDL_API_2); e.has_exported_bytable = 1; e.sha_bound_to_mapping = 1; strcpy(e.sha256, "8c4dc956e278546c96b06a6a7b0a2b8a1991e877e81e172fa1bed26a9ada453a");
    nxinput_provider_resolve(&e, 1, &d);
    CHECK(d.method == NXINPUT_PROVIDER_METHOD_PINNED_ELF && d.domain == NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, "pinned Knulli before any measurement");
    nxinput_provider_measurement_plans(&mA, &capsA, NXINPUT_SDL_API_2, &mask);
    CHECK(nxinput_provider_apply_measurement_set(&d, &mA, mask) == 0 && d.method == NXINPUT_PROVIDER_METHOD_PINNED_ELF && d.domain == NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED && d.measured && !d.measurement_conflict, "MUTANT killed: an ambiguous pad overwrote a PINNED provider with sdl2-evdev (pin inside the set = confirmed)");
    memset(&mB, 0, sizeof mB); for (k = 0; k < 13; k++) mB.button_code[k] = (int)keysB[3 + k]; mB.button_code[13] = 0x11; mB.button_code[14] = 0x72; mB.button_code[15] = 0x73; mB.buttons = 16;
    mB.axis_code[0] = 0; mB.axis_code[1] = 1; mB.axes = 2; mB.hat_code[0] = 0x10; mB.hats = 1;
    nxinput_provider_measurement_plans(&mB, &capsB, NXINPUT_SDL_API_2, &mask);
    CHECK((mask & (1u << NXINPUT_SDL_DOMAIN_SDL2_EVDEV)) && !(mask & (1u << NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED)), "high-first table of the gpio pad is reproduced by sdl2-evdev and never by the ascending plan");
    CHECK(nxinput_provider_apply_measurement_set(&d, &mB, mask) == 0 && d.method == NXINPUT_PROVIDER_METHOD_UNKNOWN && d.measurement_conflict && !nxinput_provider_allows_rewrite(&d), "a pin contradicted by the provider's own table => UNKNOWN (stock), never a rewrite under either plan");
  }

  /* 2. Pinned sha, bound to the mapping -> pinned domain. */
  evidence_init(&e, NXINPUT_SDL_API_2);
  strcpy(e.sha256, "4fd539cd0e2b2dc883beb8dedffa1f85212276b34e51b2a8bf98b4f8fd4f5cb4");
  e.sha_bound_to_mapping = 1;
  nxinput_provider_resolve(&e, 1, &d);
  CHECK(d.method == NXINPUT_PROVIDER_METHOD_PINNED_ELF && d.domain == NXINPUT_SDL_DOMAIN_SDL2_EVDEV, "pinned dArkOS 2.32.10 => sdl2-evdev");
  CHECK(d.pin_id && strcmp(d.pin_id, "pin-4fd539cd-sdl2-2.32.10") == 0, "pin id carried");
  strcpy(e.sha256, "8c4dc956e278546c96b06a6a7b0a2b8a1991e877e81e172fa1bed26a9ada453a");
  nxinput_provider_resolve(&e, 1, &d);
  CHECK(d.domain == NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, "pinned Knulli 2.30.12 => ascending");

  /* 3. Pinned sha but NOT bound to the mapped object -> UNKNOWN (file could
   * have been replaced after mmap). */
  e.sha_bound_to_mapping = 0;
  nxinput_provider_resolve(&e, 1, &d);
  CHECK(d.method == NXINPUT_PROVIDER_METHOD_UNKNOWN && !nxinput_provider_allows_rewrite(&d), "unbound sha => UNKNOWN, no rewrite");

  /* 4. Unknown sha, no exported API -> UNKNOWN. Absence of symbols is NOT
   * upstream. */
  evidence_init(&e, NXINPUT_SDL_API_2);
  strcpy(e.sha256, "1111111111111111111111111111111111111111111111111111111111111111");
  e.sha_bound_to_mapping = 1;
  nxinput_provider_resolve(&e, 1, &d);
  CHECK(d.method == NXINPUT_PROVIDER_METHOD_UNKNOWN && d.domain == NXINPUT_SDL_DOMAIN_UNDECLARED, "unknown sha without ById => UNKNOWN (never upstream by default)");

  /* 5. NextOS SDL3: pinned bytes, table undeclared -> UNKNOWN with pin id. */
  evidence_init(&e, NXINPUT_SDL_API_3); e.sha_bound_to_mapping = 1;
  strcpy(e.sha256, "eceaf5f97ca778ccb86e0389153c9a0a333557c338183123dc05f63a73912fbf");
  nxinput_provider_resolve(&e, 1, &d);
  CHECK(d.method == NXINPUT_PROVIDER_METHOD_UNKNOWN && d.pin_id && strcmp(d.pin_id, "pin-eceaf5f9-sdl3-3.5.0") == 0, "pinned-but-unmeasured bytes stay UNKNOWN (pin id for the receipt)");
  /* 5b. NextOS SDL2 fork: measured on the device -> high-first by pin. */
  evidence_init(&e, NXINPUT_SDL_API_2); e.sha_bound_to_mapping = 1;
  strcpy(e.sha256, "1ac99b5cea2844c7faa20d043811e8409ccfb30724a7314222a555b45a1917b6");
  nxinput_provider_resolve(&e, 1, &d);
  CHECK(d.method == NXINPUT_PROVIDER_METHOD_PINNED_ELF && d.domain == NXINPUT_SDL_DOMAIN_SDL2_EVDEV, "NextOS fork: measured table (not symbol absence) => sdl2-evdev");

  /* 6. Major separation: SDL3 evidence with the SDL2 Knulli sha never gets
   * the SDL2 table; ById flag on SDL3 is ignored. */
  evidence_init(&e, NXINPUT_SDL_API_3);
  strcpy(e.sha256, "8c4dc956e278546c96b06a6a7b0a2b8a1991e877e81e172fa1bed26a9ada453a");
  e.sha_bound_to_mapping = 1; e.has_exported_bytable = 1;
  nxinput_provider_resolve(&e, 1, &d);
  CHECK(d.method == NXINPUT_PROVIDER_METHOD_UNKNOWN, "SDL3 never inherits an SDL2 pin/table (5.7)");
  evidence_init(&e, NXINPUT_SDL_API_2); strcpy(e.sha256, "8c4dc956e278546c96b06a6a7b0a2b8a1991e877e81e172fa1bed26a9ada453a"); e.sha_bound_to_mapping = 1;
  nxinput_provider_resolve(&e, 7, &d); nxinput_provider_resolve(&e, 7, &d2);
  CHECK(nxinput_provider_same_instance(&d, &d2), "same api/sha/generation => same instance");
  nxinput_provider_resolve(&e, 8, &d2);
  CHECK(!nxinput_provider_same_instance(&d, &d2), "new generation => different instance");
  d2 = d; d2.evidence.api = NXINPUT_SDL_API_3;
  CHECK(!nxinput_provider_same_instance(&d, &d2), "different major => never the same instance");

  /* 7. Receipt line: sanitized, no path. */
  strcpy(d.evidence.path_class, "system-lib");
  CHECK(nxinput_provider_receipt_line(&d, line, sizeof line) > 0 && strstr(line, "NXC6-PROVIDER") && strstr(line, "domain=sdl2-ascending-patched") && !strstr(line, "/usr"), "receipt line sanitized");
  puts(line);

  /* 8. Host probe against the SDL2 this process really maps. */
  {
    const char *lib = argc > 1 ? argv[1] : "libSDL2-2.0.so.0";
    void *h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (h == NULL) {
      printf("SKIP host probe: %s\n", dlerror());
    } else {
      void *init = dlsym(h, "SDL_Init");
      nxinput_provider_probe probe;
      CHECK(nxinput_provider_probe_sdl(init, NXINPUT_SDL_API_2, &probe) == 0, "probe runs");
      CHECK(probe.evidence.sha_bound_to_mapping == 1, "sha bound to the mapped object (map_files or dev/inode)");
      CHECK(strlen(probe.evidence.sha256) == 64, "sha256 computed");
      CHECK(strcmp(probe.evidence.soname, "libSDL2-2.0.so.0") == 0, "DT_SONAME read from the mapped bytes");
      CHECK(probe.evidence.version[0] != '\0', "SDL_GetVersion answered from the same object");
      CHECK(probe.evidence.statically_linked == 0, "MUTANT killed: a DSO reported as statically linked (decided against /proc/self/exe, not against nxinput's own object)");
      nxinput_provider_resolve(&probe.evidence, 1, &d);
      nxinput_provider_receipt_line(&d, line, sizeof line);
      puts(line);
      if (probe.evidence.has_exported_bytable) {
        CHECK(d.method == NXINPUT_PROVIDER_METHOD_EXPORTED_API && d.domain == NXINPUT_SDL_DOMAIN_UNDECLARED, "host provider exports ById => measurable, undeclared until measured");
      } else {
        CHECK(d.method == NXINPUT_PROVIDER_METHOD_PINNED_ELF || d.method == NXINPUT_PROVIDER_METHOD_UNKNOWN, "host provider decided by pin or honestly UNKNOWN");
      }
      if (argc > 2) {
        CHECK(strcmp(probe.evidence.sha256, argv[2]) == 0, "sha256 equals the external sha256sum of the mapped file");
      }
      nxinput_provider_probe_close(&probe);
    }
  }
  /* 0.11.1 pin file: well-formed installs, malformed installs nothing */
  {
    char path[] = "/tmp/nxinput-pins.XXXXXX"; int fd = mkstemp(path); FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (f) {
      fputs("# harness pins\n3333333333333333333333333333333333333333333333333333333333333333 sdl2 sdl2-evdev harness-2.28.5\n", f); fclose(f);
      CHECK(nxinput_provider_load_pin_file(path) == 1 && nxinput_provider_pin_lookup("3333333333333333333333333333333333333333333333333333333333333333") != NULL, "pin file: one well-formed pin installed and found");
      f = fopen(path, "w"); fputs("zz sdl2 sdl2-evdev bad\n", f); fclose(f);
      CHECK(nxinput_provider_load_pin_file(path) == -1 && nxinput_provider_pin_lookup("3333333333333333333333333333333333333333333333333333333333333333") == NULL, "MUTANT killed: malformed pin file installing pins (refused, previous pins cleared)");
      unlink(path);
    }
  }
  /* 5.1 static provider: declared only for a main-program SDL */
  {
    nxinput_provider_evidence ev; nxinput_provider_descriptor d; char line[400];
    memset(&ev, 0, sizeof ev); ev.api_version = NXINPUT_PROVIDER_API_VERSION; ev.struct_size = sizeof ev; ev.api = NXINPUT_SDL_API_3;
    memcpy(ev.sha256, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 64); snprintf(ev.path_class, sizeof ev.path_class, "main");
    ev.statically_linked = 0;
    CHECK(nxinput_provider_declare_static(&ev, "pin-sdl3-3.2.30-source", NXINPUT_SDL_DOMAIN_SDL3_EVDEV, 1, &d) == -1, "MUTANT killed: declaring a static source for a DSO provider is refused (never fake a DSO)");
    ev.statically_linked = 1;
    CHECK(nxinput_provider_declare_static(&ev, "pin-sdl3-3.2.30-source", NXINPUT_SDL_DOMAIN_SDL3_EVDEV, 1, &d) == 0 && d.method == NXINPUT_PROVIDER_METHOD_DECLARED_STATIC_SOURCE && d.domain == NXINPUT_SDL_DOMAIN_SDL3_EVDEV && d.driver == NXINPUT_PROVIDER_DRIVER_STATIC_BUILTIN && nxinput_provider_allows_rewrite(&d), "static main-program provider declared with its pinned source and measured domain");
    CHECK(nxinput_provider_declare_static(&ev, "pin-x", NXINPUT_SDL_DOMAIN_UNDECLARED, 1, &d) == -1, "an undeclared domain cannot be declared static (no table = no rewrite)");
    CHECK(nxinput_provider_receipt_line(&d, line, sizeof line) > 0 && strstr(line, "method=declared-static-source") && strstr(line, "static=1"), "receipt names the static method");
  }
  printf(fails ? "v5-provider: FAIL\n" : "v5-provider: OK\n");
  return fails ? 1 : 0;
}
