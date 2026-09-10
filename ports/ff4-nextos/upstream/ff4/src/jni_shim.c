// jni_shim (FF IV 3D base) — ver jni_shim.h.
//
// Indices da JNINativeInterface = spec JNI padrao (confirmados no ff3/ff4a
// contra o assembly do proxy; mesmo NDK). As variantes V (va_list) sao as que
// o engine cuore realmente usa; registramos as duas por seguranca.
#include "jni_shim.h"
#include "game.h"
#include "vkbd.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define JNI_FindClass                  6
#define JNI_ExceptionOccurred          15
#define JNI_ExceptionClear             17
#define JNI_NewGlobalRef               21
#define JNI_DeleteGlobalRef            22
#define JNI_DeleteLocalRef             23
#define JNI_GetStaticMethodID          113
#define JNI_CallStaticObjectMethod     114
#define JNI_CallStaticBooleanMethod    117
#define JNI_CallStaticIntMethod        129
#define JNI_CallStaticLongMethod       132
#define JNI_CallStaticVoidMethod       141
#define JNI_NewStringUTF               167
#define JNI_GetStringUTFLength         168
#define JNI_GetStringUTFChars          169
#define JNI_ReleaseStringUTFChars      170
#define JNI_GetArrayLength             171
#define JNI_NewByteArray               176
#define JNI_NewIntArray                179
#define JNI_GetByteArrayElements       184
#define JNI_GetIntArrayElements        187
#define JNI_ReleaseByteArrayElements   192
#define JNI_ReleaseIntArrayElements    195
#define JNI_GetByteArrayRegion         200
#define JNI_GetIntArrayRegion          203
#define JNI_SetByteArrayRegion         208
#define JNI_SetIntArrayRegion          211
#define JNI_TABLE_SLOTS                256

struct jni_env { const void **functions; };

typedef struct {
    uint32_t tag;
    size_t len;
    void *data;
} jhandle_t;
#define TAG_BYTES 0x41525242u
#define TAG_INTS  0x41525249u
#define TAG_STR   0x53545220u

static jhandle_t *handle_new(uint32_t tag, size_t len, void *data) {
    jhandle_t *h = malloc(sizeof *h);
    h->tag = tag; h->len = len; h->data = data;
    return h;
}

// --- estado que o engine PUXA -----------------------------------------------
static int g_key_mask = 0;      // getKeyEvent()
static int g_screen_w = 1280;   // getResWidth()
static int g_screen_h = 720;    // getResHeight()
static int g_back_mode = 0;     // assignBackButton(k)
static int g_fps = 30;          // setFPS(i)
static void (*g_pad_poll)(void);            // re-amostra o pad a cada tick logico

// Nome digitado na tela de nomear: o engine chama createEditText(nomePadrao,max)
// e depois getEditText(). Sem teclado virtual, guardamos o nome PADRAO e o
// devolvemos — assim o personagem ja vem nomeado (Cecil etc), como nos outros FF.
static char g_edit_text[128];

void jni_set_keystate(int mask) { g_key_mask = mask; }
void jni_set_screen(int w, int h) { g_screen_w = w; g_screen_h = h; }
int  jni_get_back_mode(void) { return g_back_mode; }
int  jni_get_fps(void) { return g_fps; }
void jni_set_pad_poll(void (*cb)(void)) { g_pad_poll = cb; }

// --- metodos estaticos do MainActivity (FF IV 3D) ---------------------------
typedef enum {
    M_UNKNOWN = 0,
    M_getCurrentFrame, M_loadFile, M_loadTexture, M_drawFont, M_decodeString,
    M_getLanguage, M_getKeyEvent, M_getResWidth, M_getResHeight, M_setFPS,
    M_assignBackButton, M_createSaveFile, M_getMovieState, M_playMovie, M_stopMovie,
    M_getDeviceAndroidTV, M_isDeviceAndroidTV, M_setDeviceAndroidTV,
    M_createEditText, M_getEditText, M_startCloud, M_startDownload, M_webTo,
    M_showPrivacyPolicy,
    // viewport (o engine PUXA a posicao/tamanho da view letterboxed)
    M_updateViewportSize, M_getViewPosX, M_getViewPosY, M_getViewWidth, M_getViewHeight,
    M_getSaveDataPath, M_isOKAchievement, M_loadSound, M_getDownloadState, M_getStoragePath,
    M_isSoundFileExist, M_setBackLight,
} method_id_t;

