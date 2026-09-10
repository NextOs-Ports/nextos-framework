#ifndef __JNI_SHIM_H__
#define __JNI_SHIM_H__

#include <stdint.h>
#include <stddef.h>

void jni_shim_init(void **out_vm, void **out_env);
void jni_shim_set_callbacks(void *age_cb, void *gs_cb, void *ad_cb, void *bill_cb);

void *jni_get_vm(void);
void *jni_get_env(void);

void *jni_new_string_utf(const char *bytes);

#endif
