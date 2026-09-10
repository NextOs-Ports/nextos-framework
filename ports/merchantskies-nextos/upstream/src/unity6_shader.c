/*
 * Unity 6 only ships an ESSL300 graphics backend.  Merchant of the Skies's compiled
 * HLSLcc shaders are deliberately regular, so a small source adapter can
 * select their existing loose-uniform branch and express the same shader in
 * ESSL100 for Mali-450.  This is not a general GLSL compiler: unsupported
 * GLES3 constructs are rejected by name and measured separately.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "unity6_shader.h"

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} text_buf;

static int buf_reserve(text_buf *buf, size_t extra)
{
    if (extra > SIZE_MAX - buf->len - 1)
        return 0;
    size_t need = buf->len + extra + 1;
    if (need <= buf->cap)
        return 1;
    size_t cap = buf->cap ? buf->cap : 1024;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    char *grown = realloc(buf->data, cap);
    if (!grown)
        return 0;
    buf->data = grown;
    buf->cap = cap;
    return 1;
}

static int buf_add_n(text_buf *buf, const char *text, size_t length)
{
    if (!buf_reserve(buf, length))
        return 0;
    memcpy(buf->data + buf->len, text, length);
    buf->len += length;
    buf->data[buf->len] = '\0';
    return 1;
}

static int buf_add(text_buf *buf, const char *text)
{
    return buf_add_n(buf, text, strlen(text));
}

static int buf_add_char(text_buf *buf, char value)
{
    return buf_add_n(buf, &value, 1);
}

static void set_reason(char *reason, size_t size, const char *format, ...)
{
    if (!reason || size == 0)
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(reason, size, format, args);
    va_end(args);
}

static int id_start(unsigned char value)
{
    return value == '_' || isalpha(value);
}

static int id_char(unsigned char value)
{
    return value == '_' || isalnum(value);
}

static int word_equal(const char *start, size_t length, const char *word)
{
    return strlen(word) == length && memcmp(start, word, length) == 0;
}

static int contains_word(const char *source, size_t length, const char *word)
{
    size_t wanted = strlen(word);
    for (size_t i = 0; i < length;) {
        if (i + 1 < length && source[i] == '/' && source[i + 1] == '/') {
            while (i < length && source[i] != '\n') i++;
            continue;
        }
        if (i + 1 < length && source[i] == '/' && source[i + 1] == '*') {
            i += 2;
            while (i + 1 < length &&
                   !(source[i] == '*' && source[i + 1] == '/')) i++;
            if (i + 1 < length) i += 2;
            continue;
        }
        if (id_start((unsigned char)source[i])) {
            size_t begin = i++;
            while (i < length && id_char((unsigned char)source[i])) i++;
            if (i - begin == wanted &&
                memcmp(source + begin, word, wanted) == 0)
                return 1;
        } else {
            i++;
        }
    }
    return 0;
}

/* Find a real function-like use in shader code.  Preprocessor definitions are
 * deliberately skipped: Unity's internal prologue says
 * `#define SAMPLE_TEXTURE_2D texture` in every stage, including vertex shaders
 * that never sample a texture.  Treating that alias as a fetch rejected a
 * physically measured, sampler-free boot shader on Mali-450. */
static int contains_code_call(const char *source, size_t length,
                              const char *name)
{
    size_t wanted = strlen(name);
    int line_start = 1;
    int preprocessor = 0;
    for (size_t i = 0; i < length;) {
        if (source[i] == '\n') {
            line_start = 1;
            preprocessor = 0;
            i++;
            continue;
        }
        if (i + 1 < length && source[i] == '/' && source[i + 1] == '/') {
            while (i < length && source[i] != '\n') i++;
            continue;
        }
        if (i + 1 < length && source[i] == '/' && source[i + 1] == '*') {
            i += 2;
            while (i + 1 < length &&
                   !(source[i] == '*' && source[i + 1] == '/')) {
                if (source[i] == '\n') {
                    line_start = 1;
                    preprocessor = 0;
                }
                i++;
            }
            if (i + 1 < length) i += 2;
            continue;
        }
        if (line_start) {
            if (source[i] == ' ' || source[i] == '\t' || source[i] == '\r') {
                i++;
                continue;
            }
            preprocessor = source[i] == '#';
            line_start = 0;
        }
        if (preprocessor) {
            i++;
            continue;
        }
        if (!id_start((unsigned char)source[i])) {
            i++;
            continue;
        }
        size_t begin = i++;
        while (i < length && id_char((unsigned char)source[i])) i++;
        if (i - begin != wanted || memcmp(source + begin, name, wanted) != 0)
            continue;
        size_t at = i;
        while (at < length && isspace((unsigned char)source[at])) at++;
        if (at < length && source[at] == '(')
            return 1;
    }
    return 0;
}