// viewport letterbox (espelha updateViewportSize/getView* do MainActivity).
static int g_view_x = 0, g_view_y = 0, g_view_w = 1280, g_view_h = 720;
void jni_get_view(int *x,int *y,int *w,int *h){
  if(x)*x=g_view_x; if(y)*y=g_view_y; if(w)*w=g_view_w; if(h)*h=g_view_h;
}

static void do_update_viewport(int w, int h, int fs) {
    int F, G;
    if (fs) {
        if (w * 3 >= h * 4) {          // >= 4:3 (paisagem)
            G = h; F = w;
            int cap = h * 21 / 9;      // clamp largura a 21:9
            if (w > cap) F = cap;
        } else {                        // mais alto que 4:3
            F = w; G = w * 3 / 4;
        }
    } else {                            // janela -> alvo 960x640 (3:2)
        if (w * 960 >= h * 640) { G = h; F = h * 960 / 640; }
        else { F = w; G = w * 640 / 960; }
    }
    g_view_w = F; g_view_h = G;
    g_view_x = (w - F) / 2;
    g_view_y = (h - G) / 2;
}

typedef struct { const char *name; method_id_t id; } method_entry_t;

static const method_entry_t k_methods[] = {
    {"getCurrentFrame", M_getCurrentFrame}, {"loadFile", M_loadFile},
    {"loadTexture", M_loadTexture},         {"drawFont", M_drawFont},
    {"decodeString", M_decodeString},       {"getLanguage", M_getLanguage},
    {"getKeyEvent", M_getKeyEvent},         {"getResWidth", M_getResWidth},
    {"getResHeight", M_getResHeight},       {"setFPS", M_setFPS},
    {"assignBackButton", M_assignBackButton}, {"createSaveFile", M_createSaveFile},
    {"getMovieState", M_getMovieState},     {"playMovie", M_playMovie},
    {"stopMovie", M_stopMovie},             {"getDeviceAndroidTV", M_getDeviceAndroidTV},
    {"isDeviceAndroidTV", M_isDeviceAndroidTV}, {"setDeviceAndroidTV", M_setDeviceAndroidTV},
    {"createEditText", M_createEditText},   {"getEditText", M_getEditText},
    {"startCloud", M_startCloud},           {"startDownload", M_startDownload},
    {"webTo", M_webTo},                     {"showPrivacyPolicy", M_showPrivacyPolicy},
    {"updateViewportSize", M_updateViewportSize}, {"getViewPosX", M_getViewPosX},
    {"getViewPosY", M_getViewPosY},         {"getViewWidth", M_getViewWidth},
    {"getViewHeight", M_getViewHeight},     {"getSaveDataPath", M_getSaveDataPath},
    {"isOKAchievement", M_isOKAchievement}, {"loadSound", M_loadSound},
    {"getDownloadState", M_getDownloadState}, {"getStoragePath", M_getStoragePath},
    {"isSoundFileExist", M_isSoundFileExist}, {"setBackLight", M_setBackLight},
};

static const void *k_table[JNI_TABLE_SLOTS];
static struct jni_env g_env = {k_table};
static int g_activity_obj = 0xF4F4F4;
static int g_activity_cls = 0xC1A55;

static void *jni_find_class(jni_env_t *env, const char *name) {
    (void)env;
    if (strstr(name, "FFIV_GP/MainActivity")) return &g_activity_cls;
    fprintf(stderr, "[jni] FindClass desconhecida: %s\n", name);
    return &g_activity_cls;
}

static void *jni_get_static_method_id(jni_env_t *env, void *cls, const char *name,
                                      const char *sig) {
    (void)env; (void)cls;
    for (size_t i = 0; i < sizeof k_methods / sizeof k_methods[0]; i++)
        if (!strcmp(k_methods[i].name, name)) return (void *)&k_methods[i];
    fprintf(stderr, "[jni] metodo NAO MAPEADO: %s%s\n", name, sig);
    return NULL;
}

static method_id_t mid_of(void *mid) {
    if (!mid) return M_UNKNOWN;
    const method_entry_t *e = (const method_entry_t *)mid;
    static int lvl = -1;
    if (lvl < 0) { const char *v = getenv("FF4_JNILOG"); lvl = v ? atoi(v) : 0; }
    // metodos ~30x/s: afogariam o log.
    if (lvl && e->id != M_getCurrentFrame && e->id != M_getKeyEvent &&
        e->id != M_getViewPosX && e->id != M_getViewPosY &&
        e->id != M_getViewWidth && e->id != M_getViewHeight &&
        e->id != M_isOKAchievement && e->id != M_getDownloadState)
        fprintf(stderr, "[jni] -> %s\n", e->name);
    return e->id;
}

