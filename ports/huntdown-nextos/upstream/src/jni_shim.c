/*
 * jni_shim.c -- Android JNI bridge for Huntdown
 *
 * Android JNI works through double-indirection:
 *   JavaVM *vm;   vm->GetEnv(vm, &env, version)
 *   JNIEnv *env;  env->FindClass(env, "com/foo/Bar")
 *
 * Both vm and env are pointers to a pointer to a function table.
 * We create large stub vtables that return 0/NULL for everything,
 * with specific overrides for methods Huntdown and Unity actually use.
 */

#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "jni_shim.h"
#include "huntdown_display.h"
#include "nx_jni.h"
#include "util.h"

#define JNI_VTABLE_SIZE 512

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef int jint;
typedef unsigned char jboolean;
typedef union {
  jboolean z;
  signed char b;
  unsigned short c;
  short s;
  jint i;
  long j;
  float f;
  double d;
  void *l;
} jvalue;

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
  MID_GENERIC,
  FID_OBB_VERSIONCODE,
  FID_GENERIC,
};

static int g_method_tags[16]; /* unique addresses used as method IDs */

/* ---- Configurable package/OBB ---- */
static const char *g_package_name = "com.coffeestain.huntdown";
static int g_obb_version = 0;
static const char *g_package_version_name = "0.1.23";
static jint g_package_version_code = 200023;

static nx_jni_config g_nx_config;

typedef struct {
  uint64_t block_size;
  uint64_t available_blocks;
  uint64_t free_blocks;
  uint64_t total_blocks;
} hd_storage_stats;

static hd_storage_stats hd_query_storage(void) {
  hd_storage_stats result = {4096, 0, 0, 0};
  struct statvfs info;
  if (statvfs(hd_game_dir(), &info) != 0) return result;
  result.block_size = info.f_frsize ? info.f_frsize
                                    : (info.f_bsize ? info.f_bsize : 4096);
  result.available_blocks = info.f_bavail;
  result.free_blocks = info.f_bfree;
  result.total_blocks = info.f_blocks;
  return result;
}

static jint hd_storage_int(uint64_t value) {
  return value > INT_MAX ? INT_MAX : (jint)value;
}

static void hd_display_size(int *w, int *h) {
  static int cached_w = -1, cached_h = -1;
  if (cached_w >= 0 && cached_h >= 0) { *w = cached_w; *h = cached_h; return; }
  int ew = 0, eh = 0;
  if (hd_display_size_detect(&ew, &eh, NULL)) {
    cached_w = ew; cached_h = eh; *w = ew; *h = eh; return;
  }
  cached_w = 0; cached_h = 0; *w = 0; *h = 0;
}

void jni_shim_set_package(const char *package_name, int obb_version) {
  g_package_name = package_name;
  g_obb_version = obb_version;
  g_nx_config.package_name = package_name;
  g_nx_config.obb_version = obb_version;
}

/* ---- Fake jstring tracking ---- */
/* We return tagged pointers as jstrings and map them to C strings */
#define MAX_JSTRINGS 16384
static struct {
  void *handle;
  char *value; /* copia propria (strdup) */
} g_jstrings[MAX_JSTRINGS];
static int g_jstring_count = 0;

/* jstring = o proprio ponteiro strdup (PERSISTENTE, unico, nunca liberado). O ring-buffer
   antigo (free + reuse de 1024 slots) LIBERAVA strings ainda em uso (ex: o path do PlayerPrefs
   guardado pelo Unity) -> apos >1024 jstrings, Unity usava ponteiro liberado -> crash em
   strchrnul/vsnprintf("%s_tmp", path_liberado). Identidade resolve isso (vaza, mas sessao limitada). */
static void *make_jstring(const char *value) {
  char *copy = strdup(value ? value : "");
  if (!copy) return NULL;
  int i = __atomic_fetch_add(&g_jstring_count, 1, __ATOMIC_RELAXED);
  if (i < MAX_JSTRINGS) {
    g_jstrings[i].handle = copy;
    g_jstrings[i].value = copy;
  }
  return copy;
}

void *jni_shim_string(const char *value) { return make_jstring(value); }

void jni_shim_set_version(const char *version_name, int version_code) {
  if (version_name && *version_name) g_package_version_name = version_name;
  if (version_code > 0) g_package_version_code = (jint)version_code;
}
static void *make_game_path_jstring(const char *relative) {
  char path[PATH_MAX];
  if (hd_game_path(path, sizeof path, relative) != 0)
    return make_jstring("");
  return make_jstring(path);
}
static const char *resolve_jstring(void *jstr) {
  return jstr ? (const char *)jstr : "";
}
static int is_jstring(void *obj) {
  int n = __atomic_load_n(&g_jstring_count, __ATOMIC_RELAXED);
  if (n > MAX_JSTRINGS) n = MAX_JSTRINGS;
  for (int i = 0; i < n; i++)
    if (g_jstrings[i].handle == obj) return 1;
  return 0;
}

/* android.net.Uri.encode/decode are part of Unity's Android PlayerPrefs path:
 * every key and string value passes through them before SharedPreferences.
 * Returning an unrelated non-null sentinel collapses every value to "" when
 * Unity asks for its UTF-8 bytes.  Keep Android's reversible percent encoding
 * so the original PlayerPrefs sequence and on-disk semantics remain intact. */