static char *skip_space(char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r') text++;
    return text;
}

static int starts_word(const char *text, const char *word)
{
    size_t length = strlen(word);
    return strncmp(text, word, length) == 0 &&
           !id_char((unsigned char)text[length]);
}

static int line_is_define(const char *line, const char *name)
{
    const char *at = line;
    while (*at == ' ' || *at == '\t') at++;
    if (strncmp(at, "#define", 7) != 0 ||
        id_char((unsigned char)at[7]))
        return 0;
    at += 7;
    while (*at == ' ' || *at == '\t') at++;
    size_t length = strlen(name);
    return strncmp(at, name, length) == 0 &&
           (!id_char((unsigned char)at[length]) || name[length - 1] == ')');
}

static int line_is_standalone_statement(const char *line, const char *name)
{
    const char *at = skip_space((char *)line);
    size_t length = strlen(name);
    if (strncmp(at, name, length) != 0 || id_char((unsigned char)at[length]))
        return 0;
    at = skip_space((char *)at + length);
    if (*at == ';')
        at = skip_space((char *)at + 1);
    return *at == '\0';
}

static int line_is_version(const char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    return strncmp(line, "#version", 8) == 0;
}

static int parse_layout_location(const char *line)
{
    const char *location = strstr(line, "location");
    if (!location)
        return -1;
    location += strlen("location");
    while (*location == ' ' || *location == '\t') location++;
    if (*location != '=')
        return -1;
    location++;
    while (*location == ' ' || *location == '\t') location++;
    if (!isdigit((unsigned char)*location))
        return -1;
    return (int)strtol(location, NULL, 10);
}

/* Remove leading layout(...) and UNITY_LOCATION(...) decorators.  HLSLcc
 * emits them only at global declarations. */
static int strip_leading_decorators(char *line)
{
    char *indent = line;
    while (*indent == ' ' || *indent == '\t') indent++;
    for (;;) {
        char *at = indent;
        if (strncmp(at, "layout", 6) == 0 &&
            !id_char((unsigned char)at[6])) {
            at += 6;
        } else if (strncmp(at, "UNITY_LOCATION", 14) == 0 &&
                   !id_char((unsigned char)at[14])) {
            at += 14;
        } else {
            break;
        }
        while (*at == ' ' || *at == '\t') at++;
        if (*at != '(')
            return 0;
        int depth = 1;
        char *end = at + 1;
        while (*end && depth) {
            if (*end == '(') depth++;
            else if (*end == ')') depth--;
            end++;
        }
        if (depth)
            return 0;
        while (*end == ' ' || *end == '\t') end++;
        memmove(indent, end, strlen(end) + 1);
    }
    return 1;
}

static int brace_delta(const char *line, int *in_comment)
{
    int delta = 0;
    for (size_t i = 0; line[i]; i++) {
        if (*in_comment) {
            if (line[i] == '*' && line[i + 1] == '/') {
                *in_comment = 0;
                i++;
            }
            continue;
        }
        if (line[i] == '/' && line[i + 1] == '*') {
            *in_comment = 1;
            i++;
            continue;
        }
        if (line[i] == '/' && line[i + 1] == '/')
            break;
        if (line[i] == '{') delta++;
        else if (line[i] == '}') delta--;
    }
    return delta;
}

static int last_identifier_before_semicolon(const char *line,
                                             char *name, size_t name_size)
{
    const char *end = strchr(line, ';');
    if (!end)
        return 0;
    while (end > line && !id_char((unsigned char)end[-1])) end--;
    const char *finish = end;
    while (end > line && id_char((unsigned char)end[-1])) end--;
    size_t length = (size_t)(finish - end);
    if (length == 0 || length >= name_size || !id_start((unsigned char)*end))
        return 0;
    memcpy(name, end, length);
    name[length] = '\0';
    return 1;
}

