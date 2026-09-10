/* ab_jni.c — JNIEnv/JavaVM falsos para a Rovio Fusion.
 *
 * A ponte deste jogo NÃO se parece com a de nenhum outro port da casa: são as
 * ~54 classes `com/rovio` decompiladas do próprio APK (jadx, em
 * extracted/java/) que ditam nome, assinatura e semântica. Nada aqui foi
 * herdado de port de outra engine.
 *
 * Modelo: `jclass` e `jmethodID` são strings internadas; o despacho é por
 * "Classe::metodo". O que não tem handler é LOGADO uma vez (com a assinatura)
 * e devolve um valor neutro do tipo certo — é assim que o volume de stubs de
 * SDK (ads, billing, social, Firebase, HockeyApp) some sem travar o fluxo.
 *
 * Regras duras já pagas em outros ports e mantidas aqui:
 *   - retorno de OBJETO nunca é NULL (string vazia / objeto genérico);
 *   - a LARGURA do retorno tem que bater com a assinatura;
 *   - todo handler usa pcs("aapcs") porque o convidado é softfp.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ab_port.h"
#include "ab_locale.h"

#define SF __attribute__((pcs("aapcs")))

typedef int32_t jint;
typedef int64_t jlong;
typedef uint8_t jboolean;
typedef int8_t jbyte;
typedef uint16_t jchar;
typedef int16_t jshort;
typedef float jfloat;
typedef double jdouble;

#define JNI_VTABLE_SLOTS 240
#define VM_VTABLE_SLOTS 16

static uintptr_t g_env_vtable[JNI_VTABLE_SLOTS];
static uintptr_t g_vm_vtable[VM_VTABLE_SLOTS];
static const void *g_env = g_env_vtable;
static const void *g_vm = g_vm_vtable;
static ab_jni_hooks g_hooks;

/* ==================== objetos falsos ==================== */

#define TAG_CLASS 0x43u
#define TAG_OBJ 0x4Fu
#define TAG_STR 0x53u
#define TAG_ARR 0x41u

typedef struct AbObj {
  uint32_t tag;
  const char *cls;
} AbObj;

typedef struct AbStr {
  AbObj base;
  char *utf;
  jchar *utf16;
} AbStr;

typedef struct AbArr {
  AbObj base;
  int length;
  int elem_size;
  void *data;
} AbArr;

/* ---- nomes internados (classes e membros) ---- */
#define INTERN_MAX 1024
static char g_intern[INTERN_MAX][160];
static int g_intern_count;

static const char *intern(const char *text) {
  if (!text)
    text = "?";
  for (int i = 0; i < g_intern_count; i++)
    if (strcmp(g_intern[i], text) == 0)
      return g_intern[i];
  if (g_intern_count >= INTERN_MAX)
    return g_intern[0];
  snprintf(g_intern[g_intern_count], sizeof(g_intern[0]), "%s", text);
  return g_intern[g_intern_count++];
}

#define CLASS_MAX 256
static AbObj g_classes[CLASS_MAX];
static int g_class_count;

static AbObj *class_ref(const char *name) {
  const char *interned = intern(name);
  for (int i = 0; i < g_class_count; i++)
    if (g_classes[i].cls == interned)
      return &g_classes[i];
  if (g_class_count >= CLASS_MAX)
    return &g_classes[0];
  g_classes[g_class_count].tag = TAG_CLASS;
  g_classes[g_class_count].cls = interned;
  return &g_classes[g_class_count++];
}

/* Um membro guarda "Classe::nome" (chave de despacho) e a assinatura. */
typedef struct AbMember {
  const char *key; /* "com/rovio/fusion/Globals::getActivity" */
  const char *sig;
  const char *name;
} AbMember;

#define MEMBER_MAX 1024
static AbMember g_members[MEMBER_MAX];
static int g_member_count;

static AbMember *member_ref(const char *cls, const char *name,
                            const char *sig) {
  char key[160];
  const char *interned_key;
  snprintf(key, sizeof(key), "%s::%s", cls ? cls : "?", name ? name : "?");
  interned_key = intern(key);
  for (int i = 0; i < g_member_count; i++)
    if (g_members[i].key == interned_key)
      return &g_members[i];
  if (g_member_count >= MEMBER_MAX)
    return &g_members[0];
  g_members[g_member_count].key = interned_key;
  g_members[g_member_count].sig = intern(sig ? sig : "()V");
  g_members[g_member_count].name = intern(name ? name : "?");
  return &g_members[g_member_count++];
}

static const char *obj_class(const void *object) {
  const AbObj *o = object;
  if (!o)
    return "";
  if (o->tag == TAG_CLASS || o->tag == TAG_OBJ || o->tag == TAG_STR ||
      o->tag == TAG_ARR)
    return o->cls ? o->cls : "";
  return "";
}

static AbObj *new_object(const char *cls) {
  AbObj *o = calloc(1, sizeof(AbObj));
  o->tag = TAG_OBJ;
  o->cls = intern(cls ? cls : "java/lang/Object");
  return o;
}

static AbStr *new_string(const char *utf) {
  AbStr *s = calloc(1, sizeof(AbStr));
  s->base.tag = TAG_STR;
  s->base.cls = intern("java/lang/String");
  s->utf = strdup(utf ? utf : "");
  return s;
}

static const char *string_utf(const void *object) {
  const AbStr *s = object;
  if (s && s->base.tag == TAG_STR)
    return s->utf ? s->utf : "";
  return "";
}

static AbArr *new_array(const char *cls, int length, int elem_size) {
  AbArr *a = calloc(1, sizeof(AbArr));
  a->base.tag = TAG_ARR;
  a->base.cls = intern(cls);
  a->length = length < 0 ? 0 : length;
  a->elem_size = elem_size;
  a->data = calloc((size_t)(a->length ? a->length : 1), (size_t)elem_size);
  return a;
}

/* ==================== valor de retorno neutro ==================== */

/* Devolve o tipo de retorno da assinatura JNI (o caractere depois do ')'). */
static const char *return_type(const char *sig) {
  const char *close = sig ? strchr(sig, ')') : NULL;
  return close ? close + 1 : "V";
}

static void *neutral_object(const char *sig) {
  const char *ret = return_type(sig);
  if (ret[0] == '[') {
    if (ret[1] == 'B')
      return new_array("[B", 0, 1);
    if (ret[1] == 'I')
      return new_array("[I", 0, 4);
    if (ret[1] == 'F')
      return new_array("[F", 0, 4);
    return new_array(ret, 0, 4);
  }
  if (strncmp(ret, "Ljava/lang/String;", 18) == 0)
    return new_string("");
  if (ret[0] == 'L') {
    char cls[128];
    size_t len = strcspn(ret + 1, ";");
    if (len >= sizeof(cls))
      len = sizeof(cls) - 1;
    memcpy(cls, ret + 1, len);
    cls[len] = 0;
    return new_object(cls);
  }
  return new_object("java/lang/Object");
}

/* ==================== log de upcall não tratado ==================== */

#define UNHANDLED_MAX 512
static const char *g_unhandled[UNHANDLED_MAX];
static int g_unhandled_count;

static void log_unhandled(const AbMember *member) {
  for (int i = 0; i < g_unhandled_count; i++)
    if (g_unhandled[i] == member->key)
      return;
  if (g_unhandled_count < UNHANDLED_MAX)
    g_unhandled[g_unhandled_count++] = member->key;
  ab_log("[jni] SEM HANDLER %s %s", member->key, member->sig);
}

