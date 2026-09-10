/*
 * jni_shim.c -- fake JNI environment for the loaded Android game
 *
 * Android JNI works through double-indirection:
 *   JavaVM *vm;   vm->GetEnv(vm, &env, version)
 *   JNIEnv *env;  env->FindClass(env, "com/foo/Bar")
 *
 * Both vm and env are pointers to a pointer to a function table.
 * We create large stub vtables that return 0/NULL for everything,
 * with specific overrides for the methods the game actually uses.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "jni_shim.h"
#include "util.h"

#define JNI_VTABLE_SIZE 512

/* Nome do OBB exigido pela engine (versionCode 154 = Castle of Illusion 1.4.5).
 * O instalador (NXExtract) publica o arquivo do usuario com exatamente este
 * nome em <gamedir>/obb/. */
#define TS_OBB_BASENAME "main.31.com.devolver.titansouls.obb"

typedef int jint;

static uintptr_t jni_env_vtable[JNI_VTABLE_SIZE];
static void *jni_env_ptr;

static uintptr_t java_vm_vtable[JNI_VTABLE_SIZE];
static void *java_vm_ptr;

/* ---- Tagged method/field IDs ---- */
enum {
  MID_UNKNOWN = 0,
  MID_GET_STORAGE_DIR,
  MID_GET_PACK_NAME,
  MID_SET_ACTIVITY,
  MID_ERROR_DIALOG,
  MID_GET_CLASS_LOADER,
  MID_LOAD_CLASS,
  MID_AUDIO_STREAM_PARAMS,
  MID_BATTERY_LEVEL,
  MID_BATTERY_STATUS,
  MID_GET_RENDERER_TYPE,
  MID_GET_OBB_PATH,
  MID_PACKAGE_NAME,
  MID_VERSION_NAME,
  MID_APK_PATH,
  MID_DEVICE_LANG,
  MID_IS_FIRETV,
  MID_IS_HEADSET,
  MID_GENERIC,
  /* Contrato Java REAL de com/devolver/titansouls/TVActivity (lido do dex
   * deste APK, nao suposto). O android_main da engine fica em laco chamando
   * isStaticDataResolved()/checkIfReady() ate' os dois responderem "OK" — e'
   * a Activity que posta essa transicao, e sem ela o jogo nunca sai do laco
   * de espera (nao e' tela preta, e' o fluxo nativo esperando o dono dele). */
  MID_TS_STATIC_RESOLVED,
  MID_TS_CHECK_READY,
  MID_TS_CONTENT_DIR,
  MID_TS_LANG,
  MID_TS_MODEL,
  MID_TS_CONTROLLERS,
  MID_TS_AMAZON_SIGNED,
  MID_TS_NETWORK,
  MID_TS_OUYA_FILE_EXISTS,
  MID_TS_GET_OUYA,
  MID_TS_STORE_OUYA,
  MID_TS_TERMINATE,
  FID_OBB_VERSIONCODE,
  FID_GENERIC,
};

static int g_method_tags[FID_GENERIC + 1]; /* unique addresses used as method IDs */

/* ---- Configurable package/OBB ---- */
/* contentDir do Java = ApplicationInfo.dataDir. Aqui e' a pasta interna do
 * port, que existe e e' gravavel. */
static const char *ts_content_dir(void) {
  extern const char *ts_paths_internal(void);
  const char *p = ts_paths_internal();
  return (p && p[0]) ? p : ".";
}

/* O Java conta InputDevices com SOURCE_GAMEPAD. O equivalente honesto aqui e'
 * o numero de joysticks que o SDL enxerga — nunca um numero cravado, senao a
 * tela de "conecte um controle" mente nos dois sentidos. */
static const char *jni_shim_pad_count_str(void) {
  extern int SDL_NumJoysticks(void);
  static char buf[16];
  int n = SDL_NumJoysticks();
  if (n < 0) n = 0;
  snprintf(buf, sizeof(buf), "%d", n);
  return buf;
}

static const char *g_package_name = "com.devolver.titansouls";
static int g_obb_version = 31;

void jni_shim_set_package(const char *package_name, int obb_version) {
  g_package_name = package_name;
  g_obb_version = obb_version;
}

