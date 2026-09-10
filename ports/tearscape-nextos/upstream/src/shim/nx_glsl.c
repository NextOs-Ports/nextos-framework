/* nx_glsl.c — GLSL ES 300 -> ES 100 mechanical translator + UBO->uniform
 * emulation registry. Iteration is device-log driven: what the Mali compiler
 * rejects shows up in nx_shim.log via glCompileShader.
 *
 * std140 layout is computed for the member types Godot's shaders use.
 */
#define _GNU_SOURCE
#include "nx_shim.h"
#include <ctype.h>
#include <limits.h>

#define REAL(ret, name, args) \
    static ret (*p_##name) args; \
    if (!p_##name) p_##name = nx_mali_sym(#name);

/* ---------- block registry ---------- */
typedef struct {
    char name[64];
    int type;        /* 0=float 1=vec2 2=vec3 3=vec4 4=int 5=ivec2 6=ivec3 7=ivec4 8=mat4 9=mat3 10=bool */
    int array;       /* 0 = scalar */
    int offset;      /* std140 */
} NxMember;

typedef struct {
    char name[64];
    int nmembers;
    NxMember m[96];
    int size;
    char *decl_signature; /* all parsed declarations, including custom types */
} NxBlock;

#define NX_MAX_BLOCKS 64
static NxBlock blocks[NX_MAX_BLOCKS];
static int nblocks;

static int block_signature_equal(const NxBlock *a, const NxBlock *b)
{
    if (strcmp(a->name, b->name) ||
        a->nmembers != b->nmembers ||
        a->size != b->size ||
        !a->decl_signature || !b->decl_signature ||
        strcmp(a->decl_signature, b->decl_signature))
        return 0;
    for (int i = 0; i < a->nmembers; i++) {
        const NxMember *am = &a->m[i];
        const NxMember *bm = &b->m[i];
        if (strcmp(am->name, bm->name) ||
            am->type != bm->type ||
            am->array != bm->array ||
            am->offset != bm->offset)
            return 0;
    }
    return 1;
}

static NxBlock *block_find_layout(const NxBlock *candidate)
{
    for (int i = 0; i < nblocks; i++)
        if (block_signature_equal(&blocks[i], candidate)) return &blocks[i];
    return NULL;
}

/* ---------- per-shader / per-program records ---------- */
typedef struct {
    GLuint shader;
    int nblocks;
    int block_indices[16];
    int block_bindings[16];
    int nattrs;
    struct { int loc; char name[64]; } attrs[24];
    int ninst;
    char inst_from[8][66];
    char inst_to[8][66];
} NxShaderRec;

#define NX_MAX_SHADERS 4096
/* Keep the registry sparse.  Tearscape compiles more than 256 distinct shader
 * object ids while streaming later rooms; embedding every 2.8 KiB record in
 * BSS either imposes a large permanent cost or makes a fixed small table crash
 * when it fills.  Individual allocations keep the old memory footprint while
 * allowing the full run to grow only as much as it actually needs. */
static NxShaderRec *shaders[NX_MAX_SHADERS];

static NxShaderRec *shader_rec(GLuint id, int create)
{
    for (int i = 0; i < NX_MAX_SHADERS; i++)
        if (shaders[i] && shaders[i]->shader == id) return shaders[i];
    if (!create) return NULL;
    for (int i = 0; i < NX_MAX_SHADERS; i++)
        if (!shaders[i]) {
            NxShaderRec *rec = calloc(1, sizeof(*rec));
            if (!rec) return NULL;
            rec->shader = id;
            shaders[i] = rec;
            if (i == 256 || i == 512 || i == 1024 || i == 2048)
                nx_log("GLSL shader registry grew to %d records (shader %u)", i + 1, id);
            return rec;
        }
    return NULL;
}

typedef struct {
    GLuint prog;
    int nblocks;
    struct {
        NxBlock *blk;
        int binding;             /* UBO binding point */
        GLint locs[96];          /* uniform locations, -2 = unresolved */
    } b[16];
} NxProgRec;

#define NX_MAX_PROGS 4096
static NxProgRec *progs[NX_MAX_PROGS];
static NxProgRec *cur_prog;

static NxProgRec *prog_rec(GLuint id, int create)
{
    for (int i = 0; i < NX_MAX_PROGS; i++)
        if (progs[i] && progs[i]->prog == id) return progs[i];
    if (!create) return NULL;
    for (int i = 0; i < NX_MAX_PROGS; i++)
        if (!progs[i]) {
            NxProgRec *rec = calloc(1, sizeof(*rec));
            if (!rec) return NULL;
            rec->prog = id;
            progs[i] = rec;
            if (i == 256 || i == 512 || i == 1024 || i == 2048)
                nx_log("GLSL program registry grew to %d records (program %u)", i + 1, id);
            return rec;
        }
    return NULL;
}

/* ---------- std140 ---------- */
static int type_base_align(int t)
{
    switch (t) {
    case 0: case 4: case 10: return 4;
    case 1: case 5: return 8;
    default: return 16;
    }
}

static int type_size(int t)
{
    switch (t) {
    case 0: case 4: case 10: return 4;
    case 1: case 5: return 8;
    case 2: case 6: return 12;
    case 3: case 7: return 16;
    case 8: return 64;
    case 9: return 48; /* 3 columns, vec4 stride */
    default: return 16;
    }
}

static int parse_type(const char *s, int *len)
{
    static const struct { const char *n; int t; } tab[] = {
        {"float",0},{"vec2",1},{"vec3",2},{"vec4",3},
        {"int",4},{"ivec2",5},{"ivec3",6},{"ivec4",7},
        {"uint",4},{"uvec2",5},{"uvec3",6},{"uvec4",7},
        {"mat4",8},{"mat3",9},{"bool",10},
        {"highp",-1},{"mediump",-1},{"lowp",-1},
    };
    for (unsigned i = 0; i < sizeof(tab)/sizeof(tab[0]); i++) {
        size_t l = strlen(tab[i].n);
        if (!strncmp(s, tab[i].n, l) && (s[l]==' '||s[l]=='\t')) {
            *len = l;
            return tab[i].t;
        }
    }
    *len = 0;
    return -2;
}

static int parse_identifier(const char **cursor, char *out, size_t out_size)
{
    const char *p = *cursor;
    while (*p == ' ' || *p == '\t') p++;
    if (!(isalpha((unsigned char)*p) || *p == '_')) return 0;
    const char *start = p++;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    size_t len = (size_t)(p - start);
    if (len >= out_size) return 0;
    memcpy(out, start, len);
    out[len] = 0;
    *cursor = p;
    return 1;
}

static int parse_array_count(const char *expr, const char *source)
{
    if (!expr || !*expr) return 0;
    char *end = NULL;
    long value = strtol(expr, &end, 10);
    if (end != expr && *end == 0 && value > 0 && value <= INT_MAX)
        return (int)value;

    /* Godot uses constants such as MAX_GLOBAL_SHADER_UNIFORMS in UBO array
     * declarations. Preserve the expression in GLSL and resolve the simple
     * numeric #define here so the CPU std140 shadow has the right stride. */
    char needle[128];
    snprintf(needle, sizeof(needle), "#define %s ", expr);
    const char *hit = strstr(source, needle);
    if (!hit) return 0;
    hit += strlen(needle);
    value = strtol(hit, &end, 10);
    return (end != hit && value > 0 && value <= INT_MAX) ? (int)value : 0;
}

static void block_layout(NxBlock *b)
{
    int off = 0;
    for (int i = 0; i < b->nmembers; i++) {
        NxMember *m = &b->m[i];
        int align = type_base_align(m->type);
        int size = type_size(m->type);
        if (m->array > 0) {
            align = 16;
            size = ((size + 15) & ~15) * m->array;
        }
        off = (off + align - 1) & ~(align - 1);
        m->offset = off;
        off += size;
    }
    b->size = (off + 15) & ~15;
}

/* ---------- translator ---------- */
typedef struct {
    char *buf;
    size_t len, cap;
} Sb;

static void sb_put(Sb *sb, const char *s, size_t l)
{
    if (sb->len + l + 1 > sb->cap) {
        sb->cap = (sb->len + l + 1) * 2;
        sb->buf = realloc(sb->buf, sb->cap);
    }
    memcpy(sb->buf + sb->len, s, l);
    sb->len += l;
    sb->buf[sb->len] = 0;
}

static void sb_str(Sb *sb, const char *s) { sb_put(sb, s, strlen(s)); }

static const char *type_name(int t)
{
    static const char *n[] = {"float","vec2","vec3","vec4","int","ivec2","ivec3","ivec4","mat4","mat3","bool"};
    return (t >= 0 && t <= 10) ? n[t] : "vec4";
}

/* Fragment highp is optional in GLES2.  The Mali-4xx path needs the existing
 * mediump downgrade, while newer GLES2 implementations reject a program when
 * that downgrade makes a shared uniform/varying disagree with the highp
 * vertex declaration.  Ask the real driver instead of guessing from the
 * firmware or renderer name. */
static int fragment_highp_supported(void)
{
    static int initialized;
    static int supported;
    if (!initialized) {
        typedef void (GL_APIENTRYP NxGetShaderPrecisionFormat)(
            GLenum, GLenum, GLint *, GLint *);
        NxGetShaderPrecisionFormat get_precision =
            (NxGetShaderPrecisionFormat)nx_mali_sym(
                "glGetShaderPrecisionFormat");
        GLint range[2] = {0, 0};
        GLint precision = 0;
        if (get_precision)
            get_precision(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range,
                          &precision);
        supported = get_precision &&
            (range[0] != 0 || range[1] != 0 || precision != 0);
        initialized = 1;
        nx_log("fragment highp: %s range=%d,%d precision=%d",
               supported ? "supported" : "unsupported",
               range[0], range[1], precision);
    }
    return supported;
}

/* strip C-style float suffixes (2.0f) that ES100 rejects */
static void strip_f_suffix(char *s)
{
    for (char *p = s; *p; p++) {
        if ((*p == 'f' || *p == 'F') && p > s && (isdigit((unsigned char)p[-1]) || p[-1] == '.') &&
            !isalnum((unsigned char)p[1]) && p[1] != '_') {
            /* not hex (0x0f): scan back over [0-9a-fA-F.] for "0x" */
            const char *q = p - 1;
            while (q > s && (isxdigit((unsigned char)*q) || *q == '.')) q--;
            if (!(q > s && (*q == 'x' || *q == 'X')))
                memmove(p, p + 1, strlen(p));
        }
    }
}

/* Strip the unsigned suffix from integer literals (50u, 0x20U). GLSL ES
 * 1.00 has no unsigned integers, but blindly replacing every trailing `u`
 * would also corrupt identifiers, so first validate the complete token. */
static void strip_u_suffix(char *s)
{
    for (char *p = s; *p;) {
        if ((*p == 'u' || *p == 'U') &&
            !isalnum((unsigned char)p[1]) && p[1] != '_') {
            char *start = p;
            while (start > s && isalnum((unsigned char)start[-1])) start--;

            int numeric = start < p && isdigit((unsigned char)start[0]);
            if (numeric && p - start > 2 && start[0] == '0' &&
                (start[1] == 'x' || start[1] == 'X')) {
                for (char *q = start + 2; q < p; q++) {
                    if (!isxdigit((unsigned char)*q)) {
                        numeric = 0;
                        break;
                    }
                }
            } else if (numeric) {
                for (char *q = start; q < p; q++) {
                    if (!isdigit((unsigned char)*q)) {
                        numeric = 0;
                        break;
                    }
                }
            }

            if (numeric) {
                memmove(p, p + 1, strlen(p));
                continue;
            }
        }
        p++;
    }
}

/* replace whole-word occurrences, in place on a malloc'd string */
static char *replace_all(char *src, const char *from, const char *to)
{
    size_t fl = strlen(from), tl = strlen(to);
    Sb out = {0};
    char *p = src;
    while (*p) {
        char *hit = strstr(p, from);
        if (!hit) { sb_str(&out, p); break; }
        int lb = (hit == src) || (!isalnum((unsigned char)hit[-1]) && hit[-1] != '_');
        int rb = !isalnum((unsigned char)hit[fl]) && hit[fl] != '_';
        sb_put(&out, p, hit - p);
        if (lb && rb) sb_put(&out, to, tl);
        else sb_put(&out, hit, fl);
        p = hit + fl;
    }
    free(src);
    return out.buf ? out.buf : strdup("");
}

#include <ctype.h>


/* ---------- switch -> if/else (GLSL ES 1.00 has no switch) ---------------
 * Godot's shader compiler emits `switch` for user shaders that use it, and no
 * Mali-4xx driver can parse it. Rewriting it needs care: C switch has
 * fall-through, `default` may sit anywhere, and `break` must leave the whole
 * statement. Both are recovered exactly here by wrapping the body in a
 * single-iteration for loop (which makes `break` legal and correct) and
 * carrying a "already matched" flag across the case bodies. */
static const char *skip_ws_c(const char *p)
{
    while (*p) {
        if (isspace((unsigned char)*p)) { p++; continue; }
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        if (p[0] == '/' && p[1] == '*') { p += 2; while (*p && !(p[0] == '*' && p[1] == '/')) p++; if (*p) p += 2; continue; }
        break;
    }
    return p;
}

/* returns pointer just past the matching '}' for the '{' at p, or NULL */
static const char *match_brace(const char *p)
{
    int depth = 0;
    for (; *p; p++) {
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (!depth) return p + 1; }
    }
    return NULL;
}

static char *expand_switch(char *src)
{
    static int uid;
    int changed = 0;
    Sb out = {0};
    const char *p = src;
    while (*p) {
        const char *hit = strstr(p, "switch");
        if (!hit) { sb_str(&out, p); break; }
        int lb = (hit == src) || (!isalnum((unsigned char)hit[-1]) && hit[-1] != '_');
        const char *after = skip_ws_c(hit + 6);
        if (!lb || *after != '(') {
            sb_put(&out, p, hit + 6 - p);
            p = hit + 6;
            continue;
        }
        /* parenthesised selector */
        const char *e = after + 1;
        int depth = 1;
        while (*e && depth) { if (*e == '(') depth++; else if (*e == ')') depth--; e++; }
        if (!depth) {
            const char *sel_begin = after + 1, *sel_end = e - 1;
            const char *body = skip_ws_c(e);
            const char *body_end = (*body == '{') ? match_brace(body) : NULL;
            if (body_end) {
                int id = uid++;
                char names[128];
                snprintf(names, sizeof(names), "%d", id);
                sb_put(&out, p, hit - p);
                /* collect the case constants for a correct `default` guard */
                Sb any = {0};
                const char *q = body + 1;
                int d = 1;
                int ncase = 0;
                while (q < body_end - 1) {
                    if (*q == '{') { d++; q++; continue; }
                    if (*q == '}') { d--; q++; continue; }
                    if (d == 1 && !strncmp(q, "case", 4) &&
                            (!isalnum((unsigned char)q[4]) && q[4] != '_')) {
                        const char *cb = skip_ws_c(q + 4);
                        const char *ce = strchr(cb, ':');
                        if (!ce) break;
                        if (ncase++) sb_str(&any, " || ");
                        sb_str(&any, "nx_sv");
                        sb_str(&any, names);
                        sb_str(&any, " == (");
                        sb_put(&any, cb, ce - cb);
                        sb_str(&any, ")");
                        q = ce + 1;
                        continue;
                    }
                    q++;
                }
                sb_str(&out, "for (int nx_si");
                sb_str(&out, names);
                sb_str(&out, " = 0; nx_si");
                sb_str(&out, names);
                sb_str(&out, " < 1; nx_si");
                sb_str(&out, names);
                sb_str(&out, "++) {\nint nx_sv");
                sb_str(&out, names);
                sb_str(&out, " = (");
                sb_put(&out, sel_begin, sel_end - sel_begin);
                sb_str(&out, ");\nbool nx_sm");
                sb_str(&out, names);
                sb_str(&out, " = false;\nbool nx_sa");
                sb_str(&out, names);
                sb_str(&out, " = ");
                sb_str(&out, ncase ? (any.buf ? any.buf : "false") : "false");
                sb_str(&out, ";\n");
                free(any.buf);
                /* emit each label body */
                q = body + 1;
                d = 1;
                const char *seg = NULL;
                int seg_is_default = 0;
                Sb guards = {0};
                while (q < body_end - 1) {
                    int is_case = (d == 1 && !strncmp(q, "case", 4) &&
                            !isalnum((unsigned char)q[4]) && q[4] != '_');
                    int is_def = (d == 1 && !strncmp(q, "default", 7) &&
                            !isalnum((unsigned char)q[7]) && q[7] != '_');
                    if (is_case || is_def) {
                        if (seg) {
                            sb_str(&out, "if (nx_sm");
                            sb_str(&out, names);
                            sb_str(&out, " || (");
                            sb_str(&out, guards.buf ? guards.buf : "false");
                            sb_str(&out, ")) { nx_sm");
                            sb_str(&out, names);
                            sb_str(&out, " = true;\n");
                            sb_put(&out, seg, q - seg);
                            sb_str(&out, "\n}\n");
                        }
                        free(guards.buf);
                        memset(&guards, 0, sizeof(guards));
                        const char *cb = skip_ws_c(q + (is_case ? 4 : 7));
                        const char *ce = strchr(cb, ':');
                        if (!ce) break;
                        if (is_case) {
                            sb_str(&guards, "nx_sv");
                            sb_str(&guards, names);
                            sb_str(&guards, " == (");
                            sb_put(&guards, cb, ce - cb);
                            sb_str(&guards, ")");
                        } else {
                            sb_str(&guards, "!nx_sa");
                            sb_str(&guards, names);
                        }
                        seg_is_default = is_def;
                        (void)seg_is_default;
                        seg = ce + 1;
                        q = ce + 1;
                        continue;
                    }
                    if (*q == '{') d++;
                    else if (*q == '}') d--;
                    q++;
                }
                if (seg) {
                    sb_str(&out, "if (nx_sm");
                    sb_str(&out, names);
                    sb_str(&out, " || (");
                    sb_str(&out, guards.buf ? guards.buf : "false");
                    sb_str(&out, ")) { nx_sm");
                    sb_str(&out, names);
                    sb_str(&out, " = true;\n");
                    sb_put(&out, seg, (body_end - 1) - seg);
                    sb_str(&out, "\n}\n");
                }
                free(guards.buf);
                sb_str(&out, "}\n");
                changed = 1;
                p = body_end;
                continue;
            }
        }
        sb_put(&out, p, hit + 6 - p);
        p = hit + 6;
    }
    char *res = out.buf ? out.buf : strdup("");
    free(src);
    if (changed) {
        /* nested switches inside a rewritten body need another pass */
        static int depth_guard;
        if (depth_guard < 8) {
            depth_guard++;
            res = expand_switch(res);
            depth_guard--;
        }
    }
    return res;
}

char *nx_translate_glsl(GLuint shader, const char *src0)
{
    if (!strstr(src0, "#version 3")) return NULL; /* already ES2 */

    REAL(void, glGetShaderiv, (GLuint, GLenum, GLint *));
    GLint stype = 0;
    p_glGetShaderiv(shader, GL_SHADER_TYPE, &stype);
    int is_vs = (stype == GL_VERTEX_SHADER);
    int force_canvas_unlit = !is_vs &&
        strstr(src0, "read_draw_data_lights") &&
        strstr(src0, "light_blend_compute") &&
        strstr(src0, "#ifndef DISABLE_LIGHTING");

    NxShaderRec *rec = shader_rec(shader, 1);
    if (!rec) {
        nx_log("GLSL shader registry exhausted for shader %u", shader);
        return NULL;
    }
    rec->nblocks = 0;
    rec->nattrs = 0;
    rec->ninst = 0;

    char *src = strdup(src0);

    Sb out = {0};
    sb_str(&out, "#version 100\n");
    if (!is_vs)
        sb_str(&out, "#extension GL_OES_standard_derivatives : enable\n"
                     "precision highp float;\nprecision highp int;\n");
    else
        sb_str(&out, "precision highp float;\nprecision highp int;\n");
    sb_str(&out, "vec4 nx_tex3(sampler2D nx_s, vec3 nx_uv) { return texture2D(nx_s, nx_uv.xy); }\n"
                 "vec4 nx_texLod(sampler2D nx_s, vec2 nx_uv, float nx_l) { return texture2D(nx_s, nx_uv); }\n"
                 "vec4 nx_texLod(sampler2D nx_s, vec3 nx_uv, float nx_l) { return texture2D(nx_s, nx_uv.xy); }\n"
                 "vec4 nx_texLod(samplerCube nx_s, vec3 nx_uv, float nx_l) { return textureCube(nx_s, nx_uv); }\n"
                 "mat3 nx_transpose(mat3 m) { return mat3(vec3(m[0][0], m[1][0], m[2][0]), vec3(m[0][1], m[1][1], m[2][1]), vec3(m[0][2], m[1][2], m[2][2])); }\n"
                 "mat4 nx_transpose(mat4 m) { return mat4(vec4(m[0][0], m[1][0], m[2][0], m[3][0]), vec4(m[0][1], m[1][1], m[2][1], m[3][1]), vec4(m[0][2], m[1][2], m[2][2], m[3][2]), vec4(m[0][3], m[1][3], m[2][3], m[3][3])); }\n"
                 "mat3 nx_inverse(mat3 m) { vec3 a=m[0], b=m[1], c=m[2]; return nx_transpose(mat3(cross(b,c), cross(c,a), cross(a,b))) / dot(a,cross(b,c)); }\n"
                 "bool nx_has_bit(int v, float b) { return mod(floor(float(v) / b), 2.0) > 0.5; }\n"
                 "float mix(float nx_a, float nx_b, bool nx_c) { return nx_c ? nx_b : nx_a; }\n"
                 "vec2 mix(vec2 nx_a, vec2 nx_b, bvec2 nx_c) { return vec2(nx_c.x ? nx_b.x : nx_a.x, nx_c.y ? nx_b.y : nx_a.y); }\n"
                 "vec3 mix(vec3 nx_a, vec3 nx_b, bvec3 nx_c) { return vec3(nx_c.x ? nx_b.x : nx_a.x, nx_c.y ? nx_b.y : nx_a.y, nx_c.z ? nx_b.z : nx_a.z); }\n"
                 "vec4 mix(vec4 nx_a, vec4 nx_b, bvec4 nx_c) { return vec4(nx_c.x ? nx_b.x : nx_a.x, nx_c.y ? nx_b.y : nx_a.y, nx_c.z ? nx_b.z : nx_a.z, nx_c.w ? nx_b.w : nx_a.w); }\n");
    if (force_canvas_unlit && !strstr(src0, "#define DISABLE_LIGHTING")) {
        static int logged_canvas_unlit;
        sb_str(&out, "#define DISABLE_LIGHTING\n");
        if (!logged_canvas_unlit) {
            nx_log("canvas GLES2: forcing DISABLE_LIGHTING");
            logged_canvas_unlit = 1;
        }
    }

    char fragout[64] = "";

    char *line = src;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        size_t ll = nl ? (size_t)(nl - line) : strlen(line);
        char lbuf[4096];
        if (ll >= sizeof(lbuf)) ll = sizeof(lbuf) - 1;
        memcpy(lbuf, line, ll);
        lbuf[ll] = 0;
        char *next = nl ? nl + 1 : NULL;

        char *t = lbuf;
        while (*t == ' ' || *t == '\t') t++;

        if (!strncmp(t, "#version", 8)) { line = next; continue; }

        /* Fragment-stage layout(...) qualifiers: strip and reprocess. The
         * vertex in-with-location case keeps its own branch below (it must
         * record the location for glBindAttribLocation). */
        if (!is_vs && (!strncmp(t, "layout(", 7) || !strncmp(t, "layout (", 8))) {
            char *close = strchr(t, ')');
            if (close) {
                t = close + 1;
                while (*t == ' ' || *t == '\t') t++;
            }
        }

        /* uniform block? */
        char *ub = strstr(t, "uniform ");
        int is_block = 0;
        char bname[64] = "";
        if (ub && strchr(ub, '{')) {
            /* "layout(std140) uniform Name { //ubo:N" */
            char *n = ub + 8;
            while (*n == ' ') n++;
            char *e = n;
            while (isalnum((unsigned char)*e) || *e == '_') e++;
            if (e > n && (size_t)(e - n) < sizeof(bname)) {
                memcpy(bname, n, e - n);
                bname[e - n] = 0;
                is_block = 1;
            }
        }
        if (is_block) {
            typedef struct {
                char type[64];
                char name[64];
                char array_expr[64];
                int primitive_type;
                int array_count;
            } BlockDecl;
            BlockDecl decls[96];
            int ndecls = 0;
            char instance[64] = "";
            int def_binding = -1;
            char *binding_marker = strstr(t, "ubo:");
            if (binding_marker) def_binding = atoi(binding_marker + 4);

            /* Parse members first, then emit them once the optional block
             * instance is known. Godot 4.7 places SceneDataBlock's instance
             * on the line after `}`, which the old parser left as invalid
             * standalone GLSL. */
            line = next;
            while (line && *line) {
                char *nl2 = strchr(line, '\n');
                size_t l2 = nl2 ? (size_t)(nl2 - line) : strlen(line);
                char mb[1024];
                if (l2 >= sizeof(mb)) l2 = sizeof(mb) - 1;
                memcpy(mb, line, l2);
                mb[l2] = 0;
                line = nl2 ? nl2 + 1 : NULL;
                char *m = mb;
                while (*m == ' ' || *m == '\t') m++;
                char *cb = strchr(m, '}');
                if (cb) {
                    const char *n2 = cb + 1;
                    parse_identifier(&n2, instance, sizeof(instance));
                    if (!instance[0] && line && *line) {
                        char *peek_nl = strchr(line, '\n');
                        size_t peek_len = peek_nl ? (size_t)(peek_nl - line) : strlen(line);
                        char peek[256];
                        if (peek_len < sizeof(peek)) {
                            memcpy(peek, line, peek_len);
                            peek[peek_len] = 0;
                            const char *pc = peek;
                            char candidate[64];
                            if (parse_identifier(&pc, candidate, sizeof(candidate))) {
                                while (*pc == ' ' || *pc == '\t') pc++;
                                if (*pc == ';') {
                                    pc++;
                                    while (*pc == ' ' || *pc == '\t') pc++;
                                    if (!*pc || !strncmp(pc, "//", 2)) {
                                        strcpy(instance, candidate);
                                        line = peek_nl ? peek_nl + 1 : NULL;
                                    }
                                }
                            }
                        }
                    }
                    break;
                }
                if (!*m || !strncmp(m, "//", 2)) continue;

                int tl2;
                int ty;
                for (;;) {
                    ty = parse_type(m, &tl2);
                    if (ty == -1) { m += tl2; while (*m == ' ') m++; continue; }
                    break;
                }

                char parsed_type[64];
                const char *member_cursor;
                if (ty >= 0) {
                    snprintf(parsed_type, sizeof(parsed_type), "%s", type_name(ty));
                    member_cursor = m + tl2;
                } else {
                    member_cursor = m;
                    if (!parse_identifier(&member_cursor, parsed_type, sizeof(parsed_type)))
                        continue;
                }
                char mname[64];
                if (!parse_identifier(&member_cursor, mname, sizeof(mname)))
                    continue;

                char array_expr[64] = "";
                while (*member_cursor == ' ' || *member_cursor == '\t') member_cursor++;
                if (*member_cursor == '[') {
                    const char *ae = strchr(member_cursor + 1, ']');
                    if (ae) {
                        size_t alen = (size_t)(ae - member_cursor - 1);
                        if (alen < sizeof(array_expr)) {
                            memcpy(array_expr, member_cursor + 1, alen);
                            array_expr[alen] = 0;
                        }
                    }
                }
                int arr = parse_array_count(array_expr, src0);
                if (ndecls < (int)(sizeof(decls) / sizeof(decls[0]))) {
                    BlockDecl *d = &decls[ndecls++];
                    memset(d, 0, sizeof(*d));
                    strcpy(d->type, parsed_type);
                    strcpy(d->name, mname);
                    strcpy(d->array_expr, array_expr);
                    d->primitive_type = ty;
                    d->array_count = arr;
                }
            }

            /* Godot deliberately reuses block names such as
             * MaterialUniforms for unrelated material shaders. Build the
             * complete std140 signature before consulting the global
             * registry, otherwise whichever material compiled first lends
             * its member names and offsets to every later material. */
            NxBlock candidate;
            memset(&candidate, 0, sizeof(candidate));
            strcpy(candidate.name, bname);
            Sb decl_signature = {0};
            for (int di = 0; di < ndecls; di++) {
                BlockDecl *d = &decls[di];
                char canonical_array[64] = "";
                if (d->array_expr[0]) {
                    if (d->array_count > 0) {
                        snprintf(canonical_array, sizeof(canonical_array),
                                 "%d", d->array_count);
                    } else {
                        size_t ai = 0;
                        for (const char *ap = d->array_expr;
                             *ap && ai + 1 < sizeof(canonical_array); ap++) {
                            if (!isspace((unsigned char)*ap))
                                canonical_array[ai++] = *ap;
                        }
                        canonical_array[ai] = 0;
                    }
                }
                char sig_item[320];
                snprintf(sig_item, sizeof(sig_item), "%zu:%s%zu:%s%zu:%s;",
                         strlen(d->type), d->type,
                         strlen(d->name), d->name,
                         strlen(canonical_array), canonical_array);
                sb_str(&decl_signature, sig_item);

                if (d->primitive_type < 0 || candidate.nmembers >= 96) continue;
                NxMember *mm = &candidate.m[candidate.nmembers++];
                strcpy(mm->name, d->name);
                mm->type = d->primitive_type;
                mm->array = d->array_count;
            }
            candidate.decl_signature = decl_signature.buf ?
                decl_signature.buf : strdup("");
            block_layout(&candidate);

            NxBlock *b = block_find_layout(&candidate);
            if (!b) {
                if (nblocks >= NX_MAX_BLOCKS) {
                    nx_log("too many GLSL uniform block layouts; cannot translate %s", bname);
                    free(candidate.decl_signature);
                    free(src);
                    free(out.buf);
                    return NULL;
                }
                blocks[nblocks] = candidate;
                b = &blocks[nblocks++];
            } else {
                free(candidate.decl_signature);
            }
            if (rec->nblocks < 16) {
                int block_slot = rec->nblocks++;
                rec->block_indices[block_slot] = (int)(b - blocks);
                rec->block_bindings[block_slot] = def_binding;
            }

            for (int di = 0; di < ndecls; di++) {
                BlockDecl *d = &decls[di];
                char uname[132];
                if (instance[0])
                    snprintf(uname, sizeof(uname), "%s_%s", instance, d->name);
                else
                    snprintf(uname, sizeof(uname), "%s", d->name);
                char decl[320];
                if (d->array_expr[0])
                    snprintf(decl, sizeof(decl), "uniform %s %s[%s];\n", d->type, uname, d->array_expr);
                else
                    snprintf(decl, sizeof(decl), "uniform %s %s;\n", d->type, uname);
                sb_str(&out, decl);
            }
            if (instance[0] && rec->ninst < 8) {
                snprintf(rec->inst_from[rec->ninst], 66, "%s.", instance);
                snprintf(rec->inst_to[rec->ninst], 66, "%s_", instance);
                rec->ninst++;
            }
            continue;
        }

        /* attribute/varying rewrites */
        if (is_vs && (!strncmp(t, "layout(", 7) || !strncmp(t, "layout (", 8))) {
            char *inp = strstr(t, " in ");
            char *loc = strstr(t, "location");
            if (inp && loc) {
                int locn = atoi(strchr(loc, '=') + 1);
                /* name = last identifier before ';' */
                char *sc = strchr(inp, ';');
                if (sc) {
                    char *e2 = sc;
                    while (e2 > inp && !isalnum((unsigned char)e2[-1]) && e2[-1] != '_') e2--;
                    char *s2 = e2;
                    while (s2 > inp && (isalnum((unsigned char)s2[-1]) || s2[-1] == '_')) s2--;
                    if (rec->nattrs < 24) {
                        rec->attrs[rec->nattrs].loc = locn;
                        size_t nl3 = e2 - s2;
                        if (nl3 < 64) {
                            memcpy(rec->attrs[rec->nattrs].name, s2, nl3);
                            rec->attrs[rec->nattrs].name[nl3] = 0;
                            rec->nattrs++;
                        }
                    }
                }
                char *close = strchr(t, ')');
                char decl[512];
                snprintf(decl, sizeof(decl), "attribute%s\n", close ? strstr(close, " in ") + 3 : "");
                sb_str(&out, decl);
                line = next;
                continue;
            }
        }
        /* ES 1.00 has no interpolation qualifiers. Canvas flag varyings are
         * constant across each generated quad, so dropping `flat` preserves
         * their value while allowing the declaration to compile. */
        char *io = t;
        for (;;) {
            static const char *quals[] = { "flat", "smooth", "noperspective", "centroid" };
            int skipped = 0;
            for (unsigned qi = 0; qi < sizeof(quals) / sizeof(quals[0]); qi++) {
                size_t ql = strlen(quals[qi]);
                if (!strncmp(io, quals[qi], ql) && (io[ql] == ' ' || io[ql] == '\t')) {
                    io += ql;
                    while (*io == ' ' || *io == '\t') io++;
                    skipped = 1;
                    break;
                }
            }
            if (!skipped) break;
        }
        if (is_vs && !strncmp(io, "in ", 3) && strchr(io, ';')) {
            sb_str(&out, "attribute ");
            sb_str(&out, io + 3);
            sb_str(&out, "\n");
            line = next;
            continue;
        }
        if (is_vs && !strncmp(io, "out ", 4) && strchr(io, ';')) {
            sb_str(&out, "varying ");
            sb_str(&out, io + 4);
            sb_str(&out, "\n");
            line = next;
            continue;
        }
        if (!is_vs && !strncmp(io, "in ", 3) && strchr(io, ';')) {
            sb_str(&out, "varying ");
            sb_str(&out, io + 3);
            sb_str(&out, "\n");
            line = next;
            continue;
        }
        if (!is_vs && !strncmp(io, "out ", 4) && strchr(io, ';')) {
            /* out vec4 name; -> gl_FragColor */
            char *sc = strchr(io, ';');
            if (sc) {
                char *e2 = sc;
                char *s2 = e2;
                while (s2 > io && (isalnum((unsigned char)s2[-1]) || s2[-1] == '_')) s2--;
                size_t nl3 = e2 - s2;
                if (nl3 && nl3 < sizeof(fragout)) {
                    memcpy(fragout, s2, nl3);
                    fragout[nl3] = 0;
                }
            }
            line = next;
            continue;
        }
        sb_str(&out, lbuf);
        sb_str(&out, "\n");
        line = next;
    }
    free(src);

    char *res = out.buf;
    if (fragout[0])
        res = replace_all(res, fragout, "gl_FragColor");
    /* Mali-4xx fragment shaders only honor mediump.  Newer strict GLES2
     * linkers do support highp and require shared declarations to retain the
     * same precision as the vertex stage. */
    if (!is_vs && !fragment_highp_supported())
        res = replace_all(res, "highp", "mediump");
    /* sampler3D behaves like the array case: alias to 2D, sample .xy */
    res = replace_all(res, "sampler3D", "sampler2DArray");
    /* sampler2DArray -> sampler2D + vec3-uv wrapper for sampled names */
    if (strstr(res, "sampler2DArray")) {
        char anames[8][64];
        int na = 0;
        char *q = res;
        while ((q = strstr(q, "sampler2DArray")) != NULL && na < 8) {
            char *n = q + 14;
            while (*n == ' ') n++;
            char *e = n;
            while (isalnum((unsigned char)*e) || *e == '_') e++;
            if (e > n && (size_t)(e - n) < 64) {
                memcpy(anames[na], n, e - n);
                anames[na][e - n] = 0;
                na++;
            }
            q = e;
        }
        res = replace_all(res, "sampler2DArray", "sampler2D");
        int used = 0;
        for (int a = 0; a < na; a++) {
            char from[96], to[96];
            snprintf(from, sizeof(from), "texture2D(%s,", anames[a]);
            snprintf(to, sizeof(to), "nx_tex3(%s,", anames[a]);
            if (strstr(res, from)) used = 1;
            res = replace_all(res, from, to);
            snprintf(from, sizeof(from), "texture2DLod(%s,", anames[a]);
            res = replace_all(res, from, to);
        }
        (void)used;
    }
    /* texture() family — textureLod first, then the plain rename */
    res = replace_all(res, "textureLod", "nx_texLod");
    res = replace_all(res, "texture", "texture2D"); /* too broad: fix below */
    res = replace_all(res, "texture2D2D", "texture2D");
    res = replace_all(res, "texture2DLod", is_vs ? "texture2DLod" : "texture2D");
    res = replace_all(res, "texture2DSize", "textureSize"); /* keep for log visibility */
    /* Canvas already carries the reciprocal texture size in its draw-data
     * uniform. Use that exact value instead of the ES3-only textureSize(). */
    res = replace_all(res, "vec2(textureSize(color_texture, 0))",
                      "(vec2(1.0) / read_draw_data_color_texture_pixel_size)");
    res = replace_all(res, "transpose", "nx_transpose");
    res = replace_all(res, "inverse", "nx_inverse");
    res = replace_all(res,
                      "bool(model_flags_input & int(FLAGS_NON_UNIFORM_SCALE))",
                      "nx_has_bit(model_flags_input, 16.0)");
    res = replace_all(res,
                      "bool(model_flags_input & uint(FLAGS_NON_UNIFORM_SCALE))",
                      "nx_has_bit(int(model_flags_input), 16.0)");
    res = replace_all(res,
                      "vec2(float((gl_VertexID >> 1)), float(((6 >> gl_VertexID) & 1)))",
                      "vec2(floor(nx_corner_attrib / 2.0), mod(floor(6.0 / exp2(nx_corner_attrib)), 2.0))");
    if (strstr(res, "nx_corner_attrib"))
        res = replace_all(res, "gl_VertexID", "int(nx_corner_attrib)");
    /* text_style.gdshader intentionally marks glyph_position flat and stores
     * the UV of the quad's origin there.  GLES2 has no flat interpolation, so
     * merely dropping the qualifier makes glyph_position interpolate back to
     * UV and collapses glyph_size to zero on conforming drivers.  Godot's
     * canvas source rectangle is constant for the complete glyph instance and
     * its xy value is exactly that origin. */
    if (is_vs && strstr(res, "m_glyph_position") &&
        strstr(res, "read_draw_data_src_rect")) {
        res = replace_all(res, "m_glyph_position=uv;",
                          "m_glyph_position=read_draw_data_src_rect.xy;");
        res = replace_all(res, "m_glyph_position = uv;",
                          "m_glyph_position = read_draw_data_src_rect.xy;");
    }
    /* samplerCube: texture2D(name, ...) -> textureCube(name, ...) */
    {
        char cnames[8][64];
        int nc = 0;
        char *q = res;
        while ((q = strstr(q, "samplerCube")) != NULL && nc < 8) {
            char *n = q + 11;
            while (*n == ' ' || *n == ';') n++;
            char *e = n;
            while (isalnum((unsigned char)*e) || *e == '_') e++;
            if (e > n && (size_t)(e - n) < 64 && *e != ')' && strncmp(n, "nx_", 3) != 0) {
                memcpy(cnames[nc], n, e - n);
                cnames[nc][e - n] = 0;
                nc++;
            }
            q = e;
        }
        for (int a = 0; a < nc; a++) {
            char from[96], to[96];
            snprintf(from, sizeof(from), "texture2D(%s,", cnames[a]);
            snprintf(to, sizeof(to), "textureCube(%s,", cnames[a]);
            res = replace_all(res, from, to);
        }
    }
    /* debug dump */
    if (getenv("NX_SHIM_DUMP")) {
        char path[128];
        snprintf(path, sizeof(path), "/tmp/nx_sh_%u_%s.glsl", shader, is_vs ? "vs" : "fs");
        FILE *df = fopen(path, "w");
        if (df) { fputs(res, df); fclose(df); }
    }
    {
        NxShaderRec *rr = shader_rec(shader, 0);
        if (rr) {
            for (int ii = 0; ii < rr->ninst; ii++) {
                /* plain textual: "name." -> "name_" (member access) */
                Sb o2 = {0};
                const char *from = rr->inst_from[ii];
                size_t fl = strlen(from);
                char *pp = res;
                while (*pp) {
                    char *hit = strstr(pp, from);
                    if (!hit) { sb_str(&o2, pp); break; }
                    sb_put(&o2, pp, hit - pp);
                    sb_str(&o2, rr->inst_to[ii]);
                    pp = hit + fl;
                }
                free(res);
                res = o2.buf ? o2.buf : strdup("");
            }
        }
    }
    /* ES3 types with no ES2 counterpart in the (unused) 3D path */
    res = replace_all(res, "mat3x4", "mat4");
    res = replace_all(res, "mat4x3", "mat4");
    res = replace_all(res, "mat2x4", "mat4");
    strip_f_suffix(res);
    /* uint family -> int (values must fit) */
    res = replace_all(res, "uint", "int");
    res = replace_all(res, "uvec2", "ivec2");
    res = replace_all(res, "uvec3", "ivec3");
    res = replace_all(res, "uvec4", "ivec4");
    strip_u_suffix(res);
    if (strstr(res, "switch")) {
        res = expand_switch(res);
    }
    {
        /* Reading the shader the driver actually got is the only way to check
         * the translation; the engine only ever prints it on a compile error. */
        const char *dir = getenv("NX_GLSL_DUMP");
        if (dir && dir[0]) {
            char path[512];
            snprintf(path, sizeof(path), "%s/shader_%u_%s.glsl", dir, shader, is_vs ? "vs" : "fs");
            FILE *f = fopen(path, "w");
            if (f) {
                fputs(res, f);
                fclose(f);
            }
        }
    }
    return res;
}

