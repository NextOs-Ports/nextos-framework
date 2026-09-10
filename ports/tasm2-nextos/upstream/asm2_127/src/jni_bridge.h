#ifndef ASM2_JNI_BRIDGE_H
#define ASM2_JNI_BRIDGE_H

/*
 * Small, self-contained JNI 1.6 environment for the ARMv7 Android guest.
 *
 * The public API intentionally does not depend on a host JDK's jni.h.  Android
 * and a desktop JDK agree on the table order, but including a host jni_md.h in
 * an ARM cross build would also import the host ABI annotations.  All entry
 * points installed in the guest tables use the base AAPCS explicitly.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ASM2_GUEST_PCS
#if defined(__arm__)
#define ASM2_GUEST_PCS __attribute__((pcs("aapcs")))
#else
#define ASM2_GUEST_PCS
#endif
#endif

typedef uint8_t asm2_jboolean;
typedef int8_t asm2_jbyte;
typedef uint16_t asm2_jchar;
typedef int16_t asm2_jshort;
typedef int32_t asm2_jint;
typedef int64_t asm2_jlong;
typedef float asm2_jfloat;
typedef double asm2_jdouble;

/* Binary-compatible with JNI's jvalue on ARM32. */
typedef union asm2_jni_value {
  asm2_jboolean z;
  asm2_jbyte b;
  asm2_jchar c;
  asm2_jshort s;
  asm2_jint i;
  asm2_jlong j;
  asm2_jfloat f;
  asm2_jdouble d;
  void *l;
} asm2_jni_value;

/*
 * Return non-zero from a callback when it supplied *result.  Returning zero
 * asks the bridge for its neutral/default Android value.  ap is present for a
 * JNI .../V call, args for an A call; callbacks must not retain either pointer.
 */
typedef int (*asm2_jni_method_callback)(
    void *user, const char *class_name, const char *name,
    const char *signature, int is_static, void *receiver, va_list *ap,
    const asm2_jni_value *args, asm2_jni_value *result);

typedef int (*asm2_jni_field_get_callback)(
    void *user, const char *class_name, const char *name,
    const char *signature, int is_static, void *receiver,
    asm2_jni_value *result);

typedef void (*asm2_jni_field_set_callback)(
    void *user, const char *class_name, const char *name,
    const char *signature, int is_static, void *receiver,
    asm2_jni_value value);

typedef void (*asm2_jni_native_callback)(
    void *user, const char *class_name, const char *name,
    const char *signature, void *function);

typedef struct asm2_jni_config {
  const char *package_name;
  const char *external_storage;
  const char *files_directory;
  const char *cache_directory;
  const char *locale_language;
  const char *locale_country;
  const char *device_model;
  const char *device_manufacturer;
  const char *android_release;
  asm2_jint sdk_int;
  asm2_jint screen_width;
  asm2_jint screen_height;
  asm2_jfloat screen_density;
  asm2_jni_method_callback method_callback;
  asm2_jni_field_get_callback field_get_callback;
  asm2_jni_field_set_callback field_set_callback;
  asm2_jni_native_callback native_callback;
  void *user;
} asm2_jni_config;

struct asm2_jni_stats {
  uint64_t handles;
  uint64_t classes;
  uint64_t objects;
  uint64_t strings;
  uint64_t arrays;
  uint64_t array_bytes;
  uint64_t method_ids;
  uint64_t field_ids;
  uint64_t direct_buffers;
  uint64_t throwables;
  uint64_t bundle_entries;
  uint32_t longest_handle_bucket;
  uint32_t longest_intern_bucket;
};

/* Reinitializes configuration and tables.  Interned handles remain stable. */
void asm2_jni_init(const asm2_jni_config *config);
void asm2_jni_get_stats(struct asm2_jni_stats *stats);

/* Values to pass directly to JNI_OnLoad and Java_* guest entry points. */
void *asm2_jni_vm(void);
void *asm2_jni_env(void);
void *asm2_jni_activity(void);

/* Stable fake handles useful to the loader and callback implementations. */
void *asm2_jni_class(const char *slash_name);
void *asm2_jni_object(const char *slash_name);
void *asm2_jni_string(const char *utf8);
const char *asm2_jni_string_chars(void *string);
void *asm2_jni_int_array(const asm2_jint *values, size_t count);

/* Small android.os.Bundle implementation used by native Android bridges. */
void *asm2_jni_bundle(void);
int asm2_jni_bundle_put_boolean(void *bundle, const char *key,
                                asm2_jboolean value);
int asm2_jni_bundle_get_boolean(void *bundle, const char *key,
                                asm2_jboolean *value);
int asm2_jni_bundle_put_int(void *bundle, const char *key, asm2_jint value);
int asm2_jni_bundle_get_int(void *bundle, const char *key,
                            asm2_jint *value);
int asm2_jni_bundle_put_long(void *bundle, const char *key,
                             asm2_jlong value);
int asm2_jni_bundle_get_long(void *bundle, const char *key,
                             asm2_jlong *value);
int asm2_jni_bundle_put_string(void *bundle, const char *key,
                               const char *value);
int asm2_jni_bundle_put_byte_array(void *bundle, const char *key,
                                   const void *bytes, size_t count);
const char *asm2_jni_bundle_get_string(void *bundle, const char *key);

/* RegisterNatives inspection/call-out helpers. */
void *asm2_jni_find_registered_native(const char *class_name,
                                      const char *name,
                                      const char *signature);
size_t asm2_jni_registered_native_count(void);

#ifdef __cplusplus
}
#endif

#endif /* ASM2_JNI_BRIDGE_H */
