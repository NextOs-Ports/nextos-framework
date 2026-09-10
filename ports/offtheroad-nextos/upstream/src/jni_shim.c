/*
 * jni_shim.c -- JNIEnv falso para Cocos2d-x (Happy Wheels).
 */
#include "so_util.h"
#include "jni_shim.h"
#include "util.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define JNI_VTABLE_SIZE 512

typedef int jint;
typedef unsigned char jboolean;

/* renderizador de texto (text_render.c) */
extern unsigned char *chrono_render_text(const char *utf8, int px, int r, int g, int b,
                                         int align, int reqW, int reqH, int *outW, int *outH);
/* nativeInitBitmapDC do libchrono. A desmontagem mostra que o byte[] usado no
   GetByteArrayRegion vem do 3o arg (x4) -> passamos o array em x4 E no ultimo. */
static void (*g_nativeInitBitmapDC)(void *, void *, int, int, void *, void *, void *) = NULL;

static uintptr_t jni_env_vtable[JNI_VTABLE_SIZE];
static void *jni_env_ptr;
static uintptr_t java_vm_vtable[JNI_VTABLE_SIZE];
static void *java_vm_ptr;

/* Caminhos derivados do diretorio REAL do port (ct_gamedir), nunca cravados:
   a raiz de ROM muda por CFW (/roms, /roms2, /storage/roms, /mnt/mmc...). */
static char g_writable_path[512] = "./userdata/";
static char g_assets_path[512] = "./assets/";
void jni_set_writable_path(const char *path) {
  if (path) { strncpy(g_writable_path, path, sizeof(g_writable_path) - 1);
              g_writable_path[sizeof(g_writable_path) - 1] = 0; }
}
void jni_set_assets_path(const char *path) {
  if (path) { strncpy(g_assets_path, path, sizeof(g_assets_path) - 1);
              g_assets_path[sizeof(g_assets_path) - 1] = 0; }
}

/* ---- UserDefault (SharedPreferences do Android) --------------------------
 * O cocos2d-x no Android nao usa UserDefault.xml: ele chama
 * Cocos2dxHelper.get/setXxxForKey, que no aparelho vai pro SharedPreferences.
 * Sem isso nada persiste (termos aceitos, progresso, opcoes) e o jogo repete
 * a tela de privacidade e os avisos iniciais a cada boot.
 * Guardamos tudo num prefs.txt (uma linha "chave=valor", \n escapado). */
#define MAX_PREFS 512
static struct { char *key; char *val; } g_prefs[MAX_PREFS];
static int g_prefs_n = 0;
static int g_prefs_loaded = 0;
static char g_prefs_file[640];

static void prefs_unescape(char *s) {
  char *r = s, *w = s;
  while (*r) {
    if (r[0] == '\\' && r[1] == 'n') { *w++ = '\n'; r += 2; }
    else if (r[0] == '\\' && r[1] == '\\') { *w++ = '\\'; r += 2; }
    else *w++ = *r++;
  }
  *w = 0;
}

static void prefs_load(void) {
  if (g_prefs_loaded) return;
  g_prefs_loaded = 1;
  snprintf(g_prefs_file, sizeof(g_prefs_file), "%sprefs.txt", g_writable_path);
  FILE *f = fopen(g_prefs_file, "r");
  if (!f) {
    debugPrintf("[prefs] %s ainda nao existe (primeiro boot)\n", g_prefs_file);
    return;
  }
  char line[4096];
  while (fgets(line, sizeof(line), f)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char *val = eq + 1;
    prefs_unescape(val);
    if (g_prefs_n < MAX_PREFS) {
      g_prefs[g_prefs_n].key = strdup(line);
      g_prefs[g_prefs_n].val = strdup(val);
      g_prefs_n++;
    }
  }
  fclose(f);
  debugPrintf("[prefs] %d chaves carregadas de %s\n", g_prefs_n, g_prefs_file);
}

static void prefs_save(void) {
  FILE *f = fopen(g_prefs_file, "w");
  if (!f) { debugPrintf("[prefs] ERRO ao gravar %s\n", g_prefs_file); return; }
  for (int i = 0; i < g_prefs_n; i++) {
    fputs(g_prefs[i].key, f);
    fputc('=', f);
    for (const char *c = g_prefs[i].val; *c; c++) {
      if (*c == '\n') { fputc('\\', f); fputc('n', f); }
      else if (*c == '\\') { fputc('\\', f); fputc('\\', f); }
      else fputc(*c, f);
    }
    fputc('\n', f);
  }
  fclose(f);
}

static const char *prefs_get(const char *key, const char *def) {
  prefs_load();
  if (!key) return def;
  for (int i = 0; i < g_prefs_n; i++)
    if (strcmp(g_prefs[i].key, key) == 0) return g_prefs[i].val;
  return def;
}

static void prefs_set(const char *key, const char *val) {
  prefs_load();
  if (!key || !val) return;
  for (int i = 0; i < g_prefs_n; i++) {
    if (strcmp(g_prefs[i].key, key) == 0) {
      if (strcmp(g_prefs[i].val, val) == 0) return;
      free(g_prefs[i].val);
      g_prefs[i].val = strdup(val);
      prefs_save();
      return;
    }
  }
  if (g_prefs_n >= MAX_PREFS) return;
  g_prefs[g_prefs_n].key = strdup(key);
  g_prefs[g_prefs_n].val = strdup(val);
  g_prefs_n++;
  prefs_save();
}

