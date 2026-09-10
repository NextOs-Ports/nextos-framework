/* SPDX-License-Identifier: GPL-3.0-only */

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "jni_refs.h"

typedef struct {
    gb_jni_ref_header ref;
    uint64_t value;
} test_object;

static _Atomic unsigned destroyed;

static void destroy_test_object(void *pointer)
{
    atomic_fetch_add_explicit(&destroyed, 1, memory_order_relaxed);
    free(pointer);
}

static test_object *new_test_object(int permanent, uint64_t value)
{
    test_object *object = calloc(1, sizeof *object);
    assert(object != NULL);
    assert(gb_jni_ref_init(object, permanent, destroy_test_object) == 0);
    object->value = value;
    return object;
}

static gb_jni_ref_stats stats(void)
{
    gb_jni_ref_stats value;
    gb_jni_ref_get_stats(&value);
    return value;
}

enum {
    STRESS_THREADS = 4,
    STRESS_ITERATIONS = 25000,
};

typedef struct {
    test_object *shared;
    unsigned worker;
} stress_context;

static void *stress_worker(void *opaque)
{
    stress_context *context = opaque;
    for (unsigned i = 0; i < STRESS_ITERATIONS; i++) {
        assert(gb_jni_ref_is_managed(context->shared));
        assert(gb_jni_ref_new_global(context->shared) == context->shared);
        gb_jni_ref_delete_global(context->shared);

        int token = gb_jni_ref_scope_begin();
        assert(token == 0);
        test_object *object = new_test_object(
            0, ((uint64_t)context->worker << 32) | i);
        assert(gb_jni_ref_is_managed(object));
        assert(gb_jni_ref_new_local(object) == object);
        gb_jni_ref_delete_local(object);
        gb_jni_ref_scope_end(token);
    }

    gb_jni_ref_stats value = stats();
    assert(value.thread_local_refs == 0);
    assert(value.local_depth == 0);
    return NULL;
}

