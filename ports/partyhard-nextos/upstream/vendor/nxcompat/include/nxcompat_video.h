/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxcompat_video.h -- pure, versioned geometry for the NEXTOS_SETTINGS/2
 * video namespace (nxcompat 0.5.0, V5 mission 7A.3). No I/O, no device
 * names: only numbers in, numbers out.
 *
 *   requested (owner)  != drawable (what the display really gave)
 *                      != logical viewport (what the engine renders)
 *
 * Content rect of a source Ws×Hs presented on a drawable Wd×Hd:
 *   stretch  : the whole drawable (deliberate distortion, reported as such)
 *   preserve : scale = min(Wd/Ws, Hd/Hs), largest centered rect, Ws/Hs kept
 *              (rounding: size = floor(dim*scale), offset = floor(rest/2);
 *               odd bars land the extra pixel at the bottom/right)
 *   crop     : scale = max(...), centered, clipped by the drawable (the rect
 *              is larger than the drawable; offsets may be negative)
 *   integer  : largest integer factor k >= 1 with k*Ws <= Wd and k*Hs <= Hd,
 *              centered; UNSUPPORTED when k < 1 (reported, never faked)
 *   engine   : inherit / no-op inside the elected authority: the rect is the
 *              drawable and `effective` says ENGINE (nothing is asserted)
 *   auto     : NOT a policy by itself. Each port declares ONE total,
 *              versioned algorithm; three are provided here:
 *                nxcompat_video_auto_stretch:
 *                  always stretch. This preserves a port's established
 *                  full-panel presentation on every drawable; an owner who
 *                  wants letterboxing selects `preserve` explicitly.
 *                nxcompat_video_auto_ratio_threshold (Tearscape rule 2.2C):
 *                  Rd = Wd/Hd; Rd <= threshold(1.20) -> preserve, else stretch
 *                nxcompat_video_auto_epsilon (Blossom rule 7A.3):
 *                  |Rd - Rs| <= epsilon -> engine (no-op), else preserve
 *              A port without a declared auto algorithm must REJECT `auto`.
 *
 * Normative examples: 16:9 (1280×720) on 720×720 preserve = (0,157,720,405)
 * with bars 157/158; on 640×480 = (0,60,640,360).
 *
 * Touch/cursor: nxcompat_video_drawable_to_source applies the INVERSE of the
 * same rect (drawable pixel -> source pixel); outside the rect is reported.
 */
#ifndef NXCOMPAT_VIDEO_H
#define NXCOMPAT_VIDEO_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define NXCOMPAT_VIDEO_API_VERSION 1u
#define NXCOMPAT_VIDEO_AUTO_RATIO_THRESHOLD_DEFAULT 1.20
#define NXCOMPAT_VIDEO_AUTO_EPSILON_DEFAULT 0.01

typedef enum nxcompat_video_aspect {
  NXCOMPAT_VIDEO_ASPECT_ENGINE = 0,
  NXCOMPAT_VIDEO_ASPECT_PRESERVE,
  NXCOMPAT_VIDEO_ASPECT_STRETCH,
  NXCOMPAT_VIDEO_ASPECT_CROP,
  NXCOMPAT_VIDEO_ASPECT_INTEGER,
  NXCOMPAT_VIDEO_ASPECT_AUTO      /* only as REQUESTED, never as effective */
} nxcompat_video_aspect;

typedef struct nxcompat_video_rect { int x, y, w, h; } nxcompat_video_rect;

typedef struct nxcompat_video_decision {
  unsigned api_version;
  nxcompat_video_aspect requested, effective;
  int source_w, source_h, drawable_w, drawable_h;
  nxcompat_video_rect content;        /* in drawable pixels */
  int bar_left, bar_right, bar_top, bar_bottom; /* opaque bars (>=0); 0 for stretch */
  int integer_factor;                 /* integer policy: k; else 0 */
  int unsupported;                    /* 1 when the policy cannot be honoured */
  int distorted;                      /* 1 when Ws/Hs is not kept (stretch) */
  char reason[64];
} nxcompat_video_decision;

/* Parse the settings token. Returns 0, or -1 for an unknown token. */
int nxcompat_video_aspect_from_string(const char *token, nxcompat_video_aspect *out);
const char *nxcompat_video_aspect_name(nxcompat_video_aspect a);

/* Resolve a CONCRETE policy (not auto) into a decision. Returns 0, or -1 on
 * invalid geometry/`auto` (a port must resolve auto through one of the two
 * declared algorithms below first). */
