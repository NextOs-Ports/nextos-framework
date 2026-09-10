/*
 * jni_shim.c -- the JNIEnv/JavaVM the Cocos2d-x 2.x guest talks to.
 *
 * The vtable slots are the plain NDK jni.h indices; the per-method behaviour
 * lives in one nx_jni table (receita 14), so an upcall we did not foresee gets
 * a safe default and a log line instead of a crash.
 *
 * Four Java classes are referenced by the game:
 *   org/cocos2dx/lib/Cocos2dxHelper        paths, DPI, language, CCUserDefault
 *   org/cocos2dx/lib/Cocos2dxLocalStorage  key/value store
 *   org/cocos2dx/lib/Cocos2dxBitmap        text rendering (see text_render.c)
 *   com/customRobTop/BaseRobTopActivity    device info, ads, achievements
 *   com/customRobTop/SimpleCrypto          save encryption
 *
 * CCUserDefault and LocalStorage are the save game: both are backed by one
 * plain-text store under the port's userdata directory.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "jni_shim.h"
#include "nx_jni.h"
#include "util.h"

typedef int jint;
typedef unsigned char jboolean;

#define JNI_VTABLE_SIZE 256

static uintptr_t jni_env_vtable[JNI_VTABLE_SIZE];
static void *jni_env_ptr;
static uintptr_t java_vm_vtable[JNI_VTABLE_SIZE];
static void *java_vm_ptr;

static char g_writable[512] = "./userdata/";
static char g_package[128] = "com.robtopx.geometryjump";
static jni_bitmap_dc_fn g_bitmap_dc;

void jni_shim_set_paths(const char *writable_path, const char *package_name) {
  if (writable_path)
    snprintf(g_writable, sizeof(g_writable), "%s", writable_path);
  if (package_name)
    snprintf(g_package, sizeof(g_package), "%s", package_name);
}
void jni_shim_set_bitmap_dc(jni_bitmap_dc_fn fn) { g_bitmap_dc = fn; }

/* ------------------------------------------------------------ fake arrays - */
#define NX_ARR_TAG 0x4E584152u /* "NXAR" */
typedef struct {
  uint32_t tag;
  int elemsz;
  int len;
  void *data;
} nx_array;

static void *new_array(int elemsz, int len, const void *init) {
  nx_array *a = calloc(1, sizeof(nx_array));
  a->tag = NX_ARR_TAG;
  a->elemsz = elemsz;
  a->len = len;
  a->data = calloc((size_t)(len > 0 ? len : 1), (size_t)elemsz);
  if (init && len > 0)
    memcpy(a->data, init, (size_t)len * (size_t)elemsz);
  return a;
}
static nx_array *as_array(void *p) {
  nx_array *a = (nx_array *)p;
  return (a && a->tag == NX_ARR_TAG) ? a : NULL;
}
void *jni_shim_new_int_array(const int *v, int n) {
  return new_array(4, n, v);
}
void *jni_shim_new_float_array(const float *v, int n) {
  return new_array(4, n, v);
}
void *jni_shim_new_jstring(const char *s) {
  nx_ctx c;
  memset(&c, 0, sizeof(c));
  return nx_new_string(&c, s ? s : "");
}

/* -------------------------------------------------------- preference store -
 * One flat file, "key\tvalue" per line.  CCUserDefault (SharedPreferences on
 * Android) and Cocos2dxLocalStorage share it under distinct key prefixes. */
#define PREFS_MAX 4096
static struct {
  char *key;
  char *val;
} g_prefs[PREFS_MAX];
static int g_prefs_n;
static int g_prefs_dirty;
static int g_prefs_loaded;

static void prefs_path(char *out, size_t n) {
  snprintf(out, n, "%s/prefs.txt", g_writable);
}

static void prefs_load(void) {
  if (g_prefs_loaded)
    return;
  g_prefs_loaded = 1;
  char path[600];
  prefs_path(path, sizeof(path));
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char *line = NULL;
  size_t cap = 0;
  ssize_t n;
  while ((n = getline(&line, &cap, f)) > 0 && g_prefs_n < PREFS_MAX) {
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = 0;
    char *tab = strchr(line, '\t');
    if (!tab)
      continue;
    *tab = 0;
    g_prefs[g_prefs_n].key = strdup(line);
    g_prefs[g_prefs_n].val = strdup(tab + 1);
    g_prefs_n++;
  }
  free(line);
  fclose(f);
  debugPrintf("[prefs] %d entradas carregadas de %s\n", g_prefs_n, path);
}

void jni_shim_prefs_flush(void) {
  if (!g_prefs_dirty)
    return;
  char path[600], tmp[620];
  prefs_path(path, sizeof(path));
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "w");
  if (!f) {
    debugPrintf("[prefs] nao consegui escrever %s\n", tmp);
    return;
  }
  for (int i = 0; i < g_prefs_n; i++)
    fprintf(f, "%s\t%s\n", g_prefs[i].key, g_prefs[i].val);
  fflush(f);
  fsync(fileno(f));
  fclose(f);
  rename(tmp, path);
  g_prefs_dirty = 0;
}

static const char *prefs_get(const char *key, const char *def) {
  prefs_load();
  for (int i = 0; i < g_prefs_n; i++)
    if (!strcmp(g_prefs[i].key, key))
      return g_prefs[i].val;
  return def;
}

static void prefs_set(const char *key, const char *val) {
  prefs_load();
  for (int i = 0; i < g_prefs_n; i++)
    if (!strcmp(g_prefs[i].key, key)) {
      free(g_prefs[i].val);
      g_prefs[i].val = strdup(val ? val : "");
      g_prefs_dirty = 1;
      return;
    }
  if (g_prefs_n >= PREFS_MAX)
    return;
  g_prefs[g_prefs_n].key = strdup(key);
  g_prefs[g_prefs_n].val = strdup(val ? val : "");
  g_prefs_n++;
  g_prefs_dirty = 1;
}