/* ==================== despacho ==================== */

typedef struct {
  jlong j;      /* inteiros e booleanos */
  jdouble d;    /* float/double */
  void *object; /* referências */
  int handled;
} AbResult;

static int is(const AbMember *m, const char *key) {
  return strcmp(m->key, key) == 0;
}

/* Contexto que o main publica (display, áudio, EGL). */
static int display_width(void) {
  return g_hooks.display_width ? g_hooks.display_width() : 640;
}
static int display_height(void) {
  return g_hooks.display_height ? g_hooks.display_height() : 480;
}

static AbResult dispatch(const AbMember *member, va_list ap) {
  AbResult r;
  memset(&r, 0, sizeof(r));
  r.handled = 1;

  /* ---------- com/rovio/fusion/Globals ---------- */
  if (is(member, "com/rovio/fusion/Globals::getActivity")) {
    r.object = new_object("com/rovio/fusion/App");
    return r;
  }
  if (is(member, "com/rovio/fusion/Globals::getAPILevel")) {
    r.j = 26;
    return r;
  }
  if (is(member, "com/rovio/fusion/Globals::getPathToFileCacheDirectory")) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/", ab_cachedir());
    r.object = new_string(path);
    return r;
  }
  if (is(member, "com/rovio/fusion/Globals::getConnectivityManager")) {
    r.object = new_object("android/net/ConnectivityManager");
    return r;
  }
  if (is(member, "com/rovio/fusion/Globals::isResumed") ||
      is(member, "com/rovio/fusion/Globals::isStarted")) {
    r.j = 1;
    return r;
  }
  if (is(member, "com/rovio/fusion/Globals::isPaused") ||
      is(member, "com/rovio/fusion/Globals::isStopped")) {
    r.j = 0;
    return r;
  }

  /* ---------- com/rovio/fusion/DeviceInfoWrapper ---------- */
  if (is(member, "com/rovio/fusion/DeviceInfoWrapper::getDisplayWidth")) {
    r.j = display_width();
    return r;
  }
  if (is(member, "com/rovio/fusion/DeviceInfoWrapper::getDisplayHeight")) {
    r.j = display_height();
    return r;
  }
  if (is(member, "com/rovio/fusion/DeviceInfoWrapper::getPPI")) {
    r.j = g_hooks.ppi ? g_hooks.ppi() : 160;
    return r;
  }
  if (is(member,
         "com/rovio/fusion/DeviceInfoWrapper::getDisplayDensityGroup")) {
    r.j = 160; /* mdpi: o bucket de imagem é escolhido pelo jogo, não por nós */
    return r;
  }
  if (is(member,
         "com/rovio/fusion/DeviceInfoWrapper::getDisplayConfigurationGroup")) {
    r.j = 3; /* SCREENLAYOUT_SIZE_LARGE */
    return r;
  }
  if (is(member, "com/rovio/fusion/DeviceInfoWrapper::hasSystemFeature")) {
    r.j = 0;
    return r;
  }

  /* ---------- versão / identidade ---------- */
  if (is(member,
         "com/rovio/fusion/ApplicationVersion::getApplicationVersionString")) {
    r.object = new_string("8.0.3");
    return r;
  }
  if (is(member, "com/rovio/fusion/DeviceIDCreator::getUniqueId")) {
    r.object = new_string("nextos-angrybirds-0001");
    return r;
  }

  /* ---------- localização: NXPORT_LANGUAGE do launcher (auto = LANG do
   * sistema, fallback en; japonês nunca por padrão) ---------- */
  if (is(member, "com/rovio/rcs/Localization::deviceLocale")) {
    r.object = new_string(ab_locale_tag());      /* pt-BR */
    return r;
  }
  if (is(member, "com/rovio/rcs/Localization::systemLocale") ||
      is(member, "java/util/Locale::getLanguage")) {
    r.object = new_string(ab_locale_language()); /* pt */
    return r;
  }
  if (is(member, "java/util/Locale::getDefault")) {
    r.object = new_object("java/util/Locale");
    return r;
  }
  if (is(member, "java/util/Locale::toString")) {
    r.object = new_string(ab_locale_underscore()); /* pt_BR */
    return r;
  }
  if (is(member, "java/util/Locale::toLanguageTag")) {
    r.object = new_string(ab_locale_tag());
    return r;
  }
  if (is(member, "java/util/Locale::getISO3Language")) {
    r.object = new_string(ab_locale_iso3_language());
    return r;
  }
  if (is(member, "java/util/Locale::getCountry")) {
    r.object = new_string(ab_locale_country());
    return r;
  }
  if (is(member, "java/util/Locale::getISO3Country")) {
    r.object = new_string(ab_locale_iso3_country());
    return r;
  }

  /* ---------- acelerômetro: o device não tem, e o jogo não precisa ------- */
  if (strncmp(member->key, "com/rovio/fusion/AccelerometerWrapper::", 39) ==
      0) {
    r.d = 0.0;
    r.j = 0;
    return r;
  }

  /* ---------- áudio ---------- */
  if (is(member, "com/rovio/fusion/AudioOutput::<init>")) {
    jlong mixer = va_arg(ap, jlong);
    jint rate = va_arg(ap, jint);
    jint channels = va_arg(ap, jint);
    jint bits = va_arg(ap, jint);
    jint bufbytes = va_arg(ap, jint);
    ab_log("[audio] AudioOutput(mixer=%p, %d Hz, %d ch, %d bits, %d B)",
           (void *)(uintptr_t)mixer, rate, channels, bits, bufbytes);
    if (g_hooks.audio_create)
      g_hooks.audio_create(mixer, rate, channels, bits, bufbytes);
    r.object = new_object("com/rovio/fusion/AudioOutput");
    return r;
  }
  if (is(member, "com/rovio/fusion/AudioOutput::startOutput")) {
    r.j = g_hooks.audio_start ? g_hooks.audio_start() : 0;
    ab_log("[audio] startOutput -> %d", (int)r.j);
    return r;
  }
  if (is(member, "com/rovio/fusion/AudioOutput::stopOutput")) {
    if (g_hooks.audio_stop)
      g_hooks.audio_stop();
    return r;
  }
  if (is(member, "com/rovio/fusion/AudioOutput::requestExclusiveAudio")) {
    return r;
  }

  /* ---------- saída ---------- */
  if (is(member, "com/rovio/fusion/App::quitRequested") ||
      is(member, "android/app/Activity::finish")) {
    ab_log("[jni] jogo pediu para sair (%s)", member->key);
    if (g_hooks.quit_requested)
      g_hooks.quit_requested();
    return r;
  }
  if (is(member, "com/rovio/fusion/App::isSilentProfile")) {
    r.j = 0;
    return r;
  }
  if (is(member, "com/rovio/fusion/App::allowSleep")) {
    return r;
  }

  /* ---------- Activity / Context ---------- */
  if (is(member, "android/app/Activity::getFilesDir") ||
      is(member, "android/content/Context::getFilesDir")) {
    r.object = new_object("java/io/File");
    return r;
  }
  if (is(member, "java/io/File::getAbsolutePath") ||
      is(member, "java/io/File::getPath")) {
    r.object = new_string(ab_filesdir());
    return r;
  }
  if (is(member, "android/app/Activity::getPackageName") ||
      is(member, "com/rovio/fusion/App::getPackageName") ||
      is(member, "android/content/Context::getPackageName")) {
    r.object = new_string("com.rovio.angrybirds");
    return r;
  }
  if (is(member, "android/app/Activity::getAssets") ||
      is(member, "android/content/Context::getAssets")) {
    r.object = new_object("android/content/res/AssetManager");
    return r;
  }

  /* ---------- Build ---------- */
  if (is(member, "android/os/Build$VERSION::SDK_INT")) {
    r.j = 26;
    return r;
  }

  /* ---------- com/rovio/rcs/core/Utils: identidade e rede ----------
   * getViewWidth/getViewHeight alimentam o layout da UI: precisam ser reais. */
  if (is(member, "com/rovio/rcs/core/Utils::getViewWidth")) {
    r.j = display_width();
    return r;
  }
  if (is(member, "com/rovio/rcs/core/Utils::getViewHeight")) {
    r.j = display_height();
    return r;
  }
  if (is(member, "com/rovio/rcs/core/Utils::getAndroidId")) {
    r.object = new_string("nextosangrybird");
    return r;
  }
  if (is(member, "com/rovio/rcs/core/Utils::networkType")) {
    r.object = new_string("none");
    return r;
  }
  if (is(member, "com/rovio/rcs/core/Utils::getInstallSource")) {
    r.object = new_string("nextos");
    return r;
  }
  if (is(member, "com/rovio/rcs/core/Utils::userAgentString")) {
    r.object = new_string("AngryBirds/8.0.3 (NextOS)");
    return r;
  }
  if (is(member, "com/rovio/rcs/core/Utils::advertisingTrackingEnabled") ||
      is(member, "com/rovio/rcs/core/Utils::queryNewPlayReferrer")) {
    r.j = 0;
    return r;
  }
  if (strncmp(member->key, "com/rovio/rcs/core/Utils::", 26) == 0) {
    /* o resto é analytics/ads: valor neutro, sem barulho no log */
    r.j = 0;
    r.d = 0.0;
    if (return_type(member->sig)[0] == 'L' ||
        return_type(member->sig)[0] == '[')
      r.object = neutral_object(member->sig);
    return r;
  }

  /* ---------- java/util/UUID ---------- */
  if (is(member, "java/util/UUID::randomUUID")) {
    r.object = new_object("java/util/UUID");
    return r;
  }
  if (is(member, "java/util/UUID::toString")) {
    r.object = new_string("6ca6d2ff-0000-4000-8000-6e6578744f53");
    return r;
  }

  /* ---------- periferia stubável: ads, billing, social, vídeo, webview,
   * notificações, Firebase, HockeyApp, Flurry. Tudo devolve "ok" para o fluxo
   * CONTINUAR; nada disso existe fora do Android. ---------- */
  {
    static const char *const stub_prefixes[] = {
        "com/rovio/rcs/ads/",
        "com/rovio/rcs/payment/",
        "com/rovio/rcs/socialnetwork/",
        "com/rovio/rcs/IdentityLoginUI::",
        "com/rovio/rcs/InstallReferrerReceiver::",
        "com/rovio/fusion/payment/",
        "com/rovio/fusion/WebViewWrapper::",
        "com/rovio/fusion/VideoPlayerBridge::",
        "com/rovio/fusion/LocalNotificationsWrapper::",
        "com/rovio/fusion/RemoteNotificationsClientWrapper::",
        "com/rovio/fusion/HockeyAppWrapper::",
        "com/rovio/fusion/AlertDialogWrapper::",
        "com/rovio/fusion/AppStoreLauncher::",
        "com/rovio/fusion/Launcher::",
        "com/rovio/fusion/TextInput::",
        "com/flurry/android/",
        "com/google/firebase/",
        "android/location/",
        "android/net/",
        "java/util/HashMap::",
        "java/util/ArrayList::",
        "java/util/List::",
        "java/util/Currency::",
        NULL};
    for (int i = 0; stub_prefixes[i]; i++) {
      size_t len = strlen(stub_prefixes[i]);
      if (strncmp(member->key, stub_prefixes[i], len) == 0) {
        const char *ret = return_type(member->sig);
        r.j = 0;
        r.d = 0.0;
        if (ret[0] == 'L' || ret[0] == '[')
          r.object = neutral_object(member->sig);
        return r;
      }
    }
  }

  /* ---------- EGLWrapper (contexto é nosso) ---------- */
  if (is(member, "com/rovio/fusion/EGLWrapper::init")) {
    return r;
  }
  if (is(member, "com/rovio/fusion/EGLWrapper::getCurrentContext")) {
    r.j = g_hooks.egl_current_context ? g_hooks.egl_current_context() : 0;
    return r;
  }
  if (is(member, "com/rovio/fusion/EGLWrapper::createSharedContext")) {
    jint handle = va_arg(ap, jint);
    r.j = g_hooks.egl_create_shared ? g_hooks.egl_create_shared(handle) : 0;
    return r;
  }
  if (is(member, "com/rovio/fusion/EGLWrapper::destroySharedContext")) {
    jint handle = va_arg(ap, jint);
    if (g_hooks.egl_destroy_shared)
      g_hooks.egl_destroy_shared(handle);
    return r;
  }
  if (is(member, "com/rovio/fusion/EGLWrapper::registerThread")) {
    jint handle = va_arg(ap, jint);
    r.j = g_hooks.egl_register_thread ? g_hooks.egl_register_thread(handle) : 0;
    return r;
  }
  if (is(member, "com/rovio/fusion/EGLWrapper::unregisterThread")) {
    if (g_hooks.egl_unregister_thread)
      g_hooks.egl_unregister_thread();
    return r;
  }

  /* ---------- SystemFontRenderer ----------
   * Pendência conhecida: o jogo tem fontes bitmap próprias em
   * assets/data/fonts, então este caminho é usado só em texto de sistema.
   * Devolvemos métricas coerentes e um bitmap transparente — nunca NULL. */
  if (is(member, "com/rovio/fusion/SystemFontRenderer::<init>")) {
    r.object = new_object("com/rovio/fusion/SystemFontRenderer");
    return r;
  }
  if (is(member, "com/rovio/fusion/SystemFontRenderer::drawString")) {
    r.object = new_array("[I", 1, 4);
    return r;
  }
  if (is(member, "com/rovio/fusion/SystemFontRenderer::getWidth")) {
    void *text = va_arg(ap, void *);
    r.j = (jint)(strlen(string_utf(text)) * 8);
    return r;
  }
  if (is(member, "com/rovio/fusion/SystemFontRenderer::getHeight")) {
    r.j = 16;
    return r;
  }
  if (is(member, "com/rovio/fusion/SystemFontRenderer::getAscender")) {
    r.j = 12;
    return r;
  }
  if (is(member, "com/rovio/fusion/SystemFontRenderer::getDescender")) {
    r.j = 4;
    return r;
  }
  if (strncmp(member->key, "com/rovio/fusion/SystemFontRenderer::", 37) == 0) {
    r.j = 0;
    return r;
  }

  r.handled = 0;
  return r;
}