static void call_static_void_v(void *mid, va_list ap) {
    switch (mid_of(mid)) {
    case M_setFPS:
        g_fps = va_arg(ap, int);
        break;
    case M_assignBackButton: {
        int mode = va_arg(ap, int);
        if (mode != g_back_mode)
            fprintf(stderr, "[input] assignBackButton %d -> %d\n", g_back_mode, mode);
        g_back_mode = mode;
        break;
    }
    case M_createSaveFile:
        game_createSaveFile(va_arg(ap, int));
        break;
    case M_updateViewportSize: {
        int w = va_arg(ap, int), h = va_arg(ap, int), fs = va_arg(ap, int);
        do_update_viewport(w, h, fs);
        break;
    }
    case M_playMovie:  game_playMovie(); break;
    case M_stopMovie:  game_stopMovie(); break;
    case M_setBackLight:
        (void)va_arg(ap, int);  // controle de backlight — ignorado
        break;
    case M_createEditText: {
        // (String nomePadrao, int max). Abre o TECLADO VIRTUAL com o nome padrao.
        jhandle_t *s = va_arg(ap, jhandle_t *);
        int maxlen = va_arg(ap, int);
        const char *def = s && s->data ? (const char *)s->data : "";
        // Diagnostico do nome padrao (campo em branco no device, 22/08): o
        // handle chegou? com que tag/len? Sem isso nao da' para saber se o
        // engine mandou vazio ou se a decodificacao da jstring falhou.
        fprintf(stderr, "[edit] handle=%p tag=%08x len=%zu data=%p\n",
                (void *)s, s ? s->tag : 0u, s ? (size_t)s->len : (size_t)0,
                s ? s->data : NULL);
        snprintf(g_edit_text, sizeof g_edit_text, "%s", def);
        vkbd_activate(def, maxlen);
        fprintf(stderr, "[edit] createEditText nome padrao=\"%s\" max=%d\n", g_edit_text, maxlen);
        break;
    }
    case M_setDeviceAndroidTV:
    case M_startCloud:
    case M_startDownload:
    case M_webTo:
    case M_showPrivacyPolicy:
        break;  // stubs seguros
    default:
        fprintf(stderr, "[jni] CallStaticVoid nao tratado (id=%d)\n", mid_of(mid));
        break;
    }
}

static void *call_static_object_v(void *mid, va_list ap) {
    void *ret = NULL;
    switch (mid_of(mid)) {
    case M_loadFile: {
        jhandle_t *s = va_arg(ap, jhandle_t *);
        const char *name = s && s->data ? (const char *)s->data : "";
        size_t len = 0;
        uint8_t *data = game_loadFile(name, &len);
        ret = data ? handle_new(TAG_BYTES, len, data) : NULL;
        break;
    }
    case M_loadTexture: {
        jhandle_t *png = va_arg(ap, jhandle_t *);
        size_t n = 0;
        int32_t *px = png ? game_loadTexture(png->data, png->len, &n) : NULL;
        ret = px ? handle_new(TAG_INTS, n, px) : NULL;
        break;
    }
    case M_loadSound: {
        jhandle_t *s = va_arg(ap, jhandle_t *);
        const char *name = s && s->data ? (const char *)s->data : "";
        size_t len = 0;
        uint8_t *d = game_loadSound(name, &len);
        ret = d ? handle_new(TAG_BYTES, len, d) : NULL;
        break;
    }
    case M_getStoragePath: {
        size_t len = 0;
        uint8_t *p = game_getSaveFileName(&len);  // getFilesDir() -> dir de saves
        ret = p ? handle_new(TAG_BYTES, len, p) : NULL;
        break;
    }
    case M_drawFont: {
        jhandle_t *s = va_arg(ap, jhandle_t *);
        int a = va_arg(ap, int), b = va_arg(ap, int), c = va_arg(ap, int);
        size_t n = 0;
        int32_t *px = game_drawFont(s && s->data ? (const char *)s->data : "", a, b, c, &n);
        ret = px ? handle_new(TAG_INTS, n, px) : NULL;
        break;
    }
    case M_decodeString: {
        jhandle_t *in = va_arg(ap, jhandle_t *);
        size_t len = 0;
        uint8_t *out = in ? game_decodeString(in->data, in->len, &len) : NULL;
        ret = out ? handle_new(TAG_BYTES, len, out) : NULL;
        break;
    }
    case M_getSaveDataPath: {
        size_t len = 0;
        uint8_t *p = game_getSaveDataPath(&len);
        ret = p ? handle_new(TAG_BYTES, len, p) : NULL;
        break;
    }
    case M_getEditText: {
        // devolve o texto do teclado virtual (ou o nome padrao guardado)
        const char *t = vkbd_text();
        if (!t || !t[0]) t = g_edit_text;
        ret = handle_new(TAG_STR, strlen(t), strdup(t));
        break;
    }
    default:
        fprintf(stderr, "[jni] CallStaticObject nao tratado (id=%d)\n", mid_of(mid));
        break;
    }
    return ret;
}