static void prefs_remove(const char *key) {
  prefs_load();
  for (int i = 0; i < g_prefs_n; i++)
    if (!strcmp(g_prefs[i].key, key)) {
      free(g_prefs[i].key);
      free(g_prefs[i].val);
      g_prefs[i] = g_prefs[--g_prefs_n];
      g_prefs_dirty = 1;
      return;
    }
}

/* ------------------------------------------------------------- handlers --- */
static const char *arg_str(nx_ctx *c) {
  const char *s = nx_cstr(nx_arg_obj(c));
  return s ? s : "";
}

static nx_jval h_writable_path(nx_ctx *c) { return nx_str(c, g_writable); }
static nx_jval h_package_name(nx_ctx *c) { return nx_str(c, g_package); }
/* Regra do NextOS: o jogo abre em INGLES, sempre. */
static nx_jval h_language(nx_ctx *c) { return nx_str(c, "en"); }
static nx_jval h_dpi(nx_ctx *c) {
  (void)c;
  return nx_int(160);
}
/* The engine turns this into the frame interval (1/rate) and, once the level
 * starts, snaps every frame delta to it.  Answering the wrong width here made
 * the rate read as 0, the interval as infinity, and the delta as infinity --
 * which the collision pass then tried to simulate.  Both widths are served. */
#define GD_REFRESH_HZ 60
static nx_jval h_refresh_rate(nx_ctx *c) {
  (void)c;
  return nx_int(GD_REFRESH_HZ);
}
static nx_jval h_refresh_rate_f(nx_ctx *c) {
  (void)c;
  return nx_float((float)GD_REFRESH_HZ);
}
static nx_jval h_refresh_rate_d(nx_ctx *c) {
  (void)c;
  return nx_dbl((double)GD_REFRESH_HZ);
}
static nx_jval h_user_id(nx_ctx *c) { return nx_str(c, "0"); }

/* ------------------------------------------------ org/fmod/AudioDevice ----
 * This ELF lists no OpenSLES in NEEDED, so FMOD picks its Java output: it
 * builds an org.fmod.AudioDevice, calls init(channels, rate, buffers, samples)
 * and then feeds it short[] blocks from its mixer thread.  With those methods
 * missing the table answered a silent 0, init() read as false, and FMOD fell
 * back to NoSound -- the whole reason the port had no audio.
 *
 * SDL's queued audio backs it: write() blocks while the queue is deep enough,
 * which is the same back-pressure AudioTrack.write() gives FMOD's mixer.  The
 * audio driver is never forced; SDL chooses it. */
static SDL_AudioDeviceID g_fmod_dev;
static int g_fmod_high_water;   /* bytes: how deep the queue may get */
static int g_fmod_bytes_per_frame;

static nx_jval h_audiodev_init(nx_ctx *c) {
  int channels = nx_arg_int(c);
  int rate = nx_arg_int(c);
  int a3 = nx_arg_int(c);
  int a4 = nx_arg_int(c);
  if (channels <= 0) channels = 2;
  if (rate <= 0) rate = 44100;
  /* FMOD hands over a block size and a block count; which of the two comes
   * first differs between its Java outputs, and reading them the wrong way
   * round opened the device with a 4-sample buffer.  The block is always the
   * bigger of the pair. */
  int samples = a3 > a4 ? a3 : a4;
  int buffers = a3 > a4 ? a4 : a3;
  if (samples <= 0) samples = 1024;
  if (buffers <= 0) buffers = 4;
  if (samples < 256) samples = 256;
  if (samples > 4096) samples = 4096;

  if (g_fmod_dev) {
    SDL_CloseAudioDevice(g_fmod_dev);
    g_fmod_dev = 0;
  }
  SDL_AudioSpec want, have;
  memset(&want, 0, sizeof(want));
  want.freq = rate;
  want.format = AUDIO_S16SYS;
  want.channels = (Uint8)channels;
  want.samples = (Uint16)samples;
  want.callback = NULL;   /* queued audio */
  g_fmod_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                   SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
  if (!g_fmod_dev) {
    debugPrintf("[audio] SDL_OpenAudioDevice falhou: %s\n", SDL_GetError());
    return nx_bool(0);
  }
  g_fmod_bytes_per_frame = have.channels * 2;
  g_fmod_high_water = buffers * samples * g_fmod_bytes_per_frame;
  SDL_PauseAudioDevice(g_fmod_dev, 0);
  debugPrintf("[audio] AudioDevice.init(ch=%d rate=%d buf=%d samples=%d) -> "
              "SDL %dHz %dch, fila max %d bytes\n",
              channels, rate, buffers, samples, have.freq, have.channels,
              g_fmod_high_water);
  return nx_bool(1);
}

static nx_jval h_audiodev_write(nx_ctx *c) {
  void *arr = nx_arg_obj(c);
  int shorts = nx_arg_int(c);
  nx_array *a = as_array(arr);
  if (!g_fmod_dev || !a || shorts <= 0)
    return nx_none();
  int bytes = shorts * 2;
  if (bytes > a->len * a->elemsz)
    bytes = a->len * a->elemsz;
  /* Back-pressure with a ceiling: FMOD's mixer thread must not spin here for
   * ever if the device stalls, or the whole game stalls with it. */
  for (int i = 0; i < 200 &&
                  (int)SDL_GetQueuedAudioSize(g_fmod_dev) > g_fmod_high_water;
       i++)
    SDL_Delay(1);
  SDL_QueueAudio(g_fmod_dev, a->data, (Uint32)bytes);
  /* Peak of the block: the route can be perfect and still be silent if what
   * FMOD mixes is all zeros, and only the samples themselves tell the two
   * apart. */
  static unsigned n;
  if ((n++ % 3000) == 0) {
    const int16_t *p = (const int16_t *)a->data;
    int peak = 0;
    for (int i = 0; i < shorts; i++) {
      int v = p[i] < 0 ? -p[i] : p[i];
      if (v > peak)
        peak = v;
    }
    debugPrintf("[audio] write #%u: %d bytes, pico=%d, fila=%u\n", n - 1, bytes,
                peak, SDL_GetQueuedAudioSize(g_fmod_dev));
  }
  return nx_none();
}