/* Envolve o despacho e aplica o valor neutro quando não há handler. */
static AbResult call_member(void *method_id, va_list ap) {
  AbMember *member = method_id;
  AbResult r;
  if (!member) {
    memset(&r, 0, sizeof(r));
    return r;
  }
  r = dispatch(member, ap);
  if (!r.handled) {
    log_unhandled(member);
    r.j = 0;
    r.d = 0.0;
    r.object = NULL;
  }
  return r;
}

static void *call_object(void *method_id, va_list ap) {
  AbMember *member = method_id;
  AbResult r = call_member(method_id, ap);
  if (r.object)
    return r.object;
  /* 🚨 nunca NULL: string vazia / objeto genérico do tipo da assinatura */
  return neutral_object(member ? member->sig : "()Ljava/lang/Object;");
}

/* ==================== JNIEnv ==================== */

static jint SF jni_GetVersion(void *env) {
  (void)env;
  return 0x00010006;
}

static void *SF jni_FindClass(void *env, const char *name) {
  (void)env;
  if (ab_env_int("AB_JNI_LOG", 0))
    ab_log("[jni] FindClass(%s)", name);
  return class_ref(name);
}

static void *SF jni_GetObjectClass(void *env, void *object) {
  (void)env;
  return class_ref(obj_class(object));
}

