#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "pthread_bridge.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#if defined(__arm__) && !defined(__aarch64__)
_Static_assert(sizeof(void *) == sizeof(uint32_t),
               "the guest-handle bridge requires a 32-bit process");
_Static_assert(sizeof(pthread_t) == sizeof(uint32_t),
               "pthread_t must match the old ARM32 bionic handle width");
_Static_assert(sizeof(pthread_key_t) == sizeof(uint32_t),
               "pthread_key_t must match the ARM32 guest key width");
_Static_assert(sizeof(time_t) == sizeof(int32_t),
               "guest and host timespec must both use 32-bit time_t");
#endif

/*
 * A guest object exposes only its first 32-bit word to this bridge.  On the
 * ARM32 target that word holds the address of a separately allocated host
 * object.  Never dereference an arbitrary guest word: old bionic static
 * initializers and runtime states are integers.  The registry validates the
 * address, object kind, owner slot and magic before the wrapper is used.
 *
 * Retired wrappers deliberately stay registered.  This prevents an old raw
 * handle from becoming valid again if malloc later reuses the address.  POSIX
 * already forbids destroy racing another operation, but retaining the small
 * wrapper also removes a needless use-after-free failure mode while bringing
 * up foreign code.
 */

#define ASM2_BRIDGE_MAGIC UINT32_C(0x41313237) /* "A127" */
#define ASM2_REGISTRY_BUCKETS 256u

enum asm2_object_kind {
    ASM2_OBJECT_ATTR = 1,
    ASM2_OBJECT_MUTEX_ATTR,
    ASM2_OBJECT_MUTEX,
    ASM2_OBJECT_COND,
    ASM2_OBJECT_SEM,
};

struct asm2_bridge_object {
    uint32_t magic;
    enum asm2_object_kind kind;
    bool active;
    volatile uint32_t *owner;
    uint32_t raw_handle;
    int guest_mutex_type;
    uintptr_t mutex_owner;
    uint32_t mutex_depth;
    struct asm2_bridge_object *active_next;
    struct asm2_bridge_object *retired_next;
    union {
        pthread_attr_t attr;
        pthread_mutexattr_t mutex_attr;
        pthread_mutex_t mutex;
        pthread_cond_t cond;
        sem_t sem;
    } host;
};

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static struct asm2_bridge_object *registry[ASM2_REGISTRY_BUCKETS];
static struct asm2_bridge_object *retired_objects;
static struct asm2_pthread_bridge_stats registry_stats;
static uint64_t mutex_handoff_count;

static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t once_cond = PTHREAD_COND_INITIALIZER;

static volatile uint32_t *guest_slot(void *object)
{
    return (volatile uint32_t *)object;
}

static const volatile uint32_t *guest_const_slot(const void *object)
{
    return (const volatile uint32_t *)object;
}

static uint32_t slot_load(const volatile uint32_t *slot)
{
    return __atomic_load_n(slot, __ATOMIC_ACQUIRE);
}

static void slot_store(volatile uint32_t *slot, uint32_t value)
{
    __atomic_store_n(slot, value, __ATOMIC_RELEASE);
}

static unsigned int registry_bucket(uint32_t raw_handle)
{
    raw_handle ^= raw_handle >> 16;
    raw_handle ^= raw_handle >> 8;
    return raw_handle & (ASM2_REGISTRY_BUCKETS - 1u);
}

/*
 * Readers need no registry mutex.  Entries are immutable and never freed;
 * publishing the bucket head and active flag with release/acquire ordering is
 * enough.  This keeps the hot mutex/cond path from taking a second host lock.
 */
static struct asm2_bridge_object *registry_find(
    uint32_t raw_handle, const volatile uint32_t *owner,
    enum asm2_object_kind kind)
{
    struct asm2_bridge_object *object;

    if (raw_handle == 0)
        return NULL;

    for (object = __atomic_load_n(&registry[registry_bucket(raw_handle)],
                                  __ATOMIC_ACQUIRE);
         object;
         object = __atomic_load_n(&object->active_next, __ATOMIC_ACQUIRE)) {
        if (object->raw_handle == raw_handle && object->owner == owner &&
            object->magic == ASM2_BRIDGE_MAGIC && object->kind == kind &&
            __atomic_load_n(&object->active, __ATOMIC_ACQUIRE))
            return object;
    }
    return NULL;
}

static int object_allocate_locked(volatile uint32_t *owner,
                                  enum asm2_object_kind kind,
                                  struct asm2_bridge_object **result)
{
    struct asm2_bridge_object *object;
    uintptr_t address;

    object = calloc(1, sizeof(*object));
    if (!object)
        return ENOMEM;

    address = (uintptr_t)object;
    if (address == 0 || address > UINT32_MAX) {
        free(object);
        return EOVERFLOW;
    }

    object->magic = ASM2_BRIDGE_MAGIC;
    object->kind = kind;
    object->active = true;
    object->owner = owner;
    object->raw_handle = (uint32_t)address;
    *result = object;
    return 0;
}

