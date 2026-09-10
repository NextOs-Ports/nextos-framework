// imports — os 165 simbolos que o libff4.so importa.
//
// Tres grupos:
//   1. libc/libm/pthread padrao -> passa direto pro glibc
//   2. bionic-only (__errno, __*_chk, __cxa_*, __stack_chk_guard) -> shim aqui
//   3. GLES1 / OpenSLES / AAsset / log -> nossas implementacoes
//
// ⚠️ MISMATCH bionic x glibc: o libff4 so usa pthread_mutex/cond/create/join,
// cujos tipos opacos sao MAIORES no glibc que no bionic. Como TODAS as structs
// de pthread aqui sao alocadas pelo proprio libff4 (em bss/heap dele) com o
// tamanho do bionic, passar direto pro glibc estouraria a area. Por isso
// mutex/cond sao encapsulados: guardamos um ponteiro nosso dentro do handle.
#define _GNU_SOURCE  // sincos
#include "so_util.h"
#include "game.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <GLES/gl.h>
#include <GLES/glext.h>

#include "nxgl_gles1.h"

// ---------------------------------------------------------------------------
// Grupo 2: bionic-only
// ---------------------------------------------------------------------------
static int *bionic_errno(void) { return &errno; }

static void bionic_stack_chk_fail(void) {
    fprintf(stderr, "[ff4] __stack_chk_fail — corrupcao de pilha no libff4\n");
    abort();
}
// Simbolo de DADO: a GOT recebe o endereco desta variavel.
static uintptr_t bionic_stack_chk_guard = 0x000a0dff2f6b4b00ULL;

static void *bionic_memcpy_chk(void *d, const void *s, size_t n, size_t dlen) {
    if (n > dlen) bionic_stack_chk_fail();
    return memcpy(d, s, n);
}
static char *bionic_strcpy_chk(char *d, const char *s, size_t dlen) {
    if (strlen(s) + 1 > dlen) bionic_stack_chk_fail();
    return strcpy(d, s);
}
static char *bionic_strcat_chk(char *d, const char *s, size_t dlen) {
    if (strlen(d) + strlen(s) + 1 > dlen) bionic_stack_chk_fail();
    return strcat(d, s);
}
static size_t bionic_strlen_chk(const char *s, size_t dlen) {
    size_t n = strlen(s);
    if (n > dlen) bionic_stack_chk_fail();  // > e nao >=: n == dlen e valido
    return n;
}
static char *bionic_strncpy_chk2(char *d, const char *s, size_t n, size_t dlen, size_t slen) {
    (void)slen;
    if (n > dlen) bionic_stack_chk_fail();
    return strncpy(d, s, n);
}
// FF4 usa alguns _chk que o ff3 nao importava. Semantica bionic: o ultimo
// arg e o tamanho conhecido do buffer; abortam em overflow, NAO truncam.
static char *bionic_strncpy_chk(char *d, const char *s, size_t n, size_t dlen) {
    if (n > dlen) bionic_stack_chk_fail();
    return strncpy(d, s, n);
}
static char *bionic_strncat_chk(char *d, const char *s, size_t n, size_t dlen) {
    if (strlen(d) + (strlen(s) < n ? strlen(s) : n) + 1 > dlen) bionic_stack_chk_fail();
    return strncat(d, s, n);
}
static char *bionic_strchr_chk(const char *s, int c, size_t slen) {
    (void)slen;
    return strchr(s, c);
}
static char *bionic_strrchr_chk(const char *s, int c, size_t slen) {
    (void)slen;
    return strrchr(s, c);
}
// vsnprintf_chk: como vsnprintf normal (ESTE trunca em size-1, que e' o
// comportamento pedido). O perigoso e' o vsprintf_chk (sem limite) — ja tratado.
static int bionic_vsnprintf_chk(char *d, size_t size, int flags, size_t dlen,
                                const char *fmt, va_list ap) {
    (void)flags; (void)dlen;
    return vsnprintf(d, size, fmt, ap);
}
// FF4_STRLOG=1 mostra formatacoes de string do engine — usado para achar onde
// um nome de 4 letras vira 3.
static int str_log(void) {
    static int lvl = -1;
    if (lvl < 0) {
        const char *e = getenv("FF4_STRLOG");
        lvl = e ? atoi(e) : 0;
    }
    return lvl;
}

