/*
 * nx_jni.c — implementação da camada de dispatch por tabela (ver nx_jni.h).
 * Clean-room NextOS. C99 + libc, portável ARM64/x86_64.
 */
#define _GNU_SOURCE   /* strdup */
#include "nx_jni.h"

/* -------- pool de strings "JNI" -----------------------------------------
 * Uma "string JNI" nossa é só um objeto com um marcador + o ponteiro pro
 * UTF-8. O jogo trata como jstring opaca; GetStringUTFChars devolve ->utf8.
 * Alocação simples e nunca liberada (strings de config vivem o boot todo). */
typedef struct { uint32_t tag; const char *utf8; } nx_string;
#define NX_STR_TAG 0x4E58535Bu /* "NXS[" */

void *nx_new_string(nx_ctx *c, const char *utf8) {
    (void)c;
    nx_string *s = (nx_string *)malloc(sizeof *s);
    if (!s) return NULL;
    s->tag = NX_STR_TAG;
    s->utf8 = utf8 ? strdup(utf8) : NULL;
    return s;
}
const char *nx_cstr(void *jstring) {
    nx_string *s = (nx_string *)jstring;
    if (s && s->tag == NX_STR_TAG) return s->utf8;
    return NULL;
}

/* -------- IDs estáveis ---------------------------------------------------
 * Um ID é um índice numa tabela global de entradas resolvidas, +1, embrulhado
 * como ponteiro. Entradas repetidas reusam o mesmo ID. Cabe a jmethodID/jfieldID.
 */
typedef struct { const char *name; const char *sig; const void *entry; int is_field; } nx_id_rec;
static nx_id_rec g_ids[4096];
static int       g_id_n = 0;

static void *intern(const char *name, const char *sig, const void *entry, int is_field) {
    for (int i = 0; i < g_id_n; i++)
        if (g_ids[i].is_field == is_field && g_ids[i].name && name &&
            strcmp(g_ids[i].name, name) == 0)
            return (void *)(intptr_t)(i + 1);
    if (g_id_n >= (int)(sizeof g_ids / sizeof g_ids[0])) return (void *)(intptr_t)0;
    int i = g_id_n++;
    g_ids[i].name = name ? strdup(name) : NULL;
    g_ids[i].sig  = sig ? strdup(sig) : NULL;
    g_ids[i].entry = entry;
    g_ids[i].is_field = is_field;
    return (void *)(intptr_t)(i + 1);
}
static nx_id_rec *rec_of(void *id) {
    int i = (int)(intptr_t)id - 1;
    if (i < 0 || i >= g_id_n) return NULL;
    return &g_ids[i];
}

const char *nx_id_name(void *id) {
    nx_id_rec *r = rec_of(id);
    return (r && r->name) ? r->name : "?";
}

static int verbose(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("NX_JNI_VERBOSE"); v = (e && *e && *e != '0'); }
    return v;
}

void *nx_method_id(const nx_jni_config *cfg, const char *name, const char *sig) {
    const void *entry = NULL;
    if (cfg && cfg->methods)
        for (const nx_method *m = cfg->methods; m->name; m++)
            if (strcmp(m->name, name) == 0 && (!sig || !m->sig || strcmp(m->sig, sig) == 0)) {
                entry = m; break;
            }
    if (!entry && verbose())
        fprintf(stderr, "[nx_jni] GetMethodID nao-resolvido: %s %s\n", name, sig ? sig : "");
    return intern(name, sig, entry, 0);
}

void *nx_field_id(const nx_jni_config *cfg, const char *name, const char *sig) {
    const void *entry = NULL;
    if (cfg && cfg->fields)
        for (const nx_field *f = cfg->fields; f->name; f++)
            if (strcmp(f->name, name) == 0 && (!sig || !f->sig || strcmp(f->sig, sig) == 0)) {
                entry = f; break;
            }
    if (!entry && verbose())
        fprintf(stderr, "[nx_jni] GetFieldID nao-resolvido: %s %s\n", name, sig ? sig : "");
    return intern(name, sig, entry, 1);
}

nx_jval nx_dispatch(const nx_jni_config *cfg, void *env, void *obj,
                    void *method_id, va_list *ap, const void *jargs) {
    nx_id_rec *r = rec_of(method_id);
    const nx_method *m = (r && !r->is_field) ? (const nx_method *)r->entry : NULL;
    if (verbose())
        fprintf(stderr, "[nx_jni] Call %s%s\n", r ? nx_id_name(method_id) : "?",
                m ? "" : " (default)");
    if (m && m->fn) {
        nx_ctx c; c.env = env; c.obj = obj; c.ap = ap; c.jargs = jargs; c.cfg = cfg;
        return m->fn(&c);
    }
    /* default seguro: 0/NULL — o boot segue mesmo sem esse método previsto */
    return nx_none();
}

nx_jval nx_dispatch_field(const nx_jni_config *cfg, void *env, void *obj,
                          void *field_id) {
    nx_id_rec *r = rec_of(field_id);
    const nx_field *f = (r && r->is_field) ? (const nx_field *)r->entry : NULL;
    if (verbose())
        fprintf(stderr, "[nx_jni] GetField %s%s\n", r ? nx_id_name(field_id) : "?",
                f ? "" : " (default)");
    if (f && f->fn) {
        nx_ctx c; c.env = env; c.obj = obj; c.ap = NULL; c.jargs = NULL; c.cfg = cfg;
        return f->fn(&c);
    }
    return nx_none();
}
