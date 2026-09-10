/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test for the context/thread/unit/target-aware single-channel adapter. */
#include "nxgl_single_channel_adapter.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_TEXTURE_2D 0x0DE1u
#define GL_VERSION 0x1F02u
#define GL_EXTENSIONS 0x1F03u
#define GL_RED 0x1903u
#define GL_R8 0x8229u
#define GL_ALPHA 0x1906u
#define GL_ALPHA8 0x803Cu
#define GL_LUMINANCE 0x1909u
#define GL_LUMINANCE_ALPHA 0x190Au
#define GL_RGBA 0x1908u
#define GL_RGBA8 0x8058u
#define GL_UNSIGNED_BYTE 0x1401u
#define GL_UNSIGNED_SHORT 0x1403u
#define GL_UNPACK_ROW_LENGTH 0x0CF2u
#define GL_UNPACK_SKIP_ROWS 0x0CF3u
#define GL_UNPACK_SKIP_PIXELS 0x0CF4u
#define GL_UNPACK_ALIGNMENT 0x0CF5u

static const char *g_version;
static const char *g_extensions;
static int g_have_tex_storage;
static unsigned g_tex_storage_calls;

static void check(int condition, const char *message) {
  if (!condition) {
    (void)fprintf(stderr, "nxgl_single_channel_adapter: %s\n", message);
    exit(1);
  }
}

static const unsigned char *fake_gl_get_string(unsigned name) {
  if (name == GL_VERSION) {
    return (const unsigned char *)g_version;
  }
  if (name == GL_EXTENSIONS) {
    return (const unsigned char *)g_extensions;
  }
  return (const unsigned char *)"";
}

static void fake_gl_tex_storage_2d(unsigned target, int levels,
                                   unsigned internalformat, int width,
                                   int height) {
  (void)target;
  (void)levels;
  (void)internalformat;
  (void)width;
  (void)height;
  g_tex_storage_calls++;
}

static void *resolver(const char *name) {
  if (strcmp(name, "glGetString") == 0) {
    return (void *)fake_gl_get_string;
  }
  if (g_have_tex_storage && strcmp(name, "glTexStorage2D") == 0) {
    return (void *)fake_gl_tex_storage_2d;
  }
  return NULL;
}

static nxgl_sc_adapter_config config(nxgl_sc_semantic semantic,
                                     int allow_fallback) {
  nxgl_sc_adapter_config result;
  memset(&result, 0, sizeof(result));
  result.struct_size = sizeof(result);
  result.api_version = NXGL_SC_ADAPTER_CONFIG_API_VERSION;
  result.semantic = semantic;
  result.allow_legacy_fallback = allow_fallback;
  return result;
}

static nxgl_sc_image_desc image(unsigned internalformat, unsigned format,
                                unsigned type, int width, int height,
                                const void *pixels, size_t data_size) {
  nxgl_sc_image_desc result;
  memset(&result, 0, sizeof(result));
  result.internalformat = internalformat;
  result.format = format;
  result.type = type;
  result.width = width;
  result.height = height;
  result.pixels = pixels;
  result.data_size = data_size;
  return result;
}

static void bind_2d(nxgl_sc_adapter *adapter, uintptr_t context_id,
                    uintptr_t thread_id, unsigned unit, unsigned texture_id) {
  check(nxgl_single_channel_adapter_on_active_texture(
            adapter, context_id, thread_id, unit) == 1,
        "active texture tracking");
  check(nxgl_single_channel_adapter_on_bind_v2(
            adapter, context_id, thread_id, GL_TEXTURE_2D, texture_id) == 1,
        "2D binding tracking");
}

