/* GLSL ES 3.00 -> GLSL ES 1.00, para o Utgard aceitar os shaders do Sally Face.
 *
 * O jogo foi compilado so' com variantes GLES3 (ShaderCompilerPlatform 9): nos
 * 101 shaders nao existe nenhuma variante GLES2.  Rodando no Mali-450 a Unity
 * pedia a plataforma 5, nao achava nada e caia no error shader -- a tela rosa.
 *
 * tools/shader_gles2_patch.py corrige o TIPO nos bundles (a Unity passa a
 * entregar o shader); aqui traduzimos o CODIGO, que continua sendo ESSL 300.
 * A traducao mora em tempo de execucao de proposito: iterar num binario de 1 MB
 * e' barato, reempacotar 846 MB de asset pack nao e'.
 *
 * O que a saida do HLSLcc da Unity realmente usa (medido nos 101 shaders, nao
 * suposto):
 *
 *     precision highp float (frag)  99      texture(          92
 *     layout(location) out          98      textureLod(        9
 *     modf/round/trunc              11      uint/uvec          5
 *     flat/centroid                  2      MRT SV_Target1+    1
 *     switch                         1      inverse/transpose  1
 *
 * E, decisivo: ZERO uniform blocks de verdade.  Os 98 que citam std140 so'
 * carregam a macro UNITY_BINDING, que nunca e' usada -- todo uniform e' global,
 * entao ESSL 100 da conta sem reescrever a passagem de constantes.
 *
 * Cobrimos ~95 dos 101.  Os que sobram (uint/uvec, switch, inverse) sao efeitos
 * de post-processing; falham no compilador e o log do glCompileShader diz qual,
 * o que e' melhor que traduzir errado calado.
 */
#include "essl1.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct buf {
    char *p;
    size_t n, cap;
};

static int bgrow(struct buf *b, size_t need)
{
    if (b->n + need <= b->cap)
        return 1;
    size_t cap = b->cap ? b->cap * 2 : 8192;
    while (cap < b->n + need)
        cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p)
        return 0;
    b->p = p;
    b->cap = cap;
    return 1;
}

