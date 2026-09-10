/*
 * jni_shim.c -- JNIEnv/JavaVM falsos mínimos p/ o FF4.
 *
 * A engine recebe o JNIEnv nos args de render/resume/touch e faz upcalls p/ pegar
 * AssetManager / filesDir / idioma. Aqui montamos uma tabela JNINativeInterface
 * de 256 slots com um default seguro (retorna 0) e sobrescrevemos os slots-chave
 * com loggers, p/ descobrir no 1º boot o que ela pede. Índices = ordem do jni.h.
 */
#define _GNU_SOURCE
#include "jni_shim.h"
#include "ff_language.h"
#include "obb_data.h"
#include "vkbd.h"
#include "texture.h"
#include "util.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void *ff4a_fake_obj = (void *)0x00FF4A00; /* "this"/clazz fake não-nulo */

/* ---- jbyteArray fake: {len, data}. Registro p/ free seguro em DeleteLocalRef ---- */
typedef struct {
  int len;
  unsigned char *data;
} FakeArray;

static FakeArray *g_arrays[4096];
static int g_n_arrays;

static FakeArray *fa_new(unsigned char *data, int len) {
  FakeArray *fa = (FakeArray *)malloc(sizeof(FakeArray));
  fa->len = len;
  fa->data = data;
  if (g_n_arrays < 4096) g_arrays[g_n_arrays++] = fa;
  return fa;
}
static int fa_index(void *p) {
  for (int i = 0; i < g_n_arrays; i++)
    if (g_arrays[i] == p) return i;
  return -1;
}
static void fa_free(int idx) {
  FakeArray *fa = g_arrays[idx];
  free(fa->data);
  free(fa);
  g_arrays[idx] = g_arrays[--g_n_arrays];
}

/* método-IDs distintos: guardamos o nome por índice p/ despachar em CallStatic. */
#define MID_BASE 0x4D000000u
static char *g_method_names[512];
static int g_n_methods = 0;
static const char *mid_name(void *mid) {
  uintptr_t id = (uintptr_t)mid - MID_BASE;
  return (id < (uintptr_t)g_n_methods) ? g_method_names[id] : "?";
}

static void *g_env_table[256];
static void *g_env = &g_env_table; /* JNIEnv = ponteiro p/ a tabela */
static void *g_vm_table[8];
static void *g_vm = &g_vm_table;

/* handles fake p/ classes/métodos/objetos (não-nulos p/ não NPE) */
#define FAKE_CLASS  ((void *)0x0C1A5500)
#define FAKE_METHOD ((void *)0x0E70D000)
#define FAKE_OBJ    ((void *)0x00B1EC00)

/* default: retorna 0 (serve p/ jobject/jint/jlong/void). */
static intptr_t jni_default(void *env, ...) {
  (void)env;
  return 0;
}
static int jni_GetVersion(void *env) { (void)env; return 0x00010006; /* JNI 1.6 */ }
static void *jni_FindClass(void *env, const char *name) {
  (void)env;
  debugPrintf("[JNI] FindClass(%s)\n", name ? name : "?");
  return FAKE_CLASS;
}
static void *jni_GetObjectClass(void *env, void *obj) {
  (void)env; (void)obj;
  return FAKE_CLASS;
}
static void *jni_GetMethodID(void *env, void *cls, const char *n, const char *sig) {
  (void)env; (void)cls;
  debugPrintf("[JNI] GetMethodID(%s, %s)\n", n ? n : "?", sig ? sig : "?");
  return FAKE_METHOD;
}
static void *jni_GetStaticMethodID(void *env, void *cls, const char *n, const char *sig) {
  (void)env; (void)cls; (void)sig;
  if (!n) return FAKE_METHOD;
  /* dedupe: mesmo nome -> mesmo id (a engine re-pede todo frame; sem isso estoura) */
  for (int i = 0; i < g_n_methods; i++)
    if (!strcmp(g_method_names[i], n))
      return (void *)(uintptr_t)(MID_BASE + i);
  debugPrintf("[JNI] GetStaticMethodID(%s, %s)\n", n, sig ? sig : "?");
  if (g_n_methods < 512) {
    g_method_names[g_n_methods] = strdup(n);
    return (void *)(uintptr_t)(MID_BASE + g_n_methods++);
  }
  return FAKE_METHOD;
}
static void *jni_GetFieldID(void *env, void *cls, const char *n, const char *sig) {
  (void)env; (void)cls;
  debugPrintf("[JNI] GetFieldID(%s, %s)\n", n ? n : "?", sig ? sig : "?");
  return FAKE_METHOD;
}
static void *jni_GetStaticFieldID(void *env, void *cls, const char *n, const char *sig) {
  (void)env; (void)cls;
  debugPrintf("[JNI] GetStaticFieldID(%s, %s)\n", n ? n : "?", sig ? sig : "?");
  return FAKE_METHOD;
}
static void *jni_CallObjectMethod(void *env, void *obj, void *mid, ...) {
  (void)env; (void)obj; (void)mid;
  debugPrintf("[JNI] CallObjectMethod\n");
  return FAKE_OBJ;
}
/* ---- estado consultado pela engine via CallStaticIntMethod ---- */
static int g_screen_w = 1280, g_screen_h = 720;
static int g_key_state = 0;
static long g_frame = 0;
/* Idioma resolvido de NXPORT_LANGUAGE na primeira leitura, compartilhado
 * entre getLanguage() e o prefixo lproj do OBB (obb_data.c). */
