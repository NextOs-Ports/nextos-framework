/*
 * jni_shim.c -- fake JNI environment for Syberia
 *
 * Android JNI works through double-indirection:
 *   JavaVM *vm;   vm->GetEnv(vm, &env, version)
 *   JNIEnv *env;  env->FindClass(env, "com/foo/Bar")
 *
 * Both vm and env are pointers to a pointer to a function table.
 * We create large stub vtables that return 0/NULL for everything,
 * with specific overrides for methods Syberia actually uses.
 */

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jni_shim.h"
#include "sonic_audio.h"
#include "util.h"

#define JNI_VTABLE_SIZE 512

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
  MID_GET_DEFAULT_LOCALE,
  MID_GET_LANGUAGE,
  MID_GET_COUNTRY,
  MID_LOCAL_PATH,   /* Sonic4 F2F: getLocalPath/getBundlePath -> dir gravável */
  MID_REGION,       /* getRegionCode -> "US" */
  MID_LANG_CODE,    /* getLanguageCode -> "en" */
  MID_SHOW_INTERSTITIAL, /* Sonic4: Java showInterstitial(I)V -> simular ad fechado */
  MID_AUDIO_PAUSE_SOUND,
  MID_AUDIO_RESUME_SOUND,
  MID_AUDIO_STOP_SOUND,
  MID_AUDIO_PLAY_SOUND,
  MID_AUDIO_SET_VOLUME,
  MID_AUDIO_MUSIC_SET_DATA_SOURCE,
  MID_AUDIO_MUSIC_START,
  MID_AUDIO_MUSIC_VOLUME,
  MID_AUDIO_MUSIC_STOP,
  MID_AUDIO_MUSIC_PAUSE,
  MID_AUDIO_MUSIC_SET_LOOP,
  MID_AUDIO_SET_MAX_VOLUME,
  MID_AUDIO_SP_RESET,
  MID_AUDIO_MP_RESET,
  MID_AUDIO_MP_RESET_ID,
  MID_AUDIO_ASYNC_BUILD_SP_DATA,
  MID_AUDIO_ASYNC_BUILD_BGM_DATA,
  MID_AUDIO_IS_DONE_BUILD_SP,
  MID_AUDIO_IS_DONE_BUILD_BGM,
  MID_AUDIO_GET_MUSIC_STATE,
  MID_AUDIO_VIDEO_SET_DATA_SOURCE,
  MID_AUDIO_VIDEO_IS_PLAYING,
  MID_AUDIO_OPEN_UNLOCK_WEB,
  MID_AUDIO_OPEN_UNLOCK_WEB_EP1,
  MID_AUDIO_SET_ADS_SHOW,
  MID_GENERIC,
  FID_OBB_VERSIONCODE,
  FID_GENERIC,
};

static int g_method_tags[64]; /* unique addresses used as method IDs */
/* Sonic4: caminho local gravável p/ o F2F (Sonic4ep2.f2f). Default = CWD. */
char jni_local_path[512] = ".";
void jni_shim_set_local_path(const char *p) {
  if (p) { strncpy(jni_local_path, p, sizeof(jni_local_path) - 1);
           jni_local_path[sizeof(jni_local_path) - 1] = 0; }
}

/* ---- Configurable package/OBB ---- */
static const char *g_package_name = "com.microids.syberia";
static int g_obb_version = 12;

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
static int g_audio_sp_built = 0;
static int g_audio_bgm_built = 0;

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

static int audio_log_enabled(void) {
  return getenv("SONIC_AUDIOLOG") != NULL;
}

