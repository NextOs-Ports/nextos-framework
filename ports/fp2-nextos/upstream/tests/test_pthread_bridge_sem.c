#define _GNU_SOURCE

#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Include the implementation so this focused host test can inspect the
 * private generation/refcount state without adding a production test API. */
#include "../src/pthread_bridge.c"

void nx_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

static void require(int condition, const char *message)
{
    if (condition)
        return;
    fprintf(stderr, "fp2 pthread bridge semaphore test FAIL: %s\n", message);
    exit(1);
}

typedef struct {
    bridge_sem *entry;
    unsigned generation;
    unsigned refs;
    int initialized;
} sem_snapshot;

static int snapshot_mapping(void *key, sem_snapshot *snapshot)
{
    int found = 0;
    pthread_mutex_lock(&bridge_lock);
    for (int i = 0; i < sem_n; i++) {
        bridge_sem *entry = sems[i];
        if (entry->key != key)
            continue;
        if (snapshot) {
            snapshot->entry = entry;
            snapshot->generation = entry->generation;
            snapshot->refs = entry->refs;
            snapshot->initialized = entry->initialized;
        }
        found = 1;
        break;
    }
    pthread_mutex_unlock(&bridge_lock);
    return found;
}

static void pause_milliseconds(long milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L,
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

static int wait_for_refs(void *key, unsigned refs)
{
    for (int attempt = 0; attempt < 2000; attempt++) {
        sem_snapshot snapshot = {0};
        if (snapshot_mapping(key, &snapshot) && snapshot.refs >= refs)
            return 1;
        pause_milliseconds(1);
    }
    return 0;
}

static void test_destroy_starts_clean_generation(void)
{
    uint32_t guest_sem = 0;
    sem_snapshot before = {0};
    sem_snapshot after = {0};
    int value = -1;

    require(b_sem_init(&guest_sem, 0, 0) == 0, "first init failed");
    require(snapshot_mapping(&guest_sem, &before),
            "first generation was not mapped");
    require(b_sem_post(&guest_sem) == 0 && b_sem_post(&guest_sem) == 0,
            "could not seed first generation");
    require(b_sem_getvalue(&guest_sem, &value) == 0 && value == 2,
            "first generation did not retain its two posts");

    require(b_sem_destroy(&guest_sem) == 0, "destroy failed");
    require(!snapshot_mapping(&guest_sem, NULL),
            "destroy left the guest address mapped");
    require(b_sem_init(&guest_sem, 0, 0) == 0, "second init failed");
    require(snapshot_mapping(&guest_sem, &after),
            "second generation was not mapped");
    require(after.generation != before.generation,
            "re-init reused the destroyed generation");
    require(b_sem_getvalue(&guest_sem, &value) == 0 && value == 0,
            "new generation inherited the old count");
    errno = 0;
    require(b_sem_trywait(&guest_sem) == -1 && errno == EAGAIN,
            "new zero-count generation had a stale token");
    require(b_sem_destroy(&guest_sem) == 0, "second destroy failed");
}

static void test_preinit_post_is_preserved(void)
{
    uint32_t guest_sem = 0;
    sem_snapshot provisional = {0};
    sem_snapshot initialized = {0};
    int value = -1;

    require(b_sem_post(&guest_sem) == 0, "pre-init post failed");
    require(snapshot_mapping(&guest_sem, &provisional),
            "pre-init post did not create a provisional generation");
    require(!provisional.initialized,
            "provisional generation was marked initialized too early");
    require(b_sem_init(&guest_sem, 0, 0) == 0,
            "init after provisional post failed");
    require(snapshot_mapping(&guest_sem, &initialized),
            "initialized provisional generation disappeared");
    require(initialized.generation == provisional.generation,
            "init dropped the compatible pre-init generation");
    require(initialized.initialized,
            "init did not mark the provisional generation initialized");
    require(b_sem_getvalue(&guest_sem, &value) == 0 && value == 1,
            "init dropped the pre-init post");
    require(b_sem_wait(&guest_sem) == 0, "preserved pre-init post was unusable");
    require(b_sem_destroy(&guest_sem) == 0,
            "destroy after provisional test failed");
}

typedef struct {
    void *key;
    atomic_int done;
    int result;
} waiter_context;

static void *waiter_main(void *opaque)
{
    waiter_context *context = opaque;
    context->result = b_sem_wait(context->key);
    atomic_store_explicit(&context->done, 1, memory_order_release);
    return NULL;
}

static void test_destroy_with_concurrent_waiter(void)
{
    uint32_t guest_sem = 0;
    waiter_context context = { .key = &guest_sem, .result = -999 };
    pthread_t waiter;
    sem_snapshot old_generation = {0};
    sem_snapshot new_generation = {0};
    int provisional = 0;

    atomic_init(&context.done, 0);
    require(b_sem_init(&guest_sem, 0, 0) == 0,
            "concurrent test init failed");
    require(pthread_create(&waiter, NULL, waiter_main, &context) == 0,
            "could not create waiter thread");
    require(wait_for_refs(&guest_sem, 2),
            "waiter did not acquire a reference before timeout");
    require(snapshot_mapping(&guest_sem, &old_generation),
            "old generation disappeared before destroy");

    /* Model a post which acquired the old generation before destroy.  Its
     * reference keeps the host sem alive until sem_post itself has returned. */
    bridge_sem *old_post = sem_acquire(&guest_sem, 0, 0, &provisional);
    require(old_post == old_generation.entry && !provisional,
            "old-generation post did not acquire the mapped entry");

    require(b_sem_destroy(&guest_sem) == 0,
            "destroy with a blocked waiter failed");
    require(!snapshot_mapping(&guest_sem, NULL),
            "retired generation remained addressable by guest key");
    require(b_sem_init(&guest_sem, 0, 0) == 0,
            "fresh init while old waiter was blocked failed");
    require(snapshot_mapping(&guest_sem, &new_generation),
            "fresh generation was not mapped");
    require(new_generation.entry != old_post &&
            new_generation.generation != old_generation.generation,
            "fresh init aliased the retired waiter generation");

    require(b_sem_post(&guest_sem) == 0,
            "post to fresh generation failed");
    pause_milliseconds(20);
    require(!atomic_load_explicit(&context.done, memory_order_acquire),
            "fresh-generation post incorrectly woke the retired waiter");
    require(b_sem_trywait(&guest_sem) == 0,
            "fresh-generation token was consumed by the retired waiter");

    require(sem_post(&old_post->host) == 0,
            "could not complete the already-acquired old post");
    sem_release(old_post);

    struct timespec deadline;
    require(clock_gettime(CLOCK_REALTIME, &deadline) == 0,
            "could not read join deadline clock");
    deadline.tv_sec += 2;
    require(pthread_timedjoin_np(waiter, NULL, &deadline) == 0,
            "retired waiter did not finish before timeout");
    require(context.result == 0,
            "retired waiter returned an error after being posted");

    require(b_sem_destroy(&guest_sem) == 0,
            "destroy of fresh generation failed");
    require(sem_n == 0, "semaphore mappings leaked after concurrent test");
}

int main(void)
{
    test_destroy_starts_clean_generation();
    test_preinit_post_is_preserved();
    test_destroy_with_concurrent_waiter();
    puts("fp2 pthread bridge semaphore test: PASS "
         "generation-reset pre-init-preserved concurrent-waiter-no-uaf");
    return 0;
}
