/* SPDX-License-Identifier: GPL-3.0-only
 * nxobs_mem — implementacao. Ver o contrato completo em nxobs_mem.h. */

#define _POSIX_C_SOURCE 200809L

#include "nxobs_mem.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { NXOBS_MEM_MARKER_MAX = 24 };

int nxobs_mem_sampler_init(nxobs_mem_sampler *sampler)
{
    if (!sampler || !sampler->emit)
        return -1;
    if (!(sampler->min_interval_seconds >= 1.0))
        sampler->min_interval_seconds = 1.0;
    sampler->have_last = 0;
    sampler->initialized = 1;
    return 0;
}

static void nxobs_mem_read_status(const char *path, nxobs_mem_sample *sample)
{
    FILE *file = fopen(path ? path : "/proc/self/status", "r");
    char line[256];
    uint64_t value;

    if (!file)
        return;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "VmRSS: %" SCNu64 " kB", &value) == 1) {
            sample->vm_rss_kib = value;
            sample->status_mask |= NXOBS_MEM_STATUS_RSS;
        } else if (sscanf(line, "VmHWM: %" SCNu64 " kB", &value) == 1) {
            sample->vm_hwm_kib = value;
            sample->status_mask |= NXOBS_MEM_STATUS_HWM;
        } else if (sscanf(line, "RssAnon: %" SCNu64 " kB", &value) == 1) {
            sample->rss_anon_kib = value;
            sample->status_mask |= NXOBS_MEM_STATUS_ANON;
        } else if (sscanf(line, "RssFile: %" SCNu64 " kB", &value) == 1) {
            sample->rss_file_kib = value;
            sample->status_mask |= NXOBS_MEM_STATUS_FILE;
        } else if (sscanf(line, "VmSwap: %" SCNu64 " kB", &value) == 1) {
            sample->vm_swap_kib = value;
            sample->status_mask |= NXOBS_MEM_STATUS_SWAP;
        }
    }
    fclose(file);
}

static void nxobs_mem_read_meminfo(const char *path, nxobs_mem_sample *sample)
{
    FILE *file = fopen(path ? path : "/proc/meminfo", "r");
    char line[256];
    uint64_t value;

    if (!file)
        return;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "MemAvailable: %" SCNu64 " kB", &value) == 1) {
            sample->mem_available_kib = value;
            sample->meminfo_mask |= NXOBS_MEM_INFO_AVAILABLE;
        } else if (sscanf(line, "SwapTotal: %" SCNu64 " kB", &value) == 1) {
            sample->swap_total_kib = value;
            sample->meminfo_mask |= NXOBS_MEM_INFO_SWAP_TOTAL;
        } else if (sscanf(line, "SwapFree: %" SCNu64 " kB", &value) == 1) {
            sample->swap_free_kib = value;
            sample->meminfo_mask |= NXOBS_MEM_INFO_SWAP_FREE;
        }
    }
    fclose(file);
}

void nxobs_mem_sample_now(const nxobs_mem_sampler *sampler,
                          nxobs_mem_sample *sample)
{
    if (!sample)
        return;
    memset(sample, 0, sizeof *sample);
    nxobs_mem_read_status(sampler ? sampler->status_path : NULL, sample);
    nxobs_mem_read_meminfo(sampler ? sampler->meminfo_path : NULL, sample);
}

static void nxobs_mem_sanitize_marker(const char *marker, char *out,
                                      unsigned capacity)
{
    unsigned used = 0;

    if (!marker || !*marker)
        marker = "periodic";
    while (used + 1 < capacity && marker[used]) {
        char character = marker[used];
        int safe = (character >= 'A' && character <= 'Z') ||
                   (character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9') ||
                   character == '.' || character == '_' ||
                   character == '+' || character == '-';
        out[used] = safe ? character : '_';
        used++;
    }
    out[used] = 0;
}

int nxobs_mem_format(const nxobs_mem_sampler *sampler,
                     const nxobs_mem_sample *sample, const char *marker,
                     char *buffer, unsigned capacity)
{
    char clean[NXOBS_MEM_MARKER_MAX + 1];
    int written;

    if (!sample || !buffer || !capacity)
        return -1;
    nxobs_mem_sanitize_marker(marker, clean, sizeof clean);
    written = snprintf(
        buffer, capacity,
        "[nxobs/mem] marker=%s rss=%" PRIu64 "MiB hwm=%" PRIu64
        "MiB anon=%" PRIu64 "MiB file=%" PRIu64 "MiB swap=%" PRIu64
        "MiB memavail=%" PRIu64 "MiB swapfree=%" PRIu64 "MiB",
        clean, sample->vm_rss_kib / 1024, sample->vm_hwm_kib / 1024,
        sample->rss_anon_kib / 1024, sample->rss_file_kib / 1024,
        sample->vm_swap_kib / 1024, sample->mem_available_kib / 1024,
        sample->swap_free_kib / 1024);
    if (written < 0 || (unsigned)written >= capacity)
        return written;
    if (sampler && sampler->heap_used_bytes) {
        uint64_t used_bytes = sampler->heap_used_bytes(sampler->heap_ctx);
        uint64_t reserved_bytes = sampler->heap_reserved_bytes
            ? sampler->heap_reserved_bytes(sampler->heap_ctx) : 0;
        int extra = snprintf(
            buffer + written, capacity - (unsigned)written,
            " heap-used=%" PRIu64 "MiB heap-reserved=%" PRIu64 "MiB",
            used_bytes / (1024 * 1024), reserved_bytes / (1024 * 1024));
        if (extra > 0)
            written += extra;
    }
    if ((sample->status_mask & NXOBS_MEM_STATUS_ALL) != NXOBS_MEM_STATUS_ALL &&
        (unsigned)written < capacity) {
        int extra = snprintf(buffer + written, capacity - (unsigned)written,
                             " status=incompleto");
        if (extra > 0)
            written += extra;
    }
    if ((sample->meminfo_mask & NXOBS_MEM_INFO_ALL) != NXOBS_MEM_INFO_ALL &&
        (unsigned)written < capacity) {
        int extra = snprintf(buffer + written, capacity - (unsigned)written,
                             " meminfo=incompleto");
        if (extra > 0)
            written += extra;
    }
    return written;
}

static double nxobs_mem_elapsed(const struct timespec *newer,
                                const struct timespec *older)
{
    return (double)(newer->tv_sec - older->tv_sec) +
           (double)(newer->tv_nsec - older->tv_nsec) / 1000000000.0;
}

int nxobs_mem_tick(nxobs_mem_sampler *sampler, const char *marker)
{
    struct timespec now;
    nxobs_mem_sample sample;
    char line[320];

    if (!sampler || !sampler->initialized)
        return 0;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    if (sampler->have_last &&
        nxobs_mem_elapsed(&now, &sampler->last_sample) <
            sampler->min_interval_seconds)
        return 0;
    sampler->last_sample = now;
    sampler->have_last = 1;
    nxobs_mem_sample_now(sampler, &sample);
    if (nxobs_mem_format(sampler, &sample, marker, line, sizeof line) < 0)
        return 0;
    sampler->emit(sampler->emit_ctx, line);
    return 1;
}