static void object_publish_locked(struct asm2_bridge_object *object)
{
    unsigned int bucket = registry_bucket(object->raw_handle);

    object->active_next =
        __atomic_load_n(&registry[bucket], __ATOMIC_RELAXED);
    __atomic_store_n(&registry[bucket], object, __ATOMIC_RELEASE);
    slot_store(object->owner, object->raw_handle);
    ++registry_stats.created;
    ++registry_stats.active;
}

static void object_retire_locked(struct asm2_bridge_object *object)
{
    unsigned int bucket = registry_bucket(object->raw_handle);
    struct asm2_bridge_object **link = &registry[bucket];
    struct asm2_bridge_object *current;

    __atomic_store_n(&object->active, false, __ATOMIC_RELEASE);
    slot_store(object->owner, 0);
    current = __atomic_load_n(link, __ATOMIC_ACQUIRE);
    while (current && current != object) {
        link = &current->active_next;
        current = __atomic_load_n(link, __ATOMIC_ACQUIRE);
    }
    if (current == object) {
        struct asm2_bridge_object *next =
            __atomic_load_n(&object->active_next, __ATOMIC_ACQUIRE);
        __atomic_store_n(link, next, __ATOMIC_RELEASE);
    }
    object->retired_next = retired_objects;
    retired_objects = object;
    if (registry_stats.active != 0)
        --registry_stats.active;
    ++registry_stats.retired;
}

void asm2_pthread_bridge_get_stats(struct asm2_pthread_bridge_stats *stats)
{
    if (!stats)
        return;
    pthread_mutex_lock(&registry_lock);
    *stats = registry_stats;
    stats->mutex_handoffs =
        __atomic_load_n(&mutex_handoff_count, __ATOMIC_RELAXED);
    for (unsigned int bucket = 0; bucket < ASM2_REGISTRY_BUCKETS; ++bucket) {
        uint32_t length = 0;
        for (struct asm2_bridge_object *object =
                 __atomic_load_n(&registry[bucket], __ATOMIC_ACQUIRE);
             object;
             object = __atomic_load_n(&object->active_next,
                                      __ATOMIC_ACQUIRE))
            ++length;
        if (length > stats->longest_active_bucket)
            stats->longest_active_bucket = length;
    }
    pthread_mutex_unlock(&registry_lock);
}

static int object_already_active_locked(volatile uint32_t *slot,
                                        enum asm2_object_kind kind)
{
    return registry_find(slot_load(slot), slot, kind) != NULL;
}

static struct asm2_bridge_object *object_get(const void *guest_object,
                                             enum asm2_object_kind kind,
                                             int *error)
{
    const volatile uint32_t *slot;
    struct asm2_bridge_object *object;

    if (!guest_object) {
        *error = EINVAL;
        return NULL;
    }

    slot = guest_const_slot(guest_object);
    object = registry_find(slot_load(slot), slot, kind);

    *error = object ? 0 : EINVAL;
    return object;
}

static int guest_mutex_type_to_host(int guest_type, int *host_type)
{
    switch (guest_type) {
    case 0: /* PTHREAD_MUTEX_NORMAL / PTHREAD_MUTEX_DEFAULT */
        *host_type = PTHREAD_MUTEX_NORMAL;
        return 0;
    case 1: /* PTHREAD_MUTEX_RECURSIVE */
        *host_type = PTHREAD_MUTEX_RECURSIVE;
        return 0;
    case 2: /* PTHREAD_MUTEX_ERRORCHECK */
        *host_type = PTHREAD_MUTEX_ERRORCHECK;
        return 0;
    default:
        return EINVAL;
    }
}

static int host_mutex_type_to_guest(int host_type, int *guest_type)
{
    if (host_type == PTHREAD_MUTEX_RECURSIVE) {
        *guest_type = 1;
        return 0;
    }
    if (host_type == PTHREAD_MUTEX_ERRORCHECK) {
        *guest_type = 2;
        return 0;
    }
    if (host_type == PTHREAD_MUTEX_NORMAL ||
        host_type == PTHREAD_MUTEX_DEFAULT) {
        *guest_type = 0;
        return 0;
    }
    return EINVAL;
}

/* Decode only old-bionic static mutex initializer bits, never live state. */
static int static_mutex_type(uint32_t raw, int *guest_type)
{
    unsigned int encoded_type;

    /* bit 13 is the process-shared flag; bits 14..15 encode the type */
    if ((raw & ~UINT32_C(0x0000e000)) != 0)
        return EINVAL;

    encoded_type = (raw >> 14) & 3u;
    if (encoded_type > 2u)
        return EINVAL;
    *guest_type = (int)encoded_type;
    return 0;
}