static int uri_unescaped_byte(unsigned char byte) {
  return (byte >= 'a' && byte <= 'z') ||
         (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9') ||
         byte == '_' || byte == '-' || byte == '.' || byte == '!' ||
         byte == '~' || byte == '\'' || byte == '(' || byte == ')' ||
         byte == '*';
}

static void *uri_encode_jstring(void *input) {
  static const char hex[] = "0123456789ABCDEF";
  const unsigned char *source =
      (const unsigned char *)resolve_jstring(input);
  size_t length = strlen((const char *)source);
  if (length > (SIZE_MAX - 1) / 3) return NULL;
  char *encoded = (char *)malloc(length * 3 + 1);
  if (!encoded) return NULL;
  char *output = encoded;
  for (size_t i = 0; i < length; ++i) {
    unsigned char byte = source[i];
    if (uri_unescaped_byte(byte)) {
      *output++ = (char)byte;
    } else {
      *output++ = '%';
      *output++ = hex[byte >> 4];
      *output++ = hex[byte & 15];
    }
  }
  *output = 0;
  void *result = make_jstring(encoded);
  free(encoded);
  return result;
}

static int uri_hex_value(unsigned char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

static void *uri_decode_jstring(void *input) {
  const unsigned char *source =
      (const unsigned char *)resolve_jstring(input);
  size_t length = strlen((const char *)source);
  char *decoded = (char *)malloc(length + 1);
  if (!decoded) return NULL;
  char *output = decoded;
  for (size_t i = 0; i < length; ++i) {
    if (source[i] == '%' && i + 2 < length) {
      int high = uri_hex_value(source[i + 1]);
      int low = uri_hex_value(source[i + 2]);
      if (high >= 0 && low >= 0) {
        *output++ = (char)((high << 4) | low);
        i += 2;
        continue;
      }
    }
    *output++ = (char)source[i];
  }
  *output = 0;
  void *result = make_jstring(decoded);
  free(decoded);
  return result;
}
/* jnibridge proxy: dados no topo (usados cedo), funções definidas abaixo (precisam
   de jni_find_native). Ver bloco "EXECUTA Runnables postados". */
#define PROXY_IFACE_FRAME_CALLBACK  (1u << 0)
#define PROXY_IFACE_RESULT_CALLBACK (1u << 1)
#define PROXY_IFACE_RUNNABLE        (1u << 2)
#define PROXY_BRIDGE_REFLECTION     (1u << 3)
static struct { void *obj; long handle; unsigned interfaces; } g_proxies[512];
static int g_proxy_n;
static int g_run_method_sentinel;   /* Method fake p/ Runnable.run() */
static int g_empty_args_sentinel;   /* Object[] vazio */
static int g_runnable_class_sentinel;
/* Choreographer frame-pacing do Unity 2022: o engine cria um
 * Choreographer$FrameCallback e um HandlerThread/Looper. A ponte enfileira a
 * Message; a thread equivalente ao Looper (main.c) processa handleMessage e só
 * depois entrega doFrame. */
static int g_doframe_method_sentinel;   /* Method fake p/ FrameCallback.doFrame(long) */
static int g_doframe_args_sentinel;     /* Object[1] = { Long(frameTimeNanos) } */
static int g_long_box_sentinel;         /* o Long boxed */
/* Handler$Callback.handleMessage(Message): a main posta uma Message via Handler.obtainMessage+
 * sendToTarget e ESPERA (cond nativo em libunity+0x2f3680) o Looper processá-la (handleMessage
 * → postFrameCallback). Looper fake nunca processa → deadlock. Dirigimos handleMessage no
 * sendToTarget. */
static int g_handlemsg_method_sentinel; /* Method fake p/ Handler$Callback.handleMessage(Message)Z */
static int g_handlemsg_args_sentinel;   /* Object[1] = { Message } */
static int g_message_sentinel;          /* a Message (obtainMessage->sendToTarget) */
static int g_handlerthread_sentinel;    /* android.os.HandlerThread */
static int g_handler_sentinel;          /* android.os.Handler */
static int g_looper_sentinel;           /* android.os.Looper */
static volatile int g_message_what;     /* msg.what passado ao obtainMessage */
static volatile int g_message_pending;  /* sendToTarget -> thread do Looper */
/* PendingResult.setResultCallback do Google Play Games. O DEX não roda no
 * NextOS, então o shim entrega pelo próprio AndroidJavaProxy um resultado
 * assíncrono equivalente a Google Play Services indisponível. */
static int g_onresult_method_sentinel;
static int g_google_result_args_sentinel;
static int g_google_token_result_sentinel;
static int g_google_pending_result_sentinel;
static void *g_onresult_mid;
static void *volatile g_google_result_proxy;
static volatile int g_google_result_pending;
void jni_handlemessage(void *env);
void jni_queue_handler_message(void);
int jni_take_handler_message(void);
static _Thread_local int g_next_proxy_is_framecb;  /* setado por FindClass(Choreographer$FrameCallback) */
static _Thread_local int g_next_proxy_is_resultcb; /* setado por FindClass(ResultCallback) */
static void *volatile g_framecb_proxy;  /* proxy do FrameCallback capturado */
static volatile long g_doframe_nanos;   /* frameTimeNanos atual (a driver-thread atualiza) */
static void proxy_register(void *obj, long h, void *interfaces);
static void proxy_register_reflection(void *obj, long h, void *interface_class);
static long proxy_handle(void *obj);
static int proxy_has_interface(void *obj, unsigned interface_flag);
static void run_runnable(void *env, void *runnable);
int jni_is_run_method(void *o);
int jni_is_empty_args(void *o);

/* java.lang.reflect.Method.toString() is part of JNIBridge's lookup key.  A
 * bare non-null sentinel is not enough: the native bridge asks the reflected
 * Method for this exact Java representation and throws NoSuchMethodError when
 * it is empty.  Keep the wrappers stable while reproducing Android's public
 * reflection contract for the callback methods we synthesize. */
static const char *reflected_method_string(void *method) {
  if (method == (void *)&g_run_method_sentinel)
    return "public abstract void java.lang.Runnable.run()";
  if (method == (void *)&g_doframe_method_sentinel)
    return "public abstract void android.view.Choreographer$FrameCallback.doFrame(long)";
  if (method == (void *)&g_handlemsg_method_sentinel)
    return "public abstract boolean android.os.Handler$Callback.handleMessage(android.os.Message)";
  if (method == (void *)&g_onresult_method_sentinel)
    return "public abstract void com.google.android.gms.common.api.ResultCallback.onResult(com.google.android.gms.common.api.Result)";
  return NULL;
}

/* ---- SharedPreferences persistente ----
 * PlayerPrefs no Android usa SharedPreferences. A tabela anterior existia apenas
 * em RAM, portanto opções/progresso escritos por esse caminho sumiam ao sair.
 * Mantemos os tipos Android e gravamos atomicamente dentro de userdata/. */
#define MAX_PREFS 4096
struct pref_entry {
  char *key;
  char *sval;
  int ival;
  float fval;
  int64_t lval;
  int bval;
  unsigned has_s : 1;
  unsigned has_i : 1;
  unsigned has_f : 1;
  unsigned has_l : 1;
  unsigned has_b : 1;
};
static struct pref_entry g_prefs[MAX_PREFS];
static int g_prefs_n = 0;
static pthread_mutex_t g_prefs_mutex = PTHREAD_MUTEX_INITIALIZER;

static void prefs_reset_value_unlocked(struct pref_entry *entry) {
  if (!entry) return;
  free(entry->sval);
  entry->sval = NULL;
  entry->has_s = entry->has_i = entry->has_f = 0;
  entry->has_l = entry->has_b = 0;
}

static int prefs_find_unlocked(const char *key) {
  for (int i = 0; i < g_prefs_n; i++)
    if (g_prefs[i].key && key && !strcmp(g_prefs[i].key, key)) return i;
  return -1;
}
static int prefs_slot_unlocked(const char *key) {
  int i = prefs_find_unlocked(key);
  if (i >= 0) return i;
  if (g_prefs_n >= MAX_PREFS) return -1;
  g_prefs[g_prefs_n].key = strdup(key ? key : "");
  return g_prefs_n++;
}
static void prefs_put_string(const char *key, const char *val) {
  pthread_mutex_lock(&g_prefs_mutex);
  int i = prefs_slot_unlocked(key);
  if (i < 0) { pthread_mutex_unlock(&g_prefs_mutex); return; }
  prefs_reset_value_unlocked(&g_prefs[i]);
  g_prefs[i].sval = strdup(val ? val : "");
  g_prefs[i].has_s = g_prefs[i].sval != NULL;
  pthread_mutex_unlock(&g_prefs_mutex);
}
static void prefs_put_int(const char *key, int val) {
  pthread_mutex_lock(&g_prefs_mutex);
  int i = prefs_slot_unlocked(key);
  if (i < 0) { pthread_mutex_unlock(&g_prefs_mutex); return; }
  prefs_reset_value_unlocked(&g_prefs[i]);
  g_prefs[i].ival = val; g_prefs[i].has_i = 1;
  pthread_mutex_unlock(&g_prefs_mutex);
}
static void prefs_put_float(const char *key, float val) {
  pthread_mutex_lock(&g_prefs_mutex);
  int i = prefs_slot_unlocked(key);
  if (i >= 0) {
    prefs_reset_value_unlocked(&g_prefs[i]);
    g_prefs[i].fval = val;
    g_prefs[i].has_f = 1;
  }
  pthread_mutex_unlock(&g_prefs_mutex);
}
static void prefs_put_long(const char *key, int64_t val) {
  pthread_mutex_lock(&g_prefs_mutex);
  int i = prefs_slot_unlocked(key);
  if (i >= 0) {
    prefs_reset_value_unlocked(&g_prefs[i]);
    g_prefs[i].lval = val;
    g_prefs[i].has_l = 1;
  }
  pthread_mutex_unlock(&g_prefs_mutex);
}
static void prefs_put_bool(const char *key, int val) {
  pthread_mutex_lock(&g_prefs_mutex);
  int i = prefs_slot_unlocked(key);
  if (i >= 0) {
    prefs_reset_value_unlocked(&g_prefs[i]);
    g_prefs[i].bval = val != 0;
    g_prefs[i].has_b = 1;
  }
  pthread_mutex_unlock(&g_prefs_mutex);
}
static char *prefs_dup_string(const char *key) {
  pthread_mutex_lock(&g_prefs_mutex);
  int i = prefs_find_unlocked(key);
  char *value =
      (i >= 0 && g_prefs[i].has_s) ? strdup(g_prefs[i].sval) : NULL;
  pthread_mutex_unlock(&g_prefs_mutex);
  return value;
}
static int prefs_get_int(const char *key, int fallback) {
  pthread_mutex_lock(&g_prefs_mutex);
  int i = prefs_find_unlocked(key);
  int value = (i >= 0 && g_prefs[i].has_i) ? g_prefs[i].ival : fallback;
  pthread_mutex_unlock(&g_prefs_mutex);
  return value;
}
static float prefs_get_float(const char *key, float fallback) {
  pthread_mutex_lock(&g_prefs_mutex);
  int i = prefs_find_unlocked(key);
  float value = (i >= 0 && g_prefs[i].has_f) ? g_prefs[i].fval : fallback;
  pthread_mutex_unlock(&g_prefs_mutex);
  return value;
}
static int64_t prefs_get_long(const char *key, int64_t fallback) {
  pthread_mutex_lock(&g_prefs_mutex);
  int i = prefs_find_unlocked(key);
  int64_t value = (i >= 0 && g_prefs[i].has_l) ? g_prefs[i].lval : fallback;
  pthread_mutex_unlock(&g_prefs_mutex);
  return value;
}
static int prefs_get_bool(const char *key, int fallback) {
  pthread_mutex_lock(&g_prefs_mutex);
  int i = prefs_find_unlocked(key);
  int value = (i >= 0 && g_prefs[i].has_b) ? g_prefs[i].bval : fallback;
  pthread_mutex_unlock(&g_prefs_mutex);
  return value;
}
static int prefs_contains(const char *key) {
  pthread_mutex_lock(&g_prefs_mutex);
  int i = prefs_find_unlocked(key);
  int result =
      i >= 0 && (g_prefs[i].has_s || g_prefs[i].has_i ||
                 g_prefs[i].has_f || g_prefs[i].has_l || g_prefs[i].has_b);
  pthread_mutex_unlock(&g_prefs_mutex);
  return result;
}
static void prefs_remove(const char *key) {
  pthread_mutex_lock(&g_prefs_mutex);
  int i = prefs_find_unlocked(key);
  if (i >= 0) prefs_reset_value_unlocked(&g_prefs[i]);
  pthread_mutex_unlock(&g_prefs_mutex);
}
static void prefs_clear(void) {
  pthread_mutex_lock(&g_prefs_mutex);
  for (int i = 0; i < g_prefs_n; i++) {
    free(g_prefs[i].key);
    free(g_prefs[i].sval);
    memset(&g_prefs[i], 0, sizeof g_prefs[i]);
  }
  g_prefs_n = 0;
  pthread_mutex_unlock(&g_prefs_mutex);
}

static int prefs_write_field(FILE *file, const void *data, size_t size) {
  return fwrite(data, 1, size, file) == size;
}
static int prefs_read_field(FILE *file, void *data, size_t size) {
  return fread(data, 1, size, file) == size;
}
static int prefs_paths(char *userdata, size_t userdata_size,
                       char *path, size_t path_size,
                       char *tmp, size_t tmp_size) {
  return hd_game_path(userdata, userdata_size, "userdata") == 0 &&
         hd_game_path(path, path_size, "userdata/shared_prefs.bin") == 0 &&
         hd_game_path(tmp, tmp_size, "userdata/.shared_prefs.tmp") == 0;
}
static int prefs_save_locked(void) {
  char userdata[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX];
  if (!prefs_paths(userdata, sizeof userdata, path, sizeof path,
                   tmp, sizeof tmp))
    return 0;
  mkdir(userdata, 0755);
  FILE *file = fopen(tmp, "wb");
  if (!file) return 0;
  static const unsigned char magic[8] = {'H','C','P','R','E','F','2',0};
  uint32_t count = 0;
  for (int i = 0; i < g_prefs_n; i++)
    if (g_prefs[i].key) count++;
  int ok = prefs_write_field(file, magic, sizeof magic) &&
           prefs_write_field(file, &count, sizeof count);
  for (int i = 0; ok && i < g_prefs_n; i++) {
    struct pref_entry *entry = &g_prefs[i];
    if (!entry->key) continue;
    uint32_t key_len = (uint32_t)strlen(entry->key);
    uint32_t string_len =
        entry->has_s && entry->sval ? (uint32_t)strlen(entry->sval) : 0;
    uint8_t flags = (entry->has_s ? 1 : 0) | (entry->has_i ? 2 : 0) |
                    (entry->has_f ? 4 : 0) | (entry->has_l ? 8 : 0) |
                    (entry->has_b ? 16 : 0);
    ok = prefs_write_field(file, &key_len, sizeof key_len) &&
         prefs_write_field(file, &string_len, sizeof string_len) &&
         prefs_write_field(file, &flags, sizeof flags) &&
         prefs_write_field(file, &entry->ival, sizeof entry->ival) &&
         prefs_write_field(file, &entry->fval, sizeof entry->fval) &&
         prefs_write_field(file, &entry->lval, sizeof entry->lval) &&
         prefs_write_field(file, &entry->bval, sizeof entry->bval) &&
         prefs_write_field(file, entry->key, key_len) &&
         (!string_len ||
          prefs_write_field(file, entry->sval, string_len));
  }
  if (ok && fflush(file) == 0) ok = fsync(fileno(file)) == 0;
  if (fclose(file) != 0) ok = 0;
  if (ok) ok = rename(tmp, path) == 0;
  if (!ok) unlink(tmp);
  return ok;
}
static int prefs_save(void) {
  pthread_mutex_lock(&g_prefs_mutex);
  int ok = prefs_save_locked();
  pthread_mutex_unlock(&g_prefs_mutex);
  return ok;
}
void jni_prefs_flush(void) {
  if (!prefs_save()) {
    char path[PATH_MAX];
    if (hd_game_path(path, sizeof path, "userdata/shared_prefs.bin") != 0)
      snprintf(path, sizeof path, "%s", "userdata/shared_prefs.bin");
    debugPrintf("[PREFS] falha ao persistir %s\n", path);
  }
}
static void prefs_load(void) {
  char path[PATH_MAX];
  if (hd_game_path(path, sizeof path, "userdata/shared_prefs.bin") != 0)
    return;
  FILE *file = fopen(path, "rb");
  if (!file) return;
  static const unsigned char expected[8] = {'H','C','P','R','E','F','2',0};
  unsigned char magic[8];
  uint32_t count = 0;
  int ok = prefs_read_field(file, magic, sizeof magic) &&
           memcmp(magic, expected, sizeof magic) == 0 &&
           prefs_read_field(file, &count, sizeof count) &&
           count <= MAX_PREFS;
  pthread_mutex_lock(&g_prefs_mutex);
  for (uint32_t n = 0; ok && n < count; n++) {
    uint32_t key_len = 0, string_len = 0;
    uint8_t flags = 0;
    int ival = 0, bval = 0;
    float fval = 0.0f;
    int64_t lval = 0;
    ok = prefs_read_field(file, &key_len, sizeof key_len) &&
         prefs_read_field(file, &string_len, sizeof string_len) &&
         prefs_read_field(file, &flags, sizeof flags) &&
         prefs_read_field(file, &ival, sizeof ival) &&
         prefs_read_field(file, &fval, sizeof fval) &&
         prefs_read_field(file, &lval, sizeof lval) &&
         prefs_read_field(file, &bval, sizeof bval) &&
         key_len > 0 && key_len < 65536 && string_len < 16 * 1024 * 1024;
    char *key = ok ? malloc((size_t)key_len + 1) : NULL;
    char *string = ok && string_len ? malloc((size_t)string_len + 1) : NULL;
    if (!key || (string_len && !string)) ok = 0;
    if (ok) ok = prefs_read_field(file, key, key_len);
    if (ok && string_len) ok = prefs_read_field(file, string, string_len);
    if (!ok) { free(key); free(string); break; }
    key[key_len] = '\0';
    if (string) string[string_len] = '\0';
    int i = prefs_slot_unlocked(key);
    free(key);
    if (i < 0) { free(string); ok = 0; break; }
    g_prefs[i].sval = string;
    g_prefs[i].ival = ival;
    g_prefs[i].fval = fval;
    g_prefs[i].lval = lval;
    g_prefs[i].bval = bval;
    g_prefs[i].has_s = (flags & 1) != 0;
    g_prefs[i].has_i = (flags & 2) != 0;
    g_prefs[i].has_f = (flags & 4) != 0;
    g_prefs[i].has_l = (flags & 8) != 0;
    g_prefs[i].has_b = (flags & 16) != 0;
  }
  int loaded = ok ? g_prefs_n : 0;
  pthread_mutex_unlock(&g_prefs_mutex);
  fclose(file);
  if (ok) debugPrintf("[PREFS] %d entradas persistentes carregadas\n", loaded);
}

/* ---- Registry persistente de method/field IDs ----
 * name/sig recebidos do Unity podem apontar para buffers temporários. Guardar
 * esses ponteiros produzia IDs com texto corrompido durante o Play Games.
 * Cada ID agora possui cópias imutáveis e nunca é reciclado durante a sessão. */
#define MAX_MIDREG 4096
static nx_jval nx_package_name(nx_ctx *ctx) {
  (void)ctx;
  return nx_ptr(make_jstring(g_package_name));
}
static void *hd_installer_name(void) {
  const char *installer = getenv("HD_INSTALLER_PACKAGE");
  return installer && *installer ? make_jstring(installer) : NULL;
}
static nx_jval nx_installer_name(nx_ctx *ctx) {
  (void)ctx;
  return nx_ptr(hd_installer_name());
}
static nx_jval nx_version_name(nx_ctx *ctx) {
  (void)ctx;
  return nx_ptr(make_jstring(g_package_version_name));
}
static nx_jval nx_version_code(nx_ctx *ctx) {
  (void)ctx;
  return nx_int(g_package_version_code);
}
static nx_jval nx_service_offline(nx_ctx *ctx) {
  (void)ctx;
  return nx_bool(0);
}
static nx_jval nx_sdk_int(nx_ctx *ctx) {
  (void)ctx;
  return nx_int(30);
}

static const nx_method g_nx_methods[] = {
  { "getPackageName", "()Ljava/lang/String;", NX_OBJ, nx_package_name },
  { "getInstallerPackageName", "(Ljava/lang/String;)Ljava/lang/String;", NX_OBJ, nx_installer_name },
  { "getVersionName", "()Ljava/lang/String;", NX_OBJ, nx_version_name },
  { "getVersionCode", "()I", NX_INT, nx_version_code },
  { "isSignedIn", "()Z", NX_BOOL, nx_service_offline },
  { "isAuthenticated", "()Z", NX_BOOL, nx_service_offline },
  { "isConnected", "()Z", NX_BOOL, nx_service_offline },
  NX_METHOD_END
};
static const nx_field g_nx_fields[] = {
  { "SDK_INT", "I", NX_INT, nx_sdk_int },
  { "versionCode", "I", NX_INT, nx_version_code },
  NX_FIELD_END
};
static nx_jni_config g_nx_config = {
  "com.coffeestain.huntdown", 0, g_nx_methods, g_nx_fields
};

struct mid_entry {
  char *name;
  char *sig;
  void *nx_id;
  unsigned is_field : 1;
};
static struct mid_entry g_midreg[MAX_MIDREG];
static int g_midreg_count = 0;
static pthread_mutex_t g_midreg_mutex = PTHREAD_MUTEX_INITIALIZER;

static int same_nullable_text(const char *a, const char *b) {
  if (a == b) return 1;
  if (!a || !b) return 0;
  return strcmp(a, b) == 0;
}

static void *reg_id(const char *name, const char *sig, int is_field) {
  pthread_mutex_lock(&g_midreg_mutex);
  for (int i = 0; i < g_midreg_count; i++) {
    if (same_nullable_text(g_midreg[i].name, name) &&
        same_nullable_text(g_midreg[i].sig, sig) &&
        g_midreg[i].is_field == (unsigned)!!is_field) {
      pthread_mutex_unlock(&g_midreg_mutex);
      return &g_midreg[i];
    }
  }
  if (g_midreg_count >= MAX_MIDREG) {
    pthread_mutex_unlock(&g_midreg_mutex);
    debugPrintf("jni_shim: registry de method IDs cheio (%d)\n", MAX_MIDREG);
    return NULL;
  }
  int i = g_midreg_count++;
  g_midreg[i].name = strdup(name ? name : "");
  g_midreg[i].sig = strdup(sig ? sig : "");
  g_midreg[i].is_field = is_field ? 1u : 0u;
  g_midreg[i].nx_id = is_field
                           ? nx_field_id(&g_nx_config, name, sig)
                           : nx_method_id(&g_nx_config, name, sig);
  pthread_mutex_unlock(&g_midreg_mutex);
  return &g_midreg[i];
}
static void *reg_mid(const char *name, const char *sig) {
  return reg_id(name, sig, 0);
}
static void *reg_fid(const char *name, const char *sig) {
  return reg_id(name, sig, 1);
}
static const char *mid_name(void *tag) {
  if ((char *)tag >= (char *)g_midreg &&
      (char *)tag < (char *)(g_midreg + MAX_MIDREG))
    return ((struct mid_entry *)tag)->name;
  return NULL;
}
static const char *mid_sig(void *tag) {
  if ((char *)tag >= (char *)g_midreg &&
      (char *)tag < (char *)(g_midreg + MAX_MIDREG))
    return ((struct mid_entry *)tag)->sig;
  return NULL;
}
static void *mid_nx_id(void *tag, int is_field) {
  if ((char *)tag >= (char *)g_midreg &&
      (char *)tag < (char *)(g_midreg + MAX_MIDREG)) {
    struct mid_entry *entry = (struct mid_entry *)tag;
    if (entry->is_field == (unsigned)!!is_field) return entry->nx_id;
  }
  return NULL;
}
static nx_jval nx_call_v(void *env, void *obj, void *method_id, va_list ap) {
  void *nx_id = mid_nx_id(method_id, 0);
  if (!nx_id) return nx_none();
  va_list copy;
  va_copy(copy, ap);
  nx_jval result = nx_dispatch(&g_nx_config, env, obj, nx_id, &copy, NULL);
  va_end(copy);
  return result;
}
static nx_jval nx_call_a(void *env, void *obj, void *method_id,
                         const jvalue *args) {
  void *nx_id = mid_nx_id(method_id, 0);
  return nx_id ? nx_dispatch(&g_nx_config, env, obj, nx_id, NULL, args)
               : nx_none();
}
static nx_jval nx_get_field(void *env, void *obj, void *field_id) {
  void *nx_id = mid_nx_id(field_id, 1);
  return nx_id ? nx_dispatch_field(&g_nx_config, env, obj, nx_id)
               : nx_none();
}

/* ===================================================================
 * AssetManager bridge — lê do diretório de jogo negociado pelo launcher
 * (Unity: getAssets() + AssetManager.open(path) + InputStream.read/close)
 * =================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
static int g_assetmgr;   /* tag do objeto AssetManager */
static int g_empty_list; /* tag de uma java.util.List vazia */
static int g_iterator;   /* tag de um Iterator vazio */
static int g_appinfo;    /* tag de ApplicationInfo */

/* --- primitive-array tracking (backing real) ---
 *
 * JNI local references remain valid until the native frame that owns them is
 * released.  The old implementation recycled 128 handles immediately:
 * barr_new() freed a still-live buffer as soon as the ring wrapped.  Unity
 * converts Java strings through String.getBytes(), while the FMOD and input
 * threads also allocate primitive arrays, so a byte[] could be replaced
 * between CallObjectMethod(getBytes), GetArrayLength and
 * GetByteArrayElements.
 *
 * Keep handles stable for the process lifetime, just like fake jstrings and
 * Object[].  The table itself is small (256 KiB); payload buffers are bounded
 * by the normal number of JNI local arrays created by this game.
 */
#define MAX_BARR 16384
struct barr { unsigned char *buf; int len; };
static struct barr g_barr[MAX_BARR];
static int g_barr_n = 0;
static void *barr_new(int len) {
  int i = __atomic_fetch_add(&g_barr_n, 1, __ATOMIC_RELAXED);
  if (i < 0 || i >= MAX_BARR) {
    debugPrintf("jni_shim: registry de arrays primitivos cheio (%d)\n",
                MAX_BARR);
    return NULL;
  }
  g_barr[i].buf = (unsigned char *)malloc(len > 0 ? len : 1);
  if (!g_barr[i].buf) return NULL;
  g_barr[i].len = len;
  return &g_barr[i];
}
static struct barr *barr_find(void *h) {
  if ((char *)h >= (char *)g_barr && (char *)h < (char *)(g_barr + MAX_BARR))
    return (struct barr *)h;
  return NULL;
}

/* Object[] real. AndroidReflection monta Class[] para AndroidJavaProxy e o
 * Google Play monta arrays de argumentos; antes SetObjectArrayElement era stub
 * e descartava todos os elementos. Os handles não são reciclados. */
#define MAX_OARR 512
struct oarr { void **items; int len; };
static struct oarr g_oarr[MAX_OARR];
static int g_oarr_n;

static void *oarr_new(int len, void *initial) {
  int i = __atomic_fetch_add(&g_oarr_n, 1, __ATOMIC_RELAXED);
  if (i < 0 || i >= MAX_OARR) {
    debugPrintf("jni_shim: registry de Object[] cheio (%d)\n", MAX_OARR);
    return NULL;
  }
  int alloc_len = len > 0 ? len : 1;
  g_oarr[i].items = (void **)calloc((size_t)alloc_len, sizeof(void *));
  g_oarr[i].len = len > 0 ? len : 0;
  if (!g_oarr[i].items) return NULL;
  for (int j = 0; j < g_oarr[i].len; j++) g_oarr[i].items[j] = initial;
  return &g_oarr[i];
}

static struct oarr *oarr_find(void *h) {
  if ((char *)h >= (char *)g_oarr && (char *)h < (char *)(g_oarr + MAX_OARR)) {
    struct oarr *a = (struct oarr *)h;
    return a->items ? a : NULL;
  }
  return NULL;
}

/* Android's DisplayManager exposes at least the current physical mode. A
 * non-Android host has no Java Display.Mode objects, but Unity 2022 still
 * enumerates them before it starts Swappy. Returning a generic object made
 * GetArrayLength(getSupportedModes()) report zero; Swappy consequently kept
 * its refresh period invalid and nativeRender waited for INT_MAX
 * Choreographer ticks on its first frame.
 *
 * Keep one stable logical Display and one stable 60 Hz mode. This mirrors the
 * real single-mode fbdev panel without touching Unity's private state. */
static int g_display_sentinel;
static int g_display_mode_sentinel;
static int g_swappy_display_manager_sentinel;
static void *g_display_modes_array;

static void *display_modes_array(void) {
  if (!g_display_modes_array)
    g_display_modes_array = oarr_new(1, &g_display_mode_sentinel);
  return g_display_modes_array;
}

/* int[] real (p/ InputDevice.getDeviceIds): len = nº de ELEMENTOS, buf = 4*len bytes */
static void *iarr_new(const int *vals, int n) {
  int i = __atomic_fetch_add(&g_barr_n, 1, __ATOMIC_RELAXED);
  if (i < 0 || i >= MAX_BARR) {
    debugPrintf("jni_shim: registry de arrays primitivos cheio (%d)\n",
                MAX_BARR);
    return NULL;
  }
  g_barr[i].buf = (unsigned char *)malloc(n > 0 ? n * 4 : 4);
  if (!g_barr[i].buf) return NULL;
  g_barr[i].len = n;
  if (vals && n > 0) memcpy(g_barr[i].buf, vals, n * 4);
  else if (n > 0) memset(g_barr[i].buf, 0, n * 4);
  return &g_barr[i];
}
/* long[] real. SwappyDisplayManager entrega os periodos de refresh em
 * nanossegundos e o callback nativo os le por GetLongArrayElements. */
static void *larr_new(const int64_t *vals, int n) {
  int i = __atomic_fetch_add(&g_barr_n, 1, __ATOMIC_RELAXED);
  if (i < 0 || i >= MAX_BARR) {
    debugPrintf("jni_shim: registry de arrays primitivos cheio (%d)\n",
                MAX_BARR);
    return NULL;
  }
  size_t bytes = n > 0 ? (size_t)n * sizeof(int64_t) : sizeof(int64_t);
  g_barr[i].buf = (unsigned char *)malloc(bytes);
  if (!g_barr[i].buf) return NULL;
  g_barr[i].len = n;
  if (vals && n > 0) memcpy(g_barr[i].buf, vals, (size_t)n * sizeof(int64_t));
  else memset(g_barr[i].buf, 0, bytes);
  return &g_barr[i];
}
static void *boolarr_new(int n) {
  int i = __atomic_fetch_add(&g_barr_n, 1, __ATOMIC_RELAXED);
  if (i < 0 || i >= MAX_BARR) {
    debugPrintf("jni_shim: registry de arrays primitivos cheio (%d)\n",
                MAX_BARR);
    return NULL;
  }
  g_barr[i].buf = (unsigned char *)calloc(n > 0 ? n : 1, 1);
  if (!g_barr[i].buf) return NULL;
  g_barr[i].len = n;
  return &g_barr[i];
}

/* ---- Unity soft keyboard bridge ---- */
static int g_softinput_class;         /* classe fake p/ callbacks nativos do soft keyboard */
static int g_softinput_active, g_softinput_manual, g_softinput_limit = 32, g_softinput_suppress_empty;
static char g_softinput_text[128];
static char g_softinput_last_confirmed[128];

static void softinput_copy(char *dst, size_t cap, const char *src, int limit) {
  if (!dst || cap == 0) return;
  if (!src) src = "";
  if (limit <= 0 || limit >= (int)cap) limit = (int)cap - 1;
  size_t n = strlen(src);
  if (n > (size_t)limit) n = (size_t)limit;
  memcpy(dst, src, n);
  dst[n] = 0;
}
static void *softinput_env(void) { return &jni_env_ptr; }
static void softinput_native_visible(int visible) {
  void *fn = jni_find_native("nativeSetKeyboardIsVisible");
  if (fn) ((void (*)(void *, void *, jboolean))fn)(softinput_env(), &g_softinput_class, visible ? 1 : 0);
}
static void softinput_native_text(const char *text) {
  void *fn = jni_find_native("nativeSetInputString");
  if (fn) ((void (*)(void *, void *, void *))fn)(softinput_env(), &g_softinput_class, make_jstring(text ? text : ""));
}
static void softinput_native_selection(int start, int end) {
  void *fn = jni_find_native("nativeSetInputSelection");
  if (fn) ((void (*)(void *, void *, jint, jint))fn)(softinput_env(), &g_softinput_class, start, end);
}
static void softinput_native_closed(void) {
  void *fn = jni_find_native("nativeSoftInputClosed");
  if (fn) ((void (*)(void *, void *))fn)(softinput_env(), &g_softinput_class);
}
static void softinput_native_canceled(void) {
  void *fn = jni_find_native("nativeSoftInputCanceled");
  if (fn) ((void (*)(void *, void *))fn)(softinput_env(), &g_softinput_class);
  else softinput_native_closed();
}
static void softinput_apply_text(void) {
  int len = (int)strlen(g_softinput_text);
  softinput_native_text(g_softinput_text);
  softinput_native_selection(len, len);
}
static void softinput_show(void *env, void *text_j, void *placeholder_j, int limit) {
  (void)env;
  const char *text = resolve_jstring(text_j);
  const char *placeholder = resolve_jstring(placeholder_j);
  if (limit <= 0 || limit > 120) limit = 32;
  g_softinput_limit = limit;
  if (getenv("HD_NOVKBD") && !getenv("HD_OSK")) {
    debugPrintf("[SOFTINPUT] show ignorado por HD_NOVKBD text=\"%s\"\n", text ? text : "");
    return;
  }
  g_softinput_manual = getenv("HD_OSK") ? 1 : 0;
  if ((!text || !text[0]) && g_softinput_suppress_empty > 0 && g_softinput_last_confirmed[0]) {
    g_softinput_suppress_empty--;
    softinput_copy(g_softinput_text, sizeof g_softinput_text, g_softinput_last_confirmed, g_softinput_limit);
    softinput_apply_text();
    softinput_native_visible(0);
    softinput_native_closed();
    softinput_apply_text();
    g_softinput_active = 0;
    g_softinput_manual = 0;
    debugPrintf("[SOFTINPUT] suppress empty reopen -> keep \"%s\" (%d left)\n",
                g_softinput_text, g_softinput_suppress_empty);
    return;
  }
  softinput_copy(g_softinput_text, sizeof g_softinput_text, text, g_softinput_limit);
  g_softinput_active = 1;
  softinput_native_visible(1);
  softinput_apply_text();
  debugPrintf("[SOFTINPUT] show text=\"%s\" placeholder=\"%s\" limit=%d\n",
              g_softinput_text, placeholder ? placeholder : "", g_softinput_limit);
}
int jni_softinput_active(void) {
  return g_softinput_active && (!getenv("HD_NOVKBD") || g_softinput_manual || getenv("HD_OSK"));
}
const char *jni_softinput_text(void) { return g_softinput_text; }
int jni_softinput_limit(void) { return g_softinput_limit > 0 ? g_softinput_limit : 32; }
void jni_softinput_open(const char *text, int limit) {
  if (limit <= 0 || limit > 120) limit = 32;
  g_softinput_manual = 1;
  g_softinput_limit = limit;
  softinput_copy(g_softinput_text, sizeof g_softinput_text, text, g_softinput_limit);
  g_softinput_active = 1;
  softinput_native_visible(1);
  softinput_apply_text();
  debugPrintf("[VKBD] open virtual text=\"%s\" limit=%d\n", g_softinput_text, g_softinput_limit);
}
void jni_softinput_set_text(const char *text) {
  if (!jni_softinput_active()) return;
  softinput_copy(g_softinput_text, sizeof g_softinput_text, text, jni_softinput_limit());
  softinput_apply_text();
}
void jni_softinput_commit(const char *text) {
  softinput_copy(g_softinput_text, sizeof g_softinput_text, text, jni_softinput_limit());
  if (!g_softinput_text[0]) {
    const char *def = getenv("HD_VK_DEFAULT") ? getenv("HD_VK_DEFAULT") : "PLAYER";
    softinput_copy(g_softinput_text, sizeof g_softinput_text, def, jni_softinput_limit());
  }
  softinput_copy(g_softinput_last_confirmed, sizeof g_softinput_last_confirmed,
                 g_softinput_text, sizeof g_softinput_last_confirmed - 1);
  softinput_apply_text();
  softinput_native_visible(0);
  softinput_native_closed();
  softinput_apply_text();
  g_softinput_active = 0;
  g_softinput_manual = 0;
  /* modo OSK (teclado real): reabertura do campo deve SEMPRE abrir o teclado.
     A supressao de reopen-vazio e so do modo autoname/sem-teclado. */
  g_softinput_suppress_empty = getenv("HD_OSK") ? 0 : 5;
  debugPrintf("[VKBD] OK text=\"%s\"\n", g_softinput_text);
}
void jni_softinput_cancel(void) {
  softinput_native_visible(0);
  softinput_native_canceled();
  g_softinput_active = 0;
  g_softinput_manual = 0;
  g_softinput_suppress_empty = 0;
  debugPrintf("[VKBD] teclado cancelado\n");
}

/* --- InputStream (FILE*) tracking --- */
#define MAX_ASTREAMS 32
struct astream { FILE *fp; long size; };
static struct astream g_astreams[MAX_ASTREAMS];
static int g_astream_n = 0;
static void *asset_open(const char *path) {
  char full[PATH_MAX];
  /* alguns acessos vêm com prefixo "assets/" (ex il2cpp resource check do guid);
     nossos arquivos estão em <game>/bin/Data (sem "assets/"). Tira o prefixo. */
  const char *p = path ? path : "";
  if (!strncmp(p, "assets/", 7)) p += 7;
  if (hd_game_path(full, sizeof full, p) != 0) return NULL;
  FILE *fp = fopen(full, "rb");
  debugPrintf("asset: open(%s) -> %s\n", path ? path : "?",
              fp ? "OK" : "FALHOU (sem arquivo)");
  if (!fp) return NULL;
  int i = g_astream_n++ % MAX_ASTREAMS;
  if (g_astreams[i].fp) fclose(g_astreams[i].fp);
  g_astreams[i].fp = fp;
  fseek(fp, 0, SEEK_END);
  g_astreams[i].size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  return &g_astreams[i];
}
static struct astream *astream_find(void *h) {
  if ((char *)h >= (char *)g_astreams &&
      (char *)h < (char *)(g_astreams + MAX_ASTREAMS))
    return (struct astream *)h;
  return NULL;
}

/* --- JNI byte-array functions --- */
static void *jni_NewByteArray(void *env, int len) {
  (void)env;
  return barr_new(len);
}
static int jni_GetArrayLength_real(void *env, void *arr) {
  (void)env;
  struct barr *b = barr_find(arr);
  return b ? b->len : 0;
}
static void *jni_GetByteArrayElements(void *env, void *arr, void *isCopy) {
  (void)env;
  if (isCopy) *(unsigned char *)isCopy = 0;
  struct barr *b = barr_find(arr);
  return b ? b->buf : NULL;
}
static void jni_ReleaseByteArrayElements(void *env, void *arr, void *elems,
                                         int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void jni_GetByteArrayRegion(void *env, void *arr, int start, int len,
                                   void *buf) {
  (void)env;
  struct barr *b = barr_find(arr);
  if (b && start >= 0 && len >= 0 && start + len <= b->len)
    memcpy(buf, b->buf + start, len);
}
static void jni_SetByteArrayRegion(void *env, void *arr, int start, int len,
                                   const void *buf) {
  (void)env;
  struct barr *b = barr_find(arr);
  if (b && start >= 0 && len >= 0 && start + len <= b->len)
    memcpy(b->buf + start, buf, len);
}

/* ---- Generic stub ---- */
static intptr_t jni_stub(void) { return 0; }

/* ---- JNIEnv functions ---- */

static jint jni_GetVersion(void *env) {
  (void)env;
  return 0x00010006;
}

/* ===== Injeção de input p/ nativeInjectEvent (KeyEvent) =====
   nativeInjectEvent lê o evento via JNI (getAction/getKeyCode/...). Setamos
   g_hk_inject ANTES de chamar nativeInjectEvent e os métodos retornam daqui. */
struct hk_inject_s { int action, keycode, source, deviceId, metaState, repeat,
                     scancode, flags, unicode; long eventTime, downTime; };
struct hk_inject_s g_hk_inject;       /* exportado p/ main_recon */
static int g_obj_keyevent;            /* sentinela do objeto KeyEvent */
void *hk_keyevent_object(void) { return &g_obj_keyevent; }
static int g_gamepad_device;          /* sentinela do InputDevice (Xbox 360 virtual) */
static int g_current_activity;        /* UnityPlayer.currentActivity fake */
static int g_current_activity_field_id;

/* Classes também precisam de identidade e nome persistentes. */
#define MAX_CLASSREG 1024
static struct { char *name; int tag; } g_classreg[MAX_CLASSREG];
static int g_classreg_n = 0;
static pthread_mutex_t g_classreg_mutex = PTHREAD_MUTEX_INITIALIZER;
int g_fmod_device_obj;   /* sentinela do org.fmod.FMODAudioDevice (NewObject/métodos do FMOD) */
static volatile int g_fmod_should_run;
static void *class_for(const char *name) {
  if (!name) name = "?";
  pthread_mutex_lock(&g_classreg_mutex);
  for (int i = 0; i < g_classreg_n; i++) {
    if (g_classreg[i].name == name ||
        (g_classreg[i].name && strcmp(g_classreg[i].name, name) == 0)) {
      pthread_mutex_unlock(&g_classreg_mutex);
      return &g_classreg[i].tag;
    }
  }
  if (g_classreg_n >= MAX_CLASSREG) {
    pthread_mutex_unlock(&g_classreg_mutex);
    debugPrintf("jni_shim: registry de classes cheio (%d)\n", MAX_CLASSREG);
    return NULL;
  }
  int i = g_classreg_n++;
  g_classreg[i].name = strdup(name);
  pthread_mutex_unlock(&g_classreg_mutex);
  return &g_classreg[i].tag;
}
static const char *class_name_for(void *clazz) {
  const char *result = NULL;
  pthread_mutex_lock(&g_classreg_mutex);
  for (int i = 0; i < g_classreg_n; i++) {
    if (clazz == &g_classreg[i].tag) {
      result = g_classreg[i].name;
      break;
    }
  }
  pthread_mutex_unlock(&g_classreg_mutex);
  return result;
}

/* ClassLoader.loadClass() devolve um jclass, nao um Object generico. O player
 * usa essa rota (em vez de FindClass) para SwappyDisplayManager. */
static void *class_from_jstring(void *name_obj) {
  const char *name = resolve_jstring(name_obj);
  char normalized[512];
  size_t n = strlen(name);
  if (n >= sizeof normalized) n = sizeof normalized - 1;
  for (size_t i = 0; i < n; i++)
    normalized[i] = name[i] == '.' ? '/' : name[i];
  normalized[n] = '\0';
  debugPrintf("jni_shim: loadClass(%s)\n", normalized);
  return class_for(normalized);
}
static void *jni_FindClass(void *env, const char *name) {
  (void)env;
  debugPrintf("jni_shim: FindClass(%s)\n", name);
  /* o proxy criado logo após FindClass(Choreographer$FrameCallback) é o FrameCallback */
  g_next_proxy_is_framecb = (name && strstr(name, "Choreographer$FrameCallback")) ? 1 : 0;
  g_next_proxy_is_resultcb =
      (name && strcmp(name, "com/google/android/gms/common/api/ResultCallback") == 0) ? 1 : 0;
  return class_for(name);
}

static void *jni_GetMethodID(void *env, void *clazz, const char *name,
                             const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetMethodID(%s, %s)\n", name, sig);
  void *mid = reg_mid(name, sig);
  if (name && strcmp(name, "onResult") == 0) g_onresult_mid = mid;
  return mid;
}

static void *jni_GetStaticMethodID(void *env, void *clazz, const char *name,
                                   const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetStaticMethodID(%s, %s)\n", name, sig);
  return reg_mid(name, sig);
}

static void *jni_GetFieldID(void *env, void *clazz, const char *name,
                            const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetFieldID(%s, %s)\n", name, sig);
  return reg_fid(name, sig);   /* tabela declarativa + nome p/ DisplayMetrics */
}

static void *jni_FromReflectedField(void *env, void *field) {
  (void)env;
  return field;
}

/* O native jnibridge "invoke" da libunity chama FromReflectedMethod antes de
 * despachar AndroidJavaProxy. O ID refletido precisa ter a MESMA identidade do
 * jmethodID que o proxy armazenou via GetMethodID; devolver o sentinel em si
 * não satisfaz a comparação interna e transforma o callback em no-op. */
static void *jni_FromReflectedMethod(void *env, void *method) {
  (void)env;
  if (method == (void *)&g_handlemsg_method_sentinel)
    return reg_mid("handleMessage", "(Landroid/os/Message;)Z");
  if (method == (void *)&g_doframe_method_sentinel)
    return reg_mid("doFrame", "(J)V");
  if (method == (void *)&g_run_method_sentinel)
    return reg_mid("run", "()V");
  if (method == (void *)&g_onresult_method_sentinel)
    return g_onresult_mid ? g_onresult_mid
                          : reg_mid("onResult", "(Ljava/lang/Object;)V");
  return method;
}

/* Unity 6 converts the jmethodID used by AndroidJavaProxy back to a reflected
 * Method before invoking the bridge.  Preserve the stable sentinel identity
 * that FromReflectedMethod already understands; returning NULL here prevents
 * UnityChoreographer's proxy from ever being captured. */
static void *jni_ToReflectedMethod(void *env, void *clazz, void *method,
                                   unsigned char is_static) {
  (void)env;
  (void)clazz;
  (void)is_static;
  const char *name = mid_name(method);
  if (!name) return method;
  if (strcmp(name, "handleMessage") == 0)
    return &g_handlemsg_method_sentinel;
  if (strcmp(name, "doFrame") == 0)
    return &g_doframe_method_sentinel;
  if (strcmp(name, "run") == 0)
    return &g_run_method_sentinel;
  if (strcmp(name, "onResult") == 0)
    return &g_onresult_method_sentinel;
  return method;
}

static void *jni_GetObjectField(void *env, void *obj, void *fieldID) {
  (void)env; (void)obj;
  const char *nm = mid_name(fieldID);
  /* Install-time Play Asset Delivery packs are reported by Android in
   * ApplicationInfo.splitPublicSourceDirs.  Unity scans the split names for
   * "UnityDataAssetPack", opens that APK as a ZIP data archive and mounts
   * assets/bin/Data/datapack.unity3d from it.  The extracted directory returned
   * by getAssetPackPath is the fallback for fast-follow/on-demand packs; it
   * cannot replace this install-time split list. */
  if (nm && strcmp(nm, "splitPublicSourceDirs") == 0) {
    static void *split_dirs;
    if (!split_dirs) {
      split_dirs = oarr_new(1, NULL);
      struct oarr *dirs = oarr_find(split_dirs);
      if (dirs)
        dirs->items[0] =
            make_game_path_jstring("UnityDataAssetPack.apk");
    }
    struct oarr *dirs = oarr_find(split_dirs);
    debugPrintf("jni_shim: ApplicationInfo.splitPublicSourceDirs -> %s\n",
                dirs && dirs->items[0]
                    ? resolve_jstring(dirs->items[0]) : "");
    return split_dirs;
  }
  /* android.content.pm.PackageInfo do APK alvo. O player e o VersionVerifier
   * leem este campo; string vazia quebra o split semântico da versão. */
  if (nm && strcmp(nm, "versionName") == 0)
    return make_jstring(g_package_version_name);
  nx_jval declared = nx_get_field(env, obj, fieldID);
  if (declared.l) return declared.l;
  static int fake_obj_field;
  return &fake_obj_field;
}

/* GetIntField (idx 100): DisplayMetrics widthPixels/heightPixels/densityDpi. */
static jint jni_GetIntField(void *env, void *obj, void *fieldID) {
  (void)env; (void)obj;
  const char *nm = mid_name(fieldID);
  if (nm) {
    if (strcmp(nm, "versionCode") == 0) return g_package_version_code;
    if (obj == (void *)&g_message_sentinel && strcmp(nm, "what") == 0) return g_message_what;
    if (strcmp(nm, "widthPixels") == 0) { int w, h; hd_display_size(&w, &h); return w; }
    if (strcmp(nm, "heightPixels") == 0) { int w, h; hd_display_size(&w, &h); return h; }
    if (strcmp(nm, "densityDpi") == 0) return 160;
  }
  return nx_get_field(env, obj, fieldID).i;
}

/* GetFloatField (idx 102): DisplayMetrics density/xdpi/ydpi/scaledDensity.
   density/xdpi=0.0 -> divisão por zero / DPI inválido no engine -> loop de getMetrics. */
static float jni_GetFloatField(void *env, void *obj, void *fieldID) {
  (void)env; (void)obj;
  const char *nm = mid_name(fieldID);
  if (nm) {
    if (strcmp(nm, "density") == 0) return 1.0f;
    if (strcmp(nm, "scaledDensity") == 0) return 1.0f;
    if (strcmp(nm, "xdpi") == 0) return 160.0f;
    if (strcmp(nm, "ydpi") == 0) return 160.0f;
    if (strcmp(nm, "refreshRate") == 0) return 60.0f;
  }
  return nx_get_field(env, obj, fieldID).f;
}

/* CallFloatMethodV (idx 56): Display.getRefreshRate() -> 60Hz (0 quebra o engine) */
static float jni_CallFloatMethodV(void *env, void *obj, void *methodID, va_list ap) {
  (void)env; (void)obj;
  const char *nm = mid_name(methodID);
  if (nm && strcmp(nm, "getRefreshRate") == 0) return 60.0f;
  if (nm && strcmp(nm, "getFloat") == 0) {
    const char *key = resolve_jstring(va_arg(ap, void *));
    double fallback = va_arg(ap, double);
    return prefs_get_float(key, (float)fallback);
  }
  return nx_call_v(env, obj, methodID, ap).f;
}
static float jni_CallFloatMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  float r = jni_CallFloatMethodV(env, obj, methodID, ap);
  va_end(ap);
  return r;
}
static float jni_CallFloatMethodA(void *env, void *obj, void *methodID,
                                  const jvalue *args) {
  (void)env;
  (void)obj;
  const char *nm = mid_name(methodID);
  if (nm && strcmp(nm, "getRefreshRate") == 0) return 60.0f;
  if (nm && strcmp(nm, "getFloat") == 0)
    return prefs_get_float(args ? resolve_jstring(args[0].l) : "",
                           args ? args[1].f : 0.0f);
  return nx_call_a(env, obj, methodID, args).f;
}

static void *jni_GetStaticFieldID(void *env, void *clazz, const char *name,
                                  const char *sig) {
  (void)env;
  (void)clazz;
  debugPrintf("jni_shim: GetStaticFieldID(%s, %s)\n", name, sig);
  if (name && strcmp(name, "currentActivity") == 0) {
    debugPrintf("jni_shim: GetStaticFieldID(currentActivity) -> activity field\n");
    return &g_current_activity_field_id;
  }
  if (strcmp(name, "OBB_VERSIONCODE") == 0)
    return &g_method_tags[FID_OBB_VERSIONCODE];
  /* registra o nome p/ GetStaticObjectField devolver a chave certa
     (AudioManager.PROPERTY_OUTPUT_*  -> getProperty distingue) */
  return reg_fid(name, sig);
}

/* CallObjectMethod — Unity (C++) usa a variante V (va_list); dispatch nela. */
/* args do player entregues pelo extra "unity" do Intent (caminho nativo do Android) */
static int g_unityargs_sentinel;
static const char *hd_unity_args(void) {
  const char *e = getenv("HD_UNITY_ARGS");
  if (!e || !*e) e = getenv("HD_GFXARGS");
  if (e && *e) return e;
  /* gfx-direct = render na main (sem GfxDeviceWorker); gles20 = Utgard é GLES2 estrito */
  return "-force-gfx-direct -force-gles20";
}

static void *jni_CallObjectMethodV(void *env, void *obj, void *methodID,
                                   va_list ap) {
  (void)env;
  const char *nm = mid_name(methodID);
  debugPrintf("jni_shim: CallObjectMethod(%s)\n", nm ? nm : "?");
  static int fake_obj;
  if (nm) {
    if (strcmp(nm, "loadClass") == 0)
      return class_from_jstring(va_arg(ap, void *));
    /* Complete the Android display-mode query used during Unity/Swappy setup.
       Dimensions continue to come from the live framebuffer. */
    if (strcmp(nm, "getDisplay") == 0 ||
        strcmp(nm, "getDefaultDisplay") == 0 ||
        strcmp(nm, "getPresentationDisplay") == 0)
      return &g_display_sentinel;
    if (strcmp(nm, "getMode") == 0)
      return &g_display_mode_sentinel;
    if (strcmp(nm, "getSupportedModes") == 0) {
      debugPrintf("[DISPLAY] getSupportedModes -> 1 modo @ 60 Hz\n");
      return display_modes_array();
    }
    /* AndroidJavaObject converte System.String pelo fluxo nativo do Unity:
       byte[] UTF-8 -> new java.lang.String(bytes, "UTF-8") na ida e
       String.getBytes("UTF-8") na volta. O stub antigo descartava ambos e toda
       string gerenciada virava "", quebrando chaves de SharedPreferences. */
    if (strcmp(nm, "getBytes") == 0) {
      (void)va_arg(ap, void *); /* charset ("UTF-8") */
      if (!is_jstring(obj)) return barr_new(0);
      const char *text = resolve_jstring(obj);
      size_t len = strlen(text);
      void *array = barr_new((int)len);
      struct barr *bytes = barr_find(array);
      if (bytes && len) memcpy(bytes->buf, text, len);
      return array;
    }
    if (strcmp(nm, "getNetworkProxySettings") == 0)
      return make_jstring(""); /* Android sem proxy configurado */
    if (strcmp(nm, "toString") == 0 && is_jstring(obj))
      return obj;
    if (obj == (void *)&g_google_token_result_sentinel) {
      if (strcmp(nm, "getAccount") == 0) return NULL;
      if (strcmp(nm, "getEmail") == 0 || strcmp(nm, "getAuthCode") == 0 ||
          strcmp(nm, "getIdToken") == 0)
        return make_jstring("");
    }
    const char *class_name = class_name_for(obj);
    if (class_name &&
        (strcmp(nm, "getName") == 0 ||
         strcmp(nm, "getCanonicalName") == 0 ||
         strcmp(nm, "getTypeName") == 0)) {
      char dotted[192];
      size_t n = strlen(class_name);
      if (n >= sizeof dotted) n = sizeof dotted - 1;
      for (size_t i = 0; i < n; i++)
        dotted[i] = class_name[i] == '/' ? '.' : class_name[i];
      dotted[n] = '\0';
      return make_jstring(dotted);
    }
    if (strcmp(nm, "getPackageName") == 0)
      return make_jstring(g_package_name);
    /* ---- Gamepad Xbox 360 virtual: InputManager.getInputDevice(id) + getters ---- */
    if (strcmp(nm, "getInputDeviceIds") == 0) {
      int one[1] = {1};
      static int logged;
      if (!logged++) debugPrintf("jni_shim: InputManager.getInputDeviceIds() -> [1] (Xbox)\n");
      return iarr_new(one, 1);
    }
    if (strcmp(nm, "getInputDevice") == 0) return &g_gamepad_device;
    if (obj == (void *)&g_gamepad_device) {
      if (strcmp(nm, "getName") == 0)          return make_jstring("Microsoft X-Box 360 pad");
      if (strcmp(nm, "getDescriptor") == 0)    return make_jstring("xbox360pad-virtual");
      if (strcmp(nm, "getMotionRanges") == 0)  return &g_empty_list;
      if (strcmp(nm, "getMotionRange") == 0)   return NULL;       /* sem range específico */
      if (strcmp(nm, "getVibrator") == 0)      return obj;        /* não-nulo */
      if (strcmp(nm, "getKeyCharacterMap") == 0) return obj;      /* não-nulo */
      return obj;   /* qualquer outro método do device -> não-nulo */
    }
    /* Play Asset Delivery's AssetPackLocation.assetsPath() returns the pack's
       .../assets directory (not the pack root).  Unity strips that final
       "/assets" before mounting <root>/assets/bin/Data/datapack.unity3d.
       Returning the root makes Unity strip seven characters from the actual
       runtime directory. */
    if (strcmp(nm, "getAssetPackPath") == 0)
      return make_game_path_jstring("assets");
    if (strcmp(nm, "getInstallerPackageName") == 0)
      return hd_installer_name();
    /* Method fake do Runnable (jnibridge invoke): getName()->"run" p/ o C# despachar. */
    if (jni_is_run_method(obj)) {
      if (strcmp(nm, "getName") == 0) return make_jstring("run");
      if (strcmp(nm, "toString") == 0)
        return make_jstring(reflected_method_string(obj));
      return &g_run_method_sentinel; /* getReturnType/getParameterTypes/... -> não-nulo */
    }
    /* Method fake do FrameCallback (Choreographer): getName()->"doFrame". */
    if (obj == (void *)&g_doframe_method_sentinel) {
      if (strcmp(nm, "getName") == 0) return make_jstring("doFrame");
      if (strcmp(nm, "toString") == 0)
        return make_jstring(reflected_method_string(obj));
      return &g_doframe_method_sentinel;
    }
    /* Method fake do Handler$Callback: getName()->"handleMessage". */
    if (obj == (void *)&g_handlemsg_method_sentinel) {
      if (strcmp(nm, "getName") == 0) return make_jstring("handleMessage");
      if (strcmp(nm, "toString") == 0)
        return make_jstring(reflected_method_string(obj));
      return &g_handlemsg_method_sentinel;
    }
    /* Method fake do ResultCallback: getName()->"onResult". */
    if (obj == (void *)&g_onresult_method_sentinel) {
      if (strcmp(nm, "getName") == 0) return make_jstring("onResult");
      if (strcmp(nm, "toString") == 0)
        return make_jstring(reflected_method_string(obj));
      return &g_onresult_method_sentinel;
    }
    /* Handler.obtainMessage(what[,...]) -> a nossa Message sentinel (guarda o what). */
    if (strcmp(nm, "obtainMessage") == 0) {
      g_message_what = va_arg(ap, int);
      debugPrintf("jni_shim: obtainMessage(what=%d) -> Message sentinel\n", g_message_what);
      return &g_message_sentinel;
    }
    if (strcmp(nm, "getLooper") == 0 || strcmp(nm, "myLooper") == 0 ||
        strcmp(nm, "getMainLooper") == 0)
      return &g_looper_sentinel;
    /* Long boxed (arg do doFrame): longValue()/valueOf devolvem o frameTimeNanos. */
    if (obj == (void *)&g_long_box_sentinel) {
      return &g_long_box_sentinel;   /* getClass etc. -> não-nulo */
    }
    /* ClassLoader.findLibrary("il2cpp") -> path real do .so (ja' carregamos no F1,
       mas o UnityPlayer valida via findLibrary+System.load senao "Failed to load Il2CPP") */
    if (strcmp(nm, "findLibrary") == 0) {
      void *libname = va_arg(ap, void *);
      const char *ln = resolve_jstring(libname);
      debugPrintf("jni_shim: findLibrary(%s)\n", ln);
      if (ln && strstr(ln, "il2cpp"))
        return make_game_path_jstring("libil2cpp.so");
      if (ln && strstr(ln, "main"))
        return make_game_path_jstring("libmain.so");
      if (ln && strstr(ln, "unity"))
        return make_game_path_jstring("libunity.so");
      return make_jstring("");
    }
    /* AudioManager.getProperty(key) -> valores válidos p/ o FMOD não configurar
       buffer/samplerate=0 (parseInt do nosso stub dava 0 -> mixer travava no boot) */
    if (strcmp(nm, "getProperty") == 0) {
      void *keyo = va_arg(ap, void *);
      const char *key = resolve_jstring(keyo);
      const char *val = getenv("HD_AUDIO_RATE") ? getenv("HD_AUDIO_RATE") : "44100";
      if (key && strstr(key, "FRAMES_PER_BUFFER")) val = "256";
      debugPrintf("jni_shim: getProperty(%s) -> %s\n", key ? key : "?", val);
      return make_jstring(val);
    }
    /* AssetManager bridge */
    if (strcmp(nm, "getAssets") == 0) return &g_assetmgr;
    /* listas vazias (queryIntentActivities, etc.) + iterator vazio */
    if (strcmp(nm, "queryIntentActivities") == 0 ||
        strcmp(nm, "queryBroadcastReceivers") == 0 ||
        strcmp(nm, "getSystemSharedLibraryNames") == 0)
      return &g_empty_list;
    if (strcmp(nm, "iterator") == 0) return &g_iterator;
    if (strcmp(nm, "getApplicationInfo") == 0) return &g_appinfo;
    if ((strcmp(nm, "open") == 0 || strcmp(nm, "openNonAsset") == 0) &&
        obj == &g_assetmgr) {
      void *pathstr = va_arg(ap, void *);
      return asset_open(resolve_jstring(pathstr)); /* NULL se nao existe */
    }
    /* builders Android (Intent.addFlags/setData/...) retornam o proprio obj */
    if (strcmp(nm, "addFlags") == 0 || strcmp(nm, "setFlags") == 0 ||
        strcmp(nm, "setData") == 0 || strcmp(nm, "setAction") == 0 ||
        strcmp(nm, "append") == 0)
      return obj;
    /* SharedPreferences.edit() -> editor (encadeável); retorna o proprio obj */
    if (strcmp(nm, "edit") == 0) return obj;
    /* SharedPreferences.Editor.putString(key,val) -> ARMAZENA + retorna editor
       (encadeamento putString(...).putString(...).apply()). */
    if (strcmp(nm, "putString") == 0) {
      void *keyo = va_arg(ap, void *), *valo = va_arg(ap, void *);
      const char *key = resolve_jstring(keyo), *val = resolve_jstring(valo);
      prefs_put_string(key, val);
      debugPrintf("[PREFS] putString key='%s' (%zu bytes) ARMAZENADO\n", key, strlen(val));
      return obj;
    }
    if (strcmp(nm, "putInt") == 0) {
      void *keyo = va_arg(ap, void *); int val = va_arg(ap, int);
      prefs_put_int(resolve_jstring(keyo), val);
      debugPrintf("[PREFS] putInt key='%s' val=%d ARMAZENADO\n", resolve_jstring(keyo), val);
      return obj;
    }
    if (strcmp(nm, "putBoolean") == 0) {
      void *keyo = va_arg(ap, void *); int val = va_arg(ap, int);
      prefs_put_bool(resolve_jstring(keyo), val);
      return obj;
    }
    if (strcmp(nm, "putFloat") == 0) {
      void *keyo = va_arg(ap, void *); double val = va_arg(ap, double);
      prefs_put_float(resolve_jstring(keyo), (float)val);
      return obj;
    }
    if (strcmp(nm, "putLong") == 0) {
      void *keyo = va_arg(ap, void *); int64_t val = va_arg(ap, int64_t);
      prefs_put_long(resolve_jstring(keyo), val);
      return obj;
    }
    if (strcmp(nm, "remove") == 0) {
      prefs_remove(resolve_jstring(va_arg(ap, void *)));
      return obj;
    }
    if (strcmp(nm, "clear") == 0) {
      prefs_clear();
      return obj;
    }
    /* diretorios de dados -> path REAL gravavel (persistentDataPath do Unity).
       Sem isso (=""), PlayerPrefs/save quebram -> jogo trava em "first run". */
    if (strcmp(nm, "getFilesDir") == 0 || strcmp(nm, "getExternalFilesDir") == 0 ||
        strcmp(nm, "getCacheDir") == 0 || strcmp(nm, "getExternalCacheDir") == 0 ||
        strcmp(nm, "getDataDir") == 0 || strcmp(nm, "getExternalStorageDirectory") == 0 ||
        strcmp(nm, "getPath") == 0 || strcmp(nm, "getAbsolutePath") == 0 ||
        strcmp(nm, "getCanonicalPath") == 0)
      return make_game_path_jstring("userdata");
    /* SharedPreferences.getString(key, default) -> valor ARMAZENADO se existir,
       senão o default. Faz o round-trip do save funcionar (era sempre default). */
    if (strcmp(nm, "getString") == 0) {
      void *keystr = va_arg(ap, void *);
      void *defstr = va_arg(ap, void *);
      const char *key = resolve_jstring(keystr);
      /* extra "unity" do Intent = command line do player (Bundle.getString) */
      if (key && strcmp(key, "unity") == 0) {
        debugPrintf("jni_shim: cmdline do Unity = \"%s\"\n", hd_unity_args());
        return make_jstring(hd_unity_args());
      }
      /* A settings override from another title is intentionally not built. */
#if 0
      /* HD_NOFX: força o jogo a CARREGAR settings com PÓS-PROCESSAMENTO OFF
         (chromaticAberration/noise/blur) — esses efeitos usam FBO/render-to-texture
         que TRAVAM o GPU Mali Utgard no carregamento do título. */
      if (getenv("HD_NOFX") && key && strstr(key, "settings_data")) {
        int sw = 0, sh = 0;
        hd_display_size(&sw, &sh);
        static char FX_OFF[512];
        snprintf(FX_OFF, sizeof(FX_OFF),
          "{\"hasBootedUpGame\":true,\"overscan\":0.0,\"chromaticAberration\":0.0,"
          "\"screenWidth\":%d,\"screenHeight\":%d,\"effects\":false,\"blur\":false,"
          "\"forceOriginalTitleScreen\":false,\"masterVolume\":0.0,\"sFXVolume\":0.0,"
          "\"musicVolume\":0.0,\"canVibrate\":true,\"rotateControlsWithCamera\":false,"
          "\"language\":-1,\"chromaticAberrationEffect\":false,\"noiseEffect\":false,"
          "\"subtleBlurEffect\":false,\"brightness\":0.0}", sw, sh);
        debugPrintf("[NOFX] getString settings -> efeitos OFF (anti-wedge Utgard)\n");
        return make_jstring(FX_OFF);
      }
#endif
      char *stored = prefs_dup_string(key);
      debugPrintf("[PREFS] getString key='%s' -> %s\n", key, stored ? "ARMAZENADO" : "default");
      if (stored) {
        void *result = make_jstring(stored);
        free(stored);
        return result;
      }
      return defstr ? defstr : make_jstring("");
    }
    /* 🔑 Command line do Unity no Android = extra "unity" do Intent:
     *   extras = activity.getIntent().getExtras();
     *   if (extras.containsKey("unity")) cmdLine = extras.get("unity").toString();
     * É ASSIM que o player recebe -force-gles20/-force-gfx-direct. Sem isto o Unity
     * bootava sem argumento nenhum (o /proc/self/cmdline falso não é lido no Android)
     * e o render multi-thread continuava ligado. */
    if (strcmp(nm, "get") == 0 || strcmp(nm, "getCharSequence") == 0) {
      void *keyo = va_arg(ap, void *);
      const char *key = resolve_jstring(keyo);
      if (key && strcmp(key, "unity") == 0) {
        debugPrintf("jni_shim: Bundle.get(\"unity\") -> args\n");
        return &g_unityargs_sentinel;
      }
    }
    if (strcmp(nm, "toString") == 0) {
      if (obj == (void *)&g_unityargs_sentinel) {
        debugPrintf("jni_shim: cmdline do Unity = \"%s\"\n", hd_unity_args());
        return make_jstring(hd_unity_args());
      }
      return make_jstring("");
    }
    /* log de métodos de reflection p/ diagnóstico (gated) */
    if (getenv("HD_REFLOG") &&
        (strstr(nm,"Field")||strstr(nm,"Type")||strstr(nm,"Component")||
         strstr(nm,"getClass")||strstr(nm,"getDeclar")||strcmp(nm,"getType")==0)) {
      static int rn=0; if (rn++<40) debugPrintf("[REFLOG-obj] %s\n", nm);
    }
  }
  nx_jval declared = nx_call_v(env, obj, methodID, ap);
  return declared.l ? declared.l : &fake_obj;
}
static void *jni_CallObjectMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  void *r = jni_CallObjectMethodV(env, obj, methodID, ap);
  va_end(ap);
  return r;
}

static void *jni_CallObjectMethodA(void *env, void *obj, void *methodID, const jvalue *args) {
  (void)env;
  const char *nm = mid_name(methodID);
  if (nm && strcmp(nm, "loadClass") == 0)
    return class_from_jstring(args ? args[0].l : NULL);
  /* Unity 6 uses the JNI A-form here while older players used the V-form.
   * Returning a generic object makes its native library discovery report
   * "Failed to load Il2CPP" even though NativeLoader already initialized the
   * correlated owner-supplied module.  Keep both JNI call forms equivalent. */
  if (nm && strcmp(nm, "findLibrary") == 0) {
    const char *ln = args ? resolve_jstring(args[0].l) : "";
    debugPrintf("jni_shim: findLibraryA(%s)\n", ln);
    if (strstr(ln, "il2cpp"))
      return make_game_path_jstring("libil2cpp.so");
    if (strstr(ln, "main"))
      return make_game_path_jstring("libmain.so");
    if (strstr(ln, "unity"))
      return make_game_path_jstring("libunity.so");
    return make_jstring("");
  }
  if (nm && (strcmp(nm, "getDisplay") == 0 ||
             strcmp(nm, "getDefaultDisplay") == 0 ||
             strcmp(nm, "getPresentationDisplay") == 0))
    return &g_display_sentinel;
  if (nm && strcmp(nm, "getMode") == 0)
    return &g_display_mode_sentinel;
  if (nm && strcmp(nm, "getSupportedModes") == 0)
    return display_modes_array();
  if (nm && strcmp(nm, "getBytes") == 0) {
    if (!is_jstring(obj)) return barr_new(0);
    const char *text = resolve_jstring(obj);
    size_t len = strlen(text);
    void *array = barr_new((int)len);
    struct barr *bytes = barr_find(array);
    if (bytes && len) memcpy(bytes->buf, text, len);
    return array;
  }
  if (nm && strcmp(nm, "getNetworkProxySettings") == 0)
    return make_jstring("");
  if (nm && strcmp(nm, "toString") == 0 && is_jstring(obj))
    return obj;
  if (nm && obj == (void *)&g_google_token_result_sentinel) {
    if (strcmp(nm, "getAccount") == 0) return NULL;
    if (strcmp(nm, "getEmail") == 0 || strcmp(nm, "getAuthCode") == 0 ||
        strcmp(nm, "getIdToken") == 0)
      return make_jstring("");
  }
  if (nm && (obj == (void *)&g_run_method_sentinel ||
             obj == (void *)&g_doframe_method_sentinel ||
             obj == (void *)&g_handlemsg_method_sentinel ||
             obj == (void *)&g_onresult_method_sentinel)) {
    if (strcmp(nm, "toString") == 0)
      return make_jstring(reflected_method_string(obj));
    if (strcmp(nm, "getName") == 0) {
      if (obj == (void *)&g_run_method_sentinel) return make_jstring("run");
      if (obj == (void *)&g_doframe_method_sentinel) return make_jstring("doFrame");
      if (obj == (void *)&g_handlemsg_method_sentinel) return make_jstring("handleMessage");
      return make_jstring("onResult");
    }
    return obj;
  }
  const char *class_name = class_name_for(obj);
  if (nm && class_name &&
      (strcmp(nm, "getName") == 0 ||
       strcmp(nm, "getCanonicalName") == 0 ||
       strcmp(nm, "getTypeName") == 0)) {
    char dotted[192];
    size_t n = strlen(class_name);
    if (n >= sizeof dotted) n = sizeof dotted - 1;
    for (size_t i = 0; i < n; i++)
      dotted[i] = class_name[i] == '/' ? '.' : class_name[i];
    dotted[n] = '\0';
    return make_jstring(dotted);
  }
  if (nm && strcmp(nm, "obtainMessage") == 0) {
    g_message_what = args ? args[0].i : 0;
    debugPrintf("jni_shim: obtainMessageA(what=%d) -> Message sentinel\n", g_message_what);
    return &g_message_sentinel;
  }
  if (nm && (strcmp(nm, "getLooper") == 0 || strcmp(nm, "myLooper") == 0 ||
             strcmp(nm, "getMainLooper") == 0))
    return &g_looper_sentinel;
  if (nm && strcmp(nm, "getPackageName") == 0)
    return make_jstring(g_package_name);
  if (nm && strcmp(nm, "getAssetPackPath") == 0)
    return make_game_path_jstring("assets");
  if (nm && strcmp(nm, "getInstallerPackageName") == 0)
    return hd_installer_name();
  if (nm && strcmp(nm, "getAssets") == 0)
    return &g_assetmgr;
  if (nm && strcmp(nm, "getApplicationInfo") == 0)
    return &g_appinfo;
  if (nm && strcmp(nm, "edit") == 0)
    return obj;
  if (nm && strcmp(nm, "putString") == 0) {
    const char *key = args ? resolve_jstring(args[0].l) : "";
    const char *val = args ? resolve_jstring(args[1].l) : "";
    prefs_put_string(key, val);
    debugPrintf("[PREFS] putStringA key='%s' (%zu bytes) ARMAZENADO\n", key, strlen(val));
    return obj;
  }
  if (nm && strcmp(nm, "putInt") == 0) {
    const char *key = args ? resolve_jstring(args[0].l) : "";
    prefs_put_int(key, args ? args[1].i : 0);
    return obj;
  }
  if (nm && strcmp(nm, "putBoolean") == 0) {
    const char *key = args ? resolve_jstring(args[0].l) : "";
    prefs_put_bool(key, args ? args[1].z : 0);
    return obj;
  }
  if (nm && strcmp(nm, "putFloat") == 0) {
    const char *key = args ? resolve_jstring(args[0].l) : "";
    prefs_put_float(key, args ? args[1].f : 0.0f);
    return obj;
  }
  if (nm && strcmp(nm, "putLong") == 0) {
    const char *key = args ? resolve_jstring(args[0].l) : "";
    prefs_put_long(key, args ? args[1].j : 0);
    return obj;
  }
  if (nm && strcmp(nm, "remove") == 0) {
    prefs_remove(args ? resolve_jstring(args[0].l) : "");
    return obj;
  }
  if (nm && strcmp(nm, "clear") == 0) {
    prefs_clear();
    return obj;
  }
  if (nm && strcmp(nm, "getString") == 0) {
    const char *key = args ? resolve_jstring(args[0].l) : "";
    void *defstr = args ? args[1].l : NULL;
#if HD_DEV_DIAGNOSTICS
    if (getenv("HD_PREFS_TRACE"))
      debugPrintf("[PREFS-TRACE] getStringA keyObj=%p known=%d key='%s' defaultObj=%p\n",
                  args ? args[0].l : NULL,
                  args ? is_jstring(args[0].l) : 0, key, defstr);
#endif
    if (key && strcmp(key, "unity") == 0) {
      debugPrintf("jni_shim: cmdline do Unity (A) = \"%s\"\n", hd_unity_args());
      return make_jstring(hd_unity_args());
    }
    char *stored = prefs_dup_string(key);
    debugPrintf("[PREFS] getStringA key='%s' -> %s\n", key, stored ? "ARMAZENADO" : "default");
    if (stored) {
      void *result = make_jstring(stored);
      free(stored);
      return result;
    }
    return defstr ? defstr : make_jstring("");
  }
  if (nm && (strcmp(nm, "getFilesDir") == 0 ||
             strcmp(nm, "getExternalFilesDir") == 0 ||
             strcmp(nm, "getCacheDir") == 0 ||
             strcmp(nm, "getExternalCacheDir") == 0 ||
             strcmp(nm, "getDataDir") == 0 ||
             strcmp(nm, "getExternalStorageDirectory") == 0 ||
             strcmp(nm, "getPath") == 0 ||
             strcmp(nm, "getAbsolutePath") == 0 ||
             strcmp(nm, "getCanonicalPath") == 0))
    return make_game_path_jstring("userdata");
  nx_jval declared = nx_call_a(env, obj, methodID, args);
  static int fake_obj;
  return declared.l ? declared.l : &fake_obj;
}

/* CallBooleanMethod V (index 38) — lê args via va_list (variante que il2cpp usa) */
static unsigned char jni_CallBooleanMethodV(void *env, void *obj,
                                            void *methodID, va_list ap) {
  const char *nm = mid_name(methodID);
  if (nm) {
    if (obj == &g_fmod_device_obj && strcmp(nm, "isRunning") == 0)
      return __atomic_load_n(&g_fmod_should_run, __ATOMIC_ACQUIRE) ? 1 : 0;
    if (getenv("HD_REFLOG") && (strstr(nm,"isArray")||strstr(nm,"isPrimitive")||strstr(nm,"isAssign"))) {
      static int bn=0; if (bn++<40) debugPrintf("[REFLOG-bool] %s -> 0\n", nm);
    }
    /* Bundle.containsKey("unity") -> true: é por aqui que o Unity descobre que há
       command line (ver jni_CallObjectMethodV, extra "unity"). */
    if (strcmp(nm, "containsKey") == 0) {
      void *keyo = va_arg(ap, void *);
      const char *key = resolve_jstring(keyo);
      int has = (key && strcmp(key, "unity") == 0) ? 1 : 0;
      debugPrintf("jni_shim: Bundle.containsKey(\"%s\") -> %d\n", key ? key : "?", has);
      return (unsigned char)has;
    }
    if (strcmp(nm, "isEmpty") == 0) return 1;  /* lista vazia */
    if (strcmp(nm, "hasNext") == 0) return 0;  /* iterator vazio */
    /* Handler.post/postDelayed(Runnable[,delay]) -> RODA o Runnable, retorna true.
       (init deferida do Unity usa Handler.post; sem rodar, o boot trava no poll.) */
    if (strcmp(nm, "post") == 0 || strcmp(nm, "postDelayed") == 0 ||
        strcmp(nm, "postAtTime") == 0 || strcmp(nm, "postAtFrontOfQueue") == 0) {
      void *r = va_arg(ap, void *);
      if (!getenv("HD_NORUNUI")) run_runnable(env, r);
      return 1;
    }
    /* SharedPreferences.contains(key) -> 1 se ARMAZENADO (round-trip do save). */
    if (strcmp(nm, "contains") == 0) {
      void *keyo = va_arg(ap, void *);
      const char *key = resolve_jstring(keyo);
      int has = getenv("HD_NOCONTAINS") ? 0 : prefs_contains(key);
      debugPrintf("[PREFS] contains key='%s' -> %d\n", key, has);
      return (unsigned char)has;
    }
    if (strcmp(nm, "commit") == 0)
      return prefs_save() ? 1 : 0;  /* Editor.commit() */
    if (strcmp(nm, "getBoolean") == 0) {
      const char *key = resolve_jstring(va_arg(ap, void *));
      int fallback = va_arg(ap, int);
      int v = prefs_get_bool(key, fallback);
      debugPrintf("[PREFS] getBoolean key='%s' -> %d\n", key, v);
      return (unsigned char)v;
    }
  }
  return nx_call_v(env, obj, methodID, ap).i ? 1 : 0;
}
static unsigned char jni_CallBooleanMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  unsigned char r = jni_CallBooleanMethodV(env, obj, methodID, ap);
  va_end(ap);
  return r;
}
static unsigned char jni_CallBooleanMethodA(void *env, void *obj,
                                             void *methodID,
                                             const jvalue *args) {
  (void)env;
  (void)obj;
  const char *nm = mid_name(methodID);
  if (nm && strcmp(nm, "contains") == 0) {
    const char *key = args ? resolve_jstring(args[0].l) : "";
    int result = prefs_contains(key) ? 1 : 0;
#if HD_DEV_DIAGNOSTICS
    if (getenv("HD_PREFS_TRACE"))
      debugPrintf("[PREFS-TRACE] containsA keyObj=%p known=%d key='%s' -> %d\n",
                  args ? args[0].l : NULL,
                  args ? is_jstring(args[0].l) : 0, key, result);
#endif
    return (unsigned char)result;
  }
  if (nm && strcmp(nm, "commit") == 0) return prefs_save() ? 1 : 0;
  if (nm && strcmp(nm, "getBoolean") == 0) {
    const char *key = args ? resolve_jstring(args[0].l) : "";
    int fallback = args ? args[1].z : 0;
    return (unsigned char)prefs_get_bool(key, fallback);
  }
  return nx_call_a(env, obj, methodID, args).i ? 1 : 0;
}

