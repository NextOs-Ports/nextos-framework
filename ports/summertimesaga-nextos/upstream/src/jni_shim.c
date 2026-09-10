// JNI shim for Summertime Saga/Ren'Py (SDL2 statically linked into librenpython.so).
// Provides a fake JavaVM + JNIEnv whose function table satisfies the JNI
// upcalls SDL's android backend makes (getNativeSurface, getContext,
// isAndroidTV, getManifestEnvironmentVariables, audioOpen, ...).
#include "jni_shim.h"
#include "so_util.h"
#include "util.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define JNI_VTABLE_SIZE 512

typedef int jint;
typedef unsigned char jboolean;
typedef union {
  jboolean z;
  signed char b;
  unsigned short c;
  short s;
  jint i;
  long long j;
  float f;
  double d;
  void *l;
} jvalue;

static uintptr_t jni_env_vtable[JNI_VTABLE_SIZE];
static void *jni_env_ptr;

static uintptr_t java_vm_vtable[JNI_VTABLE_SIZE];
static void *java_vm_ptr;

enum {
  MID_GENERIC = 0,
  MID_GET_NATIVE_SURFACE,
  MID_GET_CONTEXT,
  MID_IS_ANDROID_TV,
  MID_GET_MANIFEST_ENV,
  MID_AUDIO_OPEN,
  MID_AUDIO_WRITE_SHORT,
  MID_AUDIO_WRITE_BYTE,
  MID_AUDIO_WRITE_FLOAT,
  MID_AUDIO_CLOSE,
  MID_COUNT
};
static int g_method_tags[MID_COUNT];

static int g_dummy_array[4096];

// Real array registry (for the SDL audio buffer: NewShortArray ->
// GetShortArrayElements -> write -> audioWriteShortBuffer). Handle == data ptr.
#define MAX_ARRAYS 16
static struct {
  void *data;
  int len_bytes;
  int elem_size;
} g_arrays[MAX_ARRAYS];
static int g_array_count;

static void *reg_array(int count, int elem_size) {
  void *data = calloc(count > 0 ? count : 1, elem_size);
  int idx = g_array_count;
  if (idx >= MAX_ARRAYS) {
    // reuse slot 0..; the audio buffer is allocated once, so this is rare
    idx = 0;
    free(g_arrays[0].data);
  } else {
    g_array_count++;
  }
  g_arrays[idx].data = data;
  g_arrays[idx].len_bytes = count * elem_size;
  g_arrays[idx].elem_size = elem_size;
  return data;
}

static int find_array(void *handle, int *len_bytes) {
  for (int i = 0; i < g_array_count; i++) {
    if (g_arrays[i].data == handle) {
      if (len_bytes)
        *len_bytes = g_arrays[i].len_bytes;
      return g_arrays[i].elem_size;
    }
  }
  return 0;
}

#define MAX_JSTRINGS 128
static struct {
  void *handle;
  const char *value;
} g_jstrings[MAX_JSTRINGS];
static int g_jstring_count = 0;

static void *make_jstring(const char *value) {
  if (g_jstring_count >= MAX_JSTRINGS)
    g_jstring_count = 0;
  int idx = g_jstring_count++;
  g_jstrings[idx].handle = (void *)((uintptr_t)0x10000 + idx);
  g_jstrings[idx].value = value;
  return g_jstrings[idx].handle;
}

void *jni_shim_make_jstring(const char *s) { return make_jstring(s ? s : ""); }

static const char *resolve_jstring(void *jstr) {
  for (int i = 0; i < g_jstring_count; i++)
    if (g_jstrings[i].handle == jstr)
      return g_jstrings[i].value;
  return "";
}

static intptr_t jni_stub(void) { return 0; }

static jint jni_GetVersion(void *env) {
  (void)env;
  return 0x00010006;
}

static void *jni_FindClass(void *env, const char *name) {
  (void)env;
  if (getenv("SUMMERTIME_VERBOSE"))
    debugPrintf("jni: FindClass(%s)\n", name);
  static int fake_class;
  return &fake_class;
}

static void *jni_GetObjectClass(void *env, void *obj) {
  (void)env;
  (void)obj;
  static int f;
  return &f;
}

