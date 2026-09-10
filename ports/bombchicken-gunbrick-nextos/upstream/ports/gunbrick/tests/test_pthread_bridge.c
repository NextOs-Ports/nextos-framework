#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "nx_elf.h"

int nx_verbose;

void nx_log(const char *format, ...)
{
    (void)format;
}

/* Include the adapter implementation so the test can verify its private
 * table is drained; the public build still compiles the source normally. */
#include "../src/pthread_bridge.c"

int main(void)
{
    uint32_t guest_semaphores[4096] = { 0 };

    for (size_t i = 0; i < sizeof(guest_semaphores) /
                            sizeof(guest_semaphores[0]); i++) {
        assert(b_sem_init(&guest_semaphores[i], 0, 0) == 0);
        assert(b_sem_post(&guest_semaphores[i]) == 0);
        assert(b_sem_wait(&guest_semaphores[i]) == 0);
    }
    assert(sem_n == sizeof(guest_semaphores) /
                    sizeof(guest_semaphores[0]));
    for (size_t i = 0; i < sizeof(guest_semaphores) /
                            sizeof(guest_semaphores[0]); i++)
        assert(b_sem_destroy(&guest_semaphores[i]) == 0);
    /* Destroy is deliberately non-destructive: the engine can still hold a
     * translated host pointer for the reused guest address. */
    assert(sem_n == sizeof(guest_semaphores) /
                    sizeof(guest_semaphores[0]));

    /* Repeated access after the historical 512-entry boundary must reuse the
     * mapping instead of allocating another untracked semaphore each time. */
    size_t stable_count = sem_n;
    for (unsigned i = 0; i < 100000; i++) {
        assert(b_sem_post(&guest_semaphores[4095]) == 0);
        assert(b_sem_wait(&guest_semaphores[4095]) == 0);
    }
    assert(sem_n == stable_count);

    /* Unity can post before its matching init; that post must survive. */
    assert(b_sem_post(&guest_semaphores[0]) == 0);
    assert(b_sem_init(&guest_semaphores[0], 0, 0) == 0);
    assert(b_sem_wait(&guest_semaphores[0]) == 0);
    assert(b_sem_destroy(&guest_semaphores[0]) == 0);
    assert(sem_n == stable_count);

    for (size_t i = 0; i < sem_n; i++) {
        assert(sem_destroy(sems[i].s) == 0);
        free(sems[i].s);
    }
    free(sems);
    sems = NULL;
    sem_capacity = 0;
    puts("GUNBRICK PTHREAD BRIDGE TEST: PASS");
    return 0;
}