/* CallIntMethod — variante V */
static jint jni_CallIntMethodV(void *env, void *obj, void *methodID,
                               va_list ap) {
  (void)env;
  const char *nm = mid_name(methodID);
  /* java.lang.String.length(). O Unity usa esse valor para dimensionar na
     pilha os caminhos main/patch OBB antes do sprintf. Retornar zero faz o
     buffer ficar com apenas a folga fixa e o sprintf sobrescrever os smart
     pointers vizinhos. Os caminhos usados aqui são ASCII, então strlen
     também corresponde ao número de UTF-16 code units do Java. */
  if (nm && strcmp(nm, "length") == 0 && is_jstring(obj))
    return (jint)strlen(resolve_jstring(obj));
  /* GoogleApi status 8 = INTERNAL_ERROR. O plugin converte pelo fluxo normal
     em SignInStatus.InternalError e conclui a autenticação offline. */
  if (obj == (void *)&g_google_token_result_sentinel && nm &&
      strcmp(nm, "getStatusCode") == 0)
    return 8;
  if (obj == (void *)&g_message_sentinel && nm && strcmp(nm, "getWhat") == 0)
    return g_message_what;
  /* org.fmod.FMODAudioDevice — qualquer método int/bool (start/isRunning/init...) = sucesso */
  if (obj == &g_fmod_device_obj) { debugPrintf("jni_shim: FMODAudioDevice.%s -> 1\n", nm?nm:"?"); return 1; }
  if (nm) {
    /* Manifest permissions are present in the supplied APK. Report granted and
       let network/service availability fail through its normal API path. */
    if (strcmp(nm, "checkCallingOrSelfPermission") == 0 ||
        strcmp(nm, "checkSelfPermission") == 0 ||
        strcmp(nm, "checkPermission") == 0)
      return 0;
    /* android.os.StatFs — report the actual filesystem backing userdata. */
    if (strcmp(nm, "getBlockSize") == 0)
      return hd_storage_int(hd_query_storage().block_size);
    if (strcmp(nm, "getAvailableBlocks") == 0)
      return hd_storage_int(hd_query_storage().available_blocks);
    if (strcmp(nm, "getFreeBlocks") == 0)
      return hd_storage_int(hd_query_storage().free_blocks);
    if (strcmp(nm, "getBlockCount") == 0)
      return hd_storage_int(hd_query_storage().total_blocks);
    /* ---- KeyEvent (nativeInjectEvent) ---- */
    /* ---- InputDevice Xbox 360 virtual (getters int) ---- */
    if (obj == (void *)&g_gamepad_device) {
      if (strcmp(nm, "getVendorId") == 0)        return 1118;       /* 0x045E Microsoft */
      if (strcmp(nm, "getProductId") == 0)       return 654;        /* 0x028E Xbox360 pad */
      if (strcmp(nm, "getSources") == 0)         return 0x1000611;  /* GAMEPAD|JOYSTICK|DPAD */
      if (strcmp(nm, "getId") == 0)              return 1;
      if (strcmp(nm, "getControllerNumber") == 0) return 1;
      if (strcmp(nm, "getKeyboardType") == 0)    return 0;
      if (strcmp(nm, "supportsSource") == 0)     return 1;
      return 0;
    }
    if (strcmp(nm, "getAction") == 0) { debugPrintf("[KEYEV] getAction->%d\n", g_hk_inject.action); return g_hk_inject.action; }
    if (strcmp(nm, "getKeyCode") == 0) { debugPrintf("[KEYEV] getKeyCode->%d\n", g_hk_inject.keycode); return g_hk_inject.keycode; }
    if (strcmp(nm, "getSource") == 0) return g_hk_inject.source;
    if (strcmp(nm, "getDeviceId") == 0) return g_hk_inject.deviceId;
    if (strcmp(nm, "getMetaState") == 0) return g_hk_inject.metaState;
    if (strcmp(nm, "getRepeatCount") == 0) return g_hk_inject.repeat;
    if (strcmp(nm, "getScanCode") == 0) return g_hk_inject.scancode;
    if (strcmp(nm, "getInt") == 0) { void *k = va_arg(ap, void *); int d = va_arg(ap, int);
      const char *key = resolve_jstring(k);
      int v = prefs_get_int(key, d);
      debugPrintf("[PREFS] getInt key='%s' def=%d -> %d\n", key, d, v); return v; }
    if (strcmp(nm, "getFlags") == 0) return g_hk_inject.flags;
    if (strcmp(nm, "getUnicodeChar") == 0) return g_hk_inject.unicode;
    if (strcmp(nm, "size") == 0) return 0; /* List/Collection vazia */
    /* ---- Display: o engine pega resolucao/rotacao reais, sem fallback fixo. ---- */
    if (strcmp(nm, "getWidth") == 0 || strcmp(nm, "getRawWidth") == 0) { int w, h; hd_display_size(&w, &h); return w; }
    if (strcmp(nm, "getHeight") == 0 || strcmp(nm, "getRawHeight") == 0) { int w, h; hd_display_size(&w, &h); return h; }
    if (obj == &g_display_mode_sentinel &&
        strcmp(nm, "getPhysicalWidth") == 0) {
      int w, h; hd_display_size(&w, &h); return w;
    }
    if (obj == &g_display_mode_sentinel &&
        strcmp(nm, "getPhysicalHeight") == 0) {
      int w, h; hd_display_size(&w, &h); return h;
    }
    if (obj == &g_display_mode_sentinel && strcmp(nm, "getModeId") == 0)
      return 1;
    if (strcmp(nm, "getRotation") == 0) return 0;
    if (strcmp(nm, "getDisplayId") == 0) return 0;
  }
  struct astream *s = astream_find(obj);
  if (s && nm) {
    if (strcmp(nm, "read") == 0) {
      void *barr = va_arg(ap, void *);
      int off = va_arg(ap, int);
      int len = va_arg(ap, int);
      struct barr *b = barr_find(barr);
      if (!b) return -1;
      if (off < 0) off = 0;
      if (off + len > b->len) len = b->len - off;
      if (len <= 0) return -1;
      size_t n = fread(b->buf + off, 1, (size_t)len, s->fp);
      return n > 0 ? (int)n : -1; /* -1 = EOF */
    }
    if (strcmp(nm, "available") == 0) {
      long pos = ftell(s->fp);
      return (int)(s->size - pos);
    }
  }
  return nx_call_v(env, obj, methodID, ap).i;
}
static jint jni_CallIntMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  jint r = jni_CallIntMethodV(env, obj, methodID, ap);
  va_end(ap);
  return r;
}
static jint jni_CallIntMethodA(void *env, void *obj, void *methodID,
                               const jvalue *args) {
  (void)env;
  const char *nm = mid_name(methodID);
  if (nm && strcmp(nm, "length") == 0 && is_jstring(obj))
    return (jint)strlen(resolve_jstring(obj));
  if (nm && strcmp(nm, "getInt") == 0)
    return prefs_get_int(args ? resolve_jstring(args[0].l) : "",
                         args ? args[1].i : 0);
  if (obj == (void *)&g_google_token_result_sentinel && nm &&
      strcmp(nm, "getStatusCode") == 0)
    return 8;
  if (obj == &g_display_mode_sentinel && nm) {
    if (strcmp(nm, "getPhysicalWidth") == 0) {
      int w, h; hd_display_size(&w, &h); return w;
    }
    if (strcmp(nm, "getPhysicalHeight") == 0) {
      int w, h; hd_display_size(&w, &h); return h;
    }
    if (strcmp(nm, "getModeId") == 0) return 1;
  }
  return nx_call_a(env, obj, methodID, args).i;
}