static void audioPrintf(const char *text, ...) {
  if (!audio_log_enabled()) return;
  va_list ap;
  va_start(ap, text);
  vfprintf(stderr, text, ap);
  va_end(ap);
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
  /* 🇬🇧 idioma: SEMPRE inglês (regra do projeto). Locale.getLanguage()->"en". */
  if (strcmp(name, "getLanguage") == 0)
    return &g_method_tags[MID_GET_LANGUAGE];
  if (strcmp(name, "getCountry") == 0)
    return &g_method_tags[MID_GET_COUNTRY];
  /* Sonic4 F2F: paths e região/idioma (senão getLocalPath="" -> "/Sonic4ep2.f2f"
     no root, não-gravável -> save F2F falha -> trava no boot). */
  if (strcmp(name, "getLocalPath") == 0 || strcmp(name, "getBundlePath") == 0)
    return &g_method_tags[MID_LOCAL_PATH];
  if (strcmp(name, "getRegionCode") == 0)
    return &g_method_tags[MID_REGION];
  if (strcmp(name, "getLanguageCode") == 0)
    return &g_method_tags[MID_LANG_CODE];
  /* 🔑 Sonic4: ao selecionar Start, o engine (Android_showMoPubInterstitial) chama
     o método Java showInterstitial(I)V LOGO APÓS armazenar o callback de transição
     (ir pro world map). Sem ad real, simulamos o "ad fechou" disparando
     callbackInterstitialAds 1x -> invoca o callback armazenado -> world map carrega.
     Sinalizamos via jni_inter_pending; o disparo real é no main loop (não reentrante). */
  if (strcmp(name, "showInterstitial") == 0)
    return &g_method_tags[MID_SHOW_INTERSTITIAL];
  /* Sonic4 AudioHelper: a libfox usa uma classe Java como ponte para
     SoundPool/MediaPlayer. No port, estes IDs viram callbacks locais. */
  if (strcmp(name, "PauseSound") == 0)
    return &g_method_tags[MID_AUDIO_PAUSE_SOUND];
  if (strcmp(name, "ResumeSound") == 0)
    return &g_method_tags[MID_AUDIO_RESUME_SOUND];
  if (strcmp(name, "StopSound") == 0)
    return &g_method_tags[MID_AUDIO_STOP_SOUND];
  if (strcmp(name, "PlaySound") == 0)
    return &g_method_tags[MID_AUDIO_PLAY_SOUND];
  if (strcmp(name, "SetVolume") == 0)
    return &g_method_tags[MID_AUDIO_SET_VOLUME];
  if (strcmp(name, "MusicSetDataSource") == 0)
    return &g_method_tags[MID_AUDIO_MUSIC_SET_DATA_SOURCE];
  if (strcmp(name, "MusicStart") == 0)
    return &g_method_tags[MID_AUDIO_MUSIC_START];
  if (strcmp(name, "MusicVolume") == 0)
    return &g_method_tags[MID_AUDIO_MUSIC_VOLUME];
  if (strcmp(name, "MusicStop") == 0)
    return &g_method_tags[MID_AUDIO_MUSIC_STOP];
  if (strcmp(name, "MusicPause") == 0)
    return &g_method_tags[MID_AUDIO_MUSIC_PAUSE];
  if (strcmp(name, "MusicSetLoopFlag") == 0)
    return &g_method_tags[MID_AUDIO_MUSIC_SET_LOOP];
  if (strcmp(name, "SetMaxVolume") == 0)
    return &g_method_tags[MID_AUDIO_SET_MAX_VOLUME];
  if (strcmp(name, "spReset") == 0)
    return &g_method_tags[MID_AUDIO_SP_RESET];
  if (strcmp(name, "mpReset") == 0)
    return &g_method_tags[MID_AUDIO_MP_RESET];
  if (strcmp(name, "mpResetID") == 0)
    return &g_method_tags[MID_AUDIO_MP_RESET_ID];
  if (strcmp(name, "asyncBuildSpData") == 0)
    return &g_method_tags[MID_AUDIO_ASYNC_BUILD_SP_DATA];
  if (strcmp(name, "asyncBuildBgmData") == 0)
    return &g_method_tags[MID_AUDIO_ASYNC_BUILD_BGM_DATA];
  if (strcmp(name, "isDoneBuildSp") == 0)
    return &g_method_tags[MID_AUDIO_IS_DONE_BUILD_SP];
  if (strcmp(name, "isDoneBuildBgm") == 0)
    return &g_method_tags[MID_AUDIO_IS_DONE_BUILD_BGM];
  if (strcmp(name, "GetMusicState") == 0)
    return &g_method_tags[MID_AUDIO_GET_MUSIC_STATE];
  if (strcmp(name, "VideoSetDataSource") == 0)
    return &g_method_tags[MID_AUDIO_VIDEO_SET_DATA_SOURCE];
  if (strcmp(name, "VideoIsPlaying") == 0)
    return &g_method_tags[MID_AUDIO_VIDEO_IS_PLAYING];
  if (strcmp(name, "openUnlockWeb") == 0)
    return &g_method_tags[MID_AUDIO_OPEN_UNLOCK_WEB];
  if (strcmp(name, "openUnlockWebEp1") == 0)
    return &g_method_tags[MID_AUDIO_OPEN_UNLOCK_WEB_EP1];
  if (strcmp(name, "SetAdsShow") == 0)
    return &g_method_tags[MID_AUDIO_SET_ADS_SHOW];
  return &g_method_tags[MID_GENERIC];
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
  /* Locale.getDefault() -> objeto Locale (não-nulo) p/ chamar getLanguage() */
  if (strcmp(name, "getDefault") == 0)
    return &g_method_tags[MID_GET_DEFAULT_LOCALE];
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
  /* 🇬🇧 Locale.getLanguage()/getCountry() -> inglês (regra do projeto) */
  if (methodID == &g_method_tags[MID_GET_LANGUAGE]) {
    debugPrintf("jni_shim:   -> getLanguage = \"en\"\n");
    return make_jstring("en");
  }
  if (methodID == &g_method_tags[MID_GET_COUNTRY]) {
    debugPrintf("jni_shim:   -> getCountry = \"US\"\n");
    return make_jstring("US");
  }
  /* Sonic4 F2F */
  if (methodID == &g_method_tags[MID_LOCAL_PATH]) {
    debugPrintf("jni_shim:   -> getLocalPath/BundlePath = \"%s\"\n", jni_local_path);
    return make_jstring(jni_local_path);
  }
  if (methodID == &g_method_tags[MID_REGION])    return make_jstring("US");
  if (methodID == &g_method_tags[MID_LANG_CODE]) return make_jstring("en");
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
void *jni_shim_new_string(const char *value) {
  return make_jstring(value);
}

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
  /* Sonic4 F2F: os gates booleanos (isEnoughtAge, isUserConsentGDPR,
     isUserRemoveAds) precisam de TRUE p/ passar do consent/age/intro.
     SONIC_BOOL_FALSE volta ao default 0 p/ depurar. */
  return getenv("SONIC_BOOL_FALSE") ? 0 : 1;
}