static void prefs_delete(const char *key) {
  prefs_load();
  if (!key) return;
  for (int i = 0; i < g_prefs_n; i++) {
    if (strcmp(g_prefs[i].key, key) == 0) {
      free(g_prefs[i].key); free(g_prefs[i].val);
      for (int j = i; j < g_prefs_n - 1; j++) g_prefs[j] = g_prefs[j + 1];
      g_prefs_n--;
      prefs_save();
      return;
    }
  }
}

/* ---- tabela de jstrings falsas ---- */
#define MAX_JSTRINGS 256
static struct { void *handle; const char *value; } g_jstrings[MAX_JSTRINGS];
static int g_jstring_count = 0;

void *jni_make_string(const char *value) {
  if (g_jstring_count >= MAX_JSTRINGS) g_jstring_count = 0;
  int idx = g_jstring_count++;
  g_jstrings[idx].handle = (void *)((uintptr_t)0x10000 + idx);
  g_jstrings[idx].value = value ? value : "";
  return g_jstrings[idx].handle;
}
static const char *resolve_jstring(void *jstr) {
  for (int i = 0; i < g_jstring_count; i++)
    if (g_jstrings[i].handle == jstr) return g_jstrings[i].value;
  return "";
}

/* ---- byte[] reais (texto de entrada + pixels de saida p/ createTextBitmap) ---- */
#define MAX_JARRAYS 64
static struct { void *handle; unsigned char *data; int len; int owned; } g_jarr[MAX_JARRAYS];
static int g_jarr_n = 0;
static void *jarr_new(int len, int owned, unsigned char *existing) {
  int idx = -1;
  for (int i = 0; i < g_jarr_n; i++) if (!g_jarr[i].handle) { idx = i; break; }
  if (idx < 0) { if (g_jarr_n >= MAX_JARRAYS) g_jarr_n = 0; idx = g_jarr_n++; }
  g_jarr[idx].data = existing ? existing : (len > 0 ? calloc(len, 1) : NULL);
  g_jarr[idx].len = len;
  g_jarr[idx].owned = owned;
  g_jarr[idx].handle = (void *)((uintptr_t)0x20000 + idx);
  return g_jarr[idx].handle;
}
static int jarr_find(void *h) {
  for (int i = 0; i < g_jarr_n; i++) if (g_jarr[i].handle == h) return i;
  return -1;
}
static void jarr_free(void *h) {
  int i = jarr_find(h); if (i < 0) return;
  if (g_jarr[i].owned && g_jarr[i].data) free(g_jarr[i].data);
  g_jarr[i].handle = NULL; g_jarr[i].data = NULL; g_jarr[i].len = 0;
}

/* ---- tags de metodos ---- */
enum {
  MID_GENERIC = 0,
  MID_WRITABLE_PATH, MID_CURRENT_LANGUAGE, MID_CURRENT_LANGUAGE_CODE,
  MID_PACKAGE_NAME, MID_DEVICE_MODEL, MID_OBB_PATH, MID_ASSETS_PATH,
  MID_VERSION, MID_DPI, MID_EXIT, MID_OPENGL_VERSION,
  MID_CREATE_TEXT_BITMAP, MID_LOCATION_CODE,
  MID_PREF_GET_BOOL, MID_PREF_SET_BOOL,
  MID_PREF_GET_INT, MID_PREF_SET_INT,
  MID_PREF_GET_FLOAT, MID_PREF_SET_FLOAT,
  MID_PREF_GET_DOUBLE, MID_PREF_SET_DOUBLE,
  MID_PREF_GET_STRING, MID_PREF_SET_STRING,
  MID_PREF_DELETE,
  MID_TAG_COUNT
};
static int g_method_tags[MID_TAG_COUNT];
static int g_dummy_array[4096];

static intptr_t jni_stub(void) { return 0; }
static jint jni_GetVersion(void *env) { return 0x00010006; }

static int g_jni_log = 0;
static void *jni_FindClass(void *env, const char *name) {
  if (g_jni_log) debugPrintf("JNI FindClass(%s)\n", name);
  static int fake_class; return &fake_class;
}

