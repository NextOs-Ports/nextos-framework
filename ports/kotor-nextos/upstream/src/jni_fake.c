/* jni_fake.c -- fake JNI environment for the KOTOR native libs.
 *
 * Generic JNIEnv/JavaVM function tables (indices per the JNI spec). The game's
 * only mandatory Java->native handshake is mountObb/mountPatchObb, which read
 * their jstring path via GetStringUTFChars; every native->Java callback returns
 * a safe default. Table layout adapted from the LOTR/TCS so-loader.
 *
 * MIT license. See LICENSE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include "config.h"
#include "kotor_language.h"
#include "util.h"
#include "jni_fake.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint32_t juint;   /* JNI return slots are 32-bit on armv7 */

enum { TAG_OBJECT = 0x4f424a31, TAG_STRING = 0x53545231, TAG_OBJARR = 0x4f415231, TAG_ID = 0x4d494431 };

typedef struct { uint32_t tag; char label[96]; } FakeObject;
typedef struct { uint32_t tag; char *utf; } FakeString;
typedef struct { uint32_t tag; int len; void **items; } FakeObjArray;
typedef struct { uint32_t tag; char name[96]; char sig[96]; } FakeID;

void *jni_make_object(const char *label) {
  FakeObject *o = calloc(1, sizeof(*o));
  o->tag = TAG_OBJECT; strncpy(o->label, label, sizeof(o->label) - 1); return o;
}
void *jni_make_string(const char *utf) {
  FakeString *s = calloc(1, sizeof(*s));
  s->tag = TAG_STRING; s->utf = strdup(utf ? utf : ""); return s;
}
void *jni_make_string_array(int n, const char **strs) {
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR; a->len = n; a->items = calloc(n ? n : 1, sizeof(void *));
  for (int i = 0; i < n; i++) a->items[i] = jni_make_string(strs[i]);
  return a;
}
static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  return (s && s->tag == TAG_STRING) ? s->utf : "";
}

#define MAX_IDS 256
static FakeID id_pool[MAX_IDS];
static int id_count = 0;
static FakeID *get_id(const char *name, const char *sig) {
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig)) return &id_pool[i];
  if (id_count >= MAX_IDS) return &id_pool[0];
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->name, name, sizeof(id->name) - 1);
  strncpy(id->sig, sig, sizeof(id->sig) - 1);
  return id;
}
static const char *id_name(const FakeID *id) { return (id && id->tag == TAG_ID) ? id->name : ""; }

static int jni_trace_enabled(void) {
  static int initialized;
  static int enabled;
  if (!initialized) {
    enabled = getenv("KOTOR_JNI_TRACE") != NULL;
    initialized = 1;
  }
  return enabled;
}

static juint float_bits(float value) {
  union { float f; juint u; } bits;
  bits.f = value;
  return bits.u;
}

/* generic dispatchers: report "connected / available" so any online/consent
 * gate proceeds (all networking is stubbed, so nothing real can hang). */
static juint call_boolean(const char *name, va_list va) {
  (void)va;
  /* KOTOR.java defaults this preference to false on every device except the
   * NVIDIA Shield.  Matching that native Android decision avoids loading the
   * high-resolution texture set on 1 GiB NextOS devices. */
  if (!strcmp(name, "GetHighResolution"))
    return getenv("KOTOR_HIGH_RES") != NULL;
  if (!strcmp(name, "HasTouchScreen"))
    return 0;
  if (!strcmp(name, "GetDisableSetSwapIntervalCall"))
    return 0;
  if (strstr(name, "onnect") || strstr(name, "Network") || strstr(name, "Available"))
    return 1;
  return 0;
}
static void *call_object(const char *name, va_list va) {
  (void)va;
  return jni_make_object(name && *name ? name : "object");
}
static void call_void(const char *name, va_list va) { (void)name; (void)va; }
static juint call_int(const char *name) {
  if (!strcmp(name, "GetScreenHeightPixel")) return (juint)screen_height;
  if (!strcmp(name, "getCurrentLanguage")) {
    const char *code = getenv("NXPORT_LANGUAGE");
    const juint language = (juint)kotor_language_id_from_code(code);
    static int logged;
    if (!logged) {
      debugPrintf("KOTOR language=%s native-id=%u\n",
                  code && *code ? code : "en", language);
      logged = 1;
    }
    return language;
  }
  if (strstr(name, "ConnectionState")) return 2; /* CONNECTED */
  return 0;
}

