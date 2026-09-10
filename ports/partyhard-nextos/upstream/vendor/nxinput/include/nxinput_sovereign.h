/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxinput_sovereign -- V4-CONTROLLERS-03 / C3: the single mapping authority
 * order, with the PortMaster/CFW mapping sovereign.
 *
 * DOCTRINE
 * --------
 * The framework must use the same official mapping that makes PortMaster
 * ports work, and stop applying generic "corrections" after it. CFW
 * detection may locate the SUPPLIER and its official paths; a CFW name,
 * model, VID/PID or controller name never decides the A/B/L2/R2 order by
 * itself. A complete, reachable PortMaster entry is sovereign and is used
 * BYTE-INTACT.
 *
 * AUTHORITY ORDER (single, documented, no other path exists):
 *   1. non-empty mapping already delivered by the CFW's
 *      control.txt/get_controls (the PortMaster environment);
 *   2. the exact GUID entry in the CFW's current official database;
 *   3. the NXCONTROLLER_PROFILES/1 bundle pinned inside the port ZIP;
 *   4. the runtime's built-in database, when the runtime has one;
 *   5. raw passthrough, ONLY when the consumer declared it understands raw;
 *   6. explicit failure before gameplay when nothing above is reachable.
 *
 * Every step is validated by syntax, exact GUID, measured capabilities and
 * an EFFECTIVE readback: a setter whose readback is not semantically
 * identical never wins. Selecting the first line of a list whose GUID does
 * not match is forbidden -- a non-matching source simply yields to the next
 * authority, with the reason recorded.
 *
 * REAL-WORLD DATA RULE (field regression, 2026-08-31): the sources here are
 * the CFW's OWN files and environment, and real CFW data legally carries
 * duplicated GUID entries and metadata keys this parser has never heard of.
 * The runtime that will execute the mapping is SDL, so ambiguity is resolved
 * exactly the way SDL resolves it -- the LAST occurrence wins, for duplicate
 * GUID lines in a store and for duplicate binding keys inside one line --
 * and every tolerated divergence is COUNTED in the decision
 * (duplicate_lastwins) instead of failing the step. An unknown `key:value`
 * field is metadata, not an error. Failing closed remains reserved for what
 * is genuinely unusable: broken syntax, unreachable ordinals, a drifted
 * readback, or no source at all.
 *
 * PURE: no SDL, no environment, no I/O. The adapter feeds source contents
 * and measured capabilities, and injects the apply+readback effect. The
 * chosen line is returned byte-intact for the adapter to apply.
 */
#ifndef NXINPUT_SOVEREIGN_H
#define NXINPUT_SOVEREIGN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_SOVEREIGN_API_VERSION 1u
#define NXINPUT_SOVEREIGN_LINE_MAX 1024u
#define NXINPUT_SOVEREIGN_GUID_MAX 33u

/* The authority that won (or the explicit failure). Order is the contract. */
typedef enum nxinput_sovereign_source {
  NXINPUT_SOVEREIGN_ENV_GET_CONTROLS = 0,
  NXINPUT_SOVEREIGN_CFW_DB_GUID,
  NXINPUT_SOVEREIGN_PORT_BUNDLE,
  NXINPUT_SOVEREIGN_RUNTIME_BUILTIN,
  NXINPUT_SOVEREIGN_RAW_PASSTHROUGH,
  NXINPUT_SOVEREIGN_FAIL_EXPLICIT
} nxinput_sovereign_source;

/* Stable reasons for why a given authority step did not win. */
typedef enum nxinput_sovereign_reason {
  NXINPUT_SOVEREIGN_OK = 0,
  NXINPUT_SOVEREIGN_SOURCE_EMPTY,
  NXINPUT_SOVEREIGN_GUID_NOT_FOUND,
  NXINPUT_SOVEREIGN_SYNTAX_INVALID,
  NXINPUT_SOVEREIGN_DUPLICATE_DIVERGENT, /* retired as a verdict: duplicate
                                          * GUID entries now resolve last-wins
                                          * (SDL semantics) and are counted in
                                          * duplicate_lastwins. The value stays
                                          * so receipts and consumers that name
                                          * it keep compiling and parsing. */
  NXINPUT_SOVEREIGN_UNREACHABLE,         /* ordinal beyond measured caps */
  NXINPUT_SOVEREIGN_READBACK_MISMATCH,   /* setter accepted, semantics drifted */
  NXINPUT_SOVEREIGN_BUNDLE_HEADER_INVALID,
  NXINPUT_SOVEREIGN_CONSUMER_REFUSES_RAW,
  NXINPUT_SOVEREIGN_NOT_AVAILABLE,
  NXINPUT_SOVEREIGN_REQUEST_INVALID
} nxinput_sovereign_reason;

/* Measured physical capabilities of the pad (counts, not names). */
typedef struct nxinput_sovereign_caps {
  uint32_t api_version;
  size_t struct_size;
  int buttons;
  int axes;
  int hats;
} nxinput_sovereign_caps;