/* casa por substring do nome do metodo (independe de ser static ou nao) */
static int tag_for_method(const char *name) {
  if (!name) return MID_GENERIC;
  if (strstr(name, "createTextBitmap")) return MID_CREATE_TEXT_BITMAP;
  if (strcmp(name, "getBoolForKey") == 0) return MID_PREF_GET_BOOL;
  if (strcmp(name, "setBoolForKey") == 0) return MID_PREF_SET_BOOL;
  if (strcmp(name, "getIntegerForKey") == 0) return MID_PREF_GET_INT;
  if (strcmp(name, "setIntegerForKey") == 0) return MID_PREF_SET_INT;
  if (strcmp(name, "getFloatForKey") == 0) return MID_PREF_GET_FLOAT;
  if (strcmp(name, "setFloatForKey") == 0) return MID_PREF_SET_FLOAT;
  if (strcmp(name, "getDoubleForKey") == 0) return MID_PREF_GET_DOUBLE;
  if (strcmp(name, "setDoubleForKey") == 0) return MID_PREF_SET_DOUBLE;
  if (strcmp(name, "getStringForKey") == 0) return MID_PREF_GET_STRING;
  if (strcmp(name, "setStringForKey") == 0) return MID_PREF_SET_STRING;
  if (strcmp(name, "deleteValueForKey") == 0) return MID_PREF_DELETE;
  if (strstr(name, "getLocationCode") || strstr(name, "LocationCode")) return MID_LOCATION_CODE;
  if (strstr(name, "WritablePath")) return MID_WRITABLE_PATH;
  if (strcmp(name, "getCurrentLanguage") == 0) return MID_CURRENT_LANGUAGE;
  if (strstr(name, "getCurrentLanguageCode") || strstr(name, "LanguageCode")) return MID_CURRENT_LANGUAGE_CODE;
  if (strstr(name, "PackageName")) return MID_PACKAGE_NAME;
  if (strstr(name, "DeviceModel")) return MID_DEVICE_MODEL;
  if (strstr(name, "ObbFilePath") || strstr(name, "getObbFile")) return MID_OBB_PATH;
  if (strstr(name, "AssetsPath") || strstr(name, "getAssets")) return MID_ASSETS_PATH;
  if (strstr(name, "getVersion")) return MID_VERSION;
  if (strstr(name, "DPI") || strstr(name, "getDPI")) return MID_DPI;
  if (strstr(name, "OpenGLVersion") || strstr(name, "GLVersion")) return MID_OPENGL_VERSION;
  if (strcmp(name, "exit") == 0 || strstr(name, "terminateProcess")) return MID_EXIT;
  return MID_GENERIC;
}
static void *jni_GetMethodID(void *env, void *clazz, const char *name, const char *sig) {
  if (g_jni_log) debugPrintf("JNI GetMethodID(%s, %s)\n", name, sig);
  return &g_method_tags[tag_for_method(name)];
}
static void *jni_GetStaticMethodID(void *env, void *clazz, const char *name, const char *sig) {
  if (g_jni_log) debugPrintf("JNI GetStaticMethodID(%s, %s)\n", name, sig);
  return &g_method_tags[tag_for_method(name)];
}

static void *string_for_tag(void *methodID) {
  if (methodID == &g_method_tags[MID_WRITABLE_PATH]) return jni_make_string(g_writable_path);
  if (methodID == &g_method_tags[MID_CURRENT_LANGUAGE]) return jni_make_string("English");
  if (methodID == &g_method_tags[MID_CURRENT_LANGUAGE_CODE]) return jni_make_string("en");
  if (methodID == &g_method_tags[MID_PACKAGE_NAME]) return jni_make_string("com.dogbytegames.offtheroad");
  if (methodID == &g_method_tags[MID_DEVICE_MODEL]) return jni_make_string("nextos");
  if (methodID == &g_method_tags[MID_OBB_PATH]) return jni_make_string("");
  if (methodID == &g_method_tags[MID_ASSETS_PATH]) return jni_make_string(g_assets_path);
  if (methodID == &g_method_tags[MID_VERSION]) return jni_make_string("1.1.3");
  if (methodID == &g_method_tags[MID_OPENGL_VERSION]) return jni_make_string("OpenGL ES 2.0");
  return jni_make_string("");
}


/* ---- ponte UserDefault <-> prefs.txt ---- */
static int g_prefs_log = 0;
static jboolean pref_get_bool(void *jkey, int def) {
  if (g_prefs_log) debugPrintf("[prefs] getBool jkey=%p -> \"%s\" (def=%d)\n", jkey, resolve_jstring(jkey), def);
  const char *v = prefs_get(resolve_jstring(jkey), NULL);
  if (!v) return (jboolean)(def ? 1 : 0);
  return (jboolean)((strcmp(v, "true") == 0 || strcmp(v, "1") == 0) ? 1 : 0);
}
static void pref_set_bool(void *jkey, int val) {
  if (g_prefs_log) debugPrintf("[prefs] setBool jkey=%p -> \"%s\" = %d\n", jkey, resolve_jstring(jkey), val);
  prefs_set(resolve_jstring(jkey), val ? "true" : "false");
}
static jint pref_get_int(void *jkey, jint def) {
  const char *v = prefs_get(resolve_jstring(jkey), NULL);
  return v ? (jint)atoi(v) : def;
}
static void pref_set_int(void *jkey, jint val) {
  char buf[32]; snprintf(buf, sizeof(buf), "%d", (int)val);
  prefs_set(resolve_jstring(jkey), buf);
}
static double pref_get_num(void *jkey, double def) {
  const char *v = prefs_get(resolve_jstring(jkey), NULL);
  return v ? atof(v) : def;
}
static void pref_set_num(void *jkey, double val) {
  char buf[64]; snprintf(buf, sizeof(buf), "%.17g", val);
  prefs_set(resolve_jstring(jkey), buf);
}
static void *pref_get_string(void *jkey, void *jdef) {
  const char *v = prefs_get(resolve_jstring(jkey), NULL);
  if (!v) return jdef ? jdef : jni_make_string("");
  return jni_make_string(strdup(v));
}
static void pref_set_string(void *jkey, void *jval) {
  prefs_set(resolve_jstring(jkey), resolve_jstring(jval));
}