static jint jni_CallIntMethod_args(void *env, void *obj, void *methodID,
                                   va_list ap) {
  (void)env;
  (void)obj;
  if (methodID == &g_method_tags[MID_BATTERY_STATUS])
    return 2; /* BATTERY_STATUS_CHARGING — sem power-save/cap de fps */
  if (methodID == &g_method_tags[MID_AUDIO_PLAY_SOUND]) {
    void *path_j = va_arg(ap, void *);
    double volume = va_arg(ap, double);
    int loop = va_arg(ap, int);
    const char *path = resolve_jstring(path_j);
    int h = sonic_audio_play_sfx(path, (float)volume, loop);
    if (audio_log_enabled())
      audioPrintf("jni_shim: AudioHelper.PlaySound path=\"%s\" volume=%.3f loop=%d -> handle %d\n",
                  path, volume, loop, h);
    else
      debugPrintf("jni_shim: AudioHelper.PlaySound -> handle %d\n", h);
    return h;
  }
  if (methodID == &g_method_tags[MID_AUDIO_IS_DONE_BUILD_SP]) {
    debugPrintf("jni_shim: AudioHelper.isDoneBuildSp -> %d\n", g_audio_sp_built);
    return g_audio_sp_built;
  }
  if (methodID == &g_method_tags[MID_AUDIO_IS_DONE_BUILD_BGM]) {
    debugPrintf("jni_shim: AudioHelper.isDoneBuildBgm -> %d\n", g_audio_bgm_built);
    return g_audio_bgm_built;
  }
  if (methodID == &g_method_tags[MID_AUDIO_GET_MUSIC_STATE]) {
    int id = va_arg(ap, int);
    return sonic_audio_music_state(id); /* 0 = tocando/ok no wrapper da libfox */
  }
  if (methodID == &g_method_tags[MID_AUDIO_VIDEO_SET_DATA_SOURCE])
    return 0;
  if (methodID == &g_method_tags[MID_AUDIO_VIDEO_IS_PLAYING])
    return 0;
  return 0;
}

/* CallIntMethod / CallIntMethodV */
static jint jni_CallIntMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap;
  va_start(ap, methodID);
  jint ret = jni_CallIntMethod_args(env, obj, methodID, ap);
  va_end(ap);
  return ret;
}