int main(void)
{
    _Static_assert(sizeof(gb_jni_ref_header) == 24,
                   "JNI reference header ABI changed");

    int unmanaged = 0;
    assert(!gb_jni_ref_is_managed(&unmanaged));
    assert(gb_jni_ref_new_local(&unmanaged) == &unmanaged);
    assert(gb_jni_ref_new_global(&unmanaged) == &unmanaged);
    gb_jni_ref_delete_local(&unmanaged);
    gb_jni_ref_delete_global(&unmanaged);

    const uintptr_t invalid_handles[] = { (uintptr_t)1u, (uintptr_t)1024u };
    for (size_t i = 0;
         i < sizeof invalid_handles / sizeof invalid_handles[0]; i++) {
        void *invalid = (void *)invalid_handles[i];
        assert(!gb_jni_ref_is_managed(invalid));
        assert(!gb_jni_ref_is_permanent(invalid));
        gb_jni_ref_retain(invalid);
        gb_jni_ref_release(invalid);
        assert(gb_jni_ref_new_local(invalid) == invalid);
        assert(gb_jni_ref_new_global(invalid) == invalid);
        gb_jni_ref_delete_local(invalid);
        gb_jni_ref_delete_global(invalid);
        assert(gb_jni_ref_push_local_frame(0) == 0);
        assert(gb_jni_ref_pop_local_frame(invalid) == invalid);
    }

    int token = gb_jni_ref_scope_begin();
    assert(token == 0);
    test_object *local = new_test_object(0, 1);
    assert(gb_jni_ref_is_managed(local));
    assert(!gb_jni_ref_is_permanent(local));
    gb_jni_ref_scope_end(token);
    assert(atomic_load_explicit(&destroyed, memory_order_relaxed) == 1);

    token = gb_jni_ref_scope_begin();
    test_object *global = new_test_object(0, 2);
    assert(gb_jni_ref_new_global(global) == global);
    gb_jni_ref_scope_end(token);
    assert(atomic_load_explicit(&destroyed, memory_order_relaxed) == 1);
    assert(gb_jni_ref_is_managed(global));
    gb_jni_ref_delete_global(global);
    assert(atomic_load_explicit(&destroyed, memory_order_relaxed) == 2);

    token = gb_jni_ref_scope_begin();
    int nested = gb_jni_ref_push_local_frame(16);
    assert(nested == 0);
    test_object *promoted = new_test_object(0, 3);
    assert(gb_jni_ref_pop_local_frame(promoted) == promoted);
    assert(promoted->value == 3);
    assert(stats().local_depth == 1);
    gb_jni_ref_scope_end(token);
    assert(atomic_load_explicit(&destroyed, memory_order_relaxed) == 3);

    token = gb_jni_ref_scope_begin();
    test_object *duplicate = new_test_object(0, 4);
    assert(gb_jni_ref_new_local(duplicate) == duplicate);
    assert(stats().thread_local_refs == 2);
    gb_jni_ref_delete_local(duplicate);
    assert(stats().thread_local_refs == 1);
    assert(gb_jni_ref_is_managed(duplicate));
    gb_jni_ref_scope_end(token);
    assert(atomic_load_explicit(&destroyed, memory_order_relaxed) == 4);

    for (unsigned i = 0; i < 100000; i++) {
        token = gb_jni_ref_scope_begin();
        assert(token == 0);
        (void)new_test_object(0, i);
        gb_jni_ref_scope_end(token);
    }

    gb_jni_ref_stats before_permanent = stats();
    assert(before_permanent.objects_live == 0);
    assert(before_permanent.thread_local_refs == 0);
    assert(before_permanent.local_depth == 0);
    assert(before_permanent.objects_created ==
           before_permanent.objects_destroyed);
    assert(before_permanent.objects_peak <= 2);

    token = gb_jni_ref_scope_begin();
    test_object *shared = new_test_object(0, UINT64_C(0xc0ffee));
    assert(gb_jni_ref_new_global(shared) == shared);
    gb_jni_ref_scope_end(token);

    gb_jni_ref_stats before_stress = stats();
    pthread_t threads[STRESS_THREADS];
    stress_context contexts[STRESS_THREADS];
    for (unsigned i = 0; i < STRESS_THREADS; i++) {
        contexts[i].shared = shared;
        contexts[i].worker = i;
        assert(pthread_create(&threads[i], NULL, stress_worker,
                              &contexts[i]) == 0);
    }
    for (unsigned i = 0; i < STRESS_THREADS; i++)
        assert(pthread_join(threads[i], NULL) == 0);

    assert(gb_jni_ref_is_managed(shared));
    gb_jni_ref_delete_global(shared);
    assert(!gb_jni_ref_is_managed(shared));

    gb_jni_ref_stats after_stress = stats();
    assert(after_stress.objects_created ==
           before_stress.objects_created +
           STRESS_THREADS * STRESS_ITERATIONS);
    assert(after_stress.objects_destroyed ==
           before_stress.objects_destroyed +
           STRESS_THREADS * STRESS_ITERATIONS + 1);
    assert(after_stress.objects_live == 0);
    assert(after_stress.thread_local_refs == 0);
    assert(after_stress.local_depth == 0);

    test_object *permanent = new_test_object(1, 5);
    assert(gb_jni_ref_is_permanent(permanent));
    gb_jni_ref_retain(permanent);
    gb_jni_ref_release(permanent);
    gb_jni_ref_delete_local(permanent);
    gb_jni_ref_delete_global(permanent);
    assert(atomic_load_explicit(&destroyed, memory_order_relaxed) ==
           100005 + STRESS_THREADS * STRESS_ITERATIONS);
    assert(permanent->value == 5);

    gb_jni_ref_stats final = stats();
    assert(final.objects_live == 1);
    assert(final.objects_created == final.objects_destroyed + 1);
    assert(final.thread_local_refs == 0);
    assert(final.local_depth == 0);

    /* Permanent objects intentionally remain registered for process lifetime. */
    puts("JNI REF HOST TEST: PASS");
    return 0;
}