/* ---- Fake jstring tracking ---- */
/* We return tagged pointers as jstrings and map them to C strings */
#define MAX_JSTRINGS 32
static struct {
  void *handle;
  const char *value;
} g_jstrings[MAX_JSTRINGS];
static int g_jstring_count = 0;

static void *make_jstring(const char *value) {
  static char jstring_storage[MAX_JSTRINGS];
  if (g_jstring_count >= MAX_JSTRINGS)
    g_jstring_count = 0; /* wrap around */
  int idx = g_jstring_count++;
  g_jstrings[idx].handle = &jstring_storage[idx];
  g_jstrings[idx].value = value;
  return g_jstrings[idx].handle;
}

static const char *resolve_jstring(void *jstr) {
  for (int i = 0; i < g_jstring_count; i++) {
    if (g_jstrings[i].handle == jstr)
      return g_jstrings[i].value;
  }
  return "";
}

/* ---- Generic stub ---- */
static intptr_t jni_stub(void) { return 0; }

/* ---- JNIEnv functions ---- */

static jint jni_GetVersion(void *env) {
  (void)env;
  return 0x00010006;
}

static void *jni_FindClass(void *env, const char *name) {
  (void)env;
  debugPrintf("jni_shim: FindClass(%s)\n", name);
  static int fake_class;
  return &fake_class;
}

static void *jni_GetMethodID(void *env, void *clazz, const char *name,
                             const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetMethodID(%s, %s)\n", name, sig);
  if (strcmp(name, "getClassLoader") == 0)
    return &g_method_tags[MID_GET_CLASS_LOADER];
  if (strcmp(name, "loadClass") == 0)
    return &g_method_tags[MID_LOAD_CLASS];
  if (strcmp(name, "GetDefaultAudioStreamParameters") == 0)
    return &g_method_tags[MID_AUDIO_STREAM_PARAMS];
  /* 🔋 bateria: a engine 10tons reduz pra 30fps em modo economia. Respondemos
   * 100% + carregando (level garbage/0 = power-save = cap de fps!). */
  if (strcmp(name, "getBatteryLevel") == 0)
    return &g_method_tags[MID_BATTERY_LEVEL];
  if (strcmp(name, "getBatteryStatus") == 0)
    return &g_method_tags[MID_BATTERY_STATUS];
  /* 🟢 GetRendererType: o motor pergunta ao Java qual renderer usar e compara com
   * "opengl"; se não casar, tenta VULKAN (que não temos) -> adapter não inicializa
   * -> "Failed to pre-initialize NEXUS" -> crash. Forçar "opengl" = caminho GL. */
  if (strcmp(name, "GetRendererType") == 0)
    return &g_method_tags[MID_GET_RENDERER_TYPE];
  if (strcmp(name, "getObbPath") == 0)
    return &g_method_tags[MID_GET_OBB_PATH];
  return &g_method_tags[MID_GENERIC];
}

/* Caminho do OBB no device (a engine fopen()a EXATAMENTE esta string, por isso
 * ela precisa ser absoluta e valida em qualquer CFW).
 *
 * MULTI-DEVICE: nunca cravar /storage/roms/... — o diretorio de ports muda por
 * firmware (/roms, /roms2, /storage/roms, /mnt/mmc/ports, /userdata/roms...).
 * Ordem de resolucao:
 *   1. COI_OBB          — override explicito (diagnostico/instalacao exotica);
 *   2. COI_GAMEDIR/obb/ — o launcher exporta o gamedir real que ele resolveu;
 *   3. cwd/obb/         — o launcher sempre faz cd no gamedir antes de exec.
 * Resolvido UMA vez e cacheado: a engine chama getObbPath varias vezes. */
static const char *coi_obb_path(void) {
  extern char *getenv(const char *);
  static char cached[1024];
  static int resolved = 0;
  if (resolved)
    return cached;
  resolved = 1;

  const char *e = getenv("TS_OBB");
  if (e && *e) {
    snprintf(cached, sizeof(cached), "%s", e);
    return cached;
  }

  const char *gd = getenv("TS_GAMEDIR");
  if (!gd || !*gd) {
    static char cwd[768];
    gd = getcwd(cwd, sizeof(cwd)) ? cwd : ".";
  }
  snprintf(cached, sizeof(cached),
           "%s/sdcard/Android/obb/com.devolver.titansouls/" TS_OBB_BASENAME, gd);
  debugPrintf("jni_shim: OBB resolvido -> %s\n", cached);
  return cached;
}