int nxcompat_video_content_rect(nxcompat_video_aspect policy,
                                int source_w, int source_h,
                                int drawable_w, int drawable_h,
                                nxcompat_video_decision *out);

/* Total `auto` algorithms (versioned; changing one needs its own physical proof). */
nxcompat_video_aspect nxcompat_video_auto_stretch(void);
nxcompat_video_aspect nxcompat_video_auto_ratio_threshold(int drawable_w, int drawable_h, double threshold);
nxcompat_video_aspect nxcompat_video_auto_epsilon(int source_w, int source_h, int drawable_w, int drawable_h, double epsilon);

/* Inverse transform for touch/cursor. Returns 1 inside the content rect
 * (source coordinates written), 0 outside (clamped source coordinates
 * written, so a cursor can still be positioned at the edge). */
int nxcompat_video_drawable_to_source(const nxcompat_video_decision *d, int dx, int dy, int *sx, int *sy);
/* Forward transform: source pixel -> drawable pixel. */
void nxcompat_video_source_to_drawable(const nxcompat_video_decision *d, int sx, int sy, int *dx, int *dy);

/* Sanitized receipt line: `NX-VIDEO/1 authority=.. source=.. requested=..
 * effective=.. src=WxH drawable=WxH content=x,y,w,h bars=l,r,t,b reason=..`.
 * `authority` and `source` are the elected authority and the winning source
 * (settings|port-env|engine|package-default) -- strings owned by the caller. */
int nxcompat_video_receipt(const nxcompat_video_decision *d, const char *authority, const char *source, char *out, size_t cap);

/* ===================================================================== *
 * nxcompat 0.5.1 / V5 FV3 + 7A.2: the SINGLE owner authority for aspect
 * ---------------------------------------------------------------------
 * Before 0.5.1 each port re-derived "who decides the aspect" (Tearscape
 * carried its own `nx_aspect_policy.h` with the same ratio rule). One
 * decision, one place: a port hands over the owner's raw tokens plus the
 * real drawable and receives the elected authority, the winning source and
 * the geometry. What stays in the port is only the translation of the
 * effective policy into the engine's own vocabulary (Godot: preserve->keep,
 * stretch->ignore) -- never a rule about ratios or precedence.
 *
 * Election (7A.2). The `video.*` namespace elects ATOMICALLY one authority:
 *   nextos        NEXTOSSETTINGS.txt (/2) and only allowlisted video exports
 *                 of port-env.sh are sources of THIS resolver; the engine's
 *                 native config is a projected sink + readback, never a
 *                 competing writer.
 *   engine        the native config/UI owns the key; settings and hook must
 *                 stay in inherit ("engine"/absent) for it. Anything else is
 *                 an OVERLAP and fails BEFORE init (never last-writer-wins).
 *   synchronized  both participate only through a CAS transaction: the caller
 *                 must carry a nonzero cas_generation (the
 *                 video_config_generation it read), or the resolve fails.
 *
 * Precedence inside `nextos`/`synchronized` (mission M1b item 1):
 *   port-env.sh export  >  NEXTOSSETTINGS.txt  >  owner-EDITED native config
 *   >  the port's declared total `auto` algorithm  >  package default.
 * The two settings-tier sources are ordered by the observable launcher order
 * (NX-OWNER-RUNTIME/1: the owner hook is sourced last, so it is the owner's
 * last word). An UNEDITED native config is a package default and therefore
 * ranks BELOW auto -- an owner edit of it outranks auto, which is exactly
 * the rule Tearscape proved and every Godot port now inherits.
 *
 * `auto` is never a heuristic: a port that declares no total algorithm must
 * REJECT the token instead of quietly preserving (auto_algorithm_declared=0).
 * ===================================================================== */

#define NXCOMPAT_VIDEO_OWNER_API_VERSION 1u

typedef enum nxcompat_video_authority {
  NXCOMPAT_VIDEO_AUTHORITY_NEXTOS = 0,
  NXCOMPAT_VIDEO_AUTHORITY_ENGINE,
  NXCOMPAT_VIDEO_AUTHORITY_SYNCHRONIZED
} nxcompat_video_authority;