static void bput(struct buf *b, const char *s, size_t n)
{
    if (!bgrow(b, n + 1))
        return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

static void bputs(struct buf *b, const char *s)
{
    bput(b, s, strlen(s));
}

static int word_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/* casa `word` como identificador inteiro em s+i */
static int word_at(const char *s, size_t len, size_t i, const char *word)
{
    size_t n = strlen(word);
    if (i + n > len || memcmp(s + i, word, n) != 0)
        return 0;
    if (i > 0 && word_char(s[i - 1]))
        return 0;
    if (i + n < len && word_char(s[i + n]))
        return 0;
    return 1;
}

/* identificador seguido de '(' (ignorando espacos) */
static int call_at(const char *s, size_t len, size_t i, const char *word)
{
    if (!word_at(s, len, i, word))
        return 0;
    size_t j = i + strlen(word);
    while (j < len && (s[j] == ' ' || s[j] == '\t'))
        j++;
    return j < len && s[j] == '(';
}

static int has_word(const char *s, size_t len, const char *word)
{
    for (size_t i = 0; i < len; i++)
        if (word_at(s, len, i, word))
            return 1;
    return 0;
}

/* pula espaco/tab no inicio */
static size_t skip_ws(const char *s, size_t len, size_t i)
{
    while (i < len && (s[i] == ' ' || s[i] == '\t'))
        i++;
    return i;
}

static int line_starts_with(const char *l, size_t n, const char *kw)
{
    size_t i = skip_ws(l, n, 0);
    return word_at(l, n, i, kw);
}

struct unity_alias_rewrite {
    const char *name;
    const char *modern;
    const char *legacy;
};

/* Estes dois conjuntos sao strings do proprio libunity 2022.3. O renderer
 * escolhe o conjunto moderno quando monta a fonte; depois do retag GLES2, um
 * fallback raw-EGL pode chegar com #version 100 e ainda carregar os aliases
 * modernos. O pre-processador do Mali expande `ATTRIBUTE_IN` para `in`, por
 * exemplo, e o compilador rejeita o storage qualifier antes do link. */
static const struct unity_alias_rewrite unity_aliases[] = {
    { "ATTRIBUTE_IN", "in", "attribute" },
    { "VARYING_IN", "in", "varying" },
    { "VARYING_OUT", "out", "varying" },
    { "DECLARE_FRAG_COLOR", "out vec4 fragColor", NULL },
    { "FRAG_COLOR", "fragColor", "gl_FragColor" },
    { "SAMPLE_TEXTURE_2D", "texture", "texture2D" },
};

#define UNITY_ALIAS_COUNT (sizeof unity_aliases / sizeof *unity_aliases)
#define UNITY_ALIAS_FULL_MASK ((1u << UNITY_ALIAS_COUNT) - 1u)

static int payload_matches(const char *s, size_t len, const char *expected)
{
    size_t i = 0, e = 0;
    for (;;) {
        i = skip_ws(s, len, i);
        while (expected[e] == ' ' || expected[e] == '\t')
            e++;
        if (i == len || expected[e] == '\0')
            break;

        size_t ib = i;
        while (i < len && word_char(s[i]))
            i++;
        size_t eb = e;
        while (expected[e] && word_char(expected[e]))
            e++;
        if (i == ib || e == eb || i - ib != e - eb ||
            memcmp(s + ib, expected + eb, i - ib) != 0)
            return 0;
    }
    i = skip_ws(s, len, i);
    while (i < len && s[i] == '\r')
        i++;
    while (expected[e] == ' ' || expected[e] == '\t')
        e++;
    return i == len && expected[e] == '\0';
}

static const struct unity_alias_rewrite *unity_alias_at(const char *l,
                                                         size_t n)
{
    size_t p = skip_ws(l, n, 0);
    static const char define[] = "#define";
    if (p + sizeof define - 1 > n ||
        memcmp(l + p, define, sizeof define - 1) != 0)
        return NULL;
    p += sizeof define - 1;
    if (p >= n || (l[p] != ' ' && l[p] != '\t'))
        return NULL;
    p = skip_ws(l, n, p);

    for (size_t i = 0; i < sizeof unity_aliases / sizeof *unity_aliases; i++) {
        const struct unity_alias_rewrite *alias = &unity_aliases[i];
        if (!word_at(l, n, p, alias->name))
            continue;
        size_t value = skip_ws(l, n, p + strlen(alias->name));
        if (payload_matches(l + value, n - value, alias->modern))
            return alias;
    }
    return NULL;
}

static unsigned unity_modern_alias_mask(const char *s, size_t len,
                                        int *canonical_block)
{
    unsigned mask = 0;
    size_t expected = 0;
    int full_block = 0;
    size_t i = 0;
    while (i < len) {
        size_t eol = i;
        while (eol < len && s[eol] != '\n')
            eol++;
        const struct unity_alias_rewrite *alias =
            unity_alias_at(s + i, eol - i);
        if (alias) {
            size_t index = (size_t)(alias - unity_aliases);
            mask |= 1u << index;
            if (index == expected) {
                expected++;
                if (expected == UNITY_ALIAS_COUNT) {
                    full_block = 1;
                    expected = 0;
                }
            } else {
                expected = index == 0 ? 1 : 0;
            }
        } else {
            expected = 0;
        }
        i = eol < len ? eol + 1 : eol;
    }
    if (canonical_block)
        *canonical_block = full_block;
    return mask;
}

/* Alguns drivers Mali recusam o program binary e fazem a Unity recompilar a
 * fonte. Nesse fallback a Unity pode trocar apenas o cabecalho para ESSL 100,
 * deixando declaracoes top-level `in`/`out` do payload ESSL 300. Detectamos
 * somente declaracoes terminadas em `;`: qualificadores `in`/`out` legitimos
 * de parametros de funcao ESSL 100 nao ativam a traducao. */
static int has_modern_storage_declaration(const char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        size_t eol = i;
        while (eol < len && s[eol] != '\n')
            eol++;
        size_t n = eol - i;
        const char *l = s + i;
        size_t p = skip_ws(l, n, 0);

        if (word_at(l, n, p, "flat") || word_at(l, n, p, "centroid") ||
            word_at(l, n, p, "smooth")) {
            while (p < n && word_char(l[p]))
                p++;
            p = skip_ws(l, n, p);
        }
        if (word_at(l, n, p, "layout")) {
            const char *close = memchr(l + p, ')', n - p);
            if (close)
                p = skip_ws(l, n, (size_t)(close - l) + 1);
        }
        if ((word_at(l, n, p, "in") || word_at(l, n, p, "out")) &&
            memchr(l + p, ';', n - p))
            return 1;
        i = eol < len ? eol + 1 : eol;
    }
    return 0;
}