static jint jni_CallIntMethodV(void *env, void *obj, void *methodID,
                               va_list ap) {
  return jni_CallIntMethod_args(env, obj, methodID, ap);
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

/* 🔑 sinal: engine pediu pra mostrar o interstitial (callback de transição já
   armazenado). O main loop dispara callbackInterstitialAds 1x e zera. */
volatile int jni_inter_pending = 0;

static void jni_CallVoidMethod_args(void *env, void *obj, void *methodID,
                                    va_list ap) {
  (void)env;
  (void)obj;
  if (methodID == &g_method_tags[MID_SHOW_INTERSTITIAL]) {
    jni_inter_pending = 1;
    debugPrintf("jni_shim: showInterstitial -> agenda callbackInterstitialAds\n");
    return;
  }
  if (methodID == &g_method_tags[MID_AUDIO_SP_RESET]) {
    g_audio_sp_built = 0;
    sonic_audio_reset_sfx();
    debugPrintf("jni_shim: AudioHelper.spReset\n");
    return;
  }
  if (methodID == &g_method_tags[MID_AUDIO_MP_RESET]) {
    g_audio_bgm_built = 0;
    sonic_audio_reset_music(-1);
    debugPrintf("jni_shim: AudioHelper.mpReset\n");
    return;
  }
  if (methodID == &g_method_tags[MID_AUDIO_MP_RESET_ID]) {
    int id = va_arg(ap, int);
    g_audio_bgm_built = 0;
    sonic_audio_reset_music(id);
    if (audio_log_enabled())
      audioPrintf("jni_shim: AudioHelper.mpResetID id=%d\n", id);
    else
      debugPrintf("jni_shim: AudioHelper.mpReset\n");
    return;
  }
  if (methodID == &g_method_tags[MID_AUDIO_ASYNC_BUILD_SP_DATA]) {
    void *path_j = va_arg(ap, void *);
    const char *path = resolve_jstring(path_j);
    sonic_audio_set_sfx_bank(path);
    g_audio_sp_built = 1;
    if (audio_log_enabled())
      audioPrintf("jni_shim: AudioHelper.asyncBuildSpData path=\"%s\" -> ready\n",
                  path);
    else
      debugPrintf("jni_shim: AudioHelper.asyncBuildSpData -> ready\n");
    return;
  }
  if (methodID == &g_method_tags[MID_AUDIO_ASYNC_BUILD_BGM_DATA]) {
    void *path_j = va_arg(ap, void *);
    g_audio_bgm_built = 1;
    if (audio_log_enabled())
      audioPrintf("jni_shim: AudioHelper.asyncBuildBgmData path=\"%s\" -> ready\n",
                  resolve_jstring(path_j));
    else
      debugPrintf("jni_shim: AudioHelper.asyncBuildBgmData -> ready\n");
    return;
  }
  if (methodID == &g_method_tags[MID_AUDIO_PAUSE_SOUND] ||
      methodID == &g_method_tags[MID_AUDIO_RESUME_SOUND] ||
      methodID == &g_method_tags[MID_AUDIO_STOP_SOUND]) {
    int handle = va_arg(ap, int);
    if (methodID == &g_method_tags[MID_AUDIO_PAUSE_SOUND])
      sonic_audio_pause_sfx(handle, 1);
    else if (methodID == &g_method_tags[MID_AUDIO_RESUME_SOUND])
      sonic_audio_pause_sfx(handle, 0);
    else
      sonic_audio_stop_sfx(handle);
    if (audio_log_enabled()) {
      const char *name =
          methodID == &g_method_tags[MID_AUDIO_PAUSE_SOUND] ? "PauseSound" :
          methodID == &g_method_tags[MID_AUDIO_RESUME_SOUND] ? "ResumeSound" :
                                                               "StopSound";
      audioPrintf("jni_shim: AudioHelper.%s handle=%d\n", name, handle);
    }
    return;
  }
  if (methodID == &g_method_tags[MID_AUDIO_SET_VOLUME]) {
    int handle = va_arg(ap, int);
    double volume = va_arg(ap, double);
    sonic_audio_set_sfx_volume(handle, (float)volume);
    if (audio_log_enabled()) {
      audioPrintf("jni_shim: AudioHelper.SetVolume handle=%d volume=%.3f\n",
                  handle, volume);
    }
    return;
  }
  if (methodID == &g_method_tags[MID_AUDIO_MUSIC_SET_DATA_SOURCE]) {
    int id = va_arg(ap, int);
    void *path_j = va_arg(ap, void *);
    const char *path = resolve_jstring(path_j);
    sonic_audio_music_set_source(id, path);
    if (audio_log_enabled())
      audioPrintf("jni_shim: AudioHelper.MusicSetDataSource id=%d path=\"%s\"\n",
                  id, path);
    return;
  }
  if (methodID == &g_method_tags[MID_AUDIO_MUSIC_START] ||
      methodID == &g_method_tags[MID_AUDIO_MUSIC_STOP] ||
      methodID == &g_method_tags[MID_AUDIO_MUSIC_PAUSE]) {
    int id = va_arg(ap, int);
    if (methodID == &g_method_tags[MID_AUDIO_MUSIC_START])
      sonic_audio_music_start(id);
    else if (methodID == &g_method_tags[MID_AUDIO_MUSIC_STOP])
      sonic_audio_music_stop(id);
    else
      sonic_audio_music_pause(id, 1);
    if (audio_log_enabled()) {
      const char *name =
          methodID == &g_method_tags[MID_AUDIO_MUSIC_START] ? "MusicStart" :
          methodID == &g_method_tags[MID_AUDIO_MUSIC_STOP] ? "MusicStop" :
                                                             "MusicPause";
      audioPrintf("jni_shim: AudioHelper.%s id=%d\n", name, id);
    }
    return;
  }
  if (methodID == &g_method_tags[MID_AUDIO_MUSIC_VOLUME]) {
    int id = va_arg(ap, int);
    double volume = va_arg(ap, double);
    sonic_audio_music_set_volume(id, (float)volume);
    if (audio_log_enabled()) {
      audioPrintf("jni_shim: AudioHelper.MusicVolume id=%d volume=%.3f\n",
                  id, volume);
    }
    return;
  }
  if (methodID == &g_method_tags[MID_AUDIO_MUSIC_SET_LOOP]) {
    int id = va_arg(ap, int);
    int loop = va_arg(ap, int);
    sonic_audio_music_set_loop(id, loop);
    if (audio_log_enabled()) {
      audioPrintf("jni_shim: AudioHelper.MusicSetLoopFlag id=%d loop=%d\n",
                  id, loop);
    }
    return;
  }
  if (methodID == &g_method_tags[MID_AUDIO_SET_MAX_VOLUME] ||
      methodID == &g_method_tags[MID_AUDIO_OPEN_UNLOCK_WEB] ||
      methodID == &g_method_tags[MID_AUDIO_OPEN_UNLOCK_WEB_EP1] ||
      methodID == &g_method_tags[MID_AUDIO_SET_ADS_SHOW])
    return;
}

/* CallVoidMethod / CallVoidMethodV */
static void jni_CallVoidMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap;
  va_start(ap, methodID);
  jni_CallVoidMethod_args(env, obj, methodID, ap);
  va_end(ap);
}

