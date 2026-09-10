#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "jni_shim.h"
#include "util.h"

#define JNI_VTABLE_SIZE 512

typedef int jint;
typedef unsigned char jboolean;
typedef long jlong;
typedef float jfloat;
typedef double jdouble;

typedef union {
  jboolean z;
  signed char b;
  unsigned short c;
  short s;
  jint i;
  jlong j;
  jfloat f;
  jdouble d;
  void *l;
} jvalue;

static uintptr_t jni_env_vtable[JNI_VTABLE_SIZE];
static void *jni_env_ptr;

static uintptr_t java_vm_vtable[JNI_VTABLE_SIZE];
static void *java_vm_ptr;
static int g_fmod_audio_device_obj;
static SDL_AudioStream *g_fmod_audio_stream;
static int g_fmod_audio_high_water;
static unsigned int g_fmod_audio_writes;

typedef void (*age_signal_fn)(void *env, void *cls);
static age_signal_fn g_age_signal_fn = NULL;

typedef void (*gs_signin_fn)(void *env, void *cls);
static gs_signin_fn g_gs_signin_fn = NULL;

typedef void (*ad_init_fn)(void *env, void *cls);
static ad_init_fn g_ad_init_fn = NULL;

typedef void (*billing_item_fn)(void *env, void *cls, void *item);
static billing_item_fn g_billing_item_fn = NULL;

void jni_shim_set_callbacks(void *age_cb, void *gs_cb, void *ad_cb, void *bill_cb) {
  g_age_signal_fn = (age_signal_fn)age_cb;
  g_gs_signin_fn = (gs_signin_fn)gs_cb;
  g_ad_init_fn = (ad_init_fn)ad_cb;
  g_billing_item_fn = (billing_item_fn)bill_cb;
}

enum {
  CLASS_UNKNOWN = 0,
  CLASS_MAIN_ACTIVITY,
  CLASS_RESOURCES,
  CLASS_ASSET_MANAGER,
  CLASS_DISPLAY_METRICS,
  CLASS_BUILD_VERSION,
  CLASS_LOCALE,
  CLASS_CLASS_LOADER,
  CLASS_VU_AD_HELPER,
  CLASS_VU_AGE_HELPER,
  CLASS_VU_ANALYTICS_HELPER,
  CLASS_VU_AUDIO_HELPER,
  CLASS_VU_BILLING_HELPER,
  CLASS_VU_CLOUD_TUNING_HELPER,
  CLASS_VU_COMMUNITY_HELPER,
  CLASS_VU_GAME_SERVICES_HELPER,
  CLASS_VU_SYS_HELPER,
  CLASS_FMOD,
  CLASS_FMOD_AUDIO_DEVICE,
  CLASS_FMOD_MEDIA_CODEC,
};

enum {
  MID_UNKNOWN = 0,
  MID_GET_INSTANCE,
  MID_GET_RESOURCES,
  MID_GET_ASSETS,
  MID_GET_DISPLAY_METRICS,
  MID_GET_DEFAULT_LOCALE,
  MID_GET_LANGUAGE,
  MID_GET_COUNTRY,
  MID_GET_CLASS_LOADER,
  MID_LOAD_CLASS,
  MID_GET_AD_HELPER,
  MID_GET_AGE_HELPER,
  MID_GET_ANALYTICS_HELPER,
  MID_GET_AUDIO_HELPER,
  MID_GET_BILLING_HELPER,
  MID_GET_CLOUD_TUNING_HELPER,
  MID_GET_COMMUNITY_HELPER,
  MID_GET_GAME_SERVICES_HELPER,
  MID_GET_SYS_HELPER,
  MID_AGE_CHECK_SIGNAL,
  MID_AGE_IS_UNDER_AGE,
  MID_BILLING_INIT,
  MID_BILLING_ADD_CONSUMABLE,
  MID_BILLING_ADD_NON_CONSUMABLE,
  MID_BILLING_IS_CONSUMABLE,
  MID_BILLING_GET_PUBLIC_KEY,
  MID_SYS_GET_VERSION,
  MID_SYS_GET_SIGNATURE,
  MID_SYS_HAS_TOUCH,
  MID_GS_START_SIGN_IN,
  MID_RUN_ON_UI_THREAD,
  MID_WAS_GAME_CONFIG_RECEIVED,
  MID_GET_GAME_CONFIG_VALUE,
  MID_FMOD_INIT,
  MID_FMOD_CHECK_INIT,
  MID_FMOD_SUPPORTS_AAUDIO,
  MID_FMOD_SUPPORTS_LOW_LATENCY,
  MID_FMOD_GET_OUTPUT_SAMPLE_RATE,
  MID_FMOD_GET_OUTPUT_BLOCK_SIZE,
  MID_FMOD_DEV_INIT,
  MID_FMOD_DEV_START,
  MID_FMOD_DEV_STOP,
  MID_FMOD_DEV_CLOSE,
  MID_FMOD_DEV_WRITE,
  MID_FMOD_DEV_IS_RUNNING,
};

