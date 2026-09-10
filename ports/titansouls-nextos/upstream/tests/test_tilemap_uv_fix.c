#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "../src/tilemap_uv_fix.h"

static float *quad_at(float *vbo, size_t index) {
  return vbo + index * TS_TILEMAP_QUAD_VERTICES * TS_TILEMAP_VERTEX_FLOATS;
}

static void make_quad(float *quad, float u0, float v0, int flip_u, int flip_v) {
  static const float xy[4][2] = {
      {0.0f, 0.0f}, {16.0f, 0.0f}, {0.0f, 16.0f}, {16.0f, 16.0f}};
  int i;

  for (i = 0; i < 4; i++) {
    float *v = quad + i * TS_TILEMAP_VERTEX_FLOATS;
    int high_u = ((i & 1) != 0) ^ flip_u;
    int high_v = ((i & 2) != 0) ^ flip_v;
    v[0] = xy[i][0];
    v[1] = xy[i][1];
    v[2] = 175.0f;
    v[3] = u0 + (high_u ? TS_TILEMAP_UV_SPAN : 0.0f);
    v[4] = v0 + (high_v ? TS_TILEMAP_UV_SPAN : 0.0f);
    v[12] = 360.0f;
    v[13] = 176.0f;
    v[14] = 0.0f;
    v[15] = 1.0f;
    v[16] = 1.0f;
    v[17] = 1.0f;
    v[18] = 1.0f;
  }
}

static void assert_inset_quad(const float *quad, float u0, float v0) {
  int i;
  for (i = 0; i < 4; i++) {
    const float u = quad[i * TS_TILEMAP_VERTEX_FLOATS + 3];
    const float v = quad[i * TS_TILEMAP_VERTEX_FLOATS + 4];
    assert(u == u0 + TS_TILEMAP_UV_INSET ||
           u == u0 + TS_TILEMAP_UV_SPAN - TS_TILEMAP_UV_INSET);
    assert(v == v0 + TS_TILEMAP_UV_INSET ||
           v == v0 + TS_TILEMAP_UV_SPAN - TS_TILEMAP_UV_INSET);
  }
}

/* GL_NEAREST usa floor(uv * tamanho). Com o quad 16 -> 32 pixels, o inset de
 * 0.25 texel deve manter rigorosamente duas amostras de cada texel fonte,
 * inclusive quando a orientacao do atlas esta invertida. Isso tambem prova
 * que nenhuma das 32 amostras escapa do intervalo do tile. */
static void assert_two_samples_per_texel(const float *quad, int flip_u,
                                         int flip_v, int source_u,
                                         int source_v) {
  const float u_left = quad[3];
  const float u_right = quad[TS_TILEMAP_VERTEX_FLOATS + 3];
  const float v_top = quad[4];
  const float v_bottom = quad[2 * TS_TILEMAP_VERTEX_FLOATS + 4];
  int pixel;

  for (pixel = 0; pixel < 32; pixel++) {
    const float t = ((float)pixel + 0.5f) / 32.0f;
    const int sampled_u =
        (int)((u_left + (u_right - u_left) * t) * TS_TILEMAP_ATLAS_SIZE);
    const int sampled_v =
        (int)((v_top + (v_bottom - v_top) * t) * TS_TILEMAP_ATLAS_SIZE);
    const int expected = flip_u ? 15 - pixel / 2 : pixel / 2;
    const int expected_v = flip_v ? 15 - pixel / 2 : pixel / 2;

    assert(sampled_u == source_u + expected);
    assert(sampled_v == source_v + expected_v);
    assert(sampled_u >= source_u && sampled_u < source_u + 16);
    assert(sampled_v >= source_v && sampled_v < source_v + 16);
  }
}

/* Acima de 0.5, binary16 tem passo 2^-11: exatamente 0.5 texel num atlas de
 * 1024. Este arredondamento nearest-even reproduz a faixa onde apareceram os
 * 291 escapes no VBO capturado e confirma que o ultimo sample nao vira o
 * primeiro texel do tile vizinho depois do inset. */
static int fp16_sample_above_half(float uv) {
  const float scaled = uv * 2048.0f;
  int rounded = (int)scaled;
  const float fraction = scaled - (float)rounded;

  assert(uv >= 0.5f && uv <= 1.0f);
  if (fraction > 0.5f || (fraction == 0.5f && (rounded & 1))) rounded++;
  return (int)((float)rounded * 0.5f);
}

