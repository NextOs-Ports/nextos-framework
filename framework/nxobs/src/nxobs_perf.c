/* SPDX-License-Identifier: GPL-3.0-only
 * nxobs_perf — implementacao. Ver o contrato completo em nxobs_perf.h. */

#define _POSIX_C_SOURCE 200809L

#include "nxobs_perf.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { NXOBS_PERF_MARKER_MAX = 24 };

int nxobs_perf_init(nxobs_perf *perf)
{
    if (!perf || !perf->emit)
        return -1;
    if (!(perf->min_interval_seconds >= 1.0))
        perf->min_interval_seconds = 1.0;
    perf->marked_mask = 0u;
    perf->have_last = 0;
    perf->have_present = 0;
    perf->have_swap_interval = 0;
    perf->present_count = 0u;
    perf->requested_swap_interval = 0;
    perf->effective_swap_interval = 0;
    memset(perf->marks, 0, sizeof perf->marks);
    perf->initialized = 1;
    return 0;
}

static int nxobs_perf_milestone_valid(nxobs_perf_milestone milestone)
{
    return (int)milestone >= 0 &&
           (int)milestone < (int)NXOBS_PERF_MILESTONE_COUNT;
}

void nxobs_perf_mark(nxobs_perf *perf, nxobs_perf_milestone milestone)
{
    if (!perf || !perf->initialized || !nxobs_perf_milestone_valid(milestone))
        return;
    if (perf->marked_mask & (1u << (unsigned)milestone))
        return; /* The FIRST time is the measurement. */
    if (clock_gettime(CLOCK_MONOTONIC, &perf->marks[milestone]) != 0)
        return;
    perf->marked_mask |= 1u << (unsigned)milestone;
}

static void nxobs_perf_read_status(const char *path, nxobs_perf_sample *sample)
{
    FILE *file = fopen(path ? path : "/proc/self/status", "r");
    char line[256];
    uint64_t value;

    if (!file)
        return;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "Threads: %" SCNu64, &value) == 1) {
            sample->threads = value;
            sample->mask |= NXOBS_PERF_HAVE_THREADS;
        } else if (sscanf(line, "VmRSS: %" SCNu64 " kB", &value) == 1) {
            sample->vm_rss_kib = value;
            sample->mask |= NXOBS_PERF_HAVE_RSS;
        } else if (sscanf(line, "VmHWM: %" SCNu64 " kB", &value) == 1) {
            sample->vm_hwm_kib = value;
        }
    }
    fclose(file);
}

static void nxobs_perf_read_io(const char *path, nxobs_perf_sample *sample)
{
    /* /proc/self/io is absent on kernels built without TASK_IO_ACCOUNTING.
     * That is a smaller receipt, never a fabricated one. */
    FILE *file = fopen(path ? path : "/proc/self/io", "r");
    char line[256];
    uint64_t value;
    unsigned seen = 0u;

    if (!file)
        return;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "rchar: %" SCNu64, &value) == 1) {
            sample->rchar = value; seen |= 1u;
        } else if (sscanf(line, "wchar: %" SCNu64, &value) == 1) {
            sample->wchar = value; seen |= 2u;
        } else if (sscanf(line, "read_bytes: %" SCNu64, &value) == 1) {
            sample->read_bytes = value; seen |= 4u;
        } else if (sscanf(line, "write_bytes: %" SCNu64, &value) == 1) {
            sample->write_bytes = value; seen |= 8u;
        } else if (sscanf(line, "cancelled_write_bytes: %" SCNu64,
                          &value) == 1) {
            sample->cancelled_write_bytes = value; seen |= 16u;
        }
    }
    fclose(file);
    if (seen == 31u)
        sample->mask |= NXOBS_PERF_HAVE_IO;
}

static void nxobs_perf_read_fds(const char *path, nxobs_perf_sample *sample)
{
    const int is_self = path == NULL;
    DIR *directory = opendir(is_self ? "/proc/self/fd" : path);
    struct dirent *entry;
    uint64_t count = 0u;

    if (!directory)
        return;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        ++count;
    }
    closedir(directory);
    /* When reading our OWN /proc/self/fd, opendir's descriptor is itself
     * listed and must not be counted. A fixture directory has no such
     * self-reference, so the correction would be a lie there. */
    if (is_self && count > 0u)
        --count;
    sample->open_fds = count;
    sample->mask |= NXOBS_PERF_HAVE_FDS;
}