/* CallVoidMethod (index 94) */
static void jni_CallVoidMethodV(void *env, void *obj, void *methodID, va_list ap) {
  const char *nm = mid_name(methodID);
  debugPrintf("jni_shim: CallVoidMethod(%s)\n", nm ? nm : "?");
  if (nm && strcmp(nm, "apply") == 0) {
    if (!prefs_save()) debugPrintf("[PREFS] Editor.apply falhou\n");
    return;
  }
  if (obj == &g_fmod_device_obj && nm) {
    if (strcmp(nm, "start") == 0) {
      __atomic_store_n(&g_fmod_should_run, 1, __ATOMIC_RELEASE);
      debugPrintf("[AUDIO] FMODAudioDevice.start -> AudioTrack nativo solicitado\n");
      return;
    }
    if (strcmp(nm, "stop") == 0 || strcmp(nm, "close") == 0) {
      __atomic_store_n(&g_fmod_should_run, 0, __ATOMIC_RELEASE);
      debugPrintf("[AUDIO] FMODAudioDevice.%s -> AudioTrack parado\n", nm);
      return;
    }
  }
  if (nm && strcmp(nm, "showSoftInput") == 0) {
    void *text_j = va_arg(ap, void *);
    (void)va_arg(ap, int); /* keyboardType */
    (void)va_arg(ap, int); /* autocorrection */
    (void)va_arg(ap, int); /* multiline */
    (void)va_arg(ap, int); /* secure */
    (void)va_arg(ap, int); /* alert */
    void *placeholder_j = va_arg(ap, void *);
    int limit = va_arg(ap, int);
    (void)va_arg(ap, int); /* selectionStart */
    (void)va_arg(ap, int); /* selectionEnd */
    softinput_show(env, text_j, placeholder_j, limit);
    return;
  }
  if (nm && strcmp(nm, "hideSoftInput") == 0) {
    if (g_softinput_manual && g_softinput_active) {
      debugPrintf("[SOFTINPUT] hide ignorado: teclado virtual manual ativo\n");
      return;
    }
    softinput_native_visible(0);
    g_softinput_active = 0;
    debugPrintf("[SOFTINPUT] hide\n");
    return;
  }
  if (nm && strcmp(nm, "setResultCallback") == 0) {
    void *proxy = va_arg(ap, void *);
    if (proxy_handle(proxy)) {
      g_google_result_proxy = proxy;
      __atomic_store_n(&g_google_result_pending, 1, __ATOMIC_RELEASE);
      debugPrintf("jni_shim: Google ResultCallback enfileirado (V) proxy=%p\n", proxy);
    }
    return;
  }
  /* Message.sendToTarget(): entrega assíncrona ao HandlerThread, como no Android.
     Processar handleMessage aqui, na UnityMain, causa reentrância no frame pacing. */
  if (nm && strcmp(nm, "sendToTarget") == 0 && obj == (void *)&g_message_sentinel) {
    jni_queue_handler_message();
    return;
  }
  /* runOnUiThread/post(Runnable): EXECUTA o Runnable (senão Unity Analytics/init trava). */
  if (nm && (strcmp(nm, "runOnUiThread") == 0 || strcmp(nm, "post") == 0 ||
             strcmp(nm, "postAtFrontOfQueue") == 0)) {
    void *r = va_arg(ap, void *);
    /* roda o Runnable via invoke do jnibridge (handle lido pela variante V correta).
       HD_NORUNUI desliga. */
    if (!getenv("HD_NORUNUI")) run_runnable(env, r);
    return;
  }
  /* Play Asset Delivery: getAssetPackState(name, cb) — Unity espera o callback nativo
     nativeStatusQueryResult(name, status, errorCode). Respondemos COMPLETED(4) na hora:
     os packs (UnityDataAssetPack/StreamingAssets) já estão "instalados" em bin/Data. */
  if (nm && strcmp(nm, "getAssetPackState") == 0) {
    void *name_j = va_arg(ap, void *);
    const char *pn = resolve_jstring(name_j);
    void *fn = jni_find_native("nativeStatusQueryResult");
    debugPrintf("jni_shim: getAssetPackState(%s) -> COMPLETED via nativeStatusQueryResult=%p\n",
                pn ? pn : "?", fn);
    if (fn) {
      static int fake_clazz;
      /* (JNIEnv*, jclass, jstring name, jint status=4 COMPLETED, jint errorCode=0) */
      ((void (*)(void *, void *, void *, int, int))fn)(env, &fake_clazz, name_j, 4, 0);
    }
    return;
  }
  struct astream *s = astream_find(obj);
  if (s && nm && strcmp(nm, "close") == 0) {
    if (s->fp) { fclose(s->fp); s->fp = NULL; }
    return;
  }
  (void)nx_call_v(env, obj, methodID, ap);
}
static void jni_CallVoidMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  jni_CallVoidMethodV(env, obj, methodID, ap);
  va_end(ap);
}

