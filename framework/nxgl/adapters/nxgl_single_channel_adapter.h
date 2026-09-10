/* SPDX-License-Identifier: GPL-3.0-only */
/* Opt-in, fail-closed runtime planner for single-channel 2D textures. */
#ifndef NXGL_SINGLE_CHANNEL_ADAPTER_H
#define NXGL_SINGLE_CHANNEL_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#include "nxgl_single_channel.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NXGL_SC_ADAPTER_CONFIG_API_VERSION 1u
#define NXGL_SC_ADAPTER_TARGETS 8u       /* retained API-1 storage */
#define NXGL_SC_ADAPTER_CONTEXTS 16u
#define NXGL_SC_ADAPTER_THREADS 32u
#define NXGL_SC_ADAPTER_BINDINGS 256u

typedef struct nxgl_sc_target_bind {
  unsigned target;
  unsigned texture_id;
} nxgl_sc_target_bind;

typedef struct nxgl_sc_adapter_config {
  size_t struct_size;
  uint32_t api_version;
  nxgl_sc_semantic semantic;
  int allow_legacy_fallback;
} nxgl_sc_adapter_config;

typedef struct nxgl_sc_context_state {
  uintptr_t context_id; /* caller-provided share-group identity; 0 is invalid */
  int in_use;
  int measured;
  nxgl_single_channel_route route;
  nxgl_texture_capabilities caps;
} nxgl_sc_context_state;

typedef struct nxgl_sc_thread_state {
  uintptr_t context_id;
  uintptr_t thread_id;
  int in_use;
  unsigned active_unit; /* normalized 0..31 */
  int unpack_alignment;
  int unpack_row_length;
  int unpack_skip_rows;
  int unpack_skip_pixels;
  unsigned pixel_unpack_buffer;
} nxgl_sc_thread_state;

typedef struct nxgl_sc_binding_state {
  uintptr_t context_id;
  uintptr_t thread_id;
  unsigned unit;
  unsigned target;
  unsigned texture_id;
  int in_use;
} nxgl_sc_binding_state;

typedef struct nxgl_sc_adapter {
  /* API-1 leading fields retained. Hardened integrations do not use targets. */
  int enabled;
  int measured;
  nxgl_single_channel_route route;
  nxgl_texture_capabilities caps;
  nxgl_sc_tracker tracker;
  nxgl_sc_target_bind targets[NXGL_SC_ADAPTER_TARGETS];

  int configured;
  int poisoned;
  nxgl_sc_adapter_config config;
  nxgl_sc_context_state contexts[NXGL_SC_ADAPTER_CONTEXTS];
  nxgl_sc_thread_state threads[NXGL_SC_ADAPTER_THREADS];
  nxgl_sc_binding_state bindings[NXGL_SC_ADAPTER_BINDINGS];
} nxgl_sc_adapter;

typedef enum nxgl_sc_op_status {
  NXGL_SC_OP_PASS = 0,
  NXGL_SC_OP_REWRITE = 1,
  NXGL_SC_OP_REJECT = 2
} nxgl_sc_op_status;

/* Leading fields retained. API 2 appends exact semantic/layout information. */
typedef struct nxgl_sc_op {
  int handled;
  unsigned internalformat;
  unsigned format;
  int duplicate_byte;
  int apply_swizzle_a_from_r;
  int coherent;
  nxgl_sc_op_status status;
  nxgl_sc_channel_kind source_kind;
  nxgl_sc_transform transform;
  int apply_swizzle;
  unsigned swizzle_r;
  unsigned swizzle_g;
  unsigned swizzle_b;
  unsigned swizzle_a;
  size_t source_offset;
  size_t source_stride;
  size_t source_row_bytes;
  size_t source_required;
  size_t converted_size;
  int reset_unpack_for_converted_data;
} nxgl_sc_op;

typedef struct nxgl_sc_image_desc {
  int width;
  int height;
  int xoffset;
  int yoffset;
  unsigned internalformat; /* ignored for subimage */
  unsigned format;
  unsigned type;
  const void *pixels;
  size_t data_size; /* bytes available at pixels; required for CPU conversion */
} nxgl_sc_image_desc;

void nxgl_single_channel_adapter_set_resolver(void *(*resolver)(const char *));
int nxgl_single_channel_adapter_init(nxgl_sc_adapter *adapter);
int nxgl_single_channel_adapter_configure(
    nxgl_sc_adapter *adapter, const nxgl_sc_adapter_config *config);
void nxgl_single_channel_adapter_enable(nxgl_sc_adapter *adapter, int on);
int nxgl_single_channel_adapter_enabled(const nxgl_sc_adapter *adapter);

/* Legacy measurement is retained for ABI compatibility but cannot enable the
 * hardened path because it has no context identity. */