static void *SF jni_GetMethodID(void *env, void *cls, const char *name,
                                const char *sig) {
  (void)env;
  if (ab_env_int("AB_JNI_LOG", 0))
    ab_log("[jni] GetMethodID(%s, %s, %s)", obj_class(cls), name, sig);
  return member_ref(obj_class(cls), name, sig);
}
static void *SF jni_GetStaticMethodID(void *env, void *cls, const char *name,
                                      const char *sig) {
  (void)env;
  if (ab_env_int("AB_JNI_LOG", 0))
    ab_log("[jni] GetStaticMethodID(%s, %s, %s)", obj_class(cls), name, sig);
  return member_ref(obj_class(cls), name, sig);
}
static void *SF jni_GetFieldID(void *env, void *cls, const char *name,
                               const char *sig) {
  (void)env;
  return member_ref(obj_class(cls), name, sig);
}
static void *SF jni_GetStaticFieldID(void *env, void *cls, const char *name,
                                     const char *sig) {
  (void)env;
  return member_ref(obj_class(cls), name, sig);
}

static void *SF jni_NewObject(void *env, void *cls, void *mid, ...) {
  va_list ap;
  void *result;
  (void)env;
  (void)cls;
  va_start(ap, mid);
  result = call_object(mid, ap);
  va_end(ap);
  return result;
}
static void *SF jni_NewObjectV(void *env, void *cls, void *mid, va_list ap) {
  (void)env;
  (void)cls;
  return call_object(mid, ap);
}
static void *SF jni_NewObjectA(void *env, void *cls, void *mid, void *args) {
  AbMember *member = mid;
  (void)env;
  (void)cls;
  (void)args;
  log_unhandled(member);
  return neutral_object(member ? member->sig : "()Ljava/lang/Object;");
}
static void *SF jni_AllocObject(void *env, void *cls) {
  (void)env;
  return new_object(obj_class(cls));
}

#define CALL_BODY(result_expr)      \
  va_list ap;                       \
  AbResult r;                       \
  (void)env;                        \
  (void)target;                     \
  va_start(ap, mid);                \
  r = call_member(mid, ap);         \
  va_end(ap);                       \
  return result_expr

#define CALL_V_BODY(result_expr)  \
  AbResult r;                     \
  (void)env;                      \
  (void)target;                   \
  r = call_member(mid, ap);       \
  return result_expr

#define DEFINE_CALL(prefix, type, result_expr)                                \
  static type SF jni_##prefix(void *env, void *target, void *mid, ...) {      \
    CALL_BODY(result_expr);                                                   \
  }                                                                           \
  static type SF jni_##prefix##V(void *env, void *target, void *mid,          \
                                 va_list ap) {                                \
    CALL_V_BODY(result_expr);                                                 \
  }                                                                           \
  static type SF jni_##prefix##A(void *env, void *target, void *mid,          \
                                 void *args) {                                \
    AbMember *member = mid;                                                   \
    (void)env;                                                                \
    (void)target;                                                             \
    (void)args;                                                               \
    log_unhandled(member);                                                    \
    return (type)0;                                                           \
  }

DEFINE_CALL(CallBooleanMethod, jboolean, (jboolean)(r.j != 0))
DEFINE_CALL(CallByteMethod, jbyte, (jbyte)r.j)
DEFINE_CALL(CallCharMethod, jchar, (jchar)r.j)
DEFINE_CALL(CallShortMethod, jshort, (jshort)r.j)
DEFINE_CALL(CallIntMethod, jint, (jint)r.j)
DEFINE_CALL(CallLongMethod, jlong, (jlong)r.j)
DEFINE_CALL(CallFloatMethod, jfloat, (jfloat)r.d)
DEFINE_CALL(CallDoubleMethod, jdouble, (jdouble)r.d)

DEFINE_CALL(CallStaticBooleanMethod, jboolean, (jboolean)(r.j != 0))
DEFINE_CALL(CallStaticByteMethod, jbyte, (jbyte)r.j)
DEFINE_CALL(CallStaticCharMethod, jchar, (jchar)r.j)
DEFINE_CALL(CallStaticShortMethod, jshort, (jshort)r.j)
DEFINE_CALL(CallStaticIntMethod, jint, (jint)r.j)
DEFINE_CALL(CallStaticLongMethod, jlong, (jlong)r.j)
DEFINE_CALL(CallStaticFloatMethod, jfloat, (jfloat)r.d)
DEFINE_CALL(CallStaticDoubleMethod, jdouble, (jdouble)r.d)

DEFINE_CALL(CallNonvirtualBooleanMethod, jboolean, (jboolean)(r.j != 0))
DEFINE_CALL(CallNonvirtualByteMethod, jbyte, (jbyte)r.j)
DEFINE_CALL(CallNonvirtualCharMethod, jchar, (jchar)r.j)
DEFINE_CALL(CallNonvirtualShortMethod, jshort, (jshort)r.j)
DEFINE_CALL(CallNonvirtualIntMethod, jint, (jint)r.j)
DEFINE_CALL(CallNonvirtualLongMethod, jlong, (jlong)r.j)
DEFINE_CALL(CallNonvirtualFloatMethod, jfloat, (jfloat)r.d)
DEFINE_CALL(CallNonvirtualDoubleMethod, jdouble, (jdouble)r.d)

static void SF jni_CallVoidMethod(void *env, void *target, void *mid, ...) {
  va_list ap;
  (void)env;
  (void)target;
  va_start(ap, mid);
  call_member(mid, ap);
  va_end(ap);
}
static void SF jni_CallVoidMethodV(void *env, void *target, void *mid,
                                   va_list ap) {
  (void)env;
  (void)target;
  call_member(mid, ap);
}
static void SF jni_CallVoidMethodA(void *env, void *target, void *mid,
                                   void *args) {
  (void)env;
  (void)target;
  (void)args;
  log_unhandled(mid);
}

static void *SF jni_CallObjectMethod(void *env, void *target, void *mid, ...) {
  va_list ap;
  void *result;
  (void)env;
  (void)target;
  va_start(ap, mid);
  result = call_object(mid, ap);
  va_end(ap);
  return result;
}
static void *SF jni_CallObjectMethodV(void *env, void *target, void *mid,
                                      va_list ap) {
  (void)env;
  (void)target;
  return call_object(mid, ap);
}
static void *SF jni_CallObjectMethodA(void *env, void *target, void *mid,
                                      void *args) {
  AbMember *member = mid;
  (void)env;
  (void)target;
  (void)args;
  log_unhandled(member);
  return neutral_object(member ? member->sig : "()Ljava/lang/Object;");
}