enum {
  FID_UNKNOWN = 0,
  FID_WIDTH_PIXELS,
  FID_HEIGHT_PIXELS,
  FID_DENSITY,
  FID_DENSITY_DPI,
  FID_SDK_INT,
};

typedef struct {
  char *str;
} FakeString;

typedef struct {
  jint length;
  int16_t *data;
} FakeShortArray;

static void fmod_audio_close(void) {
  if (!g_fmod_audio_stream)
    return;
  SDL_PauseAudioStreamDevice(g_fmod_audio_stream);
  SDL_DestroyAudioStream(g_fmod_audio_stream);
  g_fmod_audio_stream = NULL;
  g_fmod_audio_high_water = 0;
  logPrintf("[audio] FMOD AudioDevice closed\n");
}

static jboolean fmod_audio_init(jint channels, jint rate, jint block_count,
                                jint block_samples) {
  if (channels <= 0)
    channels = 2;
  if (rate <= 0)
    rate = 44100;
  if (block_count <= 0)
    block_count = 4;
  if (block_samples <= 0)
    block_samples = 1024;

  fmod_audio_close();
  if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) &&
      !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    logPrintf("[audio] SDL audio init failed: %s\n", SDL_GetError());
    return 0;
  }

  SDL_AudioSpec spec;
  SDL_zero(spec);
  spec.format = SDL_AUDIO_S16;
  spec.channels = channels;
  spec.freq = rate;
  g_fmod_audio_stream = SDL_OpenAudioDeviceStream(
      SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
  if (!g_fmod_audio_stream) {
    logPrintf("[audio] SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
    return 0;
  }

  g_fmod_audio_high_water = block_count * block_samples * channels *
                            (jint)sizeof(int16_t);
  if (g_fmod_audio_high_water < 4096)
    g_fmod_audio_high_water = 4096;
  g_fmod_audio_writes = 0;
  if (!SDL_ResumeAudioStreamDevice(g_fmod_audio_stream)) {
    logPrintf("[audio] SDL_ResumeAudioStreamDevice failed: %s\n",
              SDL_GetError());
    fmod_audio_close();
    return 0;
  }

  logPrintf("[audio] FMOD AudioDevice %d Hz, %d ch, %d x %d samples\n",
            rate, channels, block_count, block_samples);
  return 1;
}

static void fmod_audio_write(FakeShortArray *array, jint shorts) {
  if (!g_fmod_audio_stream || !array || !array->data || shorts <= 0)
    return;
  if (shorts > array->length)
    shorts = array->length;

  for (int i = 0;
       i < 200 &&
       SDL_GetAudioStreamQueued(g_fmod_audio_stream) > g_fmod_audio_high_water;
       i++) {
    SDL_Delay(1);
  }

  const int bytes = shorts * (jint)sizeof(int16_t);
  if (!SDL_PutAudioStreamData(g_fmod_audio_stream, array->data, bytes)) {
    logPrintf("[audio] SDL_PutAudioStreamData failed: %s\n", SDL_GetError());
    return;
  }

  {
    jint n = g_fmod_audio_writes++;
    if (n == 0 || n == 200 || n == 1000) {
      int peak = 0;
      for (int i = 0; i < shorts; i++) {
        int sample = array->data[i];
        int magnitude = sample < 0 ? -sample : sample;
        if (magnitude > peak)
          peak = magnitude;
      }
      logPrintf("[audio] FMOD block #%d: %d bytes, peak=%d\n", (int)n, bytes,
                peak);
    }
  }
}

void *jni_new_string_utf(const char *bytes) {
  if (!bytes) return NULL;
  FakeString *s = (FakeString *)malloc(sizeof(FakeString));
  s->str = strdup(bytes);
  return s;
}

static const char *get_string_utf_chars(void *env, void *str, jboolean *isCopy) {
  (void)env;
  if (isCopy) *isCopy = 0;
  if (!str) return "";
  FakeString *s = (FakeString *)str;
  return s->str ? s->str : "";
}

static void release_string_utf_chars(void *env, void *str, const char *chars) {
  (void)env; (void)str; (void)chars;
}

static jint get_string_length(void *env, void *str) {
  (void)env;
  if (!str) return 0;
  FakeString *s = (FakeString *)str;
  return s->str ? (jint)strlen(s->str) : 0;
}

static jint get_string_utf_length(void *env, void *str) {
  return get_string_length(env, str);
}

static void *find_class(void *env, const char *name) {
  (void)env;
  if (!name) return (void *)(uintptr_t)CLASS_UNKNOWN;
  debugPrintf("[jni] FindClass('%s')\n", name);

  if (strstr(name, "MainActivity")) return (void *)(uintptr_t)CLASS_MAIN_ACTIVITY;
  if (strstr(name, "Resources")) return (void *)(uintptr_t)CLASS_RESOURCES;
  if (strstr(name, "AssetManager")) return (void *)(uintptr_t)CLASS_ASSET_MANAGER;
  if (strstr(name, "DisplayMetrics")) return (void *)(uintptr_t)CLASS_DISPLAY_METRICS;
  if (strstr(name, "Build$VERSION") || strstr(name, "VERSION")) return (void *)(uintptr_t)CLASS_BUILD_VERSION;
  if (strstr(name, "Locale")) return (void *)(uintptr_t)CLASS_LOCALE;
  if (strstr(name, "ClassLoader")) return (void *)(uintptr_t)CLASS_CLASS_LOADER;
  if (strstr(name, "VuAdHelper")) return (void *)(uintptr_t)CLASS_VU_AD_HELPER;
  if (strstr(name, "VuAgeHelper")) return (void *)(uintptr_t)CLASS_VU_AGE_HELPER;
  if (strstr(name, "VuAnalyticsHelper")) return (void *)(uintptr_t)CLASS_VU_ANALYTICS_HELPER;
  if (strstr(name, "VuAudioHelper")) return (void *)(uintptr_t)CLASS_VU_AUDIO_HELPER;
  if (strstr(name, "VuBillingHelper")) return (void *)(uintptr_t)CLASS_VU_BILLING_HELPER;
  if (strstr(name, "VuCloudTuningHelper")) return (void *)(uintptr_t)CLASS_VU_CLOUD_TUNING_HELPER;
  if (strstr(name, "VuCommunityHelper")) return (void *)(uintptr_t)CLASS_VU_COMMUNITY_HELPER;
  if (strstr(name, "VuGameServicesHelper")) return (void *)(uintptr_t)CLASS_VU_GAME_SERVICES_HELPER;
  if (strstr(name, "VuSysHelper")) return (void *)(uintptr_t)CLASS_VU_SYS_HELPER;
  if (strstr(name, "org/fmod/FMODAudioDevice") || strstr(name, "org/fmod/AudioDevice")) return (void *)(uintptr_t)CLASS_FMOD_AUDIO_DEVICE;
  if (strstr(name, "org/fmod/MediaCodec")) return (void *)(uintptr_t)CLASS_FMOD_MEDIA_CODEC;
  if (strstr(name, "org/fmod/FMOD")) return (void *)(uintptr_t)CLASS_FMOD;

  return (void *)(uintptr_t)CLASS_UNKNOWN;
}

static void *get_method_id(void *env, void *cls, const char *name, const char *sig) {
  (void)env;
  if (!name) return (void *)(uintptr_t)MID_UNKNOWN;
  debugPrintf("[jni] GetMethodID('%s', '%s')\n", name, sig ? sig : "");

  if ((uintptr_t)cls == CLASS_FMOD_AUDIO_DEVICE) {
    if (!strcmp(name, "<init>") || !strcmp(name, "init"))
      return (void *)(uintptr_t)MID_FMOD_DEV_INIT;
    if (!strcmp(name, "write"))
      return (void *)(uintptr_t)MID_FMOD_DEV_WRITE;
  }

  if (!strcmp(name, "getInstance")) return (void *)(uintptr_t)MID_GET_INSTANCE;
  if (!strcmp(name, "getResources")) return (void *)(uintptr_t)MID_GET_RESOURCES;
  if (!strcmp(name, "getAssets")) return (void *)(uintptr_t)MID_GET_ASSETS;
  if (!strcmp(name, "getDisplayMetrics")) return (void *)(uintptr_t)MID_GET_DISPLAY_METRICS;
  if (!strcmp(name, "getDefault")) return (void *)(uintptr_t)MID_GET_DEFAULT_LOCALE;
  if (!strcmp(name, "getLanguage")) return (void *)(uintptr_t)MID_GET_LANGUAGE;
  if (!strcmp(name, "getCountry")) return (void *)(uintptr_t)MID_GET_COUNTRY;
  if (!strcmp(name, "getClassLoader")) return (void *)(uintptr_t)MID_GET_CLASS_LOADER;
  if (!strcmp(name, "loadClass")) return (void *)(uintptr_t)MID_LOAD_CLASS;

  if (!strcmp(name, "getAdHelper")) return (void *)(uintptr_t)MID_GET_AD_HELPER;
  if (!strcmp(name, "getAgeHelper")) return (void *)(uintptr_t)MID_GET_AGE_HELPER;
  if (!strcmp(name, "getAnalyticsHelper")) return (void *)(uintptr_t)MID_GET_ANALYTICS_HELPER;
  if (!strcmp(name, "getAudioHelper")) return (void *)(uintptr_t)MID_GET_AUDIO_HELPER;
  if (!strcmp(name, "getBillingHelper")) return (void *)(uintptr_t)MID_GET_BILLING_HELPER;
  if (!strcmp(name, "getCloudTuningHelper")) return (void *)(uintptr_t)MID_GET_CLOUD_TUNING_HELPER;
  if (!strcmp(name, "getCommunityHelper")) return (void *)(uintptr_t)MID_GET_COMMUNITY_HELPER;
  if (!strcmp(name, "getGameServicesHelper")) return (void *)(uintptr_t)MID_GET_GAME_SERVICES_HELPER;
  if (!strcmp(name, "getSysHelper")) return (void *)(uintptr_t)MID_GET_SYS_HELPER;

  if (!strcmp(name, "checkAgeSignal")) return (void *)(uintptr_t)MID_AGE_CHECK_SIGNAL;
  if (!strcmp(name, "isUnderAge")) return (void *)(uintptr_t)MID_AGE_IS_UNDER_AGE;

  if (!strcmp(name, "initialize")) return (void *)(uintptr_t)MID_BILLING_INIT;
  if (!strcmp(name, "addConsumableId")) return (void *)(uintptr_t)MID_BILLING_ADD_CONSUMABLE;
  if (!strcmp(name, "addNonConsumableId")) return (void *)(uintptr_t)MID_BILLING_ADD_NON_CONSUMABLE;
  if (!strcmp(name, "isConsumable")) return (void *)(uintptr_t)MID_BILLING_IS_CONSUMABLE;
  if (!strcmp(name, "getPublicKey")) return (void *)(uintptr_t)MID_BILLING_GET_PUBLIC_KEY;

  if (!strcmp(name, "getVersion")) return (void *)(uintptr_t)MID_SYS_GET_VERSION;
  if (!strcmp(name, "getSignature")) return (void *)(uintptr_t)MID_SYS_GET_SIGNATURE;
  if (!strcmp(name, "hasTouch")) return (void *)(uintptr_t)MID_SYS_HAS_TOUCH;

  if (!strcmp(name, "startSignIn")) return (void *)(uintptr_t)MID_GS_START_SIGN_IN;
  if (!strcmp(name, "runOnUiThread")) return (void *)(uintptr_t)MID_RUN_ON_UI_THREAD;

  if (!strcmp(name, "wasGameConfigurationReceived")) return (void *)(uintptr_t)MID_WAS_GAME_CONFIG_RECEIVED;
  if (!strcmp(name, "getGameConfigurationValue")) return (void *)(uintptr_t)MID_GET_GAME_CONFIG_VALUE;
  if (!strcmp(name, "init")) return (void *)(uintptr_t)MID_FMOD_INIT;
  if (!strcmp(name, "checkInit")) return (void *)(uintptr_t)MID_FMOD_CHECK_INIT;
  if (!strcmp(name, "supportsAAudio")) return (void *)(uintptr_t)MID_FMOD_SUPPORTS_AAUDIO;
  if (!strcmp(name, "supportsLowLatency")) return (void *)(uintptr_t)MID_FMOD_SUPPORTS_LOW_LATENCY;
  if (!strcmp(name, "getOutputSampleRate")) return (void *)(uintptr_t)MID_FMOD_GET_OUTPUT_SAMPLE_RATE;
  if (!strcmp(name, "getOutputBlockSize")) return (void *)(uintptr_t)MID_FMOD_GET_OUTPUT_BLOCK_SIZE;

  if (!strcmp(name, "start")) return (void *)(uintptr_t)MID_FMOD_DEV_START;
  if (!strcmp(name, "stop")) return (void *)(uintptr_t)MID_FMOD_DEV_STOP;
  if (!strcmp(name, "close")) return (void *)(uintptr_t)MID_FMOD_DEV_CLOSE;
  if (!strcmp(name, "isRunning")) return (void *)(uintptr_t)MID_FMOD_DEV_IS_RUNNING;

  return (void *)(uintptr_t)MID_UNKNOWN;
}

static void *get_static_method_id(void *env, void *cls, const char *name, const char *sig) {
  return get_method_id(env, cls, name, sig);
}

static void *get_field_id(void *env, void *cls, const char *name, const char *sig) {
  (void)env; (void)cls; (void)sig;
  if (!name) return (void *)(uintptr_t)FID_UNKNOWN;
  debugPrintf("[jni] GetFieldID('%s')\n", name);

  if (!strcmp(name, "widthPixels")) return (void *)(uintptr_t)FID_WIDTH_PIXELS;
  if (!strcmp(name, "heightPixels")) return (void *)(uintptr_t)FID_HEIGHT_PIXELS;
  if (!strcmp(name, "density")) return (void *)(uintptr_t)FID_DENSITY;
  if (!strcmp(name, "densityDpi")) return (void *)(uintptr_t)FID_DENSITY_DPI;
  if (!strcmp(name, "SDK_INT")) return (void *)(uintptr_t)FID_SDK_INT;

  return (void *)(uintptr_t)FID_UNKNOWN;
}

static void *get_static_field_id(void *env, void *cls, const char *name, const char *sig) {
  return get_field_id(env, cls, name, sig);
}

static void *call_object_method_v(void *env, void *obj, void *methodID, va_list args) {
  (void)env; (void)obj; (void)args;
  uintptr_t mid = (uintptr_t)methodID;
  debugPrintf("[jni] CallObjectMethodV(mid=%lu)\n", (unsigned long)mid);

  switch (mid) {
    case MID_GET_RESOURCES:
      return (void *)(uintptr_t)CLASS_RESOURCES;
    case MID_GET_ASSETS:
      return (void *)(uintptr_t)CLASS_ASSET_MANAGER;
    case MID_GET_DISPLAY_METRICS:
      return (void *)(uintptr_t)CLASS_DISPLAY_METRICS;
    case MID_GET_DEFAULT_LOCALE:
      return (void *)(uintptr_t)CLASS_LOCALE;
    case MID_GET_LANGUAGE:
      return jni_new_string_utf("en");
    case MID_GET_COUNTRY:
      return jni_new_string_utf("US");
    case MID_GET_CLASS_LOADER:
      return (void *)(uintptr_t)CLASS_CLASS_LOADER;
    case MID_GET_AD_HELPER:
      return (void *)(uintptr_t)CLASS_VU_AD_HELPER;
    case MID_GET_AGE_HELPER:
      return (void *)(uintptr_t)CLASS_VU_AGE_HELPER;
    case MID_GET_ANALYTICS_HELPER:
      return (void *)(uintptr_t)CLASS_VU_ANALYTICS_HELPER;
    case MID_GET_AUDIO_HELPER:
      return (void *)(uintptr_t)CLASS_VU_AUDIO_HELPER;
    case MID_GET_BILLING_HELPER:
      return (void *)(uintptr_t)CLASS_VU_BILLING_HELPER;
    case MID_GET_CLOUD_TUNING_HELPER:
      return (void *)(uintptr_t)CLASS_VU_CLOUD_TUNING_HELPER;
    case MID_GET_COMMUNITY_HELPER:
      return (void *)(uintptr_t)CLASS_VU_COMMUNITY_HELPER;
    case MID_GET_GAME_SERVICES_HELPER:
      return (void *)(uintptr_t)CLASS_VU_GAME_SERVICES_HELPER;
    case MID_GET_SYS_HELPER:
      return (void *)(uintptr_t)CLASS_VU_SYS_HELPER;
    case MID_SYS_GET_VERSION:
      return jni_new_string_utf("2026.05.18");
    case MID_SYS_GET_SIGNATURE:
      return jni_new_string_utf("VALID_SIGNATURE");
    case MID_BILLING_GET_PUBLIC_KEY:
      return jni_new_string_utf("");
    case MID_GET_GAME_CONFIG_VALUE:
      return jni_new_string_utf("");
    default:
      return (void *)0x1;
  }
}

static void *call_object_method(void *env, void *obj, void *methodID, ...) {
  va_list args;
  va_start(args, methodID);
  void *ret = call_object_method_v(env, obj, methodID, args);
  va_end(args);
  return ret;
}

static void *call_static_object_method_v(void *env, void *cls, void *methodID, va_list args) {
  return call_object_method_v(env, cls, methodID, args);
}

static void *call_static_object_method(void *env, void *cls, void *methodID, ...) {
  va_list args;
  va_start(args, methodID);
  void *ret = call_object_method_v(env, cls, methodID, args);
  va_end(args);
  return ret;
}

static void call_void_method_v(void *env, void *obj, void *methodID, va_list args) {
  (void)env; (void)obj;
  uintptr_t mid = (uintptr_t)methodID;
  debugPrintf("[jni] CallVoidMethodV(mid=%lu)\n", (unsigned long)mid);

  if (mid == MID_AGE_CHECK_SIGNAL) {
    debugPrintf("[jni] AgeHelper.checkAgeSignal() -> trigger nativeOnAgeSignalNotUnderAge\n");
    if (g_age_signal_fn) g_age_signal_fn(env, obj);
  } else if (mid == MID_GS_START_SIGN_IN) {
    debugPrintf("[jni] GameServices.startSignIn() -> trigger onSignInSuccess\n");
    if (g_gs_signin_fn) g_gs_signin_fn(env, obj);
  } else if (mid == MID_BILLING_INIT) {
    debugPrintf("[jni] BillingHelper.initialize() -> trigger addOwnedItem\n");
    if (g_billing_item_fn) {
      g_billing_item_fn(env, obj, jni_new_string_utf("premium"));
    }
    if (g_ad_init_fn) {
      debugPrintf("[jni] AdHelper.initialize() -> trigger nativeInitializationComplete\n");
      g_ad_init_fn(env, obj);
    }
  } else if (mid == MID_FMOD_DEV_WRITE) {
    FakeShortArray *array = va_arg(args, FakeShortArray *);
    jint shorts = va_arg(args, jint);
    fmod_audio_write(array, shorts);
  } else if (mid == MID_FMOD_DEV_CLOSE) {
    fmod_audio_close();
  }
}

static void call_void_method(void *env, void *obj, void *methodID, ...) {
  va_list args;
  va_start(args, methodID);
  call_void_method_v(env, obj, methodID, args);
  va_end(args);
}

static void call_static_void_method_v(void *env, void *cls, void *methodID, va_list args) {
  call_void_method_v(env, cls, methodID, args);
}

static void call_static_void_method(void *env, void *cls, void *methodID, ...) {
  va_list args;
  va_start(args, methodID);
  call_void_method_v(env, cls, methodID, args);
  va_end(args);
}

static jboolean call_boolean_method_v(void *env, void *obj, void *methodID, va_list args) {
  (void)env; (void)obj;
  uintptr_t mid = (uintptr_t)methodID;
  debugPrintf("[jni] CallBooleanMethodV(mid=%lu)\n", (unsigned long)mid);
  if (mid == MID_AGE_IS_UNDER_AGE) return 0;
  if (mid == MID_SYS_HAS_TOUCH) return 0;
  if (mid == MID_BILLING_IS_CONSUMABLE) return 1;
  if (mid == MID_FMOD_CHECK_INIT) return 1;
  if (mid == MID_WAS_GAME_CONFIG_RECEIVED) return 0;
  if (mid == MID_FMOD_SUPPORTS_AAUDIO) return 0;
  if (mid == MID_FMOD_SUPPORTS_LOW_LATENCY) return 0;
  if (mid == MID_FMOD_DEV_IS_RUNNING) return 1;
  if (mid == MID_FMOD_DEV_INIT) {
    jint channels = va_arg(args, jint);
    jint rate = va_arg(args, jint);
    jint block_count = va_arg(args, jint);
    jint block_samples = va_arg(args, jint);
    return fmod_audio_init(channels, rate, block_count, block_samples);
  }
  return 1;
}

static jboolean call_boolean_method(void *env, void *obj, void *methodID, ...) {
  va_list args;
  va_start(args, methodID);
  jboolean ret = call_boolean_method_v(env, obj, methodID, args);
  va_end(args);
  return ret;
}

static jboolean call_static_boolean_method_v(void *env, void *cls, void *methodID, va_list args) {
  return call_boolean_method_v(env, cls, methodID, args);
}

static jboolean call_static_boolean_method(void *env, void *cls, void *methodID, ...) {
  va_list args;
  va_start(args, methodID);
  jboolean ret = call_boolean_method_v(env, cls, methodID, args);
  va_end(args);
  return ret;
}

static jint call_int_method_v(void *env, void *obj, void *methodID, va_list args) {
  (void)env; (void)obj; (void)args;
  uintptr_t mid = (uintptr_t)methodID;
  if (mid == MID_FMOD_GET_OUTPUT_SAMPLE_RATE) return 44100;
  if (mid == MID_FMOD_GET_OUTPUT_BLOCK_SIZE) return 1024;
  return 0;
}

static jint call_int_method(void *env, void *obj, void *methodID, ...) {
  va_list args;
  va_start(args, methodID);
  jint ret = call_int_method_v(env, obj, methodID, args);
  va_end(args);
  return ret;
}

static jint call_static_int_method_v(void *env, void *cls, void *methodID, va_list args) {
  return call_int_method_v(env, cls, methodID, args);
}

static jint call_static_int_method(void *env, void *cls, void *methodID, ...) {
  va_list args;
  va_start(args, methodID);
  jint ret = call_int_method_v(env, cls, methodID, args);
  va_end(args);
  return ret;
}

static jint get_int_field(void *env, void *obj, void *fieldID) {
  (void)env; (void)obj;
  uintptr_t fid = (uintptr_t)fieldID;
  if (fid == FID_WIDTH_PIXELS) return 1280;
  if (fid == FID_HEIGHT_PIXELS) return 720;
  if (fid == FID_DENSITY_DPI) return 160;
  if (fid == FID_SDK_INT) return 28;
  return 0;
}

static jint get_static_int_field(void *env, void *cls, void *fieldID) {
  return get_int_field(env, cls, fieldID);
}

static jfloat get_float_field(void *env, void *obj, void *fieldID) {
  (void)env; (void)obj;
  uintptr_t fid = (uintptr_t)fieldID;
  if (fid == FID_DENSITY) return 1.0f;
  return 1.0f;
}

static jfloat get_static_float_field(void *env, void *cls, void *fieldID) {
  return get_float_field(env, cls, fieldID);
}

static jint jni_GetVersion(void *env) {
  (void)env;
  return 0x00010006;
}

static void *jni_NewGlobalRef(void *env, void *obj) {
  (void)env;
  return obj;
}

static void *jni_NewObject(void *env, void *clazz, void *methodID, ...) {
  (void)env;
  (void)methodID;
  if ((uintptr_t)clazz == CLASS_FMOD_AUDIO_DEVICE)
    return &g_fmod_audio_device_obj;
  return NULL;
}

static void *jni_NewObjectV(void *env, void *clazz, void *methodID,
                            va_list args) {
  (void)args;
  return jni_NewObject(env, clazz, methodID);
}

static void *jni_NewObjectA(void *env, void *clazz, void *methodID,
                            const jvalue *args) {
  (void)args;
  return jni_NewObject(env, clazz, methodID);
}

static void *jni_NewShortArray(void *env, jint length) {
  (void)env;
  if (length <= 0)
    return NULL;
  FakeShortArray *array = calloc(1, sizeof(*array));
  if (!array)
    return NULL;
  array->data = calloc((size_t)length, sizeof(*array->data));
  if (!array->data) {
    free(array);
    return NULL;
  }
  array->length = length;
  return array;
}

static void jni_SetShortArrayRegion(void *env, FakeShortArray *array,
                                    jint start, jint length,
                                    const int16_t *source) {
  (void)env;
  if (!array || !array->data || !source || start < 0 || length < 0 ||
      start > array->length || length > array->length - start)
    return;
  memcpy(array->data + start, source,
         (size_t)length * sizeof(*array->data));
}

static jint jni_GetJavaVM(void *env, void **vm) {
  (void)env;
  if (vm) *vm = java_vm_ptr;
  return 0;
}

static jint jni_RegisterNatives(void *env, void *clazz, const void *methods, jint nMethods) {
  (void)env; (void)clazz; (void)methods;
  debugPrintf("[jni] RegisterNatives(%d methods)\n", nMethods);
  return 0;
}

static jint jni_UnregisterNatives(void *env, void *clazz) {
  (void)env; (void)clazz;
  return 0;
}

static jint get_env(void *vm, void **env, jint version) {
  (void)vm; (void)version;
  if (env) *env = jni_env_ptr;
  return 0;
}

static jint attach_current_thread(void *vm, void **env, void *args) {
  (void)vm; (void)args;
  if (env) *env = jni_env_ptr;
  return 0;
}

static jint detach_current_thread(void *vm) {
  (void)vm;
  return 0;
}

void jni_shim_init(void **out_vm, void **out_env) {
  // Fill all slots with safe default (ret0) so no unhandled index crashes
  for (int i = 0; i < JNI_VTABLE_SIZE; i++) {
    jni_env_vtable[i] = (uintptr_t)ret0;
    java_vm_vtable[i] = (uintptr_t)ret0;
  }

  // Exact standard JNIEnv vtable indices from Android NDK jni.h:
  jni_env_vtable[4]   = (uintptr_t)jni_GetVersion;
  jni_env_vtable[6]   = (uintptr_t)find_class;                    // FindClass
  jni_env_vtable[21]  = (uintptr_t)jni_NewGlobalRef;              // NewGlobalRef
  jni_env_vtable[28]  = (uintptr_t)jni_NewObject;                 // NewObject
  jni_env_vtable[29]  = (uintptr_t)jni_NewObjectV;                // NewObjectV
  jni_env_vtable[30]  = (uintptr_t)jni_NewObjectA;                // NewObjectA
  jni_env_vtable[33]  = (uintptr_t)get_method_id;                // GetMethodID
  jni_env_vtable[34]  = (uintptr_t)call_object_method;           // CallObjectMethod
  jni_env_vtable[35]  = (uintptr_t)call_object_method_v;         // CallObjectMethodV
  jni_env_vtable[37]  = (uintptr_t)call_boolean_method;          // CallBooleanMethod
  jni_env_vtable[38]  = (uintptr_t)call_boolean_method_v;        // CallBooleanMethodV
  jni_env_vtable[49]  = (uintptr_t)call_int_method;              // CallIntMethod
  jni_env_vtable[50]  = (uintptr_t)call_int_method_v;            // CallIntMethodV
  jni_env_vtable[61]  = (uintptr_t)call_void_method;             // CallVoidMethod
  jni_env_vtable[62]  = (uintptr_t)call_void_method_v;           // CallVoidMethodV
  jni_env_vtable[94]  = (uintptr_t)get_field_id;                 // GetFieldID
  jni_env_vtable[95]  = (uintptr_t)ret0;                         // GetObjectField
  jni_env_vtable[96]  = (uintptr_t)ret0;                         // GetBooleanField
  jni_env_vtable[100] = (uintptr_t)get_int_field;                // GetIntField
  jni_env_vtable[102] = (uintptr_t)get_float_field;              // GetFloatField
  jni_env_vtable[113] = (uintptr_t)get_static_method_id;        // GetStaticMethodID
  jni_env_vtable[114] = (uintptr_t)call_static_object_method;   // CallStaticObjectMethod
  jni_env_vtable[115] = (uintptr_t)call_static_object_method_v; // CallStaticObjectMethodV
  jni_env_vtable[117] = (uintptr_t)call_static_boolean_method;  // CallStaticBooleanMethod
  jni_env_vtable[118] = (uintptr_t)call_static_boolean_method_v;// CallStaticBooleanMethodV
  jni_env_vtable[129] = (uintptr_t)call_static_int_method;      // CallStaticIntMethod
  jni_env_vtable[130] = (uintptr_t)call_static_int_method_v;    // CallStaticIntMethodV
  jni_env_vtable[141] = (uintptr_t)call_static_void_method;     // CallStaticVoidMethod
  jni_env_vtable[142] = (uintptr_t)call_static_void_method_v;   // CallStaticVoidMethodV
  jni_env_vtable[144] = (uintptr_t)get_static_field_id;         // GetStaticFieldID
  jni_env_vtable[145] = (uintptr_t)ret0;                         // GetStaticObjectField
  jni_env_vtable[146] = (uintptr_t)ret0;                         // GetStaticBooleanField
  jni_env_vtable[150] = (uintptr_t)get_static_int_field;        // GetStaticIntField
  jni_env_vtable[152] = (uintptr_t)get_static_float_field;      // GetStaticFloatField
  jni_env_vtable[164] = (uintptr_t)get_string_length;           // GetStringLength
  jni_env_vtable[167] = (uintptr_t)jni_new_string_utf;          // NewStringUTF
  jni_env_vtable[168] = (uintptr_t)get_string_utf_length;       // GetStringUTFLength
  jni_env_vtable[169] = (uintptr_t)get_string_utf_chars;        // GetStringUTFChars
  jni_env_vtable[170] = (uintptr_t)release_string_utf_chars;    // ReleaseStringUTFChars
  jni_env_vtable[178] = (uintptr_t)jni_NewShortArray;            // NewShortArray
  jni_env_vtable[210] = (uintptr_t)jni_SetShortArrayRegion;      // SetShortArrayRegion
  jni_env_vtable[215] = (uintptr_t)jni_RegisterNatives;         // RegisterNatives
  jni_env_vtable[216] = (uintptr_t)jni_UnregisterNatives;       // UnregisterNatives
  jni_env_vtable[219] = (uintptr_t)jni_GetJavaVM;               // GetJavaVM

  // JavaVM vtable indices
  java_vm_vtable[3] = (uintptr_t)ret0;                   // DestroyJavaVM
  java_vm_vtable[4] = (uintptr_t)attach_current_thread;  // AttachCurrentThread
  java_vm_vtable[5] = (uintptr_t)detach_current_thread;  // DetachCurrentThread
  java_vm_vtable[6] = (uintptr_t)get_env;                // GetEnv
  java_vm_vtable[7] = (uintptr_t)attach_current_thread;  // AttachCurrentThreadAsDaemon

  static uintptr_t env_ptr_val;
  env_ptr_val = (uintptr_t)jni_env_vtable;
  jni_env_ptr = &env_ptr_val;

  static uintptr_t vm_ptr_val;
  vm_ptr_val = (uintptr_t)java_vm_vtable;
  java_vm_ptr = &vm_ptr_val;

  if (out_vm) *out_vm = java_vm_ptr;
  if (out_env) *out_env = jni_env_ptr;
  logPrintf("[jni] jni_shim_init complete (vm=%p, env=%p)\n", java_vm_ptr, jni_env_ptr);
}

void *jni_get_vm(void) { return java_vm_ptr; }
void *jni_get_env(void) { return jni_env_ptr; }
