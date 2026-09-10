/* SPDX-License-Identifier: GPL-3.0-only */
/* V5 / 1.4, B3, B4, B9, C2: the ONE mapping-decision machine and the
 * integral equivalence behind BYTE_INTACT. Every line here is a mutant of
 * the mission list 8.3/B9 that must be killed by the pure machine. */
#include "../../include/nxinput_decision.h"
#include "../../include/nxinput_godot.h"
#include <stdio.h>
#include <string.h>
#define NL ((NXINPUT_GODOT_KEY_BITS + 63) / 64)
#define AL ((NXINPUT_GODOT_ABS_BITS + 63) / 64)
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)
static void setb(unsigned long *b, unsigned c) { b[c / 64] |= 1ul << (c % 64); }
static const unsigned h700[] = {0x11, 0x72, 0x73, 0x130, 0x131, 0x132, 0x133, 0x134, 0x135, 0x136, 0x137, 0x138, 0x139, 0x13a, 0x13b, 0x13c};
static nxinput_table_identity ident(nxinput_sdl_domain d, uint64_t table, uint64_t corpus, uint64_t phys) {
  nxinput_table_identity t; memset(&t, 0, sizeof t);
  t.domain = (uint8_t)d; t.backend = 1; t.hat_complete = 1; t.table_digest = table; t.corpus_digest = corpus; t.physical_digest = phys; return t;
}
int main(void) {
  unsigned long kb[NL], ab[AL], kb2[NL]; unsigned i; uint64_t asc, hf, asc2;
  nxinput_decision_input in; nxinput_decision out;
  memset(kb, 0, sizeof kb); memset(ab, 0, sizeof ab);
  for (i = 0; i < sizeof h700 / sizeof h700[0]; i++) setb(kb, h700[i]);
  setb(ab, 0); setb(ab, 1); setb(ab, 2); setb(ab, 3); setb(ab, 0x10); setb(ab, 0x11);
  asc = nxinput_decision_table_digest(NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, kb, NXINPUT_GODOT_KEY_BITS, ab, NXINPUT_GODOT_ABS_BITS);
  hf = nxinput_decision_table_digest(NXINPUT_SDL_DOMAIN_SDL2_EVDEV, kb, NXINPUT_GODOT_KEY_BITS, ab, NXINPUT_GODOT_ABS_BITS);
  memcpy(kb2, kb, sizeof kb); setb(kb2, 0x74); /* one more low key (power) */
  asc2 = nxinput_decision_table_digest(NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, kb2, NXINPUT_GODOT_KEY_BITS, ab, NXINPUT_GODOT_ABS_BITS);
  CHECK(asc != 0 && hf != 0 && asc != hf, "table digests: ascending != high-first on the H700 caps");
  CHECK(asc != asc2, "same domain label, one extra EV_KEY: different complete table digest");
  CHECK(nxinput_decision_table_digest(NXINPUT_SDL_DOMAIN_UNDECLARED, kb, NXINPUT_GODOT_KEY_BITS, ab, NXINPUT_GODOT_ABS_BITS) == 0, "undeclared domain has no table (digest 0 = never equivalent)");

  /* PROVED + PROVED + integral equivalence -> KEEP_EXISTING_BYTE_INTACT */
  memset(&in, 0, sizeof in); in.source_trust = NXINPUT_TRUST_PROVED; in.consumer_kind = NXINPUT_CONSUMER_SDL2; in.consumer_table = NXINPUT_CTABLE_PROVED;
  in.source = ident(NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, asc, 0xC0, 0xF0); in.consumer = in.source;
  CHECK(nxinput_decision_decide(&in, &out) == 0 && out.decision == NXINPUT_DECIDE_KEEP_EXISTING_BYTE_INTACT && out.byte_intact_claim && out.ordinal_setter_allowed, "PROVED/PROVED, every field equal: KEEP_EXISTING_BYTE_INTACT");
  /* mutants B9/8.3: same label, some field divergent -> NOT byte-intact (TRANSLATE_TYPED) */
  in.consumer = ident(NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, asc2, 0xC0, 0xF0);
  CHECK(nxinput_decision_decide(&in, &out) == 0 && out.decision == NXINPUT_DECIDE_TRANSLATE_TYPED && !out.byte_intact_claim && out.equivalence_failed_field == 7, "MUTANT killed: label 'ascending' equal, complete table differs -> no BYTE_INTACT");
  in.consumer = ident(NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, asc, 0xC1, 0xF0);
  CHECK(nxinput_decision_decide(&in, &out) == 0 && !out.byte_intact_claim && out.equivalence_failed_field == 8, "MUTANT killed: corpus/precedence digest differs -> no BYTE_INTACT");
  in.consumer = ident(NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, asc, 0xC0, 0xF1);
  CHECK(nxinput_decision_decide(&in, &out) == 0 && !out.byte_intact_claim && out.equivalence_failed_field == 9, "MUTANT killed: physical identity differs -> no BYTE_INTACT");
  in.consumer = in.source; in.consumer.backend = 2; /* hidapi vs evdev */
  CHECK(nxinput_decision_decide(&in, &out) == 0 && !out.byte_intact_claim && out.equivalence_failed_field == 2, "MUTANT killed: backend/driver differs -> no BYTE_INTACT");
  in.consumer = in.source; in.consumer.half_axis = 1;
  CHECK(nxinput_decision_decide(&in, &out) == 0 && !out.byte_intact_claim && out.equivalence_failed_field == 4, "MUTANT killed: half-axis flag differs -> no BYTE_INTACT");
  in.consumer = in.source; in.consumer.hat_complete = 0;
  CHECK(nxinput_decision_decide(&in, &out) == 0 && !out.byte_intact_claim && out.equivalence_failed_field == 3, "MUTANT killed: hat completeness differs -> no BYTE_INTACT");
  in.consumer = in.source; in.consumer.inversion = 1;
  CHECK(nxinput_decision_decide(&in, &out) == 0 && !out.byte_intact_claim && out.equivalence_failed_field == 5, "MUTANT killed: inversion flag differs -> no BYTE_INTACT");
  in.consumer = in.source; in.consumer.trigger_as_button = 1;
  CHECK(nxinput_decision_decide(&in, &out) == 0 && !out.byte_intact_claim && out.equivalence_failed_field == 6, "MUTANT killed: trigger presentation differs -> no BYTE_INTACT");
  in.consumer = in.source; in.consumer.table_digest = 0; in.source.table_digest = 0;
  CHECK(nxinput_decision_decide(&in, &out) == 0 && !out.byte_intact_claim, "MUTANT killed: two undeclared tables (digest 0) are never 'equal'");
  /* PROVED + PROVED + divergent tables -> TRANSLATE_TYPED (setter allowed, no byte-intact) */
  in.source = ident(NXINPUT_SDL_DOMAIN_SDL2_ASCENDING_PATCHED, asc, 0xC0, 0xF0); in.consumer = ident(NXINPUT_SDL_DOMAIN_SDL2_EVDEV, hf, 0xC0, 0xF0);
  CHECK(nxinput_decision_decide(&in, &out) == 0 && out.decision == NXINPUT_DECIDE_TRANSLATE_TYPED && out.ordinal_setter_allowed && !out.byte_intact_claim, "ascending source on high-first consumer: TRANSLATE_TYPED");
  /* any side UNKNOWN -> DO_NOT_MUTATE_STORE */
  in.source_trust = NXINPUT_TRUST_UNKNOWN;
  CHECK(nxinput_decision_decide(&in, &out) == 0 && out.decision == NXINPUT_DECIDE_DO_NOT_MUTATE_STORE && !out.ordinal_setter_allowed && !out.store_may_mutate && !out.byte_intact_claim && out.blocked_degraded, "MUTANT killed: source UNKNOWN -> no setter, no store mutation, no BYTE_INTACT (blocked/degraded)");
  in.existing_native_proved = 1;
  CHECK(nxinput_decision_decide(&in, &out) == 0 && out.decision == NXINPUT_DECIDE_DO_NOT_MUTATE_STORE && out.passthrough && !out.ordinal_setter_allowed, "source UNKNOWN with a native mapping already owned by the provider: EXISTING_NATIVE_PASSTHROUGH, still no setter");
  in.source_trust = NXINPUT_TRUST_PROVED; in.consumer_table = NXINPUT_CTABLE_UNKNOWN; in.existing_native_proved = 0;
  CHECK(nxinput_decision_decide(&in, &out) == 0 && out.decision == NXINPUT_DECIDE_DO_NOT_MUTATE_STORE && !out.ordinal_setter_allowed && !out.store_may_mutate, "MUTANT killed: consumer (provider) UNKNOWN injecting an unchanged external line -> refused before the setter");
  /* consumer NOT_APPLICABLE -> ROUTE_TYPED_DIRECT */
  in.consumer_kind = NXINPUT_CONSUMER_GODOT; in.consumer_table = NXINPUT_CTABLE_NOT_APPLICABLE;
  CHECK(nxinput_decision_decide(&in, &out) == 0 && out.decision == NXINPUT_DECIDE_ROUTE_TYPED_DIRECT && !out.ordinal_setter_allowed && !out.byte_intact_claim && !out.store_may_mutate, "Godot consumer: ROUTE_TYPED_DIRECT + NO_ORDINAL_SETTER + NO_BYTE_INTACT");
  in.consumer_kind = NXINPUT_CONSUMER_RAW_EVDEV; CHECK(nxinput_decision_decide(&in, &out) == 0 && out.decision == NXINPUT_DECIDE_ROUTE_TYPED_DIRECT, "raw evdev consumer: ROUTE_TYPED_DIRECT");
  in.consumer_kind = NXINPUT_CONSUMER_ANDROID; CHECK(nxinput_decision_decide(&in, &out) == 0 && out.decision == NXINPUT_DECIDE_ROUTE_TYPED_DIRECT, "Android consumer: ROUTE_TYPED_DIRECT");
  in.consumer_kind = NXINPUT_CONSUMER_ENGINE_DIRECT; CHECK(nxinput_decision_decide(&in, &out) == 0 && out.decision == NXINPUT_DECIDE_ROUTE_TYPED_DIRECT, "engine-direct consumer: ROUTE_TYPED_DIRECT");
  /* mutant: a direct consumer FORCED to an SDL ordinal table -> refused, never coerced */
  in.consumer_kind = NXINPUT_CONSUMER_GODOT; in.consumer_table = NXINPUT_CTABLE_PROVED;
  CHECK(nxinput_decision_decide(&in, &out) == -1, "MUTANT killed: Godot/raw/Android/engine-direct forced to use an SDL ordinal table -> invalid input, refused");
  in.consumer_kind = NXINPUT_CONSUMER_SDL3; in.consumer_table = NXINPUT_CTABLE_NOT_APPLICABLE;
  CHECK(nxinput_decision_decide(&in, &out) == -1, "MUTANT killed: an SDL consumer declaring NOT_APPLICABLE to skip the table proof -> refused");
  in.consumer_kind = 99;
  CHECK(nxinput_decision_decide(&in, &out) == -1, "unknown consumer kind refused");
  /* RESOLVED_EDGE_OWNER */
  CHECK(nxinput_decision_edge_owner(NXINPUT_AUTHORITY_NEXTOS, NXINPUT_EDGE_ACTION, 0) == NXINPUT_OWNER_AUTHORITY_ACTION, "nextos + action -> nextos-action");
  CHECK(nxinput_decision_edge_owner(NXINPUT_AUTHORITY_NEXTOS, NXINPUT_EDGE_NATIVE, 0) == NXINPUT_OWNER_ENGINE_NATIVE, "nextos + native -> engine-native for THAT edge only");
  CHECK(nxinput_decision_edge_owner(NXINPUT_AUTHORITY_NEXTOS, NXINPUT_EDGE_NULL, 0) == NXINPUT_OWNER_SUPPRESSED, "nextos + null -> suppressed");
  CHECK(nxinput_decision_edge_owner(NXINPUT_AUTHORITY_ENGINE, NXINPUT_EDGE_ACTION, 0) == NXINPUT_OWNER_ENGINE_NATIVE, "engine mode: no GPTK owner, engine governs the edge");
  CHECK(nxinput_decision_edge_owner(NXINPUT_AUTHORITY_SYNCHRONIZED, NXINPUT_EDGE_ACTION, 0) == NXINPUT_OWNER_AUTHORITY_ACTION, "synchronized + action -> nextos-action");
  CHECK(nxinput_decision_edge_owner(NXINPUT_AUTHORITY_NEXTOS, NXINPUT_EDGE_ACTION, 1) == NXINPUT_OWNER_SUPPRESSED && nxinput_decision_edge_owner(NXINPUT_AUTHORITY_ENGINE, NXINPUT_EDGE_NATIVE, 1) == NXINPUT_OWNER_SUPPRESSED, "chord hold suppresses the edge in every mode (pre-router is outside the mode)");
  CHECK(!strcmp(nxinput_decision_name(NXINPUT_DECIDE_DO_NOT_MUTATE_STORE), "DO_NOT_MUTATE_STORE") && !strcmp(nxinput_decision_owner_name(NXINPUT_OWNER_SUPPRESSED), "suppressed"), "names");
  printf(fails ? "v5-decision: FAIL\n" : "v5-decision: OK\n"); return fails ? 1 : 0;
}