static void jni_CallVoidMethodA(void *env, void *obj, void *methodID, const jvalue *args) {
  const char *nm = mid_name(methodID);
  debugPrintf("jni_shim: CallVoidMethodA(%s)\n", nm ? nm : "?");
  if (nm && strcmp(nm, "apply") == 0) {
    if (!prefs_save()) debugPrintf("[PREFS] Editor.apply(A) falhou\n");
    return;
  }
  if (obj == &g_fmod_device_obj && nm) {
    if (strcmp(nm, "start") == 0) {
      __atomic_store_n(&g_fmod_should_run, 1, __ATOMIC_RELEASE);
      debugPrintf("[AUDIO] FMODAudioDevice.start(A) -> AudioTrack nativo solicitado\n");
      return;
    }
    if (strcmp(nm, "stop") == 0 || strcmp(nm, "close") == 0) {
      __atomic_store_n(&g_fmod_should_run, 0, __ATOMIC_RELEASE);
      debugPrintf("[AUDIO] FMODAudioDevice.%s(A) -> AudioTrack parado\n", nm);
      return;
    }
  }
  if (nm && strcmp(nm, "sendToTarget") == 0 &&
      obj == (void *)&g_message_sentinel) {
    jni_queue_handler_message();
    return;
  }
  if (nm && (strcmp(nm, "runOnUiThread") == 0 ||
             strcmp(nm, "post") == 0 ||
             strcmp(nm, "postAtFrontOfQueue") == 0)) {
    void *r = args ? args[0].l : NULL;
    if (!getenv("HD_NORUNUI")) run_runnable(env, r);
    return;
  }
  if (nm && strcmp(nm, "showSoftInput") == 0) {
    void *text_j = args ? args[0].l : NULL;
    void *placeholder_j = args ? args[6].l : NULL;
    int limit = args ? args[7].i : 32;
    softinput_show(env, text_j, placeholder_j, limit);
    return;
  }
  if (nm && strcmp(nm, "hideSoftInput") == 0) {
    if (g_softinput_manual && g_softinput_active) {
      debugPrintf("[SOFTINPUT] hideA ignorado: teclado virtual manual ativo\n");
      return;
    }
    softinput_native_visible(0);
    g_softinput_active = 0;
    debugPrintf("[SOFTINPUT] hide\n");
    return;
  }
  if (nm && strcmp(nm, "setResultCallback") == 0) {
    void *proxy = args ? args[0].l : NULL;
    if (proxy_handle(proxy)) {
      g_google_result_proxy = proxy;
      __atomic_store_n(&g_google_result_pending, 1, __ATOMIC_RELEASE);
      debugPrintf("jni_shim: Google ResultCallback enfileirado (A) proxy=%p\n", proxy);
    } else {
      debugPrintf("jni_shim: setResultCallback sem proxy AndroidJavaProxy (%p)\n", proxy);
    }
    return;
  }
  (void)nx_call_a(env, obj, methodID, args);
}

