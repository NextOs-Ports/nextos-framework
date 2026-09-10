/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_DECISION_H
#define NXINPUT_DECISION_H
/*
 * nxinput_decision -- V5 (0.11.0), mission 1.4 as ONE pure machine.
 *
 *   SOURCE_TRUST   = PROVED | UNKNOWN
 *   CONSUMER_KIND  = SDL2 | SDL3 | GODOT | RAW_EVDEV | ANDROID | ENGINE_DIRECT
 *   CONSUMER_TABLE = PROVED | NOT_APPLICABLE | UNKNOWN
 *
 *   source PROVED + consumer PROVED + integral equivalence -> KEEP_EXISTING_BYTE_INTACT
 *   source PROVED + consumer PROVED + divergent tables     -> TRANSLATE_TYPED
 *   source PROVED + consumer NOT_APPLICABLE                -> ROUTE_TYPED_DIRECT
 *                                                            (NO_ORDINAL_SETTER, NO_BYTE_INTACT)
 *   any side UNKNOWN                                       -> DO_NOT_MUTATE_STORE
 *                                                            + EXISTING_NATIVE_PASSTHROUGH only
 *                                                              when already native/proved
 *                                                            else BLOCKED_DEGRADED
 *
 * INTEGRAL EQUIVALENCE (C2) is a conjunction of every field able to choose
 * code: backend/driver, the COMPLETE ordinal table of buttons/axes/hats for
 * the measured caps (digest), corpus/precedence digest, flags (hat complete,
 * half-axis, inversion, trigger presentation) and the physical identity
 * digest. Sharing the label `ascending`, the SDL major, a GUID or a CFW name
 * never suffices: one differing field => NOT equivalent => TRANSLATE_TYPED.
 *
 * RESOLVED_EDGE_OWNER: given PORT_AUTHORITY_MODE and the binding kind of one
 * edge, exactly one owner (authority action, engine native or suppressed). The
 * sovereign chord is a pre-router outside the mode (nxinput_prerouter).
 *
 * Pure. No I/O.
 */
#include "nxinput_sdl.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum nxinput_trust { NXINPUT_TRUST_UNKNOWN = 0, NXINPUT_TRUST_PROVED = 1 } nxinput_trust;
typedef enum nxinput_consumer_kind {
  NXINPUT_CONSUMER_SDL2 = 0, NXINPUT_CONSUMER_SDL3, NXINPUT_CONSUMER_GODOT,
  NXINPUT_CONSUMER_RAW_EVDEV, NXINPUT_CONSUMER_ANDROID, NXINPUT_CONSUMER_ENGINE_DIRECT,
  NXINPUT_CONSUMER_KIND_COUNT
} nxinput_consumer_kind;
typedef enum nxinput_consumer_table {
  NXINPUT_CTABLE_UNKNOWN = 0, NXINPUT_CTABLE_PROVED, NXINPUT_CTABLE_NOT_APPLICABLE
} nxinput_consumer_table;

typedef enum nxinput_mapping_decision {
  NXINPUT_DECIDE_DO_NOT_MUTATE_STORE = 0,   /* + passthrough or blocked, see flags */
  NXINPUT_DECIDE_KEEP_EXISTING_BYTE_INTACT,
  NXINPUT_DECIDE_TRANSLATE_TYPED,
  NXINPUT_DECIDE_ROUTE_TYPED_DIRECT
} nxinput_mapping_decision;

/* Every field able to choose code. Two descriptors are equivalent only when
 * ALL of them are equal. `label` (domain enum) is included but never enough. */
