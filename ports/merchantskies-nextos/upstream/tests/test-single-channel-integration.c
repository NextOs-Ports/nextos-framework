/* SPDX-License-Identifier: GPL-3.0-only */
/* Merchant-specific regression contract for the vendored nxgl adapter. */
#include "nxgl_single_channel_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_TEXTURE_2D 0x0DE1u
#define GL_VERSION 0x1F02u
#define GL_EXTENSIONS 0x1F03u
#define GL_RED 0x1903u
#define GL_RGBA 0x1908u
#define GL_LUMINANCE_ALPHA 0x190Au
#define GL_UNSIGNED_BYTE 0x1401u
#define GL_R8 0x8229u
#define GL_RGBA8 0x8058u
#define GL_UNPACK_ROW_LENGTH 0x0CF2u
#define GL_UNPACK_SKIP_ROWS 0x0CF3u
#define GL_UNPACK_SKIP_PIXELS 0x0CF4u
#define GL_UNPACK_ALIGNMENT 0x0CF5u

static const char *fake_version;
static const char *fake_extensions;
static int fake_has_tex_storage;
static unsigned fake_tex_storage_resolves;
static unsigned fake_tex_storage_calls;

static void require(int condition, const char *message) {
  if (!condition) {
    (void)fprintf(stderr, "merchant_single_channel=FAIL %s\n", message);
    exit(1);
  }
}