typedef enum nxcompat_video_source {
  NXCOMPAT_VIDEO_SOURCE_NONE = 0,
  NXCOMPAT_VIDEO_SOURCE_PORT_ENV,        /* allowlisted export of port-env.sh */
  NXCOMPAT_VIDEO_SOURCE_SETTINGS,        /* NEXTOSSETTINGS.txt (/2) */
  NXCOMPAT_VIDEO_SOURCE_NATIVE_CONFIG,   /* owner-edited engine config */
  NXCOMPAT_VIDEO_SOURCE_AUTO,            /* the declared total algorithm */
  NXCOMPAT_VIDEO_SOURCE_PACKAGE_DEFAULT  /* the untouched seed */
} nxcompat_video_source;

/* Parse the authority token. Returns 0, or -1 for an unknown token.
 * An EMPTY/NULL token is the documented default `nextos`. */
int nxcompat_video_authority_from_string(const char *token, nxcompat_video_authority *out);
const char *nxcompat_video_authority_name(nxcompat_video_authority a);
const char *nxcompat_video_source_name(nxcompat_video_source s);

typedef struct nxcompat_video_owner_input {
  /* Raw owner tokens. NULL or "" means ABSENT (never a hidden value). */
  const char *settings_authority;   /* video.authority */
  const char *settings_aspect;      /* video.aspect */
  const char *port_env_aspect;      /* NX_VIDEO_ASPECT export (allowlisted) */
  /* The engine's native config, already translated by the port's adapter
   * into a schema token (Godot keep->"preserve", ignore->"stretch"). */
  const char *native_config_aspect;
  int native_config_present;        /* the declared native config exists */
  int native_config_edited;         /* owner moved it off the packaged seed */
  /* The port's ONE declared total algorithm, already applied to the real
   * drawable (nxcompat_video_auto_stretch / _auto_ratio_threshold /
   * _auto_epsilon / a future versioned one). Only read when the winning
   * token is `auto`. */
  int auto_algorithm_declared;
  nxcompat_video_aspect auto_algorithm_result;
  /* The package default for the key, used only when nothing else spoke. */
  nxcompat_video_aspect package_default;
  /* Geometry: the LOGICAL source and the REAL drawable (never the request). */
  int source_w, source_h, drawable_w, drawable_h;
  /* video_config_generation read by the caller; required by `synchronized`. */
  unsigned cas_generation;
} nxcompat_video_owner_input;

typedef struct nxcompat_video_owner_decision {
  unsigned api_version;
  nxcompat_video_authority authority;
  nxcompat_video_source source;          /* the WINNING source */
  nxcompat_video_aspect requested;       /* the winning token as written */
  int via_auto;                          /* 1 = resolved by the algorithm */
  int native_config_apply;               /* 1 = project onto the native config */
  unsigned cas_generation;
  nxcompat_video_decision geometry;      /* effective content rect */
  int failed;                            /* 1 = refuse to launch */
  char reason[96];
} nxcompat_video_owner_decision;

/* Elect the authority and resolve the aspect in ONE call.
 * Returns 0 when the decision is usable, -1 when the port must fail before
 * init (`out->failed` is 1 and `out->reason` names the cause: overlap
 * outside the election, `auto` with no declared algorithm, unknown token,
 * `synchronized` with no CAS generation, invalid geometry). */
int nxcompat_video_resolve_owner(const nxcompat_video_owner_input *in,
                                 nxcompat_video_owner_decision *out);

/* Receipt of the elected decision, NX-VIDEO/1 with the authority and source
 * this module elected (never a caller-supplied label). */
int nxcompat_video_owner_receipt(const nxcompat_video_owner_decision *d, char *out, size_t cap);

/* ---- Readback (7A.2: "never approve because the file was merely read") ---- */
typedef struct nxcompat_video_readback {
  unsigned api_version;
  int drawable_w, drawable_h;        /* MEASURED after the first present */
  nxcompat_video_rect content;       /* MEASURED content rect */
  nxcompat_video_aspect effective;   /* what the engine reports it applied */
  unsigned cas_generation;           /* video_config_generation observed */
} nxcompat_video_readback;

/* Compare a measured readback against the elected decision.  Returns 0 on a
 * full match; -1 on any mismatch, with `out` (when given) carrying
 * `NX-VIDEO-READBACK/1 ... match=0 mismatch=<field>` naming the FIRST field
 * that differs.  A readback whose api_version is 0 (nothing measured) is a
 * mismatch, so "the adapter only read the file" can never pass. */
int nxcompat_video_readback_check(const nxcompat_video_owner_decision *want,
                                  const nxcompat_video_readback *got,
                                  char *out, size_t cap);
#ifdef __cplusplus
}
#endif
#endif