static void *tag_for_method(const char *name) {
  if (strcmp(name, "getNativeSurface") == 0)
    return &g_method_tags[MID_GET_NATIVE_SURFACE];
  if (strcmp(name, "getContext") == 0)
    return &g_method_tags[MID_GET_CONTEXT];
  if (strcmp(name, "isAndroidTV") == 0)
    return &g_method_tags[MID_IS_ANDROID_TV];
  if (strcmp(name, "getManifestEnvironmentVariables") == 0)
    return &g_method_tags[MID_GET_MANIFEST_ENV];
  if (strcmp(name, "audioOpen") == 0)
    return &g_method_tags[MID_AUDIO_OPEN];
  if (strcmp(name, "audioWriteShortBuffer") == 0)
    return &g_method_tags[MID_AUDIO_WRITE_SHORT];
  if (strcmp(name, "audioWriteByteBuffer") == 0)
    return &g_method_tags[MID_AUDIO_WRITE_BYTE];
  if (strcmp(name, "audioWriteFloatBuffer") == 0)
    return &g_method_tags[MID_AUDIO_WRITE_FLOAT];
  if (strcmp(name, "audioClose") == 0)
    return &g_method_tags[MID_AUDIO_CLOSE];
  return &g_method_tags[MID_GENERIC];
}

static void *jni_GetMethodID(void *env, void *clazz, const char *name,
                             const char *sig) {
  (void)env;
  (void)clazz;
  (void)sig;
  return tag_for_method(name);
}

static void *jni_GetStaticMethodID(void *env, void *clazz, const char *name,
                                   const char *sig) {
  (void)env;
  (void)clazz;
  if (getenv("SUMMERTIME_VERBOSE"))
    debugPrintf("jni: GetStaticMethodID(%s, %s)\n", name, sig);
  return tag_for_method(name);
}

static void *jni_CallObjectMethod(void *env, void *obj, void *mid, ...) {
  (void)env;
  (void)obj;
  (void)mid;
  static int fake_obj;
  return &fake_obj;
}
static jboolean jni_CallBooleanMethod(void *env, void *obj, void *mid, ...) {
  (void)env;
  (void)obj;
  (void)mid;
  return 0;
}
static jint jni_CallIntMethod(void *env, void *obj, void *mid, ...) {
  (void)env;
  (void)obj;
  (void)mid;
  return 0;
}
static void jni_CallVoidMethod(void *env, void *obj, void *mid, ...) {
  (void)env;
  (void)obj;
  (void)mid;
}

static jni_audio_open_cb g_audio_open;
static jni_audio_write_cb g_audio_write_short;
static jni_audio_write_cb g_audio_write_byte;
static jni_audio_write_cb g_audio_write_float;
static jni_audio_close_cb g_audio_close;

static void jni_audio_write_array(void *mid, void *arr);

static void *jni_audio_open_result(int sampleRate, int audioFormat,
                                   int desiredChannels, int desiredFrames) {
  debugPrintf("jni: audioOpen[](rate=%d,fmt=%d,ch=%d,frames=%d)\n",
              sampleRate, audioFormat, desiredChannels, desiredFrames);

  if (g_audio_open &&
      g_audio_open(sampleRate, audioFormat, desiredChannels, desiredFrames) != 0)
    return NULL;

  int *result = (int *)reg_array(4, sizeof(int));
  result[0] = sampleRate;
  result[1] = audioFormat;
  result[2] = desiredChannels;
  result[3] = desiredFrames;
  return result;
}

static void *jni_CallStaticObjectMethod(void *env, void *clazz, void *mid,
                                        ...) {
  (void)env;
  (void)clazz;

  if (mid == &g_method_tags[MID_AUDIO_OPEN]) {
    va_list ap;
    va_start(ap, mid);
    int sampleRate = va_arg(ap, int);
    int audioFormat = va_arg(ap, int);
    int desiredChannels = va_arg(ap, int);
    int desiredFrames = va_arg(ap, int);
    va_end(ap);
    return jni_audio_open_result(sampleRate, audioFormat, desiredChannels,
                                 desiredFrames);
  }

  // getNativeSurface / getContext / getDisplayDPI: must be non-null.
  static int fake_result;
  return &fake_result;
}