static int64_t call_static_long_v(void *mid, va_list ap) {
    if (mid_of(mid) == M_getCurrentFrame) {
        int64_t t = game_getCurrentFrame(va_arg(ap, int64_t));
        // re-amostra o pad NESTE tick (o app real atualiza a mascara num thread
        // Java proprio -> input vivo ate nos loops internos; congelada, um toque
        // curto virava "segurado por N ticks" e o repeat disparava varias vezes).
        if (g_pad_poll) g_pad_poll();
        return t;
    }
    fprintf(stderr, "[jni] CallStaticLong nao tratado (id=%d)\n", mid_of(mid));
    return 0;
}

static int32_t call_static_int_v(void *mid, va_list ap) {
    (void)ap;
    switch (mid_of(mid)) {
    case M_getLanguage:        return g_language;
    case M_getKeyEvent:        return g_key_mask;
    case M_getResWidth:        return g_screen_w;
    case M_getResHeight:       return g_screen_h;
    case M_getViewPosX:        return g_view_x;
    case M_getViewPosY:        return g_view_y;
    case M_getViewWidth:       return g_view_w;
    case M_getViewHeight:      return g_view_h;
    case M_getDownloadState:   return 0;  // APK completo, sem download
    case M_getDeviceAndroidTV: return 0;
    default:
        fprintf(stderr, "[jni] CallStaticInt nao tratado (id=%d)\n", mid_of(mid));
        return 0;
    }
}

static uint8_t call_static_boolean_v(void *mid, va_list ap) {
    (void)ap;
    switch (mid_of(mid)) {
    case M_getMovieState:     return (uint8_t)game_getMovieState();
    case M_isSoundFileExist: {
        jhandle_t *s = va_arg(ap, jhandle_t *);
        const char *name = s && s->data ? (const char *)s->data : "";
        size_t len = 0;
        uint8_t *d = game_loadSound(name, &len);
        if (d) { free(d); return 1; }
        return 0;
    }
    case M_isOKAchievement:   return 0;
    case M_isDeviceAndroidTV: return 0;
    default:
        fprintf(stderr, "[jni] CallStaticBoolean nao tratado (id=%d)\n", mid_of(mid));
        return 0;
    }
}

#define CALL_PAIR(suffix, ret_type, dispatch)                                            \
    static ret_type jni_call_static_##suffix(jni_env_t *env, void *cls, void *mid, ...) { \
        (void)env; (void)cls;                                                            \
        va_list ap; va_start(ap, mid);                                                   \
        ret_type r = dispatch(mid, ap); va_end(ap); return r;                            \
    }                                                                                    \
    static ret_type jni_call_static_##suffix##_v(jni_env_t *env, void *cls, void *mid,   \
                                                 va_list ap) {                           \
        (void)env; (void)cls; return dispatch(mid, ap);                                  \
    }
CALL_PAIR(object, void *, call_static_object_v)
CALL_PAIR(boolean, uint8_t, call_static_boolean_v)
CALL_PAIR(int, int32_t, call_static_int_v)
CALL_PAIR(long, int64_t, call_static_long_v)

static void jni_call_static_void(jni_env_t *env, void *cls, void *mid, ...) {
    (void)env; (void)cls;
    va_list ap; va_start(ap, mid); call_static_void_v(mid, ap); va_end(ap);
}
static void jni_call_static_void_v(jni_env_t *env, void *cls, void *mid, va_list ap) {
    (void)env; (void)cls; call_static_void_v(mid, ap);
}