/* HLSLcc masks every shift count even when it is a compile-time constant.
 * This exact suffix is algebraically one bit left, and replacing it avoids
 * pretending ESSL100 has general integer bitwise support. */
static void simplify_constant_shift(char *line)
{
    static const char pattern[] = " << (1 & int(0x1F))";
    static const char replacement[] = " * 2";
    char *at;
    while ((at = strstr(line, pattern)) != NULL) {
        size_t tail = strlen(at + sizeof pattern - 1);
        memcpy(at, replacement, sizeof replacement - 1);
        memmove(at + sizeof replacement - 1,
                at + sizeof pattern - 1, tail + 1);
    }
}

enum sampler_kind {
    SAMPLER_2D,
    SAMPLER_CUBE,
    SAMPLER_3D,
    SAMPLER_EXTERNAL,
    SAMPLER_SHADOW_2D,
    SAMPLER_UNSUPPORTED,
};

typedef struct {
    char name[96];
    enum sampler_kind kind;
} sampler_rec;

static enum sampler_kind sampler_kind_for(const char *type, size_t length)
{
    if (word_equal(type, length, "sampler2D")) return SAMPLER_2D;
    if (word_equal(type, length, "samplerCube")) return SAMPLER_CUBE;
    if (word_equal(type, length, "sampler3D")) return SAMPLER_3D;
    if (word_equal(type, length, "samplerExternalOES"))
        return SAMPLER_EXTERNAL;
    if (word_equal(type, length, "sampler2DShadow"))
        return SAMPLER_SHADOW_2D;
    return SAMPLER_UNSUPPORTED;
}

static size_t collect_samplers(const char *source, size_t length,
                               sampler_rec *samplers, size_t capacity)
{
    size_t count = 0;
    for (size_t i = 0; i < length;) {
        if (i + 1 < length && source[i] == '/' && source[i + 1] == '/') {
            while (i < length && source[i] != '\n') i++;
            continue;
        }
        if (i + 1 < length && source[i] == '/' && source[i + 1] == '*') {
            i += 2;
            while (i + 1 < length &&
                   !(source[i] == '*' && source[i + 1] == '/')) i++;
            if (i + 1 < length) i += 2;
            continue;
        }
        if (!id_start((unsigned char)source[i])) {
            i++;
            continue;
        }
        size_t type_begin = i++;
        while (i < length && id_char((unsigned char)source[i])) i++;
        size_t type_len = i - type_begin;
        if (type_len < 7 ||
            memcmp(source + type_begin, "sampler", 7) != 0)
            continue;
        enum sampler_kind kind =
            sampler_kind_for(source + type_begin, type_len);
        while (i < length && isspace((unsigned char)source[i])) i++;
        if (i >= length || !id_start((unsigned char)source[i]))
            continue;
        size_t name_begin = i++;
        while (i < length && id_char((unsigned char)source[i])) i++;
        size_t name_len = i - name_begin;
        if (count < capacity && name_len < sizeof samplers[count].name) {
            memcpy(samplers[count].name, source + name_begin, name_len);
            samplers[count].name[name_len] = '\0';
            samplers[count].kind = kind;
            count++;
        }
    }
    return count;
}

static enum sampler_kind find_sampler(const sampler_rec *samplers,
                                      size_t count, const char *name,
                                      size_t name_len)
{
    for (size_t i = 0; i < count; i++)
        if (strlen(samplers[i].name) == name_len &&
            memcmp(samplers[i].name, name, name_len) == 0)
            return samplers[i].kind;
    return SAMPLER_UNSUPPORTED;
}

static const char *texture_replacement(enum st_shader_stage stage,
                                       enum sampler_kind kind, int lod,
                                       char *reason, size_t reason_size)
{
    if (stage == ST_SHADER_STAGE_VERTEX) {
        set_reason(reason, reason_size,
                   lod ? "vertex texture LOD (Mali reports zero vertex samplers)"
                       : "vertex texture fetch (Mali reports zero vertex samplers)");
        return NULL;
    }
    if (!lod) {
        switch (kind) {
        case SAMPLER_2D: return "texture2D";
        case SAMPLER_CUBE: return "textureCube";
        case SAMPLER_3D: return "texture3D";
        case SAMPLER_EXTERNAL: return "texture2D";
        case SAMPLER_SHADOW_2D: return "shadow2DEXT";
        default: break;
        }
    } else {
        switch (kind) {
        case SAMPLER_2D: return "texture2DLodEXT";
        case SAMPLER_CUBE: return "textureCubeLodEXT";
        case SAMPLER_3D:
            set_reason(reason, reason_size, "textureLod on sampler3D");
            return NULL;
        case SAMPLER_SHADOW_2D:
            set_reason(reason, reason_size,
                       "textureLod on sampler2DShadow");
            return NULL;
        default: break;
        }
    }
    set_reason(reason, reason_size, "texture call with unknown sampler");
    return NULL;
}