static void *jni_CallObjectMethod(void *env, void *obj, void *methodID, ...) {
  return string_for_tag(methodID);
}
static void *jni_CallStaticObjectMethod(void *env, void *clazz, void *methodID, ...) {
  if (methodID == &g_method_tags[MID_PREF_GET_STRING]) {
    va_list ap; va_start(ap, methodID);
    void *k = va_arg(ap, void *); void *d = va_arg(ap, void *);
    va_end(ap);
    return pref_get_string(k, d);
  }
  return string_for_tag(methodID);
}
static void *jni_CallStaticObjectMethodV(void *env, void *clazz, void *methodID, va_list a) {
  if (methodID == &g_method_tags[MID_PREF_GET_STRING]) {
    void *k = va_arg(a, void *); void *d = va_arg(a, void *);
    return pref_get_string(k, d);
  }
  return string_for_tag(methodID);
}
static void *jni_CallStaticObjectMethodA(void *env, void *clazz, void *methodID, const void *args) {
  if (methodID == &g_method_tags[MID_PREF_GET_STRING]) {
    const uint64_t *a = (const uint64_t *)args;
    return pref_get_string((void *)a[0], (void *)a[1]);
  }
  return string_for_tag(methodID);
}

static jboolean jni_CallBooleanMethod(void *env, void *obj, void *mid, ...) { return 0; }
/* renderiza texto do byte[] e alimenta nativeInitBitmapDC. retorna 1 se ok. */
static jboolean do_create_text_bitmap(void *textArr, void *fontStr, int fontSize,
                                      int tintR, int tintG, int tintB, int align,
                                      int width, int height) {
  int ai = jarr_find(textArr);
  if (g_jni_log) debugPrintf("createText textArr=%p ai=%d\n", textArr, ai);
  if (ai < 0 || !g_jarr[ai].data) return 0;
  int tlen = g_jarr[ai].len;
  char *txt = malloc(tlen + 1);
  if (!txt) return 0;
  memcpy(txt, g_jarr[ai].data, tlen);
  txt[tlen] = 0;
  if (getenv("HW_TEXTLOG"))
    debugPrintf("TEXT '%s' font='%s' size=%d rgb=%d,%d,%d align=%d %dx%d\n",
                txt, resolve_jstring(fontStr), fontSize, tintR, tintG, tintB, align, width, height);
  int W = 0, Hh = 0;
  unsigned char *rgba = chrono_render_text(txt, fontSize, tintR & 255, tintG & 255,
                                           tintB & 255, align, width, height, &W, &Hh);
  free(txt);
  if (!rgba || W <= 0 || Hh <= 0) { if (rgba) free(rgba); return 0; }
  if (!g_nativeInitBitmapDC)
    g_nativeInitBitmapDC = (void *)so_find_addr_safe(
        "Java_org_cocos2dx_lib_Cocos2dxBitmap_nativeInitBitmapDC");
  if (!g_nativeInitBitmapDC) { free(rgba); return 0; }
  void *parr = jarr_new(W * Hh * 4, 1, rgba);
  g_nativeInitBitmapDC(&jni_env_ptr, NULL, W, Hh, parr, fontStr, parr);
  jarr_free(parr);
  return 1;
}

/* variante varargs (...) */
static jboolean jni_CallStaticBooleanMethod(void *env, void *clazz, void *mid, ...) {
  if (mid == &g_method_tags[MID_PREF_GET_BOOL]) {
    va_list ap; va_start(ap, mid);
    void *k = va_arg(ap, void *); int d = va_arg(ap, int);
    va_end(ap);
    return pref_get_bool(k, d);
  }
  if (mid == &g_method_tags[MID_CREATE_TEXT_BITMAP]) {
    va_list ap; va_start(ap, mid);
    void *textArr = va_arg(ap, void *);
    void *fontStr = va_arg(ap, void *);
    int fontSize = va_arg(ap, int), tintR = va_arg(ap, int), tintG = va_arg(ap, int),
        tintB = va_arg(ap, int), align = va_arg(ap, int), width = va_arg(ap, int),
        height = va_arg(ap, int);
    va_end(ap);
    return do_create_text_bitmap(textArr, fontStr, fontSize, tintR, tintG, tintB, align, width, height);
  }
  return 0;
}
/* variante A: args = jvalue[] (8 bytes cada). E' a que o cocos usa aqui. */
static jboolean jni_CallStaticBooleanMethodA(void *env, void *clazz, void *mid, const void *args) {
  if (mid == &g_method_tags[MID_PREF_GET_BOOL]) {
    const uint64_t *a = (const uint64_t *)args;
    return pref_get_bool((void *)a[0], (int)a[1]);
  }
  if (mid == &g_method_tags[MID_CREATE_TEXT_BITMAP]) {
    const uint64_t *a = (const uint64_t *)args;
    if (getenv("CHRONO_TEXTLOG"))
      debugPrintf("RAWARGS i2..i11 = %d %d %d %d %d %d %d %d %d %d\n",
                  (int)a[2],(int)a[3],(int)a[4],(int)a[5],(int)a[6],
                  (int)a[7],(int)a[8],(int)a[9],(int)a[10],(int)a[11]);
    /* a[2]=fontSize a[3,4,5]=RGB. width/height passados sao ruins (17/0) ->
       auto-dimensiona (0,0) e alinha left; o jogo posiciona o label. */
    return do_create_text_bitmap((void *)a[0], (void *)a[1], (int)a[2], (int)a[3],
                                 (int)a[4], (int)a[5], 0, 0, 0);
  }
  return 0;
}
/* variante V: va_list */
static jboolean jni_CallStaticBooleanMethodV(void *env, void *clazz, void *mid, va_list ap) {
  if (mid == &g_method_tags[MID_PREF_GET_BOOL]) {
    void *k = va_arg(ap, void *); int d = va_arg(ap, int);
    return pref_get_bool(k, d);
  }
  if (mid == &g_method_tags[MID_CREATE_TEXT_BITMAP]) {
    void *textArr = va_arg(ap, void *);
    void *fontStr = va_arg(ap, void *);
    int fontSize = va_arg(ap, int), tintR = va_arg(ap, int), tintG = va_arg(ap, int),
        tintB = va_arg(ap, int), align = va_arg(ap, int), width = va_arg(ap, int),
        height = va_arg(ap, int);
    return do_create_text_bitmap(textArr, fontStr, fontSize, tintR, tintG, tintB, align, width, height);
  }
  return 0;
}