/* ---------- link-time: attrib locations + block resolve ---------- */
void nx_prelink(GLuint prog)
{
    REAL(void, glGetAttachedShaders, (GLuint, GLsizei, GLsizei *, GLuint *));
    REAL(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *));
    GLuint sh[4];
    GLsizei n = 0;
    p_glGetAttachedShaders(prog, 4, &n, sh);
    NxProgRec *pr = prog_rec(prog, 1);
    if (!pr) {
        nx_log("GLSL program registry exhausted for program %u", prog);
        return;
    }
    pr->nblocks = 0;
    for (GLsizei i = 0; i < n; i++) {
        NxShaderRec *sr = shader_rec(sh[i], 0);
        if (!sr) continue;
        for (int a = 0; a < sr->nattrs; a++)
            p_glBindAttribLocation(prog, sr->attrs[a].loc, sr->attrs[a].name);
        for (int b = 0; b < sr->nblocks; b++) {
            int block_index = sr->block_indices[b];
            if (block_index < 0 || block_index >= nblocks) continue;
            NxBlock *blk = &blocks[block_index];
            int existing = -1;
            for (int k = 0; k < pr->nblocks; k++)
                if (pr->b[k].blk == blk) { existing = k; break; }
            if (existing >= 0) {
                int incoming = sr->block_bindings[b];
                int *binding = &pr->b[existing].binding;
                if (*binding != -2 && incoming >= 0) {
                    if (*binding < 0) {
                        *binding = incoming;
                    } else if (*binding != incoming) {
                        nx_log("conflicting GLSL bindings for %s in program %u: %d vs %d",
                               blk->name, prog, *binding, incoming);
                        *binding = -2; /* do not flush an ambiguous UBO */
                    }
                }
            } else if (pr->nblocks < 16) {
                pr->b[pr->nblocks].blk = blk;
                pr->b[pr->nblocks].binding = sr->block_bindings[b];
                for (int u = 0; u < 96; u++) pr->b[pr->nblocks].locs[u] = -2;
                pr->nblocks++;
            }
        }
    }
    for (int b = 0; b < pr->nblocks; b++)
        if (pr->b[b].binding == -1) pr->b[b].binding = 0;
}