int main(void) {
  const uintptr_t context_a = (uintptr_t)0x101u;
  const uintptr_t context_b = (uintptr_t)0x202u;
  const uintptr_t thread_a = (uintptr_t)0x301u;
  nxgl_sc_adapter adapter;
  nxgl_sc_adapter_config cfg;
  nxgl_sc_image_desc desc;
  nxgl_sc_op op;

  nxgl_single_channel_adapter_set_resolver(resolver);

  /* The API-1 path lacks context/thread/unit/pixel-store/PBO identity and is
   * deliberately quarantined: enable alone cannot activate it. */
  check(nxgl_single_channel_adapter_init(&adapter) == 0, "init");
  nxgl_single_channel_adapter_enable(&adapter, 1);
  check(nxgl_single_channel_adapter_enabled(&adapter) == 0,
        "legacy enable without semantic configuration stays disabled");
  nxgl_single_channel_adapter_on_bind(&adapter, GL_TEXTURE_2D, 5u);
  check(nxgl_single_channel_adapter_plan_upload(
            &adapter, GL_TEXTURE_2D, GL_R8, GL_RED, &op) == 1 &&
            op.status == NXGL_SC_OP_PASS && op.handled == 0,
        "unscoped API-1 planner is pass-through");

  /* Native ES3 route: preserve RED's exact (r,0,0,1) sampling semantics. */
  cfg = config(NXGL_SC_SEMANTIC_PRESERVE, 0);
  check(nxgl_single_channel_adapter_configure(&adapter, &cfg) == 0,
        "configure preserve/native-only");
  nxgl_single_channel_adapter_enable(&adapter, 1);
  g_version = "OpenGL ES 3.2 fake";
  g_extensions = "";
  g_have_tex_storage = 1;
  g_tex_storage_calls = 0u;
  check(nxgl_single_channel_adapter_measure_context(&adapter, context_a) ==
            NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE,
        "context A measured native");
  check(g_tex_storage_calls == 0u,
        "capability measurement resolves but never calls TexStorage");
  bind_2d(&adapter, context_a, thread_a, 0u, 5u);
  check(nxgl_single_channel_adapter_plan_storage_v2(
            &adapter, context_a, thread_a, GL_TEXTURE_2D, GL_R8, &op) == 1 &&
            op.status == NXGL_SC_OP_REWRITE && op.internalformat == GL_R8 &&
            op.apply_swizzle == 1 && op.swizzle_r == GL_RED &&
            op.swizzle_g == 0u && op.swizzle_b == 0u && op.swizzle_a == 1u,
        "native RED storage has complete semantic swizzle");
  desc = image(GL_R8, GL_RED, GL_UNSIGNED_BYTE, 8, 8, NULL, 0u);
  check(nxgl_single_channel_adapter_plan_image_v2(
            &adapter, context_a, thread_a, GL_TEXTURE_2D, &desc, &op) == 1 &&
            op.status == NXGL_SC_OP_REWRITE &&
            op.transform == NXGL_SC_TRANSFORM_NONE,
        "native upload preserves one-byte layout");
  check(g_tex_storage_calls == 0u,
        "storage/image planning never calls or substitutes TexStorage");
  desc = image(GL_RGBA8, GL_RGBA, GL_UNSIGNED_SHORT, INT_MAX, INT_MAX,
               (const void *)(uintptr_t)1u, 0u);
  check(nxgl_single_channel_adapter_plan_image_v2(
            &adapter, context_a, thread_a, GL_TEXTURE_2D, &desc, &op) == 1 &&
            op.status == NXGL_SC_OP_PASS && op.handled == 0,
        "RGBA remains byte-for-byte untouched, regardless of type/size");

  /* GLES2 ALPHA fallback: respect alignment, row length and skips, and emit
   * (255,alpha), not the old (alpha,alpha) semantic corruption. */
  check(nxgl_single_channel_adapter_init(&adapter) == 0, "fallback init");
  cfg = config(NXGL_SC_SEMANTIC_PRESERVE, 1);
  check(nxgl_single_channel_adapter_configure(&adapter, &cfg) == 0,
        "fallback configure");
  nxgl_single_channel_adapter_enable(&adapter, 1);
  g_version = "OpenGL ES 2.0 fake";
  g_extensions = "";
  g_have_tex_storage = 0;
  check(nxgl_single_channel_adapter_measure_context(&adapter, context_a) ==
            NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP,
        "context A measured fallback");
  bind_2d(&adapter, context_a, thread_a, 0u, 9u);
  check(nxgl_single_channel_adapter_on_pixel_store(
            &adapter, context_a, thread_a, GL_UNPACK_ALIGNMENT, 4) == 1 &&
            nxgl_single_channel_adapter_on_pixel_store(
                &adapter, context_a, thread_a, GL_UNPACK_ROW_LENGTH, 5) == 1 &&
            nxgl_single_channel_adapter_on_pixel_store(
                &adapter, context_a, thread_a, GL_UNPACK_SKIP_ROWS, 1) == 1 &&
            nxgl_single_channel_adapter_on_pixel_store(
                &adapter, context_a, thread_a, GL_UNPACK_SKIP_PIXELS, 1) == 1,
        "tracked unpack state");
  {
    unsigned char source[20];
    unsigned char converted[12];
    size_t i;
    for (i = 0u; i < sizeof(source); i++) {
      source[i] = (unsigned char)i;
    }
    desc = image(GL_ALPHA8, GL_ALPHA, GL_UNSIGNED_BYTE, 3, 2,
                 source, sizeof(source));
    check(nxgl_single_channel_adapter_plan_image_v2(
              &adapter, context_a, thread_a, GL_TEXTURE_2D, &desc, &op) == 1 &&
              op.status == NXGL_SC_OP_REWRITE &&
              op.transform == NXGL_SC_TRANSFORM_ALPHA_TO_LA &&
              op.source_offset == 9u && op.source_stride == 8u &&
              op.source_required == 20u && op.converted_size == 12u &&
              op.reset_unpack_for_converted_data == 1,
          "ALPHA layout honors row-length/alignment/skips");
    check(nxgl_single_channel_adapter_convert(
              &op, &desc, converted, sizeof(converted)) == sizeof(converted) &&
              converted[0] == 0xffu && converted[1] == 9u &&
              converted[4] == 0xffu && converted[5] == 11u &&
              converted[6] == 0xffu && converted[7] == 17u &&
              converted[10] == 0xffu && converted[11] == 19u,
          "ALPHA conversion preserves legacy sampling semantics");
  }

  /* Context and texture-unit keys are independent, and context loss clears
   * only the selected context. */
  nxgl_single_channel_adapter_on_context_lost(&adapter, context_a);

  /* Rebuild a clean fallback state for LUMINANCE subrect conversion. */
  check(nxgl_single_channel_adapter_measure_context(&adapter, context_b) ==
            NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP,
        "context B measured fallback");
  bind_2d(&adapter, context_b, thread_a, 3u, 12u);
  check(nxgl_single_channel_adapter_on_pixel_store(
            &adapter, context_b, thread_a, GL_UNPACK_ALIGNMENT, 1) == 1,
        "context B alignment");
  desc = image(GL_LUMINANCE, GL_LUMINANCE, GL_UNSIGNED_BYTE,
               8, 8, NULL, 0u);
  check(nxgl_single_channel_adapter_plan_image_v2(
            &adapter, context_b, thread_a, GL_TEXTURE_2D, &desc, &op) == 1 &&
            op.status == NXGL_SC_OP_REWRITE,
        "LUMINANCE base image establishes object semantics");
  {
    unsigned char source[2] = {0x22u, 0x44u};
    unsigned char converted[4];
    desc = image(0u, GL_LUMINANCE, GL_UNSIGNED_BYTE, 2, 1,
                 source, sizeof(source));
    desc.xoffset = 3;
    desc.yoffset = 4;
    check(nxgl_single_channel_adapter_plan_subimage_v2(
              &adapter, context_b, thread_a, GL_TEXTURE_2D, &desc, &op) == 1 &&
              op.status == NXGL_SC_OP_REWRITE &&
              op.transform == NXGL_SC_TRANSFORM_LUMINANCE_TO_LA &&
              nxgl_single_channel_adapter_convert(
                  &op, &desc, converted, sizeof(converted)) == 4u &&
              converted[0] == 0x22u && converted[1] == 0xffu &&
              converted[2] == 0x44u && converted[3] == 0xffu,
          "LUMINANCE subrect preserves (l,l,l,1)");
  }

  /* A RED texture cannot be represented by LA under PRESERVE semantics. */
  check(nxgl_single_channel_adapter_init(&adapter) == 0, "red reject init");
  cfg = config(NXGL_SC_SEMANTIC_PRESERVE, 1);
  check(nxgl_single_channel_adapter_configure(&adapter, &cfg) == 0,
        "red reject configure");
  nxgl_single_channel_adapter_enable(&adapter, 1);
  check(nxgl_single_channel_adapter_measure_context(&adapter, context_a) ==
            NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP,
        "red reject fallback measured");
  bind_2d(&adapter, context_a, thread_a, 0u, 20u);
  {
    unsigned char source[1] = {0x77u};
    desc = image(GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, 1,
                 source, sizeof(source));
    check(nxgl_single_channel_adapter_plan_image_v2(
              &adapter, context_a, thread_a, GL_TEXTURE_2D, &desc, &op) == 1 &&
              op.status == NXGL_SC_OP_REJECT && op.coherent == 0,
          "RED preserve on LA fallback fails closed");
  }

  /* Explicit atlas semantics authorize RED -> (255,coverage), but a PBO or an
   * unsupported type still fails closed because CPU expansion is impossible. */
  check(nxgl_single_channel_adapter_init(&adapter) == 0, "mask init");
  cfg = config(NXGL_SC_SEMANTIC_ALPHA_MASK, 1);
  check(nxgl_single_channel_adapter_configure(&adapter, &cfg) == 0,
        "mask configure");
  nxgl_single_channel_adapter_enable(&adapter, 1);
  check(nxgl_single_channel_adapter_measure_context(&adapter, context_a) ==
            NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP,
        "mask fallback measured");
  bind_2d(&adapter, context_a, thread_a, 0u, 30u);
  check(nxgl_single_channel_adapter_on_pixel_store(
            &adapter, context_a, thread_a, GL_UNPACK_ALIGNMENT, 1) == 1,
        "mask alignment");
  {
    unsigned char source[2] = {0x10u, 0x80u};
    unsigned char converted[4];
    desc = image(GL_R8, GL_RED, GL_UNSIGNED_BYTE, 2, 1,
                 source, sizeof(source));
    check(nxgl_single_channel_adapter_plan_image_v2(
              &adapter, context_a, thread_a, GL_TEXTURE_2D, &desc, &op) == 1 &&
              op.transform == NXGL_SC_TRANSFORM_MASK_TO_LA &&
              nxgl_single_channel_adapter_convert(
                  &op, &desc, converted, sizeof(converted)) == 4u &&
              converted[0] == 0xffu && converted[1] == 0x10u &&
              converted[2] == 0xffu && converted[3] == 0x80u,
          "explicit RED alpha-mask conversion");
  }

  /* Explicit Unity/TextMeshPro coverage semantic: TexImage and TexSubImage
   * rewrite R8/RED to LA and duplicate the exact RED byte into both channels.
   * The generic contiguous helper is bounded; layout remains adapter-owned. */
  {
    static const unsigned char source[3] = {0x00u, 0x7fu, 0xffu};
    static const unsigned char expected[6] = {
        0x00u, 0x00u, 0x7fu, 0x7fu, 0xffu, 0xffu};
    unsigned char converted[6];
    unsigned char too_small[5] = {1u, 2u, 3u, 4u, 5u};

    check(nxgl_single_channel_adapter_expand_red_coverage_to_la_contiguous(
              source, 3u, converted, sizeof(converted)) == sizeof(converted) &&
              memcmp(converted, expected, sizeof(expected)) == 0,
          "bounded contiguous helper maps 00 7f ff to exact (R,R) bytes");
    check(nxgl_single_channel_adapter_expand_red_coverage_to_la_contiguous(
              source, 3u, too_small, sizeof(too_small)) == 0u &&
              too_small[0] == 1u && too_small[4] == 5u,
          "bounded contiguous helper rejects short destination before writing");
    check(nxgl_single_channel_adapter_expand_red_coverage_to_la_contiguous(
              NULL, 3u, converted, sizeof(converted)) == 0u &&
              nxgl_single_channel_adapter_expand_red_coverage_to_la_contiguous(
                  source, 3u, NULL, sizeof(converted)) == 0u &&
              nxgl_single_channel_adapter_expand_red_coverage_to_la_contiguous(
                  source, SIZE_MAX / 2u + 1u, converted,
                  sizeof(converted)) == 0u,
          "bounded contiguous helper rejects NULL and size overflow");

    check(nxgl_single_channel_adapter_init(&adapter) == 0,
          "RED coverage init");
    cfg = config(NXGL_SC_SEMANTIC_RED_COVERAGE_COMPAT, 1);
    check(nxgl_single_channel_adapter_configure(&adapter, &cfg) == 0,
          "RED coverage configure");
    nxgl_single_channel_adapter_enable(&adapter, 1);
    g_version = "OpenGL ES 2.0 fake";
    g_extensions = "";
    g_have_tex_storage = 0;
    check(nxgl_single_channel_adapter_measure_context(&adapter, context_a) ==
              NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP,
          "RED coverage fallback measured by capability");
    bind_2d(&adapter, context_a, thread_a, 0u, 40u);
    check(nxgl_single_channel_adapter_on_pixel_store(
              &adapter, context_a, thread_a, GL_UNPACK_ALIGNMENT, 1) == 1,
          "RED coverage TexImage alignment");
    desc = image(GL_R8, GL_RED, GL_UNSIGNED_BYTE, 3, 1,
                 source, sizeof(source));
    check(nxgl_single_channel_adapter_plan_image_v2(
              &adapter, context_a, thread_a, GL_TEXTURE_2D, &desc, &op) == 1 &&
              op.status == NXGL_SC_OP_REWRITE &&
              op.internalformat == GL_LUMINANCE_ALPHA &&
              op.format == GL_LUMINANCE_ALPHA &&
              op.transform == NXGL_SC_TRANSFORM_RED_TO_LA_DUP &&
              op.source_offset == 0u && op.source_stride == 3u &&
              op.source_required == 3u && op.converted_size == 6u &&
              nxgl_single_channel_adapter_convert(
                  &op, &desc, converted, sizeof(converted)) ==
                  sizeof(converted) &&
              memcmp(converted, expected, sizeof(expected)) == 0,
          "TexImage R8/RED fallback and contiguous layout produce exact (R,R)");

    check(nxgl_single_channel_adapter_on_pixel_store(
              &adapter, context_a, thread_a, GL_UNPACK_ALIGNMENT, 4) == 1 &&
              nxgl_single_channel_adapter_on_pixel_store(
                  &adapter, context_a, thread_a,
                  GL_UNPACK_ROW_LENGTH, 5) == 1 &&
              nxgl_single_channel_adapter_on_pixel_store(
                  &adapter, context_a, thread_a,
                  GL_UNPACK_SKIP_ROWS, 1) == 1 &&
              nxgl_single_channel_adapter_on_pixel_store(
                  &adapter, context_a, thread_a,
                  GL_UNPACK_SKIP_PIXELS, 1) == 1,
          "RED coverage TexSubImage unpack layout");
    {
      unsigned char layout_source[12] = {0u};
      layout_source[9] = 0x00u;
      layout_source[10] = 0x7fu;
      layout_source[11] = 0xffu;
      desc = image(0u, GL_RED, GL_UNSIGNED_BYTE, 3, 1,
                   layout_source, sizeof(layout_source));
      desc.xoffset = 2;
      desc.yoffset = 3;
      check(nxgl_single_channel_adapter_plan_subimage_v2(
                &adapter, context_a, thread_a, GL_TEXTURE_2D, &desc, &op) == 1 &&
                op.status == NXGL_SC_OP_REWRITE &&
                op.format == GL_LUMINANCE_ALPHA &&
                op.transform == NXGL_SC_TRANSFORM_RED_TO_LA_DUP &&
                op.source_offset == 9u && op.source_stride == 8u &&
                op.source_required == 12u && op.converted_size == 6u &&
                nxgl_single_channel_adapter_convert(
                    &op, &desc, converted, sizeof(converted)) ==
                    sizeof(converted) &&
                memcmp(converted, expected, sizeof(expected)) == 0,
            "TexSubImage honors row/skip/alignment and emits exact (R,R)");
    }
  }

  cfg = config(NXGL_SC_SEMANTIC_ALPHA_MASK, 1);
  check(nxgl_single_channel_adapter_init(&adapter) == 0, "PBO init");
  check(nxgl_single_channel_adapter_configure(&adapter, &cfg) == 0,
        "PBO configure");
  nxgl_single_channel_adapter_enable(&adapter, 1);
  (void)nxgl_single_channel_adapter_measure_context(&adapter, context_a);
  bind_2d(&adapter, context_a, thread_a, 0u, 31u);
  check(nxgl_single_channel_adapter_on_unpack_buffer(
            &adapter, context_a, thread_a, 77u) == 1,
        "PBO tracked");
  desc = image(GL_R8, GL_RED, GL_UNSIGNED_BYTE, 2, 1,
               (const void *)(uintptr_t)16u, 0u);
  check(nxgl_single_channel_adapter_plan_image_v2(
            &adapter, context_a, thread_a, GL_TEXTURE_2D, &desc, &op) == 1 &&
            op.status == NXGL_SC_OP_REJECT,
        "fallback PBO upload fails closed");

  check(nxgl_single_channel_adapter_init(&adapter) == 0, "type init");
  check(nxgl_single_channel_adapter_configure(&adapter, &cfg) == 0,
        "type configure");
  nxgl_single_channel_adapter_enable(&adapter, 1);
  (void)nxgl_single_channel_adapter_measure_context(&adapter, context_a);
  bind_2d(&adapter, context_a, thread_a, 0u, 32u);
  desc = image(GL_R8, GL_RED, GL_UNSIGNED_SHORT, 1, 1,
               (const void *)(uintptr_t)1u, 2u);
  check(nxgl_single_channel_adapter_plan_image_v2(
            &adapter, context_a, thread_a, GL_TEXTURE_2D, &desc, &op) == 1 &&
            op.status == NXGL_SC_OP_REJECT,
        "unsupported single-channel type fails closed");

  /* Table exhaustion poisons the opt-in path instead of silently losing state. */
  check(nxgl_single_channel_adapter_init(&adapter) == 0, "overflow init");
  check(nxgl_single_channel_adapter_configure(&adapter, &cfg) == 0,
        "overflow configure");
  {
    unsigned i;
    int last = 1;
    for (i = 1u; i <= NXGL_SC_ADAPTER_THREADS + 1u; i++) {
      last = nxgl_single_channel_adapter_on_active_texture(
          &adapter, context_a, (uintptr_t)i, 0u);
      if (!last) {
        break;
      }
    }
    check(last == 0 && adapter.poisoned == 1,
          "thread-state overflow is explicit and fail-closed");
  }

  check(nxgl_single_channel_adapter_duplicate_r_to_la(
            (const unsigned char *)"x", 1u, (unsigned char *)&op,
            sizeof(op)) == 0u,
        "unsafe API-1 duplicate helper is quarantined");

  (void)puts("nxgl_single_channel_adapter tests: ok "
             "(legacy-blocked/semantic-swizzle/context-thread-unit-target/"
             "pixel-store/subrect/PBO/type/overflow/context-loss/rgba-pass)");
  return 0;
}