static int rewrite_tokens(const text_buf *body, enum st_shader_stage stage,
                          const char *frag_output, const char *frag_target,
                          text_buf *result,
                          char *reason, size_t reason_size)
{
    sampler_rec samplers[128];
    size_t sampler_count = collect_samplers(body->data, body->len, samplers,
                                            sizeof samplers / sizeof *samplers);
    const char *source = body->data;
    size_t length = body->len;
    for (size_t i = 0; i < length;) {
        if (i + 1 < length && source[i] == '/' && source[i + 1] == '/') {
            size_t begin = i;
            while (i < length && source[i] != '\n') i++;
            if (!buf_add_n(result, source + begin, i - begin)) return 0;
            continue;
        }
        if (i + 1 < length && source[i] == '/' && source[i + 1] == '*') {
            size_t begin = i;
            i += 2;
            while (i + 1 < length &&
                   !(source[i] == '*' && source[i + 1] == '/')) i++;
            if (i + 1 < length) i += 2;
            if (!buf_add_n(result, source + begin, i - begin)) return 0;
            continue;
        }
        if (!id_start((unsigned char)source[i])) {
            if (i + 11 <= length && source[i] == '0' && source[i + 1] == 'x' &&
                strncasecmp(source + i, "0xFFFFFFFFu", 11) == 0) {
                if (!buf_add(result, "(-1)")) return 0;
                i += 11;
                continue;
            }
            if (!buf_add_char(result, source[i++])) return 0;
            continue;
        }
        size_t begin = i++;
        while (i < length && id_char((unsigned char)source[i])) i++;
        size_t token_len = i - begin;

        if (frag_output && *frag_output &&
            strlen(frag_output) == token_len &&
            memcmp(source + begin, frag_output, token_len) == 0) {
            if (!buf_add(result, frag_target)) return 0;
            continue;
        }
        if (word_equal(source + begin, token_len, "roundEven")) {
            if (!buf_add(result, "st_roundEven")) return 0;
            continue;
        }
        int is_texture = word_equal(source + begin, token_len, "texture");
        int is_lod = word_equal(source + begin, token_len, "textureLod");
        if (is_texture || is_lod) {
            size_t at = i;
            while (at < length && isspace((unsigned char)source[at])) at++;
            if (at < length && source[at] == '(') {
                at++;
                while (at < length && isspace((unsigned char)source[at])) at++;
                if (at >= length || !id_start((unsigned char)source[at])) {
                    set_reason(reason, reason_size,
                               "texture call with non-identifier sampler");
                    return -1;
                }
                size_t sampler_begin = at++;
                while (at < length && id_char((unsigned char)source[at])) at++;
                enum sampler_kind kind =
                    find_sampler(samplers, sampler_count,
                                 source + sampler_begin, at - sampler_begin);
                const char *replacement = texture_replacement(
                    stage, kind, is_lod, reason, reason_size);
                if (!replacement)
                    return -1;
                if (!buf_add(result, replacement)) return 0;
                continue;
            }
        }
        if (word_equal(source + begin, token_len, "max") ||
            word_equal(source + begin, token_len, "min")) {
            size_t at = i;
            while (at < length && isspace((unsigned char)source[at])) at++;
            if (at < length && source[at] == '(') {
                size_t first = ++at;
                int nesting = 1;
                while (at < length && nesting > 0) {
                    if (source[at] == '(') nesting++;
                    else if (source[at] == ')') nesting--;
                    else if (source[at] == ',' && nesting == 1) break;
                    at++;
                }
                if (at < length && at > first &&
                    memmem(source + first, at - first,
                           "u_xlati", strlen("u_xlati"))) {
                    if (!buf_add(result,
                                 source[begin] == 'm' && source[begin + 1] == 'a'
                                     ? "st_maxi" : "st_mini"))
                        return 0;
                    continue;
                }
            }
        }
        if (!buf_add_n(result, source + begin, token_len)) return 0;
    }
    return 1;
}