void nx_on_use_program(GLuint prog)
{
    cur_prog = prog ? prog_rec(prog, 0) : NULL;
}

/* ---------- ES3 UBO API ---------- */
GL_APICALL GLuint GL_APIENTRY glGetUniformBlockIndex(GLuint prog, const GLchar *name)
{
    NxProgRec *pr = prog_rec(prog, 0);
    if (pr)
        for (int i = 0; i < pr->nblocks; i++)
            if (!strcmp(pr->b[i].blk->name, name)) return i;
    return GL_INVALID_INDEX;
}

GL_APICALL void GL_APIENTRY glUniformBlockBinding(GLuint prog, GLuint idx, GLuint binding)
{
    NxProgRec *pr = prog_rec(prog, 0);
    if (pr && idx < (GLuint)pr->nblocks)
        pr->b[idx].binding = binding;
}

GL_APICALL void GL_APIENTRY glGetActiveUniformBlockiv(GLuint prog, GLuint idx, GLenum pname, GLint *v)
{
    NxProgRec *pr = prog_rec(prog, 0);
    if (!pr || idx >= (GLuint)pr->nblocks) { if (v) *v = 0; return; }
    switch (pname) {
    case GL_UNIFORM_BLOCK_DATA_SIZE: *v = pr->b[idx].blk->size; break;
    case GL_UNIFORM_BLOCK_BINDING: *v = pr->b[idx].binding; break;
    case GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS: *v = pr->b[idx].blk->nmembers; break;
    default: *v = 0;
    }
}