static nx_jval h_audiodev_close(nx_ctx *c) {
  (void)c;
  if (g_fmod_dev) {
    SDL_CloseAudioDevice(g_fmod_dev);
    g_fmod_dev = 0;
    debugPrintf("[audio] AudioDevice.close()\n");
  }
  return nx_none();
}
static nx_jval h_audiodev_ctor(nx_ctx *c) {
  (void)c;
  return nx_none();
}

/* android.os.SystemClock.uptimeMillis() -- JniHelper::getPlatformTimestamp().
 * This is the game's ONLY clock: with the table's safe default of 0 the engine
 * sees time standing still, so a level freezes the instant it starts while the
 * renderer happily keeps drawing the same frame at 60 fps.  Monotonic, and
 * zeroed at the first call so the value stays small like Android's. */

/* The engine's frame delta comes from here, and it steps its physics one fixed
 * tick at a time for however long that delta says.  A real stall -- loading a
 * level out of the 217 MB apk takes seconds -- would therefore be charged to
 * the first frame after it, and the collision pass would run for millions of
 * ticks: the game does not crash, it simply never returns from nativeRender.
 * So the clock advances in real time but refuses to jump: a gap longer than a
 * few frames is reported as one frame, exactly as if the game had been paused
 * through it. */
extern int64_t gd_clock_us(void);

int64_t jni_shim_uptime_ms(void) { return gd_clock_us() / 1000; }

static nx_jval h_uptime_millis(nx_ctx *c) {
  (void)c;
  return nx_long(jni_shim_uptime_ms());
}
static nx_jval h_out_rate(nx_ctx *c) {
  (void)c;
  return nx_int(44100);
}
static nx_jval h_out_block(nx_ctx *c) {
  (void)c;
  return nx_int(1024);
}
static nx_jval h_false(nx_ctx *c) {
  (void)c;
  return nx_bool(0);
}
static nx_jval h_true(nx_ctx *c) {
  (void)c;
  return nx_bool(1);
}
static nx_jval h_nop(nx_ctx *c) {
  (void)c;
  return nx_none();
}
static nx_jval h_terminate(nx_ctx *c) {
  (void)c;
  debugPrintf("[jni] terminateProcess\n");
  jni_shim_prefs_flush();
  _exit(0);
  return nx_none();
}
static nx_jval h_open_url(nx_ctx *c) {
  debugPrintf("[jni] openURL(%s) ignorado\n", arg_str(c));
  return nx_none();
}
static nx_jval h_show_dialog(nx_ctx *c) {
  const char *t = arg_str(c);
  debugPrintf("[jni] showDialog: %s\n", t);
  return nx_none();
}
static nx_jval h_set_anim_interval(nx_ctx *c) {
  double d = nx_arg_dbl(c);
  debugPrintf("[jni] setAnimationInterval(%.4f)\n", d);
  return nx_none();
}

/* --- CCUserDefault (SharedPreferences) --- */
static void kv_key(char *out, size_t n, const char *k) {
  snprintf(out, n, "ud.%s", k ? k : "");
}
static nx_jval h_get_bool(nx_ctx *c) {
  char k[256];
  kv_key(k, sizeof(k), arg_str(c));
  int def = nx_arg_int(c);
  const char *v = prefs_get(k, NULL);
  return nx_bool(v ? atoi(v) : def);
}
static nx_jval h_set_bool(nx_ctx *c) {
  char k[256];
  kv_key(k, sizeof(k), arg_str(c));
  char b[8];
  snprintf(b, sizeof(b), "%d", nx_arg_int(c) ? 1 : 0);
  prefs_set(k, b);
  return nx_none();
}
static nx_jval h_get_int(nx_ctx *c) {
  char k[256];
  kv_key(k, sizeof(k), arg_str(c));
  int def = nx_arg_int(c);
  const char *v = prefs_get(k, NULL);
  return nx_int(v ? atoi(v) : def);
}
static nx_jval h_set_int(nx_ctx *c) {
  char k[256];
  kv_key(k, sizeof(k), arg_str(c));
  char b[32];
  snprintf(b, sizeof(b), "%d", nx_arg_int(c));
  prefs_set(k, b);
  return nx_none();
}
/* jfloat travels through the variadic call promoted to double. */
static nx_jval h_get_float(nx_ctx *c) {
  char k[256];
  kv_key(k, sizeof(k), arg_str(c));
  double def = nx_arg_dbl(c);
  const char *v = prefs_get(k, NULL);
  nx_jval r;
  r.f = (float)(v ? atof(v) : def);
  return r;
}
static nx_jval h_set_float(nx_ctx *c) {
  char k[256];
  kv_key(k, sizeof(k), arg_str(c));
  char b[64];
  snprintf(b, sizeof(b), "%.9g", nx_arg_dbl(c));
  prefs_set(k, b);
  return nx_none();
}
static nx_jval h_get_double(nx_ctx *c) {
  char k[256];
  kv_key(k, sizeof(k), arg_str(c));
  double def = nx_arg_dbl(c);
  const char *v = prefs_get(k, NULL);
  nx_jval r;
  r.d = v ? atof(v) : def;
  return r;
}
static nx_jval h_set_double(nx_ctx *c) {
  char k[256];
  kv_key(k, sizeof(k), arg_str(c));
  char b[64];
  snprintf(b, sizeof(b), "%.17g", nx_arg_dbl(c));
  prefs_set(k, b);
  return nx_none();
}
static nx_jval h_get_string(nx_ctx *c) {
  char k[256];
  kv_key(k, sizeof(k), arg_str(c));
  const char *def = arg_str(c);
  return nx_str(c, prefs_get(k, def));
}
static nx_jval h_set_string(nx_ctx *c) {
  char k[256];
  kv_key(k, sizeof(k), arg_str(c));
  prefs_set(k, arg_str(c));
  return nx_none();
}