static void *jni_GetStaticMethodID(void *env, void *clazz, const char *name,
                                   const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetStaticMethodID(%s, %s)\n", name, sig);
  if (strcmp(name, "getStorageDir") == 0)
    return &g_method_tags[MID_GET_STORAGE_DIR];
  if (strcmp(name, "getPackName") == 0)
    return &g_method_tags[MID_GET_PACK_NAME];
  if (strcmp(name, "setActivity") == 0)
    return &g_method_tags[MID_SET_ACTIVITY];
  if (strcmp(name, "errorDialog") == 0)
    return &g_method_tags[MID_ERROR_DIALOG];
  if (strcmp(name, "getObbPath") == 0)
    return &g_method_tags[MID_GET_OBB_PATH];
  /* tabela JNI do coi_nx (port Switch funcional) */
  if (strcmp(name, "getThePackageName") == 0)
    return &g_method_tags[MID_PACKAGE_NAME];
  if (strcmp(name, "getVersionName") == 0)
    return &g_method_tags[MID_VERSION_NAME];
  if (strcmp(name, "getApkPath") == 0)
    return &g_method_tags[MID_APK_PATH];
  if (strcmp(name, "getDeviceLanguage") == 0)
    return &g_method_tags[MID_DEVICE_LANG];
  if (strcmp(name, "isFireTV") == 0)
    return &g_method_tags[MID_IS_FIRETV];
  if (strcmp(name, "isHeadSetConnected") == 0)
    return &g_method_tags[MID_IS_HEADSET];
  /* TVActivity (Titan Souls) */
  if (strcmp(name, "isStaticDataResolved") == 0)
    return &g_method_tags[MID_TS_STATIC_RESOLVED];
  if (strcmp(name, "checkIfReady") == 0)
    return &g_method_tags[MID_TS_CHECK_READY];
  if (strcmp(name, "getContentDir") == 0)
    return &g_method_tags[MID_TS_CONTENT_DIR];
  if (strcmp(name, "getLang") == 0)
    return &g_method_tags[MID_TS_LANG];
  if (strcmp(name, "getModel") == 0)
    return &g_method_tags[MID_TS_MODEL];
  if (strcmp(name, "getGameControllersNumber") == 0)
    return &g_method_tags[MID_TS_CONTROLLERS];
  if (strcmp(name, "isAmazonSignedIn") == 0)
    return &g_method_tags[MID_TS_AMAZON_SIGNED];
  if (strcmp(name, "isNetworkAvailable") == 0)
    return &g_method_tags[MID_TS_NETWORK];
  if (strcmp(name, "ouyaFileExists") == 0)
    return &g_method_tags[MID_TS_OUYA_FILE_EXISTS];
  if (strcmp(name, "getOuyaData") == 0)
    return &g_method_tags[MID_TS_GET_OUYA];
  if (strcmp(name, "storeOuyaData") == 0)
    return &g_method_tags[MID_TS_STORE_OUYA];
  if (strcmp(name, "terminateApp") == 0)
    return &g_method_tags[MID_TS_TERMINATE];
  return &g_method_tags[MID_GENERIC];
}

static void *jni_GetFieldID(void *env, void *clazz, const char *name,
                            const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetFieldID(%s, %s)\n", name, sig);
  return &g_method_tags[FID_GENERIC];
}

static void *jni_GetStaticFieldID(void *env, void *clazz, const char *name,
                                  const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetStaticFieldID(%s, %s)\n", name, sig);
  if (strcmp(name, "OBB_VERSIONCODE") == 0)
    return &g_method_tags[FID_OBB_VERSIONCODE];
  return &g_method_tags[FID_GENERIC];
}

/* Array fake de parâmetros de áudio: [sampleRate, framesPerBurst]
 * (Oboe/engine pedem via GetDefaultAudioStreamParameters()[I]) */
static jint g_audio_params[2] = {44100, 1024};