static jint jni_CallIntMethod(void *env, void *obj, void *mid, ...) {
  if (mid == &g_method_tags[MID_DPI]) return 160;
  return 0;
}
static jint jni_CallStaticIntMethod(void *env, void *clazz, void *mid, ...) {
  if (mid == &g_method_tags[MID_PREF_GET_INT]) {
    va_list ap; va_start(ap, mid);
    void *k = va_arg(ap, void *); jint d = (jint)va_arg(ap, int);
    va_end(ap);
    return pref_get_int(k, d);
  }
  if (mid == &g_method_tags[MID_DPI]) return 160;
  if (mid == &g_method_tags[MID_LOCATION_CODE]) {
    const char *e = getenv("CHRONO_LOC");
    int v = e ? atoi(e) : 1;   /* regiao: 0=Japao -> usar !=0 p/ ingles */
    if (g_jni_log) debugPrintf("JNI getLocationCode -> %d\n", v);
    return v;
  }
  return 0;
}

static jint jni_CallStaticIntMethodV(void *env, void *clazz, void *mid, va_list ap) {
  if (mid == &g_method_tags[MID_PREF_GET_INT]) {
    void *k = va_arg(ap, void *); jint d = (jint)va_arg(ap, int);
    return pref_get_int(k, d);
  }
  if (mid == &g_method_tags[MID_DPI]) return 160;
  return 0;
}
static jint jni_CallStaticIntMethodA(void *env, void *clazz, void *mid, const void *args) {
  if (mid == &g_method_tags[MID_PREF_GET_INT]) {
    const uint64_t *a = (const uint64_t *)args;
    return pref_get_int((void *)a[0], (jint)(int32_t)a[1]);
  }
  if (mid == &g_method_tags[MID_DPI]) return 160;
  return 0;
}

static float jni_CallStaticFloatMethod(void *env, void *clazz, void *mid, ...) {
  if (mid == &g_method_tags[MID_PREF_GET_FLOAT]) {
    va_list ap; va_start(ap, mid);
    void *k = va_arg(ap, void *); double d = va_arg(ap, double);
    va_end(ap);
    return (float)pref_get_num(k, d);
  }
  return 0.0f;
}
static float jni_CallStaticFloatMethodV(void *env, void *clazz, void *mid, va_list ap) {
  if (mid == &g_method_tags[MID_PREF_GET_FLOAT]) {
    void *k = va_arg(ap, void *); double d = va_arg(ap, double);
    return (float)pref_get_num(k, d);
  }
  return 0.0f;
}
static float jni_CallStaticFloatMethodA(void *env, void *clazz, void *mid, const void *args) {
  if (mid == &g_method_tags[MID_PREF_GET_FLOAT]) {
    const uint64_t *a = (const uint64_t *)args;
    float d; memcpy(&d, &a[1], sizeof(float));
    return (float)pref_get_num((void *)a[0], d);
  }
  return 0.0f;
}

static double jni_CallStaticDoubleMethod(void *env, void *clazz, void *mid, ...) {
  if (mid == &g_method_tags[MID_PREF_GET_DOUBLE]) {
    va_list ap; va_start(ap, mid);
    void *k = va_arg(ap, void *); double d = va_arg(ap, double);
    va_end(ap);
    return pref_get_num(k, d);
  }
  return 0.0;
}
static double jni_CallStaticDoubleMethodV(void *env, void *clazz, void *mid, va_list ap) {
  if (mid == &g_method_tags[MID_PREF_GET_DOUBLE]) {
    void *k = va_arg(ap, void *); double d = va_arg(ap, double);
    return pref_get_num(k, d);
  }
  return 0.0;
}
static double jni_CallStaticDoubleMethodA(void *env, void *clazz, void *mid, const void *args) {
  if (mid == &g_method_tags[MID_PREF_GET_DOUBLE]) {
    const uint64_t *a = (const uint64_t *)args;
    double d; memcpy(&d, &a[1], sizeof(double));
    return pref_get_num((void *)a[0], d);
  }
  return 0.0;
}