/* --- Cocos2dxLocalStorage --- */
static nx_jval h_ls_get(nx_ctx *c) {
  char k[256];
  snprintf(k, sizeof(k), "ls.%s", arg_str(c));
  const char *v = prefs_get(k, NULL);
  return v ? nx_str(c, v) : nx_ptr(NULL);
}
static nx_jval h_ls_set(nx_ctx *c) {
  char k[256];
  snprintf(k, sizeof(k), "ls.%s", arg_str(c));
  prefs_set(k, arg_str(c));
  return nx_none();
}
static nx_jval h_ls_remove(nx_ctx *c) {
  char k[256];
  snprintf(k, sizeof(k), "ls.%s", arg_str(c));
  prefs_remove(k);
  return nx_none();
}

/* --- SimpleCrypto: identity.  Both writer and reader are us. --- */
static nx_jval h_crypto(nx_ctx *c) {
  nx_arg_obj(c); /* key */
  return nx_str(c, arg_str(c));
}

/* --- Cocos2dxBitmap.createTextBitmapShadowStroke --- */
extern void text_render_shadow_stroke(void *env, const char *text,
                                      const char *font, int size, float r,
                                      float g, float b, int align, int width,
                                      int height, int shadow, float sdx,
                                      float sdy, float sblur, int stroke,
                                      float sr, float sg, float sb,
                                      float ssize, jni_bitmap_dc_fn dc);

static nx_jval h_create_text_bitmap(nx_ctx *c) {
  const char *text = arg_str(c);
  const char *font = arg_str(c);
  int size = nx_arg_int(c);
  float r = (float)nx_arg_dbl(c);
  float g = (float)nx_arg_dbl(c);
  float b = (float)nx_arg_dbl(c);
  int align = nx_arg_int(c);
  int width = nx_arg_int(c);
  int height = nx_arg_int(c);
  int shadow = nx_arg_int(c);
  float sdx = (float)nx_arg_dbl(c);
  float sdy = (float)nx_arg_dbl(c);
  float sblur = (float)nx_arg_dbl(c);
  int stroke = nx_arg_int(c);
  float sr = (float)nx_arg_dbl(c);
  float sg = (float)nx_arg_dbl(c);
  float sb = (float)nx_arg_dbl(c);
  float ssize = (float)nx_arg_dbl(c);
  text_render_shadow_stroke(c->env, text, font, size, r, g, b, align, width,
                            height, shadow, sdx, sdy, sblur, stroke, sr, sg, sb,
                            ssize, g_bitmap_dc);
  return nx_none();
}