/* Os estáticos de void/objeto compartilham o mesmo corpo. */
#define ALIAS_STATIC(name) name

/* ---------- campos ---------- */
static void *SF jni_GetObjectField(void *env, void *object, void *fid) {
  AbMember *member = fid;
  (void)env;
  (void)object;
  if (!member)
    return new_string("");
  if (is(member, "android/os/Build::MANUFACTURER"))
    return new_string("NextOS");
  if (is(member, "android/os/Build::MODEL"))
    return new_string("NextOS Elite");
  if (is(member, "android/os/Build::DEVICE"))
    return new_string("nextos");
  if (is(member, "android/os/Build::BRAND"))
    return new_string("NextOS");
  if (is(member, "android/os/Build$VERSION::RELEASE"))
    return new_string("8.0.0");
  log_unhandled(member);
  return neutral_object(member->sig);
}
static void *SF jni_GetStaticObjectField(void *env, void *cls, void *fid) {
  return jni_GetObjectField(env, cls, fid);
}

/* Leitura de campo: sem argumentos, então não há va_list envolvida. Os poucos
 * campos que o nativo realmente lê (Build$VERSION.SDK_INT) estão em
 * field_value(); o resto é logado e devolve 0 na largura certa. */
static jlong field_value(void *fid, int *handled) {
  AbMember *member = fid;
  *handled = 1;
  if (!member) {
    *handled = 0;
    return 0;
  }
  if (is(member, "android/os/Build$VERSION::SDK_INT"))
    return 26;
  *handled = 0;
  log_unhandled(member);
  return 0;
}

#define DEFINE_FIELD(prefix, type, result_expr)                          \
  static type SF jni_Get##prefix##Field(void *env, void *object,         \
                                        void *fid) {                     \
    AbResult r;                                                          \
    int handled = 0;                                                     \
    (void)env;                                                           \
    (void)object;                                                        \
    memset(&r, 0, sizeof(r));                                            \
    r.j = field_value(fid, &handled);                                    \
    r.d = (jdouble)r.j;                                                  \
    return result_expr;                                                   \
  }                                                                       \
  static type SF jni_GetStatic##prefix##Field(void *env, void *cls,      \
                                              void *fid) {                \
    return jni_Get##prefix##Field(env, cls, fid);                         \
  }                                                                       \
  static void SF jni_Set##prefix##Field(void *env, void *object, void *fid, \
                                        type value) {                     \
    (void)env;                                                            \
    (void)object;                                                         \
    (void)fid;                                                            \
    (void)value;                                                          \
  }                                                                       \
  static void SF jni_SetStatic##prefix##Field(void *env, void *cls,      \
                                              void *fid, type value) {    \
    (void)env;                                                            \
    (void)cls;                                                            \
    (void)fid;                                                            \
    (void)value;                                                          \
  }

DEFINE_FIELD(Boolean, jboolean, (jboolean)(r.j != 0))
DEFINE_FIELD(Byte, jbyte, (jbyte)r.j)
DEFINE_FIELD(Char, jchar, (jchar)r.j)
DEFINE_FIELD(Short, jshort, (jshort)r.j)
DEFINE_FIELD(Int, jint, (jint)r.j)
DEFINE_FIELD(Long, jlong, (jlong)r.j)
DEFINE_FIELD(Float, jfloat, (jfloat)r.d)
DEFINE_FIELD(Double, jdouble, (jdouble)r.d)

static void SF jni_SetObjectField(void *env, void *object, void *fid,
                                  void *value) {
  (void)env;
  (void)object;
  (void)fid;
  (void)value;
}
static void SF jni_SetStaticObjectField(void *env, void *cls, void *fid,
                                        void *value) {
  (void)env;
  (void)cls;
  (void)fid;
  (void)value;
}

/* ---------- strings ---------- */
static void *SF jni_NewStringUTF(void *env, const char *utf) {
  (void)env;
  return new_string(utf);
}
static const char *SF jni_GetStringUTFChars(void *env, void *string,
                                            jboolean *is_copy) {
  (void)env;
  if (is_copy)
    *is_copy = 0;
  return string_utf(string);
}
static void SF jni_ReleaseStringUTFChars(void *env, void *string,
                                         const char *chars) {
  (void)env;
  (void)string;
  (void)chars;
}
static jint SF jni_GetStringUTFLength(void *env, void *string) {
  (void)env;
  return (jint)strlen(string_utf(string));
}
static jint SF jni_GetStringLength(void *env, void *string) {
  (void)env;
  return (jint)strlen(string_utf(string));
}
static const jchar *SF jni_GetStringChars(void *env, void *string,
                                          jboolean *is_copy) {
  AbStr *s = string;
  const char *utf;
  size_t len;
  (void)env;
  if (is_copy)
    *is_copy = 0;
  if (!s || s->base.tag != TAG_STR)
    return NULL;
  if (s->utf16)
    return s->utf16;
  utf = s->utf ? s->utf : "";
  len = strlen(utf);
  s->utf16 = calloc(len + 1, sizeof(jchar));
  for (size_t i = 0; i < len; i++)
    s->utf16[i] = (jchar)(unsigned char)utf[i];
  return s->utf16;
}
static void SF jni_ReleaseStringChars(void *env, void *string,
                                      const jchar *chars) {
  (void)env;
  (void)string;
  (void)chars;
}
static void *SF jni_NewString(void *env, const jchar *chars, jint length) {
  char *utf = malloc((size_t)length + 1);
  void *result;
  (void)env;
  for (jint i = 0; i < length; i++)
    utf[i] = (char)(chars[i] & 0x7f);
  utf[length] = 0;
  result = new_string(utf);
  free(utf);
  return result;
}
static void SF jni_GetStringUTFRegion(void *env, void *string, jint start,
                                      jint length, char *buffer) {
  const char *utf = string_utf(string);
  size_t total = strlen(utf);
  (void)env;
  if (!buffer)
    return;
  if ((size_t)start > total)
    start = (jint)total;
  if ((size_t)(start + length) > total)
    length = (jint)(total - (size_t)start);
  memcpy(buffer, utf + start, (size_t)length);
  buffer[length] = 0;
}
static void SF jni_GetStringRegion(void *env, void *string, jint start,
                                   jint length, jchar *buffer) {
  const char *utf = string_utf(string);
  (void)env;
  for (jint i = 0; i < length; i++)
    buffer[i] = (jchar)(unsigned char)utf[start + i];
}

