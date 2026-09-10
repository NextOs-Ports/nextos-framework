/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxgl_quality -- the quality=low|medium|high lifecycle (V3), adapter-owned.
 *
 * WHY THIS EXISTS, AND WHY IT IS NEVER GLOBAL
 * -------------------------------------------
 * NEXTOSSETTINGS.txt carries `quality=auto|low|medium|high`, but the string is
 * only a REQUEST. What a tier MEANS (render scale, texture budget, effect set)
 * is a fact about ONE engine, measured by ONE adapter -- never a global nxgl
 * policy. A forced "high" would stutter a heavy port and a forced "low" would
 * needlessly blur a light one. So nxgl never interprets the tiers and never
 * picks knobs; it only carries the LEVEL through a small, verifiable lifecycle
 * and emits a one-line receipt at each stage, exactly as nxgl_config_request
 * judges-and-reports without ever selecting the config.
 *
 * THE LIFECYCLE (this is the "resolve -> apply -> ready" the audit asks for)
 *   resolve : the requested level is turned into a CONCRETE tier. "auto" is
 *             resolved to the tier the ADAPTER recommends (what it measured on
 *             its own engine), never to a global default.
 *   apply   : the adapter has configured its engine for the resolved tier.
 *   ready   : the engine confirmed the tier (first frame drawn with it). The
 *             device-side frame proof watches for this; here it is only marked.
 *
 * Everything is pure: no EGL, no I/O, no clock, fixed storage. The adapter
 * drives the state machine and writes the receipts wherever it logs events
 * (e.g. events.jsonl); nxgl only formats them.
 */
#ifndef NXGL_QUALITY_H
#define NXGL_QUALITY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXGL_QUALITY_API_VERSION 1u

/* The declared tiers. AUTO is the request "let the adapter choose"; it is
 * never a run-time tier -- resolve() turns it into LOW/MEDIUM/HIGH. */
typedef enum nxgl_quality_level {
  NXGL_QUALITY_AUTO = 0,
  NXGL_QUALITY_LOW,
  NXGL_QUALITY_MEDIUM,
  NXGL_QUALITY_HIGH,
  NXGL_QUALITY_LEVEL_COUNT
} nxgl_quality_level;

typedef enum nxgl_quality_stage {
  NXGL_QUALITY_STAGE_RESOLVE = 0,
  NXGL_QUALITY_STAGE_APPLY,
  NXGL_QUALITY_STAGE_READY,
  NXGL_QUALITY_STAGE_COUNT
} nxgl_quality_stage;

/* Parse a NEXTOSSETTINGS.txt quality value. Exactly the runtime allowlist
 * (auto|low|medium|high, case-sensitive). NULL or anything else -> AUTO, so a
 * corrupt value fails SAFE to "let the adapter choose", never to a hard tier. */
nxgl_quality_level nxgl_quality_parse(const char *value);

/* Canonical lowercase name of a level, "auto"/"low"/"medium"/"high"; a level
 * out of range returns "auto". Never NULL. */
const char *nxgl_quality_name(nxgl_quality_level level);

/* Resolve a requested level to a CONCRETE tier. A concrete request passes
 * through unchanged. AUTO resolves to `recommended` -- the tier the adapter
 * measured as its own default. If `recommended` is itself AUTO or out of
 * range, resolve falls to MEDIUM (a defined, middle floor -- still not a
 * device/brand decision). */
nxgl_quality_level nxgl_quality_resolve(nxgl_quality_level requested,
                                        nxgl_quality_level recommended);

/* The lifecycle state. Fixed storage, safe to memcpy; no pointers. */
typedef struct nxgl_quality_state {
  uint32_t api_version; /* NXGL_QUALITY_API_VERSION */
  size_t struct_size;   /* sizeof(nxgl_quality_state) */
  nxgl_quality_level requested;
  nxgl_quality_level resolved; /* concrete after init */
  int applied;                 /* apply() succeeded */
  int ready;                   /* ready() confirmed */
} nxgl_quality_state;

/* Initialise: record the request, resolve it against the adapter's
 * recommendation, and start un-applied / not-ready. Returns 0, or -1 on a NULL
 * state (state left untouched). */
int nxgl_quality_state_init(nxgl_quality_state *s,
                            nxgl_quality_level requested,
                            nxgl_quality_level recommended);

/* Mark the resolved tier applied by the adapter. Returns 0 on success, -1 if
 * `s` is NULL or the resolved tier is not concrete (LOW/MEDIUM/HIGH). */
int nxgl_quality_state_apply(nxgl_quality_state *s);

/* Mark the engine ready at the applied tier. Returns 0 on success, -1 if `s`
 * is NULL or apply() has not run first (order is enforced: no ready before
 * apply). */
int nxgl_quality_state_ready(nxgl_quality_state *s);

/* Write the one-line receipt for `stage` into `buf`, e.g.
 *   QUALITY: stage=resolve requested=auto resolved=medium applied=0 ready=0
 * Returns the length written (excluding the NUL), or 0 on bad args / short
 * buffer (buf then holds ""). Pure: no I/O. A stage the state has not reached
 * still formats (applied/ready flags tell the reader what actually happened). */
size_t nxgl_quality_receipt(const nxgl_quality_state *s,
                            nxgl_quality_stage stage,
                            char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NXGL_QUALITY_H */