/* CallObjectMethod (index 36) - variadic */
static void *jni_CallObjectMethod(void *env, void *obj, void *methodID, ...) {
  (void)env;
  (void)obj;
  debugPrintf("jni_shim: CallObjectMethod(mid=%p)\n", methodID);
  if (methodID == &g_method_tags[MID_AUDIO_STREAM_PARAMS]) {
    debugPrintf("jni_shim:   -> AudioStreamParameters jintArray {%d,%d}\n",
                g_audio_params[0], g_audio_params[1]);
    return g_audio_params;
  }
  if (methodID == &g_method_tags[MID_GET_RENDERER_TYPE]) {
    debugPrintf("jni_shim:   -> GetRendererType = \"opengl\"\n");
    return make_jstring("opengl");
  }
  if (methodID == &g_method_tags[MID_GET_OBB_PATH]) {
    debugPrintf("jni_shim:   -> getObbPath = \"%s\"\n", coi_obb_path());
    return make_jstring(coi_obb_path());
  }
  static int fake_obj;
  return &fake_obj;
}

/* NewObject (index 28/29/30) - Paddleboat constrói GameControllerManager Java;
 * NULL aqui = init -2002. Devolvemos objeto fake não-nulo. */
static void *jni_NewObject(void *env, void *clazz, void *methodID, ...) {
  (void)env; (void)clazz;
  debugPrintf("jni_shim: NewObject(mid=%p) -> fake\n", methodID);
  static int fake_new_obj;
  return &fake_new_obj;
}

/* ---- Arrays fake genéricos (int/float) p/ JNI Region/Elements ---- */
#define MAX_FAKE_ARRAYS 16
static struct {
  void *handle;
  const void *data; /* int32 ou float */
  int len;
} g_fake_arrays[MAX_FAKE_ARRAYS];
static int g_fake_array_count = 0;
void *jni_shim_make_array(const void *data, int len) {
  static char handles[MAX_FAKE_ARRAYS];
  if (g_fake_array_count >= MAX_FAKE_ARRAYS) g_fake_array_count = 0;
  int i = g_fake_array_count++;
  g_fake_arrays[i].handle = &handles[i];
  g_fake_arrays[i].data = data;
  g_fake_arrays[i].len = len;
  return g_fake_arrays[i].handle;
}
static int find_fake_array(void *h) {
  for (int i = 0; i < g_fake_array_count; i++)
    if (g_fake_arrays[i].handle == h) return i;
  return -1;
}

/* CallBooleanMethod (index 49) */
static unsigned char jni_CallBooleanMethod(void *env, void *obj,
                                           void *methodID, ...) {
  (void)env;
  (void)obj;
  (void)methodID;
  return 0;
}

/* CallIntMethod (index 61) */
static jint jni_CallIntMethod(void *env, void *obj, void *methodID, ...) {
  (void)env;
  (void)obj;
  if (methodID == &g_method_tags[MID_BATTERY_STATUS])
    return 2; /* BATTERY_STATUS_CHARGING — sem power-save/cap de fps */
  return 0;
}

/* CallFloatMethod (index 55-57) — sem isto o retorno float era LIXO em s0
 * (getBatteryLevel lia qualquer coisa -> engine entrava em power-save 30fps) */
static float jni_CallFloatMethod(void *env, void *obj, void *methodID, ...) {
  (void)env;
  (void)obj;
  if (methodID == &g_method_tags[MID_BATTERY_LEVEL])
    return 100.0f; /* 100% (escala 0..1 ou 0..100, ambas acima do threshold) */
  return 0.0f;
}

/* CallVoidMethod (index 94) */
static void jni_CallVoidMethod(void *env, void *obj, void *methodID, ...) {
  (void)env;
  (void)obj;
  (void)methodID;
}