/* setters (void): comuns aos tres despachos */
static int pref_void_call_v(void *mid, va_list ap) {
  if (mid == &g_method_tags[MID_PREF_SET_BOOL]) {
    void *k = va_arg(ap, void *); int v = va_arg(ap, int); pref_set_bool(k, v); return 1;
  }
  if (mid == &g_method_tags[MID_PREF_SET_INT]) {
    void *k = va_arg(ap, void *); jint v = (jint)va_arg(ap, int); pref_set_int(k, v); return 1;
  }
  if (mid == &g_method_tags[MID_PREF_SET_FLOAT] || mid == &g_method_tags[MID_PREF_SET_DOUBLE]) {
    void *k = va_arg(ap, void *); double v = va_arg(ap, double); pref_set_num(k, v); return 1;
  }
  if (mid == &g_method_tags[MID_PREF_SET_STRING]) {
    void *k = va_arg(ap, void *); void *v = va_arg(ap, void *); pref_set_string(k, v); return 1;
  }
  if (mid == &g_method_tags[MID_PREF_DELETE]) {
    void *k = va_arg(ap, void *); prefs_delete(resolve_jstring(k)); return 1;
  }
  return 0;
}
static int pref_void_call_a(void *mid, const void *args) {
  const uint64_t *a = (const uint64_t *)args;
  if (mid == &g_method_tags[MID_PREF_SET_BOOL]) { pref_set_bool((void *)a[0], (int)a[1]); return 1; }
  if (mid == &g_method_tags[MID_PREF_SET_INT]) { pref_set_int((void *)a[0], (jint)(int32_t)a[1]); return 1; }
  if (mid == &g_method_tags[MID_PREF_SET_FLOAT]) {
    float v; memcpy(&v, &a[1], sizeof(float)); pref_set_num((void *)a[0], v); return 1;
  }
  if (mid == &g_method_tags[MID_PREF_SET_DOUBLE]) {
    double v; memcpy(&v, &a[1], sizeof(double)); pref_set_num((void *)a[0], v); return 1;
  }
  if (mid == &g_method_tags[MID_PREF_SET_STRING]) { pref_set_string((void *)a[0], (void *)a[1]); return 1; }
  if (mid == &g_method_tags[MID_PREF_DELETE]) { prefs_delete(resolve_jstring((void *)a[0])); return 1; }
  return 0;
}

static void jni_CallVoidMethod(void *env, void *obj, void *mid, ...) {
  if (mid == &g_method_tags[MID_EXIT]) { debugPrintf("jni_shim: exit()\n"); }
}
static void jni_CallStaticVoidMethod(void *env, void *clazz, void *mid, ...) {
  va_list ap; va_start(ap, mid);
  int handled = pref_void_call_v(mid, ap);
  va_end(ap);
  if (handled) return;
  if (mid == &g_method_tags[MID_EXIT]) { debugPrintf("jni_shim: static exit()\n"); }
}
static void jni_CallStaticVoidMethodV(void *env, void *clazz, void *mid, va_list a) {
  pref_void_call_v(mid, a);
}
static void jni_CallStaticVoidMethodA(void *env, void *clazz, void *mid, const void *a) {
  pref_void_call_a(mid, a);
}

static void *jni_NewStringUTF(void *env, const char *str) {
  if (g_prefs_log) debugPrintf("[prefs] NewStringUTF(\"%s\") count=%d\n", str ? str : "(null)", g_jstring_count);
  /* precisa sobreviver -> strdup (vaza pouco, e ok p/ poucas chamadas) */
  return jni_make_string(str ? strdup(str) : "");
}
/* NewString (UTF-16): o cocos cria as chaves do UserDefault por aqui
 * (StringUtils::newStringUTFJNI), nao por NewStringUTF. Sem isto a chave
 * chegava NULL e nada persistia. */
static void *jni_NewString(void *env, const unsigned short *chars, jint len) {
  if (!chars || len < 0) return jni_make_string("");
  char *buf = (char *)malloc((size_t)len * 3 + 1);
  if (!buf) return jni_make_string("");
  size_t w = 0;
  for (jint i = 0; i < len; i++) {
    unsigned short c = chars[i];
    if (c < 0x80) buf[w++] = (char)c;
    else if (c < 0x800) {
      buf[w++] = (char)(0xC0 | (c >> 6));
      buf[w++] = (char)(0x80 | (c & 0x3F));
    } else {
      buf[w++] = (char)(0xE0 | (c >> 12));
      buf[w++] = (char)(0x80 | ((c >> 6) & 0x3F));
      buf[w++] = (char)(0x80 | (c & 0x3F));
    }
  }
  buf[w] = 0;
  if (g_prefs_log) debugPrintf("[prefs] NewString(\"%s\")\n", buf);
  return jni_make_string(buf);
}

static jint jni_GetStringUTFLength(void *env, void *jstr) { return (jint)strlen(resolve_jstring(jstr)); }
static const char *jni_GetStringUTFChars(void *env, void *jstr, void *isCopy) {
  if (isCopy) *(unsigned char *)isCopy = 0;
  return resolve_jstring(jstr);
}
static void jni_ReleaseStringUTFChars(void *env, void *jstr, const char *chars) {}
static jint jni_GetStringLength(void *env, void *jstr) { return (jint)strlen(resolve_jstring(jstr)); }

