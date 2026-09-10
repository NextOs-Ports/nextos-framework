/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * JNI references are ownership, not decoration.  Android clears every local
 * frame on return from a native entry point; reproducing that rule keeps the
 * adapter's Java-object stand-ins bounded during long Unity sessions.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "jni_refs.h"

typedef struct local_ref_node {
    void *object;
    struct local_ref_node *next;
} local_ref_node;

typedef struct managed_ref_node {
    void *object;
    struct managed_ref_node *next;
} managed_ref_node;

#define GB_JNI_LOCAL_DEPTH_MAX 64u
#define GB_JNI_REGISTRY_BUCKETS 1024u

static _Thread_local local_ref_node *local_frames[GB_JNI_LOCAL_DEPTH_MAX + 1];
static _Thread_local size_t thread_local_refs;
static _Thread_local unsigned local_depth;

/*
 * JNI handles come from guest code and therefore cannot be dereferenced just
 * to discover whether they belong to us.  The registry is authoritative and
 * also serializes the final release against concurrent retain operations.
 */
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static managed_ref_node *managed_refs[GB_JNI_REGISTRY_BUCKETS];

static _Atomic size_t objects_created;
static _Atomic size_t objects_destroyed;
static _Atomic size_t objects_live;
static _Atomic size_t objects_peak;

static size_t registry_bucket(const void *object)
{
    uintptr_t value = (uintptr_t)object;
    value >>= 3;
    value ^= value >> 11;
    value ^= value >> 19;
    return (size_t)value & (GB_JNI_REGISTRY_BUCKETS - 1u);
}

static managed_ref_node **registry_find_locked(const void *object)
{
    managed_ref_node **link = &managed_refs[registry_bucket(object)];
    while (*link && (*link)->object != object)
        link = &(*link)->next;
    return link;
}

static int retain_nonpermanent(void *object)
{
    int retained = 0;
    pthread_mutex_lock(&registry_lock);
    managed_ref_node *node = *registry_find_locked(object);
    if (node) {
        gb_jni_ref_header *header = node->object;
        if (!header->permanent) {
            atomic_fetch_add_explicit(&header->refs, 1,
                                      memory_order_relaxed);
            retained = 1;
        }
    }
    pthread_mutex_unlock(&registry_lock);
    return retained;
}

static int is_nonpermanent(const void *object)
{
    int result = 0;
    pthread_mutex_lock(&registry_lock);
    managed_ref_node *node = *registry_find_locked(object);
    if (node)
        result = !((gb_jni_ref_header *)node->object)->permanent;
    pthread_mutex_unlock(&registry_lock);
    return result;
}

int gb_jni_ref_is_managed(const void *object)
{
    if (!object)
        return 0;
    pthread_mutex_lock(&registry_lock);
    int result = *registry_find_locked(object) != NULL;
    pthread_mutex_unlock(&registry_lock);
    return result;
}

int gb_jni_ref_is_permanent(const void *object)
{
    if (!object)
        return 0;
    pthread_mutex_lock(&registry_lock);
    managed_ref_node *node = *registry_find_locked(object);
    int result = node &&
        ((const gb_jni_ref_header *)node->object)->permanent != 0;
    pthread_mutex_unlock(&registry_lock);
    return result;
}

int gb_jni_ref_init(void *object, int permanent,
                    gb_jni_ref_destructor destroy)
{
    if (!object || !destroy)
        return -1;

    managed_ref_node *managed = malloc(sizeof *managed);
    local_ref_node *local = permanent ? NULL : malloc(sizeof *local);
    if (!managed || (!permanent && !local)) {
        free(local);
        free(managed);
        return -1;
    }

    pthread_mutex_lock(&registry_lock);
    managed_ref_node **link = registry_find_locked(object);
    if (*link) {
        pthread_mutex_unlock(&registry_lock);
        free(local);
        free(managed);
        return -1;
    }

    gb_jni_ref_header *header = object;
    header->magic = GB_JNI_REF_MAGIC;
    atomic_init(&header->refs, 1);
    header->permanent = permanent != 0;
    memset(header->reserved, 0, sizeof header->reserved);
    header->destroy = destroy;

    managed->object = object;
    managed->next = NULL;
    *link = managed;

    if (local) {
        local->object = object;
        local->next = local_frames[local_depth];
        local_frames[local_depth] = local;
        thread_local_refs++;
    }

    size_t live = atomic_fetch_add_explicit(&objects_live, 1,
                                             memory_order_relaxed) + 1;
    atomic_fetch_add_explicit(&objects_created, 1, memory_order_relaxed);
    size_t peak = atomic_load_explicit(&objects_peak, memory_order_relaxed);
    while (peak < live &&
           !atomic_compare_exchange_weak_explicit(
               &objects_peak, &peak, live,
               memory_order_relaxed, memory_order_relaxed))
        ;
    pthread_mutex_unlock(&registry_lock);
    return 0;
}

