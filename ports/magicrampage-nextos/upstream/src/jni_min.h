#ifndef MAGIC_JNI_MIN_H
#define MAGIC_JNI_MIN_H

#include <stddef.h>

void jni_min_init(void);
void *jni_env(void);
void *jni_class(void);
void *jni_string(const char *s);
const char *jni_string_cstr(void *s);
void jni_string_free(void *s);

#endif