static int g_lang_resolved = 0;
static int g_lang_index = 1;      /* en */
static const char *g_lang_lproj = "en";
static void ff4a_resolve_language(void) {
  if (g_lang_resolved) return;
  ff_language sel = ff_language_select("pt"); /* FF4a: prefixo pt */
  g_lang_index = sel.index;
  g_lang_lproj = sel.lproj;
  g_lang_resolved = 1;
  debugPrintf("[ff4a] idioma index=%d (%s.lproj)\n", g_lang_index, g_lang_lproj);
}
const char *ff4a_language_lproj(void) { ff4a_resolve_language(); return g_lang_lproj; }
void jni_set_screen(int w, int h) { g_screen_w = w; g_screen_h = h; }
void jni_set_keystate(int mask) { g_key_state = mask; }
void jni_set_frame(long f) { g_frame = f; }

/* despacho compartilhado (a engine C++ chama a variante ...MethodV, arg0 já lido) */
static int dispatch_int(const char *nm) {
  if (!nm) return 0;
  if (!strcmp(nm, "getResWidth") || !strcmp(nm, "getViewWidth")) return g_screen_w;
  if (!strcmp(nm, "getResHeight") || !strcmp(nm, "getViewHeight")) return g_screen_h;
  if (!strcmp(nm, "getViewPosX") || !strcmp(nm, "getViewPosY")) return 0;
  if (!strcmp(nm, "getLanguage")) {
    ff4a_resolve_language();
    return g_lang_index;
  }
  if (!strcmp(nm, "getKeyEvent")) {
    static int last = -1, npoll = 0;
    npoll++;
    if (g_key_state != last) {
      debugPrintf("[JNI] getKeyEvent -> 0x%x (poll #%d)\n", g_key_state, npoll);
      last = g_key_state;
    }
    return g_key_state;
  }
  return 0;
}
/* getCurrentFrame(prev): NO JAVA ORIGINAL isso e' o FRAME-LIMITER do jogo —
 * bloqueia (yield) ate' o tick de wallclock a M Hz (setFPS; M=30 no clinit)
 * passar de prev, e retorna o tick absoluto. Replicar 1:1 e' o que corrige o
 * "jogo/audio acelerado": o render passa a rodar no fps que a ENGINE pede
 * (30), nao no vsync de 60. Delta>1 vira catch-up (engine capa em 3). */
/* Tela de nome: o After Years ainda NAO chama createEditText (medido), mas o
 * gancho fica pronto -- se chamar, o teclado virtual abre com o nome padrao
 * e getEditText devolve o que o jogador digitou. */
static char g_edit_text[128];

