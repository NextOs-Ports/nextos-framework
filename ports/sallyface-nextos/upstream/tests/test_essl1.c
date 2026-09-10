#include "essl1.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void contains(const char *text, const char *needle)
{
    assert(text != NULL);
    assert(strstr(text, needle) != NULL);
}

static void excludes(const char *text, const char *needle)
{
    assert(text != NULL);
    assert(strstr(text, needle) == NULL);
}

static void write_fixture(const char *dir, const char *name,
                          const char *text, size_t length)
{
    if (!dir)
        return;
    char path[512];
    int n = snprintf(path, sizeof path, "%s/%s", dir, name);
    assert(n > 0 && (size_t)n < sizeof path);
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    assert(fwrite(text, 1, length, fp) == length);
    assert(fclose(fp) == 0);
}

int main(int argc, char **argv)
{
    const char *fixture_dir = argc == 2 ? argv[1] : NULL;
    static const char vertex_fallback[] =
        "#version 100\n"
        "precision highp float;\n"
        "in vec4 in_POSITION0;\n"
        "out vec2 vs_TEXCOORD0;\n"
        "void main(){vs_TEXCOORD0=in_POSITION0.xy;gl_Position=in_POSITION0;}\n";
    size_t length = 0;
    char *translated = sf_essl1_translate(
        vertex_fallback, sizeof vertex_fallback - 1, 0, &length);
    assert(translated != NULL && length == strlen(translated));
    contains(translated, "#version 100\n");
    contains(translated, "attribute vec4 in_POSITION0;");
    contains(translated, "varying vec2 vs_TEXCOORD0;");
    excludes(translated, "\nin vec4");
    excludes(translated, "\nout vec2");
    free(translated);

    static const char fragment_fallback[] =
        "#version 100\n"
        "precision mediump float;\n"
        "in vec2 vs_TEXCOORD0;\n"
        "layout(location = 0) out vec4 SV_Target0;\n"
        "uniform sampler2D _MainTex;\n"
        "void main(){SV_Target0=texture(_MainTex,vs_TEXCOORD0);}\n";
    translated = sf_essl1_translate(
        fragment_fallback, sizeof fragment_fallback - 1, 1, &length);
    contains(translated, "varying vec2 vs_TEXCOORD0;");
    contains(translated, "gl_FragColor=texture2D(");
    excludes(translated, "layout(location");
    free(translated);

    static const char no_version_fallback[] =
        "in vec4 position;\n"
        "void main(){gl_Position=position;}\n";
    translated = sf_essl1_translate(
        no_version_fallback, sizeof no_version_fallback - 1, 0, &length);
    contains(translated, "#version 100\n");
    contains(translated, "attribute vec4 position;");
    free(translated);

    static const char genuine_essl100[] =
        "#version 100\n"
        "attribute vec4 position;\n"
        "float helper(in float value){return value;}\n"
        "void main(){gl_Position=position*helper(1.0);}\n";
    translated = sf_essl1_translate(
        genuine_essl100, sizeof genuine_essl100 - 1, 0, &length);
    assert(translated == NULL);

    /* Template real embutido pelo libunity 2022.3: a Unity monta o shader com
     * aliases, portanto procurar apenas `in`/`out` no inicio da declaracao nao
     * cobre o fallback raw-EGL. */
    static const char unity_vertex_fallback[] =
        "#version 100\n"
        "#define ATTRIBUTE_IN in\n"
        "#define VARYING_IN in\n"
        "#define VARYING_OUT out\n"
        "#define DECLARE_FRAG_COLOR out vec4 fragColor\n"
        "#define FRAG_COLOR fragColor\n"
        "#define SAMPLE_TEXTURE_2D texture\n"
        "ATTRIBUTE_IN highp vec4 in_POSITION0;\n"
        "VARYING_OUT mediump vec2 vs_TEXCOORD0;\n"
        "void main(){vs_TEXCOORD0=in_POSITION0.xy;gl_Position=in_POSITION0;}\n";
    assert(sf_essl1_classify(unity_vertex_fallback,
                             sizeof unity_vertex_fallback - 1) &
           SF_ESSL1_REASON_UNITY_ALIASES);
    translated = sf_essl1_translate(
        unity_vertex_fallback, sizeof unity_vertex_fallback - 1, 0, &length);
    contains(translated, "#define ATTRIBUTE_IN attribute\n");
    contains(translated, "#define VARYING_IN varying\n");
    contains(translated, "#define VARYING_OUT varying\n");
    contains(translated, "#undef DECLARE_FRAG_COLOR\n");
    contains(translated, "#define FRAG_COLOR gl_FragColor\n");
    contains(translated, "#define SAMPLE_TEXTURE_2D texture2D\n");
    assert(sf_essl1_classify(translated, length) == 0);
    write_fixture(fixture_dir, "unity-alias.vert", translated, length);
    free(translated);

    static const char unity_fragment_fallback[] =
        "#version 100\n"
        "precision highp float;\n"
        "precision highp int;\n"
        "#define ATTRIBUTE_IN in\n"
        "#define VARYING_IN in\n"
        "#define VARYING_OUT out\n"
        "#define DECLARE_FRAG_COLOR out vec4 fragColor\n"
        "#define FRAG_COLOR fragColor\n"
        "#define SAMPLE_TEXTURE_2D texture\n"
        "uniform sampler2D _MainTex;\n"
        "VARYING_IN mediump vec2 vs_TEXCOORD0;\n"
        "#ifdef DECLARE_FRAG_COLOR\n"
        "DECLARE_FRAG_COLOR;\n"
        "#endif\n"
        "void main(){FRAG_COLOR=SAMPLE_TEXTURE_2D(_MainTex,vs_TEXCOORD0);}\n";
    translated = sf_essl1_translate(
        unity_fragment_fallback, sizeof unity_fragment_fallback - 1, 1,
        &length);
    contains(translated, "#define VARYING_IN varying\n");
    contains(translated, "#undef DECLARE_FRAG_COLOR\n");
    contains(translated, "#define FRAG_COLOR gl_FragColor\n");
    contains(translated, "#define SAMPLE_TEXTURE_2D texture2D\n");
    assert(sf_essl1_classify(translated, length) == 0);
    write_fixture(fixture_dir, "unity-alias.frag", translated, length);
    free(translated);

    static const char unity_legacy_vertex[] =
        "#version 100\n"
        "#define ATTRIBUTE_IN attribute\n"
        "#define VARYING_IN varying\n"
        "#define VARYING_OUT varying\n"
        "#define FRAG_COLOR gl_FragColor\n"
        "#define SAMPLE_TEXTURE_2D texture2D\n"
        "ATTRIBUTE_IN highp vec4 in_POSITION0;\n"
        "VARYING_OUT mediump vec2 vs_TEXCOORD0;\n"
        "void main(){vs_TEXCOORD0=in_POSITION0.xy;gl_Position=in_POSITION0;}\n";
    assert(sf_essl1_classify(unity_legacy_vertex,
                             sizeof unity_legacy_vertex - 1) == 0);
    translated = sf_essl1_translate(
        unity_legacy_vertex, sizeof unity_legacy_vertex - 1, 0, &length);
    assert(translated == NULL);

    static const char partial_alias_block[] =
        "#version 100\n"
        "#define ATTRIBUTE_IN in\n"
        "ATTRIBUTE_IN highp vec4 in_POSITION0;\n";
    assert(sf_essl1_classify(partial_alias_block,
                             sizeof partial_alias_block - 1) ==
           SF_ESSL1_REASON_UNITY_ALIAS_PARTIAL);
    translated = sf_essl1_translate(
        partial_alias_block, sizeof partial_alias_block - 1, 0, &length);
    assert(translated == NULL);

    return 0;
}
