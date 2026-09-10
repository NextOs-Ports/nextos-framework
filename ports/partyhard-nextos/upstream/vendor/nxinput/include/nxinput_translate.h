/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_TRANSLATE_H
#define NXINPUT_TRANSLATE_H
/*
 * nxinput_translate -- V5 (0.11.0): the PHYSICAL TRANSLATION of a mapping
 * from its SOURCE ordinal domain into the CONSUMER PROVIDER's ordinal domain.
 *
 * Replaces the V4 `nxinput_pm_normalize_source` decision (kept for the
 * legacy gate only), which knew ONE source dialect ("joydev-legacy", the
 * all-keys ascending sweep) and ONE presumed target (upstream by major).
 *
 *   source line  bN / aN / hN.mask (+ sign, inversion, half-axis suffixes)
 *     -> source ordinal domain  (declared by a source descriptor, or proved
 *                                by exclusion against the measured caps)
 *     -> typed physical identity (EV_KEY / EV_ABS / hat pair)
 *     -> consumer provider ordinal domain (nxinput_provider descriptor)
 *     -> line rewritten, or byte-intact when source == provider
 *
 * Every step is reversible and every option/flag is preserved. Nothing is
 * done when the provider is UNKNOWN or when the source domain cannot be
 * proved: the line stays byte-intact and the receipt says so.
 */
#include "nxinput_sdl.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum nxinput_translate_result {
  NXINPUT_TRANSLATE_ERROR = -1,
  NXINPUT_TRANSLATE_BYTE_INTACT_NATIVE = 0,   /* source == provider */
  NXINPUT_TRANSLATE_REWRITTEN = 1,            /* source != provider, proved */
  NXINPUT_TRANSLATE_BYTE_INTACT_UNPROVEN = 2, /* provider UNKNOWN or source
                                                 ambiguous: untouched */
  NXINPUT_TRANSLATE_REJECTED = 3              /* no coherent reading at all */
} nxinput_translate_result;

/* How the source domain became known. */
typedef enum nxinput_source_provenance {
  NXINPUT_SOURCE_UNDECLARED = 0,     /* prove by exclusion, else unproven */
  NXINPUT_SOURCE_DECLARED_BY_PRODUCER, /* descriptor from the helper/CFW that
                                          authored the line (same provider) */
  NXINPUT_SOURCE_PINNED_BUNDLE       /* NXCONTROLLER_PROFILES bundle whose
                                        domain is declared in its header */
} nxinput_source_provenance;

typedef struct nxinput_source_descriptor {
  uint8_t provenance;   /* nxinput_source_provenance */
  uint8_t domain;       /* nxinput_sdl_domain when declared; UNDECLARED
                           means "prove it" */
} nxinput_source_descriptor;

typedef struct nxinput_translate_evidence {
  uint8_t source_domain;      /* the domain finally used, UNDECLARED if none */
  uint8_t provider_domain;
  uint8_t source_proved_by;   /* 0 none, 1 declared, 2 exclusion, 3 identical */
  unsigned int button_bindings, axis_bindings, hat_bindings;
  unsigned int rewritten_bindings;
  unsigned int coherent_domains; /* how many candidate domains read the line */
  /* 0.11.1: the line's own flags, for the integral equivalence (1.4/C2):
   * +aN/-aN present, aN~ present, lefttrigger/righttrigger bound to bN. */
  uint8_t half_axis, inversion, trigger_as_button;
} nxinput_translate_evidence;

/* Translate ONE mapping line (guid,name,fields...). `key_bits`/`abs_bits`
 * are the MEASURED capability bitmaps of the exact node. `out` receives the
 * (possibly identical) line. */
nxinput_translate_result nxinput_translate_line(
    const char *line, const unsigned long *key_bits, size_t key_bit_count,
    const unsigned long *abs_bits, size_t abs_bit_count,
    nxinput_sdl_domain provider_domain,
    const nxinput_source_descriptor *source, char *out, size_t cap,
    nxinput_translate_evidence *evidence);

const char *nxinput_translate_result_name(nxinput_translate_result r);
#ifdef __cplusplus
}
#endif
#endif