/* CallStaticObjectMethod (index 113) */
static void *jni_CallStaticObjectMethod(void *env, void *clazz,
                                        void *methodID, ...) {
  (void)env;
  (void)clazz;

  if (methodID == &g_method_tags[MID_GET_STORAGE_DIR]) {
    debugPrintf("jni_shim: CallStaticObjectMethod -> getStorageDir = \".\"\n");
    return make_jstring(".");
  }
  if (methodID == &g_method_tags[MID_GET_PACK_NAME]) {
    debugPrintf(
        "jni_shim: CallStaticObjectMethod -> getPackName = \"%s\"\n",
        g_package_name);
    return make_jstring(g_package_name);
  }
  if (methodID == &g_method_tags[MID_GET_OBB_PATH]) {
    debugPrintf("jni_shim: CallStaticObjectMethod -> getObbPath = \"%s\"\n",
                coi_obb_path());
    return make_jstring(coi_obb_path());
  }
  if (methodID == &g_method_tags[MID_PACKAGE_NAME])
    return make_jstring(g_package_name);
  if (methodID == &g_method_tags[MID_VERSION_NAME])
    return make_jstring("1.4.5");
  if (methodID == &g_method_tags[MID_APK_PATH])
    return make_jstring(".");
  if (methodID == &g_method_tags[MID_DEVICE_LANG])
    return make_jstring("en");

  /* ---- TVActivity: as respostas SAO as do Java deste APK ---- */
  if (methodID == &g_method_tags[MID_TS_STATIC_RESOLVED]) {
    /* onCreate faz `staticDataResolved = true` incondicionalmente. */
    return make_jstring("OK");
  }
  if (methodID == &g_method_tags[MID_TS_CHECK_READY]) {
    /* _ready vira "OK" quando a FileTask termina; aqui o dado ja' esta' no
     * disco antes de a engine subir, entao a resposta e' imediata. */
    return make_jstring("OK");
  }
  if (methodID == &g_method_tags[MID_TS_CONTENT_DIR])
    return make_jstring(ts_content_dir());
  if (methodID == &g_method_tags[MID_TS_LANG]) {
    /* Fallback for callers outside the hooked AgLocale path; the native menu
     * owns manual language selection. */
    return make_jstring("en");
  }
  if (methodID == &g_method_tags[MID_TS_MODEL]) {
    /* "Unknown" = caminho generico do Android (fontsmall.png +
     * Google_Gamepad_*.png). Ouya/AmazonTV/Shield trocariam o layout de
     * botoes por um aparelho que nao e' este. */
    return make_jstring("Unknown");
  }
  if (methodID == &g_method_tags[MID_TS_CONTROLLERS]) {
    /* O Java conta InputDevices com SOURCE_GAMEPAD; aqui o pad e' o do CFW. */
    return make_jstring(jni_shim_pad_count_str());
  }
  if (methodID == &g_method_tags[MID_TS_AMAZON_SIGNED])
    return make_jstring("Not signed in");
  if (methodID == &g_method_tags[MID_TS_NETWORK])
    return make_jstring("NO");
  if (methodID == &g_method_tags[MID_TS_OUYA_FILE_EXISTS])
    return make_jstring("NO");
  if (methodID == &g_method_tags[MID_TS_GET_OUYA])
    return make_jstring("");
  if (methodID == &g_method_tags[MID_TS_STORE_OUYA])
    return make_jstring("OK");
  if (methodID == &g_method_tags[MID_TS_TERMINATE]) {
    /* terminateApp() e' o Process.killProcess do Java: a engine ja' salvou. */
    debugPrintf("jni_shim: terminateApp -> saindo\n");
    _exit(0);
  }

  debugPrintf("jni_shim: CallStaticObjectMethod(mid=%p) -> NULL\n", methodID);
  static int fake_result;
  return &fake_result;
}

/* CallStaticBooleanMethod (index 124) */
static unsigned char jni_CallStaticBooleanMethod(void *env, void *clazz,
                                                 void *methodID, ...) {
  (void)env;
  (void)clazz;
  /* isFireTV: COI_TOUCH=1 -> false (modo touch). Default agora = TRUE
   * (modo console/gamepad): engine processa keys via AndroidInput::OnKeyDown
   * (caminho nativo de controle Xbox); IsControllerConnected ja retorna 1
   * hardcoded, entao NAO trava. A trava anterior era a struct (corrigida). */
  if (methodID == &g_method_tags[MID_IS_FIRETV]) {
    extern char *getenv(const char *);
    int v = getenv("COI_TOUCH") ? 0 : 1;
    debugPrintf("jni_shim: CallStaticBooleanMethod isFireTV -> %d\n", v);
    return v;
  }
  if (methodID == &g_method_tags[MID_IS_HEADSET])
    return 0;
  debugPrintf("jni_shim: CallStaticBooleanMethod(mid=%p) -> 1\n", methodID);
  return 1;
}

/* CallStaticIntMethod (index 136) */
static jint jni_CallStaticIntMethod(void *env, void *clazz, void *methodID,
                                    ...) {
  (void)env;
  (void)clazz;
  (void)methodID;
  return 0;
}