static int mutex_host_init(struct asm2_bridge_object *object, int guest_type)
{
    pthread_mutexattr_t attr;
    int host_type;
    int result;

    result = guest_mutex_type_to_host(guest_type, &host_type);
    if (result != 0)
        return result;
    result = pthread_mutexattr_init(&attr);
    if (result != 0)
        return result;
    result = pthread_mutexattr_settype(&attr, host_type);
    if (result == 0)
        result = pthread_mutex_init(&object->host.mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    if (result == 0)
        object->guest_mutex_type = guest_type;
    return result;
}

static struct asm2_bridge_object *mutex_get_or_create(void *guest_mutex,
                                                       int *error)
{
    volatile uint32_t *slot;
    struct asm2_bridge_object *object = NULL;
    uint32_t raw;
    int guest_type;
    int result;

    if (!guest_mutex) {
        *error = EINVAL;
        return NULL;
    }
    slot = guest_slot(guest_mutex);

    raw = slot_load(slot);
    object = registry_find(raw, slot, ASM2_OBJECT_MUTEX);
    if (object) {
        *error = 0;
        return object;
    }

    pthread_mutex_lock(&registry_lock);
    raw = slot_load(slot);
    object = registry_find(raw, slot, ASM2_OBJECT_MUTEX);
    if (object) {
        pthread_mutex_unlock(&registry_lock);
        *error = 0;
        return object;
    }

    result = static_mutex_type(raw, &guest_type);
    if (result == 0)
        result = object_allocate_locked(slot, ASM2_OBJECT_MUTEX, &object);
    if (result == 0)
        result = mutex_host_init(object, guest_type);
    if (result == 0)
        object_publish_locked(object);
    else if (object)
        free(object);
    pthread_mutex_unlock(&registry_lock);

    *error = result;
    return result == 0 ? object : NULL;
}

static uintptr_t mutex_current_owner(void)
{
    pthread_t thread = pthread_self();
    uintptr_t token = 0;
    size_t count = sizeof(thread) < sizeof(token) ? sizeof(thread)
                                                  : sizeof(token);

    memcpy(&token, &thread, count);
    return token ? token : UINTPTR_MAX;
}

static void mutex_record_acquired(struct asm2_bridge_object *object)
{
    uintptr_t current = mutex_current_owner();
    uintptr_t owner = __atomic_load_n(&object->mutex_owner,
                                      __ATOMIC_ACQUIRE);

    if (owner == current) {
        __atomic_add_fetch(&object->mutex_depth, 1u, __ATOMIC_RELAXED);
        return;
    }
    __atomic_store_n(&object->mutex_depth, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&object->mutex_owner, current, __ATOMIC_RELEASE);
}

static int mutex_record_releasing(struct asm2_bridge_object *object,
                                  uint32_t *previous_depth)
{
    uintptr_t current = mutex_current_owner();
    uintptr_t owner = __atomic_load_n(&object->mutex_owner,
                                      __ATOMIC_ACQUIRE);
    uint32_t depth = __atomic_load_n(&object->mutex_depth,
                                     __ATOMIC_RELAXED);

    if (owner != current || depth == 0)
        return EPERM;
    if (previous_depth)
        *previous_depth = depth;
    if (depth > 1) {
        __atomic_store_n(&object->mutex_depth, depth - 1u,
                         __ATOMIC_RELAXED);
    } else {
        __atomic_store_n(&object->mutex_owner, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&object->mutex_depth, 0, __ATOMIC_RELAXED);
    }
    return 0;
}

static void mutex_record_restore(struct asm2_bridge_object *object,
                                 uint32_t depth)
{
    __atomic_store_n(&object->mutex_depth, depth, __ATOMIC_RELAXED);
    __atomic_store_n(&object->mutex_owner, mutex_current_owner(),
                     __ATOMIC_RELEASE);
}

static int cond_host_init(struct asm2_bridge_object *object)
{
    /* Android/Bionic's default pthread_cond_t clock is CLOCK_REALTIME, and
     * this exact guest constructs all six absolute timed-wait deadlines from
     * gettimeofday().  A monotonic host cond would interpret those epoch
     * seconds as decades in the future. */
    return pthread_cond_init(&object->host.cond, NULL);
}

static struct asm2_bridge_object *cond_get_or_create(void *guest_cond,
                                                      int *error)
{
    volatile uint32_t *slot;
    struct asm2_bridge_object *object = NULL;
    uint32_t raw;
    int result = 0;

    if (!guest_cond) {
        *error = EINVAL;
        return NULL;
    }
    slot = guest_slot(guest_cond);

    raw = slot_load(slot);
    object = registry_find(raw, slot, ASM2_OBJECT_COND);
    if (object) {
        *error = 0;
        return object;
    }

    pthread_mutex_lock(&registry_lock);
    raw = slot_load(slot);
    object = registry_find(raw, slot, ASM2_OBJECT_COND);
    if (object) {
        pthread_mutex_unlock(&registry_lock);
        *error = 0;
        return object;
    }

    /* Old bionic uses only low flag bits in static cond initializers. */
    if ((raw & ~UINT32_C(0x3)) != 0)
        result = EINVAL;
    if (result == 0)
        result = object_allocate_locked(slot, ASM2_OBJECT_COND, &object);
    if (result == 0)
        result = cond_host_init(object);
    if (result == 0)
        object_publish_locked(object);
    else if (object)
        free(object);
    pthread_mutex_unlock(&registry_lock);

    *error = result;
    return result == 0 ? object : NULL;
}

static int sem_host_init(struct asm2_bridge_object *object,
                         unsigned int value)
{
    if (sem_init(&object->host.sem, 0, value) != 0)
        return errno ? errno : EINVAL;
    return 0;
}

static struct asm2_bridge_object *sem_get_or_create(void *guest_sem,
                                                     int *error)
{
    volatile uint32_t *slot;
    struct asm2_bridge_object *object = NULL;
    uint32_t raw;
    unsigned int initial_value;
    int result = 0;

    if (!guest_sem) {
        *error = EINVAL;
        return NULL;
    }
    slot = guest_slot(guest_sem);

    raw = slot_load(slot);
    object = registry_find(raw, slot, ASM2_OBJECT_SEM);
    if (object) {
        *error = 0;
        return object;
    }

    pthread_mutex_lock(&registry_lock);
    raw = slot_load(slot);
    object = registry_find(raw, slot, ASM2_OBJECT_SEM);
    if (object) {
        pthread_mutex_unlock(&registry_lock);
        *error = 0;
        return object;
    }

    /* sem_t has no POSIX static initializer; zero is the useful lazy case. */
    if (raw > UINT32_C(0x7fff))
        result = EINVAL;
    initial_value = raw;
    if (result == 0)
        result = object_allocate_locked(slot, ASM2_OBJECT_SEM, &object);
    if (result == 0)
        result = sem_host_init(object, initial_value);
    if (result == 0)
        object_publish_locked(object);
    else if (object)
        free(object);
    pthread_mutex_unlock(&registry_lock);

    *error = result;
    return result == 0 ? object : NULL;
}

int asm2_pthread_attr_init(void *guest_attr)
{
    volatile uint32_t *slot;
    struct asm2_bridge_object *object = NULL;
    int result;

    if (!guest_attr)
        return EINVAL;
    slot = guest_slot(guest_attr);

    pthread_mutex_lock(&registry_lock);
    if (object_already_active_locked(slot, ASM2_OBJECT_ATTR)) {
        pthread_mutex_unlock(&registry_lock);
        return EBUSY;
    }
    result = object_allocate_locked(slot, ASM2_OBJECT_ATTR, &object);
    if (result == 0)
        result = pthread_attr_init(&object->host.attr);
    if (result == 0)
        object_publish_locked(object);
    else if (object)
        free(object);
    pthread_mutex_unlock(&registry_lock);
    return result;
}

int asm2_pthread_attr_destroy(void *guest_attr)
{
    volatile uint32_t *slot;
    struct asm2_bridge_object *object;
    int result;

    if (!guest_attr)
        return EINVAL;
    slot = guest_slot(guest_attr);
    pthread_mutex_lock(&registry_lock);
    object = registry_find(slot_load(slot), slot, ASM2_OBJECT_ATTR);
    if (!object) {
        pthread_mutex_unlock(&registry_lock);
        return EINVAL;
    }
    result = pthread_attr_destroy(&object->host.attr);
    if (result == 0)
        object_retire_locked(object);
    pthread_mutex_unlock(&registry_lock);
    return result;
}

int asm2_pthread_attr_getdetachstate(const void *guest_attr, int *detachstate)
{
    struct asm2_bridge_object *object;
    int host_state;
    int error;

    if (!detachstate)
        return EINVAL;
    object = object_get(guest_attr, ASM2_OBJECT_ATTR, &error);
    if (!object)
        return error;
    error = pthread_attr_getdetachstate(&object->host.attr, &host_state);
    if (error == 0)
        *detachstate = host_state == PTHREAD_CREATE_DETACHED ? 1 : 0;
    return error;
}

int asm2_pthread_attr_setdetachstate(void *guest_attr, int detachstate)
{
    struct asm2_bridge_object *object;
    int host_state;
    int error;

    if (detachstate == 0)
        host_state = PTHREAD_CREATE_JOINABLE;
    else if (detachstate == 1)
        host_state = PTHREAD_CREATE_DETACHED;
    else
        return EINVAL;

    object = object_get(guest_attr, ASM2_OBJECT_ATTR, &error);
    return object ? pthread_attr_setdetachstate(&object->host.attr, host_state)
                  : error;
}

int asm2_pthread_attr_getstacksize(const void *guest_attr, size_t *stacksize)
{
    struct asm2_bridge_object *object;
    int error;

    if (!stacksize)
        return EINVAL;
    object = object_get(guest_attr, ASM2_OBJECT_ATTR, &error);
    return object ? pthread_attr_getstacksize(&object->host.attr, stacksize)
                  : error;
}

int asm2_pthread_attr_setstacksize(void *guest_attr, size_t stacksize)
{
    struct asm2_bridge_object *object;
    int error;

    object = object_get(guest_attr, ASM2_OBJECT_ATTR, &error);
    return object ? pthread_attr_setstacksize(&object->host.attr, stacksize)
                  : error;
}

int asm2_pthread_mutexattr_init(void *guest_attr)
{
    volatile uint32_t *slot;
    struct asm2_bridge_object *object = NULL;
    int result;

    if (!guest_attr)
        return EINVAL;
    slot = guest_slot(guest_attr);

    pthread_mutex_lock(&registry_lock);
    if (object_already_active_locked(slot, ASM2_OBJECT_MUTEX_ATTR)) {
        pthread_mutex_unlock(&registry_lock);
        return EBUSY;
    }
    result = object_allocate_locked(slot, ASM2_OBJECT_MUTEX_ATTR, &object);
    if (result == 0)
        result = pthread_mutexattr_init(&object->host.mutex_attr);
    if (result == 0) {
        object->guest_mutex_type = 0;
        object_publish_locked(object);
    } else if (object) {
        free(object);
    }
    pthread_mutex_unlock(&registry_lock);
    return result;
}

int asm2_pthread_mutexattr_destroy(void *guest_attr)
{
    volatile uint32_t *slot;
    struct asm2_bridge_object *object;
    int result;

    if (!guest_attr)
        return EINVAL;
    slot = guest_slot(guest_attr);
    pthread_mutex_lock(&registry_lock);
    object = registry_find(slot_load(slot), slot, ASM2_OBJECT_MUTEX_ATTR);
    if (!object) {
        pthread_mutex_unlock(&registry_lock);
        return EINVAL;
    }
    result = pthread_mutexattr_destroy(&object->host.mutex_attr);
    if (result == 0)
        object_retire_locked(object);
    pthread_mutex_unlock(&registry_lock);
    return result;
}

int asm2_pthread_mutexattr_gettype(const void *guest_attr, int *type)
{
    struct asm2_bridge_object *object;
    int host_type;
    int error;

    if (!type)
        return EINVAL;
    object = object_get(guest_attr, ASM2_OBJECT_MUTEX_ATTR, &error);
    if (!object)
        return error;
    error = pthread_mutexattr_gettype(&object->host.mutex_attr, &host_type);
    return error == 0 ? host_mutex_type_to_guest(host_type, type) : error;
}

int asm2_pthread_mutexattr_settype(void *guest_attr, int type)
{
    struct asm2_bridge_object *object;
    int host_type;
    int error;

    error = guest_mutex_type_to_host(type, &host_type);
    if (error != 0)
        return error;
    object = object_get(guest_attr, ASM2_OBJECT_MUTEX_ATTR, &error);
    if (!object)
        return error;
    error = pthread_mutexattr_settype(&object->host.mutex_attr, host_type);
    if (error == 0)
        object->guest_mutex_type = type;
    return error;
}

int asm2_pthread_mutex_init(void *guest_mutex, const void *guest_attr)
{
    volatile uint32_t *slot;
    struct asm2_bridge_object *object = NULL;
    struct asm2_bridge_object *attr = NULL;
    int guest_type = 0;
    int error = 0;

    if (!guest_mutex)
        return EINVAL;
    slot = guest_slot(guest_mutex);

    if (guest_attr) {
        attr = object_get(guest_attr, ASM2_OBJECT_MUTEX_ATTR, &error);
        if (!attr)
            return error;
        guest_type = attr->guest_mutex_type;
    }

    pthread_mutex_lock(&registry_lock);
    if (object_already_active_locked(slot, ASM2_OBJECT_MUTEX)) {
        pthread_mutex_unlock(&registry_lock);
        return EBUSY;
    }
    error = object_allocate_locked(slot, ASM2_OBJECT_MUTEX, &object);
    if (error == 0)
        error = mutex_host_init(object, guest_type);
    if (error == 0)
        object_publish_locked(object);
    else if (object)
        free(object);
    pthread_mutex_unlock(&registry_lock);
    return error;
}

int asm2_pthread_mutex_destroy(void *guest_mutex)
{
    struct asm2_bridge_object *object;
    int error;

    object = mutex_get_or_create(guest_mutex, &error);
    if (!object)
        return error;

    pthread_mutex_lock(&registry_lock);
    error = pthread_mutex_destroy(&object->host.mutex);
    if (error == 0)
        object_retire_locked(object);
    pthread_mutex_unlock(&registry_lock);
    return error;
}

int asm2_pthread_mutex_lock(void *guest_mutex)
{
    struct asm2_bridge_object *object;
    int error;

    object = mutex_get_or_create(guest_mutex, &error);
    if (!object)
        return error;
    error = pthread_mutex_lock(&object->host.mutex);
    if (error == 0)
        mutex_record_acquired(object);
    return error;
}

int asm2_pthread_mutex_trylock(void *guest_mutex)
{
    struct asm2_bridge_object *object;
    int error;

    object = mutex_get_or_create(guest_mutex, &error);
    if (!object)
        return error;
    error = pthread_mutex_trylock(&object->host.mutex);
    if (error == 0)
        mutex_record_acquired(object);
    return error;
}

int asm2_pthread_mutex_unlock(void *guest_mutex)
{
    struct asm2_bridge_object *object;
    int error;
    uint32_t previous_depth = 0;

    object = mutex_get_or_create(guest_mutex, &error);
    if (!object)
        return error;
    error = mutex_record_releasing(object, &previous_depth);
    if (error != 0)
        return error;
    error = pthread_mutex_unlock(&object->host.mutex);
    if (error != 0)
        mutex_record_restore(object, previous_depth);
    return error;
}

int asm2_pthread_mutex_handoff_begin(
    void *guest_mutex, struct asm2_pthread_mutex_handoff *handoff)
{
    struct asm2_bridge_object *object;
    uint32_t previous_depth = 0;
    int error;

    if (!handoff)
        return -EINVAL;
    memset(handoff, 0, sizeof(*handoff));
    object = mutex_get_or_create(guest_mutex, &error);
    if (!object)
        return -error;
    if (__atomic_load_n(&object->mutex_owner, __ATOMIC_ACQUIRE) !=
            mutex_current_owner() ||
        __atomic_load_n(&object->mutex_depth, __ATOMIC_RELAXED) != 1u)
        return 0;
    error = mutex_record_releasing(object, &previous_depth);
    if (error != 0)
        return -error;
    error = pthread_mutex_unlock(&object->host.mutex);
    if (error != 0) {
        mutex_record_restore(object, previous_depth);
        return -error;
    }
    handoff->bridge_object = object;
    handoff->active = 1;
    __atomic_add_fetch(&mutex_handoff_count, 1u, __ATOMIC_RELAXED);
    return 1;
}

int asm2_pthread_mutex_handoff_end(
    struct asm2_pthread_mutex_handoff *handoff)
{
    if (!handoff || !handoff->active || !handoff->bridge_object)
        return EINVAL;
    struct asm2_bridge_object *object = handoff->bridge_object;
    int error = pthread_mutex_lock(&object->host.mutex);
    if (error == 0) {
        mutex_record_acquired(object);
        handoff->bridge_object = NULL;
        handoff->active = 0;
    }
    return error;
}

int asm2_pthread_cond_init(void *guest_cond, const void *guest_attr)
{
    volatile uint32_t *slot;
    struct asm2_bridge_object *object = NULL;
    int error;

    (void)guest_attr;
    if (!guest_cond)
        return EINVAL;
    slot = guest_slot(guest_cond);

    pthread_mutex_lock(&registry_lock);
    if (object_already_active_locked(slot, ASM2_OBJECT_COND)) {
        pthread_mutex_unlock(&registry_lock);
        return EBUSY;
    }
    error = object_allocate_locked(slot, ASM2_OBJECT_COND, &object);
    if (error == 0)
        error = cond_host_init(object);
    if (error == 0)
        object_publish_locked(object);
    else if (object)
        free(object);
    pthread_mutex_unlock(&registry_lock);
    return error;
}

int asm2_pthread_cond_destroy(void *guest_cond)
{
    struct asm2_bridge_object *object;
    int error;

    object = cond_get_or_create(guest_cond, &error);
    if (!object)
        return error;
    pthread_mutex_lock(&registry_lock);
    error = pthread_cond_destroy(&object->host.cond);
    if (error == 0)
        object_retire_locked(object);
    pthread_mutex_unlock(&registry_lock);
    return error;
}

int asm2_pthread_cond_wait(void *guest_cond, void *guest_mutex)
{
    struct asm2_bridge_object *cond;
    struct asm2_bridge_object *mutex;
    int error;

    cond = cond_get_or_create(guest_cond, &error);
    if (!cond)
        return error;
    mutex = mutex_get_or_create(guest_mutex, &error);
    if (!mutex)
        return error;
    uint32_t previous_depth = 0;
    error = mutex_record_releasing(mutex, &previous_depth);
    if (error != 0)
        return error;
    if (previous_depth != 1u) {
        mutex_record_restore(mutex, previous_depth);
        return EINVAL;
    }
    error = pthread_cond_wait(&cond->host.cond, &mutex->host.mutex);
    mutex_record_acquired(mutex);
    return error;
}

int asm2_pthread_cond_timedwait(void *guest_cond, void *guest_mutex,
                                const struct timespec *abstime)
{
    struct asm2_bridge_object *cond;
    struct asm2_bridge_object *mutex;
    int error;

    if (!abstime)
        return EINVAL;
    cond = cond_get_or_create(guest_cond, &error);
    if (!cond)
        return error;
    mutex = mutex_get_or_create(guest_mutex, &error);
    if (!mutex)
        return error;
    uint32_t previous_depth = 0;
    error = mutex_record_releasing(mutex, &previous_depth);
    if (error != 0)
        return error;
    if (previous_depth != 1u) {
        mutex_record_restore(mutex, previous_depth);
        return EINVAL;
    }
    error = pthread_cond_timedwait(&cond->host.cond,
                                   &mutex->host.mutex, abstime);
    mutex_record_acquired(mutex);
    return error;
}

int asm2_pthread_cond_signal(void *guest_cond)
{
    struct asm2_bridge_object *object;
    int error;

    object = cond_get_or_create(guest_cond, &error);
    return object ? pthread_cond_signal(&object->host.cond) : error;
}

int asm2_pthread_cond_broadcast(void *guest_cond)
{
    struct asm2_bridge_object *object;
    int error;

    object = cond_get_or_create(guest_cond, &error);
    return object ? pthread_cond_broadcast(&object->host.cond) : error;
}

static int guest_thread_to_host(uint32_t guest_thread, pthread_t *host_thread)
{
    if (sizeof(*host_thread) < sizeof(guest_thread))
        return EOVERFLOW;
    memset(host_thread, 0, sizeof(*host_thread));
    memcpy(host_thread, &guest_thread, sizeof(guest_thread));
    return 0;
}

static int host_thread_to_guest(pthread_t host_thread, uint32_t *guest_thread)
{
    unsigned char high_bytes[sizeof(host_thread)];
    size_t index;

    if (!guest_thread || sizeof(host_thread) < sizeof(*guest_thread))
        return EOVERFLOW;
    memcpy(guest_thread, &host_thread, sizeof(*guest_thread));

    if (sizeof(host_thread) > sizeof(*guest_thread)) {
        memcpy(high_bytes, &host_thread, sizeof(host_thread));
        for (index = sizeof(*guest_thread); index < sizeof(host_thread); ++index) {
            if (high_bytes[index] != 0)
                return EOVERFLOW;
        }
    }
    return 0;
}

int asm2_pthread_create(uint32_t *guest_thread, const void *guest_attr,
                        void *(*start_routine)(void *), void *arg)
{
    struct asm2_bridge_object *attr = NULL;
    const pthread_attr_t *host_attr = NULL;
    pthread_t host_thread;
    int error;

    if (!guest_thread || !start_routine)
        return EINVAL;
    if (guest_attr) {
        attr = object_get(guest_attr, ASM2_OBJECT_ATTR, &error);
        if (!attr)
            return error;
        host_attr = &attr->host.attr;
    }

    error = pthread_create(&host_thread, host_attr, start_routine, arg);
    if (error != 0)
        return error;
    error = host_thread_to_guest(host_thread, guest_thread);
    if (error != 0) {
        pthread_detach(host_thread);
        return error;
    }
    return 0;
}

int asm2_pthread_detach(uint32_t guest_thread)
{
    pthread_t host_thread;
    int error = guest_thread_to_host(guest_thread, &host_thread);

    return error == 0 ? pthread_detach(host_thread) : error;
}

int asm2_pthread_equal(uint32_t first, uint32_t second)
{
    pthread_t host_first;
    pthread_t host_second;

    if (guest_thread_to_host(first, &host_first) != 0 ||
        guest_thread_to_host(second, &host_second) != 0)
        return 0;
    return pthread_equal(host_first, host_second);
}

int asm2_pthread_getschedparam(uint32_t guest_thread, int *policy,
                              struct sched_param *param)
{
    pthread_t host_thread;
    int error = guest_thread_to_host(guest_thread, &host_thread);

    if (!policy || !param)
        return EINVAL;
    return error == 0 ? pthread_getschedparam(host_thread, policy, param)
                      : error;
}

int asm2_pthread_setschedparam(uint32_t guest_thread, int policy,
                              const struct sched_param *param)
{
    pthread_t host_thread;
    int error = guest_thread_to_host(guest_thread, &host_thread);

    if (!param)
        return EINVAL;
    return error == 0 ? pthread_setschedparam(host_thread, policy, param)
                      : error;
}

void *asm2_pthread_getspecific(uint32_t guest_key)
{
    return pthread_getspecific((pthread_key_t)guest_key);
}

int asm2_pthread_setspecific(uint32_t guest_key, const void *value)
{
    return pthread_setspecific((pthread_key_t)guest_key, value);
}

int asm2_pthread_join(uint32_t guest_thread, void **retval)
{
    pthread_t host_thread;
    int error = guest_thread_to_host(guest_thread, &host_thread);

    return error == 0 ? pthread_join(host_thread, retval) : error;
}

int asm2_pthread_key_create(uint32_t *guest_key,
                            void (*destructor)(void *))
{
    pthread_key_t host_key;
    int error;

    if (!guest_key)
        return EINVAL;
    error = pthread_key_create(&host_key, destructor);
    if (error == 0)
        *guest_key = (uint32_t)host_key;
    return error;
}

int asm2_pthread_key_delete(uint32_t guest_key)
{
    return pthread_key_delete((pthread_key_t)guest_key);
}

uint32_t asm2_pthread_self(void)
{
    uint32_t guest_thread = 0;

    (void)host_thread_to_guest(pthread_self(), &guest_thread);
    return guest_thread;
}

struct asm2_once_cleanup {
    volatile uint32_t *control;
};

static void once_init_cancelled(void *opaque)
{
    struct asm2_once_cleanup *cleanup = opaque;

    pthread_mutex_lock(&once_lock);
    if (slot_load(cleanup->control) == 1)
        slot_store(cleanup->control, 0);
    pthread_cond_broadcast(&once_cond);
    pthread_mutex_unlock(&once_lock);
}

int asm2_pthread_once(volatile uint32_t *guest_once,
                      void (*init_routine)(void))
{
    struct asm2_once_cleanup cleanup;
    uint32_t state;
    int old_cancel_state;

    if (!guest_once || !init_routine)
        return EINVAL;

    /* Do not let cancellation strand once_lock while this thread is waiting. */
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel_state);
    pthread_mutex_lock(&once_lock);
    while (slot_load(guest_once) == 1)
        pthread_cond_wait(&once_cond, &once_lock);

    state = slot_load(guest_once);
    if (state == 2) {
        pthread_mutex_unlock(&once_lock);
        pthread_setcancelstate(old_cancel_state, NULL);
        return 0;
    }
    if (state != 0) {
        pthread_mutex_unlock(&once_lock);
        pthread_setcancelstate(old_cancel_state, NULL);
        return EINVAL;
    }

    slot_store(guest_once, 1);
    pthread_mutex_unlock(&once_lock);

    cleanup.control = guest_once;
    pthread_cleanup_push(once_init_cancelled, &cleanup);
    /* Restoring ENABLE here is safe: the cleanup is already installed. */
    pthread_setcancelstate(old_cancel_state, NULL);
    init_routine();

    pthread_mutex_lock(&once_lock);
    slot_store(guest_once, 2);
    pthread_cond_broadcast(&once_cond);
    pthread_mutex_unlock(&once_lock);
    pthread_cleanup_pop(0);
    return 0;
}