static int add_preamble(text_buf *output, const char *source, size_t length)
{
    if (!buf_add(output, "#version 100\n")) return 0;
    if (contains_word(source, length, "dFdx") ||
        contains_word(source, length, "dFdy") ||
        contains_word(source, length, "fwidth"))
        if (!buf_add(output,
                     "#extension GL_OES_standard_derivatives : enable\n"))
            return 0;
    if (contains_word(source, length, "textureLod"))
        if (!buf_add(output,
                     "#extension GL_EXT_shader_texture_lod : enable\n"))
            return 0;
    if (contains_word(source, length, "sampler2DShadow"))
        if (!buf_add(output,
                     "#extension GL_EXT_shadow_samplers : enable\n"))
            return 0;
    if (contains_word(source, length, "sampler3D"))
        if (!buf_add(output, "#extension GL_OES_texture_3D : enable\n"))
            return 0;
    if (contains_word(source, length, "samplerExternalOES"))
        if (!buf_add(output,
                     "#extension GL_OES_EGL_image_external : enable\n"))
            return 0;
    if (!buf_add(output,
                 "precision highp float;\n"
                 "precision highp int;\n"
                 "#define ST_UNITY6_GLES2 1\n"
                 "int st_maxi(int a, int b) { return a > b ? a : b; }\n"
                 "int st_mini(int a, int b) { return a < b ? a : b; }\n"))
        return 0;
    if (contains_word(source, length, "roundEven")) {
        static const char helper[] =
            "highp float st_roundEven(highp float x) {\n"
            "    highp float f = floor(x);\n"
            "    highp float d = x - f;\n"
            "    return d < 0.5 ? f : (d > 0.5 ? f + 1.0 : "
            "(mod(f, 2.0) == 0.0 ? f : f + 1.0));\n"
            "}\n"
            "highp vec2 st_roundEven(highp vec2 x) { return vec2("
            "st_roundEven(x.x), st_roundEven(x.y)); }\n"
            "highp vec3 st_roundEven(highp vec3 x) { return vec3("
            "st_roundEven(x.x), st_roundEven(x.y), st_roundEven(x.z)); }\n"
            "highp vec4 st_roundEven(highp vec4 x) { return vec4("
            "st_roundEven(x.x), st_roundEven(x.y), st_roundEven(x.z), "
            "st_roundEven(x.w)); }\n";
        if (!buf_add(output, helper)) return 0;
    }
    return 1;
}

static int reject_known_unsupported(enum st_shader_stage stage,
                                    const char *source, size_t length,
                                    char *reason, size_t reason_size)
{
    static const char *const words[] = {
        "uvec2", "uvec3", "uvec4", "uint",
        "sampler2DArray", "samplerCubeShadow",
        "texelFetch", "gl_InstanceID", "gl_VertexID", "gl_Layer",
        "image2D", "atomic_uint", "floatBitsToUint", "floatBitsToInt",
        "uintBitsToFloat", "intBitsToFloat", "bitfieldExtract",
        "bitfieldInsert", "bitCount", "findLSB", "findMSB",
    };
    for (size_t i = 0; i < sizeof words / sizeof *words; i++) {
        if (contains_word(source, length, words[i])) {
            set_reason(reason, reason_size, "%s", words[i]);
            return 1;
        }
    }
    if (contains_word(source, length, "gl_FragDepth")) {
        set_reason(reason, reason_size,
                   "gl_FragDepth (driver has no EXT_frag_depth)");
        return 1;
    }
    if (contains_word(source, length, "flat") ||
        contains_word(source, length, "noperspective")) {
        set_reason(reason, reason_size, "non-smooth interpolation qualifier");
        return 1;
    }
    if (contains_word(source, length, "transpose")) {
        set_reason(reason, reason_size, "transpose");
        return 1;
    }
    if (stage == ST_SHADER_STAGE_VERTEX) {
        static const char *const calls[] = {
            "texture", "textureLod", "texture2D", "textureCube", "texture3D",
            "SAMPLE_TEXTURE_2D", "SAMPLE_TEXTURE_2D_LOD",
            "SAMPLE_TEXTURE_CUBE", "UNITY_SAMPLE_TEX2D",
            "UNITY_SAMPLE_TEX2D_SAMPLER", "UNITY_SAMPLE_TEXCUBE",
        };
        for (size_t i = 0; i < sizeof calls / sizeof *calls; i++) {
            if (!contains_code_call(source, length, calls[i]))
                continue;
            set_reason(reason, reason_size,
                       "vertex texture fetch (Mali reports zero vertex samplers)");
            return 1;
        }
    }
    return 0;
}