// ⚠️ O __vsprintf_chk do bionic se comporta como vsprintf (SEM LIMITE): o
// terceiro argumento e o tamanho do buffer conhecido pelo compilador, usado
// so para ABORTAR em overflow — nao para truncar. Usar vsnprintf(d, dlen, ...)
// TRUNCA em dlen-1 e come o ultimo caractere de nomes que ocupam o buffer
// inteiro. Por isso escrevemos completo e so checamos depois.
static int bionic_vsprintf_chk(char *d, int flags, size_t dlen, const char *fmt, va_list ap) {
    (void)flags;
    int n = vsprintf(d, fmt, ap);
    if (n >= 0 && (size_t)n >= dlen) {
        fprintf(stderr, "[str] __vsprintf_chk ESTOUROU: %d bytes em buffer de %zu (fmt=%s)\n", n,
                dlen, fmt);
    }
    if (str_log()) fprintf(stderr, "[str] vsprintf_chk(dlen=%zu, \"%s\") -> \"%s\"\n", dlen, fmt, d);
    return n;
}

static int shim_sprintf(char *d, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(d, fmt, ap);
    va_end(ap);
    if (str_log()) fprintf(stderr, "[str] sprintf(\"%s\") -> \"%s\"\n", fmt, d);
    return n;
}

static char *shim_strncpy(char *d, const char *s, size_t n) {
    char *r = strncpy(d, s, n);
    if (str_log()) fprintf(stderr, "[str] strncpy(n=%zu, \"%s\")\n", n, s);
    return r;
}

static int bionic_cxa_atexit(void (*f)(void *), void *a, void *dso) {
    (void)f; (void)a; (void)dso;
    return 0;  // nao rodamos destrutores: o processo morre inteiro
}
static void bionic_cxa_finalize(void *dso) { (void)dso; }
static void bionic_cxa_pure_virtual(void) {
    fprintf(stderr, "[ff4] chamada virtual pura\n");
    abort();
}
// Guardas de inicializacao de estatico local. O bionic usa 4 bytes; o
// itanium ABI do gcc usa 8. Como o libff4 aloca o guarda, tratamos como 32 bits.
static int bionic_cxa_guard_acquire(uint32_t *g) { return *g == 0; }
static void bionic_cxa_guard_release(uint32_t *g) { *g = 1; }

static int android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", tag ? tag : "ff3");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return 0;
}

// ---------------------------------------------------------------------------
// pthread encapsulado (ver nota de mismatch no topo)
// ---------------------------------------------------------------------------
// O handle que o libff4 possui tem tamanho bionic; guardamos so um ponteiro.
static int shim_mutex_init(void **m, const void *attr) {
    (void)attr;
    pthread_mutex_t *real = calloc(1, sizeof *real);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    // O engine usa mutex recursivo em alguns pontos; recursivo e superset seguro.
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(real, &a);
    pthread_mutexattr_destroy(&a);
    *m = real;
    return 0;
}
static int shim_mutex_destroy(void **m) {
    if (*m) { pthread_mutex_destroy(*m); free(*m); *m = NULL; }
    return 0;
}
static int shim_mutex_lock(void **m) {
    if (!*m) shim_mutex_init(m, NULL);  // alguns mutex vem de inicializador estatico
    return pthread_mutex_lock(*m);
}
static int shim_mutex_unlock(void **m) {
    return *m ? pthread_mutex_unlock(*m) : 0;
}
static int shim_cond_init(void **c, const void *attr) {
    (void)attr;
    pthread_cond_t *real = calloc(1, sizeof *real);
    pthread_cond_init(real, NULL);
    *c = real;
    return 0;
}
static int shim_cond_destroy(void **c) {
    if (*c) { pthread_cond_destroy(*c); free(*c); *c = NULL; }
    return 0;
}
static int shim_cond_wait(void **c, void **m) {
    if (!*c) shim_cond_init(c, NULL);
    if (!*m) shim_mutex_init(m, NULL);
    return pthread_cond_wait(*c, *m);
}
static int shim_cond_broadcast(void **c) {
    return *c ? pthread_cond_broadcast(*c) : 0;
}
// pthread_attr_t do bionic e menor que o do glibc: ignoramos o attr recebido.
static int shim_thread_create(void **thread, const void *attr, void *(*fn)(void *), void *arg) {
    (void)attr;
    pthread_t *t = calloc(1, sizeof *t);
    int r = pthread_create(t, NULL, fn, arg);
    *thread = t;
    return r;
}
static int shim_thread_join(void *thread, void **ret) {
    return thread ? pthread_join(*(pthread_t *)thread, ret) : 0;
}
static int shim_mutexattr_init(void *a) { (void)a; return 0; }
static int shim_mutexattr_destroy(void *a) { (void)a; return 0; }
static int shim_mutexattr_settype(void *a, int t) { (void)a; (void)t; return 0; }

