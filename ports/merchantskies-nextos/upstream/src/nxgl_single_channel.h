/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Pure policy for an explicitly opted-in single-channel texture adapter.
 * RED, ALPHA and LUMINANCE sampling semantics remain distinct; unknown,
 * multi-channel and compressed formats are never rewritten.
 */
#ifndef NXGL_SINGLE_CHANNEL_H
#define NXGL_SINGLE_CHANNEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXGL_SINGLE_CHANNEL_API_VERSION 2u

typedef enum nxgl_single_channel_route {
  NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE = 0,
  NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP = 1,
  NXGL_SC_ROUTE_PASSTHROUGH = 2
} nxgl_single_channel_route;

typedef struct nxgl_texture_capabilities {
  uint32_t api_version;
  size_t struct_size;
  int physical_es3;
  int has_tex_storage;
  int has_texture_swizzle;
  int has_gl_red;
} nxgl_texture_capabilities;

/* Invalid/unmeasured capabilities select PASSTHROUGH. A coherent measured
 * GLES2 tuple may select the legacy fallback, but using it still requires an
 * explicit semantic configuration in the runtime adapter. */
nxgl_single_channel_route nxgl_single_channel_decide(
    const nxgl_texture_capabilities *caps);
int nxgl_single_channel_coherent(nxgl_single_channel_route storage_route,
                                 nxgl_single_channel_route upload_route);
const char *nxgl_single_channel_route_name(nxgl_single_channel_route route);
size_t nxgl_single_channel_receipt(const nxgl_texture_capabilities *caps,
                                   nxgl_single_channel_route route,
                                   char *buf, size_t cap);

typedef enum nxgl_sc_channel_kind {
  NXGL_SC_KIND_RGBA = 0, /* unknown/multi-channel/compressed: untouched */
  NXGL_SC_KIND_RED = 1,
  NXGL_SC_KIND_ALPHA = 2,
  NXGL_SC_KIND_LUMINANCE = 3,
  NXGL_SC_KIND_INVALID = 4 /* contradictory single-channel declaration */
} nxgl_sc_channel_kind;

typedef enum nxgl_sc_semantic {
  NXGL_SC_SEMANTIC_PRESERVE = 1,
  /* Explicit atlas contract: sample source value as (1,1,1,coverage), even
   * when the source enum is RED. */
  NXGL_SC_SEMANTIC_ALPHA_MASK = 2,
  /* Explicit RED coverage compatibility contract. The native route stays
   * R8/RED with the alpha-mask swizzle; only the legacy fallback expands each
   * RED byte into LUMINANCE_ALPHA as (R,R). */
  NXGL_SC_SEMANTIC_RED_COVERAGE_COMPAT = 3
} nxgl_sc_semantic;

/* Pair classification is strict. Known-but-different kinds, or one known and
 * one unknown side, are INVALID and must be rejected rather than guessed. */
nxgl_sc_channel_kind nxgl_single_channel_classify(unsigned gl_internalformat,
                                                  unsigned gl_format);
nxgl_sc_channel_kind nxgl_single_channel_classify_internal(
    unsigned gl_internalformat);

typedef enum nxgl_sc_transform {
  NXGL_SC_TRANSFORM_NONE = 0,
  NXGL_SC_TRANSFORM_ALPHA_TO_LA = 1,
  NXGL_SC_TRANSFORM_LUMINANCE_TO_LA = 2,
  NXGL_SC_TRANSFORM_MASK_TO_LA = 3,
  NXGL_SC_TRANSFORM_RED_TO_LA_DUP = 4
} nxgl_sc_transform;

/* Leading fields are retained from API 1. API 2 appends rejection, an exact
 * CPU transform, and the complete RGBA swizzle. GLenum values are carried as
 * unsigned integers so this module needs no GL headers. */
typedef struct nxgl_sc_upload_plan {
  int handled;
  unsigned internalformat;
  unsigned format;
  int duplicate_byte;
  int apply_swizzle_a_from_r;
  int reject;
  nxgl_sc_transform transform;
  int apply_swizzle;
  unsigned swizzle_r;
  unsigned swizzle_g;
  unsigned swizzle_b;
  unsigned swizzle_a;
} nxgl_sc_upload_plan;

/* API-1 entry point retained. It now means PRESERVE semantics and returns 0
 * for an unsafe/unrepresentable conversion. New integrations use _v2. */
int nxgl_single_channel_plan(nxgl_single_channel_route route,
                             nxgl_sc_channel_kind kind,
                             nxgl_sc_upload_plan *out);
int nxgl_single_channel_plan_v2(nxgl_single_channel_route route,
                                nxgl_sc_channel_kind kind,
                                nxgl_sc_semantic semantic,
                                nxgl_sc_upload_plan *out);

#define NXGL_SC_TRACKER_CAPACITY 512u

typedef struct nxgl_sc_texture_state {
  unsigned texture_id;
  int in_use;
  int storage_route;
  int upload_route;
  uintptr_t context_id;
  unsigned target;
  int channel_kind;
  int semantic;
} nxgl_sc_texture_state;

typedef struct nxgl_sc_tracker {
  nxgl_sc_texture_state slots[NXGL_SC_TRACKER_CAPACITY];
  unsigned used;
} nxgl_sc_tracker;

int nxgl_single_channel_tracker_reset(nxgl_sc_tracker *tracker);

typedef enum nxgl_sc_note_result {
  NXGL_SC_NOTE_COHERENT = 0,
  NXGL_SC_NOTE_MIXED,
  NXGL_SC_NOTE_OVERFLOW
} nxgl_sc_note_result;

/* Legacy id-only entry points are retained for API compatibility. Hardened
 * runtime code uses _v2, whose object key is context/share-group + target + id
 * and whose state also binds kind and semantic. */
nxgl_sc_note_result nxgl_single_channel_tracker_note(
    nxgl_sc_tracker *tracker, unsigned texture_id, int is_storage,
    nxgl_single_channel_route route);
int nxgl_single_channel_tracker_forget(nxgl_sc_tracker *tracker,
                                       unsigned texture_id);
nxgl_sc_note_result nxgl_single_channel_tracker_note_v2(
    nxgl_sc_tracker *tracker, uintptr_t context_id, unsigned target,
    unsigned texture_id, int is_storage, nxgl_single_channel_route route,
    nxgl_sc_channel_kind kind, nxgl_sc_semantic semantic);
int nxgl_single_channel_tracker_forget_v2(nxgl_sc_tracker *tracker,
                                          uintptr_t context_id,
                                          unsigned target,
                                          unsigned texture_id);
void nxgl_single_channel_tracker_forget_context(nxgl_sc_tracker *tracker,
                                                uintptr_t context_id);

#ifdef __cplusplus
}
#endif

#endif /* NXGL_SINGLE_CHANNEL_H */