int st_unity6_translate_shader(enum st_shader_stage stage,
                               const char *source, size_t source_len,
                               char **translated, size_t *translated_len,
                               char *reason, size_t reason_size)
{
    if (translated) *translated = NULL;
    if (translated_len) *translated_len = 0;
    if (reason && reason_size) reason[0] = '\0';
    if (!source || !translated || !translated_len ||
        (stage != ST_SHADER_STAGE_VERTEX &&
         stage != ST_SHADER_STAGE_FRAGMENT)) {
        set_reason(reason, reason_size, "invalid translator arguments");
        return ST_SHADER_UNSUPPORTED;
    }
    if (!memmem(source, source_len, "#version 300 es", 15)) {
        if (memmem(source, source_len, "#version 310 es", 15)) {
            set_reason(reason, reason_size, "ESSL 3.10/compute shader");
            return ST_SHADER_UNSUPPORTED;
        }
        return ST_SHADER_PASSTHROUGH;
    }
    if (reject_known_unsupported(stage, source, source_len,
                                 reason, reason_size))
        return ST_SHADER_UNSUPPORTED;

    text_buf body = { 0 };
    char frag_output[96] = "";
    char frag_target[32] = "gl_FragColor";
    int output_location = -1;
    int depth = 0, in_comment = 0, saw_version = 0;

    size_t position = 0;
    while (position < source_len) {
        size_t end = position;
        while (end < source_len && source[end] != '\n') end++;
        size_t line_len = end - position;
        char *line = malloc(line_len + 1);
        if (!line) goto no_memory;
        memcpy(line, source + position, line_len);
        line[line_len] = '\0';

        if (line_is_version(line)) {
            saw_version = 1;
            free(line);
            position = end < source_len ? end + 1 : end;
            continue;
        }

        const char *replacement = NULL;
        if (line_is_define(line, "HLSLCC_ENABLE_UNIFORM_BUFFERS"))
            replacement = "#define HLSLCC_ENABLE_UNIFORM_BUFFERS 0";
        else if (line_is_define(line, "UNITY_SUPPORTS_UNIFORM_LOCATION"))
            replacement = "#define UNITY_SUPPORTS_UNIFORM_LOCATION 0";
        else if (line_is_define(line, "UNITY_LOCATION(x)"))
            replacement = "#define UNITY_LOCATION(x)";
        else if (line_is_define(line, "UNITY_BINDING(x)"))
            replacement = "#define UNITY_BINDING(x)";
        else if (line_is_define(line, "ATTRIBUTE_IN"))
            replacement = "#define ATTRIBUTE_IN attribute";
        else if (line_is_define(line, "VARYING_IN"))
            replacement = "#define VARYING_IN varying";
        else if (line_is_define(line, "VARYING_OUT"))
            replacement = "#define VARYING_OUT varying";
        else if (line_is_define(line, "DECLARE_FRAG_COLOR"))
            replacement = "#define DECLARE_FRAG_COLOR";
        else if (line_is_define(line, "FRAG_COLOR"))
            replacement = "#define FRAG_COLOR gl_FragColor";
        else if (line_is_define(line, "SAMPLE_TEXTURE_2D"))
            replacement = "#define SAMPLE_TEXTURE_2D texture2D";

        if (replacement) {
            if (!buf_add(&body, replacement) || !buf_add_char(&body, '\n')) {
                free(line);
                goto no_memory;
            }
            free(line);
            position = end < source_len ? end + 1 : end;
            continue;
        }

        /* Unity's internal shader macros expand DECLARE_FRAG_COLOR to an ES3
         * output declaration.  Its separate invocation ends in a semicolon;
         * after the macro becomes empty that would leave a bare global `;`.
         * glslang accepts it, but the physical Mali compiler correctly rejects
         * it as "Typename expected". Drop the now-empty declaration line. */
        if (stage == ST_SHADER_STAGE_FRAGMENT &&
            line_is_standalone_statement(line, "DECLARE_FRAG_COLOR")) {
            free(line);
            position = end < source_len ? end + 1 : end;
            continue;
        }

        int location = parse_layout_location(line);
        if (!strip_leading_decorators(line)) {
            set_reason(reason, reason_size, "malformed layout qualifier");
            free(line);
            goto unsupported;
        }
        simplify_constant_shift(line);

        char *at = skip_space(line);
        if (depth == 0) {
            if (starts_word(at, "smooth")) {
                char *after = skip_space(at + strlen("smooth"));
                memmove(at, after, strlen(after) + 1);
            }
            if (starts_word(at, "in") || starts_word(at, "out")) {
                int is_out = starts_word(at, "out");
                size_t qualifier_len = is_out ? 3 : 2;
                if (stage == ST_SHADER_STAGE_FRAGMENT && is_out) {
                    char name[96];
                    if (!last_identifier_before_semicolon(at, name,
                                                          sizeof name)) {
                        set_reason(reason, reason_size, "malformed fragment output");
                        free(line);
                        goto unsupported;
                    }
                    if (contains_word(at, strlen(at), "vec4"))
                        snprintf(frag_target, sizeof frag_target,
                                 "gl_FragColor");
                    else if (contains_word(at, strlen(at), "vec3"))
                        snprintf(frag_target, sizeof frag_target,
                                 "gl_FragColor.xyz");
                    else if (contains_word(at, strlen(at), "vec2"))
                        snprintf(frag_target, sizeof frag_target,
                                 "gl_FragColor.xy");
                    else if (contains_word(at, strlen(at), "float"))
                        snprintf(frag_target, sizeof frag_target,
                                 "gl_FragColor.x");
                    else {
                        set_reason(reason, reason_size,
                                   "unsupported fragment output type");
                        free(line);
                        goto unsupported;
                    }
                    if ((location > 0) || frag_output[0]) {
                        set_reason(reason, reason_size,
                                   "multiple render targets");
                        free(line);
                        goto unsupported;
                    }
                    snprintf(frag_output, sizeof frag_output, "%s", name);
                    output_location = location < 0 ? 0 : location;
                    free(line);
                    position = end < source_len ? end + 1 : end;
                    continue;
                }
                const char *qualifier =
                    stage == ST_SHADER_STAGE_VERTEX
                        ? (is_out ? "varying" : "attribute")
                        : "varying";
                size_t indent_len = (size_t)(at - line);
                if (!buf_add_n(&body, line, indent_len) ||
                    !buf_add(&body, qualifier) ||
                    !buf_add(&body, at + qualifier_len) ||
                    !buf_add_char(&body, '\n')) {
                    free(line);
                    goto no_memory;
                }
                depth += brace_delta(line, &in_comment);
                free(line);
                position = end < source_len ? end + 1 : end;
                continue;
            }
        }

        if (!buf_add(&body, line) || !buf_add_char(&body, '\n')) {
            free(line);
            goto no_memory;
        }
        depth += brace_delta(line, &in_comment);
        free(line);
        position = end < source_len ? end + 1 : end;
    }

    if (!saw_version) {
        set_reason(reason, reason_size, "missing #version line");
        goto unsupported;
    }
    if (stage == ST_SHADER_STAGE_FRAGMENT && output_location > 0) {
        set_reason(reason, reason_size, "fragment output location > 0");
        goto unsupported;
    }

    text_buf output = { 0 };
    if (!add_preamble(&output, source, source_len)) {
        free(output.data);
        goto no_memory;
    }
    int rewritten = rewrite_tokens(&body, stage, frag_output, frag_target,
                                   &output,
                                   reason, reason_size);
    if (rewritten <= 0) {
        free(output.data);
        if (rewritten < 0) goto unsupported;
        goto no_memory;
    }
    free(body.data);
    *translated = output.data;
    *translated_len = output.len;
    return ST_SHADER_TRANSLATED;

unsupported:
    free(body.data);
    return ST_SHADER_UNSUPPORTED;
no_memory:
    free(body.data);
    set_reason(reason, reason_size, "out of memory");
    return ST_SHADER_NO_MEMORY;
}