/* ---------- arrays ---------- */
static jint SF jni_GetArrayLength(void *env, void *array) {
  AbArr *a = array;
  (void)env;
  return (a && a->base.tag == TAG_ARR) ? a->length : 0;
}
#define DEFINE_ARRAY(prefix, tag_name, elem_size, type)                       \
  static void *SF jni_New##prefix##Array(void *env, jint length) {            \
    (void)env;                                                                \
    return new_array(tag_name, length, elem_size);                            \
  }                                                                           \
  static type *SF jni_Get##prefix##ArrayElements(void *env, void *array,      \
                                                 jboolean *is_copy) {         \
    AbArr *a = array;                                                         \
    (void)env;                                                                \
    if (is_copy)                                                              \
      *is_copy = 0;                                                           \
    return (a && a->base.tag == TAG_ARR) ? (type *)a->data : NULL;            \
  }                                                                           \
  static void SF jni_Release##prefix##ArrayElements(void *env, void *array,   \
                                                    type *elements,           \
                                                    jint mode) {              \
    (void)env;                                                                \
    (void)array;                                                              \
    (void)elements;                                                           \
    (void)mode;                                                               \
  }                                                                           \
  static void SF jni_Get##prefix##ArrayRegion(void *env, void *array,         \
                                              jint start, jint length,        \
                                              type *buffer) {                 \
    AbArr *a = array;                                                         \
    (void)env;                                                                \
    if (!a || a->base.tag != TAG_ARR || !buffer)                              \
      return;                                                                 \
    if (start < 0 || length < 0 || start + length > a->length)                \
      return;                                                                 \
    memcpy(buffer, (type *)a->data + start, (size_t)length * elem_size);      \
  }                                                                           \
  static void SF jni_Set##prefix##ArrayRegion(void *env, void *array,         \
                                              jint start, jint length,        \
                                              const type *buffer) {           \
    AbArr *a = array;                                                         \
    (void)env;                                                                \
    if (!a || a->base.tag != TAG_ARR || !buffer)                              \
      return;                                                                 \
    if (start < 0 || length < 0 || start + length > a->length)                \
      return;                                                                 \
    memcpy((type *)a->data + start, buffer, (size_t)length * elem_size);      \
  }

DEFINE_ARRAY(Boolean, "[Z", 1, jboolean)
DEFINE_ARRAY(Byte, "[B", 1, jbyte)
DEFINE_ARRAY(Char, "[C", 2, jchar)
DEFINE_ARRAY(Short, "[S", 2, jshort)
DEFINE_ARRAY(Int, "[I", 4, jint)
DEFINE_ARRAY(Long, "[J", 8, jlong)
DEFINE_ARRAY(Float, "[F", 4, jfloat)
DEFINE_ARRAY(Double, "[D", 8, jdouble)

static void *SF jni_NewObjectArray(void *env, jint length, void *cls,
                                   void *initial) {
  AbArr *a;
  (void)env;
  a = new_array("[Ljava/lang/Object;", length, (int)sizeof(void *));
  if (initial) {
    void **slots = a->data;
    for (jint i = 0; i < length; i++)
      slots[i] = initial;
  }
  (void)cls;
  return a;
}
static void *SF jni_GetObjectArrayElement(void *env, void *array, jint index) {
  AbArr *a = array;
  void **slots;
  (void)env;
  if (!a || a->base.tag != TAG_ARR || index < 0 || index >= a->length)
    return new_string("");
  slots = a->data;
  return slots[index] ? slots[index] : (void *)new_string("");
}
static void SF jni_SetObjectArrayElement(void *env, void *array, jint index,
                                         void *value) {
  AbArr *a = array;
  void **slots;
  (void)env;
  if (!a || a->base.tag != TAG_ARR || index < 0 || index >= a->length)
    return;
  slots = a->data;
  slots[index] = value;
}
static void *SF jni_GetPrimitiveArrayCritical(void *env, void *array,
                                              jboolean *is_copy) {
  AbArr *a = array;
  (void)env;
  if (is_copy)
    *is_copy = 0;
  return (a && a->base.tag == TAG_ARR) ? a->data : NULL;
}
static void SF jni_ReleasePrimitiveArrayCritical(void *env, void *array,
                                                 void *data, jint mode) {
  (void)env;
  (void)array;
  (void)data;
  (void)mode;
}

/* ---------- referências, exceções, monitores ---------- */
static void *SF jni_ref_passthrough(void *env, void *object) {
  (void)env;
  return object;
}
static void SF jni_ref_release(void *env, void *object) {
  (void)env;
  (void)object;
}
static jboolean SF jni_IsSameObject(void *env, void *a, void *b) {
  (void)env;
  return (jboolean)(a == b);
}
static jboolean SF jni_IsInstanceOf(void *env, void *object, void *cls) {
  (void)env;
  (void)object;
  (void)cls;
  return 1;
}
static jboolean SF jni_IsAssignableFrom(void *env, void *a, void *b) {
  (void)env;
  (void)a;
  (void)b;
  return 1;
}
static void *SF jni_ExceptionOccurred(void *env) {
  (void)env;
  return NULL;
}
static void SF jni_ExceptionClear(void *env) { (void)env; }
static void SF jni_ExceptionDescribe(void *env) { (void)env; }
static jboolean SF jni_ExceptionCheck(void *env) {
  (void)env;
  return 0;
}
static jint SF jni_Throw(void *env, void *throwable) {
  (void)env;
  (void)throwable;
  return 0;
}
static jint SF jni_ThrowNew(void *env, void *cls, const char *message) {
  (void)env;
  ab_log("[jni] ThrowNew(%s): %s", obj_class(cls), message ? message : "");
  return 0;
}
static void SF jni_FatalError(void *env, const char *message) {
  (void)env;
  ab_log("[jni] FatalError: %s", message ? message : "");
  ab_log_close();
  _Exit(1);
}
static jint SF jni_PushLocalFrame(void *env, jint capacity) {
  (void)env;
  (void)capacity;
  return 0;
}
static void *SF jni_PopLocalFrame(void *env, void *result) {
  (void)env;
  return result;
}
static jint SF jni_EnsureLocalCapacity(void *env, jint capacity) {
  (void)env;
  (void)capacity;
  return 0;
}
static jint SF jni_MonitorEnter(void *env, void *object) {
  (void)env;
  (void)object;
  return 0;
}
static jint SF jni_MonitorExit(void *env, void *object) {
  (void)env;
  (void)object;
  return 0;
}
static jint SF jni_RegisterNatives(void *env, void *cls, const void *methods,
                                   jint count) {
  (void)env;
  (void)methods;
  ab_log("[jni] RegisterNatives(%s, %d) — aceito sem efeito (chamamos os "
         "Java_* exportados direto)",
         obj_class(cls), (int)count);
  return 0;
}
static jint SF jni_UnregisterNatives(void *env, void *cls) {
  (void)env;
  (void)cls;
  return 0;
}
static void *SF jni_GetSuperclass(void *env, void *cls) {
  (void)env;
  (void)cls;
  return class_ref("java/lang/Object");
}
static void *SF jni_NewDirectByteBuffer(void *env, void *address,
                                        jlong capacity) {
  AbArr *a;
  (void)env;
  a = calloc(1, sizeof(AbArr));
  a->base.tag = TAG_ARR;
  a->base.cls = intern("java/nio/ByteBuffer");
  a->length = (int)capacity;
  a->elem_size = 1;
  a->data = address;
  return a;
}
static void *SF jni_GetDirectBufferAddress(void *env, void *buffer) {
  AbArr *a = buffer;
  (void)env;
  return (a && a->base.tag == TAG_ARR) ? a->data : NULL;
}
static jlong SF jni_GetDirectBufferCapacity(void *env, void *buffer) {
  AbArr *a = buffer;
  (void)env;
  return (a && a->base.tag == TAG_ARR) ? a->length : 0;
}
static jint SF jni_GetJavaVM(void *env, void **vm) {
  (void)env;
  if (vm)
    *vm = (void *)&g_vm;
  return 0;
}
static jint SF jni_GetObjectRefType(void *env, void *object) {
  (void)env;
  (void)object;
  return 2; /* JNIGlobalRefType */
}