/* CallStaticObjectMethod (index 113) */
static void *jni_CallStaticObjectMethodV(void *env, void *clazz,
                                         void *methodID, va_list ap) {
  (void)env;
  const char *nm = mid_name(methodID);
  /* Unity 6 resolves the HandlerThread and Choreographer interfaces through
   * Class.forName(String, boolean, ClassLoader).  Returning a generic object
   * here prevents NewObject(HandlerThread), leaves the Java callback proxy
   * untyped and makes the engine wait forever for its looper. */
  if (nm && !strcmp(nm, "forName") &&
      clazz == class_for("java/lang/Class"))
    return class_from_jstring(va_arg(ap, void *));
  /* InputDevice.getDeviceIds() -> int[] REAL (sem args). Huntdown always
     exposes the validated virtual Xbox device used by its native adapter. */
  if (nm && !strcmp(nm, "getDeviceIds")) {
    int one[1] = {1};
    static int o2 = 0;
    if (!o2) {
      o2 = 1;
      debugPrintf("getDeviceIds()->[1] (Xbox virtual)\n");
    }
    return iarr_new(one, 1);
  }
  /* Environment.getExternalStorageState() -> "mounted" (senão o jogo acha o storage indisponível
     e mostra "low on storage" ao entrar no Single Player). */
  if (nm && !strcmp(nm, "getExternalStorageState")) return make_jstring("mounted");
  /* Proven PlayerPrefs routing used by the working Horizon Chase port, with
     Android-compatible percent encoding rather than a lossy fake object. */
  const char *static_class = class_name_for(clazz);
  if (nm && static_class && !strcmp(static_class, "android/net/Uri")) {
    if (!strcmp(nm, "encode"))
      return uri_encode_jstring(va_arg(ap, void *));
    if (!strcmp(nm, "decode"))
      return uri_decode_jstring(va_arg(ap, void *));
  }
  /* InputDevice.getDevice(id) é ESTÁTICO → o device Xbox 360 virtual (HD_GAMEPAD). */
  if (nm && !strcmp(nm, "getDevice")) {
    debugPrintf("jni_shim: getDevice() -> Xbox virtual\n");
    return &g_gamepad_device;
  }
  /* jnibridge: newInterfaceProxy(long handle, Class[] ifaces) -> proxy. Guarda o
     handle p/ rodar o Runnable depois (runOnUiThread). */
  if (nm && !strcmp(nm, "newInterfaceProxy")) {
    long h = va_arg(ap, long);
    void *interfaces = va_arg(ap, void *);
    void *proxy = malloc(16);
    proxy_register(proxy, h, interfaces);
    debugPrintf("jni_shim: newInterfaceProxy(handle=%ld) -> %p\n", h, proxy);
    if (g_next_proxy_is_framecb ||
        proxy_has_interface(proxy, PROXY_IFACE_FRAME_CALLBACK)) {
      g_framecb_proxy = proxy;
      g_next_proxy_is_framecb = 0;
      debugPrintf("jni_shim: [CHOREO] FrameCallback capturado: proxy=%p handle=%ld\n", proxy, h);
    }
    return proxy;
  }
  debugPrintf("jni_shim: CallStaticObjectMethod(%s)\n", nm ? nm : "?");
  nx_jval declared = nx_call_v(env, clazz, methodID, ap);
  if (declared.l) return declared.l;
  static int fake_result;
  return &fake_result;  /* fake Class/objeto nao-nulo (forName etc.) */
}
static void *jni_CallStaticObjectMethod(void *env, void *clazz, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  void *r = jni_CallStaticObjectMethodV(env, clazz, methodID, ap);
  va_end(ap);
  return r;
}

static void *jni_CallStaticObjectMethodA(void *env, void *clazz, void *methodID, const jvalue *args) {
  (void)env;
  const char *nm = mid_name(methodID);
  if (nm && !strcmp(nm, "forName") &&
      clazz == class_for("java/lang/Class"))
    return class_from_jstring(args ? args[0].l : NULL);
  /* Unity 2022 resolve AndroidJavaObject/AndroidJavaClass por ReflectionHelper.
     O Java real devolve um java.lang.reflect.Method e o player o converte com
     FromReflectedMethod. O handle persistente de reg_mid serve aos dois papéis,
     conservando nome e assinatura até a chamada JNI efetiva. */
  if (nm && !strcmp(nm, "getMethodID")) {
    const char *target = args ? class_name_for(args[0].l) : NULL;
    const char *name = args ? resolve_jstring(args[1].l) : "";
    const char *sig = args ? resolve_jstring(args[2].l) : "";
    int is_static = args ? args[3].z : 0;
    void *reflected = reg_mid(name, sig);
    if (name && !strcmp(name, "onResult")) g_onresult_mid = reflected;
    debugPrintf("jni_shim: ReflectionHelper.getMethodID(%s.%s, %s, static=%d)\n",
                target ? target : "?", name ? name : "?", sig ? sig : "?",
                is_static);
    return reflected;
  }
  if (nm && !strcmp(nm, "getConstructorID")) {
    const char *target = args ? class_name_for(args[0].l) : NULL;
    const char *sig = args ? resolve_jstring(args[1].l) : "";
    debugPrintf("jni_shim: ReflectionHelper.getConstructorID(%s, %s)\n",
                target ? target : "?", sig ? sig : "?");
    return reg_mid("<init>", sig);
  }
  if (nm && !strcmp(nm, "newInterfaceProxy")) {
    long h = args ? args[0].j : 0;
    void *interfaces = args ? args[1].l : NULL;
    void *proxy = malloc(16);
    proxy_register(proxy, h, interfaces);
    debugPrintf("jni_shim: newInterfaceProxyA(handle=%ld) -> %p\n", h, proxy);
    if (g_next_proxy_is_framecb ||
        proxy_has_interface(proxy, PROXY_IFACE_FRAME_CALLBACK)) {
      g_framecb_proxy = proxy;
      g_next_proxy_is_framecb = 0;
      debugPrintf("jni_shim: [CHOREO] FrameCallback capturado por A: proxy=%p handle=%ld\n",
                  proxy, h);
    }
    return proxy;
  }
  /* AndroidJavaProxy da API atual do Unity usa ReflectionHelper, e não o
     bitter/jnibridge usado internamente por outros proxies. */
  if (nm && !strcmp(nm, "newProxyInstance")) {
    long h = args ? args[1].j : 0;
    void *interface_class = args ? args[2].l : NULL;
    void *proxy = malloc(16);
    proxy_register_reflection(proxy, h, interface_class);
    debugPrintf("jni_shim: ReflectionHelper.newProxyInstance(handle=%ld iface=%s) -> %p\n",
                h, class_name_for(interface_class) ? class_name_for(interface_class) : "?",
                proxy);
    return proxy;
  }
  /* HelperFragment.fetchToken é o ponto Java real do plugin. No NextOS não há
     Google Play Services, mas ainda devolvemos um PendingResult e concluímos
     pelo ResultCallback normal com INTERNAL_ERROR. */
  if (nm && !strcmp(nm, "fetchToken")) {
    const char *target = class_name_for(clazz);
    if (target && !strcmp(target, "com/google/games/bridge/HelperFragment")) {
      debugPrintf("jni_shim: HelperFragment.fetchToken -> PendingResult offline\n");
      return &g_google_pending_result_sentinel;
    }
  }
  if (nm && !strcmp(nm, "getExternalStorageState"))
    return make_jstring("mounted");
  const char *static_class = class_name_for(clazz);
  if (nm && static_class && !strcmp(static_class, "android/net/Uri")) {
    if (!strcmp(nm, "encode"))
      return uri_encode_jstring(args ? args[0].l : NULL);
    if (!strcmp(nm, "decode"))
      return uri_decode_jstring(args ? args[0].l : NULL);
  }
  if (nm && !strcmp(nm, "getDevice"))
    return &g_gamepad_device;
  if (nm && !strcmp(nm, "getFieldID")) {
    const char *target = args ? class_name_for(args[0].l) : NULL;
    const char *name = args ? resolve_jstring(args[1].l) : "";
    const char *sig = args ? resolve_jstring(args[2].l) : "";
    int is_static = args ? args[3].z : 0;
    debugPrintf("jni_shim: ReflectionHelper.getFieldID(%s.%s, %s, static=%d)\n",
                target ? target : "?", name ? name : "?", sig ? sig : "?",
                is_static);
    return reg_fid(name, sig);
  }
  if (nm && !strcmp(nm, "getFieldSignature"))
    return make_jstring(args ? mid_sig(args[0].l) : "");
  debugPrintf("jni_shim: CallStaticObjectMethodA(%s)\n", nm ? nm : "?");
  nx_jval declared = nx_call_a(env, clazz, methodID, args);
  if (declared.l) return declared.l;
  static int fake_result;
  return &fake_result;
}

/* CallStaticBooleanMethod (index 124) */
static unsigned char jni_CallStaticBooleanMethod(void *env, void *clazz,
                                                 void *methodID, ...) {
  const char *nm = mid_name(methodID);
  if (nm && strcmp(nm, "hasTouchScreen") == 0) return 0;
  va_list ap;
  va_start(ap, methodID);
  unsigned char result = nx_call_v(env, clazz, methodID, ap).i ? 1 : 0;
  va_end(ap);
  return result;
}

/* CallStaticIntMethod (index 136) */
static jint cint_dispatch(void *methodID, void *arg0) {
  const char *nm = mid_name(methodID);
  if (nm && (strcmp(nm, "parseInt") == 0 || strcmp(nm, "valueOf") == 0 ||
             strcmp(nm, "intValue") == 0)) {
    const char *str = resolve_jstring(arg0);
    int v = str ? atoi(str) : 0;
    debugPrintf("jni_shim: %s(%s) -> %d\n", nm, str ? str : "?", v);
    return v;
  }
  return 0;
}
static jint jni_CallStaticIntMethod(void *env, void *clazz, void *methodID, ...) {
  va_list ap;
  va_start(ap, methodID);
  const char *nm = mid_name(methodID);
  jint result;
  if (nm && (strcmp(nm, "parseInt") == 0 || strcmp(nm, "valueOf") == 0 ||
             strcmp(nm, "intValue") == 0))
    result = cint_dispatch(methodID, va_arg(ap, void *));
  else
    result = nx_call_v(env, clazz, methodID, ap).i;
  va_end(ap);
  return result;
}
static jint jni_CallStaticIntMethodV(void *env, void *clazz, void *methodID, va_list ap) {
  const char *nm = mid_name(methodID);
  if (nm && (strcmp(nm, "parseInt") == 0 || strcmp(nm, "valueOf") == 0 ||
             strcmp(nm, "intValue") == 0))
    return cint_dispatch(methodID, va_arg(ap, void *));
  return nx_call_v(env, clazz, methodID, ap).i;
}

/* CallStaticVoidMethod (index 145) */
static void jni_CallStaticVoidMethod(void *env, void *clazz, void *methodID,
                                     ...) {
  debugPrintf("jni_shim: CallStaticVoidMethod(mid=%p)\n", methodID);
  va_list ap;
  va_start(ap, methodID);
  (void)nx_call_v(env, clazz, methodID, ap);
  va_end(ap);
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
  /* Build.VERSION.SDK_INT: 0 faz Unity 2021.3 abortar ("Unable to initialize the
     Unity Engine" — feature-level inválido). 30 = Android 11. */
  { const char *nm = mid_name(fieldID);
    if (nm && strcmp(nm, "SDK_INT") == 0) {
      debugPrintf("jni_shim: GetStaticIntField(SDK_INT) -> 30\n");
      return 30;
    } }
  debugPrintf("jni_shim: GetStaticIntField(fid=%p) -> 0\n", fieldID);
  return nx_get_field(env, clazz, fieldID).i;
}

/* GetStaticObjectField (index 156) */
static void *jni_GetStaticObjectField(void *env, void *clazz, void *fieldID) {
  (void)env;
  (void)clazz;
  const char *nm = mid_name(fieldID);
  if (fieldID == &g_current_activity_field_id ||
      (nm && strcmp(nm, "currentActivity") == 0)) {
    debugPrintf("jni_shim: GetStaticObjectField(currentActivity) -> activity\n");
    return &g_current_activity;
  }
  /* constantes String do AudioManager: devolver o NOME como valor p/ getProperty
     distinguir SAMPLE_RATE x FRAMES_PER_BUFFER */
  if (nm && (strstr(nm, "PROPERTY_") || strstr(nm, "SERVICE"))) {
    debugPrintf("jni_shim: GetStaticObjectField(%s) -> chave\n", nm);
    return make_jstring(nm);
  }
  /* android.os.Build.* — stable, device-neutral host identity. */
  if (nm) {
    if (!strcmp(nm, "MODEL")) return make_jstring("NextOS Linux Handheld");
    if (!strcmp(nm, "DEVICE")) return make_jstring("aarch64-linux");
    if (!strcmp(nm, "MANUFACTURER")) return make_jstring("NextOS Community");
    if (!strcmp(nm, "BRAND")) return make_jstring("NextOS");
    if (!strcmp(nm, "PRODUCT")) return make_jstring("huntdown-port");
    if (!strcmp(nm, "HARDWARE")) return make_jstring("linux-aarch64");
    if (!strcmp(nm, "BOARD")) return make_jstring("linux-aarch64");
    if (!strcmp(nm, "FINGERPRINT")) return make_jstring("NextOS/huntdown/aarch64:11/host/1:user/release-keys");
    if (!strcmp(nm, "RELEASE")) return make_jstring("11");
    if (!strcmp(nm, "ID")) return make_jstring("HOST");
    if (!strcmp(nm, "INCREMENTAL")) return make_jstring("1");
    if (!strcmp(nm, "TAGS")) return make_jstring("release-keys");
    if (!strcmp(nm, "TYPE")) return make_jstring("user");
    if (!strcmp(nm, "HOST")) return make_jstring("nextos");
    if (!strcmp(nm, "USER")) return make_jstring("nextos");
    if (!strcmp(nm, "SERIAL")) return make_jstring("unknown");
    if (!strcmp(nm, "DISPLAY")) return make_jstring("NextOS");
    if (!strcmp(nm, "BOOTLOADER")) return make_jstring("unknown");
    if (!strcmp(nm, "CODENAME")) return make_jstring("REL");
  }
  debugPrintf("jni_shim: GetStaticObjectField(%s) -> fake\n", nm ? nm : "?");
  static int fake;
  return &fake;
}

static size_t utf8_put_codepoint(char *out, uint32_t cp) {
  if (cp <= 0x7f) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp <= 0x7ff) {
    out[0] = (char)(0xc0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3f));
    return 2;
  }
  if (cp <= 0xffff) {
    out[0] = (char)(0xe0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[2] = (char)(0x80 | (cp & 0x3f));
    return 3;
  }
  out[0] = (char)(0xf0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
  out[3] = (char)(0x80 | (cp & 0x3f));
  return 4;
}

static uint32_t utf8_get_codepoint(const unsigned char **cursor) {
  const unsigned char *s = *cursor;
  uint32_t cp;
  size_t extra;
  if (*s < 0x80) {
    *cursor = s + 1;
    return *s;
  }
  if ((*s & 0xe0) == 0xc0) {
    cp = *s & 0x1f;
    extra = 1;
  } else if ((*s & 0xf0) == 0xe0) {
    cp = *s & 0x0f;
    extra = 2;
  } else if ((*s & 0xf8) == 0xf0) {
    cp = *s & 0x07;
    extra = 3;
  } else {
    *cursor = s + 1;
    return 0xfffd;
  }
  s++;
  for (size_t i = 0; i < extra; i++, s++) {
    if ((*s & 0xc0) != 0x80) {
      *cursor = s;
      return 0xfffd;
    }
    cp = (cp << 6) | (*s & 0x3f);
  }
  *cursor = s;
  return cp <= 0x10ffff ? cp : 0xfffd;
}

/* NewString (index 163): Unity 2022 usa esta variante UTF-16 para os nomes e
   assinaturas entregues ao ReflectionHelper. */
static void *jni_NewString(void *env, const unsigned short *unicode, jint len) {
  (void)env;
  if (!unicode || len <= 0) return make_jstring("");
  size_t cap = (size_t)len * 4 + 1;
  char *utf8 = (char *)malloc(cap);
  if (!utf8) return NULL;
  size_t n = 0;
  for (jint i = 0; i < len; i++) {
    uint32_t cp = unicode[i];
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < len) {
      uint32_t low = unicode[i + 1];
      if (low >= 0xdc00 && low <= 0xdfff) {
        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
        i++;
      } else {
        cp = 0xfffd;
      }
    } else if (cp >= 0xdc00 && cp <= 0xdfff) {
      cp = 0xfffd;
    }
    n += utf8_put_codepoint(utf8 + n, cp);
  }
  utf8[n] = '\0';
  void *result = make_jstring(utf8);
  static int log_count;
  if (log_count++ < 160)
    debugPrintf("jni_shim: NewString(UTF16) -> \"%s\"\n", utf8);
  free(utf8);
  return result;
}

static jint jni_GetStringLength(void *env, void *jstr) {
  (void)env;
  const unsigned char *p = (const unsigned char *)resolve_jstring(jstr);
  jint units = 0;
  while (*p) {
    uint32_t cp = utf8_get_codepoint(&p);
    units += cp > 0xffff ? 2 : 1;
  }
  return units;
}

static const unsigned short *jni_GetStringChars(void *env, void *jstr,
                                                 void *isCopy) {
  (void)env;
  if (isCopy) *(jboolean *)isCopy = 1;
  const unsigned char *p = (const unsigned char *)resolve_jstring(jstr);
  jint units = jni_GetStringLength(env, jstr);
  unsigned short *out =
      (unsigned short *)calloc((size_t)units + 1, sizeof(unsigned short));
  if (!out) return NULL;
  jint i = 0;
  while (*p && i < units) {
    uint32_t cp = utf8_get_codepoint(&p);
    if (cp <= 0xffff) {
      out[i++] = (unsigned short)cp;
    } else {
      cp -= 0x10000;
      out[i++] = (unsigned short)(0xd800 | (cp >> 10));
      if (i < units) out[i++] = (unsigned short)(0xdc00 | (cp & 0x3ff));
    }
  }
  return out;
}

