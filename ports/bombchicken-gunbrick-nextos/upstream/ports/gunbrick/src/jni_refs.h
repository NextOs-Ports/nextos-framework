/* SPDX-License-Identifier: GPL-3.0-only */
/* Ownership and local-frame tracking for the fake JNI object model. */

#ifndef GB_JNI_REFS_H
#define GB_JNI_REFS_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define GB_JNI_REF_MAGIC      UINT32_C(0x47424a52)
#define GB_JNI_REF_DEAD_MAGIC UINT32_C(0xdead4a52)

typedef void (*gb_jni_ref_destructor)(void *);

typedef struct {
    uint32_t magic;
    _Atomic uint32_t refs;
    uint8_t permanent;
    uint8_t reserved[3];
    gb_jni_ref_destructor destroy;
} gb_jni_ref_header;

typedef struct {
    size_t objects_created;
    size_t objects_destroyed;
    size_t objects_live;
    size_t objects_peak;
    size_t thread_local_refs;
    unsigned local_depth;
} gb_jni_ref_stats;

int gb_jni_ref_init(void *object, int permanent,
                    gb_jni_ref_destructor destroy);
int gb_jni_ref_is_managed(const void *object);
int gb_jni_ref_is_permanent(const void *object);
void gb_jni_ref_retain(void *object);
void gb_jni_ref_release(void *object);

int gb_jni_ref_push_local_frame(int capacity);
void *gb_jni_ref_pop_local_frame(void *result);
void *gb_jni_ref_new_local(void *object);
void *gb_jni_ref_new_global(void *object);
void gb_jni_ref_delete_local(void *object);
void gb_jni_ref_delete_global(void *object);

int gb_jni_ref_scope_begin(void);
void gb_jni_ref_scope_end(int token);
unsigned gb_jni_ref_scope_depth(void);
void gb_jni_ref_get_stats(gb_jni_ref_stats *out);

#endif /* GB_JNI_REFS_H */