void gb_jni_ref_retain(void *object)
{
    if (!object)
        return;
    (void)retain_nonpermanent(object);
}

void gb_jni_ref_release(void *object)
{
    if (!object)
        return;

    gb_jni_ref_destructor destroy = NULL;
    managed_ref_node *retired = NULL;

    pthread_mutex_lock(&registry_lock);
    managed_ref_node **link = registry_find_locked(object);
    managed_ref_node *node = *link;
    if (!node) {
        pthread_mutex_unlock(&registry_lock);
        return;
    }

    gb_jni_ref_header *header = node->object;
    if (header->permanent) {
        pthread_mutex_unlock(&registry_lock);
        return;
    }

    uint32_t old = atomic_fetch_sub_explicit(&header->refs, 1,
                                              memory_order_acq_rel);
    if (old != 1) {
        pthread_mutex_unlock(&registry_lock);
        return;
    }

    *link = node->next;
    retired = node;
    header->magic = GB_JNI_REF_DEAD_MAGIC;
    destroy = header->destroy;
    pthread_mutex_unlock(&registry_lock);

    atomic_fetch_sub_explicit(&objects_live, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&objects_destroyed, 1, memory_order_relaxed);
    free(retired);
    destroy(object);
}

int gb_jni_ref_push_local_frame(int capacity)
{
    (void)capacity;
    if (local_depth >= GB_JNI_LOCAL_DEPTH_MAX ||
        local_frames[local_depth + 1] != NULL)
        return -1;
    local_depth++;
    return 0;
}

void *gb_jni_ref_pop_local_frame(void *result)
{
    if (local_depth == 0)
        return result;

    local_ref_node *promoted = result ? malloc(sizeof *promoted) : NULL;
    if (promoted && retain_nonpermanent(result)) {
        promoted->object = result;
        promoted->next = NULL;
    } else {
        free(promoted);
        promoted = NULL;
        if (result && is_nonpermanent(result))
            result = NULL;
    }

    local_ref_node *node = local_frames[local_depth];
    local_frames[local_depth] = NULL;
    while (node) {
        local_ref_node *next = node->next;
        gb_jni_ref_release(node->object);
        free(node);
        if (thread_local_refs)
            thread_local_refs--;
        node = next;
    }

    local_depth--;
    if (promoted) {
        promoted->next = local_frames[local_depth];
        local_frames[local_depth] = promoted;
        thread_local_refs++;
    }
    return result;
}

void *gb_jni_ref_new_local(void *object)
{
    if (!object)
        return object;

    local_ref_node *node = malloc(sizeof *node);
    if (!node) {
        if (is_nonpermanent(object))
            return NULL;
        return object;
    }

    if (!retain_nonpermanent(object)) {
        free(node);
        return object;
    }

    node->object = object;
    node->next = local_frames[local_depth];
    local_frames[local_depth] = node;
    thread_local_refs++;
    return object;
}

void *gb_jni_ref_new_global(void *object)
{
    gb_jni_ref_retain(object);
    return object;
}

void gb_jni_ref_delete_local(void *object)
{
    for (unsigned frame = local_depth + 1; frame-- > 0;) {
        local_ref_node **link = &local_frames[frame];
        while (*link) {
            local_ref_node *node = *link;
            if (node->object == object) {
                *link = node->next;
                free(node);
                if (thread_local_refs)
                    thread_local_refs--;
                gb_jni_ref_release(object);
                return;
            }
            link = &node->next;
        }
    }
}

void gb_jni_ref_delete_global(void *object)
{
    gb_jni_ref_release(object);
}

int gb_jni_ref_scope_begin(void)
{
    int token = (int)local_depth;
    return gb_jni_ref_push_local_frame(0) < 0 ? -1 : token;
}

void gb_jni_ref_scope_end(int token)
{
    if (token < 0)
        return;
    while (local_depth > (unsigned)token)
        (void)gb_jni_ref_pop_local_frame(NULL);
}

unsigned gb_jni_ref_scope_depth(void)
{
    return local_depth;
}

void gb_jni_ref_get_stats(gb_jni_ref_stats *out)
{
    if (!out)
        return;
    out->objects_created = atomic_load_explicit(&objects_created,
                                                 memory_order_relaxed);
    out->objects_destroyed = atomic_load_explicit(&objects_destroyed,
                                                   memory_order_relaxed);
    out->objects_live = atomic_load_explicit(&objects_live,
                                              memory_order_relaxed);
    out->objects_peak = atomic_load_explicit(&objects_peak,
                                              memory_order_relaxed);
    out->thread_local_refs = thread_local_refs;
    out->local_depth = local_depth;
}