static const nx_method METHODS[] = {
    /* Cocos2dxHelper */
    {"getCocos2dxWritablePath", NULL, NX_OBJ, h_writable_path},
    {"getCocos2dxPackageName", NULL, NX_OBJ, h_package_name},
    {"getCurrentLanguage", NULL, NX_OBJ, h_language},
    {"getDPI", NULL, NX_INT, h_dpi},
    {"setAnimationInterval", NULL, NX_VOID, h_set_anim_interval},
    {"enableAccelerometer", NULL, NX_VOID, h_nop},
    {"disableAccelerometer", NULL, NX_VOID, h_nop},
    {"setAccelerometerInterval", NULL, NX_VOID, h_nop},
    {"openURL", NULL, NX_VOID, h_open_url},
    {"showDialog", NULL, NX_VOID, h_show_dialog},
    {"showEditTextDialog", NULL, NX_VOID, h_nop},
    {"terminateProcess", NULL, NX_VOID, h_terminate},
    {"getBoolForKey", NULL, NX_BOOL, h_get_bool},
    {"setBoolForKey", NULL, NX_VOID, h_set_bool},
    {"getIntegerForKey", NULL, NX_INT, h_get_int},
    {"setIntegerForKey", NULL, NX_VOID, h_set_int},
    {"getFloatForKey", NULL, NX_FLOAT, h_get_float},
    {"setFloatForKey", NULL, NX_VOID, h_set_float},
    {"getDoubleForKey", NULL, NX_DOUBLE, h_get_double},
    {"setDoubleForKey", NULL, NX_VOID, h_set_double},
    {"getStringForKey", NULL, NX_OBJ, h_get_string},
    {"setStringForKey", NULL, NX_VOID, h_set_string},
    /* Cocos2dxLocalStorage */
    {"getItem", NULL, NX_OBJ, h_ls_get},
    {"setItem", NULL, NX_VOID, h_ls_set},
    {"removeItem", NULL, NX_VOID, h_ls_remove},
    /* BaseRobTopActivity */
    /* Signature-specific first: the generic entry below matches any of them. */
    {"getDeviceRefreshRate", "()F", NX_FLOAT, h_refresh_rate_f},
    {"getDeviceRefreshRate", "()D", NX_DOUBLE, h_refresh_rate_d},
    {"getDeviceRefreshRate", "()I", NX_INT, h_refresh_rate},
    {"getDeviceRefreshRate", NULL, NX_INT, h_refresh_rate},
    {"getUserID", NULL, NX_OBJ, h_user_id},
    {"isNetworkAvailable", NULL, NX_BOOL, h_false},
    {"showInterstitial", NULL, NX_VOID, h_nop},
    {"cacheInterstitial", NULL, NX_VOID, h_nop},
    {"hasCachedInterstitial", NULL, NX_BOOL, h_false},
    {"showRewardedVideo", NULL, NX_VOID, h_nop},
    {"cacheRewardedVideo", NULL, NX_VOID, h_nop},
    {"hasCachedRewardedVideo", NULL, NX_BOOL, h_false},
    {"enableBanner", NULL, NX_VOID, h_nop},
    {"enableBannerNoRefresh", NULL, NX_VOID, h_nop},
    {"disableBanner", NULL, NX_VOID, h_nop},
    {"queueRefreshBanner", NULL, NX_VOID, h_nop},
    {"showAchievements", NULL, NX_VOID, h_nop},
    {"unlockAchievement", NULL, NX_VOID, h_nop},
    {"reportedAchievements", NULL, NX_VOID, h_nop},
    {"tryShowRateDialog", NULL, NX_VOID, h_nop},
    /* Cocos2dxBitmap */
    {"createTextBitmapShadowStroke", NULL, NX_VOID, h_create_text_bitmap},
    /* SimpleCrypto */
    {"encryptString", NULL, NX_OBJ, h_crypto},
    {"decryptString", NULL, NX_OBJ, h_crypto},
    {"encrypt", NULL, NX_OBJ, h_crypto},
    {"decrypt", NULL, NX_OBJ, h_crypto},
    /* org/fmod/AudioDevice -- FMOD's Java output, backed by SDL */
    {"<init>", "()V", NX_VOID, h_audiodev_ctor},
    {"init", "(IIII)Z", NX_BOOL, h_audiodev_init},
    {"write", "([SI)V", NX_VOID, h_audiodev_write},
    {"close", "()V", NX_VOID, h_audiodev_close},
    /* android/os/SystemClock */
    {"uptimeMillis", NULL, NX_LONG, h_uptime_millis},
    {"elapsedRealtime", NULL, NX_LONG, h_uptime_millis},
    /* Cocos2dxETCLoader */
    {"loadTexture", NULL, NX_BOOL, h_false},
    /* org/fmod/FMOD -- libfmod refuses to pick a platform output backend until
     * checkInit() says the Java side is up.  Answering no here is what leaves
     * the game silent on FMOD's "NoSound" output. */
    {"checkInit", NULL, NX_BOOL, h_true},
    {"supportsAAudio", NULL, NX_BOOL, h_false},
    {"supportsLowLatency", NULL, NX_BOOL, h_false},
    {"getOutputSampleRate", NULL, NX_INT, h_out_rate},
    {"getOutputBlockSize", NULL, NX_INT, h_out_block},
    NX_METHOD_END,
};
static const nx_field FIELDS[] = {NX_FIELD_END};

static nx_jni_config g_cfg = {"com.robtopx.geometryjump", 11, METHODS,
                              FIELDS};

/* ---------------------------------------------------------------- JNIEnv -- */
static intptr_t jni_stub(void) { return 0; }

static jint jni_GetVersion(void *env) {
  (void)env;
  return 0x00010006;
}

static void *jni_FindClass(void *env, const char *name) {
  (void)env;
  /* One stable object per class name, so the guest can cache it. */
  static struct {
    char name[96];
    int tag;
  } cls[32];
  static int n;
  for (int i = 0; i < n; i++)
    if (!strcmp(cls[i].name, name))
      return &cls[i];
  if (n < 32) {
    snprintf(cls[n].name, sizeof(cls[n].name), "%s", name);
    debugPrintf("[jni] FindClass(%s)\n", name);
    return &cls[n++];
  }
  static int fallback;
  return &fallback;
}

/* A real object would carry fields; nothing here needs any, so a distinct
 * allocation per call is enough for the guest to hold and compare. */
static void *jni_NewObject(void *env, void *clazz, void *mid, ...) {
  (void)env;
  (void)clazz;
  (void)mid;
  return calloc(1, 16);
}
static void *jni_NewObjectV(void *env, void *clazz, void *mid, va_list ap) {
  (void)ap;
  return jni_NewObject(env, clazz, mid);
}
static void *jni_NewObjectA(void *env, void *clazz, void *mid, const void *a) {
  (void)a;
  return jni_NewObject(env, clazz, mid);
}
static void *jni_AllocObject(void *env, void *clazz) {
  return jni_NewObject(env, clazz, NULL);
}

static void *jni_GetMethodID(void *env, void *clazz, const char *name,
                             const char *sig) {
  (void)env;
  (void)clazz;
  return nx_method_id(&g_cfg, name, sig);
}
static void *jni_GetStaticMethodID(void *env, void *clazz, const char *name,
                                   const char *sig) {
  (void)env;
  (void)clazz;
  return nx_method_id(&g_cfg, name, sig);
}
static void *jni_GetFieldID(void *env, void *clazz, const char *name,
                            const char *sig) {
  (void)env;
  (void)clazz;
  return nx_field_id(&g_cfg, name, sig);
}
static void *jni_GetStaticFieldID(void *env, void *clazz, const char *name,
                                  const char *sig) {
  (void)env;
  (void)clazz;
  return nx_field_id(&g_cfg, name, sig);
}

