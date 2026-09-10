/*
 * diagnostics.c -- opt-in, crash-resilient bring-up diagnostics.
 *
 * Nothing in this file is active in a normal run.  Physical Mali-450 tests
 * opt in with FP2_PERSIST_LOG and/or one of the two timeout variables.  The
 * persistent log is opened by the binary itself so the watchdog can fsync it;
 * redirecting stderr from a shell only leaves dirty VFAT pages behind when a
 * device needs a hard reset.
 */

#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>

#include "gb.h"
#include "nx_elf.h"

static int persistent_log;
static int startup_timeout;
static int frame_timeout;
static int heartbeat_interval;
static pid_t render_tid;
static uint64_t started_ns;
static atomic_uint_fast64_t last_progress_ns;
static atomic_ulong completed_frames;
static atomic_int render_ready;
static _Atomic(const char *) current_phase = "entry";

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static int seconds_env(const char *name)
{
    const char *value = getenv(name);
    if (!value || !*value)
        return 0;
    errno = 0;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (errno || end == value || *end || parsed <= 0 || parsed > 3600)
        return 0;
    return (int)parsed;
}

void fp2_diag_sync(void)
{
    fflush(stderr);
    if (persistent_log)
        (void)fdatasync(STDERR_FILENO);
}

void fp2_diag_open_persistent_log(void)
{
    const char *path = getenv("FP2_PERSIST_LOG");
    if (!path || !*path)
        return;

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW,
                  0600);
    if (fd < 0) {
        fprintf(stderr, "[fp2/diag] cannot open persistent log %s: %s\n",
                path, strerror(errno));
        return;
    }
    (void)fchmod(fd, 0600);
    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
        int saved = errno;
        if (fd > STDERR_FILENO)
            close(fd);
        errno = saved;
        fprintf(stderr, "[fp2/diag] cannot attach persistent log: %s\n",
                strerror(errno));
        return;
    }
    if (fd > STDERR_FILENO)
        close(fd);
    persistent_log = 1;
    fprintf(stderr, "[fp2/diag] persistent log attached pid=%d path=%s\n",
            (int)getpid(), path);
    fp2_diag_sync();
}

void fp2_diag_phase(const char *phase)
{
    if (!phase || !*phase)
        return;
    atomic_store_explicit(&current_phase, phase, memory_order_release);
    if (!started_ns)
        return;
    uint64_t now = monotonic_ns();
    fprintf(stderr, "[fp2/diag] phase=%s startup=%.3fs\n", phase,
            (double)(now - started_ns) / 1e9);
    fp2_diag_sync();
}

void fp2_diag_render_ready(void)
{
    uint64_t now = monotonic_ns();
    atomic_store_explicit(&last_progress_ns, now, memory_order_release);
    atomic_store_explicit(&render_ready, 1, memory_order_release);
    fp2_diag_phase("render-ready");
}

void fp2_diag_frame_complete(unsigned long frame)
{
    if (!started_ns)
        return;
    uint64_t now = monotonic_ns();
    unsigned long previous = atomic_exchange_explicit(
        &completed_frames, frame, memory_order_acq_rel);
    atomic_store_explicit(&last_progress_ns, now, memory_order_release);
    if (previous == 0 && frame != 0) {
        fprintf(stderr, "[fp2/diag] first-frame=%lu startup=%.3fs\n", frame,
                (double)(now - started_ns) / 1e9);
        fp2_diag_sync();
    }
}

static void copy_proc_record(const char *label, const char *path)
{
    char buffer[4096];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "[fp2/watchdog] %s unavailable: %s\n", label,
                strerror(errno));
        return;
    }
    ssize_t got = read(fd, buffer, sizeof buffer);
    int saved = errno;
    close(fd);
    if (got <= 0) {
        fprintf(stderr, "[fp2/watchdog] %s unreadable: %s\n", label,
                got == 0 ? "empty" : strerror(saved));
        return;
    }
    fprintf(stderr, "[fp2/watchdog] %s-begin\n", label);
    (void)write(STDERR_FILENO, buffer, (size_t)got);
    if (buffer[got - 1] != '\n')
        (void)write(STDERR_FILENO, "\n", 1);
    fprintf(stderr, "[fp2/watchdog] %s-end\n", label);
}