static void jni_ReleaseStringChars(void *env, void *jstr,
                                   const unsigned short *chars) {
  (void)env;
  (void)jstr;
  free((void *)chars);
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
static void *jni_NewWeakGlobalRef(void *env, void *obj) {
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
static void jni_DeleteWeakGlobalRef(void *env, void *obj) {
  (void)env;
  (void)obj;
}
static void *jni_GetObjectClass(void *env, void *obj) {
  (void)env;
  if (is_jstring(obj)) return class_for("java/lang/String");
  if (obj == &g_current_activity)
    return class_for("com/unity3d/player/UnityPlayerActivity");
  if (obj == &g_google_token_result_sentinel)
    return class_for("com/google/games/bridge/TokenResult");
  if (obj == &g_google_pending_result_sentinel)
    return class_for("com/google/android/gms/common/api/PendingResult");
  if (obj == &g_obj_keyevent) return class_for("android/view/KeyEvent");
  if (obj == &g_long_box_sentinel) return class_for("java/lang/Long");
  if (obj == &g_message_sentinel) return class_for("android/os/Message");
  if (obj == &g_handlerthread_sentinel) return class_for("android/os/HandlerThread");
  if (obj == &g_handler_sentinel) return class_for("android/os/Handler");
  if (obj == &g_looper_sentinel) return class_for("android/os/Looper");
  if (obj == &g_display_sentinel) return class_for("android/view/Display");
  if (obj == &g_display_mode_sentinel)
    return class_for("android/view/Display$Mode");
  static int fake_obj_class;
  return &fake_obj_class;
}
static unsigned char jni_IsInstanceOf(void *env, void *obj, void *clazz) {
  (void)env;
  if (is_jstring(obj)) return clazz == class_for("java/lang/String");
  if (obj == &g_obj_keyevent) return clazz == class_for("android/view/KeyEvent");
  return 1; /* permissivo p/ outros casts */
}
static unsigned char jni_IsSameObject(void *env, void *a, void *b) {
  (void)env; return a == b;
}
/* CallLongMethod V — getEventTime/getDownTime do KeyEvent (retornam long) */
static long jni_CallLongMethodV(void *env, void *obj, void *methodID, va_list ap) {
  (void)env;
  if (obj == (void *)&g_long_box_sentinel) return g_doframe_nanos;  /* Long.longValue() do doFrame */
  const char *nm = mid_name(methodID);
  if (nm) {
    if (strcmp(nm, "getLong") == 0) {
      const char *key = resolve_jstring(va_arg(ap, void *));
      int64_t fallback = va_arg(ap, int64_t);
      return (long)prefs_get_long(key, fallback);
    }
    if (strcmp(nm, "getLongVersionCode") == 0) return (long)g_package_version_code;
    if (strcmp(nm, "getEventTime") == 0) return g_hk_inject.eventTime;
    if (strcmp(nm, "getDownTime") == 0) return g_hk_inject.downTime;
    if (strcmp(nm, "longValue") == 0) return g_doframe_nanos;
    if (obj == &g_display_sentinel &&
        strcmp(nm, "getAppVsyncOffsetNanos") == 0)
      return 1000000L;
    if (obj == &g_display_sentinel &&
        strcmp(nm, "getPresentationDeadlineNanos") == 0)
      return 16666667L;
    hd_storage_stats storage = hd_query_storage();
    if (strcmp(nm, "getAvailableBytes") == 0 ||
        strcmp(nm, "getUsableSpace") == 0)
      return (long)(storage.available_blocks * storage.block_size);
    if (strcmp(nm, "getFreeBytes") == 0 || strcmp(nm, "getFreeSpace") == 0)
      return (long)(storage.free_blocks * storage.block_size);
    if (strcmp(nm, "getTotalBytes") == 0 || strcmp(nm, "getTotalSpace") == 0)
      return (long)(storage.total_blocks * storage.block_size);
    if (strcmp(nm, "getBlockSizeLong") == 0) return (long)storage.block_size;
    if (strcmp(nm, "getAvailableBlocksLong") == 0)
      return (long)storage.available_blocks;
    if (strcmp(nm, "getFreeBlocksLong") == 0) return (long)storage.free_blocks;
    if (strcmp(nm, "getBlockCountLong") == 0) return (long)storage.total_blocks;
  }
  return (long)nx_call_v(env, obj, methodID, ap).j;
}
static long jni_CallLongMethod(void *env, void *obj, void *methodID, ...) {
  va_list ap; va_start(ap, methodID);
  long r = jni_CallLongMethodV(env, obj, methodID, ap);
  va_end(ap);
  return r;
}
static long jni_CallLongMethodA(void *env, void *obj, void *methodID,
                                const jvalue *args) {
  (void)env;
  if (obj == (void *)&g_long_box_sentinel) return g_doframe_nanos;
  const char *nm = mid_name(methodID);
  if (nm && strcmp(nm, "getLong") == 0)
    return (long)prefs_get_long(args ? resolve_jstring(args[0].l) : "",
                                args ? args[1].j : 0);
  if (nm && strcmp(nm, "getLongVersionCode") == 0)
    return (long)g_package_version_code;
  if (nm && strcmp(nm, "longValue") == 0) return g_doframe_nanos;
  if (nm && strcmp(nm, "getEventTime") == 0) return g_hk_inject.eventTime;
  if (nm && strcmp(nm, "getDownTime") == 0) return g_hk_inject.downTime;
  if (obj == &g_display_sentinel && nm &&
      strcmp(nm, "getAppVsyncOffsetNanos") == 0)
    return 1000000L;
  if (obj == &g_display_sentinel && nm &&
      strcmp(nm, "getPresentationDeadlineNanos") == 0)
    return 16666667L;
  return (long)nx_call_a(env, obj, methodID, args).j;
}

/* Exception handling is thread-local in JNI.  Unity 6 deliberately throws and
 * catches Java exceptions while probing optional Android services; reporting
 * ThrowNew success but then claiming no exception leaves those fallback paths
 * in an impossible state. */
static _Thread_local int g_pending_exception_sentinel;
static _Thread_local void *g_pending_exception;

static int jni_Throw(void *env, void *throwable) {
  (void)env;
  debugPrintf("jni_shim: Throw(%p)\n", throwable);
  g_pending_exception = throwable ? throwable : &g_pending_exception_sentinel;
  return 0;
}

static int jni_ThrowNew(void *env, void *clazz, const char *message) {
  (void)env;
  debugPrintf("jni_shim: ThrowNew(%s): %s\n",
              class_name_for(clazz) ? class_name_for(clazz) : "?",
              message ? message : "");
  g_pending_exception = &g_pending_exception_sentinel;
  return 0;
}

static unsigned char jni_ExceptionCheck(void *env) {
  (void)env;
  return g_pending_exception != NULL;
}
static void jni_ExceptionDescribe(void *env) { (void)env; }
static void jni_ExceptionClear(void *env) {
  (void)env;
  g_pending_exception = NULL;
}
static void *jni_ExceptionOccurred(void *env) {
  (void)env;
  return g_pending_exception;
}

/* Array */
static jint jni_GetArrayLength(void *env, void *array) {
  (void)env;
  if (array == (void *)&g_doframe_args_sentinel) return 1;   /* doFrame: Object[1] */
  if (array == (void *)&g_handlemsg_args_sentinel) return 1; /* handleMessage: Object[1] */
  if (array == (void *)&g_google_result_args_sentinel) return 1;
  struct oarr *oa = oarr_find(array);
  if (oa) return oa->len;
  struct barr *b = barr_find(array);
  return b ? b->len : 0;
}
static void *jni_NewObjectArray(void *env, jint len, void *clazz, void *initial) {
  (void)env; (void)clazz;
  return oarr_new(len, initial);
}
/* GetObjectArrayElement: args do doFrame -> o Long boxed (slot 0) */
static void *jni_GetObjectArrayElement(void *env, void *array, jint idx) {
  (void)env;
  if (idx < 0) return NULL;
  if (array == (void *)&g_doframe_args_sentinel)
    return idx == 0 ? &g_long_box_sentinel : NULL;
  if (array == (void *)&g_handlemsg_args_sentinel)
    return idx == 0 ? &g_message_sentinel : NULL;
  if (array == (void *)&g_google_result_args_sentinel)
    return idx == 0 ? &g_google_token_result_sentinel : NULL;
  struct oarr *oa = oarr_find(array);
  if (oa && idx < oa->len) return oa->items[idx];
  return NULL;
}
static void jni_SetObjectArrayElement(void *env, void *array, jint idx, void *value) {
  (void)env;
  struct oarr *oa = oarr_find(array);
  if (oa && idx >= 0 && idx < oa->len) oa->items[idx] = value;
}
/* int[] accessors (InputDevice IDs etc.) */
static void *jni_GetIntArrayElements(void *env, void *arr, void *isCopy) {
  (void)env; if (isCopy) *(unsigned char *)isCopy = 0;
  struct barr *b = barr_find(arr); return b ? b->buf : NULL;
}
static void jni_ReleaseIntArrayElements(void *env, void *arr, void *el, int m) {
  (void)env; (void)arr; (void)el; (void)m;
}
static void jni_GetIntArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env; struct barr *b = barr_find(arr);
  if (b && buf && start >= 0 && (start + len) * 4 <= (b->len * 4 > 0 ? b->len * 4 : 0) + 4)
    memcpy(buf, b->buf + start * 4, len * 4);
}
static void *jni_NewIntArray(void *env, int len) { (void)env; return iarr_new(NULL, len); }
static void *jni_NewLongArray(void *env, int len) { (void)env; return larr_new(NULL, len); }
static void *jni_GetLongArrayElements(void *env, void *arr, void *isCopy) {
  (void)env; if (isCopy) *(unsigned char *)isCopy = 0;
  struct barr *b = barr_find(arr); return b ? b->buf : NULL;
}
static void jni_ReleaseLongArrayElements(void *env, void *arr, void *el, int m) {
  (void)env; (void)arr; (void)el; (void)m;
}
static void jni_GetLongArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env; struct barr *b = barr_find(arr);
  if (b && buf && start >= 0 && len >= 0 && start + len <= b->len)
    memcpy(buf, b->buf + (size_t)start * sizeof(int64_t),
           (size_t)len * sizeof(int64_t));
}
static void jni_SetLongArrayRegion(void *env, void *arr, int start, int len,
                                   const void *buf) {
  (void)env; struct barr *b = barr_find(arr);
  if (b && buf && start >= 0 && len >= 0 && start + len <= b->len)
    memcpy(b->buf + (size_t)start * sizeof(int64_t), buf,
           (size_t)len * sizeof(int64_t));
}
static void *jni_NewBooleanArray(void *env, int len) { (void)env; return boolarr_new(len); }
static void *jni_GetBooleanArrayElements(void *env, void *arr, void *isCopy) {
  (void)env; if (isCopy) *(unsigned char *)isCopy = 0;
  struct barr *b = barr_find(arr); return b ? b->buf : NULL;
}
static void jni_ReleaseBooleanArrayElements(void *env, void *arr, void *el, int m) {
  (void)env; (void)arr; (void)el; (void)m;
}
static void jni_GetBooleanArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env; struct barr *b = barr_find(arr);
  if (b && buf && start >= 0 && len >= 0 && start + len <= b->len)
    memcpy(buf, b->buf + start, len);
}
static void jni_SetBooleanArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env; struct barr *b = barr_find(arr);
  if (b && buf && start >= 0 && len >= 0 && start + len <= b->len)
    memcpy(b->buf + start, buf, len);
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
  /* GetEnv e' chamado milhares de vezes (cada thread/icall) -> silenciado */
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

/* ---- recon: RegisterNatives com log + STORAGE dos ponteiros ---- */
struct native_method { const char *name; const char *sig; void *fn; };
static struct native_method g_natives[512];
static int g_natives_count = 0;

void *jni_find_native(const char *name) {
  for (int i = 0; i < g_natives_count; i++)
    if (strcmp(g_natives[i].name, name) == 0) return g_natives[i].fn;
  return 0;
}

static void *jni_find_native_sig(const char *name, const char *sig) {
  for (int i = 0; i < g_natives_count; i++)
    if (strcmp(g_natives[i].name, name) == 0 &&
        strcmp(g_natives[i].sig, sig) == 0)
      return g_natives[i].fn;
  return NULL;
}

static int jni_RegisterNatives(void *env, void *clazz, const void *methods, int n) {
  (void)env; (void)clazz;
  debugPrintf("jni_shim: >> RegisterNatives(%d metodos)\n", n);
  const uintptr_t *m = (const uintptr_t *)methods;  /* {name, sig, fnPtr} x n */
  for (int i = 0; i < n && i < 128; i++) {
    const char *nm = (const char *)m[i * 3];
    const char *sg = (const char *)m[i * 3 + 1];
    void *fn = (void *)m[i * 3 + 2];
    debugPrintf("     [%d] %s %s  -> %p\n", i, nm ? nm : "?", sg ? sg : "?", fn);
    if (nm && g_natives_count < 512) {
      g_natives[g_natives_count].name = strdup(nm);  /* COPIA: nomes do jnibridge
        (invoke/delete) são transientes → ponteiro dangle → jni_find_native falhava */
      g_natives[g_natives_count].sig = strdup(sg ? sg : "");
      g_natives[g_natives_count].fn = fn;
      g_natives_count++;
    }
  }
  return 0;
}

/* O DEX original chama nSetSupportedRefreshPeriods de dentro do construtor de
 * SwappyDisplayManager. Como nao executamos bytecode Java, reproduzimos aqui
 * exatamente esse lado do construtor com o unico modo fisico do fbdev. Isso
 * mantem o cookie e a entrada nativa oficiais do Android Game SDK. */
static void *swappy_display_manager_construct(void *env, int64_t cookie,
                                              void *activity) {
  (void)activity;
  const int64_t period_ns = 16666667LL; /* (long)(1.0E9f / 60.0f), como no DEX */
  const int mode_id = 1;
  void *periods = larr_new(&period_ns, 1);
  void *mode_ids = iarr_new(&mode_id, 1);
  void *fn = jni_find_native_sig("nSetSupportedRefreshPeriods", "(J[J[I)V");

  fprintf(stderr,
          "[SWAPPY-DISPLAY] construtor cookie=%lld periodo=%lld modo=%d fn=%p\n",
          (long long)cookie, (long long)period_ns, mode_id, fn);
  fflush(stderr);
  if (fn && periods && mode_ids) {
    typedef void (*set_periods_fn)(void *, void *, int64_t, void *, void *);
    ((set_periods_fn)fn)(env, &g_swappy_display_manager_sentinel, cookie,
                        periods, mode_ids);
  }
  return &g_swappy_display_manager_sentinel;
}

/* ---- jnibridge proxy: execute Runnables posted through runOnUiThread/post ----
 * Store proxy->handle and invoke the managed delegate when the emulated Android
 * looper processes it; otherwise the native engine would wait forever. */
static void proxy_register(void *obj, long h, void *interfaces) {
  if (g_proxy_n >= 512) return;
  unsigned flags = 0;
  struct oarr *oa = oarr_find(interfaces);
  if (oa) {
    for (int i = 0; i < oa->len; i++) {
      const char *name = class_name_for(oa->items[i]);
      if (!name) continue;
      if (strcmp(name, "android/view/Choreographer$FrameCallback") == 0)
        flags |= PROXY_IFACE_FRAME_CALLBACK;
      else if (strcmp(name, "com/google/android/gms/common/api/ResultCallback") == 0)
        flags |= PROXY_IFACE_RESULT_CALLBACK;
      else if (strcmp(name, "java/lang/Runnable") == 0)
        flags |= PROXY_IFACE_RUNNABLE;
    }
  } else {
    const char *name = class_name_for(interfaces);
    if (name) {
      if (strcmp(name, "android/view/Choreographer$FrameCallback") == 0)
        flags |= PROXY_IFACE_FRAME_CALLBACK;
      else if (strcmp(name, "com/google/android/gms/common/api/ResultCallback") == 0)
        flags |= PROXY_IFACE_RESULT_CALLBACK;
      else if (strcmp(name, "java/lang/Runnable") == 0)
        flags |= PROXY_IFACE_RUNNABLE;
    }
  }
  if (g_next_proxy_is_framecb) flags |= PROXY_IFACE_FRAME_CALLBACK;
  if (g_next_proxy_is_resultcb) flags |= PROXY_IFACE_RESULT_CALLBACK;
  g_next_proxy_is_resultcb = 0;
  g_proxies[g_proxy_n].obj = obj;
  g_proxies[g_proxy_n].handle = h;
  g_proxies[g_proxy_n].interfaces = flags;
  g_proxy_n++;
}
static void proxy_register_reflection(void *obj, long h, void *interface_class) {
  proxy_register(obj, h, interface_class);
  for (int i = g_proxy_n - 1; i >= 0; i--) {
    if (g_proxies[i].obj == obj) {
      g_proxies[i].interfaces |= PROXY_BRIDGE_REFLECTION;
      return;
    }
  }
}
static long proxy_handle(void *obj) {
  for (int i = g_proxy_n - 1; i >= 0; i--) if (g_proxies[i].obj == obj) return g_proxies[i].handle;
  return 0;
}
static int proxy_has_interface(void *obj, unsigned interface_flag) {
  for (int i = g_proxy_n - 1; i >= 0; i--)
    if (g_proxies[i].obj == obj)
      return (g_proxies[i].interfaces & interface_flag) != 0;
  return 0;
}
static _Thread_local int g_in_run;
int jni_is_run_method(void *o) { return o == (void *)&g_run_method_sentinel; }
int jni_is_empty_args(void *o) { return o == (void *)&g_empty_args_sentinel; }
static void run_runnable(void *env, void *runnable) {
  if (!runnable) return;
  if (g_in_run >= 6) { debugPrintf("jni_shim: runOnUiThread anti-recursao\n"); return; }
  long h = proxy_handle(runnable);
  int reflection = proxy_has_interface(runnable, PROXY_BRIDGE_REFLECTION);
  void *invoke = jni_find_native(reflection ? "nativeProxyInvoke" : "invoke");
  void *method_class = class_for("java/lang/Runnable");
  if (!h || !invoke) { debugPrintf("jni_shim: runOnUiThread sem handle/invoke (r=%p h=%ld invoke=%p natives=%d)\n", runnable, h, invoke, g_natives_count); return; }
  /* Android dispatches this proxy on the Activity/UI thread.  The host has no
   * Java VM and invokes it inline, so preserve the caller's thread-local JNI
   * exception state explicitly: a NoSuchMethodError raised by an optional UI
   * callback must not leak into UnityMain and abort NativeLoader's subsequent
   * findLibrary("il2cpp") probe. */
  void *caller_exception = g_pending_exception;
  g_pending_exception = NULL;
  g_in_run++;
  debugPrintf("jni_shim: >> RODANDO Runnable (handle=%ld bridge=%s) ...\n",
              h, reflection ? "ReflectionHelper" : "JNIBridge");
  if (reflection) {
    void *reflection_class = class_for("com/unity3d/player/ReflectionHelper");
    ((void *(*)(void *, void *, long, void *, void *))invoke)(
        env, reflection_class, h, make_jstring("run"),
        &g_empty_args_sentinel);
  } else {
    ((void *(*)(void *, void *, long, void *, void *, void *))invoke)(
        env, method_class, h, method_class,
        &g_run_method_sentinel, &g_empty_args_sentinel);
  }
  if (g_pending_exception)
    debugPrintf("jni_shim: excecao do Runnable isolada da UnityMain\n");
  g_pending_exception = caller_exception;
  debugPrintf("jni_shim: << Runnable terminou (handle=%ld)\n", h);
  g_in_run--;
}

/* ---- Choreographer: dispara FrameCallback.doFrame(frameTimeNanos) ----
 * Mesmo caminho do run_runnable, mas com o Method "doFrame" + args = Object[1]{Long}.
 * Chamado pela driver-thread (main.c) ~60Hz. Retorna 1 se disparou, 0 se ainda não há
 * FrameCallback capturado. g_choreo_log liga log detalhado das queries (1ª vez). */
int g_choreo_log = 0;
int jni_choreo_doframe(void *env, long nanos) {
  void *proxy = g_framecb_proxy;
  if (!proxy) return 0;
  long h = proxy_handle(proxy);
  void *invoke = jni_find_native("invoke");
  void *method_class = class_for("android/view/Choreographer$FrameCallback");
  if (!h || !invoke) return 0;
  g_doframe_nanos = nanos;
  static int once = 0;
  if (g_choreo_log && !once) { once = 1; debugPrintf("jni_shim: [CHOREO] 1º doFrame(handle=%ld nanos=%ld)\n", h, nanos); }
  ((void *(*)(void *, void *, long, void *, void *, void *))invoke)(
      env, method_class, h, method_class,
      &g_doframe_method_sentinel, &g_doframe_args_sentinel);
  return 1;
}
void *jni_shim_env(void) { return jni_env_ptr; }
int jni_choreo_captured(void) { return g_framecb_proxy != NULL; }

/* ---- Handler$Callback.handleMessage(Message): invoca o delegate C# do proxy ----
 * Mesmo caminho do doFrame, com Method "handleMessage" + args Object[1]{Message}. O proxy
 * (g_framecb_proxy) implementa Handler$Callback E Choreographer$FrameCallback (mesmo handle). */
void jni_handlemessage(void *env) {
  void *proxy = g_framecb_proxy;
  if (!proxy) { debugPrintf("jni_shim: handleMessage sem proxy capturado\n"); return; }
  long h = proxy_handle(proxy);
  void *invoke = jni_find_native("invoke");
  void *method_class = class_for("android/os/Handler$Callback");
  if (!h || !invoke) { debugPrintf("jni_shim: handleMessage sem handle/invoke (h=%ld invoke=%p)\n", h, invoke); return; }
  debugPrintf("jni_shim: >> handleMessage(what=%d handle=%ld) ...\n", g_message_what, h);
  ((void *(*)(void *, void *, long, void *, void *, void *))invoke)(
      env, method_class, h, method_class,
      &g_handlemsg_method_sentinel, &g_handlemsg_args_sentinel);
  debugPrintf("jni_shim: << handleMessage terminou\n");
}