/* CallStaticVoidMethod (index 145) */
static void jni_CallStaticVoidMethod(void *env, void *clazz, void *methodID,
                                     ...) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: CallStaticVoidMethod(mid=%p)\n", methodID);
}

/* GetStaticIntField (index 155) */
static jint jni_GetStaticIntField(void *env, void *clazz, void *fieldID) {
  (void)env;
  (void)clazz;

  if (fieldID == &g_method_tags[FID_OBB_VERSIONCODE]) {
    debugPrintf("jni_shim: GetStaticIntField -> OBB_VERSIONCODE = %d\n",
                g_obb_version);
    return g_obb_version;
  }
  debugPrintf("jni_shim: GetStaticIntField(fid=%p) -> 0\n", fieldID);
  return 0;
}

/* GetStaticObjectField (index 156) */
static void *jni_GetStaticObjectField(void *env, void *clazz, void *fieldID) {
  (void)env;
  (void)clazz;
  (void)fieldID;
  debugPrintf("jni_shim: GetStaticObjectField -> NULL\n");
  static int fake;
  return &fake;
}

/* NewStringUTF (index 167) */
static void *jni_NewStringUTF(void *env, const char *str) {
  (void)env;
  debugPrintf("jni_shim: NewStringUTF(%s)\n", str ? str : "(null)");
  return make_jstring(str ? str : "");
}

/* GetStringUTFLength (index 168) */
static jint jni_GetStringUTFLength(void *env, void *jstr) {
  (void)env;
  const char *s = resolve_jstring(jstr);
  return (jint)strlen(s);
}

/* GetStringUTFChars (index 169) */
static const char *jni_GetStringUTFChars(void *env, void *jstr,
                                         void *isCopy) {
  (void)env;
  (void)isCopy;
  const char *s = resolve_jstring(jstr);
  debugPrintf("jni_shim: GetStringUTFChars -> \"%s\"\n", s);
  return s;
}

/* ReleaseStringUTFChars (index 170) */
static void jni_ReleaseStringUTFChars(void *env, void *jstr,
                                      const char *chars) {
  (void)env;
  (void)jstr;
  (void)chars;
}

/* Ref management */
static void *jni_NewGlobalRef(void *env, void *obj) {
  (void)env;
  return obj;
}
static void *jni_NewLocalRef(void *env, void *obj) {
  (void)env;
  return obj;
}
static void jni_DeleteGlobalRef(void *env, void *obj) {
  (void)env;
  (void)obj;
}
static void jni_DeleteLocalRef(void *env, void *obj) {
  (void)env;
  (void)obj;
}
static void *jni_GetObjectClass(void *env, void *obj) {
  (void)env;
  (void)obj;
  static int fake_obj_class;
  return &fake_obj_class;
}

/* Exception handling */
static unsigned char jni_ExceptionCheck(void *env) {
  (void)env;
  return 0;
}
static void jni_ExceptionClear(void *env) { (void)env; }
static void *jni_ExceptionOccurred(void *env) {
  (void)env;
  return 0;
}

/* Array */
static jint jni_GetArrayLength(void *env, void *array) {
  (void)env;
  if (array == g_audio_params) return 2;
  int i = find_fake_array(array);
  if (i >= 0) return g_fake_arrays[i].len;
  return 0;
}
static void jni_GetIntArrayRegion(void *env, void *array, jint start, jint len,
                                  jint *buf) {
  (void)env;
  int i = find_fake_array(array);
  debugPrintf("jni_shim: GetIntArrayRegion(%p, %d, %d) idx=%d\n", array,
              (int)start, (int)len, i);
  if (i < 0 || !buf) return;
  const jint *d = (const jint *)g_fake_arrays[i].data;
  for (jint k = 0; k < len && (start + k) < g_fake_arrays[i].len; k++)
    buf[k] = d[start + k];
}
static void jni_GetFloatArrayRegion(void *env, void *array, jint start,
                                    jint len, float *buf) {
  (void)env;
  int i = find_fake_array(array);
  if (i < 0 || !buf) return;
  const float *d = (const float *)g_fake_arrays[i].data;
  for (jint k = 0; k < len && (start + k) < g_fake_arrays[i].len; k++)
    buf[k] = d[start + k];
}
static jint *jni_GetIntArrayElements(void *env, void *array, unsigned char *isCopy) {
  (void)env;
  if (isCopy) *isCopy = 0;
  debugPrintf("jni_shim: GetIntArrayElements(%p)\n", array);
  if (array == g_audio_params) return g_audio_params;
  static jint zeros[8];
  return zeros;
}
static void jni_ReleaseIntArrayElements(void *env, void *array, jint *elems,
                                        jint mode) {
  (void)env; (void)array; (void)elems; (void)mode;
}

