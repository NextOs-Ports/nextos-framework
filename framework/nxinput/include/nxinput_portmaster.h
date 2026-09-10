/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_PORTMASTER_H
#define NXINPUT_PORTMASTER_H

/*
 * Capability-proved projection of PortMaster mappings written in the legacy
 * joydev button domain into the evdev domain used by current SDL2/SDL3.
 *
 * This is not a controller-name, CFW, board or VID/PID workaround. Since
 * 0.10.0 the domain of a line is decided by SEMANTIC PROOF against the
 * measured EV_KEY bitmap of the exact event node: every `bN` binding is
 * interpreted in BOTH candidate domains (legacy joydev ascending-rank and
 * the current SDL evdev enumeration) and a domain is coherent only when
 * every class-known semantic lands in a code of its own capability class
 * (gamepad semantics on BTN_* codes, volumedown/volumeup on the exact
 * volume keys) and the whole line stays reachable. Exactly one coherent
 * domain selects it; two coherent domains producing the very same codes
 * preserve the bytes; two coherent but divergent interpretations are
 * AMBIGUOUS and the SOURCE yields to the next authority -- ambiguity is
 * never downgraded to "not applicable", which would let the original line
 * pass in the wrong domain silently. The volumedown/volumeup markers stay
 * strong positive evidence when present, but they are no longer the only
 * admissible proof. Native/current mappings are a byte-for-byte no-op.
 * No rule here may name a CFW, model, GUID, controller name or VID/PID.
 */

#include "nxinput_sdl.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_PM_MAPPING_MAX 2048u

enum {
  NXINPUT_PM_ERROR = -1,
  NXINPUT_PM_NOT_APPLICABLE = 0,
  NXINPUT_PM_REWRITTEN = 1,
  /* 0.10.0: the SOURCE must yield to the next authority. Returned when a
   * target-GUID line is AMBIGUOUS (both domains coherent, divergent codes)
   * or INVALID (no coherent domain). Callers that predate this value treat
   * any non-{-1,0,1} answer as unusable, which is the same safe outcome. */
  NXINPUT_PM_SOURCE_YIELDS = 2,
};

/* 0.10.0 -- the explicit domain classification of one mapping line against
 * the measured capabilities of the exact event node (contract 5.6). */
typedef enum nxinput_pm_domain_class {
  NXINPUT_PM_CLASS_INVALID = 0,          /* no coherent interpretation */
  NXINPUT_PM_CLASS_CURRENT_NATIVE,       /* only the current evdev domain */
  NXINPUT_PM_CLASS_LEGACY_JOYDEV_REWRITE,/* only the legacy joydev domain */
  NXINPUT_PM_CLASS_IDENTICAL_IN_BOTH,    /* both coherent, same exact codes */
  NXINPUT_PM_CLASS_AMBIGUOUS             /* both coherent, divergent codes */
} nxinput_pm_domain_class;

typedef struct nxinput_pm_evidence {
  unsigned int key_buttons;
  unsigned int gamepad_buttons;
  unsigned int lower_key_buttons;
  unsigned int button_bindings;
  unsigned int rewritten_bindings;
  unsigned int legacy_volume_markers;
  /* 0.10.0 tail (additive; consumers built before it must be recompiled
   * against this header before calling into a 0.10.0 archive). */
  unsigned int domain_class; /* nxinput_pm_domain_class of this line */
} nxinput_pm_evidence;

typedef struct nxinput_pm_source_evidence {
  unsigned int matching_lines;
  unsigned int rewritten_lines;
  unsigned int rewritten_bindings;
  unsigned int legacy_volume_markers;
  /* 0.10.0 tail (additive). */
  unsigned int native_lines;    /* CURRENT_NATIVE, byte-intact */
  unsigned int identical_lines; /* IDENTICAL_IN_BOTH, byte-intact */
  unsigned int ambiguous_lines; /* AMBIGUOUS: the source yielded */
  unsigned int invalid_lines;   /* INVALID: the source yielded */
} nxinput_pm_source_evidence;

/* Classify one mapping line without rewriting anything. `mapping` must be a
 * single line (no newline). Pure over the measured bitmap. */
nxinput_pm_domain_class nxinput_pm_classify_mapping(
    const char *mapping, const unsigned long *key_bits, size_t key_bit_count,
    nxinput_sdl_domain target_domain, nxinput_pm_evidence *evidence);

const char *nxinput_pm_domain_class_name(nxinput_pm_domain_class value);

/* Convert one mapping line. `target_domain` must be one of the measured
 * current SDL evdev domains. `key_bits` belongs to the exact event node whose
 * live GUID is `target_guid`. */
int nxinput_pm_convert_joydev_mapping(
    const char *mapping, const unsigned long *key_bits, size_t key_bit_count,
    nxinput_sdl_domain target_domain, const char *target_guid, char *output,
    size_t output_size, nxinput_pm_evidence *evidence);

/* Normalize only lines whose first field is the exact target GUID. Comments,
 * blank lines, bundle headers, non-target entries and native mappings remain
 * byte-identical. The caller projects SDL's zero-name-CRC alias first, so a
 * database entry can be selected without this layer guessing identity. */
int nxinput_pm_normalize_source(
    const char *source, const unsigned long *key_bits, size_t key_bit_count,
    nxinput_sdl_domain target_domain, const char *target_guid, char *output,
    size_t output_size, nxinput_pm_source_evidence *evidence);

#ifdef __cplusplus
}
#endif

#endif /* NXINPUT_PORTMASTER_H */
