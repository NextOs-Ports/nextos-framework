/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_PROVIDER_H
#define NXINPUT_PROVIDER_H
/*
 * nxinput_provider -- V5 (nxinput 0.11.0): the PROVIDER DESCRIPTOR.
 *
 * THE DEFECT THIS FIXES (P0, the two P0 field incidents, 2026-09-03)
 * -----------------------------------------------------------------
 * V4 reduced every SDL2 to `nxinput_sdl_api_domain(API_2) == SDL2_EVDEV`
 * (upstream high-first) and used that presumption as the TARGET of the C6
 * mapping rewrite. the RetroArch-derived-derived CFW family ship an SDL2 carrying the
 * RetroArch/RetroArch-derived patch set whose ConfigJoystick numbers EVERY EV_KEY in
 * one ascending sweep. Their own mappings are native to that provider; the
 * V4 rewrite projected them into the upstream order and every ordinal that a
 * low key precedes moved: A became a volume key, L1 became START, SELECT
 * became L3, START became L2. The pad was admitted, GPTK was alive, and the
 * game was deaf.
 *
 *     major/API of the SDL  !=  ordinal algorithm of the provider loaded
 *
 * THE CONTRACT
 * ------------
 * A provider descriptor names the SDL that the PROCESS REALLY MAPPED, with:
 *   - api (major), version string, soname, sanitized path class;
 *   - the bytes: sha256 of the mapped object, its ELF Build ID (if any), the
 *     dev/inode of the mapping and whether the sha was taken from a FD that
 *     `fstat`s to that same object (mapped-object-bound);
 *   - the ordinal domain (nxinput_sdl_domain) of buttons/axes/hats, and the
 *     METHOD that produced it, in evidence order:
 *       1. MEASURED_INPROCESS  (0.11.1) the table the provider REALLY built
 *                         for THIS device, read back through the exported
 *                         SDL_Joystick{Button,Axis,Hat}EventCodeById API of
 *                         the mapped object (post-open, per instance) and
 *                         matched against the transcribed plans. The
 *                         PRESENCE of the ById symbols (EXPORTED_API) proves
 *                         only that a table can be measured -- never which
 *                         table: an EXPORTED_API descriptor stays UNDECLARED
 *                         (no rewrite) until the measurement lands;
 *       2. PINNED_ELF     the sha256 of the mapped object matches a pinned
 *                         entry of the provider manifest (compiled in, or a
 *                         runtime pin file the harness declares) whose table
 *                         was reproduced from source/patch;
 *       3. UNKNOWN        nothing above holds. NO REWRITE IS EVER DONE; the
 *                         pad keeps the provider's STOCK behaviour (0.11.1:
 *                         never muted -- see nxinput_sdl_seam stock mode).
 *   - joystick driver per device (evdev / hidapi / virtual / unknown);
 *   - the generation at which it was resolved.
 *
 * What it NEVER uses to decide the domain: CFW name, device name, GUID,
 * VID/PID, volume markers, the SDL major alone, or `ldconfig`.
 *
 * PURE CORE: this header and nxinput_provider.c have no I/O. The Linux
 * probe that fills the raw evidence (dladdr, /proc/self/maps, map_files,
 * fstat, sha256, ELF notes, dlsym) lives in nxinput_provider_linux.c.
 */
#include "nxinput_sdl.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_PROVIDER_API_VERSION 1u
#define NXINPUT_PROVIDER_SHA256_HEX 65u
#define NXINPUT_PROVIDER_BUILD_ID_HEX 41u
#define NXINPUT_PROVIDER_TEXT_MAX 96u

typedef enum nxinput_provider_method {
  NXINPUT_PROVIDER_METHOD_UNKNOWN = 0,
  NXINPUT_PROVIDER_METHOD_EXPORTED_API,
  NXINPUT_PROVIDER_METHOD_PINNED_ELF,
  /* Reserved for the in-process measured table (post-open, per device). */
  NXINPUT_PROVIDER_METHOD_MEASURED_INPROCESS,
  /* A STATICALLY linked provider (engine with SDL built in): the port
   * declares the pinned SOURCE identity it was built from (tarball +
   * patch shas in the manifest) and the table measured for that source.
   * Only accepted when the evidence proves SDL_Init lives in the main
   * program; a DSO can never be declared this way (5.1: explicit, never
   * "faking a DSO"). */
  NXINPUT_PROVIDER_METHOD_DECLARED_STATIC_SOURCE,
  NXINPUT_PROVIDER_METHOD_COUNT
} nxinput_provider_method;

typedef enum nxinput_provider_driver {
  NXINPUT_PROVIDER_DRIVER_UNKNOWN = 0,
  NXINPUT_PROVIDER_DRIVER_EVDEV,
  NXINPUT_PROVIDER_DRIVER_HIDAPI,
  NXINPUT_PROVIDER_DRIVER_VIRTUAL,
  NXINPUT_PROVIDER_DRIVER_STATIC_BUILTIN
} nxinput_provider_driver;