typedef struct nxinput_table_identity {
  uint8_t domain;            /* nxinput_sdl_domain label */
  uint8_t backend;           /* nxinput_provider_driver: evdev/hidapi/... */
  uint8_t hat_complete;      /* both hat axes present */
  uint8_t half_axis;         /* provider exposes +aN/-aN */
  uint8_t inversion;         /* provider honours aN~ */
  uint8_t trigger_as_button; /* trigger presented as EV_KEY */
  uint64_t table_digest;     /* full button+axis+hat table for the caps */
  uint64_t corpus_digest;    /* corpus/precedence that authored the line */
  uint64_t physical_digest;  /* sysfs parent/phys/uniq/capability digest */
} nxinput_table_identity;

typedef struct nxinput_decision_input {
  uint8_t source_trust;      /* nxinput_trust */
  uint8_t consumer_kind;     /* nxinput_consumer_kind */
  uint8_t consumer_table;    /* nxinput_consumer_table */
  uint8_t existing_native_proved; /* a mapping already owned by the provider,
                                     correlated by readback, exists */
  nxinput_table_identity source, consumer;
} nxinput_decision_input;

typedef struct nxinput_decision {
  uint8_t decision;          /* nxinput_mapping_decision */
  uint8_t ordinal_setter_allowed; /* may the external line reach AddMapping? */
  uint8_t byte_intact_claim;      /* may the receipt say BYTE_INTACT? */
  uint8_t store_may_mutate;
  uint8_t passthrough;       /* EXISTING_NATIVE_PASSTHROUGH */
  uint8_t blocked_degraded;
  uint8_t equivalence_failed_field; /* 0 none, else 1-based field index */
} nxinput_decision;

/* Ordinal table digest of one domain over the measured caps: every button
 * ordinal 0..N-1 -> code, every axis ordinal -> code, every hat pair. */
uint64_t nxinput_decision_table_digest(nxinput_sdl_domain domain,
                                       const unsigned long *key_bits, size_t key_bit_count,
                                       const unsigned long *abs_bits, size_t abs_bit_count);
uint64_t nxinput_decision_fnv1a64(const void *data, size_t n, uint64_t seed);

/* 0 when equal on EVERY field; otherwise the 1-based index of the first
 * differing field (1 domain, 2 backend, 3 hat, 4 half, 5 inversion,
 * 6 trigger, 7 table, 8 corpus, 9 physical). */
int nxinput_decision_equivalence(const nxinput_table_identity *a, const nxinput_table_identity *b);

/* Whether this consumer kind consumes an ordinal table at all. SDL2/SDL3
 * do; Godot, raw evdev, Android and engine-direct are NOT_APPLICABLE. */
nxinput_consumer_table nxinput_decision_consumer_table_kind(nxinput_consumer_kind k);

/* THE machine. Returns 0, -1 on invalid input (e.g. a NOT_APPLICABLE
 * consumer kind declared with a PROVED table: refused, never coerced). */
int nxinput_decision_decide(const nxinput_decision_input *in, nxinput_decision *out);
const char *nxinput_decision_name(nxinput_mapping_decision d);

/* RESOLVED_EDGE_OWNER */
typedef enum nxinput_authority_mode { NXINPUT_AUTHORITY_NEXTOS = 0, NXINPUT_AUTHORITY_ENGINE, NXINPUT_AUTHORITY_SYNCHRONIZED } nxinput_authority_mode;
typedef enum nxinput_edge_binding { NXINPUT_EDGE_NATIVE = 0, NXINPUT_EDGE_NULL, NXINPUT_EDGE_ACTION } nxinput_edge_binding;
typedef enum nxinput_edge_owner { NXINPUT_OWNER_SUPPRESSED = 0, NXINPUT_OWNER_AUTHORITY_ACTION, NXINPUT_OWNER_ENGINE_NATIVE } nxinput_edge_owner;
/* `chord_hold`: the sovereign pre-router currently retains this edge. */
nxinput_edge_owner nxinput_decision_edge_owner(nxinput_authority_mode mode, nxinput_edge_binding b, int chord_hold);
const char *nxinput_decision_owner_name(nxinput_edge_owner o);
#ifdef __cplusplus
}
#endif
#endif