/* GetStringChars (UTF-16): cocos2d StringUtils::getStringUTFCharsJNI usa ESTE,
 * nao GetStringUTFChars. Convertemos a string ASCII para jchar (16-bit). */
static unsigned short *jni_GetStringChars(void *env, void *jstr, void *isCopy) {
  const char *s = resolve_jstring(jstr);
  size_t n = strlen(s);
  unsigned short *buf = (unsigned short *)malloc((n + 1) * sizeof(unsigned short));
  for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)s[i];
  buf[n] = 0;
  if (isCopy) *(unsigned char *)isCopy = 1;
  return buf;
}
static void jni_ReleaseStringChars(void *env, void *jstr, unsigned short *chars) {
  free((void *)chars);
}

static void *jni_NewGlobalRef(void *env, void *obj) { return obj; }
static void *jni_NewLocalRef(void *env, void *obj) { return obj; }
static void jni_DeleteGlobalRef(void *env, void *obj) {}
static void jni_DeleteLocalRef(void *env, void *obj) {}
static void *jni_GetObjectClass(void *env, void *obj) { static int f; return &f; }
static jint jni_GetArrayLength(void *env, void *array) {
  int i = jarr_find(array); if (i >= 0) return g_jarr[i].len; return 0;
}
static void *jni_NewByteArray(void *env, jint len) {
  void *h = jarr_new(len, 1, NULL);
  if (g_jni_log) debugPrintf("JNI NewByteArray(%d) = %p\n", len, h);
  return h;
}
static void *jni_GetByteArrayElements(void *env, void *array, void *isCopy) {
  if (isCopy) *(unsigned char *)isCopy = 0;
  int i = jarr_find(array); if (i >= 0) return g_jarr[i].data;
  return g_dummy_array;
}
static void jni_ReleaseByteArrayElements(void *env, void *array, void *elems, jint mode) {}
static void jni_GetByteArrayRegion(void *env, void *array, jint start, jint len, void *buf) {
  int i = jarr_find(array); if (i < 0 || !g_jarr[i].data) return;
  if (start < 0 || start + len > g_jarr[i].len) return;
  memcpy(buf, g_jarr[i].data + start, len);
}
static void jni_SetByteArrayRegion(void *env, void *array, jint start, jint len, const void *buf) {
  int i = jarr_find(array); if (i < 0 || !g_jarr[i].data) return;
  if (start < 0 || start + len > g_jarr[i].len) return;
  memcpy(g_jarr[i].data + start, buf, len);
}
static void *jni_GetArrayElements(void *env, void *array, void *isCopy) {
  if (isCopy) *(unsigned char *)isCopy = 0; return g_dummy_array;
}
static void *jni_GetObjectArrayElement(void *env, void *array, jint i) { return jni_make_string("."); }
static jboolean jni_ExceptionCheck(void *env) { return 0; }
static void jni_ExceptionClear(void *env) {}
static void *jni_ExceptionOccurred(void *env) { return NULL; }
static void jni_ExceptionDescribe(void *env) {}
static jint jni_GetJavaVM(void *env, void **vm) { if (vm) *vm = &java_vm_ptr; return 0; }

static jint vm_DestroyJavaVM(void *vm) { return 0; }
static jint vm_AttachCurrentThread(void *vm, void **penv, void *args) { if (penv) *penv = &jni_env_ptr; return 0; }
static jint vm_DetachCurrentThread(void *vm) { return 0; }
static jint vm_GetEnv(void *vm, void **penv, jint version) { if (penv) *penv = &jni_env_ptr; return 0; }

void *AAssetManager_fromJava(void *env, void *assetManager) { return (void *)0x1337; }