static void nxobs_perf_read_cpu(nxobs_perf_sample *sample)
{
    /* The CPU counters live in /proc/self/stat, which is a KERNEL FILE, not
     * the forbidden external `stat` command. */
    FILE *file = fopen("/proc/self/stat", "r");
    char buffer[4096];
    size_t length;
    char *cursor;
    int field;
    unsigned long long utime = 0ull;
    unsigned long long stime = 0ull;

    if (!file)
        return;
    length = fread(buffer, 1u, sizeof buffer - 1u, file);
    fclose(file);
    if (length == 0u)
        return;
    buffer[length] = '\0';
    /* The comm field may itself contain spaces and parentheses; the last ')'
     * is the only reliable anchor. */
    cursor = strrchr(buffer, ')');
    if (!cursor || cursor[1] == '\0')
        return;
    cursor += 2;
    for (field = 3; field <= 13; ++field) {
        cursor = strchr(cursor, ' ');
        if (!cursor)
            return;
        ++cursor;
    }
    if (sscanf(cursor, "%llu %llu", &utime, &stime) != 2)
        return;
    sample->utime_ticks = (uint64_t)utime;
    sample->stime_ticks = (uint64_t)stime;
    sample->mask |= NXOBS_PERF_HAVE_CPU;
}

void nxobs_perf_sample_now(const nxobs_perf *perf, nxobs_perf_sample *sample)
{
    if (!sample)
        return;
    memset(sample, 0, sizeof *sample);
    if (!perf)
        return;
    nxobs_perf_read_status(perf->status_path, sample);
    nxobs_perf_read_io(perf->io_path, sample);
    nxobs_perf_read_fds(perf->fd_dir_path, sample);
    if (!perf->status_path && !perf->io_path && !perf->fd_dir_path)
        nxobs_perf_read_cpu(sample);
}

static double nxobs_perf_elapsed(const struct timespec *from,
                                 const struct timespec *to)
{
    return (double)(to->tv_sec - from->tv_sec) +
           (double)(to->tv_nsec - from->tv_nsec) / 1e9;
}