static const unsigned char *fake_gl_get_string(unsigned name) {
  if (name == GL_VERSION) {
    return (const unsigned char *)fake_version;
  }
  if (name == GL_EXTENSIONS) {
    return (const unsigned char *)fake_extensions;
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
  fake_tex_storage_calls++;
}

static void *fake_resolver(const char *name) {
  if (strcmp(name, "glGetString") == 0) {
    return (void *)fake_gl_get_string;
  }
  if (strcmp(name, "glTexStorage2D") == 0) {
    fake_tex_storage_resolves++;
    if (fake_has_tex_storage) {
      return (void *)fake_gl_tex_storage_2d;
    }
  }
  return NULL;
}

static nxgl_sc_adapter_config make_config(nxgl_sc_semantic semantic) {
  nxgl_sc_adapter_config config;
  memset(&config, 0, sizeof(config));
  config.struct_size = sizeof(config);
  config.api_version = NXGL_SC_ADAPTER_CONFIG_API_VERSION;
  config.semantic = semantic;
  config.allow_legacy_fallback = 1;
  return config;
}

static nxgl_sc_image_desc make_image(unsigned internalformat,
                                     unsigned format, int width, int height,
                                     const void *pixels, size_t data_size) {
  nxgl_sc_image_desc image;
  memset(&image, 0, sizeof(image));
  image.width = width;
  image.height = height;
  image.internalformat = internalformat;
  image.format = format;
  image.type = GL_UNSIGNED_BYTE;
  image.pixels = pixels;
  image.data_size = data_size;
  return image;
}

static void bind_texture(nxgl_sc_adapter *adapter, uintptr_t context_id,
                         uintptr_t thread_id, unsigned texture_id) {
  require(nxgl_single_channel_adapter_on_active_texture(
              adapter, context_id, thread_id, 0u) == 1,
          "active texture tracking");
  require(nxgl_single_channel_adapter_on_bind_v2(
              adapter, context_id, thread_id, GL_TEXTURE_2D, texture_id) == 1,
          "texture binding tracking");
}

static void configure_adapter(nxgl_sc_adapter *adapter,
                              nxgl_sc_semantic semantic) {
  nxgl_sc_adapter_config config = make_config(semantic);
  require(nxgl_single_channel_adapter_init(adapter) == 0, "adapter init");
  require(nxgl_single_channel_adapter_configure(adapter, &config) == 0,
          "adapter semantic configure");
  nxgl_single_channel_adapter_enable(adapter, 1);
  require(nxgl_single_channel_adapter_enabled(adapter) == 1,
          "configured adapter enabled");
}

static void test_es2_red_coverage(void) {
  static const uintptr_t context_id = (uintptr_t)0x450u;
  static const uintptr_t thread_id = (uintptr_t)0x451u;
  static const unsigned char contiguous[3] = {0x00u, 0x7fu, 0xffu};
  static const unsigned char expected[6] = {
      0x00u, 0x00u, 0x7fu, 0x7fu, 0xffu, 0xffu};
  nxgl_sc_adapter adapter;
  nxgl_sc_image_desc image;
  nxgl_sc_op op;
  unsigned char converted[6];

  configure_adapter(&adapter, NXGL_SC_SEMANTIC_RED_COVERAGE_COMPAT);
  fake_version = "OpenGL ES 2.0 Merchant fake";
  fake_extensions = "";
  fake_has_tex_storage = 0;
  require(nxgl_single_channel_adapter_measure_context(&adapter, context_id) ==
              NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP,
          "ES2 must select LUMINANCE_ALPHA fallback");
  bind_texture(&adapter, context_id, thread_id, 40u);

  require(nxgl_single_channel_adapter_plan_storage_v2(
              &adapter, context_id, thread_id, GL_TEXTURE_2D, GL_R8, &op) ==
              1 &&
              op.status == NXGL_SC_OP_REWRITE &&
              op.internalformat == GL_LUMINANCE_ALPHA &&
              op.format == GL_LUMINANCE_ALPHA &&
              op.transform == NXGL_SC_TRANSFORM_RED_TO_LA_DUP,
          "ES2 storage plan must remain R8 -> LUMINANCE_ALPHA (R,R)");
  require(nxgl_single_channel_adapter_on_pixel_store(
              &adapter, context_id, thread_id, GL_UNPACK_ALIGNMENT, 1) == 1,
          "ES2 contiguous unpack alignment");
  image = make_image(GL_R8, GL_RED, 3, 1, contiguous, sizeof(contiguous));
  require(nxgl_single_channel_adapter_plan_image_v2(
              &adapter, context_id, thread_id, GL_TEXTURE_2D, &image, &op) ==
              1 &&
              op.status == NXGL_SC_OP_REWRITE &&
              op.internalformat == GL_LUMINANCE_ALPHA &&
              op.format == GL_LUMINANCE_ALPHA &&
              op.transform == NXGL_SC_TRANSFORM_RED_TO_LA_DUP &&
              op.source_offset == 0u && op.source_stride == 3u &&
              op.source_required == 3u && op.converted_size == 6u,
          "ES2 TexImage plan and contiguous layout");
  require(nxgl_single_channel_adapter_convert(
              &op, &image, converted, sizeof(converted)) ==
              sizeof(converted) &&
              memcmp(converted, expected, sizeof(expected)) == 0,
          "ES2 TexImage bytes must be 00 00 7f 7f ff ff");

  require(nxgl_single_channel_adapter_on_pixel_store(
              &adapter, context_id, thread_id, GL_UNPACK_ALIGNMENT, 4) == 1 &&
              nxgl_single_channel_adapter_on_pixel_store(
                  &adapter, context_id, thread_id, GL_UNPACK_ROW_LENGTH, 5) ==
                  1 &&
              nxgl_single_channel_adapter_on_pixel_store(
                  &adapter, context_id, thread_id, GL_UNPACK_SKIP_ROWS, 1) ==
                  1 &&
              nxgl_single_channel_adapter_on_pixel_store(
                  &adapter, context_id, thread_id, GL_UNPACK_SKIP_PIXELS, 1) ==
                  1,
          "ES2 TexSubImage row/skip/alignment tracking");
  {
    unsigned char layout_source[12] = {0u};
    layout_source[9] = 0x00u;
    layout_source[10] = 0x7fu;
    layout_source[11] = 0xffu;
    image = make_image(0u, GL_RED, 3, 1, layout_source,
                       sizeof(layout_source));
    image.xoffset = 2;
    image.yoffset = 3;
    require(nxgl_single_channel_adapter_plan_subimage_v2(
                &adapter, context_id, thread_id, GL_TEXTURE_2D, &image, &op) ==
                1 &&
                op.status == NXGL_SC_OP_REWRITE &&
                op.format == GL_LUMINANCE_ALPHA &&
                op.transform == NXGL_SC_TRANSFORM_RED_TO_LA_DUP &&
                op.source_offset == 9u && op.source_stride == 8u &&
                op.source_required == 12u && op.converted_size == 6u,
            "ES2 TexSubImage must honor row/skip/alignment layout");
    require(nxgl_single_channel_adapter_convert(
                &op, &image, converted, sizeof(converted)) ==
                sizeof(converted) &&
                memcmp(converted, expected, sizeof(expected)) == 0,
            "ES2 TexSubImage bytes must be 00 00 7f 7f ff ff");
  }

  memset(converted, 0xa5, sizeof(converted));
  require(nxgl_single_channel_adapter_expand_red_coverage_to_la_contiguous(
              contiguous, 3u, converted, sizeof(converted)) ==
              sizeof(converted) &&
              memcmp(converted, expected, sizeof(expected)) == 0,
          "bounded contiguous helper must duplicate RED as (R,R)");
}

static void test_es3_native_route_and_texstorage_ownership(void) {
  static const uintptr_t context_id = (uintptr_t)0x31u;
  static const uintptr_t thread_id = (uintptr_t)0x32u;
  static const unsigned char source[3] = {0x00u, 0x7fu, 0xffu};
  nxgl_sc_adapter adapter;
  nxgl_sc_image_desc image;
  nxgl_sc_op op;

  configure_adapter(&adapter, NXGL_SC_SEMANTIC_RED_COVERAGE_COMPAT);
  fake_version = "OpenGL ES 3.2 Merchant fake";
  fake_extensions = "";
  fake_has_tex_storage = 1;
  fake_tex_storage_resolves = 0u;
  fake_tex_storage_calls = 0u;
  require(nxgl_single_channel_adapter_measure_context(&adapter, context_id) ==
              NXGL_SC_ROUTE_NATIVE_R8_SWIZZLE,
          "ES3 must select native R8/RED route");
  require(fake_tex_storage_resolves > 0u && fake_tex_storage_calls == 0u,
          "measurement may resolve but must never call TexStorage");
  bind_texture(&adapter, context_id, thread_id, 31u);

  require(nxgl_single_channel_adapter_plan_storage_v2(
              &adapter, context_id, thread_id, GL_TEXTURE_2D, GL_R8, &op) ==
              1 &&
              op.status == NXGL_SC_OP_REWRITE && op.internalformat == GL_R8 &&
              op.format == GL_RED &&
              op.transform == NXGL_SC_TRANSFORM_NONE &&
              op.apply_swizzle == 1 && op.swizzle_r == 1u &&
              op.swizzle_g == 1u && op.swizzle_b == 1u &&
              op.swizzle_a == GL_RED,
          "ES3 storage must stay native R8/RED with Unity coverage swizzle");
  image = make_image(GL_R8, GL_RED, 3, 1, source, sizeof(source));
  require(nxgl_single_channel_adapter_plan_image_v2(
              &adapter, context_id, thread_id, GL_TEXTURE_2D, &image, &op) ==
              1 &&
              op.status == NXGL_SC_OP_REWRITE && op.internalformat == GL_R8 &&
              op.format == GL_RED &&
              op.transform == NXGL_SC_TRANSFORM_NONE &&
              op.converted_size == 0u && op.apply_swizzle == 1 &&
              op.swizzle_r == 1u && op.swizzle_g == 1u &&
              op.swizzle_b == 1u && op.swizzle_a == GL_RED,
          "ES3 TexImage must stay native without CPU conversion");
  image = make_image(0u, GL_RED, 3, 1, source, sizeof(source));
  image.xoffset = 1;
  image.yoffset = 1;
  require(nxgl_single_channel_adapter_plan_subimage_v2(
              &adapter, context_id, thread_id, GL_TEXTURE_2D, &image, &op) ==
              1 &&
              op.status == NXGL_SC_OP_REWRITE && op.format == GL_RED &&
              op.transform == NXGL_SC_TRANSFORM_NONE &&
              op.converted_size == 0u,
          "ES3 TexSubImage must stay native without CPU conversion");
  require(fake_tex_storage_calls == 0u,
          "adapter/planners must not call, wrap or replace TexStorage");
}

static void test_legacy_alpha_mask_and_rgba_passthrough(void) {
  static const uintptr_t context_id = (uintptr_t)0xa1u;
  static const uintptr_t thread_id = (uintptr_t)0xa2u;
  static const unsigned char source[3] = {0x00u, 0x7fu, 0xffu};
  static const unsigned char expected_mask[6] = {
      0xffu, 0x00u, 0xffu, 0x7fu, 0xffu, 0xffu};
  nxgl_sc_adapter adapter;
  nxgl_sc_image_desc image;
  nxgl_sc_op op;
  unsigned char converted[6];
  unsigned char rgba[4] = {0x12u, 0x34u, 0x56u, 0x78u};

  configure_adapter(&adapter, NXGL_SC_SEMANTIC_ALPHA_MASK);
  fake_version = "OpenGL ES 2.0 Merchant fake";
  fake_extensions = "";
  fake_has_tex_storage = 0;
  require(nxgl_single_channel_adapter_measure_context(&adapter, context_id) ==
              NXGL_SC_ROUTE_LUMINANCE_ALPHA_DUP,
          "legacy alpha-mask fallback measurement");
  bind_texture(&adapter, context_id, thread_id, 20u);
  require(nxgl_single_channel_adapter_on_pixel_store(
              &adapter, context_id, thread_id, GL_UNPACK_ALIGNMENT, 1) == 1,
          "legacy alpha-mask unpack alignment");
  image = make_image(GL_R8, GL_RED, 3, 1, source, sizeof(source));
  require(nxgl_single_channel_adapter_plan_image_v2(
              &adapter, context_id, thread_id, GL_TEXTURE_2D, &image, &op) ==
              1 &&
              op.status == NXGL_SC_OP_REWRITE &&
              op.transform == NXGL_SC_TRANSFORM_MASK_TO_LA &&
              nxgl_single_channel_adapter_convert(
                  &op, &image, converted, sizeof(converted)) ==
                  sizeof(converted) &&
              memcmp(converted, expected_mask, sizeof(expected_mask)) == 0,
          "legacy ALPHA_MASK must remain exactly (255,R)");

  image = make_image(GL_RGBA8, GL_RGBA, 1, 1, rgba, sizeof(rgba));
  require(nxgl_single_channel_adapter_plan_image_v2(
              &adapter, context_id, thread_id, GL_TEXTURE_2D, &image, &op) ==
              1 &&
              op.status == NXGL_SC_OP_PASS && op.handled == 0 &&
              op.internalformat == GL_RGBA8 && op.format == GL_RGBA &&
              op.transform == NXGL_SC_TRANSFORM_NONE &&
              rgba[0] == 0x12u && rgba[1] == 0x34u && rgba[2] == 0x56u &&
              rgba[3] == 0x78u,
          "RGBA must remain exact pass-through");
}

int main(void) {
  nxgl_single_channel_adapter_set_resolver(fake_resolver);
  test_es2_red_coverage();
  test_es3_native_route_and_texstorage_ownership();
  test_legacy_alpha_mask_and_rgba_passthrough();
  require(fake_tex_storage_calls == 0u,
          "TexStorage ownership belongs exclusively to the proven backend");
  (void)puts("merchant_single_channel=PASS "
             "es2_la_rr=PASS es3_r8_red_swizzle=PASS "
             "legacy_255r=PASS rgba_passthrough=PASS texstorage_owner=backend");
  return 0;
}