/* Raw evidence the probe collected. Everything here is a FACT about the
 * process, not a decision. */
typedef struct nxinput_provider_evidence {
  uint32_t api_version;
  uint32_t struct_size;
  uint8_t api;                     /* nxinput_sdl_api */
  uint8_t has_exported_bytable;    /* all four ById symbols resolved in the
                                      same object as SDL_Init */
  uint8_t sha_bound_to_mapping;    /* 1: sha256 came from map_files or a FD
                                      whose fstat dev+inode equal the mapped
                                      object; 2: inode equal on a virtual
                                      (major 0) device where maps reports
                                      the lower dev (overlayfs): accepted
                                      with reduced confidence; 0: unbound */
  uint8_t statically_linked;       /* SDL_Init lives in the main program */
  uint8_t compat_over_sdl3;        /* 0.11.1 (B8): the SDL2 ABI is sdl2-compat,
                                      a shim over the SDL3 core mapped in the
                                      same process (found in the mapped bytes):
                                      SDL2 and SDL3 then share ONE store and
                                      ONE ordinal backend -- never two providers */
  uint64_t map_dev;
  uint64_t map_inode;
  uint64_t map_offset;
  char sha256[NXINPUT_PROVIDER_SHA256_HEX];
  char build_id[NXINPUT_PROVIDER_BUILD_ID_HEX]; /* "" when absent */
  char soname[NXINPUT_PROVIDER_TEXT_MAX];
  char version[NXINPUT_PROVIDER_TEXT_MAX];   /* SDL_GetVersion "2.30.12" */
  char revision[NXINPUT_PROVIDER_TEXT_MAX];  /* SDL_GetRevision, bounded */
  char path_class[NXINPUT_PROVIDER_TEXT_MAX]; /* sanitized: "system-lib",
                                                 "portmaster-lib", "main",
                                                 "other" */
} nxinput_provider_evidence;

/* One pinned provider: a sha256 whose ordinal table was reproduced from a
 * pinned source/patch by an independent tool (tests/providers). */
typedef struct nxinput_provider_pin {
  const char *sha256;
  const char *id;          /* stable manifest id */
  uint8_t api;             /* nxinput_sdl_api */
  uint8_t domain;          /* nxinput_sdl_domain; UNDECLARED = pinned as
                              UNKNOWN on purpose (bytes known, table not) */
} nxinput_provider_pin;

/* The descriptor: evidence + decision. */
typedef struct nxinput_provider_descriptor {
  uint32_t api_version;
  uint32_t struct_size;
  nxinput_provider_evidence evidence;
  uint8_t method;          /* nxinput_provider_method */
  uint8_t domain;          /* nxinput_sdl_domain; UNDECLARED when UNKNOWN */
  uint8_t driver;          /* nxinput_provider_driver (per device, later) */
  uint8_t confidence;      /* 0..100, documentation only */
  uint32_t provider_generation;
  const char *pin_id;      /* manifest id when method == PINNED_ELF */
  /* 0.11.1 tail. */
  uint8_t table_api_available; /* ById symbols resolved in the same object */
  uint8_t measured;            /* a MEASURED_INPROCESS table decided `domain` */
  uint8_t measurement_conflict;/* measured table matched NO known plan */
  uint8_t measured_ambiguous;  /* number of plans the measured table matched
                                  (identical numbering for this pad) */
} nxinput_provider_descriptor;

/* 0.11.1: one in-process MEASUREMENT of the provider's ordinal table for one
 * opened device, as the provider itself reports it (ById API). */
#define NXINPUT_PROVIDER_MEASURE_BUTTONS 64u
#define NXINPUT_PROVIDER_MEASURE_AXES 32u
#define NXINPUT_PROVIDER_MEASURE_HATS 4u
typedef struct nxinput_provider_measurement {
  unsigned buttons, axes, hats;   /* how many ordinals the provider names */
  int button_code[NXINPUT_PROVIDER_MEASURE_BUTTONS]; /* ordinal -> EV_KEY */
  int axis_code[NXINPUT_PROVIDER_MEASURE_AXES];      /* ordinal -> EV_ABS */
  int hat_code[NXINPUT_PROVIDER_MEASURE_HATS];       /* hat -> ABS_HATnX */
  int instance_id;
} nxinput_provider_measurement;

/* Match a measurement against every transcribed plan of `api` over the
 * MEASURED caps. `matched` receives the first domain whose complete
 * button/axis/hat numbering equals the measurement (UNDECLARED when none);
 * `ambiguous` the number of plans that matched (2+ = identical numbering for
 * this pad: translation between them is the identity). Returns 0, -1 on
 * invalid input. Pure. */