// ---------------------------------------------------------------------------
// Contadores de GL (FF4_GLLOG=1): dizem se o engine esta MESMO desenhando.
// Tela solida com draws > 0 = problema de estado/matriz; draws == 0 = o engine
// nao esta emitindo geometria.
// ---------------------------------------------------------------------------
static unsigned long g_n_draw, g_n_clear, g_n_tex, g_n_teximg;

static void count_draw(GLenum mode, GLint first, GLsizei count) {
    g_n_draw++;
    glDrawArrays(mode, first, count);
}
static void count_clear(GLbitfield mask) {
    g_n_clear++;
    glClear(mask);
}
static void count_bindtex(GLenum t, GLuint tex) {
    g_n_tex++;
    glBindTexture(t, tex);
}
static void count_teximage(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b,
                           GLenum f, GLenum ty, const void *px) {
    g_n_teximg++;
    glTexImage2D(t, l, ifmt, w, h, b, f, ty, px);
}

void ff4_gl_stats(void) {
    fprintf(stderr, "[gl] draws=%lu clears=%lu binds=%lu teximage=%lu err=0x%x\n", g_n_draw,
            g_n_clear, g_n_tex, g_n_teximg, glGetError());
    g_n_draw = g_n_clear = g_n_tex = g_n_teximg = 0;
}

// ---------------------------------------------------------------------------
// AAsset — o engine so usa isto para o obb; redirecionamos pro nosso VFS.
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t *data;
    size_t len;
    size_t pos;
} asset_t;

static void *aasset_manager_from_java(void *env, void *obj) {
    (void)env; (void)obj;
    return (void *)1;  // handle simbolico
}
static void *aasset_open(void *mgr, const char *name, int mode) {
    (void)mgr; (void)mode;
    size_t len = 0;
    uint8_t *data = game_vfs_read(name, &len);
    if (!data) return NULL;
    asset_t *a = calloc(1, sizeof *a);
    a->data = data;
    a->len = len;
    return a;
}
static int aasset_read(void *h, void *buf, size_t count) {
    asset_t *a = h;
    if (!a) return -1;
    size_t n = a->len - a->pos < count ? a->len - a->pos : count;
    memcpy(buf, a->data + a->pos, n);
    a->pos += n;
    return (int)n;
}
static long aasset_seek(void *h, long off, int whence) {
    asset_t *a = h;
    if (!a) return -1;
    size_t base = whence == SEEK_CUR ? a->pos : (whence == SEEK_END ? a->len : 0);
    a->pos = base + off;
    return (long)a->pos;
}
static long aasset_get_length(void *h) { return h ? (long)((asset_t *)h)->len : 0; }
static void aasset_close(void *h) {
    asset_t *a = h;
    if (a) { free(a->data); free(a); }
}

// ---------------------------------------------------------------------------
// OpenSL ES — implementado em audio.c
// ---------------------------------------------------------------------------
extern uint32_t sl_create_engine(void **engine, uint32_t num_opts, const void *opts,
                                 uint32_t num_ifaces, const void *ifaces, const void *req);
extern const void *SL_IID_TABLE[];  // ids simbolicos, ver audio.c
extern const void *sl_iid(int index);

// Os SL_IID_* sao VARIAVEIS que CONTEM um ponteiro (SLInterfaceID). O engine
// le o VALOR da variavel e o repassa ao GetInterface — por isso cada uma precisa
// de um armazenamento proprio cujo ENDERECO e o identificador comparavel.
#define SL_IID_DEF(n)                        \
    static const char sl_store_##n = 0;      \
    const void *sl_var_##n = &sl_store_##n;