static void *jni_new_string_utf(jni_env_t *env, const char *s) {
    (void)env;
    if (getenv("FF4_STRLOG") && s && strlen(s) <= 24)
        fprintf(stderr, "[jni] NewStringUTF(\"%s\")\n", s);
    return handle_new(TAG_STR, s ? strlen(s) : 0, s ? strdup(s) : NULL);
}
static const char *jni_get_string_utf_chars(jni_env_t *env, void *str, uint8_t *is_copy) {
    (void)env;
    if (is_copy) *is_copy = 0;
    jhandle_t *h = str;
    return h ? (const char *)h->data : NULL;
}
static int32_t jni_get_string_utf_length(jni_env_t *env, void *str) {
    (void)env;
    jhandle_t *h = str;
    return h ? (int32_t)h->len : 0;
}
static void jni_release_string_utf_chars(jni_env_t *env, void *str, const char *chars) {
    (void)env; (void)str; (void)chars;
}
static int32_t jni_get_array_length(jni_env_t *env, void *arr) {
    (void)env;
    jhandle_t *h = arr;
    return h ? (int32_t)h->len : 0;
}
static void *jni_new_byte_array(jni_env_t *env, int32_t len) {
    (void)env;
    return handle_new(TAG_BYTES, len, calloc(len ? len : 1, 1));
}
static void *jni_new_int_array(jni_env_t *env, int32_t len) {
    (void)env;
    return handle_new(TAG_INTS, len, calloc(len ? len : 1, 4));
}
static void *jni_get_array_elements(jni_env_t *env, void *arr, uint8_t *is_copy) {
    (void)env;
    if (is_copy) *is_copy = 0;
    jhandle_t *h = arr;
    return h ? h->data : NULL;
}
static void jni_release_array_elements(jni_env_t *env, void *arr, void *elems, int32_t mode) {
    (void)env; (void)arr; (void)elems; (void)mode;
}
static size_t elem_size(const jhandle_t *h) { return (h && h->tag == TAG_INTS) ? 4 : 1; }
static void jni_get_array_region(jni_env_t *env, void *arr, int32_t start, int32_t len, void *buf) {
    (void)env;
    jhandle_t *h = arr;
    if (!h || !h->data || !buf) return;
    size_t es = elem_size(h);
    if (start < 0 || len < 0 || (size_t)(start + len) > h->len) {
        fprintf(stderr, "[jni] GetArrayRegion fora dos limites (%d+%d de %zu)\n", start, len, h->len);
        return;
    }
    memcpy(buf, (uint8_t *)h->data + (size_t)start * es, (size_t)len * es);
}
static void jni_set_array_region(jni_env_t *env, void *arr, int32_t start, int32_t len, const void *buf) {
    (void)env;
    jhandle_t *h = arr;
    if (!h || !h->data || !buf) return;
    size_t es = elem_size(h);
    if (start < 0 || len < 0 || (size_t)(start + len) > h->len) {
        fprintf(stderr, "[jni] SetArrayRegion fora dos limites (%d+%d de %zu)\n", start, len, h->len);
        return;
    }
    memcpy((uint8_t *)h->data + (size_t)start * es, buf, (size_t)len * es);
}
static int jni_get_version(jni_env_t *env) { (void)env; return 0x00010006; /* JNI 1.6 */ }
static void *jni_new_global_ref(jni_env_t *env, void *obj) { (void)env; return obj; }
static void jni_delete_ref(jni_env_t *env, void *obj) { (void)env; (void)obj; }
static void *jni_exception_occurred(jni_env_t *env) { (void)env; return NULL; }
static void jni_exception_clear(jni_env_t *env) { (void)env; }