void jni_queue_handler_message(void) {
  __atomic_store_n(&g_message_pending, 1, __ATOMIC_RELEASE);
  debugPrintf("jni_shim: Message enfileirada para HandlerThread\n");
}

int jni_take_handler_message(void) {
  return __atomic_exchange_n(&g_message_pending, 0, __ATOMIC_ACQ_REL);
}

/* Chamado pela UnityMain depois de nativeRender retornar. Assim o resultado do
 * PendingResult não reentra no frame que registrou o callback e conserva a
 * ordem Android: fetchToken -> setResultCallback -> onResult. */
void jni_poll_java_callbacks(void *env) {
  if (!__atomic_exchange_n(&g_google_result_pending, 0, __ATOMIC_ACQ_REL)) return;
  void *proxy = g_google_result_proxy;
  long h = proxy_handle(proxy);
  if (proxy_has_interface(proxy, PROXY_BRIDGE_REFLECTION)) {
    void *invoke = jni_find_native("nativeProxyInvoke");
    void *reflection_class = class_for("com/unity3d/player/ReflectionHelper");
    if (!proxy || !h || !invoke) {
      debugPrintf("jni_shim: Google nativeProxyInvoke sem proxy/handle/fn (%p/%ld/%p)\n",
                  proxy, h, invoke);
      return;
    }
    debugPrintf("jni_shim: >> Google ResultCallback.onResult(INTERNAL_ERROR) via ReflectionHelper, handle=%ld\n",
                h);
    ((void *(*)(void *, void *, long, void *, void *))invoke)(
        env, reflection_class, h, make_jstring("onResult"),
        &g_google_result_args_sentinel);
    debugPrintf("jni_shim: << Google ResultCallback.onResult terminou\n");
    return;
  }
  void *invoke = jni_find_native("invoke");
  void *method_class = class_for("com/google/android/gms/common/api/ResultCallback");
  if (!proxy || !h || !invoke) {
    debugPrintf("jni_shim: Google onResult sem proxy/handle/invoke (%p/%ld/%p)\n",
                proxy, h, invoke);
    return;
  }
  debugPrintf("jni_shim: >> Google ResultCallback.onResult(INTERNAL_ERROR), handle=%ld\n", h);
  ((void *(*)(void *, void *, long, void *, void *, void *))invoke)(
      env, method_class, h, method_class,
      &g_onresult_method_sentinel, &g_google_result_args_sentinel);
  debugPrintf("jni_shim: << Google ResultCallback.onResult terminou\n");
}

/* ---- DirectByteBuffer p/ a thread de áudio do FMOD (AudioTrack Java) ----
   fmodProcess(env, thiz, ByteBuffer) faz GetDirectBufferAddress/Capacity no buffer
   p/ saber onde escrever o PCM. Damos um buffer real nosso. */
static unsigned char g_fmod_pcm[32768];
static int g_fmod_bb_sentinel;
/* 🔑 CAPACIDADE do DirectByteBuffer = nº de bytes que fmodProcess PREENCHE por chamada
   (FMOD enche o buffer inteiro e AVANÇA o clock do mixer nesse tanto de frames). ISSO PRECISA
   CASAR com os bytes que o pump (fmod_audio_thread) enfileira no SDL. Antes reportávamos 32768
   (8192 frames) mas o pump só enfileirava 4096 (1024 frames) -> o FMOD avançava 8x mais rápido
   que o playback -> ÁUDIO ACELERADO. 4096 = bloco DSP padrão do FMOD mobile (1024 frames
   stereo s16). Tunável por HD_AUDIO_BUF (bytes). O backing g_fmod_pcm[32768] é só folga. */
int g_fmod_cap = 32768;   /* capacidade reportada = backing inteiro (fmodProcess mixa blockSize
                             frames independente disto; só precisa de FOLGA p/ não estourar) */
void *jni_fmod_bytebuffer(void) { return &g_fmod_bb_sentinel; }
void *jni_fmod_pcm(void) { return g_fmod_pcm; }
int jni_fmod_pcm_size(void) { return g_fmod_cap; }
void jni_fmod_set_pcm_size(int bytes) {
  if (bytes >= 256 && bytes <= (int)sizeof(g_fmod_pcm))
    g_fmod_cap = bytes;
}
int jni_fmod_should_run(void) {
  return __atomic_load_n(&g_fmod_should_run, __ATOMIC_ACQUIRE);
}
static void *jni_GetDirectBufferAddress(void *env, void *buf) {
  (void)env; if (buf == &g_fmod_bb_sentinel) return g_fmod_pcm; return NULL;
}
static long jni_GetDirectBufferCapacity(void *env, void *buf) {
  (void)env; if (buf == &g_fmod_bb_sentinel) return (long)g_fmod_cap; return -1;
}
/* org.fmod.FMODAudioDevice — FMOD faz NewObject(FMODAudioDevice) e chama start() p/ subir o
   AudioTrack. Sem isso (NewObject→NULL via jni_stub) o System::init do FMOD dá erro 60. Damos
   um device fake não-nulo + métodos (start/etc.) OK; a thread C (fmod_audio_thread) bombeia
   fmodProcess no lugar da thread Java. */
void *jni_fmod_device(void) { return &g_fmod_device_obj; }
static void *jni_string_from_byte_array(void *array) {
  struct barr *bytes = barr_find(array);
  if (!bytes || bytes->len <= 0) return make_jstring("");
  char *text = (char *)malloc((size_t)bytes->len + 1);
  if (!text) return NULL;
  memcpy(text, bytes->buf, (size_t)bytes->len);
  text[bytes->len] = '\0';
  void *result = make_jstring(text);
  free(text);
  return result;
}
static void *jni_NewObjectCommon(void *env, void *clazz, void *mid) {
  (void)env;
  const char *nm = mid_name(mid);
  if (clazz == class_for("org/fmod/FMODAudioDevice")) {
    debugPrintf("jni_shim: NewObject(FMODAudioDevice) -> device fake\n"); return &g_fmod_device_obj;
  }
  if (clazz == class_for("android/os/HandlerThread")) {
    debugPrintf("jni_shim: NewObject(HandlerThread/%s) -> %p\n",
                nm ? nm : "?", (void *)&g_handlerthread_sentinel);
    return &g_handlerthread_sentinel;
  }
  if (clazz == class_for("android/os/Handler")) {
    debugPrintf("jni_shim: NewObject(Handler/%s) -> %p\n",
                nm ? nm : "?", (void *)&g_handler_sentinel);
    return &g_handler_sentinel;
  }
  if (clazz == class_for("android/os/Message"))
    return &g_message_sentinel;
  return NULL;
}
static void *jni_NewObjectV(void *env, void *clazz, void *mid, va_list ap) {
  const char *class_name = class_name_for(clazz);
  if (class_name && strcmp(class_name, "java/lang/String") == 0) {
    void *array = va_arg(ap, void *);
    (void)va_arg(ap, void *); /* charset ("UTF-8") */
    return jni_string_from_byte_array(array);
  }
  if (class_name &&
      strcmp(class_name, "com/google/androidgamesdk/SwappyDisplayManager") == 0 &&
      mid_name(mid) && strcmp(mid_name(mid), "<init>") == 0 &&
      mid_sig(mid) && strcmp(mid_sig(mid), "(JLandroid/app/Activity;)V") == 0) {
    int64_t cookie = va_arg(ap, int64_t);
    void *activity = va_arg(ap, void *);
    return swappy_display_manager_construct(env, cookie, activity);
  }
  return jni_NewObjectCommon(env, clazz, mid);
}
static void *jni_NewObject(void *env, void *clazz, void *mid, ...) {
  va_list ap;
  va_start(ap, mid);
  void *result = jni_NewObjectV(env, clazz, mid, ap);
  va_end(ap);
  return result;
}
static void *jni_NewObjectA(void *env, void *clazz, void *mid, const jvalue *args) {
  const char *class_name = class_name_for(clazz);
  if (class_name && strcmp(class_name, "java/lang/String") == 0)
    return jni_string_from_byte_array(args ? args[0].l : NULL);
  if (class_name &&
      strcmp(class_name, "com/google/androidgamesdk/SwappyDisplayManager") == 0 &&
      mid_name(mid) && strcmp(mid_name(mid), "<init>") == 0 &&
      mid_sig(mid) && strcmp(mid_sig(mid), "(JLandroid/app/Activity;)V") == 0)
    return swappy_display_manager_construct(env, args ? args[0].j : 0,
                                            args ? args[1].l : NULL);
  return jni_NewObjectCommon(env, clazz, mid);
}

/* GetJavaVM (index 219) — initJni chama isso */
static jint jni_GetJavaVM(void *env, void **vm) {
  (void)env;
  debugPrintf("jni_shim: GetJavaVM -> nossa VM\n");
  *vm = &java_vm_ptr;   /* mesma JavaVM passada no out_vm */
  return 0;
}

/* ---- Init ---- */

void jni_install_indexed(uintptr_t *vt, int n);

void jni_shim_init(void **out_vm, void **out_env) {
  prefs_load();
  if (getenv("HD_AUDIO_BUF")) { int v = atoi(getenv("HD_AUDIO_BUF"));
    if (v >= 512 && v <= 32768) g_fmod_cap = v & ~3; }   /* casa capacidade do BB com o que o pump enfileira */
  for (int i = 0; i < JNI_VTABLE_SIZE; i++) {
    jni_env_vtable[i] = (uintptr_t)jni_stub;
    java_vm_vtable[i] = (uintptr_t)jni_stub;
  }
  jni_install_indexed(jni_env_vtable, JNI_VTABLE_SIZE);

  /*
   * JNIEnv vtable indices from Android NDK jni.h.
   * C++ wrappers in the .so call the *V (va_list) variants,
   * so we must set both the variadic and V slots.
   *
   *   0-3:   reserved
   *   4:     GetVersion
   *   6:     FindClass
   *   7/8:   FromReflectedMethod / FromReflectedField
   *  15:     ExceptionOccurred
   *  17:     ExceptionClear
   *  21:     NewGlobalRef
   *  22:     DeleteGlobalRef
   *  23:     DeleteLocalRef
   *  25:     NewLocalRef
   *  31:     GetObjectClass
   *  33:     GetMethodID
   *  34/35/36: CallObjectMethod / V / A
   *  37/38:  CallBooleanMethod / V
   *  49/50:  CallIntMethod / V
   *  61/62/63: CallVoidMethod / V / A
   *  94/95:  GetFieldID / GetObjectField
   * 113:     GetStaticMethodID
   * 114/115/116: CallStaticObjectMethod / V / A
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
  jni_env_vtable[7] = (uintptr_t)jni_FromReflectedMethod;
  jni_env_vtable[8] = (uintptr_t)jni_FromReflectedField;
  jni_env_vtable[9] = (uintptr_t)jni_ToReflectedMethod;
  jni_env_vtable[215] = (uintptr_t)jni_RegisterNatives;  /* recon: Unity */
  jni_env_vtable[219] = (uintptr_t)jni_GetJavaVM;        /* recon: Unity initJni */
  /* AssetManager bridge: byte-array functions */
  jni_env_vtable[171] = (uintptr_t)jni_GetArrayLength_real;
  jni_env_vtable[175] = (uintptr_t)jni_NewBooleanArray;
  jni_env_vtable[176] = (uintptr_t)jni_NewByteArray;
  jni_env_vtable[183] = (uintptr_t)jni_GetBooleanArrayElements;
  jni_env_vtable[184] = (uintptr_t)jni_GetByteArrayElements;
  jni_env_vtable[191] = (uintptr_t)jni_ReleaseBooleanArrayElements;
  jni_env_vtable[192] = (uintptr_t)jni_ReleaseByteArrayElements;
  jni_env_vtable[199] = (uintptr_t)jni_GetBooleanArrayRegion;
  jni_env_vtable[200] = (uintptr_t)jni_GetByteArrayRegion;
  jni_env_vtable[207] = (uintptr_t)jni_SetBooleanArrayRegion;
  jni_env_vtable[208] = (uintptr_t)jni_SetByteArrayRegion;
  jni_env_vtable[13] = (uintptr_t)jni_Throw;
  jni_env_vtable[14] = (uintptr_t)jni_ThrowNew;
  jni_env_vtable[15] = (uintptr_t)jni_ExceptionOccurred;
  jni_env_vtable[16] = (uintptr_t)jni_ExceptionDescribe;
  jni_env_vtable[17] = (uintptr_t)jni_ExceptionClear;
  jni_env_vtable[21] = (uintptr_t)jni_NewGlobalRef;
  jni_env_vtable[22] = (uintptr_t)jni_DeleteGlobalRef;
  jni_env_vtable[23] = (uintptr_t)jni_DeleteLocalRef;
  jni_env_vtable[25] = (uintptr_t)jni_NewLocalRef;
  jni_env_vtable[24] = (uintptr_t)jni_IsSameObject;
  jni_env_vtable[28] = (uintptr_t)jni_NewObject;    /* NewObject (varargs) — FMODAudioDevice */
  jni_env_vtable[29] = (uintptr_t)jni_NewObjectV;   /* NewObjectV (va_list) */
  jni_env_vtable[30] = (uintptr_t)jni_NewObjectA;   /* NewObjectA (jvalue*) */
  jni_env_vtable[31] = (uintptr_t)jni_GetObjectClass;
  jni_env_vtable[32] = (uintptr_t)jni_IsInstanceOf;
  jni_env_vtable[33] = (uintptr_t)jni_GetMethodID;
  jni_env_vtable[52] = (uintptr_t)jni_CallLongMethod;
  jni_env_vtable[53] = (uintptr_t)jni_CallLongMethodV;
  jni_env_vtable[54] = (uintptr_t)jni_CallLongMethodA;
  jni_env_vtable[34] = (uintptr_t)jni_CallObjectMethod;
  jni_env_vtable[35] = (uintptr_t)jni_CallObjectMethodV;   /* V variant (va_list) */
  jni_env_vtable[36] = (uintptr_t)jni_CallObjectMethodA;   /* A variant (jvalue*) */
  jni_env_vtable[37] = (uintptr_t)jni_CallBooleanMethod;
  jni_env_vtable[38] = (uintptr_t)jni_CallBooleanMethodV;  /* V (va_list) */
  jni_env_vtable[39] = (uintptr_t)jni_CallBooleanMethodA;  /* A (jvalue*) */
  jni_env_vtable[49] = (uintptr_t)jni_CallIntMethod;
  jni_env_vtable[50] = (uintptr_t)jni_CallIntMethodV;      /* V (va_list) */
  jni_env_vtable[51] = (uintptr_t)jni_CallIntMethodA;      /* A (jvalue*) */
  jni_env_vtable[55] = (uintptr_t)jni_CallFloatMethod;     /* getRefreshRate */
  jni_env_vtable[56] = (uintptr_t)jni_CallFloatMethodV;    /* V */
  jni_env_vtable[57] = (uintptr_t)jni_CallFloatMethodA;    /* A */
  jni_env_vtable[100] = (uintptr_t)jni_GetIntField;        /* DisplayMetrics int fields */
  jni_env_vtable[102] = (uintptr_t)jni_GetFloatField;      /* DisplayMetrics float fields */
  jni_env_vtable[61] = (uintptr_t)jni_CallVoidMethod;
  jni_env_vtable[62] = (uintptr_t)jni_CallVoidMethodV;     /* V (va_list) */
  jni_env_vtable[63] = (uintptr_t)jni_CallVoidMethodA;     /* A (jvalue*) */
  jni_env_vtable[94] = (uintptr_t)jni_GetFieldID;
  jni_env_vtable[95] = (uintptr_t)jni_GetObjectField;
  jni_env_vtable[113] = (uintptr_t)jni_GetStaticMethodID;
  jni_env_vtable[114] = (uintptr_t)jni_CallStaticObjectMethod;
  jni_env_vtable[115] = (uintptr_t)jni_CallStaticObjectMethodV; /* V (va_list) */
  jni_env_vtable[116] = (uintptr_t)jni_CallStaticObjectMethodA; /* A (jvalue*) */
  jni_env_vtable[117] = (uintptr_t)jni_CallStaticBooleanMethod;
  jni_env_vtable[118] = (uintptr_t)jni_CallStaticBooleanMethod; /* V */
  jni_env_vtable[129] = (uintptr_t)jni_CallStaticIntMethod;
  jni_env_vtable[130] = (uintptr_t)jni_CallStaticIntMethodV; /* V (va_list) */
  jni_env_vtable[141] = (uintptr_t)jni_CallStaticVoidMethod;
  jni_env_vtable[142] = (uintptr_t)jni_CallStaticVoidMethod; /* V */
  jni_env_vtable[144] = (uintptr_t)jni_GetStaticFieldID;
  jni_env_vtable[145] = (uintptr_t)jni_GetStaticObjectField;
  jni_env_vtable[150] = (uintptr_t)jni_GetStaticIntField;
  jni_env_vtable[163] = (uintptr_t)jni_NewString;
  jni_env_vtable[164] = (uintptr_t)jni_GetStringLength;
  jni_env_vtable[165] = (uintptr_t)jni_GetStringChars;
  jni_env_vtable[166] = (uintptr_t)jni_ReleaseStringChars;
  jni_env_vtable[167] = (uintptr_t)jni_NewStringUTF;
  jni_env_vtable[168] = (uintptr_t)jni_GetStringUTFLength;
  jni_env_vtable[169] = (uintptr_t)jni_GetStringUTFChars;
  jni_env_vtable[170] = (uintptr_t)jni_ReleaseStringUTFChars;
  jni_env_vtable[171] = (uintptr_t)jni_GetArrayLength;
  jni_env_vtable[172] = (uintptr_t)jni_NewObjectArray;
  jni_env_vtable[173] = (uintptr_t)jni_GetObjectArrayElement; /* doFrame args[0]=Long */
  jni_env_vtable[174] = (uintptr_t)jni_SetObjectArrayElement;
  jni_env_vtable[175] = (uintptr_t)jni_NewBooleanArray;       /* NewBooleanArray */
  jni_env_vtable[179] = (uintptr_t)jni_NewIntArray;          /* NewIntArray */
  jni_env_vtable[180] = (uintptr_t)jni_NewLongArray;         /* NewLongArray */
  jni_env_vtable[183] = (uintptr_t)jni_GetBooleanArrayElements;
  jni_env_vtable[187] = (uintptr_t)jni_GetIntArrayElements;  /* int[] elements */
  jni_env_vtable[188] = (uintptr_t)jni_GetLongArrayElements; /* long[] elements */
  jni_env_vtable[191] = (uintptr_t)jni_ReleaseBooleanArrayElements;
  jni_env_vtable[195] = (uintptr_t)jni_ReleaseIntArrayElements;
  jni_env_vtable[196] = (uintptr_t)jni_ReleaseLongArrayElements;
  jni_env_vtable[199] = (uintptr_t)jni_GetBooleanArrayRegion;
  jni_env_vtable[203] = (uintptr_t)jni_GetIntArrayRegion;
  jni_env_vtable[204] = (uintptr_t)jni_GetLongArrayRegion;
  jni_env_vtable[207] = (uintptr_t)jni_SetBooleanArrayRegion;
  jni_env_vtable[212] = (uintptr_t)jni_SetLongArrayRegion;
  jni_env_vtable[205] = (uintptr_t)jni_ExceptionCheck;
  jni_env_vtable[226] = (uintptr_t)jni_NewWeakGlobalRef;
  jni_env_vtable[227] = (uintptr_t)jni_DeleteWeakGlobalRef;
  jni_env_vtable[230] = (uintptr_t)jni_GetDirectBufferAddress;
  jni_env_vtable[231] = (uintptr_t)jni_GetDirectBufferCapacity;

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

/* lista todos os métodos nativos registrados via RegisterNatives (debug F1) */
extern void jni_dump_natives(void);
void jni_dump_natives(void) {
  fprintf(stderr, "[NATIVES] %d métodos registrados:\n", g_natives_count);
  for (int i = 0; i < g_natives_count; i++)
    fprintf(stderr, "  %s %s = %p\n", g_natives[i].name,
            g_natives[i].sig ? g_natives[i].sig : "", g_natives[i].fn);
}
