/* SPDX-License-Identifier: GPL-3.0-only
 *
 * nxobs_perf — V3-PERF-01 (carried into V4) and the VSYNC/OBS receipt.
 *
 * Passive, read-only, bounded telemetry. The same rule as nxobs_mem applies
 * and is not negotiable here either:
 *
 *   - OBSERVING NEVER AUTHORIZES ACTING. This module never kills a process,
 *     never throttles a frame, never limits a save and never touches the
 *     game's own budget. It reads /proc entries the process already owns and
 *     it reports.
 *   - Cost is bounded by construction: no /proc read happens faster than the
 *     minimum interval, no matter how often the adapter ticks.
 *   - Per-frame spam is a QA failure, so the frame path only ever increments
 *     counters. A receipt is emitted when the adapter asks, never per frame.
 *   - Missing fields become a completeness mask, never a silent zero. A
 *     kernel without /proc/self/io is a smaller receipt, not a fake one.
 *   - Nothing is decided by device, CFW or firmware name.
 *
 * The milestones are exactly the ones the specification names, so a baseline
 * can be compared before and after a change instead of being argued about.
 */
#ifndef NXOBS_PERF_H
#define NXOBS_PERF_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXOBS_PERF_API_VERSION 1u

/* Ordered boot milestones. The adapter marks the ones it really reaches; an
 * unmarked milestone is reported as absent, never as zero. */
typedef enum nxobs_perf_milestone {
    NXOBS_PERF_LAUNCHER_START = 0,
    NXOBS_PERF_NXEXTRACT_START,
    NXOBS_PERF_DATA_GATE_DONE,
    NXOBS_PERF_SPLASH_SHOWN,
    NXOBS_PERF_ADAPTER_START,
    NXOBS_PERF_FIRST_FRAME,
    NXOBS_PERF_MENU_READY,
    NXOBS_PERF_MILESTONE_COUNT
} nxobs_perf_milestone;

enum {
    NXOBS_PERF_HAVE_THREADS = 1u << 0,
    NXOBS_PERF_HAVE_FDS = 1u << 1,
    NXOBS_PERF_HAVE_CPU = 1u << 2,
    NXOBS_PERF_HAVE_IO = 1u << 3,
    NXOBS_PERF_HAVE_RSS = 1u << 4,
    NXOBS_PERF_HAVE_ALL = (1u << 5) - 1u
};

typedef struct nxobs_perf_sample {
    uint64_t threads;
    uint64_t open_fds;
    uint64_t utime_ticks;
    uint64_t stime_ticks;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t rchar;
    uint64_t wchar;
    uint64_t cancelled_write_bytes;
    uint64_t vm_rss_kib;
    uint64_t vm_hwm_kib;
    unsigned mask; /* NXOBS_PERF_HAVE_* actually read */
} nxobs_perf_sample;

typedef struct nxobs_perf {
    /* Configuration: fill before nxobs_perf_init. */
    double min_interval_seconds;               /* floor 1.0 */
    void (*emit)(void *ctx, const char *line); /* required */
    void *emit_ctx;
    /* Tests only: alternative paths (NULL = the real /proc). */
    const char *status_path;
    const char *io_path;
    const char *fd_dir_path;
    /* Internal state. */
    struct timespec marks[NXOBS_PERF_MILESTONE_COUNT];
    unsigned marked_mask;
    struct timespec last_sample;
    int have_last;
    int initialized;
    /* Presentation counters. Only ever incremented on the frame path. */
    struct timespec present_first;
    struct timespec present_last;
    uint64_t present_count;
    int32_t requested_swap_interval;
    int32_t effective_swap_interval;
    int have_present;
    int have_swap_interval;
} nxobs_perf;

/* Validate and normalize. Returns 0, or -1 when emit is NULL. Allocates
 * nothing; the caller owns the struct. */
int nxobs_perf_init(nxobs_perf *perf);

/* Record a boot milestone once. A repeated mark is ignored, because the first
 * time something happened is the measurement; the last time is noise. */
void nxobs_perf_mark(nxobs_perf *perf, nxobs_perf_milestone milestone);

/* Read the counters NOW, without any rate limit. */
void nxobs_perf_sample_now(const nxobs_perf *perf, nxobs_perf_sample *sample);

/* Rate-limited tick: emits one "[nxobs/perf] ..." line when the minimum
 * interval has elapsed. Returns 1 when it emitted, 0 when it suppressed. */
int nxobs_perf_tick(nxobs_perf *perf, const char *marker);

/* VSYNC/OBS (the AUD-22 item reserved for this cycle).
 *
 * The adapter declares the swap interval it REQUESTED and the one the driver
 * reported as EFFECTIVE; they are recorded separately on purpose, because a
 * driver that silently ignores the request is exactly what this receipt has to
 * make visible. Call once, after the context is current. */
void nxobs_perf_declare_swap_interval(nxobs_perf *perf, int32_t requested,
                                      int32_t effective);

/* Frame path. O(1), no I/O, no allocation, no logging. */
void nxobs_perf_present(nxobs_perf *perf);

/* Measured frames per second over the observed window, times 1000 to stay
 * integral. Returns 0 when fewer than two frames were presented. */
uint64_t nxobs_perf_fps_milli(const nxobs_perf *perf);

/* Milestone interval in milliseconds. Returns -1 when either end is unmarked
 * or the pair is out of order. */
int64_t nxobs_perf_interval_ms(const nxobs_perf *perf,
                               nxobs_perf_milestone from,
                               nxobs_perf_milestone to);

/* One-shot receipts. Both write at most cap-1 bytes plus NUL and return the
 * number of bytes that would have been written. */
size_t nxobs_perf_receipt(const nxobs_perf *perf, char *buf, size_t cap);
size_t nxobs_perf_vsync_receipt(const nxobs_perf *perf, char *buf, size_t cap);

const char *nxobs_perf_milestone_name(nxobs_perf_milestone milestone);

#ifdef __cplusplus
}
#endif

#endif /* NXOBS_PERF_H */