GL_APICALL void GL_APIENTRY glGetActiveUniformBlockName(GLuint prog, GLuint idx, GLsizei sz, GLsizei *len, GLchar *name)
{
    NxProgRec *pr = prog_rec(prog, 0);
    if (pr && idx < (GLuint)pr->nblocks) {
        snprintf(name, sz, "%s", pr->b[idx].blk->name);
        if (len) *len = strlen(name);
    }
}

GL_APICALL void GL_APIENTRY glGetActiveUniformsiv(GLuint p, GLsizei n, const GLuint *ix, GLenum pn, GLint *v)
{
    (void)p; (void)ix; (void)pn;
    for (GLsizei i = 0; i < n; i++) v[i] = 0;
}

GL_APICALL void GL_APIENTRY glGetUniformIndices(GLuint p, GLsizei n, const GLchar *const *names, GLuint *ix)
{
    (void)p; (void)names;
    for (GLsizei i = 0; i < n; i++) ix[i] = GL_INVALID_INDEX;
}

/* ---------- draw-time UBO flush ---------- */
extern const void *nx_ubo_data(GLuint binding, size_t *size);

/* One-shot dump of every uniform block this program consumes: the emulated
 * std140 layout is the single most likely place for a silent wrong-image bug,
 * because a mis-sized member shifts every matrix after it. */