SL_IID_DEF(NULL_) SL_IID_DEF(OBJECT) SL_IID_DEF(ENGINE) SL_IID_DEF(OUTPUTMIX)
SL_IID_DEF(PLAY) SL_IID_DEF(BUFFERQUEUE) SL_IID_DEF(ANDROIDSIMPLEBUFFERQUEUE)
SL_IID_DEF(VOLUME) SL_IID_DEF(SEEK) SL_IID_DEF(PREFETCHSTATUS)
SL_IID_DEF(ANDROIDCONFIGURATION) SL_IID_DEF(PLAYBACKRATE) SL_IID_DEF(PITCH)
SL_IID_DEF(RATEPITCH) SL_IID_DEF(MUTESOLO) SL_IID_DEF(METADATAEXTRACTION)
SL_IID_DEF(METADATATRAVERSAL) SL_IID_DEF(DYNAMICSOURCE) SL_IID_DEF(EFFECTSEND)
SL_IID_DEF(ENVIRONMENTALREVERB) SL_IID_DEF(PRESETREVERB) SL_IID_DEF(EQUALIZER)
SL_IID_DEF(BASSBOOST) SL_IID_DEF(VIRTUALIZER) SL_IID_DEF(VISUALIZATION)
SL_IID_DEF(RECORD) SL_IID_DEF(AUDIOIODEVICECAPABILITIES) SL_IID_DEF(AUDIODECODERCAPABILITIES)
SL_IID_DEF(AUDIOENCODER) SL_IID_DEF(AUDIOENCODERCAPABILITIES) SL_IID_DEF(ENGINECAPABILITIES)
SL_IID_DEF(DEVICEVOLUME) SL_IID_DEF(DYNAMICINTERFACEMANAGEMENT) SL_IID_DEF(LED)
SL_IID_DEF(VIBRA) SL_IID_DEF(MIDIMESSAGE) SL_IID_DEF(MIDIMUTESOLO) SL_IID_DEF(MIDITEMPO)
SL_IID_DEF(MIDITIME) SL_IID_DEF(THREADSYNC) SL_IID_DEF(3DCOMMIT) SL_IID_DEF(3DDOPPLER)
SL_IID_DEF(3DGROUPING) SL_IID_DEF(3DLOCATION) SL_IID_DEF(3DMACROSCOPIC) SL_IID_DEF(3DSOURCE)
SL_IID_DEF(ANDROIDEFFECT) SL_IID_DEF(ANDROIDEFFECTCAPABILITIES) SL_IID_DEF(ANDROIDEFFECTSEND)

// O audio.c compara contra os VALORES.
const void *g_iid_engine, *g_iid_play, *g_iid_bq, *g_iid_volume;
__attribute__((constructor)) static void init_iids(void) {
    g_iid_engine = sl_var_ENGINE;
    g_iid_play = sl_var_PLAY;
    g_iid_bq = sl_var_ANDROIDSIMPLEBUFFERQUEUE;
    g_iid_volume = sl_var_VOLUME;
}

// ---------------------------------------------------------------------------
// Tabela final
// ---------------------------------------------------------------------------
#define IMP(name) {#name, (uintptr_t)&name}
#define IMP_AS(sym, fn) {sym, (uintptr_t)&fn}
#define IMP_VAL(sym, v) {sym, (uintptr_t)(v)}