static void *jni_CallStaticObjectMethodV(void *env, void *clazz, void *mid,
                                         va_list ap) {
  (void)env;
  (void)clazz;
  if (mid == &g_method_tags[MID_AUDIO_OPEN]) {
    int sampleRate = va_arg(ap, int);
    int audioFormat = va_arg(ap, int);
    int desiredChannels = va_arg(ap, int);
    int desiredFrames = va_arg(ap, int);
    return jni_audio_open_result(sampleRate, audioFormat, desiredChannels,
                                 desiredFrames);
  }
  static int fake_result;
  return &fake_result;
}

static void *jni_CallStaticObjectMethodA(void *env, void *clazz, void *mid,
                                         const jvalue *args) {
  (void)env;
  (void)clazz;
  if (mid == &g_method_tags[MID_AUDIO_OPEN] && args) {
    return jni_audio_open_result(args[0].i, args[1].i, args[2].i, args[3].i);
  }
  static int fake_result;
  return &fake_result;
}

static jboolean jni_CallStaticBooleanMethod(void *env, void *clazz, void *mid,
                                            ...) {
  (void)env;
  (void)clazz;
  (void)mid;
  return 0; // isAndroidTV / getManifestEnvironmentVariables -> false
}

void jni_shim_set_audio_cb(jni_audio_open_cb open_cb,
                           jni_audio_write_cb write_short_cb,
                           jni_audio_write_cb write_byte_cb,
                           jni_audio_close_cb close_cb) {
  g_audio_open = open_cb;
  g_audio_write_short = write_short_cb;
  g_audio_write_byte = write_byte_cb;
  g_audio_write_float = write_byte_cb;
  g_audio_close = close_cb;
}

static jint jni_CallStaticIntMethod(void *env, void *clazz, void *mid, ...) {
  (void)env;
  (void)clazz;
  if (mid == &g_method_tags[MID_AUDIO_OPEN]) {
    va_list ap;
    va_start(ap, mid);
    int sampleRate = va_arg(ap, int);
    int is16Bit = va_arg(ap, int);
    int isStereo = va_arg(ap, int);
    int desiredFrames = va_arg(ap, int);
    va_end(ap);
    int audioFormat = is16Bit ? 2 : 3;
    int channels = isStereo ? 2 : 1;
    debugPrintf("jni: audioOpen(rate=%d,fmt=%d,ch=%d,frames=%d)\n",
                sampleRate, audioFormat, channels, desiredFrames);
    if (g_audio_open)
      return g_audio_open(sampleRate, audioFormat, channels, desiredFrames);
    return desiredFrames;
  }
  return 0;
}

static void jni_CallStaticVoidMethod(void *env, void *clazz, void *mid, ...) {
  (void)env;
  (void)clazz;
  if (mid == &g_method_tags[MID_AUDIO_WRITE_SHORT] ||
      mid == &g_method_tags[MID_AUDIO_WRITE_BYTE] ||
      mid == &g_method_tags[MID_AUDIO_WRITE_FLOAT]) {
    va_list ap;
    va_start(ap, mid);
    void *arr = va_arg(ap, void *);
    va_end(ap);
    jni_audio_write_array(mid, arr);
    return;
  }
  if (mid == &g_method_tags[MID_AUDIO_CLOSE]) {
    if (g_audio_close)
      g_audio_close();
  }
}

static void jni_audio_write_array(void *mid, void *arr) {
  if (mid == &g_method_tags[MID_AUDIO_WRITE_SHORT]) {
    int len_bytes = 0;
    if (find_array(arr, &len_bytes) && g_audio_write_short)
      g_audio_write_short(arr, len_bytes);
    return;
  }
  if (mid == &g_method_tags[MID_AUDIO_WRITE_BYTE]) {
    int len_bytes = 0;
    if (find_array(arr, &len_bytes) && g_audio_write_byte)
      g_audio_write_byte(arr, len_bytes);
    return;
  }
  if (mid == &g_method_tags[MID_AUDIO_WRITE_FLOAT]) {
    int len_bytes = 0;
    if (find_array(arr, &len_bytes) && g_audio_write_float)
      g_audio_write_float(arr, len_bytes);
    return;
  }
}