static juint j_GetVersion(void *e) { (void)e; return JNI_VERSION_1_6; }
static void *j_FindClass(void *e, const char *n) {
  (void)e;
  if (jni_trace_enabled()) debugPrintf("JNI FindClass %s\n", n ? n : "(null)");
  return jni_make_object(n);
}
static void *j_GetMethodID(void *e, void *c, const char *n, const char *s) {
  (void)e; (void)c;
  if (jni_trace_enabled()) debugPrintf("JNI GetMethodID %s %s\n", n ? n : "(null)", s ? s : "");
  return get_id(n, s);
}
static void *j_GetFieldID(void *e, void *c, const char *n, const char *s) {
  (void)e; (void)c;
  if (jni_trace_enabled()) debugPrintf("JNI GetFieldID %s %s\n", n ? n : "(null)", s ? s : "");
  return get_id(n, s);
}
static void *j_GetObjectClass(void *e, void *o) { (void)e; (void)o; return jni_make_object("class"); }
static void *j_NewGlobalRef(void *e, void *o) { (void)e; return o; }
static void *j_NewLocalRef(void *e, void *o) { (void)e; return o; }
static juint j_ret0_2(void *e, void *a) { (void)e; (void)a; return 0; }
static juint j_ret0_3(void *e, void *a, void *b) { (void)e; (void)a; (void)b; return 0; }

static juint j_CallBooleanMethodV(void *e, void *o, FakeID *id, va_list va) { (void)e; (void)o; return call_boolean(id_name(id), va); }
static juint j_CallBooleanMethod(void *e, void *o, FakeID *id, ...) { va_list va; va_start(va, id); juint r = call_boolean(id_name(id), va); va_end(va); return r; }
static void *j_CallObjectMethodV(void *e, void *o, FakeID *id, va_list va) { (void)e; (void)o; return call_object(id_name(id), va); }
static void *j_CallObjectMethod(void *e, void *o, FakeID *id, ...) { va_list va; va_start(va, id); void *r = call_object(id_name(id), va); va_end(va); return r; }
static void j_CallVoidMethodV(void *e, void *o, FakeID *id, va_list va) { (void)e; (void)o; call_void(id_name(id), va); }
static void j_CallVoidMethod(void *e, void *o, FakeID *id, ...) { va_list va; va_start(va, id); call_void(id_name(id), va); va_end(va); }
static juint j_CallIntMethodV(void *e, void *o, FakeID *id, va_list va) { (void)e; (void)o; (void)va; return call_int(id_name(id)); }
static juint j_CallIntMethod(void *e, void *o, FakeID *id, ...) { (void)e; (void)o; return call_int(id_name(id)); }
static void *j_CallStaticObjectMethodV(void *e, void *c, FakeID *id, va_list va) { (void)e; (void)c; return call_object(id_name(id), va); }
static void *j_CallStaticObjectMethod(void *e, void *c, FakeID *id, ...) { va_list va; va_start(va, id); void *r = call_object(id_name(id), va); va_end(va); return r; }
static juint j_CallStaticBooleanMethodV(void *e, void *c, FakeID *id, va_list va) { (void)e; (void)c; return call_boolean(id_name(id), va); }
static juint j_CallStaticBooleanMethod(void *e, void *c, FakeID *id, ...) { va_list va; va_start(va, id); juint r = call_boolean(id_name(id), va); va_end(va); return r; }
static void j_CallStaticVoidMethodV(void *e, void *c, FakeID *id, va_list va) { (void)e; (void)c; call_void(id_name(id), va); }
static void j_CallStaticVoidMethod(void *e, void *c, FakeID *id, ...) { va_list va; va_start(va, id); call_void(id_name(id), va); va_end(va); }
static juint j_CallStaticIntMethodV(void *e, void *c, FakeID *id, va_list va) { (void)e; (void)c; (void)va; return call_int(id_name(id)); }
static juint j_CallStaticIntMethod(void *e, void *c, FakeID *id, ...) { (void)e; (void)c; return call_int(id_name(id)); }
static juint j_CallStaticFloatMethodV(void *e, void *c, FakeID *id, va_list va) {
  (void)e; (void)c; (void)va;
  const char *name = id_name(id);
  float value = !strcmp(name, "GetScreenHeightInch")
                  ? (float)screen_height / 160.0f : 0.0f;
  if (jni_trace_enabled()) debugPrintf("JNI CallStaticFloat %s -> %.3f\n", name, value);
  return float_bits(value);
}
static juint j_CallStaticFloatMethod(void *e, void *c, FakeID *id, ...) {
  (void)e; (void)c;
  const char *name = id_name(id);
  float value = !strcmp(name, "GetScreenHeightInch")
                  ? (float)screen_height / 160.0f : 0.0f;
  return float_bits(value);
}

static void *j_GetObjectField(void *e, void *o, FakeID *id) {
  (void)e; (void)o;
  const char *name = id_name(id);
  if (!strcmp(name, "MANUFACTURER")) return jni_make_string(DEVICE_MANUFACTURER);
  if (!strcmp(name, "MODEL")) return jni_make_string(DEVICE_MODEL);
  return NULL;
}
static juint j_GetIntField(void *e, void *o, FakeID *id) { (void)e; (void)o; (void)id; return 0; }
static juint j_GetFloatField(void *e, void *o, FakeID *id) {
  (void)e; (void)o;
  const char *name = id_name(id);
  float value = (!strcmp(name, "xdpi") || !strcmp(name, "ydpi")) ? 160.0f : 0.0f;
  if (jni_trace_enabled()) debugPrintf("JNI GetFloatField %s -> %.3f\n", name, value);
  return float_bits(value);
}