/* ==================== JavaVM ==================== */

static jint SF vm_GetEnv(void *vm, void **env, jint version) {
  (void)vm;
  (void)version;
  if (env)
    *env = (void *)&g_env;
  return 0;
}
static jint SF vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm;
  (void)args;
  if (env)
    *env = (void *)&g_env;
  return 0;
}
static jint SF vm_DetachCurrentThread(void *vm) {
  (void)vm;
  return 0;
}
static jint SF vm_DestroyJavaVM(void *vm) {
  (void)vm;
  return 0;
}

/* ==================== montagem das vtables ==================== */

void ab_jni_set_hooks(const ab_jni_hooks *hooks) {
  if (hooks)
    g_hooks = *hooks;
}

void *ab_jni_env(void) { return (void *)&g_env; }
void *ab_jni_vm(void) { return (void *)&g_vm; }

#define SLOT(index, fn) g_env_vtable[index] = (uintptr_t)(fn)

void ab_jni_init(void) {
  memset(g_env_vtable, 0, sizeof(g_env_vtable));
  memset(g_vm_vtable, 0, sizeof(g_vm_vtable));

  SLOT(4, jni_GetVersion);
  SLOT(6, jni_FindClass);
  SLOT(10, jni_GetSuperclass);
  SLOT(11, jni_IsAssignableFrom);
  SLOT(13, jni_Throw);
  SLOT(14, jni_ThrowNew);
  SLOT(15, jni_ExceptionOccurred);
  SLOT(16, jni_ExceptionDescribe);
  SLOT(17, jni_ExceptionClear);
  SLOT(18, jni_FatalError);
  SLOT(19, jni_PushLocalFrame);
  SLOT(20, jni_PopLocalFrame);
  SLOT(21, jni_ref_passthrough); /* NewGlobalRef */
  SLOT(22, jni_ref_release);     /* DeleteGlobalRef */
  SLOT(23, jni_ref_release);     /* DeleteLocalRef */
  SLOT(24, jni_IsSameObject);
  SLOT(25, jni_ref_passthrough); /* NewLocalRef */
  SLOT(26, jni_EnsureLocalCapacity);
  SLOT(27, jni_AllocObject);
  SLOT(28, jni_NewObject);
  SLOT(29, jni_NewObjectV);
  SLOT(30, jni_NewObjectA);
  SLOT(31, jni_GetObjectClass);
  SLOT(32, jni_IsInstanceOf);
  SLOT(33, jni_GetMethodID);

  SLOT(34, jni_CallObjectMethod);
  SLOT(35, jni_CallObjectMethodV);
  SLOT(36, jni_CallObjectMethodA);
  SLOT(37, jni_CallBooleanMethod);
  SLOT(38, jni_CallBooleanMethodV);
  SLOT(39, jni_CallBooleanMethodA);
  SLOT(40, jni_CallByteMethod);
  SLOT(41, jni_CallByteMethodV);
  SLOT(42, jni_CallByteMethodA);
  SLOT(43, jni_CallCharMethod);
  SLOT(44, jni_CallCharMethodV);
  SLOT(45, jni_CallCharMethodA);
  SLOT(46, jni_CallShortMethod);
  SLOT(47, jni_CallShortMethodV);
  SLOT(48, jni_CallShortMethodA);
  SLOT(49, jni_CallIntMethod);
  SLOT(50, jni_CallIntMethodV);
  SLOT(51, jni_CallIntMethodA);
  SLOT(52, jni_CallLongMethod);
  SLOT(53, jni_CallLongMethodV);
  SLOT(54, jni_CallLongMethodA);
  SLOT(55, jni_CallFloatMethod);
  SLOT(56, jni_CallFloatMethodV);
  SLOT(57, jni_CallFloatMethodA);
  SLOT(58, jni_CallDoubleMethod);
  SLOT(59, jni_CallDoubleMethodV);
  SLOT(60, jni_CallDoubleMethodA);
  SLOT(61, jni_CallVoidMethod);
  SLOT(62, jni_CallVoidMethodV);
  SLOT(63, jni_CallVoidMethodA);

  SLOT(64, jni_CallObjectMethod);
  SLOT(65, jni_CallObjectMethodV);
  SLOT(66, jni_CallObjectMethodA);
  SLOT(67, jni_CallNonvirtualBooleanMethod);
  SLOT(68, jni_CallNonvirtualBooleanMethodV);
  SLOT(69, jni_CallNonvirtualBooleanMethodA);
  SLOT(70, jni_CallNonvirtualByteMethod);
  SLOT(71, jni_CallNonvirtualByteMethodV);
  SLOT(72, jni_CallNonvirtualByteMethodA);
  SLOT(73, jni_CallNonvirtualCharMethod);
  SLOT(74, jni_CallNonvirtualCharMethodV);
  SLOT(75, jni_CallNonvirtualCharMethodA);
  SLOT(76, jni_CallNonvirtualShortMethod);
  SLOT(77, jni_CallNonvirtualShortMethodV);
  SLOT(78, jni_CallNonvirtualShortMethodA);
  SLOT(79, jni_CallNonvirtualIntMethod);
  SLOT(80, jni_CallNonvirtualIntMethodV);
  SLOT(81, jni_CallNonvirtualIntMethodA);
  SLOT(82, jni_CallNonvirtualLongMethod);
  SLOT(83, jni_CallNonvirtualLongMethodV);
  SLOT(84, jni_CallNonvirtualLongMethodA);
  SLOT(85, jni_CallNonvirtualFloatMethod);
  SLOT(86, jni_CallNonvirtualFloatMethodV);
  SLOT(87, jni_CallNonvirtualFloatMethodA);
  SLOT(88, jni_CallNonvirtualDoubleMethod);
  SLOT(89, jni_CallNonvirtualDoubleMethodV);
  SLOT(90, jni_CallNonvirtualDoubleMethodA);
  SLOT(91, jni_CallVoidMethod);
  SLOT(92, jni_CallVoidMethodV);
  SLOT(93, jni_CallVoidMethodA);

  SLOT(94, jni_GetFieldID);
  SLOT(95, jni_GetObjectField);
  SLOT(96, jni_GetBooleanField);
  SLOT(97, jni_GetByteField);
  SLOT(98, jni_GetCharField);
  SLOT(99, jni_GetShortField);
  SLOT(100, jni_GetIntField);
  SLOT(101, jni_GetLongField);
  SLOT(102, jni_GetFloatField);
  SLOT(103, jni_GetDoubleField);
  SLOT(104, jni_SetObjectField);
  SLOT(105, jni_SetBooleanField);
  SLOT(106, jni_SetByteField);
  SLOT(107, jni_SetCharField);
  SLOT(108, jni_SetShortField);
  SLOT(109, jni_SetIntField);
  SLOT(110, jni_SetLongField);
  SLOT(111, jni_SetFloatField);
  SLOT(112, jni_SetDoubleField);

  SLOT(113, jni_GetStaticMethodID);
  SLOT(114, jni_CallObjectMethod);
  SLOT(115, jni_CallObjectMethodV);
  SLOT(116, jni_CallObjectMethodA);
  SLOT(117, jni_CallStaticBooleanMethod);
  SLOT(118, jni_CallStaticBooleanMethodV);
  SLOT(119, jni_CallStaticBooleanMethodA);
  SLOT(120, jni_CallStaticByteMethod);
  SLOT(121, jni_CallStaticByteMethodV);
  SLOT(122, jni_CallStaticByteMethodA);
  SLOT(123, jni_CallStaticCharMethod);
  SLOT(124, jni_CallStaticCharMethodV);
  SLOT(125, jni_CallStaticCharMethodA);
  SLOT(126, jni_CallStaticShortMethod);
  SLOT(127, jni_CallStaticShortMethodV);
  SLOT(128, jni_CallStaticShortMethodA);
  SLOT(129, jni_CallStaticIntMethod);
  SLOT(130, jni_CallStaticIntMethodV);
  SLOT(131, jni_CallStaticIntMethodA);
  SLOT(132, jni_CallStaticLongMethod);
  SLOT(133, jni_CallStaticLongMethodV);
  SLOT(134, jni_CallStaticLongMethodA);
  SLOT(135, jni_CallStaticFloatMethod);
  SLOT(136, jni_CallStaticFloatMethodV);
  SLOT(137, jni_CallStaticFloatMethodA);
  SLOT(138, jni_CallStaticDoubleMethod);
  SLOT(139, jni_CallStaticDoubleMethodV);
  SLOT(140, jni_CallStaticDoubleMethodA);
  SLOT(141, jni_CallVoidMethod);
  SLOT(142, jni_CallVoidMethodV);
  SLOT(143, jni_CallVoidMethodA);

  SLOT(144, jni_GetStaticFieldID);
  SLOT(145, jni_GetStaticObjectField);
  SLOT(146, jni_GetStaticBooleanField);
  SLOT(147, jni_GetStaticByteField);
  SLOT(148, jni_GetStaticCharField);
  SLOT(149, jni_GetStaticShortField);
  SLOT(150, jni_GetStaticIntField);
  SLOT(151, jni_GetStaticLongField);
  SLOT(152, jni_GetStaticFloatField);
  SLOT(153, jni_GetStaticDoubleField);
  SLOT(154, jni_SetStaticObjectField);
  SLOT(155, jni_SetStaticBooleanField);
  SLOT(156, jni_SetStaticByteField);
  SLOT(157, jni_SetStaticCharField);
  SLOT(158, jni_SetStaticShortField);
  SLOT(159, jni_SetStaticIntField);
  SLOT(160, jni_SetStaticLongField);
  SLOT(161, jni_SetStaticFloatField);
  SLOT(162, jni_SetStaticDoubleField);

  SLOT(163, jni_NewString);
  SLOT(164, jni_GetStringLength);
  SLOT(165, jni_GetStringChars);
  SLOT(166, jni_ReleaseStringChars);
  SLOT(167, jni_NewStringUTF);
  SLOT(168, jni_GetStringUTFLength);
  SLOT(169, jni_GetStringUTFChars);
  SLOT(170, jni_ReleaseStringUTFChars);
  SLOT(171, jni_GetArrayLength);
  SLOT(172, jni_NewObjectArray);
  SLOT(173, jni_GetObjectArrayElement);
  SLOT(174, jni_SetObjectArrayElement);
  SLOT(175, jni_NewBooleanArray);
  SLOT(176, jni_NewByteArray);
  SLOT(177, jni_NewCharArray);
  SLOT(178, jni_NewShortArray);
  SLOT(179, jni_NewIntArray);
  SLOT(180, jni_NewLongArray);
  SLOT(181, jni_NewFloatArray);
  SLOT(182, jni_NewDoubleArray);
  SLOT(183, jni_GetBooleanArrayElements);
  SLOT(184, jni_GetByteArrayElements);
  SLOT(185, jni_GetCharArrayElements);
  SLOT(186, jni_GetShortArrayElements);
  SLOT(187, jni_GetIntArrayElements);
  SLOT(188, jni_GetLongArrayElements);
  SLOT(189, jni_GetFloatArrayElements);
  SLOT(190, jni_GetDoubleArrayElements);
  SLOT(191, jni_ReleaseBooleanArrayElements);
  SLOT(192, jni_ReleaseByteArrayElements);
  SLOT(193, jni_ReleaseCharArrayElements);
  SLOT(194, jni_ReleaseShortArrayElements);
  SLOT(195, jni_ReleaseIntArrayElements);
  SLOT(196, jni_ReleaseLongArrayElements);
  SLOT(197, jni_ReleaseFloatArrayElements);
  SLOT(198, jni_ReleaseDoubleArrayElements);
  SLOT(199, jni_GetBooleanArrayRegion);
  SLOT(200, jni_GetByteArrayRegion);
  SLOT(201, jni_GetCharArrayRegion);
  SLOT(202, jni_GetShortArrayRegion);
  SLOT(203, jni_GetIntArrayRegion);
  SLOT(204, jni_GetLongArrayRegion);
  SLOT(205, jni_GetFloatArrayRegion);
  SLOT(206, jni_GetDoubleArrayRegion);
  SLOT(207, jni_SetBooleanArrayRegion);
  SLOT(208, jni_SetByteArrayRegion);
  SLOT(209, jni_SetCharArrayRegion);
  SLOT(210, jni_SetShortArrayRegion);
  SLOT(211, jni_SetIntArrayRegion);
  SLOT(212, jni_SetLongArrayRegion);
  SLOT(213, jni_SetFloatArrayRegion);
  SLOT(214, jni_SetDoubleArrayRegion);
  SLOT(215, jni_RegisterNatives);
  SLOT(216, jni_UnregisterNatives);
  SLOT(217, jni_MonitorEnter);
  SLOT(218, jni_MonitorExit);
  SLOT(219, jni_GetJavaVM);
  SLOT(220, jni_GetStringRegion);
  SLOT(221, jni_GetStringUTFRegion);
  SLOT(222, jni_GetPrimitiveArrayCritical);
  SLOT(223, jni_ReleasePrimitiveArrayCritical);
  SLOT(224, jni_GetStringChars);          /* GetStringCritical */
  SLOT(225, jni_ReleaseStringChars);      /* ReleaseStringCritical */
  SLOT(226, jni_ref_passthrough);         /* NewWeakGlobalRef */
  SLOT(227, jni_ref_release);             /* DeleteWeakGlobalRef */
  SLOT(228, jni_ExceptionCheck);
  SLOT(229, jni_NewDirectByteBuffer);
  SLOT(230, jni_GetDirectBufferAddress);
  SLOT(231, jni_GetDirectBufferCapacity);
  SLOT(232, jni_GetObjectRefType);

  g_vm_vtable[3] = (uintptr_t)vm_DestroyJavaVM;
  g_vm_vtable[4] = (uintptr_t)vm_AttachCurrentThread;
  g_vm_vtable[5] = (uintptr_t)vm_DetachCurrentThread;
  g_vm_vtable[6] = (uintptr_t)vm_GetEnv;
  g_vm_vtable[7] = (uintptr_t)vm_AttachCurrentThread; /* AsDaemon */
}

/* ---- utilitários usados pelo main ---- */
void *ab_jni_new_object(const char *cls) { return new_object(cls); }
void *ab_jni_new_string(const char *utf) { return new_string(utf); }
void *ab_jni_new_byte_array(int length) { return new_array("[B", length, 1); }
void *ab_jni_array_data(void *array) {
  AbArr *a = array;
  return (a && a->base.tag == TAG_ARR) ? a->data : NULL;
}