static void jni_CallStaticVoidMethodV(void *env, void *clazz, void *mid,
                                      va_list ap) {
  (void)env;
  (void)clazz;
  if (mid == &g_method_tags[MID_AUDIO_WRITE_SHORT] ||
      mid == &g_method_tags[MID_AUDIO_WRITE_BYTE] ||
      mid == &g_method_tags[MID_AUDIO_WRITE_FLOAT]) {
    jni_audio_write_array(mid, va_arg(ap, void *));
    return;
  }
  if (mid == &g_method_tags[MID_AUDIO_CLOSE] && g_audio_close)
    g_audio_close();
}
static void jni_CallStaticVoidMethodA(void *env, void *clazz, void *mid,
                                      const jvalue *args) {
  (void)env;
  (void)clazz;
  if ((mid == &g_method_tags[MID_AUDIO_WRITE_SHORT] ||
       mid == &g_method_tags[MID_AUDIO_WRITE_BYTE]) &&
      args) {
    jni_audio_write_array(mid, args[0].l);
    return;
  }
  if (mid == &g_method_tags[MID_AUDIO_WRITE_FLOAT] &&
      args) {
    jni_audio_write_array(mid, args[0].l);
    return;
  }
  if (mid == &g_method_tags[MID_AUDIO_CLOSE] && g_audio_close)
    g_audio_close();
}

static void *jni_NewStringUTF(void *env, const char *str) {
  (void)env;
  return make_jstring(str ? str : "");
}
static jint jni_GetStringUTFLength(void *env, void *jstr) {
  (void)env;
  return (jint)strlen(resolve_jstring(jstr));
}
static const char *jni_GetStringUTFChars(void *env, void *jstr, void *isCopy) {
  (void)env;
  if (isCopy)
    *(unsigned char *)isCopy = 0;
  return resolve_jstring(jstr);
}
static void jni_ReleaseStringUTFChars(void *env, void *jstr, const char *c) {
  (void)env;
  (void)jstr;
  (void)c;
}

static void *jni_NewGlobalRef(void *env, void *o) {
  (void)env;
  return o;
}
static void *jni_NewLocalRef(void *env, void *o) {
  (void)env;
  return o;
}
static void jni_DeleteGlobalRef(void *env, void *o) {
  (void)env;
  (void)o;
}
static void jni_DeleteLocalRef(void *env, void *o) {
  (void)env;
  (void)o;
}
static jboolean jni_ExceptionCheck(void *env) {
  (void)env;
  return 0;
}
static void jni_ExceptionClear(void *env) { (void)env; }
static void *jni_ExceptionOccurred(void *env) {
  (void)env;
  return NULL;
}

static void *jni_NewShortArray(void *env, jint count) {
  (void)env;
  return reg_array(count, 2);
}
static void *jni_NewByteArray(void *env, jint count) {
  (void)env;
  return reg_array(count, 1);
}
static void *jni_NewFloatArray(void *env, jint count) {
  (void)env;
  return reg_array(count, 4);
}
static jint jni_GetArrayLength(void *env, void *array) {
  (void)env;
  int len_bytes = 0, elem = find_array(array, &len_bytes);
  if (elem)
    return len_bytes / elem;
  return 0;
}
static void *jni_GetArrayElements(void *env, void *array, void *isCopy) {
  (void)env;
  if (isCopy)
    *(unsigned char *)isCopy = 0;
  // Registered (audio) arrays: handle == real data pointer.
  if (find_array(array, NULL))
    return array;
  return g_dummy_array;
}
static void *jni_GetObjectArrayElement(void *env, void *array, jint index) {
  (void)env;
  (void)array;
  (void)index;
  return make_jstring(".");
}

static jint vm_DestroyJavaVM(void *vm) {
  (void)vm;
  return 0;
}
static jint vm_AttachCurrentThread(void *vm, void **penv, void *args) {
  (void)vm;
  (void)args;
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
  if (penv)
    *penv = &jni_env_ptr;
  return 0;
}

void *AAssetManager_fromJava(void *env, void *assetManager) {
  (void)env;
  (void)assetManager;
  return (void *)0x1337;
}