unsigned sf_essl1_classify(const char *src, size_t len)
{
    unsigned reasons = 0;
    for (size_t i = 0; i + 8 < len; i++)
        if (memcmp(src + i, "#version", 8) == 0) {
            size_t j = skip_ws(src, len, i + 8);
            if (j + 3 <= len && memcmp(src + j, "300", 3) == 0)
                reasons |= SF_ESSL1_REASON_VERSION_300;
            break;
        }
    if (has_modern_storage_declaration(src, len))
        reasons |= SF_ESSL1_REASON_STORAGE;
    int canonical_alias_block = 0;
    unsigned alias_mask = unity_modern_alias_mask(
        src, len, &canonical_alias_block);
    if (canonical_alias_block && alias_mask == UNITY_ALIAS_FULL_MASK)
        reasons |= SF_ESSL1_REASON_UNITY_ALIASES;
    else if (alias_mask)
        reasons |= SF_ESSL1_REASON_UNITY_ALIAS_PARTIAL;
    return reasons;
}

#define MAX_TARGETS 8

struct outvar {
    char name[64];
    int index;
};

/* Acha as saidas do fragment: `layout(location = N) out <tipo> NOME;` */
static int collect_outputs(const char *s, size_t len, struct outvar *out)
{
    int count = 0;
    size_t i = 0;
    while (i < len && count < MAX_TARGETS) {
        size_t eol = i;
        while (eol < len && s[eol] != '\n')
            eol++;
        size_t n = eol - i;
        const char *l = s + i;

        size_t p = skip_ws(l, n, 0);
        int loc = -1;
        if (word_at(l, n, p, "layout")) {
            const char *eq = memchr(l + p, '=', n - p);
            if (eq)
                loc = atoi(eq + 1);
            const char *close = memchr(l + p, ')', n - p);
            if (close)
                p = (size_t)(close - l) + 1;
            p = skip_ws(l, n, p);
        }
        if (word_at(l, n, p, "out")) {
            /* ultimo identificador antes do ';' e' o nome */
            size_t semi = n;
            for (size_t k = p; k < n; k++)
                if (l[k] == ';') {
                    semi = k;
                    break;
                }
            if (semi < n) {
                size_t e = semi;
                while (e > p && !word_char(l[e - 1]))
                    e--;
                size_t b = e;
                while (b > p && word_char(l[b - 1]))
                    b--;
                if (e > b && e - b < sizeof out[0].name) {
                    memcpy(out[count].name, l + b, e - b);
                    out[count].name[e - b] = '\0';
                    out[count].index = loc < 0 ? count : loc;
                    count++;
                }
            }
        }
        i = eol + 1;
    }
    return count;
}

/* Emite uma linha trocando tokens do corpo. */
static void emit_body(struct buf *o, const char *l, size_t n, int is_fragment,
                      const struct outvar *outs, int nouts, int single_target)
{
    /* uniform e' compartilhado entre os estagios: a precisao TEM que bater */
    int uniform_line = 0;
    for (size_t i = 0; i < n; i++)
        if (word_at(l, n, i, "uniform")) {
            uniform_line = 1;
            break;
        }
    /* MATRIZ nao entra no rebaixamento.  A Unity empacota as matrizes como
     * vec4[] chamados hlslcc_mtx*, e elas so' vivem no VERTEX, onde highp
     * existe no Utgard.  Rebaixar a MatrixVP para mediump (10 bits de
     * mantissa) colapsa a posicao de mundo e a tela fica PRETA -- foi o que
     * aconteceu em 09/08/2026 assim que o link passou a fechar. */
    int matrix_line = 0;
    for (size_t i = 0; i < n; i++)
        if (word_at(l, n, i, "mat2") || word_at(l, n, i, "mat3") ||
            word_at(l, n, i, "mat4")) {
            matrix_line = 1;
            break;
        }
    if (!matrix_line && memmem(l, n, "hlslcc_mtx", strlen("hlslcc_mtx")))
        matrix_line = 1;

    int downgrade_hp = (is_fragment || uniform_line) && !(matrix_line && !is_fragment);
    for (size_t i = 0; i < n;) {
        /* Uniform SEM qualificador explicito herda o default do estagio:
         * highp no vertex, mediump no fragment -> o link morre com
         * "Uniform '_ClipRect' differ on precision".  Qualificar nos DOIS
         * lados com a mesma macro faz eles baterem. */
        if (word_at(l, n, i, "uniform")) {
            size_t j = i + strlen("uniform");
            size_t k = skip_ws(l, n, j);
            int has_qualifier = word_at(l, n, k, "lowp") ||
                                word_at(l, n, k, "mediump") ||
                                word_at(l, n, k, "highp") ||
                                word_at(l, n, k, "SF_HP");
            bputs(o, "uniform ");
            if (!has_qualifier && !(matrix_line && !is_fragment))
                bputs(o, "SF_HP ");
            i = k;
            continue;
        }
        if (call_at(l, n, i, "texture")) {
            bputs(o, "texture2D");
            i += strlen("texture");
            continue;
        }
        if (call_at(l, n, i, "textureLod")) {
            bputs(o, is_fragment ? "texture2DLodEXT" : "texture2DLod");
            i += strlen("textureLod");
            continue;
        }
        if (call_at(l, n, i, "textureProj")) {
            bputs(o, "texture2DProj");
            i += strlen("textureProj");
            continue;
        }
        if (downgrade_hp && word_at(l, n, i, "highp")) {
            bputs(o, "SF_HP");
            i += strlen("highp");
            continue;
        }
        int matched = 0;
        for (int k = 0; k < nouts; k++) {
            if (word_at(l, n, i, outs[k].name)) {
                if (single_target) {
                    bputs(o, "gl_FragColor");
                } else {
                    char tmp[32];
                    snprintf(tmp, sizeof tmp, "gl_FragData[%d]", outs[k].index);
                    bputs(o, tmp);
                }
                i += strlen(outs[k].name);
                matched = 1;
                break;
            }
        }
        if (matched)
            continue;
        bput(o, l + i, 1);
        i++;
    }
    bputs(o, "\n");
}