/* The guest uses both the variadic and the va_list forms; generate both. */
#define CALL_FAMILY(NAME, RET, FIELD)                                          \
  static RET jni_##NAME(void *env, void *obj, void *mid, ...) {                \
    va_list ap;                                                                \
    va_start(ap, mid);                                                         \
    nx_jval v = nx_dispatch(&g_cfg, env, obj, mid, &ap, NULL);                 \
    va_end(ap);                                                                \
    return (RET)v.FIELD;                                                       \
  }                                                                            \
  static RET jni_##NAME##V(void *env, void *obj, void *mid, va_list ap) {      \
    va_list cp;                                                                \
    va_copy(cp, ap);                                                           \
    nx_jval v = nx_dispatch(&g_cfg, env, obj, mid, &cp, NULL);                 \
    va_end(cp);                                                                \
    return (RET)v.FIELD;                                                       \
  }                                                                            \
  static RET jni_##NAME##A(void *env, void *obj, void *mid, const void *args) {\
    nx_jval v = nx_dispatch(&g_cfg, env, obj, mid, NULL, args);                \
    return (RET)v.FIELD;                                                       \
  }

CALL_FAMILY(CallObject, void *, l)
CALL_FAMILY(CallBoolean, jboolean, i)
CALL_FAMILY(CallInt, jint, i)
CALL_FAMILY(CallLong, int64_t, j)
CALL_FAMILY(CallFloat, float, f)
CALL_FAMILY(CallDouble, double, d)
CALL_FAMILY(CallStaticObject, void *, l)
CALL_FAMILY(CallStaticBoolean, jboolean, i)
CALL_FAMILY(CallStaticInt, jint, i)
CALL_FAMILY(CallStaticLong, int64_t, j)
CALL_FAMILY(CallStaticFloat, float, f)
CALL_FAMILY(CallStaticDouble, double, d)

static void jni_CallVoid(void *env, void *obj, void *mid, ...) {
  va_list ap;
  va_start(ap, mid);
  nx_dispatch(&g_cfg, env, obj, mid, &ap, NULL);
  va_end(ap);
}
static void jni_CallVoidV(void *env, void *obj, void *mid, va_list ap) {
  va_list cp;
  va_copy(cp, ap);
  nx_dispatch(&g_cfg, env, obj, mid, &cp, NULL);
  va_end(cp);
}
static void jni_CallVoidA(void *env, void *obj, void *mid, const void *args) {
  nx_dispatch(&g_cfg, env, obj, mid, NULL, args);
}
static void jni_CallStaticVoid(void *env, void *clazz, void *mid, ...) {
  va_list ap;
  va_start(ap, mid);
  nx_dispatch(&g_cfg, env, clazz, mid, &ap, NULL);
  va_end(ap);
}
static void jni_CallStaticVoidV(void *env, void *clazz, void *mid, va_list ap) {
  va_list cp;
  va_copy(cp, ap);
  nx_dispatch(&g_cfg, env, clazz, mid, &cp, NULL);
  va_end(cp);
}
static void jni_CallStaticVoidA(void *env, void *clazz, void *mid,
                                const void *args) {
  nx_dispatch(&g_cfg, env, clazz, mid, NULL, args);
}

static void *jni_GetStaticObjectField(void *env, void *clazz, void *fid) {
  nx_jval v = nx_dispatch_field(&g_cfg, env, clazz, fid);
  return v.l;
}
static jint jni_GetStaticIntField(void *env, void *clazz, void *fid) {
  nx_jval v = nx_dispatch_field(&g_cfg, env, clazz, fid);
  return v.i;
}
static jboolean jni_GetStaticBooleanField(void *env, void *clazz, void *fid) {
  nx_jval v = nx_dispatch_field(&g_cfg, env, clazz, fid);
  return (jboolean)v.i;
}

/* ------------------------------------------------------------- strings ---- */
static void *jni_NewStringUTF(void *env, const char *str) {
  (void)env;
  nx_ctx c;
  memset(&c, 0, sizeof(c));
  return nx_new_string(&c, str ? str : "");
}
static jint jni_GetStringUTFLength(void *env, void *s) {
  (void)env;
  const char *p = nx_cstr(s);
  return (jint)(p ? strlen(p) : 0);
}
static const char *jni_GetStringUTFChars(void *env, void *s, void *isCopy) {
  (void)env;
  if (isCopy)
    *(jboolean *)isCopy = 0;
  const char *p = nx_cstr(s);
  return p ? p : "";
}
static void jni_ReleaseStringUTFChars(void *env, void *s, const char *c) {
  (void)env;
  (void)s;
  (void)c;
}
static jint jni_GetStringLength(void *env, void *s) {
  return jni_GetStringUTFLength(env, s);
}
static void jni_GetStringUTFRegion(void *env, void *s, jint start, jint len,
                                   char *buf) {
  (void)env;
  const char *p = nx_cstr(s);
  if (!p || !buf)
    return;
  size_t n = strlen(p);
  if ((size_t)start >= n)
    return;
  size_t take = (size_t)len;
  if (start + take > n)
    take = n - (size_t)start;
  memcpy(buf, p + start, take);
}

