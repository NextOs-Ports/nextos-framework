/*
 * nx_jni.h — Fake JNI por TABELA, estilo NextOS (clean-room).
 * ---------------------------------------------------------------------------
 * Substitui o padrão "jni_shim.c escrito à mão com switch gigante por port"
 * por um dispatch dirigido por DADOS: cada port declara UMA tabela de métodos
 * e campos; o core faz o resto (GetMethodID, GetFieldID, Call*, Get*Field, New*).
 *
 * Portável: só C99 + libc. Roda igual em Linux ARM64 (Mali) e x86_64 (PC).
 * Zero dependência de plataforma — não é o FalsoJNI (Vita): é a nossa versão,
 * pensada pro so-loader multiarch do NextOS.
 *
 * USO (resumo — veja receitas/14-jni-por-tabela.md):
 *
 *   static nx_jval m_getPackage(nx_ctx *c) { return nx_str(c, "com.jogo"); }
 *   static const nx_method METHODS[] = {
 *       { "getPackageName", "()Ljava/lang/String;", NX_OBJ, m_getPackage },
 *       NX_METHOD_END
 *   };
 *   static const nx_field FIELDS[] = { NX_FIELD_END };
 *
 *   void *vm, *env;
 *   nx_jni_config cfg = { "com.jogo", 110, METHODS, FIELDS };
 *   nx_jni_init(&cfg, &vm, &env);   // entrega JavaVM* e JNIEnv* prontos
 *
 * Handlers recebem um nx_ctx* (obj + args + config) e devolvem nx_jval.
 * Nomes não-registrados caem num default seguro (0/NULL/objeto-fantasma) e são
 * logados, então o boot não morre por causa de uma chamada JNI não prevista.
 */
#ifndef NX_JNI_H
#define NX_JNI_H

#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* tipo de retorno declarado na tabela (dirige qual slot do JNIEnv despacha) */
typedef enum {
    NX_VOID = 0, NX_OBJ, NX_BOOL, NX_BYTE, NX_CHAR, NX_SHORT,
    NX_INT, NX_LONG, NX_FLOAT, NX_DOUBLE
} nx_type;

/* valor de retorno de um handler — união crua, o core converte pro slot certo */
typedef union {
    void       *l;   /* objeto/string */
    int64_t     j;   /* long */
    int32_t     i;   /* int/bool/byte/char/short */
    float       f;
    double      d;
} nx_jval;

struct nx_jni_config;

/* contexto entregue a cada handler */
typedef struct {
    void  *env;                    /* JNIEnv* (raw) */
    void  *obj;                    /* this (instância) ou classe (static) */
    va_list *ap;                   /* argumentos da chamada (pode ser NULL) */
    const void *jargs;             /* variante A: jvalue* (pode ser NULL) */
    const struct nx_jni_config *cfg;
} nx_ctx;

typedef nx_jval (*nx_handler)(nx_ctx *c);

typedef struct { const char *name; const char *sig; nx_type ret; nx_handler fn; } nx_method;
typedef struct { const char *name; const char *sig; nx_type type; nx_handler fn; } nx_field;

#define NX_METHOD_END { NULL, NULL, NX_VOID, NULL }
#define NX_FIELD_END  { NULL, NULL, NX_VOID, NULL }

typedef struct nx_jni_config {
    const char      *package_name;   /* ex.: "jp.konami.castlevania" */
    int              obb_version;     /* ex.: 110 */
    const nx_method *methods;         /* tabela terminada por NX_METHOD_END */
    const nx_field  *fields;          /* tabela terminada por NX_FIELD_END  */
} nx_jni_config;

/* helpers pros handlers montarem retornos */
static inline nx_jval nx_none(void)         { nx_jval v; v.j = 0; return v; }
static inline nx_jval nx_int(int32_t x)     { nx_jval v; v.i = x; return v; }
static inline nx_jval nx_long(int64_t x)    { nx_jval v; v.j = x; return v; }
static inline nx_jval nx_bool(int x)        { nx_jval v; v.i = !!x; return v; }
static inline nx_jval nx_ptr(void *p)       { nx_jval v; v.l = p; return v; }
/* nx_str: cria uma "string JNI" gerenciada pelo core (ver nx_jni.c) */
void *nx_new_string(nx_ctx *c, const char *utf8);
static inline nx_jval nx_str(nx_ctx *c, const char *s) { nx_jval v; v.l = nx_new_string(c, s); return v; }

/* argumentos: o handler lê na ordem declarada na assinatura */
static inline void   *nx_arg_obj(nx_ctx *c)  { return c->ap ? va_arg(*c->ap, void*)   : NULL; }
static inline int32_t nx_arg_int(nx_ctx *c)  { return c->ap ? va_arg(*c->ap, int32_t) : 0; }
static inline int64_t nx_arg_long(nx_ctx *c) { return c->ap ? va_arg(*c->ap, int64_t) : 0; }
static inline double  nx_arg_dbl(nx_ctx *c)  { return c->ap ? va_arg(*c->ap, double)  : 0; }

/* converte uma "string JNI" (nossa) de volta pra const char* (ou NULL) */
const char *nx_cstr(void *jstring);

/* ---------------------------------------------------------------------------
 * CAMADA DE DISPATCH (o coração)
 * ---------------------------------------------------------------------------
 * Em vez de reimplementar a vtable inteira do JNIEnv (230+ slots — frágil),
 * o nx_jni é a camada que a vtable JÁ-TESTADA do seu port chama. No seu
 * GetMethodID você devolve nx_method_id(...); no seu CallObjectMethodV você
 * chama nx_dispatch(...). Assim cada port novo só preenche a TABELA.
 *
 * nx_method_id / nx_field_id: resolvem nome+assinatura -> um ID estável
 * (opaco, reutilizável como jmethodID/jfieldID). Nome desconhecido ainda
 * recebe um ID válido (dispatch cai no default seguro), então o jogo não
 * quebra por causa de um método não previsto.
 */
void *nx_method_id(const nx_jni_config *cfg, const char *name, const char *sig);
void *nx_field_id (const nx_jni_config *cfg, const char *name, const char *sig);

/* nome registrado de um ID (pra logs/diagnóstico); "?" se desconhecido */
const char *nx_id_name(void *id);

/* despacha uma chamada de método. Passe ap (variante V/va_list) OU jargs
 * (variante A/jvalue*) — o outro NULL. Retorna nx_jval (o core sabe o tipo). */
nx_jval nx_dispatch(const nx_jni_config *cfg, void *env, void *obj,
                    void *method_id, va_list *ap, const void *jargs);

/* despacha leitura de campo (Get<T>Field / GetStatic<T>Field). */
nx_jval nx_dispatch_field(const nx_jni_config *cfg, void *env, void *obj,
                          void *field_id);

/*
 * Log: por padrão só nomes não-resolvidos. Ligue NX_JNI_VERBOSE=1 no ambiente
 * pra ver TODA chamada (útil no bring-up de um port novo).
 */

#ifdef __cplusplus
}
#endif
#endif /* NX_JNI_H */