static int g_fps = 30;
int jni_get_fps(void) { return g_fps; }
static long do_getCurrentFrame(long prev) {
  for (;;) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long now = ((long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000) * g_fps / 1000;
    if (now != prev) return now;
    usleep(300);
  }
}
static long dispatch_long(const char *nm, long a0) {
  if (nm && !strcmp(nm, "getCurrentFrame")) return do_getCurrentFrame(a0);
  return 0;
}
/* ---- helpers (compartilhados entre variantes varargs/V/A) ---- */
static void *do_loadFile(const char *path) {
  if (!path) path = "";
  int len = 0;
  unsigned char *data = obb_load_file(path, &len);
  if (!data) { debugPrintf("[JNI] loadFile(\"%s\") -> NÃO ACHOU\n", path); return NULL; }
  return fa_new(data, len);
}
static void *do_loadSound(const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "sound/%s.akb", name ? name : "");
  int len = 0;
  unsigned char *d = obb_load_exact(path, &len);
  if (!d) { debugPrintf("[JNI] loadSound(\"%s\") -> NÃO ACHOU\n", name ? name : ""); return NULL; }
  return fa_new(d, len);
}
static const char *save_dir(void) {
  static char dir[512];
  if (!dir[0] && !getcwd(dir, sizeof(dir))) snprintf(dir, sizeof(dir), ".");
  return dir;
}
static void *do_getSaveFileName(void) {
  const char *dir = save_dir();
  int n = (int)strlen(dir);
  unsigned char *b = (unsigned char *)malloc(n ? n : 1);
  memcpy(b, dir, n);
  return fa_new(b, n);
}
/* createSaveFile(size) -> createFile("save.bin",size): cria save.bin com size zeros */
static void do_createSaveFile(int size) {
  char path[600];
  snprintf(path, sizeof(path), "%s/save.bin", save_dir());
  FILE *f = fopen(path, "wb");
  if (f) {
    if (size > 0) {
      void *z = calloc(size, 1);
      if (z) { fwrite(z, 1, size, f); free(z); }
    }
    fclose(f);
  }
  debugPrintf("[JNI] createSaveFile(%d) -> %s %s\n", size, path, f ? "OK" : "FALHOU");
}
static void *do_loadTexture(FakeArray *in) {
  if (!in || !in->data) return NULL;
  int cnt = 0;
  int *px = decode_texture(in->data, in->len, &cnt);
  if (!px) { debugPrintf("[JNI] loadTexture: decode falhou\n"); return NULL; }
  return fa_new((unsigned char *)px, cnt);
}
static void *do_drawFont(const char *text, int size, int ts, int bl) {
  int cnt = 0;
  int *px = draw_font(text, size, ts, bl, &cnt);
  if (!px) return NULL;
  return fa_new((unsigned char *)px, cnt);
}

/* variante varargs/V: args via va_list */
static void *dispatch_object(const char *nm, va_list ap) {
  if (!nm) return FAKE_OBJ;
  if (!strcmp(nm, "loadFile")) return do_loadFile(va_arg(ap, void *));
  if (!strcmp(nm, "loadSound")) return do_loadSound(va_arg(ap, void *));
  if (!strcmp(nm, "getSaveFileName")) return do_getSaveFileName();
  if (!strcmp(nm, "loadTexture")) return do_loadTexture((FakeArray *)va_arg(ap, void *));
  if (!strcmp(nm, "drawFont")) {
    const char *text = va_arg(ap, void *);
    int size = va_arg(ap, int), ts = va_arg(ap, int), bl = va_arg(ap, int);
    return do_drawFont(text, size, ts, bl);
  }
  if (!strcmp(nm, "getEditText")) {
    const char *t = vkbd_text();
    if (!t || !t[0]) t = g_edit_text;
    return (void *)t;  /* jstring fake = o proprio char* */
  }
  debugPrintf("[JNI] CallStaticObjectMethod %s\n", nm);
  return FAKE_OBJ;
}
/* variante A: args num array de jvalue (8 bytes cada) */
static void *dispatch_object_a(const char *nm, const uintptr_t *jv) {
  if (!nm) return FAKE_OBJ;
  if (!strcmp(nm, "getEditText")) {
    const char *t = vkbd_text();
    if (!t || !t[0]) t = g_edit_text;
    return (void *)t;
  }
  if (!strcmp(nm, "loadFile")) return do_loadFile((void *)jv[0]);
  if (!strcmp(nm, "loadSound")) return do_loadSound((void *)jv[0]);
  if (!strcmp(nm, "getSaveFileName")) return do_getSaveFileName();
  if (!strcmp(nm, "loadTexture")) return do_loadTexture((FakeArray *)jv[0]);
  if (!strcmp(nm, "drawFont"))
    return do_drawFont((void *)jv[0], (int)jv[1], (int)jv[2], (int)jv[3]);
  debugPrintf("[JNI] CallStaticObjectMethodA %s\n", nm);
  return FAKE_OBJ;
}