/* -------------------------------------------------------------- arrays ---- */
static jint jni_GetArrayLength(void *env, void *arr) {
  (void)env;
  nx_array *a = as_array(arr);
  return a ? a->len : 0;
}
static void *jni_NewByteArray(void *env, jint len) {
  (void)env;
  return new_array(1, len, NULL);
}
static void *jni_NewIntArray(void *env, jint len) {
  (void)env;
  return new_array(4, len, NULL);
}
static void *jni_NewShortArray(void *env, jint len) {
  (void)env;
  return new_array(2, len, NULL);
}
static void *jni_NewFloatArray(void *env, jint len) {
  (void)env;
  return new_array(4, len, NULL);
}
static void *jni_GetArrayElements(void *env, void *arr, void *isCopy) {
  (void)env;
  if (isCopy)
    *(jboolean *)isCopy = 0;
  nx_array *a = as_array(arr);
  return a ? a->data : NULL;
}
static void jni_ReleaseArrayElements(void *env, void *arr, void *elems,
                                     jint mode) {
  (void)env;
  (void)arr;
  (void)elems;
  (void)mode;
}
static void jni_GetArrayRegion(void *env, void *arr, jint start, jint len,
                               void *buf) {
  (void)env;
  nx_array *a = as_array(arr);
  if (!a || !buf || start < 0 || len < 0 || start + len > a->len)
    return;
  memcpy(buf, (char *)a->data + (size_t)start * a->elemsz,
         (size_t)len * a->elemsz);
}
static void jni_SetArrayRegion(void *env, void *arr, jint start, jint len,
                               const void *buf) {
  (void)env;
  nx_array *a = as_array(arr);
  if (!a || !buf || start < 0 || len < 0 || start + len > a->len)
    return;
  memcpy((char *)a->data + (size_t)start * a->elemsz, buf,
         (size_t)len * a->elemsz);
}

/* ----------------------------------------------------------------- refs --- */
static void *jni_id(void *env, void *obj) {
  (void)env;
  return obj;
}
static void jni_void_obj(void *env, void *obj) {
  (void)env;
  (void)obj;
}
static void *jni_GetObjectClass(void *env, void *obj) {
  (void)env;
  (void)obj;
  static int fake;
  return &fake;
}
static jboolean jni_ExceptionCheck(void *env) {
  (void)env;
  return 0;
}
static void *jni_ExceptionOccurred(void *env) {
  (void)env;
  return NULL;
}
static void jni_ExceptionClear(void *env) { (void)env; }
static jint jni_RegisterNatives(void *env, void *clazz, const void *m, jint n) {
  (void)env;
  (void)clazz;
  (void)m;
  debugPrintf("[jni] RegisterNatives(%d) ignorado (usamos os exports Java_*)\n",
              (int)n);
  return 0;
}
static jint jni_GetJavaVM(void *env, void **vm) {
  (void)env;
  if (vm)
    *vm = &java_vm_ptr;
  return 0;
}
static jint jni_MonitorOp(void *env, void *obj) {
  (void)env;
  (void)obj;
  return 0;
}

/* ---------------------------------------------------------------- JavaVM -- */
static jint vm_Destroy(void *vm) {
  (void)vm;
  return 0;
}
static jint vm_Attach(void *vm, void **penv, void *args) {
  (void)vm;
  (void)args;
  if (penv)
    *penv = &jni_env_ptr;
  return 0;
}
static jint vm_Detach(void *vm) {
  (void)vm;
  return 0;
}
static jint vm_GetEnv(void *vm, void **penv, jint version) {
  (void)vm;
  (void)version;
  if (penv)
    *penv = &jni_env_ptr;
  return 0;
}

