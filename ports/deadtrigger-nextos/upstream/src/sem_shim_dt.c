/* Dead Trigger semaphore adapter layered over the proven opaque bionic shim. */
#define sh_sem_timedwait sh_sem_timedwait_legacy_unchecked
#include "../../terraria/src/sem_shim.c"
#undef sh_sem_timedwait

/*
 * Unity 2019 sometimes produces an absolute deadline by adding nanoseconds
 * without carrying values >= 1 second into tv_sec.  Bionic accepts this in
 * the Android flow used by the game, while glibc pthread_cond_timedwait()
 * correctly rejects it with EINVAL.  The legacy shim retried every error
 * except ETIMEDOUT, turning that EINVAL into a permanent 100% CPU loop during
 * scene transitions.  Normalize the same absolute instant and propagate every
 * wait error with POSIX sem_timedwait semantics instead of retrying forever.
 */
static void normalize_timespec(const struct timespec *source,
                               struct timespec *normalized) {
    *normalized = *source;
    if (normalized->tv_nsec >= 1000000000L ||
        normalized->tv_nsec <= -1000000000L) {
        time_t carry = normalized->tv_nsec / 1000000000L;
        normalized->tv_sec += carry;
        normalized->tv_nsec -= carry * 1000000000L;
    }
    if (normalized->tv_nsec < 0) {
        normalized->tv_nsec += 1000000000L;
        normalized->tv_sec--;
    }
}

int sh_sem_timedwait(void *s, const struct timespec *absolute_timeout) {
    struct mysem *m = sem_lookup(s, 1, 0);
    if (!m) {
        errno = ENOSPC;
        return -1;
    }

    struct timespec normalized_timeout;
    const struct timespec *deadline = absolute_timeout;
    if (absolute_timeout) {
        normalize_timespec(absolute_timeout, &normalized_timeout);
        deadline = &normalized_timeout;
        if (normalized_timeout.tv_sec != absolute_timeout->tv_sec ||
            normalized_timeout.tv_nsec != absolute_timeout->tv_nsec) {
            static int normalization_logged;
            if (!normalization_logged) {
                normalization_logged = 1;
                fprintf(stderr,
                        "[SEM] normalized Unity absolute timeout "
                        "(scene-transition compatibility)\n");
            }
        }
    }

    int rc = 0;
    pthread_mutex_lock(&m->m);
    while (m->count <= 0) {
        sigset_t old_mask;
        gc_wait_unblock(&old_mask);
        if (deadline)
            rc = pthread_cond_timedwait(&m->c, &m->m, deadline);
        else
            rc = pthread_cond_wait(&m->c, &m->m);
        gc_wait_restore(&old_mask);
        if (rc != 0) {
            errno = rc;
            rc = -1;
            break;
        }
    }
    if (rc == 0)
        m->count--;
    pthread_mutex_unlock(&m->m);
    return rc;
}
