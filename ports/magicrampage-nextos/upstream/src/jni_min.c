#define _GNU_SOURCE
#include "jni_min.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define JSTR_MAGIC 0x4d524a53u

typedef struct FakeJString {
  uint32_t magic;
  char *text;
} FakeJString;

static uintptr_t g_env[256];
static uintptr_t g_vm[64];
static char g_fake_class;

static int GetVersion(void *env) {
  (void)env;
  return 0x00010006;
}

static void *FindClass(void *env, const char *name) {
  (void)env;
  fprintf(stderr, "[jni] FindClass(%s)\n", name ? name : "?");
  return &g_fake_class;
}

static void ExceptionClear(void *env) { (void)env; }
static int ExceptionCheck(void *env) {
  (void)env;
  return 0;
}

static void FatalError(void *env, const char *msg) {
  (void)env;
  fprintf(stderr, "[jni] FatalError: %s\n", msg ? msg : "");
  abort();
}

static void *NewStringUTF(void *env, const char *s) {
  (void)env;
  return jni_string(s ? s : "");
}

static int GetStringUTFLength(void *env, void *s) {
  (void)env;
  return (int)strlen(jni_string_cstr(s));
}

static int GetStringLength(void *env, void *s) {
  (void)env;
  return (int)strlen(jni_string_cstr(s));
}

static const char *GetStringUTFChars(void *env, void *s, unsigned char *is_copy) {
  (void)env;
  if (is_copy)
    *is_copy = 0;
  return jni_string_cstr(s);
}

static void ReleaseStringUTFChars(void *env, void *s, const char *chars) {
  (void)env;
  (void)s;
  (void)chars;
}

static void DeleteLocalRef(void *env, void *obj) {
  (void)env;
  (void)obj;
}

static void *GetObjectClass(void *env, void *obj) {
  (void)env;
  (void)obj;
  return &g_fake_class;
}

static void *GetMethodID(void *env, void *cls, const char *name,
                         const char *sig) {
  (void)env;
  (void)cls;
  fprintf(stderr, "[jni] GetMethodID(%s %s)\n", name ? name : "?",
          sig ? sig : "?");
  return NULL;
}

static void *GetStaticMethodID(void *env, void *cls, const char *name,
                               const char *sig) {
  (void)env;
  (void)cls;
  fprintf(stderr, "[jni] GetStaticMethodID(%s %s)\n", name ? name : "?",
          sig ? sig : "?");
  return NULL;
}

static void *CallObjectMethod(void *env, void *obj, void *mid, ...) {
  (void)env;
  (void)obj;
  (void)mid;
  return NULL;
}

static void CallVoidMethod(void *env, void *obj, void *mid, ...) {
  (void)env;
  (void)obj;
  (void)mid;
}

static int CallBooleanMethod(void *env, void *obj, void *mid, ...) {
  (void)env;
  (void)obj;
  (void)mid;
  return 0;
}

static int CallIntMethod(void *env, void *obj, void *mid, ...) {
  (void)env;
  (void)obj;
  (void)mid;
  return 0;
}

static void *CallStaticObjectMethod(void *env, void *cls, void *mid, ...) {
  (void)env;
  (void)cls;
  (void)mid;
  return NULL;
}

static void CallStaticVoidMethod(void *env, void *cls, void *mid, ...) {
  (void)env;
  (void)cls;
  (void)mid;
}

static int CallStaticBooleanMethod(void *env, void *cls, void *mid, ...) {
  (void)env;
  (void)cls;
  (void)mid;
  return 0;
}

static int CallStaticIntMethod(void *env, void *cls, void *mid, ...) {
  (void)env;
  (void)cls;
  (void)mid;
  return 0;
}

static int RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env;
  (void)cls;
  (void)methods;
  fprintf(stderr, "[jni] RegisterNatives(%d)\n", n);
  return 0;
}

static int GetJavaVM(void *env, void **vm) {
  (void)env;
  if (vm)
    *vm = g_vm;
  return 0;
}

static int VM_GetEnv(void *vm, void **env, int version) {
  (void)vm;
  (void)version;
  if (env)
    *env = g_env;
  return 0;
}

static int VM_DestroyJavaVM(void *vm) {
  (void)vm;
  return 0;
}

static int VM_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm;
  (void)args;
  if (env)
    *env = g_env;
  return 0;
}

static int VM_DetachCurrentThread(void *vm) {
  (void)vm;
  return 0;
}

#define SET(idx, fn) (g_env[(idx)] = (uintptr_t)(fn))
#define VMSET(idx, fn) (g_vm[(idx)] = (uintptr_t)(fn))

void jni_min_init(void) {
  for (unsigned i = 0; i < sizeof(g_env) / sizeof(g_env[0]); i++)
    g_env[i] = (uintptr_t)ret0;
  g_env[0] = (uintptr_t)g_env;

  SET(4, GetVersion);
  SET(6, FindClass);
  SET(17, ExceptionClear);
  SET(18, FatalError);
  SET(24, ret0);
  SET(26, ret0);
  SET(31, GetObjectClass);
  SET(33, GetMethodID);
  SET(34, CallObjectMethod);
  SET(37, CallBooleanMethod);
  SET(49, CallIntMethod);
  SET(61, CallVoidMethod);
  SET(113, GetStaticMethodID);
  SET(114, CallStaticObjectMethod);
  SET(117, CallStaticBooleanMethod);
  SET(129, CallStaticIntMethod);
  SET(138, CallStaticVoidMethod);
  SET(165, GetStringLength);
  SET(167, NewStringUTF);
  SET(168, GetStringUTFLength);
  SET(169, GetStringUTFChars);
  SET(170, ReleaseStringUTFChars);
  SET(215, RegisterNatives);
  SET(219, GetJavaVM);
  SET(228, ExceptionCheck);

  for (unsigned i = 0; i < sizeof(g_vm) / sizeof(g_vm[0]); i++)
    g_vm[i] = (uintptr_t)ret0;
  g_vm[0] = (uintptr_t)g_vm;
  VMSET(3, VM_DestroyJavaVM);
  VMSET(4, VM_AttachCurrentThread);
  VMSET(5, VM_DetachCurrentThread);
  VMSET(6, VM_GetEnv);
  VMSET(7, VM_AttachCurrentThread);
}

void *jni_env(void) { return g_env; }
void *jni_class(void) { return &g_fake_class; }

void *jni_string(const char *s) {
  FakeJString *j = (FakeJString *)calloc(1, sizeof(*j));
  if (!j)
    return NULL;
  j->magic = JSTR_MAGIC;
  j->text = strdup(s ? s : "");
  return j;
}

const char *jni_string_cstr(void *s) {
  if (!s)
    return "";
  FakeJString *j = (FakeJString *)s;
  if (j->magic == JSTR_MAGIC)
    return j->text ? j->text : "";
  return (const char *)s;
}

void jni_string_free(void *s) {
  if (!s)
    return;
  FakeJString *j = (FakeJString *)s;
  if (j->magic != JSTR_MAGIC)
    return;
  free(j->text);
  free(j);
}