typedef struct nxinput_sovereign_request {
  uint32_t api_version;
  size_t struct_size;
  char guid[NXINPUT_SOVEREIGN_GUID_MAX]; /* lowercase hex, 32 chars */
  nxinput_sovereign_caps caps;
  int consumer_accepts_raw;   /* step 5 is legal only when 1 */
  int runtime_has_builtin;    /* step 4 exists only when 1 */
} nxinput_sovereign_request;

/* Injected effect: apply `line` to the real runtime and write the EFFECTIVE
 * mapping the runtime reports back into `readback` (NUL-terminated). Return
 * 0 on success, -1 when the setter or the readback failed. The core then
 * compares request line and readback SEMANTICALLY; accepting a setter
 * without an identical readback is forbidden. NULL means "validate
 * everything except the live readback" and is only legal for dry planning --
 * the decision then reports readback_checked=0 and MUST NOT be used to
 * authorize gameplay. */
typedef int (*nxinput_sovereign_readback_fn)(void *userdata, const char *line,
                                             char *readback, size_t cap);

typedef struct nxinput_sovereign_decision {
  uint32_t api_version;
  size_t struct_size;
  nxinput_sovereign_source source;
  nxinput_sovereign_reason reason; /* OK for a win; the terminal reason for
                                    * FAIL_EXPLICIT */
  /* Why each earlier authority yielded (index = nxinput_sovereign_source of
   * the step; OK means "did not reach this step"). */
  nxinput_sovereign_reason step_reason[5];
  char line[NXINPUT_SOVEREIGN_LINE_MAX]; /* the winning line, BYTE-INTACT */
  int readback_checked; /* 1 when the injected readback ran and matched */
  /* Divergent duplicate GUID entries resolved last-wins (SDL semantics)
   * across every source this resolution consulted. Evidence, not an error:
   * 0 means no source needed the tolerance. Trailing member; a consumer
   * compiled before it existed keeps working (decision_init zeroes it). */
  int duplicate_lastwins;
} nxinput_sovereign_decision;

int nxinput_sovereign_request_init(nxinput_sovereign_request *request);
int nxinput_sovereign_decision_init(nxinput_sovereign_decision *decision);

/* Validate one SDL2-dialect mapping line: GUID,name,key:value,... with known
 * keys, well-formed bN/aN/±aN/aN~/hN.M values and bounded length. Returns
 * NXINPUT_SOVEREIGN_OK or SYNTAX_INVALID. Pure. */
nxinput_sovereign_reason nxinput_sovereign_line_syntax(const char *line);

/* Are all ordinals referenced by `line` within the measured capabilities?
 * (bN < buttons, aN < axes, hN.M with N < hats). SYNTAX first. */
nxinput_sovereign_reason nxinput_sovereign_line_reachable(
    const char *line, const nxinput_sovereign_caps *caps);

/* Semantic comparison of two mapping lines: same GUID, and the exact same
 * key->value binding set regardless of field order (name is cosmetic and
 * ignored; an axis is never equal to a button). Returns 1 when identical. */
int nxinput_sovereign_semantically_identical(const char *a, const char *b);

/* Find the entry for `guid` in a newline-separated database. Exact GUID
 * only: when the GUID does not appear, the answer is GUID_NOT_FOUND -- the
 * first line is NEVER chosen on a mismatch. Two entries with the same GUID
 * and different bodies are DUPLICATE_DIVERGENT (order must never decide);
 * a byte-identical duplicate is fine. On success the line is copied
 * byte-intact into `out`. */
nxinput_sovereign_reason nxinput_sovereign_db_lookup(
    const char *database, const char *guid, char *out, size_t cap);

/* NXCONTROLLER_PROFILES/1 bundle: validate the header (first line must be
 * exactly "NXCONTROLLER_PROFILES/1"; '#' comment/metadata lines follow) and
 * look up `guid` among its mapping lines under the same exact-GUID rules. */
nxinput_sovereign_reason nxinput_sovereign_bundle_lookup(
    const char *bundle, const char *guid, char *out, size_t cap);

/* The single authority resolution. Each source may be NULL/"" when absent.
 * The winning line is validated by syntax -> exact GUID -> capabilities ->
 * injected readback (semantic identity); a step that fails any validation
 * yields to the next authority with its reason recorded. When nothing is
 * reachable the decision is FAIL_EXPLICIT -- the caller must refuse to
 * reach gameplay. Returns 0 (the decision struct carries the outcome), -1
 * only on structurally invalid arguments. */
int nxinput_sovereign_resolve(
    const nxinput_sovereign_request *request,
    const char *env_get_controls_mapping,
    const char *cfw_database,
    const char *port_bundle,
    nxinput_sovereign_readback_fn readback, void *userdata,
    nxinput_sovereign_decision *decision);

const char *nxinput_sovereign_source_name(nxinput_sovereign_source source);
const char *nxinput_sovereign_reason_name(nxinput_sovereign_reason reason);

#ifdef __cplusplus
}
#endif

#endif /* NXINPUT_SOVEREIGN_H */