static int sem_result(int result)
{
    if (result == 0)
        return 0;
    if (errno == 0)
        errno = EINVAL;
    return -1;
}

int asm2_sem_init(void *guest_sem, int pshared, unsigned int value)
{
    volatile uint32_t *slot;
    struct asm2_bridge_object *object = NULL;
    int error;

    if (!guest_sem) {
        errno = EINVAL;
        return -1;
    }
    if (pshared != 0) {
        errno = ENOSYS;
        return -1;
    }
    slot = guest_slot(guest_sem);

    pthread_mutex_lock(&registry_lock);
    if (object_already_active_locked(slot, ASM2_OBJECT_SEM)) {
        pthread_mutex_unlock(&registry_lock);
        errno = EBUSY;
        return -1;
    }
    error = object_allocate_locked(slot, ASM2_OBJECT_SEM, &object);
    if (error == 0)
        error = sem_host_init(object, value);
    if (error == 0)
        object_publish_locked(object);
    else if (object)
        free(object);
    pthread_mutex_unlock(&registry_lock);

    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

int asm2_sem_destroy(void *guest_sem)
{
    struct asm2_bridge_object *object;
    int error;
    int result;

    object = sem_get_or_create(guest_sem, &error);
    if (!object) {
        errno = error;
        return -1;
    }
    pthread_mutex_lock(&registry_lock);
    result = sem_destroy(&object->host.sem);
    if (result == 0)
        object_retire_locked(object);
    pthread_mutex_unlock(&registry_lock);
    return sem_result(result);
}

int asm2_sem_post(void *guest_sem)
{
    struct asm2_bridge_object *object;
    int error;

    object = sem_get_or_create(guest_sem, &error);
    if (!object) {
        errno = error;
        return -1;
    }
    return sem_result(sem_post(&object->host.sem));
}

int asm2_sem_trywait(void *guest_sem)
{
    struct asm2_bridge_object *object;
    int error;

    object = sem_get_or_create(guest_sem, &error);
    if (!object) {
        errno = error;
        return -1;
    }
    return sem_result(sem_trywait(&object->host.sem));
}

int asm2_sem_wait(void *guest_sem)
{
    struct asm2_bridge_object *object;
    int error;

    object = sem_get_or_create(guest_sem, &error);
    if (!object) {
        errno = error;
        return -1;
    }
    return sem_result(sem_wait(&object->host.sem));
}

/* sem_timedwait NAO existia na tabela e caia no dlsym: a glibc do host
 * receberia o sem_t de 4 bytes do bionic e esperaria em cima de lixo — trava
 * silenciosa e INTERMITENTE (foi o que segurou o carregamento do mundo, com a
 * thread principal dormindo dentro de AgAudioChannelFMOD::_play). */
int asm2_sem_timedwait(void *guest_sem, const void *guest_abstime)
{
    struct asm2_bridge_object *object;
    int error;
    struct timespec ts;
    const int32_t *raw = (const int32_t *)guest_abstime;

    object = sem_get_or_create(guest_sem, &error);
    if (!object) {
        errno = error;
        return -1;
    }
    if (!raw)
        return sem_result(sem_wait(&object->host.sem));
    /* bionic ARM32: struct timespec = { long tv_sec; long tv_nsec; } (32 bits) */
    ts.tv_sec = (time_t)raw[0];
    ts.tv_nsec = (long)raw[1];
    return sem_result(sem_timedwait(&object->host.sem, &ts));
}

int asm2_sem_getvalue(void *guest_sem, int *value)
{
    struct asm2_bridge_object *object;
    int error;

    if (!value) {
        errno = EINVAL;
        return -1;
    }
    object = sem_get_or_create(guest_sem, &error);
    if (!object) {
        errno = error;
        return -1;
    }
    return sem_result(sem_getvalue(&object->host.sem, value));
}