const so_import_t g_ff4_imports[] = {
    // --- bionic ---
    IMP_AS("__errno", bionic_errno),
    IMP_AS("__stack_chk_fail", bionic_stack_chk_fail),
    IMP_VAL("__stack_chk_guard", &bionic_stack_chk_guard),
    IMP_AS("__memcpy_chk", bionic_memcpy_chk),
    IMP_AS("__strcpy_chk", bionic_strcpy_chk),
    IMP_AS("__strcat_chk", bionic_strcat_chk),
    IMP_AS("__strlen_chk", bionic_strlen_chk),
    IMP_AS("__strncpy_chk2", bionic_strncpy_chk2),
    IMP_AS("__strncpy_chk", bionic_strncpy_chk),
    IMP_AS("__strncat_chk", bionic_strncat_chk),
    IMP_AS("__strchr_chk", bionic_strchr_chk),
    IMP_AS("__strrchr_chk", bionic_strrchr_chk),
    IMP_AS("__vsprintf_chk", bionic_vsprintf_chk),
    IMP_AS("__vsnprintf_chk", bionic_vsnprintf_chk),
    IMP_AS("__cxa_atexit", bionic_cxa_atexit),
    IMP_AS("__cxa_finalize", bionic_cxa_finalize),
    IMP_AS("__cxa_pure_virtual", bionic_cxa_pure_virtual),
    IMP_AS("__cxa_guard_acquire", bionic_cxa_guard_acquire),
    IMP_AS("__cxa_guard_release", bionic_cxa_guard_release),
    IMP_AS("__android_log_print", android_log_print),

    // --- libc ---
    IMP(abort), IMP(atof), IMP(atoi), IMP(calloc), IMP(fclose), IMP(fopen), IMP(fread),
    IMP(free), IMP(fseek), IMP(ftell), IMP(fwrite), IMP(gettimeofday), IMP(localtime),
    IMP(malloc), IMP(memchr), IMP(memcmp), IMP(memcpy), IMP(memmove), IMP(memset),
    IMP(qsort), IMP(rand), IMP(realloc), IMP_AS("sprintf", shim_sprintf), IMP(srand), IMP(stpcpy),
    IMP(strchr), IMP(strcmp), IMP(strcpy), IMP(strlen), IMP(strncasecmp), IMP(strncmp),
    IMP_AS("strncpy", shim_strncpy), IMP(strrchr), IMP(strtok), IMP(strtol), IMP(time), IMP(toupper),
    IMP(usleep), IMP(vsnprintf),
    // --- extras que o FF4 base importa alem do ff3 ---
    IMP(fputc), IMP(gmtime), IMP(printf), IMP(puts), IMP(strcat), IMP(strstr), IMP(vsprintf),

    // --- libm ---
    IMP(atan2f), IMP(atanf), IMP(cos), IMP(cosf), IMP(sin), IMP(sinf), IMP(sincos),
    IMP(sqrtf), IMP(tanf),

    // --- C++ ---
    IMP_AS("_ZdaPv", free),          // operator delete[]
    IMP_AS("_Znam", malloc),         // operator new[]
    IMP_AS("_ZdlPv", free),          // operator delete
    IMP_AS("_Znwm", malloc),         // operator new

    // --- pthread (encapsulado) ---
    IMP_AS("pthread_mutex_init", shim_mutex_init),
    IMP_AS("pthread_mutex_destroy", shim_mutex_destroy),
    IMP_AS("pthread_mutex_lock", shim_mutex_lock),
    IMP_AS("pthread_mutex_unlock", shim_mutex_unlock),
    IMP_AS("pthread_mutexattr_init", shim_mutexattr_init),
    IMP_AS("pthread_mutexattr_destroy", shim_mutexattr_destroy),
    IMP_AS("pthread_mutexattr_settype", shim_mutexattr_settype),
    IMP_AS("pthread_cond_init", shim_cond_init),
    IMP_AS("pthread_cond_destroy", shim_cond_destroy),
    IMP_AS("pthread_cond_wait", shim_cond_wait),
    IMP_AS("pthread_cond_broadcast", shim_cond_broadcast),
    IMP_AS("pthread_create", shim_thread_create),
    IMP_AS("pthread_join", shim_thread_join),

    // --- AAsset ---
    IMP_AS("AAssetManager_fromJava", aasset_manager_from_java),
    IMP_AS("AAssetManager_open", aasset_open),
    IMP_AS("AAsset_read", aasset_read),
    IMP_AS("AAsset_seek", aasset_seek),
    IMP_AS("AAsset_getLength", aasset_get_length),
    IMP_AS("AAsset_close", aasset_close),

    // --- GLES1 ---
    // Só ficam aqui as 4 entradas INSTRUMENTADAS (contadores de diagnostico).
    // As outras 41 nao entram na tabela estatica: o so_util as pede ao
    // resolvedor do nxgl (nxgl_gles1_lookup), porque o binario nao linka
    // libGLESv1_CM — esse SONAME nao existe em toda CFW. Um endereco de
    // ponteiro resolvido em tempo de execucao nao e' expressao constante e
    // portanto nao pode inicializar esta tabela.
    IMP_AS("glBindTexture", count_bindtex), IMP_AS("glClear", count_clear),
    IMP_AS("glDrawArrays", count_draw), IMP_AS("glTexImage2D", count_teximage),

    // --- OpenSL ES ---
    IMP_AS("slCreateEngine", sl_create_engine),
    IMP_VAL("SL_IID_NULL", &sl_var_NULL_),
    IMP_VAL("SL_IID_OBJECT", &sl_var_OBJECT),
    IMP_VAL("SL_IID_ENGINE", &sl_var_ENGINE),
    IMP_VAL("SL_IID_OUTPUTMIX", &sl_var_OUTPUTMIX),
    IMP_VAL("SL_IID_PLAY", &sl_var_PLAY),
    IMP_VAL("SL_IID_BUFFERQUEUE", &sl_var_BUFFERQUEUE),
    IMP_VAL("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", &sl_var_ANDROIDSIMPLEBUFFERQUEUE),
    IMP_VAL("SL_IID_VOLUME", &sl_var_VOLUME),
    IMP_VAL("SL_IID_SEEK", &sl_var_SEEK),
    IMP_VAL("SL_IID_PREFETCHSTATUS", &sl_var_PREFETCHSTATUS),
    IMP_VAL("SL_IID_ANDROIDCONFIGURATION", &sl_var_ANDROIDCONFIGURATION),
    IMP_VAL("SL_IID_PLAYBACKRATE", &sl_var_PLAYBACKRATE),
    IMP_VAL("SL_IID_PITCH", &sl_var_PITCH),
    IMP_VAL("SL_IID_RATEPITCH", &sl_var_RATEPITCH),
    IMP_VAL("SL_IID_MUTESOLO", &sl_var_MUTESOLO),
    IMP_VAL("SL_IID_METADATAEXTRACTION", &sl_var_METADATAEXTRACTION),
    IMP_VAL("SL_IID_METADATATRAVERSAL", &sl_var_METADATATRAVERSAL),
    IMP_VAL("SL_IID_DYNAMICSOURCE", &sl_var_DYNAMICSOURCE),
    IMP_VAL("SL_IID_EFFECTSEND", &sl_var_EFFECTSEND),
    IMP_VAL("SL_IID_ENVIRONMENTALREVERB", &sl_var_ENVIRONMENTALREVERB),
    IMP_VAL("SL_IID_PRESETREVERB", &sl_var_PRESETREVERB),
    IMP_VAL("SL_IID_EQUALIZER", &sl_var_EQUALIZER),
    IMP_VAL("SL_IID_BASSBOOST", &sl_var_BASSBOOST),
    IMP_VAL("SL_IID_VIRTUALIZER", &sl_var_VIRTUALIZER),
    IMP_VAL("SL_IID_VISUALIZATION", &sl_var_VISUALIZATION),
    IMP_VAL("SL_IID_RECORD", &sl_var_RECORD),
    IMP_VAL("SL_IID_AUDIOIODEVICECAPABILITIES", &sl_var_AUDIOIODEVICECAPABILITIES),
    IMP_VAL("SL_IID_AUDIODECODERCAPABILITIES", &sl_var_AUDIODECODERCAPABILITIES),
    IMP_VAL("SL_IID_AUDIOENCODER", &sl_var_AUDIOENCODER),
    IMP_VAL("SL_IID_AUDIOENCODERCAPABILITIES", &sl_var_AUDIOENCODERCAPABILITIES),
    IMP_VAL("SL_IID_ENGINECAPABILITIES", &sl_var_ENGINECAPABILITIES),
    IMP_VAL("SL_IID_DEVICEVOLUME", &sl_var_DEVICEVOLUME),
    IMP_VAL("SL_IID_DYNAMICINTERFACEMANAGEMENT", &sl_var_DYNAMICINTERFACEMANAGEMENT),
    IMP_VAL("SL_IID_LED", &sl_var_LED),
    IMP_VAL("SL_IID_VIBRA", &sl_var_VIBRA),
    IMP_VAL("SL_IID_MIDIMESSAGE", &sl_var_MIDIMESSAGE),
    IMP_VAL("SL_IID_MIDIMUTESOLO", &sl_var_MIDIMUTESOLO),
    IMP_VAL("SL_IID_MIDITEMPO", &sl_var_MIDITEMPO),
    IMP_VAL("SL_IID_MIDITIME", &sl_var_MIDITIME),
    IMP_VAL("SL_IID_THREADSYNC", &sl_var_THREADSYNC),
    IMP_VAL("SL_IID_3DCOMMIT", &sl_var_3DCOMMIT),
    IMP_VAL("SL_IID_3DDOPPLER", &sl_var_3DDOPPLER),
    IMP_VAL("SL_IID_3DGROUPING", &sl_var_3DGROUPING),
    IMP_VAL("SL_IID_3DLOCATION", &sl_var_3DLOCATION),
    IMP_VAL("SL_IID_3DMACROSCOPIC", &sl_var_3DMACROSCOPIC),
    IMP_VAL("SL_IID_3DSOURCE", &sl_var_3DSOURCE),
    IMP_VAL("SL_IID_ANDROIDEFFECT", &sl_var_ANDROIDEFFECT),
    IMP_VAL("SL_IID_ANDROIDEFFECTCAPABILITIES", &sl_var_ANDROIDEFFECTCAPABILITIES),
    IMP_VAL("SL_IID_ANDROIDEFFECTSEND", &sl_var_ANDROIDEFFECTSEND),
};

const size_t g_ff4_imports_count = sizeof g_ff4_imports / sizeof g_ff4_imports[0];