static int nx_ubo_dump_budget = -1;
static int nx_ubo_dump;

void nx_flush_ubos(void)
{
    if (!cur_prog) return;
    if (nx_ubo_dump_budget < 0) {
        const char *value = getenv("NX_UBO_DUMP");
        nx_ubo_dump_budget = value && !strcmp(value, "1") ? 200 : 0;
    }
    nx_ubo_dump = nx_ubo_dump_budget > 0;
    if (nx_ubo_dump) nx_ubo_dump_budget--;
    REAL(GLint, glGetUniformLocation, (GLuint, const GLchar *));
    REAL(void, glUniform1fv, (GLint, GLsizei, const GLfloat *));
    REAL(void, glUniform2fv, (GLint, GLsizei, const GLfloat *));
    REAL(void, glUniform3fv, (GLint, GLsizei, const GLfloat *));
    REAL(void, glUniform4fv, (GLint, GLsizei, const GLfloat *));
    REAL(void, glUniform1iv, (GLint, GLsizei, const GLint *));
    REAL(void, glUniform2iv, (GLint, GLsizei, const GLint *));
    REAL(void, glUniform3iv, (GLint, GLsizei, const GLint *));
    REAL(void, glUniform4iv, (GLint, GLsizei, const GLint *));
    REAL(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *));
    REAL(void, glUniformMatrix3fv, (GLint, GLsizei, GLboolean, const GLfloat *));

    for (int i = 0; i < cur_prog->nblocks; i++) {
        NxBlock *blk = cur_prog->b[i].blk;
        if (cur_prog->b[i].binding < 0) continue;
        size_t bsz = 0;
        const char *data = nx_ubo_data(cur_prog->b[i].binding, &bsz);
        if (!data) continue;
        for (int m = 0; m < blk->nmembers; m++) {
            NxMember *mm = &blk->m[m];
            GLint *locp = &cur_prog->b[i].locs[m];
            if (*locp == -2)
                *locp = p_glGetUniformLocation(cur_prog->prog, mm->name);
            GLint loc = *locp;
            if (loc < 0) continue;
            const char *src = data + mm->offset;
            int cnt = mm->array ? mm->array : 1;
            if (nx_ubo_dump) {
                const GLfloat *f = (const GLfloat *)src;
                nx_log("ubo %s.%s type=%d array=%d off=%d loc=%d [%.4f %.4f %.4f %.4f]",
                       blk->name, mm->name, mm->type, mm->array, mm->offset, loc,
                       (mm->offset + 16 <= (int)bsz) ? f[0] : 0.0f,
                       (mm->offset + 16 <= (int)bsz) ? f[1] : 0.0f,
                       (mm->offset + 16 <= (int)bsz) ? f[2] : 0.0f,
                       (mm->offset + 16 <= (int)bsz) ? f[3] : 0.0f);
            }
            switch (mm->type) {
            case 0: p_glUniform1fv(loc, cnt, (const GLfloat *)src); break;
            case 1: p_glUniform2fv(loc, cnt, (const GLfloat *)src); break;
            case 2: p_glUniform3fv(loc, cnt, (const GLfloat *)src); break;
            case 3: p_glUniform4fv(loc, cnt, (const GLfloat *)src); break;
            case 4: case 10: p_glUniform1iv(loc, cnt, (const GLint *)src); break;
            case 5: p_glUniform2iv(loc, cnt, (const GLint *)src); break;
            case 6: p_glUniform3iv(loc, cnt, (const GLint *)src); break;
            case 7: p_glUniform4iv(loc, cnt, (const GLint *)src); break;
            case 8: p_glUniformMatrix4fv(loc, cnt, GL_FALSE, (const GLfloat *)src); break;
            case 9: {
                /* std140 mat3 = 3 vec4 columns */
                GLfloat tmp[9];
                const GLfloat *c = (const GLfloat *)src;
                for (int k = 0; k < 3; k++) {
                    tmp[k*3+0] = c[k*4+0];
                    tmp[k*3+1] = c[k*4+1];
                    tmp[k*3+2] = c[k*4+2];
                }
                p_glUniformMatrix3fv(loc, 1, GL_FALSE, tmp);
                break;
            }
            }
        }
    }
}
