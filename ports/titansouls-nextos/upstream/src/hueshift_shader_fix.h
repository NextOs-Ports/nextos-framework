#ifndef TITANSOULS_HUESHIFT_SHADER_FIX_H
#define TITANSOULS_HUESHIFT_SHADER_FIX_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum ts_hueshift_shader_stage {
  TS_HUESHIFT_SHADER_VERTEX = 1,
  TS_HUESHIFT_SHADER_FRAGMENT = 2
};

/* Fontes canonicas do OBB Titan Souls Android 1.0.3:
 *   hueshift_android.vsh SHA-256
 *     35c2a0385068d333f448306bcca23d4d71a5499e68fa5945b398bed2464e332b
 *   hueshift_android.fsh SHA-256
 *     a94a6128887b304aa2f28c394550b03665bdf76818e25f070a0fc4d2f6dccbe3
 *
 * O runtime pode expandir includes e entregar o shader em varios strings,
 * portanto o gate usa um conjunto de assinaturas independentes e fortes em
 * vez de depender dos bytes/CRLF do arquivo no disco. */
static inline int ts_hueshift_shader_matches(
    const char *source, enum ts_hueshift_shader_stage stage) {
  static const char *const vertex_signatures[] = {
      "attribute vec3 _offset", "uniform lowp vec3 HSV",
      "varying lowp mat3 vHSV", "vHSV = createHSVMatrix",
      "v_texCoord = tcoord", "floor(pos.x * scale)"};
  static const char *const fragment_signatures[] = {
      "uniform sampler2D InputTexture", "uniform lowp vec3 HSV",
      "uniform lowp float huePercent", "varying lowp mat3 vHSV",
      "rgb2hsv", "hsv2rgb", "texture2D( InputTexture, v_texCoord )"};
  const char *const *signatures;
  size_t count;
  size_t i;

  if (!source) return 0;
  if (stage == TS_HUESHIFT_SHADER_VERTEX) {
    signatures = vertex_signatures;
    count = sizeof(vertex_signatures) / sizeof(vertex_signatures[0]);
  } else if (stage == TS_HUESHIFT_SHADER_FRAGMENT) {
    signatures = fragment_signatures;
    count = sizeof(fragment_signatures) / sizeof(fragment_signatures[0]);
  } else {
    return 0;
  }
  for (i = 0; i < count; i++)
    if (!strstr(source, signatures[i])) return 0;
  return 1;
}

/* Promove somente o varying de UV do par hueshift Android. Exigir uma unica
 * ocorrencia evita aceitar fontes concatenadas ou outro contrato inesperado. */
static inline char *ts_hueshift_shader_promote_uv(
    const char *source, enum ts_hueshift_shader_stage stage) {
  static const char from[] = "varying lowp vec2 v_texCoord";
  static const char to[] = "varying mediump vec2 v_texCoord";
  const char *match;
  size_t prefix;
  size_t source_size;
  char *patched;

  if (!ts_hueshift_shader_matches(source, stage)) return NULL;
  match = strstr(source, from);
  if (!match || strstr(match + sizeof(from) - 1u, from)) return NULL;

  prefix = (size_t)(match - source);
  source_size = strlen(source);
  patched = (char *)malloc(source_size + sizeof(to) - sizeof(from) + 1u);
  if (!patched) return NULL;
  memcpy(patched, source, prefix);
  memcpy(patched + prefix, to, sizeof(to) - 1u);
  memcpy(patched + prefix + sizeof(to) - 1u,
         match + sizeof(from) - 1u,
         source_size - prefix - (sizeof(from) - 1u) + 1u);
  return patched;
}

#endif