void jni_shim_init(void **out_vm, void **out_env) {
  for (int i = 0; i < JNI_VTABLE_SIZE; i++) {
    jni_env_vtable[i] = (uintptr_t)jni_stub;
    java_vm_vtable[i] = (uintptr_t)jni_stub;
  }

  jni_env_vtable[4] = (uintptr_t)jni_GetVersion;
  jni_env_vtable[6] = (uintptr_t)jni_FindClass;
  jni_env_vtable[15] = (uintptr_t)jni_ExceptionOccurred;
  jni_env_vtable[17] = (uintptr_t)jni_ExceptionClear;
  jni_env_vtable[21] = (uintptr_t)jni_NewGlobalRef;
  jni_env_vtable[22] = (uintptr_t)jni_DeleteGlobalRef;
  jni_env_vtable[23] = (uintptr_t)jni_DeleteLocalRef;
  jni_env_vtable[25] = (uintptr_t)jni_NewLocalRef;
  jni_env_vtable[31] = (uintptr_t)jni_GetObjectClass;
  jni_env_vtable[33] = (uintptr_t)jni_GetMethodID;
  jni_env_vtable[34] = (uintptr_t)jni_CallObjectMethod;
  jni_env_vtable[35] = (uintptr_t)jni_CallObjectMethod;
  jni_env_vtable[36] = (uintptr_t)jni_CallObjectMethod;
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
  jni_env_vtable[118] = (uintptr_t)jni_CallStaticBooleanMethod;
  jni_env_vtable[119] = (uintptr_t)jni_CallStaticBooleanMethod;
  jni_env_vtable[129] = (uintptr_t)jni_CallStaticIntMethod;
  jni_env_vtable[130] = (uintptr_t)jni_CallStaticIntMethod;
  jni_env_vtable[131] = (uintptr_t)jni_CallStaticIntMethod;
  jni_env_vtable[141] = (uintptr_t)jni_CallStaticVoidMethod;
  jni_env_vtable[142] = (uintptr_t)jni_CallStaticVoidMethodV;
  jni_env_vtable[143] = (uintptr_t)jni_CallStaticVoidMethodA;

  jni_env_vtable[167] = (uintptr_t)jni_NewStringUTF;
  jni_env_vtable[168] = (uintptr_t)jni_GetStringUTFLength;
  jni_env_vtable[169] = (uintptr_t)jni_GetStringUTFChars;
  jni_env_vtable[170] = (uintptr_t)jni_ReleaseStringUTFChars;
  jni_env_vtable[171] = (uintptr_t)jni_GetArrayLength;
  jni_env_vtable[173] = (uintptr_t)jni_GetObjectArrayElement;
  jni_env_vtable[176] = (uintptr_t)jni_NewByteArray;
  jni_env_vtable[178] = (uintptr_t)jni_NewShortArray;
  jni_env_vtable[181] = (uintptr_t)jni_NewFloatArray;
  jni_env_vtable[205] = (uintptr_t)jni_ExceptionCheck;

  jni_env_vtable[183] = (uintptr_t)jni_GetArrayElements;
  jni_env_vtable[184] = (uintptr_t)jni_GetArrayElements;
  jni_env_vtable[185] = (uintptr_t)jni_GetArrayElements;
  jni_env_vtable[186] = (uintptr_t)jni_GetArrayElements;
  jni_env_vtable[187] = (uintptr_t)jni_GetArrayElements;
  jni_env_vtable[188] = (uintptr_t)jni_GetArrayElements;
  jni_env_vtable[189] = (uintptr_t)jni_GetArrayElements;
  jni_env_vtable[190] = (uintptr_t)jni_GetArrayElements;

  java_vm_vtable[3] = (uintptr_t)vm_DestroyJavaVM;
  java_vm_vtable[4] = (uintptr_t)vm_AttachCurrentThread;
  java_vm_vtable[5] = (uintptr_t)vm_DetachCurrentThread;
  java_vm_vtable[6] = (uintptr_t)vm_GetEnv;

  jni_env_ptr = jni_env_vtable;
  java_vm_ptr = java_vm_vtable;

  if (out_vm)
    *out_vm = &java_vm_ptr;
  if (out_env)
    *out_env = &jni_env_ptr;
}