/* varargs (114/129/132/141) */
static int jni_CallStaticIntMethod(void *e, void *c, void *mid, ...) {
  (void)e; (void)c; return dispatch_int(mid_name(mid));
}
static long jni_CallStaticLongMethod(void *e, void *c, void *mid, ...) {
  (void)e; (void)c;
  va_list ap; va_start(ap, mid);
  long a0 = va_arg(ap, long); va_end(ap);
  return dispatch_long(mid_name(mid), a0);
}
/* assignBackButton(k): a engine anuncia o modo do botao BACK do Android.
 * k==0 desliga (e o Java zerava o estado), k==1 -> bit 0x8000, k>=2 -> 0x4000.
 * Guardamos o modo p/ o main.c mapear o B do pad no bit certo (menus mobile
 * so' voltam com esse bit — o isPadCancel/K_B nao cobre as telas touch). */
static int g_back_mode = 0;
int jni_get_back_mode(void) { return g_back_mode; }

static void dispatch_void(const char *nm, int arg0) {
  if (nm && !strcmp(nm, "createSaveFile")) do_createSaveFile(arg0);
  if (nm && !strcmp(nm, "assignBackButton")) {
    if (arg0 != g_back_mode)
      debugPrintf("[JNI] assignBackButton(%d)\n", arg0);
    g_back_mode = arg0;
  }
  if (nm && !strcmp(nm, "setFPS")) {
    if (arg0 > 0 && arg0 != g_fps) {
      debugPrintf("[JNI] setFPS(%d)\n", arg0);
      g_fps = arg0;
    }
  }
}
/* createEditText(String nomePadrao, int max): abre o teclado virtual com o
 * nome padrao. Vem pelas variantes varargs/V/A -- tratado aqui, uma vez. */
static void ff4a_create_edit_text(const char *def, int maxlen) {
  if (!def) def = "";
  snprintf(g_edit_text, sizeof g_edit_text, "%s", def);
  vkbd_activate(def, maxlen > 0 ? maxlen : 8);
  debugPrintf("[edit] createEditText nome padrao=\"%s\" max=%d\n",
              g_edit_text, maxlen);
}

static void jni_CallStaticVoidMethod(void *e, void *c, void *mid, ...) {
  (void)e; (void)c;
  const char *nm = mid_name(mid);
  va_list ap; va_start(ap, mid);
  if (nm && !strcmp(nm, "createEditText")) {  /* (String, int): arg0 e' ponteiro */
    const char *def = va_arg(ap, const char *);
    int maxlen = va_arg(ap, int);
    va_end(ap);
    ff4a_create_edit_text(def, maxlen);
    return;
  }
  int a0 = va_arg(ap, int); va_end(ap);
  dispatch_void(nm, a0);
}
static void *jni_CallStaticObjectMethod(void *e, void *c, void *mid, ...) {
  (void)e; (void)c;
  va_list ap; va_start(ap, mid);
  void *r = dispatch_object(mid_name(mid), ap);
  va_end(ap);
  return r;
}
/* variante V (115/130/133/142) — é a que a engine C++ realmente chama */
static int jni_CallStaticIntMethodV(void *e, void *c, void *mid, va_list ap) {
  (void)e; (void)c; (void)ap; return dispatch_int(mid_name(mid));
}
static long jni_CallStaticLongMethodV(void *e, void *c, void *mid, va_list ap) {
  (void)e; (void)c;
  long a0 = va_arg(ap, long);
  return dispatch_long(mid_name(mid), a0);
}
static void jni_CallStaticVoidMethodV(void *e, void *c, void *mid, va_list ap) {
  (void)e; (void)c;
  const char *nm = mid_name(mid);
  if (nm && !strcmp(nm, "createEditText")) {
    const char *def = va_arg(ap, const char *);
    int maxlen = va_arg(ap, int);
    ff4a_create_edit_text(def, maxlen);
    return;
  }
  int a0 = va_arg(ap, int);
  dispatch_void(nm, a0);
}
static void *jni_CallStaticObjectMethodV(void *e, void *c, void *mid, va_list ap) {
  (void)e; (void)c;
  return dispatch_object(mid_name(mid), ap);
}
/* variante A (116/131/134/143) — args em jvalue[] */
static int jni_CallStaticIntMethodA(void *e, void *c, void *mid, const void *a) {
  (void)e; (void)c; (void)a; return dispatch_int(mid_name(mid));
}
static long jni_CallStaticLongMethodA(void *e, void *c, void *mid, const void *a) {
  (void)e; (void)c;
  long a0 = a ? (long)((const uintptr_t *)a)[0] : 0;
  return dispatch_long(mid_name(mid), a0);
}
static void jni_CallStaticVoidMethodA(void *e, void *c, void *mid, const void *a) {
  (void)e; (void)c;
  const char *nm = mid_name(mid);
  const uintptr_t *jv = (const uintptr_t *)a;
  if (nm && !strcmp(nm, "createEditText") && jv) {
    ff4a_create_edit_text((const char *)jv[0], (int)jv[1]);
    return;
  }
  int a0 = jv ? (int)jv[0] : 0;
  dispatch_void(nm, a0);
}
static void *jni_CallStaticObjectMethodA(void *e, void *c, void *mid, const void *a) {
  (void)e; (void)c;
  return dispatch_object_a(mid_name(mid), (const uintptr_t *)a);
}
static void *jni_NewStringUTF(void *env, const char *s) {
  (void)env;
  return (void *)(s ? s : ""); /* jstring fake = o próprio char* */
}
static const char *jni_GetStringUTFChars(void *env, void *jstr, unsigned char *isCopy) {
  (void)env;
  if (isCopy) *isCopy = 0;
  return (const char *)jstr; /* casamos com NewStringUTF acima */
}
static void jni_ReleaseStringUTFChars(void *env, void *jstr, const char *s) {
  (void)env; (void)jstr; (void)s;
}
static int jni_GetStringUTFLength(void *env, void *jstr) {
  (void)env;
  return jstr ? (int)strlen((const char *)jstr) : 0;
}
static unsigned char jni_ExceptionCheck(void *env) { (void)env; return 0; }
static void *jni_ExceptionOccurred(void *env) { (void)env; return NULL; }