static void assert_fp16_no_outside_u(const float *quad, int source_u) {
  const float u_left = quad[3];
  const float u_right = quad[TS_TILEMAP_VERTEX_FLOATS + 3];
  int pixel;

  for (pixel = 0; pixel < 32; pixel++) {
    const float t = ((float)pixel + 0.5f) / 32.0f;
    const float u = u_left + (u_right - u_left) * t;
    const int sampled = fp16_sample_above_half(u);
    assert(sampled >= source_u && sampled < source_u + 16);
  }
}

int main(void) {
  float *vbo = (float *)calloc(1, TS_TILEMAP_VBO_BYTES);
  unsigned char invalid_before[TS_TILEMAP_VERTEX_BYTES *
                               TS_TILEMAP_QUAD_VERTICES];
  float *normal;
  float *flipped;
  float *wrong_span;
  float *wrong_transform;
  size_t fixed;

  assert(vbo != NULL);
  normal = quad_at(vbo, 0);
  flipped = quad_at(vbo, 1);
  wrong_span = quad_at(vbo, 2);
  wrong_transform = quad_at(vbo, 3);

  make_quad(normal, 528.0f / 1024.0f, 32.0f / 1024.0f, 0, 0);
  make_quad(flipped, 608.0f / 1024.0f, 64.0f / 1024.0f, 1, 1);
  make_quad(wrong_span, 128.0f / 1024.0f, 96.0f / 1024.0f, 0, 0);
  wrong_span[TS_TILEMAP_VERTEX_FLOATS + 3] += TS_TILEMAP_UV_SPAN;
  wrong_span[3 * TS_TILEMAP_VERTEX_FLOATS + 3] += TS_TILEMAP_UV_SPAN;
  make_quad(wrong_transform, 256.0f / 1024.0f, 128.0f / 1024.0f, 0, 0);
  wrong_transform[TS_TILEMAP_VERTEX_FLOATS + 12] += 1.0f;
  memcpy(invalid_before, wrong_span, sizeof(invalid_before));

  assert(ts_tilemap_uv_fix_in_place(vbo, TS_TILEMAP_VBO_BYTES - 4u) == 0u);
  fixed = ts_tilemap_uv_fix_in_place(vbo, TS_TILEMAP_VBO_BYTES);
  assert(fixed == 2u);
  assert(!ts_tilemap_uv_is_canonical_batch(fixed));
  assert_inset_quad(normal, 528.0f / 1024.0f, 32.0f / 1024.0f);
  assert_inset_quad(flipped, 608.0f / 1024.0f, 64.0f / 1024.0f);
  assert_two_samples_per_texel(normal, 0, 0, 528, 32);
  assert_two_samples_per_texel(flipped, 1, 1, 608, 64);
  assert_fp16_no_outside_u(normal, 528);
  assert_fp16_no_outside_u(flipped, 608);
  assert(memcmp(invalid_before, wrong_span, sizeof(invalid_before)) == 0);
  assert(wrong_transform[TS_TILEMAP_VERTEX_FLOATS + 12] == 361.0f);

  /* O wrapper de GL so' aceita o fingerprint completo medido no VBO real:
   * 1954 quads normais dentro dos 1974 grupos. */
  memset(vbo, 0, TS_TILEMAP_VBO_BYTES);
  for (fixed = 0; fixed < TS_TILEMAP_EXPECTED_NORMAL_QUADS; fixed++) {
    const float u = (float)((fixed % 64u) * 16u) / 1024.0f;
    const float v = (float)(((fixed / 64u) % 64u) * 16u) / 1024.0f;
    make_quad(quad_at(vbo, fixed), u, v, (int)(fixed & 1u),
              (int)((fixed >> 1u) & 1u));
  }
  fixed = ts_tilemap_uv_fix_in_place(vbo, TS_TILEMAP_VBO_BYTES);
  assert(fixed == TS_TILEMAP_EXPECTED_NORMAL_QUADS);
  assert(ts_tilemap_uv_is_canonical_batch(fixed));

  free(vbo);
  return 0;
}