static void jni_CallVoidMethodV(void *env, void *obj, void *methodID,
                                va_list ap) {
  jni_CallVoidMethod_args(env, obj, methodID, ap);
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
  /* Locale.getDefault() -> objeto Locale fake não-nulo (p/ getLanguage depois) */
  if (methodID == &g_method_tags[MID_GET_DEFAULT_LOCALE]) {
    debugPrintf("jni_shim: CallStaticObjectMethod -> Locale.getDefault (fake)\n");
    return make_jstring("locale");
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
  debugPrintf("jni_shim: CallStaticBooleanMethod(mid=%p) -> 1\n", methodID);
  // Return true for hasTouchScreen — prevents game from managing
  // Shield gamepad button layouts that don't exist in the OBB.
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
  jni_env_vtable[50] = (uintptr_t)jni_CallIntMethodV;      /* V */
  jni_env_vtable[55] = (uintptr_t)jni_CallFloatMethod;
  jni_env_vtable[56] = (uintptr_t)jni_CallFloatMethod;     /* V */
  jni_env_vtable[57] = (uintptr_t)jni_CallFloatMethod;     /* A */
  jni_env_vtable[61] = (uintptr_t)jni_CallVoidMethod;
  jni_env_vtable[62] = (uintptr_t)jni_CallVoidMethodV;     /* V */
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