/* ---- byte[] (jbyteArray = FakeArray*) ---- */
static int jni_GetArrayLength(void *env, void *arr) {
  (void)env;
  return arr ? ((FakeArray *)arr)->len : 0;
}
static void *jni_NewByteArray(void *env, int len) {
  (void)env;
  unsigned char *d = (unsigned char *)calloc(len ? len : 1, 1);
  return fa_new(d, len);
}
static void *jni_GetByteArrayElements(void *env, void *arr, unsigned char *isCopy) {
  (void)env;
  if (isCopy) *isCopy = 0;
  return arr ? ((FakeArray *)arr)->data : NULL;
}
static void jni_ReleaseByteArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)elems; (void)mode;
  int i = fa_index(arr);
  if (i >= 0) fa_free(i);
}
static void jni_GetByteArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  if (!arr || !buf) return;
  FakeArray *fa = (FakeArray *)arr;
  if (start < 0 || len < 0 || start + len > fa->len) return;
  memcpy(buf, fa->data + start, len);
}
static void jni_SetByteArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  if (!arr || !buf) return;
  FakeArray *fa = (FakeArray *)arr;
  if (start < 0 || len < 0 || start + len > fa->len) return;
  memcpy(fa->data + start, buf, len);
}
static void *jni_GetPrimitiveArrayCritical(void *env, void *arr, unsigned char *isCopy) {
  (void)env;
  if (isCopy) *isCopy = 0;
  return arr ? ((FakeArray *)arr)->data : NULL;
}
static void jni_ReleasePrimitiveArrayCritical(void *env, void *arr, void *c, int mode) {
  (void)env; (void)c; (void)mode;
  int i = fa_index(arr);
  if (i >= 0) fa_free(i);
}
static void jni_DeleteLocalRef(void *env, void *ref) {
  (void)env;
  int i = fa_index(ref); /* só libera se for um FakeArray nosso (registro) */
  if (i >= 0) fa_free(i);
}

/* ---- int[] (jintArray = FakeArray*, len = nº de ints, data = buffer de ints) ---- */
static void *jni_NewIntArray(void *env, int len) {
  (void)env;
  int *d = (int *)calloc(len ? len : 1, sizeof(int));
  return fa_new((unsigned char *)d, len);
}
static void *jni_GetIntArrayElements(void *env, void *arr, unsigned char *isCopy) {
  (void)env;
  if (isCopy) *isCopy = 0;
  return arr ? ((FakeArray *)arr)->data : NULL;
}
static void jni_ReleaseIntArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)elems; (void)mode;
  int i = fa_index(arr);
  if (i >= 0) fa_free(i);
}
static void jni_GetIntArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  if (!arr || !buf) return;
  FakeArray *fa = (FakeArray *)arr;
  if (start < 0 || len < 0 || start + len > fa->len) return;
  memcpy(buf, fa->data + (size_t)start * 4, (size_t)len * 4);
}