struct nxinput_godot_caps;
int nxinput_provider_measurement_match(const nxinput_provider_measurement *m,
                                       const struct nxinput_godot_caps *caps,
                                       uint8_t api, uint8_t *matched,
                                       uint8_t *ambiguous);

/* Apply a measurement to a descriptor: a matched domain => method
 * MEASURED_INPROCESS (confidence 99); no match => UNKNOWN with
 * measurement_conflict=1 (an EXPORTED_API claim never survives a table it
 * cannot reproduce). Returns 0, -1 on invalid input. */
/* 0.11.4 (review 2, N1): the SET of plans (bitmask by nxinput_sdl_domain)
 * that reproduce the measured table; apply against the set:
 *   - decided descriptor (pin / declared static / earlier measurement) inside
 *     the set => CONFIRMED (measured=1); outside a non-empty set => UNKNOWN
 *     with measurement_conflict=1 (never replaced by another plan);
 *   - undecided + exactly one plan => MEASURED_INPROCESS;
 *   - undecided + several plans => NOT decided (measured=0, stock mode) --
 *     a pad numbered identically by several plans decides nothing, the next
 *     admitted pad is measured on its own;
 *   - empty set => UNKNOWN, measurement_conflict=1. */
int nxinput_provider_measurement_plans(const nxinput_provider_measurement *m,
                                       const struct nxinput_godot_caps *caps,
                                       uint8_t api, uint32_t *mask);
int nxinput_provider_apply_measurement_set(nxinput_provider_descriptor *d,
                                           const nxinput_provider_measurement *m,
                                           uint32_t mask);
int nxinput_provider_apply_measurement(nxinput_provider_descriptor *d,
                                       const nxinput_provider_measurement *m,
                                       uint8_t matched, uint8_t ambiguous);

/* 0.11.1: RUNTIME pins (evidence order 2, declared by a harness or a CFW
 * integration for a DSO it built from a pinned source). The table is
 * consulted after the compiled-in pins; the caller keeps the storage alive.
 * NULL/0 clears. */
void nxinput_provider_set_runtime_pins(const nxinput_provider_pin *pins,
                                       size_t count);

/* The compiled-in pin table (generated from
 * tests/providers/provider-manifest-v5.json; the gate compares both). */
const nxinput_provider_pin *nxinput_provider_pins(size_t *count);

/* Look a sha256 up in the pin table. NULL when unknown. */
const nxinput_provider_pin *nxinput_provider_pin_lookup(const char *sha256);

/* THE decision. Pure. Fills `out` from `evidence` in the evidence order
 * documented above. Never returns a domain without a method that justifies
 * it; UNKNOWN is the honest default. Returns 0, or -1 on invalid input. */
int nxinput_provider_resolve(const nxinput_provider_evidence *evidence,
                             uint32_t provider_generation,
                             nxinput_provider_descriptor *out);

/* Static provider declaration (see METHOD_DECLARED_STATIC_SOURCE). `evidence`
 * must say statically_linked=1; `pin_id` names the manifest row of the
 * pinned source; `domain` is the table measured for it. Returns 0, or -1
 * when the evidence is not a static main-program provider (the descriptor
 * then stays whatever resolve() decided, usually UNKNOWN). */
int nxinput_provider_declare_static(const nxinput_provider_evidence *evidence,
                                    const char *pin_id, uint8_t domain,
                                    uint32_t provider_generation,
                                    nxinput_provider_descriptor *out);

/* 0.11.1 (B8): do two descriptors of DIFFERENT majors share one core?
 * (sdl2-compat over SDL3 in the same process). 1 = shared core: the pair is
 * ONE provider behind two ABIs; every "two majors isolated" claim must
 * fail closed and the stores are known to be shared by construction. */
int nxinput_provider_shared_core(const nxinput_provider_descriptor *sdl2,
                                 const nxinput_provider_descriptor *sdl3);

/* Two descriptors are the SAME PROVIDER INSTANCE only when api, sha256 and
 * generation all agree. Different majors are never equal, whatever the
 * table says (5.7: no shared vtable/store/table across majors). */
int nxinput_provider_same_instance(const nxinput_provider_descriptor *a,
                                   const nxinput_provider_descriptor *b);

/* Is rewriting a source into this provider's ordinal domain permitted at
 * all? Only when method != UNKNOWN. */
int nxinput_provider_allows_rewrite(const nxinput_provider_descriptor *d);

const char *nxinput_provider_method_name(nxinput_provider_method m);
const char *nxinput_provider_driver_name(nxinput_provider_driver d);

/* One sanitized receipt line, "NXC6-PROVIDER ..." without newline. Never a
 * personal path: only path_class. Returns chars written or -1. */
int nxinput_provider_receipt_line(const nxinput_provider_descriptor *d,
                                  char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
