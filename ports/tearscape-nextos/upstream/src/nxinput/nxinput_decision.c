/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_decision -- see include/nxinput_decision.h. Pure. */
#include "nxinput_decision.h"
#include "nxinput_godot.h"
#include <string.h>

uint64_t nxinput_decision_fnv1a64(const void *data, size_t n, uint64_t seed) {
  const unsigned char *p = data; size_t i;
  uint64_t h = seed ? seed : 0xcbf29ce484222325ull;
  for (i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
  return h;
}

uint64_t nxinput_decision_table_digest(nxinput_sdl_domain domain,
                                       const unsigned long *key_bits, size_t key_bit_count,
                                       const unsigned long *abs_bits, size_t abs_bit_count) {
  nxinput_godot_caps caps; uint64_t h = 0; unsigned i; int code; int32_t v;
  if (nxinput_sdl_domain_plan(domain) == NULL) return 0;
  if (nxinput_godot_caps_init(&caps, key_bits, key_bit_count, abs_bits, abs_bit_count) != 0) return 0;
  v = (int32_t)domain; h = nxinput_decision_fnv1a64(&v, sizeof v, h);
  for (i = 0; i < 512; i++) {
    code = nxinput_sdl_button_code(domain, &caps, i);
    if (code < 0) break;
    v = code; h = nxinput_decision_fnv1a64(&v, sizeof v, h);
  }
  v = -1; h = nxinput_decision_fnv1a64(&v, sizeof v, h);
  for (i = 0; i < 64; i++) {
    code = nxinput_sdl_axis_code(domain, &caps, i);
    if (code < 0) break;
    v = code; h = nxinput_decision_fnv1a64(&v, sizeof v, h);
  }
  v = -2; h = nxinput_decision_fnv1a64(&v, sizeof v, h);
  for (i = NXINPUT_GODOT_ABS_HAT0X; i + 1 < caps.abs_bit_count && i < NXINPUT_GODOT_ABS_HAT0X + 8; i += 2) {
    int a = (int)((caps.abs_bits[i / (8 * sizeof(unsigned long))] >> (i % (8 * sizeof(unsigned long)))) & 1ul);
    int b = (int)((caps.abs_bits[(i + 1) / (8 * sizeof(unsigned long))] >> ((i + 1) % (8 * sizeof(unsigned long)))) & 1ul);
    v = (int32_t)(a && b ? (int)i : -3); h = nxinput_decision_fnv1a64(&v, sizeof v, h);
  }
  return h;
}

int nxinput_decision_equivalence(const nxinput_table_identity *a, const nxinput_table_identity *b) {
  if (a == NULL || b == NULL) return 1;
  if (a->domain != b->domain) return 1;
  if (a->backend != b->backend) return 2;
  if (a->hat_complete != b->hat_complete) return 3;
  if (a->half_axis != b->half_axis) return 4;
  if (a->inversion != b->inversion) return 5;
  if (a->trigger_as_button != b->trigger_as_button) return 6;
  if (a->table_digest != b->table_digest || a->table_digest == 0) return 7;
  if (a->corpus_digest != b->corpus_digest) return 8;
  if (a->physical_digest != b->physical_digest) return 9;
  return 0;
}

nxinput_consumer_table nxinput_decision_consumer_table_kind(nxinput_consumer_kind k) {
  switch (k) {
    case NXINPUT_CONSUMER_SDL2: case NXINPUT_CONSUMER_SDL3: return NXINPUT_CTABLE_UNKNOWN; /* to be proved */
    case NXINPUT_CONSUMER_GODOT: case NXINPUT_CONSUMER_RAW_EVDEV:
    case NXINPUT_CONSUMER_ANDROID: case NXINPUT_CONSUMER_ENGINE_DIRECT: return NXINPUT_CTABLE_NOT_APPLICABLE;
    default: return NXINPUT_CTABLE_UNKNOWN;
  }
}

int nxinput_decision_decide(const nxinput_decision_input *in, nxinput_decision *out) {
  nxinput_consumer_table natural;
  if (in == NULL || out == NULL) return -1;
  memset(out, 0, sizeof *out);
  if (in->consumer_kind >= NXINPUT_CONSUMER_KIND_COUNT) return -1;
  natural = nxinput_decision_consumer_table_kind((nxinput_consumer_kind)in->consumer_kind);
  /* A direct consumer can never be forced to own/use an SDL ordinal table. */
  if (natural == NXINPUT_CTABLE_NOT_APPLICABLE && in->consumer_table == NXINPUT_CTABLE_PROVED) return -1;
  /* An SDL consumer cannot declare itself NOT_APPLICABLE to skip the proof. */
  if (natural != NXINPUT_CTABLE_NOT_APPLICABLE && in->consumer_table == NXINPUT_CTABLE_NOT_APPLICABLE) return -1;

  if (in->source_trust != NXINPUT_TRUST_PROVED || in->consumer_table == NXINPUT_CTABLE_UNKNOWN) {
    out->decision = NXINPUT_DECIDE_DO_NOT_MUTATE_STORE;
    if (in->existing_native_proved) out->passthrough = 1; else out->blocked_degraded = 1;
    return 0;
  }
  if (in->consumer_table == NXINPUT_CTABLE_NOT_APPLICABLE) {
    out->decision = NXINPUT_DECIDE_ROUTE_TYPED_DIRECT;
    return 0; /* no ordinal setter, no byte-intact claim */
  }
  {
    int f = nxinput_decision_equivalence(&in->source, &in->consumer);
    out->equivalence_failed_field = (uint8_t)f;
    if (f == 0) {
      out->decision = NXINPUT_DECIDE_KEEP_EXISTING_BYTE_INTACT;
      out->ordinal_setter_allowed = 1; out->byte_intact_claim = 1; out->store_may_mutate = 1;
    } else {
      out->decision = NXINPUT_DECIDE_TRANSLATE_TYPED;
      out->ordinal_setter_allowed = 1; out->store_may_mutate = 1;
    }
  }
  return 0;
}

const char *nxinput_decision_name(nxinput_mapping_decision d) {
  switch (d) {
    case NXINPUT_DECIDE_KEEP_EXISTING_BYTE_INTACT: return "KEEP_EXISTING_BYTE_INTACT";
    case NXINPUT_DECIDE_TRANSLATE_TYPED: return "TRANSLATE_TYPED";
    case NXINPUT_DECIDE_ROUTE_TYPED_DIRECT: return "ROUTE_TYPED_DIRECT";
    default: return "DO_NOT_MUTATE_STORE";
  }
}

nxinput_edge_owner nxinput_decision_edge_owner(nxinput_authority_mode mode, nxinput_edge_binding b, int chord_hold) {
  if (chord_hold) return NXINPUT_OWNER_SUPPRESSED;
  switch (mode) {
    case NXINPUT_AUTHORITY_ENGINE:
      /* no owner GPTK: the engine governs every edge; null = suppressed */
      return b == NXINPUT_EDGE_NULL ? NXINPUT_OWNER_SUPPRESSED : NXINPUT_OWNER_ENGINE_NATIVE;
    case NXINPUT_AUTHORITY_NEXTOS: case NXINPUT_AUTHORITY_SYNCHRONIZED:
      if (b == NXINPUT_EDGE_ACTION) return NXINPUT_OWNER_AUTHORITY_ACTION;
      if (b == NXINPUT_EDGE_NATIVE) return NXINPUT_OWNER_ENGINE_NATIVE; /* that edge only */
      return NXINPUT_OWNER_SUPPRESSED;
    default: return NXINPUT_OWNER_SUPPRESSED;
  }
}
const char *nxinput_decision_owner_name(nxinput_edge_owner o) {
  switch (o) {
    case NXINPUT_OWNER_AUTHORITY_ACTION: return "nextos-action"; /* the owner authority side */
    case NXINPUT_OWNER_ENGINE_NATIVE: return "engine-native";
    default: return "suppressed";
  }
}