static void *j_NewObjectV(void *e, void *c, FakeID *id, va_list va) {
  (void)e; (void)c; (void)va;
  return jni_make_object(id_name(id));
}

static void *j_NewStringUTF(void *e, const char *utf) { (void)e; return jni_make_string(utf); }
static const char *j_GetStringUTFChars(void *e, void *jstr, uint8_t *is_copy) { (void)e; if (is_copy) *is_copy = 0; return obj_str(jstr); }
static void j_ReleaseStringUTFChars(void *e, void *jstr, const char *utf) { (void)e; (void)jstr; (void)utf; }
static juint j_GetStringUTFLength(void *e, void *jstr) { (void)e; return strlen(obj_str(jstr)); }
static juint j_GetStringLength(void *e, void *jstr) { (void)e; return strlen(obj_str(jstr)); }
static void j_GetStringUTFRegion(void *e, void *jstr, int start, int len, char *buf) {
  (void)e;
  const char *src = obj_str(jstr);
  int src_len = (int)strlen(src);
  if (!buf || start < 0 || len < 0 || start > src_len) return;
  if (start + len > src_len) len = src_len - start;
  memcpy(buf, src + start, (size_t)len);
}

static juint j_GetArrayLength(void *e, void *arr) { (void)e; FakeObjArray *a = arr; return (a && a->tag == TAG_OBJARR) ? a->len : 0; }
static void *j_GetObjectArrayElement(void *e, void *arr, int i) { (void)e; FakeObjArray *a = arr; return (a && a->tag == TAG_OBJARR && i >= 0 && i < a->len) ? a->items[i] : jni_make_string(""); }
static void *j_NewObjectArray(void *e, int len, void *c, void *init) {
  (void)e; (void)c; FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR; a->len = len; a->items = calloc(len ? len : 1, sizeof(void *));
  for (int i = 0; i < len; i++) a->items[i] = init; return a;
}

extern void *fake_vm;
static juint j_RegisterNatives(void *e, void *c, void *m, int n) { (void)e; (void)c; (void)m; (void)n; return 0; }
static juint j_GetJavaVM(void *e, void **vm) { (void)e; *vm = fake_vm; return JNI_OK; }
static juint j_ExceptionCheck(void *e) { (void)e; return 0; }
static void *j_ExceptionOccurred(void *e) { (void)e; return NULL; }
static void j_void_1(void *e) { (void)e; }
static void j_DeleteRef(void *e, void *o) { (void)e; (void)o; }
static juint j_PushLocalFrame(void *e, int cap) { (void)e; (void)cap; return 0; }
static void *j_PopLocalFrame(void *e, void *r) { (void)e; return r; }
static juint j_unimplemented(void) {
  debugPrintf("JNI: unimplemented slot called (ra=%p)\n", __builtin_return_address(0));
  return 0;
}

static void *env_table[233];
static void **env_table_ptr = env_table;
void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) { (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK; }
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int v) { (void)vm; (void)v; if (env) *env = fake_env; return JNI_OK; }

static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  for (int i = 0; i < 233; i++) env_table[i] = (void *)j_unimplemented;
  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void_1;
  env_table[17]  = (void *)j_void_1;
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteRef;
  env_table[23]  = (void *)j_DeleteRef;
  env_table[24]  = (void *)j_ret0_3;
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_ret0_2;
  env_table[29]  = (void *)j_NewObjectV;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  env_table[94]  = (void *)j_GetFieldID;
  env_table[95]  = (void *)j_GetObjectField;
  env_table[100] = (void *)j_GetIntField;
  env_table[113] = (void *)j_GetMethodID;
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[135] = (void *)j_CallStaticFloatMethod;
  env_table[136] = (void *)j_CallStaticFloatMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  env_table[144] = (void *)j_GetFieldID;
  env_table[145] = (void *)j_GetObjectField;
  env_table[150] = (void *)j_GetIntField;
  env_table[102] = (void *)j_GetFloatField;
  env_table[164] = (void *)j_GetStringLength;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[216] = (void *)j_ret0_2;
  env_table[217] = (void *)j_ret0_2;
  env_table[218] = (void *)j_ret0_2;
  env_table[219] = (void *)j_GetJavaVM;
  env_table[221] = (void *)j_GetStringUTFRegion;
  env_table[228] = (void *)j_ExceptionCheck;

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread;

  debugPrintf("JNI: fake environment initialized (env=%p vm=%p)\n", fake_env, fake_vm);
}
