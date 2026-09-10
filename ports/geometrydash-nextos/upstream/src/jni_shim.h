/* jni_shim.h -- fake JNIEnv/JavaVM for the Cocos2d-x guest. */
#include <stdint.h>

#ifndef __JNI_SHIM_H__
#define __JNI_SHIM_H__

void jni_shim_init(void **out_vm, void **out_env);

/* Paths the Java side would have answered with. */
void jni_shim_set_paths(const char *writable_path, const char *package_name);

/* Called by main once the engine entry points are resolved, so the bitmap
 * upcall can hand the pixels back through nativeInitBitmapDC. */
typedef void (*jni_bitmap_dc_fn)(void *env, void *thiz, int w, int h,
                                 void *pixels);
void jni_shim_set_bitmap_dc(jni_bitmap_dc_fn fn);

/* Flush the preference store to disk (called on quit and on pause). */
void jni_shim_prefs_flush(void);

/* Interface used by main.c to build fake arrays for nativeTouchesMove. */
void *jni_shim_new_int_array(const int *v, int n);
void *jni_shim_new_float_array(const float *v, int n);
void *jni_shim_new_jstring(const char *s);

/* Monotonic, jump-free milliseconds -- the engine's only clock. */
int64_t jni_shim_uptime_ms(void);

#endif
