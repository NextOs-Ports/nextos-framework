#ifndef TITANSOULS_TILEMAP_UV_FIX_H
#define TITANSOULS_TILEMAP_UV_FIX_H

#include <stddef.h>

/* Layout exato do lote de HueTilemap observado na libgame.so 1.0.3.
 * Manter estes numeros aqui faz a correcao falhar fechada: nenhum outro VBO
 * do jogo (e nenhum outro port) recebe ajuste de UV por engano. */
enum {
  TS_TILEMAP_VBO_BYTES = 600096,
  TS_TILEMAP_VERTEX_FLOATS = 19,
  TS_TILEMAP_VERTEX_BYTES = 76,
  TS_TILEMAP_QUAD_VERTICES = 4,
  TS_TILEMAP_EXPECTED_NORMAL_QUADS = 1954
};

#define TS_TILEMAP_ATLAS_SIZE 1024.0f
#define TS_TILEMAP_UV_SPAN (16.0f / TS_TILEMAP_ATLAS_SIZE)
#define TS_TILEMAP_UV_INSET (0.25f / TS_TILEMAP_ATLAS_SIZE)

static inline int ts_tilemap_uv_is_texel_boundary(float value) {
  float texel;
  int whole;

  if (!(value >= 0.0f && value <= 1.0f)) return 0;
  texel = value * TS_TILEMAP_ATLAS_SIZE;
  whole = (int)texel;
  return texel == (float)whole;
}

/* Confirma um quad local 16x16 completo, UV 16x16 completo e o mesmo
 * offset/scale nos quatro vertices. Os masks rejeitam triangulos degenerados
 * ou quatro vertices que por acaso tenham apenas o mesmo min/max. */
static inline int ts_tilemap_uv_quad_is_normal(const float *quad) {
  float u_min = quad[3], u_max = quad[3];
  float v_min = quad[4], v_max = quad[4];
  unsigned position_corners = 0;
  unsigned uv_corners = 0;
  int vertex;

  for (vertex = 0; vertex < TS_TILEMAP_QUAD_VERTICES; vertex++) {
    const float *v = quad + vertex * TS_TILEMAP_VERTEX_FLOATS;
    unsigned corner;

    if (!((v[0] == 0.0f || v[0] == 16.0f) &&
          (v[1] == 0.0f || v[1] == 16.0f)))
      return 0;
    corner = (v[0] == 16.0f ? 1u : 0u) |
             (v[1] == 16.0f ? 2u : 0u);
    if (position_corners & (1u << corner)) return 0;
    position_corners |= 1u << corner;

    if (v[3] < u_min) u_min = v[3];
    if (v[3] > u_max) u_max = v[3];
    if (v[4] < v_min) v_min = v[4];
    if (v[4] > v_max) v_max = v[4];

    if (vertex > 0) {
      int field;
      for (field = 12; field <= 18; field++)
        if (v[field] != quad[field]) return 0;
    }
  }
  if (position_corners != 0x0fu) return 0;
  if (u_max - u_min != TS_TILEMAP_UV_SPAN ||
      v_max - v_min != TS_TILEMAP_UV_SPAN)
    return 0;
  if (!ts_tilemap_uv_is_texel_boundary(u_min) ||
      !ts_tilemap_uv_is_texel_boundary(u_max) ||
      !ts_tilemap_uv_is_texel_boundary(v_min) ||
      !ts_tilemap_uv_is_texel_boundary(v_max))
    return 0;

  for (vertex = 0; vertex < TS_TILEMAP_QUAD_VERTICES; vertex++) {
    const float *v = quad + vertex * TS_TILEMAP_VERTEX_FLOATS;
    unsigned corner;

    if (!((v[3] == u_min || v[3] == u_max) &&
          (v[4] == v_min || v[4] == v_max)))
      return 0;
    corner = (v[3] == u_max ? 1u : 0u) |
             (v[4] == v_max ? 2u : 0u);
    if (uv_corners & (1u << corner)) return 0;
    uv_corners |= 1u << corner;
  }
  return uv_corners == 0x0fu;
}

/* Ajusta somente os quads validados. A comparacao com min/max, em vez de
 * assumir uma ordem dos vertices, preserva tiles espelhados nos dois eixos. */
static inline size_t ts_tilemap_uv_fix_in_place(void *data, size_t bytes) {
  float *vertices = (float *)data;
  const size_t floats_per_quad =
      TS_TILEMAP_VERTEX_FLOATS * TS_TILEMAP_QUAD_VERTICES;
  const size_t quad_count =
      TS_TILEMAP_VBO_BYTES /
      (TS_TILEMAP_VERTEX_BYTES * TS_TILEMAP_QUAD_VERTICES);
  size_t fixed = 0;
  size_t quad_index;

  if (!data || bytes != TS_TILEMAP_VBO_BYTES || sizeof(float) != 4u)
    return 0;

  for (quad_index = 0; quad_index < quad_count; quad_index++) {
    float *quad = vertices + quad_index * floats_per_quad;
    float u_min, u_max, v_min, v_max;
    int vertex;

    if (!ts_tilemap_uv_quad_is_normal(quad)) continue;

    u_min = u_max = quad[3];
    v_min = v_max = quad[4];
    for (vertex = 1; vertex < TS_TILEMAP_QUAD_VERTICES; vertex++) {
      const float *v = quad + vertex * TS_TILEMAP_VERTEX_FLOATS;
      if (v[3] < u_min) u_min = v[3];
      if (v[3] > u_max) u_max = v[3];
      if (v[4] < v_min) v_min = v[4];
      if (v[4] > v_max) v_max = v[4];
    }

    for (vertex = 0; vertex < TS_TILEMAP_QUAD_VERTICES; vertex++) {
      float *v = quad + vertex * TS_TILEMAP_VERTEX_FLOATS;
      v[3] = v[3] == u_min ? u_min + TS_TILEMAP_UV_INSET
                           : u_max - TS_TILEMAP_UV_INSET;
      v[4] = v[4] == v_min ? v_min + TS_TILEMAP_UV_INSET
                           : v_max - TS_TILEMAP_UV_INSET;
    }
    fixed++;
  }
  return fixed;
}

static inline int ts_tilemap_uv_is_canonical_batch(size_t fixed_quads) {
  return fixed_quads == TS_TILEMAP_EXPECTED_NORMAL_QUADS;
}

#endif