void jni_shim_init(void **out_vm, void **out_env) {
  for (int i = 0; i < JNI_VTABLE_SIZE; i++) {
    jni_env_vtable[i] = (uintptr_t)jni_stub;
    java_vm_vtable[i] = (uintptr_t)jni_stub;
  }

  /* Indices straight out of the NDK jni.h JNINativeInterface layout. */
  jni_env_vtable[4] = (uintptr_t)jni_GetVersion;
  jni_env_vtable[6] = (uintptr_t)jni_FindClass;
  jni_env_vtable[15] = (uintptr_t)jni_ExceptionOccurred;
  jni_env_vtable[17] = (uintptr_t)jni_ExceptionClear;
  jni_env_vtable[21] = (uintptr_t)jni_id;      /* NewGlobalRef */
  jni_env_vtable[22] = (uintptr_t)jni_void_obj; /* DeleteGlobalRef */
  jni_env_vtable[23] = (uintptr_t)jni_void_obj; /* DeleteLocalRef */
  jni_env_vtable[25] = (uintptr_t)jni_id;      /* NewLocalRef */
  jni_env_vtable[27] = (uintptr_t)jni_AllocObject;
  jni_env_vtable[28] = (uintptr_t)jni_NewObject;
  jni_env_vtable[29] = (uintptr_t)jni_NewObjectV;
  jni_env_vtable[30] = (uintptr_t)jni_NewObjectA;
  jni_env_vtable[31] = (uintptr_t)jni_GetObjectClass;
  jni_env_vtable[33] = (uintptr_t)jni_GetMethodID;

  jni_env_vtable[34] = (uintptr_t)jni_CallObject;
  jni_env_vtable[35] = (uintptr_t)jni_CallObjectV;
  jni_env_vtable[36] = (uintptr_t)jni_CallObjectA;
  jni_env_vtable[37] = (uintptr_t)jni_CallBoolean;
  jni_env_vtable[38] = (uintptr_t)jni_CallBooleanV;
  jni_env_vtable[39] = (uintptr_t)jni_CallBooleanA;
  jni_env_vtable[49] = (uintptr_t)jni_CallInt;
  jni_env_vtable[50] = (uintptr_t)jni_CallIntV;
  jni_env_vtable[51] = (uintptr_t)jni_CallIntA;
  jni_env_vtable[52] = (uintptr_t)jni_CallLong;
  jni_env_vtable[53] = (uintptr_t)jni_CallLongV;
  jni_env_vtable[54] = (uintptr_t)jni_CallLongA;
  jni_env_vtable[55] = (uintptr_t)jni_CallFloat;
  jni_env_vtable[56] = (uintptr_t)jni_CallFloatV;
  jni_env_vtable[57] = (uintptr_t)jni_CallFloatA;
  jni_env_vtable[58] = (uintptr_t)jni_CallDouble;
  jni_env_vtable[59] = (uintptr_t)jni_CallDoubleV;
  jni_env_vtable[60] = (uintptr_t)jni_CallDoubleA;
  jni_env_vtable[61] = (uintptr_t)jni_CallVoid;
  jni_env_vtable[62] = (uintptr_t)jni_CallVoidV;
  jni_env_vtable[63] = (uintptr_t)jni_CallVoidA;

  jni_env_vtable[94] = (uintptr_t)jni_GetFieldID;
  jni_env_vtable[113] = (uintptr_t)jni_GetStaticMethodID;
  jni_env_vtable[114] = (uintptr_t)jni_CallStaticObject;
  jni_env_vtable[115] = (uintptr_t)jni_CallStaticObjectV;
  jni_env_vtable[116] = (uintptr_t)jni_CallStaticObjectA;
  jni_env_vtable[117] = (uintptr_t)jni_CallStaticBoolean;
  jni_env_vtable[118] = (uintptr_t)jni_CallStaticBooleanV;
  jni_env_vtable[119] = (uintptr_t)jni_CallStaticBooleanA;
  jni_env_vtable[129] = (uintptr_t)jni_CallStaticInt;
  jni_env_vtable[130] = (uintptr_t)jni_CallStaticIntV;
  jni_env_vtable[131] = (uintptr_t)jni_CallStaticIntA;
  jni_env_vtable[132] = (uintptr_t)jni_CallStaticLong;
  jni_env_vtable[133] = (uintptr_t)jni_CallStaticLongV;
  jni_env_vtable[134] = (uintptr_t)jni_CallStaticLongA;
  jni_env_vtable[135] = (uintptr_t)jni_CallStaticFloat;
  jni_env_vtable[136] = (uintptr_t)jni_CallStaticFloatV;
  jni_env_vtable[137] = (uintptr_t)jni_CallStaticFloatA;
  jni_env_vtable[138] = (uintptr_t)jni_CallStaticDouble;
  jni_env_vtable[139] = (uintptr_t)jni_CallStaticDoubleV;
  jni_env_vtable[140] = (uintptr_t)jni_CallStaticDoubleA;
  jni_env_vtable[141] = (uintptr_t)jni_CallStaticVoid;
  jni_env_vtable[142] = (uintptr_t)jni_CallStaticVoidV;
  jni_env_vtable[143] = (uintptr_t)jni_CallStaticVoidA;
  jni_env_vtable[144] = (uintptr_t)jni_GetStaticFieldID;
  jni_env_vtable[145] = (uintptr_t)jni_GetStaticObjectField;
  jni_env_vtable[146] = (uintptr_t)jni_GetStaticBooleanField;
  jni_env_vtable[150] = (uintptr_t)jni_GetStaticIntField;

  jni_env_vtable[163] = (uintptr_t)jni_NewStringUTF; /* NewString */
  jni_env_vtable[164] = (uintptr_t)jni_GetStringLength;
  jni_env_vtable[167] = (uintptr_t)jni_NewStringUTF;
  jni_env_vtable[168] = (uintptr_t)jni_GetStringUTFLength;
  jni_env_vtable[169] = (uintptr_t)jni_GetStringUTFChars;
  jni_env_vtable[170] = (uintptr_t)jni_ReleaseStringUTFChars;

  jni_env_vtable[171] = (uintptr_t)jni_GetArrayLength;
  jni_env_vtable[176] = (uintptr_t)jni_NewByteArray;
  jni_env_vtable[178] = (uintptr_t)jni_NewShortArray;
  jni_env_vtable[179] = (uintptr_t)jni_NewIntArray;
  jni_env_vtable[181] = (uintptr_t)jni_NewFloatArray;
  for (int i = 183; i <= 190; i++)
    jni_env_vtable[i] = (uintptr_t)jni_GetArrayElements;
  for (int i = 191; i <= 198; i++)
    jni_env_vtable[i] = (uintptr_t)jni_ReleaseArrayElements;
  for (int i = 199; i <= 206; i++)
    jni_env_vtable[i] = (uintptr_t)jni_GetArrayRegion;
  for (int i = 207; i <= 214; i++)
    jni_env_vtable[i] = (uintptr_t)jni_SetArrayRegion;

  jni_env_vtable[215] = (uintptr_t)jni_RegisterNatives;
  jni_env_vtable[217] = (uintptr_t)jni_MonitorOp;
  jni_env_vtable[218] = (uintptr_t)jni_MonitorOp;
  jni_env_vtable[219] = (uintptr_t)jni_GetJavaVM;
  jni_env_vtable[221] = (uintptr_t)jni_GetStringUTFRegion;
  jni_env_vtable[222] = (uintptr_t)jni_GetArrayElements;
  jni_env_vtable[223] = (uintptr_t)jni_ReleaseArrayElements;
  jni_env_vtable[226] = (uintptr_t)jni_id;      /* NewWeakGlobalRef */
  jni_env_vtable[227] = (uintptr_t)jni_void_obj; /* DeleteWeakGlobalRef */
  jni_env_vtable[228] = (uintptr_t)jni_ExceptionCheck;

  jni_env_ptr = jni_env_vtable;

  java_vm_vtable[3] = (uintptr_t)vm_Destroy;
  java_vm_vtable[4] = (uintptr_t)vm_Attach;
  java_vm_vtable[5] = (uintptr_t)vm_Detach;
  java_vm_vtable[6] = (uintptr_t)vm_GetEnv;
  java_vm_vtable[7] = (uintptr_t)vm_Attach;
  java_vm_ptr = java_vm_vtable;

  if (out_vm)
    *out_vm = &java_vm_ptr;
  if (out_env)
    *out_env = &jni_env_ptr;
  debugPrintf("[jni] pronto (vm=%p env=%p)\n", &java_vm_ptr, &jni_env_ptr);
}