/* ---- JavaVM functions ---- */

static jint vm_DestroyJavaVM(void *vm) {
  (void)vm;
  return 0;
}

static jint vm_AttachCurrentThread(void *vm, void **penv, void *args) {
  (void)vm;
  (void)args;
  debugPrintf("jni_shim: AttachCurrentThread\n");
  if (penv)
    *penv = &jni_env_ptr;
  return 0;
}

static jint vm_DetachCurrentThread(void *vm) {
  (void)vm;
  return 0;
}

static jint vm_GetEnv(void *vm, void **penv, jint version) {
  (void)vm;
  (void)version;
  debugPrintf("jni_shim: GetEnv(version=0x%x)\n", version);
  if (penv)
    *penv = &jni_env_ptr;
  return 0;
}

static jint vm_AttachCurrentThreadAsDaemon(void *vm, void **penv, void *args) {
  (void)vm;
  (void)args;
  if (penv)
    *penv = &jni_env_ptr;
  return 0;
}

/* ---- Init ---- */

void jni_shim_init(void **out_vm, void **out_env) {
  for (int i = 0; i < JNI_VTABLE_SIZE; i++) {
    jni_env_vtable[i] = (uintptr_t)jni_stub;
    java_vm_vtable[i] = (uintptr_t)jni_stub;
  }

  /*
   * JNIEnv vtable indices from Android NDK jni.h.
   * C++ wrappers in the .so call the *V (va_list) variants,
   * so we must set both the variadic and V slots.
   *
   *   0-3:   reserved
   *   4:     GetVersion
   *   6:     FindClass
   *  15:     ExceptionOccurred
   *  17:     ExceptionClear
   *  21:     NewGlobalRef
   *  22:     DeleteGlobalRef
   *  23:     DeleteLocalRef
   *  25:     NewLocalRef
   *  31:     GetObjectClass
   *  33:     GetMethodID
   *  34/35:  CallObjectMethod / V
   *  37/38:  CallBooleanMethod / V
   *  49/50:  CallIntMethod / V
   *  61/62:  CallVoidMethod / V
   *  94:     GetFieldID
   * 113:     GetStaticMethodID
   * 114/115: CallStaticObjectMethod / V
   * 117/118: CallStaticBooleanMethod / V
   * 129/130: CallStaticIntMethod / V
   * 141/142: CallStaticVoidMethod / V
   * 144:     GetStaticFieldID
   * 145:     GetStaticObjectField
   * 150:     GetStaticIntField
   * 167:     NewStringUTF
   * 168:     GetStringUTFLength
   * 169:     GetStringUTFChars
   * 170:     ReleaseStringUTFChars
   * 171:     GetArrayLength
   * 205:     ExceptionCheck
   */
  jni_env_vtable[4] = (uintptr_t)jni_GetVersion;
  jni_env_vtable[6] = (uintptr_t)jni_FindClass;
  jni_env_vtable[15] = (uintptr_t)jni_ExceptionOccurred;
  jni_env_vtable[17] = (uintptr_t)jni_ExceptionClear;
  jni_env_vtable[21] = (uintptr_t)jni_NewGlobalRef;
  jni_env_vtable[22] = (uintptr_t)jni_DeleteGlobalRef;
  jni_env_vtable[23] = (uintptr_t)jni_DeleteLocalRef;
  jni_env_vtable[25] = (uintptr_t)jni_NewLocalRef;
  jni_env_vtable[28] = (uintptr_t)jni_NewObject;
  jni_env_vtable[29] = (uintptr_t)jni_NewObject;           /* V variant */
  jni_env_vtable[30] = (uintptr_t)jni_NewObject;           /* A variant */
  jni_env_vtable[31] = (uintptr_t)jni_GetObjectClass;
  jni_env_vtable[33] = (uintptr_t)jni_GetMethodID;
  jni_env_vtable[34] = (uintptr_t)jni_CallObjectMethod;
  jni_env_vtable[35] = (uintptr_t)jni_CallObjectMethod;    /* V variant */
  jni_env_vtable[37] = (uintptr_t)jni_CallBooleanMethod;
  jni_env_vtable[38] = (uintptr_t)jni_CallBooleanMethod;   /* V */
  jni_env_vtable[49] = (uintptr_t)jni_CallIntMethod;
  jni_env_vtable[50] = (uintptr_t)jni_CallIntMethod;       /* V */
  jni_env_vtable[55] = (uintptr_t)jni_CallFloatMethod;
  jni_env_vtable[56] = (uintptr_t)jni_CallFloatMethod;     /* V */
  jni_env_vtable[57] = (uintptr_t)jni_CallFloatMethod;     /* A */
  jni_env_vtable[61] = (uintptr_t)jni_CallVoidMethod;
  jni_env_vtable[62] = (uintptr_t)jni_CallVoidMethod;      /* V */
  jni_env_vtable[94] = (uintptr_t)jni_GetFieldID;
  jni_env_vtable[113] = (uintptr_t)jni_GetStaticMethodID;
  jni_env_vtable[114] = (uintptr_t)jni_CallStaticObjectMethod;
  jni_env_vtable[115] = (uintptr_t)jni_CallStaticObjectMethod; /* V */
  jni_env_vtable[117] = (uintptr_t)jni_CallStaticBooleanMethod;
  jni_env_vtable[118] = (uintptr_t)jni_CallStaticBooleanMethod; /* V */
  jni_env_vtable[129] = (uintptr_t)jni_CallStaticIntMethod;
  jni_env_vtable[130] = (uintptr_t)jni_CallStaticIntMethod; /* V */
  jni_env_vtable[141] = (uintptr_t)jni_CallStaticVoidMethod;
  jni_env_vtable[142] = (uintptr_t)jni_CallStaticVoidMethod; /* V */
  jni_env_vtable[144] = (uintptr_t)jni_GetStaticFieldID;
  jni_env_vtable[145] = (uintptr_t)jni_GetStaticObjectField;
  jni_env_vtable[150] = (uintptr_t)jni_GetStaticIntField;
  jni_env_vtable[167] = (uintptr_t)jni_NewStringUTF;
  jni_env_vtable[168] = (uintptr_t)jni_GetStringUTFLength;
  jni_env_vtable[169] = (uintptr_t)jni_GetStringUTFChars;
  jni_env_vtable[170] = (uintptr_t)jni_ReleaseStringUTFChars;
  jni_env_vtable[171] = (uintptr_t)jni_GetArrayLength;
  jni_env_vtable[187] = (uintptr_t)jni_GetIntArrayElements;     /* GetIntArrayElements */
  jni_env_vtable[195] = (uintptr_t)jni_ReleaseIntArrayElements; /* ReleaseIntArrayElements */
  jni_env_vtable[203] = (uintptr_t)jni_GetIntArrayRegion;       /* GetIntArrayRegion */
  jni_env_vtable[205] = (uintptr_t)jni_GetFloatArrayRegion;     /* GetFloatArrayRegion */
  jni_env_vtable[228] = (uintptr_t)jni_ExceptionCheck;          /* ExceptionCheck (228 na spec) */

  jni_env_ptr = jni_env_vtable;

  /* JavaVM vtable */
  java_vm_vtable[3] = (uintptr_t)vm_DestroyJavaVM;
  java_vm_vtable[4] = (uintptr_t)vm_AttachCurrentThread;
  java_vm_vtable[5] = (uintptr_t)vm_DetachCurrentThread;
  java_vm_vtable[6] = (uintptr_t)vm_GetEnv;
  java_vm_vtable[7] = (uintptr_t)vm_AttachCurrentThreadAsDaemon;

  java_vm_ptr = java_vm_vtable;

  if (out_vm)
    *out_vm = &java_vm_ptr;
  if (out_env)
    *out_env = &jni_env_ptr;

  debugPrintf("jni_shim: Initialized (vm=%p, env=%p)\n", &java_vm_ptr,
              &jni_env_ptr);
}