// Slot JNI nao implementado: em vez de ABORTAR (fechava o jogo), loga 1x e
// devolve 0 (NULL/0/void) -> degrada gracioso. Assim um upcall raro nao mata a
// sessao do jogador. O log aponta o slot p/ implementarmos depois se importar.
static int jni_unimplemented(int slot) {
    static uint8_t logged[JNI_TABLE_SLOTS];
    if (slot >= 0 && slot < JNI_TABLE_SLOTS && !logged[slot]) {
        logged[slot] = 1;
        fprintf(stderr, "[jni] slot %d (offset %d) nao implementado -> retorna 0\n", slot, slot * 8);
    }
    /* Onda v2 (AUD-08): slots Call*Float/Double retornam em d0, nao em x0 --
     * so' zerar w0 deixava d0 com lixo do frame anterior (volume/delta-time
     * aleatorios SEM log). Zerar d0 aqui cobre os dois mundos; o fprintf
     * acima pode ter sujado d0, por isso o asm vem DEPOIS dele. */
    __asm__ volatile("movi d0, #0" ::: "d0");
    return 0;
}
static void install_slot_stubs(void) {
    const size_t stub_words = 5;
    uint32_t *pool = mmap(NULL, JNI_TABLE_SLOTS * stub_words * 4, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pool == MAP_FAILED) return;
    for (int i = 0; i < JNI_TABLE_SLOTS; i++) {
        if (k_table[i]) continue;
        uint32_t *c = pool + (size_t)i * stub_words;
        c[0] = 0x52800000 | ((i & 0xffff) << 5);  // mov w0, #i
        c[1] = 0x58000050;                        // ldr x16, [pc, #8]
        c[2] = 0xd61f0200;                        // br x16
        *(uint64_t *)&c[3] = (uint64_t)(uintptr_t)jni_unimplemented;
        k_table[i] = c;
    }
    __builtin___clear_cache((char *)pool, (char *)pool + JNI_TABLE_SLOTS * stub_words * 4);
    mprotect(pool, JNI_TABLE_SLOTS * stub_words * 4, PROT_READ | PROT_EXEC);
}

void jni_shim_init(void) {
    memset(k_table, 0, sizeof k_table);
    /* Onda v2 (AUD-18 parcial): GetVersion devolvendo 0 fazia engine checar a
     * versao e desligar subsistema em silencio; slot 4 = JNI 1.6, identico ao
     * ff4a. Os upcalls de metodo de instancia continuam no stub de proposito:
     * o ff4 roda perfeito com o modelo atual e dispatch novo sem prova fisica
     * seria risco, nao conserto (#33). */
    k_table[4] = jni_get_version;
    k_table[JNI_FindClass] = jni_find_class;
    k_table[JNI_ExceptionOccurred] = jni_exception_occurred;
    k_table[JNI_ExceptionClear] = jni_exception_clear;
    k_table[JNI_NewGlobalRef] = jni_new_global_ref;
    k_table[JNI_DeleteGlobalRef] = jni_delete_ref;
    k_table[JNI_DeleteLocalRef] = jni_delete_ref;
    k_table[JNI_GetStaticMethodID] = jni_get_static_method_id;
    k_table[JNI_CallStaticObjectMethod] = jni_call_static_object;
    k_table[JNI_CallStaticBooleanMethod] = jni_call_static_boolean;
    k_table[JNI_CallStaticIntMethod] = jni_call_static_int;
    k_table[JNI_CallStaticLongMethod] = jni_call_static_long;
    k_table[JNI_CallStaticVoidMethod] = jni_call_static_void;
    k_table[JNI_CallStaticObjectMethod + 1] = jni_call_static_object_v;
    k_table[JNI_CallStaticBooleanMethod + 1] = jni_call_static_boolean_v;
    k_table[JNI_CallStaticIntMethod + 1] = jni_call_static_int_v;
    k_table[JNI_CallStaticLongMethod + 1] = jni_call_static_long_v;
    k_table[JNI_CallStaticVoidMethod + 1] = jni_call_static_void_v;
    k_table[JNI_NewStringUTF] = jni_new_string_utf;
    k_table[JNI_GetStringUTFLength] = jni_get_string_utf_length;
    k_table[JNI_GetStringUTFChars] = jni_get_string_utf_chars;
    k_table[JNI_ReleaseStringUTFChars] = jni_release_string_utf_chars;
    k_table[JNI_GetArrayLength] = jni_get_array_length;
    k_table[JNI_NewByteArray] = jni_new_byte_array;
    k_table[JNI_NewIntArray] = jni_new_int_array;
    k_table[JNI_GetByteArrayElements] = jni_get_array_elements;
    k_table[JNI_GetIntArrayElements] = jni_get_array_elements;
    k_table[JNI_ReleaseByteArrayElements] = jni_release_array_elements;
    k_table[JNI_ReleaseIntArrayElements] = jni_release_array_elements;
    k_table[JNI_GetByteArrayRegion] = jni_get_array_region;
    k_table[JNI_GetIntArrayRegion] = jni_get_array_region;
    k_table[JNI_SetByteArrayRegion] = jni_set_array_region;
    k_table[JNI_SetIntArrayRegion] = jni_set_array_region;
    install_slot_stubs();
}

jni_env_t *jni_shim_env(void) { return &g_env; }
jni_obj_t jni_shim_activity(void) { return &g_activity_obj; }