/* JavaVM: GetEnv / AttachCurrentThread devolvem nosso env */
static int vm_GetEnv(void *vm, void **penv, int ver) {
  (void)vm; (void)ver;
  *penv = &g_env; /* JNIEnv = duplo-ponteiro (&g_env), não g_env */
  return 0;       /* JNI_OK */
}
static int vm_AttachCurrentThread(void *vm, void **penv, void *args) {
  (void)vm; (void)args;
  *penv = &g_env;
  return 0;
}

void jni_shim_init(void **out_vm, void **out_env) {
  for (int i = 0; i < 256; i++)
    g_env_table[i] = (void *)jni_default;
  /* índices conforme jni.h (JNINativeInterface) */
  g_env_table[4] = (void *)jni_GetVersion;
  g_env_table[6] = (void *)jni_FindClass;
  g_env_table[15] = (void *)jni_ExceptionOccurred;
  g_env_table[31] = (void *)jni_GetObjectClass;
  g_env_table[33] = (void *)jni_GetMethodID;
  g_env_table[34] = (void *)jni_CallObjectMethod;
  g_env_table[94] = (void *)jni_GetFieldID;
  g_env_table[113] = (void *)jni_GetStaticMethodID;
  g_env_table[114] = (void *)jni_CallStaticObjectMethod;
  g_env_table[115] = (void *)jni_CallStaticObjectMethodV;
  g_env_table[116] = (void *)jni_CallStaticObjectMethodA;
  g_env_table[129] = (void *)jni_CallStaticIntMethod;
  g_env_table[130] = (void *)jni_CallStaticIntMethodV;
  g_env_table[131] = (void *)jni_CallStaticIntMethodA;
  g_env_table[132] = (void *)jni_CallStaticLongMethod;
  g_env_table[133] = (void *)jni_CallStaticLongMethodV;
  g_env_table[134] = (void *)jni_CallStaticLongMethodA;
  g_env_table[141] = (void *)jni_CallStaticVoidMethod;
  g_env_table[142] = (void *)jni_CallStaticVoidMethodV;
  g_env_table[143] = (void *)jni_CallStaticVoidMethodA;
  g_env_table[144] = (void *)jni_GetStaticFieldID;
  g_env_table[23] = (void *)jni_DeleteLocalRef;
  g_env_table[167] = (void *)jni_NewStringUTF;
  g_env_table[169] = (void *)jni_GetStringUTFChars;
  g_env_table[170] = (void *)jni_ReleaseStringUTFChars;
  g_env_table[168] = (void *)jni_GetStringUTFLength;
  /* byte[] (âncora: NewStringUTF=167 confirmado) */
  g_env_table[171] = (void *)jni_GetArrayLength;
  g_env_table[176] = (void *)jni_NewByteArray;
  g_env_table[184] = (void *)jni_GetByteArrayElements;
  g_env_table[192] = (void *)jni_ReleaseByteArrayElements;
  g_env_table[200] = (void *)jni_GetByteArrayRegion;
  g_env_table[208] = (void *)jni_SetByteArrayRegion;
  /* int[] */
  g_env_table[179] = (void *)jni_NewIntArray;
  g_env_table[187] = (void *)jni_GetIntArrayElements;
  g_env_table[195] = (void *)jni_ReleaseIntArrayElements;
  g_env_table[203] = (void *)jni_GetIntArrayRegion;
  g_env_table[222] = (void *)jni_GetPrimitiveArrayCritical;
  g_env_table[223] = (void *)jni_ReleasePrimitiveArrayCritical;
  g_env_table[228] = (void *)jni_ExceptionCheck;

  for (int i = 0; i < 8; i++)
    g_vm_table[i] = (void *)jni_default;
  g_vm_table[4] = (void *)vm_AttachCurrentThread; /* slot 4 = AttachCurrentThread */
  g_vm_table[6] = (void *)vm_GetEnv;              /* slot 6 = GetEnv */

  if (out_vm) *out_vm = &g_vm;
  if (out_env) *out_env = &g_env;
}