static void nxobs_perf_sanitize(const char *marker, char *out, size_t cap)
{
    size_t index = 0u;
    if (cap == 0u)
        return;
    if (!marker)
        marker = "-";
    for (; index + 1u < cap && marker[index] != '\0'; ++index) {
        const char character = marker[index];
        if ((character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') ||
            character == '.' || character == '_' || character == '+' ||
            character == '-') {
            out[index] = character;
        } else {
            out[index] = '_';
        }
    }
    out[index] = '\0';
}

int nxobs_perf_tick(nxobs_perf *perf, const char *marker)
{
    struct timespec now;
    nxobs_perf_sample sample;
    char safe[NXOBS_PERF_MARKER_MAX + 1];
    char line[512];

    if (!perf || !perf->initialized || !perf->emit)
        return 0;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    if (perf->have_last &&
        nxobs_perf_elapsed(&perf->last_sample, &now) <
            perf->min_interval_seconds) {
        return 0;
    }
    perf->last_sample = now;
    perf->have_last = 1;
    nxobs_perf_sample_now(perf, &sample);
    nxobs_perf_sanitize(marker, safe, sizeof safe);
    (void)snprintf(line, sizeof line,
                   "[nxobs/perf] marker=%s mask=0x%x threads=%" PRIu64
                   " fds=%" PRIu64 " cpu_utime=%" PRIu64 " cpu_stime=%" PRIu64
                   " rss_kib=%" PRIu64 " hwm_kib=%" PRIu64
                   " read_bytes=%" PRIu64 " write_bytes=%" PRIu64
                   " rchar=%" PRIu64 " wchar=%" PRIu64
                   " cancelled_write_bytes=%" PRIu64 " frames=%" PRIu64,
                   safe, sample.mask, sample.threads, sample.open_fds,
                   sample.utime_ticks, sample.stime_ticks, sample.vm_rss_kib,
                   sample.vm_hwm_kib, sample.read_bytes, sample.write_bytes,
                   sample.rchar, sample.wchar, sample.cancelled_write_bytes,
                   perf->present_count);
    perf->emit(perf->emit_ctx, line);
    return 1;
}

void nxobs_perf_declare_swap_interval(nxobs_perf *perf, int32_t requested,
                                      int32_t effective)
{
    if (!perf || !perf->initialized)
        return;
    perf->requested_swap_interval = requested;
    perf->effective_swap_interval = effective;
    perf->have_swap_interval = 1;
}

void nxobs_perf_present(nxobs_perf *perf)
{
    struct timespec now;
    if (!perf || !perf->initialized)
        return;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return;
    if (!perf->have_present) {
        perf->present_first = now;
        perf->have_present = 1;
    }
    perf->present_last = now;
    ++perf->present_count;
}

uint64_t nxobs_perf_fps_milli(const nxobs_perf *perf)
{
    double window;
    if (!perf || !perf->have_present || perf->present_count < 2u)
        return 0u;
    window = nxobs_perf_elapsed(&perf->present_first, &perf->present_last);
    if (!(window > 0.0))
        return 0u;
    /* The first frame starts the window, so it bounds n-1 intervals. */
    return (uint64_t)(((double)(perf->present_count - 1u) * 1000.0) / window +
                      0.5);
}

int64_t nxobs_perf_interval_ms(const nxobs_perf *perf,
                               nxobs_perf_milestone from,
                               nxobs_perf_milestone to)
{
    double seconds;
    if (!perf || !nxobs_perf_milestone_valid(from) ||
        !nxobs_perf_milestone_valid(to))
        return -1;
    if (!(perf->marked_mask & (1u << (unsigned)from)) ||
        !(perf->marked_mask & (1u << (unsigned)to)))
        return -1;
    seconds = nxobs_perf_elapsed(&perf->marks[from], &perf->marks[to]);
    if (seconds < 0.0)
        return -1;
    return (int64_t)(seconds * 1000.0 + 0.5);
}

const char *nxobs_perf_milestone_name(nxobs_perf_milestone milestone)
{
    switch (milestone) {
    case NXOBS_PERF_LAUNCHER_START: return "launcher-start";
    case NXOBS_PERF_NXEXTRACT_START: return "nxextract-start";
    case NXOBS_PERF_DATA_GATE_DONE: return "data-gate-done";
    case NXOBS_PERF_SPLASH_SHOWN: return "splash-shown";
    case NXOBS_PERF_ADAPTER_START: return "adapter-start";
    case NXOBS_PERF_FIRST_FRAME: return "first-frame";
    case NXOBS_PERF_MENU_READY: return "menu-ready";
    default: return "unknown";
    }
}

static size_t nxobs_perf_copy(char *buf, size_t cap, const char *scratch,
                              int written)
{
    if (written < 0)
        return 0u;
    if (buf && cap > 0u) {
        size_t copy = (size_t)written < cap - 1u ? (size_t)written : cap - 1u;
        memcpy(buf, scratch, copy);
        buf[copy] = '\0';
    }
    return (size_t)written;
}

size_t nxobs_perf_receipt(const nxobs_perf *perf, char *buf, size_t cap)
{
    char scratch[640];
    nxobs_perf_sample sample;
    int written;
    if (!perf)
        return 0u;
    nxobs_perf_sample_now(perf, &sample);
    written = snprintf(
        scratch, sizeof scratch,
        "PERF: launcher_to_nxextract_ms=%" PRId64
        " data_gate_to_splash_ms=%" PRId64
        " adapter_to_first_frame_ms=%" PRId64
        " first_frame_to_menu_ready_ms=%" PRId64
        " mask=0x%x threads=%" PRIu64 " fds=%" PRIu64
        " cpu_utime=%" PRIu64 " cpu_stime=%" PRIu64
        " rss_kib=%" PRIu64 " hwm_kib=%" PRIu64
        " read_bytes=%" PRIu64 " write_bytes=%" PRIu64
        " rchar=%" PRIu64 " wchar=%" PRIu64
        " cancelled_write_bytes=%" PRIu64,
        nxobs_perf_interval_ms(perf, NXOBS_PERF_LAUNCHER_START,
                               NXOBS_PERF_NXEXTRACT_START),
        nxobs_perf_interval_ms(perf, NXOBS_PERF_DATA_GATE_DONE,
                               NXOBS_PERF_SPLASH_SHOWN),
        nxobs_perf_interval_ms(perf, NXOBS_PERF_ADAPTER_START,
                               NXOBS_PERF_FIRST_FRAME),
        nxobs_perf_interval_ms(perf, NXOBS_PERF_FIRST_FRAME,
                               NXOBS_PERF_MENU_READY),
        sample.mask, sample.threads, sample.open_fds, sample.utime_ticks,
        sample.stime_ticks, sample.vm_rss_kib, sample.vm_hwm_kib,
        sample.read_bytes, sample.write_bytes, sample.rchar, sample.wchar,
        sample.cancelled_write_bytes);
    return nxobs_perf_copy(buf, cap, scratch, written);
}

size_t nxobs_perf_vsync_receipt(const nxobs_perf *perf, char *buf, size_t cap)
{
    char scratch[256];
    const uint64_t fps_milli = nxobs_perf_fps_milli(perf);
    int written;
    if (!perf)
        return 0u;
    /* requested and effective are reported SEPARATELY on purpose: a driver
     * that silently ignores the request is exactly what has to be visible. */
    written = snprintf(
        scratch, sizeof scratch,
        "VSYNC: declared=%d requested=%d effective=%d honored=%d frames=%"
        PRIu64 " fps_milli=%" PRIu64,
        perf->have_swap_interval,
        perf->have_swap_interval ? perf->requested_swap_interval : 0,
        perf->have_swap_interval ? perf->effective_swap_interval : 0,
        perf->have_swap_interval &&
            perf->requested_swap_interval == perf->effective_swap_interval,
        perf->present_count, fps_milli);
    return nxobs_perf_copy(buf, cap, scratch, written);
}