void jni_shim_init(void **out_vm, void **out_env) {
  g_jni_log = getenv("CHRONO_JNILOG") != NULL;
  g_prefs_log = getenv("HW_PREFSLOG") != NULL;
  for (int i = 0; i < JNI_VTABLE_SIZE; i++) {
    jni_env_vtable[i] = (uintptr_t)jni_stub;
    java_vm_vtable[i] = (uintptr_t)jni_stub;
  }
  jni_env_vtable[4]  = (uintptr_t)jni_GetVersion;
  jni_env_vtable[6]  = (uintptr_t)jni_FindClass;
  jni_env_vtable[15] = (uintptr_t)jni_ExceptionOccurred;
  jni_env_vtable[16] = (uintptr_t)jni_ExceptionDescribe;
  jni_env_vtable[17] = (uintptr_t)jni_ExceptionClear;
  jni_env_vtable[21] = (uintptr_t)jni_NewGlobalRef;
  jni_env_vtable[22] = (uintptr_t)jni_DeleteGlobalRef;
  jni_env_vtable[23] = (uintptr_t)jni_DeleteLocalRef;
  jni_env_vtable[25] = (uintptr_t)jni_NewLocalRef;
  jni_env_vtable[31] = (uintptr_t)jni_GetObjectClass;
  jni_env_vtable[33] = (uintptr_t)jni_GetMethodID;
  jni_env_vtable[34] = (uintptr_t)jni_CallObjectMethod;   /* CallObjectMethod */
  jni_env_vtable[35] = (uintptr_t)jni_CallObjectMethod;   /* V */
  jni_env_vtable[36] = (uintptr_t)jni_CallObjectMethod;   /* A */
  jni_env_vtable[37] = (uintptr_t)jni_CallBooleanMethod;
  jni_env_vtable[38] = (uintptr_t)jni_CallBooleanMethod;
  jni_env_vtable[39] = (uintptr_t)jni_CallBooleanMethod;
  jni_env_vtable[49] = (uintptr_t)jni_CallIntMethod;
  jni_env_vtable[50] = (uintptr_t)jni_CallIntMethod;
  jni_env_vtable[51] = (uintptr_t)jni_CallIntMethod;
  jni_env_vtable[61] = (uintptr_t)jni_CallVoidMethod;
  jni_env_vtable[62] = (uintptr_t)jni_CallVoidMethod;
  jni_env_vtable[63] = (uintptr_t)jni_CallVoidMethod;
  jni_env_vtable[113] = (uintptr_t)jni_GetStaticMethodID;
  jni_env_vtable[114] = (uintptr_t)jni_CallStaticObjectMethod;
  jni_env_vtable[115] = (uintptr_t)jni_CallStaticObjectMethodV;
  jni_env_vtable[116] = (uintptr_t)jni_CallStaticObjectMethodA;
  jni_env_vtable[117] = (uintptr_t)jni_CallStaticBooleanMethod;
  jni_env_vtable[118] = (uintptr_t)jni_CallStaticBooleanMethodV;
  jni_env_vtable[119] = (uintptr_t)jni_CallStaticBooleanMethodA;
  jni_env_vtable[129] = (uintptr_t)jni_CallStaticIntMethod;
  jni_env_vtable[130] = (uintptr_t)jni_CallStaticIntMethodV;
  jni_env_vtable[131] = (uintptr_t)jni_CallStaticIntMethodA;
  jni_env_vtable[135] = (uintptr_t)jni_CallStaticFloatMethod;
  jni_env_vtable[136] = (uintptr_t)jni_CallStaticFloatMethodV;
  jni_env_vtable[137] = (uintptr_t)jni_CallStaticFloatMethodA;
  jni_env_vtable[138] = (uintptr_t)jni_CallStaticDoubleMethod;
  jni_env_vtable[139] = (uintptr_t)jni_CallStaticDoubleMethodV;
  jni_env_vtable[140] = (uintptr_t)jni_CallStaticDoubleMethodA;
  jni_env_vtable[141] = (uintptr_t)jni_CallStaticVoidMethod;
  jni_env_vtable[142] = (uintptr_t)jni_CallStaticVoidMethodV;
  jni_env_vtable[143] = (uintptr_t)jni_CallStaticVoidMethodA;
  jni_env_vtable[163] = (uintptr_t)jni_NewString;
  jni_env_vtable[164] = (uintptr_t)jni_GetStringLength;
  jni_env_vtable[165] = (uintptr_t)jni_GetStringChars;
  jni_env_vtable[166] = (uintptr_t)jni_ReleaseStringChars;
  jni_env_vtable[167] = (uintptr_t)jni_NewStringUTF;
  jni_env_vtable[168] = (uintptr_t)jni_GetStringUTFLength;
  jni_env_vtable[169] = (uintptr_t)jni_GetStringUTFChars;
  jni_env_vtable[170] = (uintptr_t)jni_ReleaseStringUTFChars;
  jni_env_vtable[171] = (uintptr_t)jni_GetArrayLength;
  jni_env_vtable[173] = (uintptr_t)jni_GetObjectArrayElement;
  jni_env_vtable[205] = (uintptr_t)jni_ExceptionCheck;
  jni_env_vtable[176] = (uintptr_t)jni_NewByteArray;
  jni_env_vtable[183] = (uintptr_t)jni_GetArrayElements;
  jni_env_vtable[184] = (uintptr_t)jni_GetByteArrayElements; /* byte[] real */
  jni_env_vtable[185] = (uintptr_t)jni_GetArrayElements;
  jni_env_vtable[186] = (uintptr_t)jni_GetArrayElements;
  jni_env_vtable[187] = (uintptr_t)jni_GetArrayElements;
  jni_env_vtable[188] = (uintptr_t)jni_GetArrayElements;
  jni_env_vtable[189] = (uintptr_t)jni_GetArrayElements;
  jni_env_vtable[190] = (uintptr_t)jni_GetArrayElements;
  jni_env_vtable[192] = (uintptr_t)jni_ReleaseByteArrayElements;
  jni_env_vtable[200] = (uintptr_t)jni_GetByteArrayRegion;
  jni_env_vtable[208] = (uintptr_t)jni_SetByteArrayRegion;
  jni_env_vtable[219] = (uintptr_t)jni_GetJavaVM;

  java_vm_vtable[3] = (uintptr_t)vm_DestroyJavaVM;
  java_vm_vtable[4] = (uintptr_t)vm_AttachCurrentThread;
  java_vm_vtable[5] = (uintptr_t)vm_DetachCurrentThread;
  java_vm_vtable[6] = (uintptr_t)vm_GetEnv;

  jni_env_ptr = jni_env_vtable;
  java_vm_ptr = java_vm_vtable;
  if (out_vm) *out_vm = &java_vm_ptr;
  if (out_env) *out_env = &jni_env_ptr;
}