char *sf_essl1_translate(const char *src, size_t len, int is_fragment,
                         size_t *out_len)
{
    /* Caminho normal: payload ESSL 300. Caminho de fallback medido no
     * Mali-G31/raw-EGL: cabecalho 100 (ou ausente), mas declaracoes top-level
     * modernas ainda presentes. Shaders ESSL 100 genuinos ficam intactos. */
    unsigned reasons = sf_essl1_classify(src, len);
    if (!reasons)
        return NULL;
    if (reasons & SF_ESSL1_REASON_UNITY_ALIAS_PARTIAL)
        return NULL;

    struct outvar outs[MAX_TARGETS];
    int nouts = is_fragment ? collect_outputs(src, len, outs) : 0;
    int single_target = nouts <= 1;

    int need_lod = has_word(src, len, "textureLod");
    int need_deriv = has_word(src, len, "dFdx") || has_word(src, len, "dFdy") ||
                     has_word(src, len, "fwidth");
    int need_trunc = has_word(src, len, "trunc");
    int need_round = has_word(src, len, "roundEven");

    struct buf o = {0};
    bputs(&o, "#version 100\n");
    if (need_lod)
        bputs(&o, "#extension GL_EXT_shader_texture_lod : enable\n");
    if (need_deriv)
        bputs(&o, "#extension GL_OES_standard_derivatives : enable\n");
    /* SF_HP entra nos DOIS estagios com a MESMA expansao.  Uniform precisa
     * casar de precisao entre vertex e fragment, senao o link morre com
     * "Uniform '_ClipRect' differ on precision" (TextMeshPro, 09/08/2026).
     * No vertex a macro so' e' aplicada em linha de UNIFORM -- posicao continua
     * em highp, que o vertex do Utgard suporta. */
    bputs(&o,
          "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
          "#define SF_HP highp\n"
          "#else\n"
          "#define SF_HP mediump\n"
          "#endif\n");
    if (is_fragment) {
        /* No fragment nao ha' precisao default para float: qualquer funcao
         * declarada antes do `precision` do proprio shader (os polyfills abaixo)
         * morre com "no default precision defined for return value".  Declarar
         * aqui e' seguro -- o shader redeclara logo em seguida e a ultima vale. */
        bputs(&o, "precision mediump float;\nprecision mediump int;\n");
    }
    if (need_trunc)
        bputs(&o,
              "float trunc(float x){return x<0.0?-floor(-x):floor(x);}\n"
              "vec2 trunc(vec2 x){return vec2(trunc(x.x),trunc(x.y));}\n"
              "vec3 trunc(vec3 x){return vec3(trunc(x.x),trunc(x.y),trunc(x.z));}\n"
              "vec4 trunc(vec4 x){return vec4(trunc(x.x),trunc(x.y),trunc(x.z),trunc(x.w));}\n");
    if (need_round)
        bputs(&o,
              "float roundEven(float x){return floor(x+0.5);}\n"
              "vec2 roundEven(vec2 x){return floor(x+0.5);}\n"
              "vec3 roundEven(vec3 x){return floor(x+0.5);}\n"
              "vec4 roundEven(vec4 x){return floor(x+0.5);}\n");

    size_t i = 0;
    while (i < len) {
        size_t eol = i;
        while (eol < len && src[eol] != '\n')
            eol++;
        size_t n = eol - i;
        const char *l = src + i;
        i = eol + 1;

        /* carriage return solto */
        while (n > 0 && (l[n - 1] == '\r'))
            n--;

        size_t p = skip_ws(l, n, 0);

        /* #version ja' foi emitido */
        if (n > p + 8 && memcmp(l + p, "#version", 8) == 0)
            continue;

        const struct unity_alias_rewrite *alias =
            (reasons & SF_ESSL1_REASON_UNITY_ALIASES)
                ? unity_alias_at(l, n) : NULL;
        if (alias) {
            if (!alias->legacy) {
                /* O template real guarda a declaracao num
                 * `#ifdef DECLARE_FRAG_COLOR`. Uma macro vazia ainda ativa a
                 * guarda e deixa `;` global, invalido no perfil ESSL100. */
                bputs(&o, "#undef ");
                bputs(&o, alias->name);
            } else {
                bputs(&o, "#define ");
                bputs(&o, alias->name);
                bputs(&o, " ");
                bputs(&o, alias->legacy);
            }
            bputs(&o, "\n");
            continue;
        }

        /* as duas chaves que a propria Unity oferece para degradar */
        if (word_at(l, n, p, "#define") || (n > p && l[p] == '#')) {
            if (memmem(l, n, "HLSLCC_ENABLE_UNIFORM_BUFFERS 1",
                       strlen("HLSLCC_ENABLE_UNIFORM_BUFFERS 1"))) {
                bputs(&o, "#define HLSLCC_ENABLE_UNIFORM_BUFFERS 0\n");
                continue;
            }
            if (memmem(l, n, "UNITY_SUPPORTS_UNIFORM_LOCATION 1",
                       strlen("UNITY_SUPPORTS_UNIFORM_LOCATION 1"))) {
                bputs(&o, "#define UNITY_SUPPORTS_UNIFORM_LOCATION 0\n");
                continue;
            }
            /* mesma razao do SF_HP nos uniforms: se o jogo declarar via macro,
             * a precisao tem que vir junto, senao vertex e fragment divergem */
            if (memmem(l, n, "#define UNITY_UNIFORM uniform",
                       strlen("#define UNITY_UNIFORM uniform"))) {
                bputs(&o, "#define UNITY_UNIFORM uniform SF_HP\n");
                continue;
            }
            bput(&o, l, n);
            bputs(&o, "\n");
            continue;
        }

        /* qualificadores de interpolacao nao existem em ESSL 100 */
        if (word_at(l, n, p, "flat") || word_at(l, n, p, "centroid") ||
            word_at(l, n, p, "smooth")) {
            size_t q = p;
            while (q < n && word_char(l[q]))
                q++;
            p = skip_ws(l, n, q);
            /* segue a classificacao abaixo com p ja' avancado */
        }

        /* declaracao de saida do fragment: some, vira gl_FragColor/gl_FragData */
        if (is_fragment) {
            size_t q = p;
            if (word_at(l, n, q, "layout")) {
                const char *close = memchr(l + q, ')', n - q);
                if (close)
                    q = (size_t)(close - l) + 1;
                q = skip_ws(l, n, q);
            }
            if (word_at(l, n, q, "out"))
                continue;
        }

        /* in/out viram attribute/varying */
        if (line_starts_with(l + p, n - p, "in")) {
            bputs(&o, is_fragment ? "varying " : "attribute ");
            size_t q = skip_ws(l, n, p) + 2;
            q = skip_ws(l, n, q);
            emit_body(&o, l + q, n - q, is_fragment, outs, nouts, single_target);
            continue;
        }
        if (!is_fragment && line_starts_with(l + p, n - p, "out")) {
            bputs(&o, "varying ");
            size_t q = skip_ws(l, n, p) + 3;
            q = skip_ws(l, n, q);
            emit_body(&o, l + q, n - q, is_fragment, outs, nouts, single_target);
            continue;
        }

        emit_body(&o, l, n, is_fragment, outs, nouts, single_target);
    }

    if (!o.p)
        return NULL;
    /* Nunca devolva uma traducao parcial. Em especial, este gate impede que
     * aliases modernos voltem a chegar ao pre-processador do Mali sem recibo. */
    if (sf_essl1_classify(o.p, o.n) != 0) {
        free(o.p);
        return NULL;
    }
    if (out_len)
        *out_len = o.n;
    return o.p;
}

/* Escotilha de emergencia: SF_NO_ESSL1=1 entrega o shader cru ao driver, para
 * separar "a traducao quebrou" de "o shader ja' vinha quebrado". */
int sf_essl1_disabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("SF_NO_ESSL1");
        cached = v && *v && *v != '0';
    }
    return cached;
}