nxgl_single_channel_route nxgl_single_channel_adapter_measure(
    nxgl_sc_adapter *adapter);
nxgl_single_channel_route nxgl_single_channel_adapter_measure_context(
    nxgl_sc_adapter *adapter, uintptr_t context_id);

/* Measurement only resolves TexStorage to observe availability. Measurement
 * and every plan_* function are side-effect-free with respect to texture
 * storage: they never invoke, wrap or replace the caller-owned GL operation. */

/* The caller supplies an opaque nonzero current-context/share-group token and
 * a nonzero thread token. Binding lookup is keyed by context/thread/unit/target;
 * texture coherence is correctly keyed by share-group/target/object. */
int nxgl_single_channel_adapter_on_active_texture(nxgl_sc_adapter *adapter,
                                                  uintptr_t context_id,
                                                  uintptr_t thread_id,
                                                  unsigned unit);
int nxgl_single_channel_adapter_on_bind_v2(nxgl_sc_adapter *adapter,
                                           uintptr_t context_id,
                                           uintptr_t thread_id,
                                           unsigned target,
                                           unsigned texture_id);
int nxgl_single_channel_adapter_on_pixel_store(nxgl_sc_adapter *adapter,
                                               uintptr_t context_id,
                                               uintptr_t thread_id,
                                               unsigned pname, int value);
int nxgl_single_channel_adapter_on_unpack_buffer(nxgl_sc_adapter *adapter,
                                                 uintptr_t context_id,
                                                 uintptr_t thread_id,
                                                 unsigned buffer_id);
void nxgl_single_channel_adapter_on_delete_v2(nxgl_sc_adapter *adapter,
                                              uintptr_t context_id,
                                              int count,
                                              const unsigned *ids);
void nxgl_single_channel_adapter_on_context_lost(nxgl_sc_adapter *adapter,
                                                 uintptr_t context_id);

int nxgl_single_channel_adapter_plan_storage_v2(
    nxgl_sc_adapter *adapter, uintptr_t context_id, uintptr_t thread_id,
    unsigned target, unsigned internalformat, nxgl_sc_op *out);
int nxgl_single_channel_adapter_plan_image_v2(
    nxgl_sc_adapter *adapter, uintptr_t context_id, uintptr_t thread_id,
    unsigned target, const nxgl_sc_image_desc *image, nxgl_sc_op *out);
int nxgl_single_channel_adapter_plan_subimage_v2(
    nxgl_sc_adapter *adapter, uintptr_t context_id, uintptr_t thread_id,
    unsigned target, const nxgl_sc_image_desc *image, nxgl_sc_op *out);

/* Convert exactly the selected subrectangle into tightly packed LA bytes.
 * The caller temporarily uses UNPACK_ALIGNMENT=1, ROW_LENGTH=0 and zero skips
 * for this buffer, then restores the tracked GL state. */
size_t nxgl_single_channel_adapter_convert(
    const nxgl_sc_op *op, const nxgl_sc_image_desc *image,
    unsigned char *dst, size_t dst_cap);

/* Explicit bounded helper for an already-contiguous CPU RED coverage buffer.
 * It writes each byte as LUMINANCE_ALPHA (R,R). Source and destination must not
 * overlap. This helper does not interpret pixel-store state or PBO offsets and
 * must not bypass plan_image_v2/plan_subimage_v2 for a GL upload. Returns the
 * bytes written, or zero for invalid input, overflow or insufficient capacity. */
size_t nxgl_single_channel_adapter_expand_red_coverage_to_la_contiguous(
    const unsigned char *src, size_t count, unsigned char *dst,
    size_t dst_cap);

/* API-1 functions remain linkable but never activate an unscoped rewrite. */
void nxgl_single_channel_adapter_on_bind(nxgl_sc_adapter *adapter,
                                         unsigned target,
                                         unsigned texture_id);
void nxgl_single_channel_adapter_on_delete(nxgl_sc_adapter *adapter, int count,
                                           const unsigned *ids);
int nxgl_single_channel_adapter_plan_storage(nxgl_sc_adapter *adapter,
                                             unsigned target,
                                             unsigned internalformat,
                                             unsigned format,
                                             nxgl_sc_op *out);
int nxgl_single_channel_adapter_plan_upload(nxgl_sc_adapter *adapter,
                                            unsigned target,
                                            unsigned internalformat,
                                            unsigned format,
                                            nxgl_sc_op *out);
size_t nxgl_single_channel_adapter_duplicate_r_to_la(
    const unsigned char *src, size_t count, unsigned char *dst,
    size_t dst_cap);

#ifdef __cplusplus
}
#endif

#endif /* NXGL_SINGLE_CHANNEL_ADAPTER_H */