static void record_render_thread(void)
{
    char path[128];
    snprintf(path, sizeof path, "/proc/self/task/%d/wchan", (int)render_tid);
    copy_proc_record("render-wchan", path);
    snprintf(path, sizeof path, "/proc/self/task/%d/syscall", (int)render_tid);
    copy_proc_record("render-syscall", path);
    snprintf(path, sizeof path, "/proc/self/task/%d/status", (int)render_tid);
    copy_proc_record("render-status", path);
}

static void watchdog_timeout(const char *kind, uint64_t now,
                             uint64_t reference, unsigned long frame)
{
    const char *phase = atomic_load_explicit(&current_phase,
                                             memory_order_acquire);
    fprintf(stderr,
            "[fp2/watchdog] TIMEOUT kind=%s phase=%s frame=%lu "
            "startup=%.3fs stalled=%.3fs\n",
            kind, phase ? phase : "?", frame,
            (double)(now - started_ns) / 1e9,
            (double)(now - reference) / 1e9);
    record_render_thread();
    fp2_diag_sync();

    /* SIGUSR2 only samples the render thread.  The independent watchdog then
     * terminates the process itself, so a guest signal handler cannot swallow
     * the deadline as happened with the old SIGSEGV watchdog. */
    (void)syscall(SYS_tgkill, getpid(), render_tid, SIGUSR2);
    struct timespec grace = { 0, 300000000L };
    nanosleep(&grace, NULL);
    fp2_diag_sync();
    _exit(124);
}

static void *watchdog_thread(void *unused)
{
    (void)unused;
    uint64_t next_heartbeat = started_ns +
        (uint64_t)heartbeat_interval * UINT64_C(1000000000);
    for (;;) {
        struct timespec pause = { 1, 0 };
        nanosleep(&pause, NULL);
        uint64_t now = monotonic_ns();
        unsigned long frame = atomic_load_explicit(&completed_frames,
                                                   memory_order_acquire);
        uint64_t progress = atomic_load_explicit(&last_progress_ns,
                                                 memory_order_acquire);
        int ready = atomic_load_explicit(&render_ready, memory_order_acquire);
        const char *phase = atomic_load_explicit(&current_phase,
                                                 memory_order_acquire);

        if (heartbeat_interval > 0 && now >= next_heartbeat) {
            fprintf(stderr,
                    "[fp2/watchdog] heartbeat phase=%s frame=%lu "
                    "startup=%.3fs progress-age=%.3fs\n",
                    phase ? phase : "?", frame,
                    (double)(now - started_ns) / 1e9,
                    (double)(now - progress) / 1e9);
            fp2_diag_sync();
            next_heartbeat = now +
                (uint64_t)heartbeat_interval * UINT64_C(1000000000);
        }

        if (frame == 0 && startup_timeout > 0 &&
            now - started_ns >=
                (uint64_t)startup_timeout * UINT64_C(1000000000))
            watchdog_timeout("startup", now, started_ns, frame);

        if (ready && frame_timeout > 0 &&
            now - progress >=
                (uint64_t)frame_timeout * UINT64_C(1000000000))
            watchdog_timeout("frame-stall", now, progress, frame);
    }
    return NULL;
}

void fp2_diag_watchdog_start(void)
{
    startup_timeout = seconds_env("FP2_STARTUP_TIMEOUT");
    frame_timeout = seconds_env("FP2_WATCHDOG");
    heartbeat_interval = seconds_env("FP2_HEARTBEAT");
    if (!heartbeat_interval && persistent_log)
        heartbeat_interval = 2;
    if (!persistent_log && !startup_timeout && !frame_timeout &&
        !heartbeat_interval)
        return;

    render_tid = (pid_t)syscall(SYS_gettid);
    started_ns = monotonic_ns();
    atomic_store_explicit(&last_progress_ns, started_ns,
                          memory_order_release);
    pthread_t thread;
    int error = pthread_create(&thread, NULL, watchdog_thread, NULL);
    if (error) {
        fprintf(stderr, "[fp2/watchdog] cannot start: %s\n", strerror(error));
        fp2_diag_sync();
        return;
    }
    pthread_detach(thread);
    fprintf(stderr,
            "[fp2/watchdog] armed startup=%ds frame-stall=%ds heartbeat=%ds "
            "render-tid=%d\n",
            startup_timeout, frame_timeout, heartbeat_interval,
            (int)render_tid);
    fp2_diag_sync();
}
