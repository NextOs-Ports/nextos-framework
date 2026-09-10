#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "../src/hueshift_shader_fix.h"

static const char vertex_shader[] =
    "attribute vec3 _offset;\n"
    "uniform lowp vec3 HSV;\n"
    "varying lowp vec2 v_texCoord;\n"
    "varying lowp mat3 vHSV;\n"
    "void main(){ lowp vec3 pos; pos.x=floor(pos.x * scale); "
    "v_texCoord = tcoord; vHSV = createHSVMatrix( HSV ); }\n";

static const char fragment_shader[] =
    "varying lowp vec2 v_texCoord;\n"
    "varying lowp mat3 vHSV;\n"
    "uniform sampler2D InputTexture;\n"
    "uniform lowp vec3 HSV;\n"
    "uniform lowp float huePercent;\n"
    "lowp vec3 rgb2hsv(lowp vec3 c);\n"
    "lowp vec3 hsv2rgb(lowp vec3 c);\n"
    "void main(){ texture2D( InputTexture, v_texCoord ); }\n";

int main(void) {
  const char depth_shader[] =
      "varying lowp vec2 v_texCoord; varying lowp mat3 vHSV; "
      "uniform sampler2D InputTexture; vec3 transformHSV(vec3 a, vec3 b);";
  const char generic_texture[] =
      "varying lowp vec2 v_texCoord; uniform sampler2D InputTexture;";
  char duplicate[sizeof(vertex_shader) * 2u];
  char *patched_vertex;
  char *patched_fragment;

  patched_vertex = ts_hueshift_shader_promote_uv(
      vertex_shader, TS_HUESHIFT_SHADER_VERTEX);
  patched_fragment = ts_hueshift_shader_promote_uv(
      fragment_shader, TS_HUESHIFT_SHADER_FRAGMENT);
  assert(patched_vertex != NULL);
  assert(patched_fragment != NULL);
  assert(strstr(patched_vertex, "varying mediump vec2 v_texCoord") != NULL);
  assert(strstr(patched_fragment, "varying mediump vec2 v_texCoord") != NULL);
  assert(strstr(patched_vertex, "uniform lowp vec3 HSV") != NULL);
  assert(strstr(patched_fragment, "varying lowp mat3 vHSV") != NULL);

  assert(ts_hueshift_shader_promote_uv(
             vertex_shader, TS_HUESHIFT_SHADER_FRAGMENT) == NULL);
  assert(ts_hueshift_shader_promote_uv(
             fragment_shader, TS_HUESHIFT_SHADER_VERTEX) == NULL);
  assert(ts_hueshift_shader_promote_uv(
             depth_shader, TS_HUESHIFT_SHADER_FRAGMENT) == NULL);
  assert(ts_hueshift_shader_promote_uv(
             generic_texture, TS_HUESHIFT_SHADER_FRAGMENT) == NULL);

  strcpy(duplicate, vertex_shader);
  strcat(duplicate, "varying lowp vec2 v_texCoord;");
  assert(ts_hueshift_shader_promote_uv(
             duplicate, TS_HUESHIFT_SHADER_VERTEX) == NULL);

  free(patched_vertex);
  free(patched_fragment);
  return 0;
}
