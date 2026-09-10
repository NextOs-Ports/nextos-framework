/* SPDX-License-Identifier: GPL-3.0-only
 * Host gate for nxobs_perf: V3-PERF-01 metrics and the VSYNC/OBS receipt.
 * Hermetic: the /proc sources are fixtures, so no device is required and the
 * absent-field path is exercised for real. */

#define _POSIX_C_SOURCE 200809L

#include "nxobs_perf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char work[512];
static char last_line[1024];
static unsigned emit_count;

static void check(int condition, const char *message)
{
    if (!condition) {
        (void)fprintf(stderr, "nxobs_perf: %s\n", message);
        exit(1);
    }
}

static void collect(void *ctx, const char *line)
{
    (void)ctx;
    ++emit_count;
    (void)snprintf(last_line, sizeof last_line, "%s", line);
}

static void write_file(const char *name, const char *content)
{
    char path[640];
    FILE *file;
    (void)snprintf(path, sizeof path, "%s/%s", work, name);
    file = fopen(path, "w");
    check(file != NULL, "fixture could not be written");
    (void)fputs(content, file);
    (void)fclose(file);
}

static void path_of(char *out, size_t cap, const char *name)
{
    (void)snprintf(out, cap, "%s/%s", work, name);
}

int main(void)
{
    nxobs_perf perf;
    nxobs_perf_sample sample;
    char status_path[640];
    char io_path[640];
    char fd_path[640];
    char receipt[1024];
    char vsync[512];
    size_t length;
    int64_t interval;
    uint64_t fps;
    unsigned index;

    (void)snprintf(work, sizeof work, "/tmp/nxobs-perf.%d", (int)getpid());
    check(mkdir(work, 0700) == 0, "work directory could not be created");

    write_file("status",
               "Name:\tfixture\nThreads:\t7\n"
               "VmRSS:\t131072 kB\nVmHWM:\t262144 kB\n");
    write_file("io",
               "rchar: 1000\nwchar: 2000\nsyscr: 10\nsyscw: 20\n"
               "read_bytes: 4096\nwrite_bytes: 8192\n"
               "cancelled_write_bytes: 512\n");
    path_of(status_path, sizeof status_path, "status");
    path_of(io_path, sizeof io_path, "io");
    path_of(fd_path, sizeof fd_path, "fd");
    check(mkdir(fd_path, 0700) == 0, "fd fixture directory failed");
    write_file("fd/0", "");
    write_file("fd/1", "");
    write_file("fd/2", "");

    memset(&perf, 0, sizeof perf);
    perf.emit = collect;
    perf.status_path = status_path;
    perf.io_path = io_path;
    perf.fd_dir_path = fd_path;
    check(nxobs_perf_init(&perf) == 0, "init refused a valid configuration");
    check(perf.min_interval_seconds >= 1.0,
          "the minimum interval floor was not applied");

    /* A sampler without an emit sink is refused: a receipt nobody can read is
     * not observability. */
    {
        nxobs_perf broken;
        memset(&broken, 0, sizeof broken);
        check(nxobs_perf_init(&broken) == -1, "init accepted a NULL sink");
    }

    nxobs_perf_sample_now(&perf, &sample);
    check(sample.threads == 7u, "Threads was not read");
    check(sample.vm_rss_kib == 131072u && sample.vm_hwm_kib == 262144u,
          "RSS/HWM were not read");
    check(sample.rchar == 1000u && sample.wchar == 2000u &&
          sample.read_bytes == 4096u && sample.write_bytes == 8192u &&
          sample.cancelled_write_bytes == 512u,
          "/proc/self/io fields were not read");
    check(sample.open_fds == 3u, "the open descriptor count is wrong");
    check((sample.mask & NXOBS_PERF_HAVE_THREADS) &&
          (sample.mask & NXOBS_PERF_HAVE_IO) &&
          (sample.mask & NXOBS_PERF_HAVE_FDS) &&
          (sample.mask & NXOBS_PERF_HAVE_RSS),
          "the completeness mask does not report what was read");

    /* A kernel without TASK_IO_ACCOUNTING produces a SMALLER receipt, never a
     * fabricated one. */
    {
        nxobs_perf partial = perf;
        char missing[640];
        path_of(missing, sizeof missing, "io-absent");
        partial.io_path = missing;
        nxobs_perf_sample_now(&partial, &sample);
        check(!(sample.mask & NXOBS_PERF_HAVE_IO),
              "an absent /proc/self/io was reported as present");
        check(sample.read_bytes == 0u && sample.rchar == 0u,
              "an absent counter was not left at zero");
        check((sample.mask & NXOBS_PERF_HAVE_THREADS) != 0u,
              "one absent source suppressed the others");
    }
    /* A truncated io file is incomplete, not partially trusted. */
    write_file("io-partial", "rchar: 1\nwchar: 2\n");
    {
        nxobs_perf partial = perf;
        char truncated[640];
        path_of(truncated, sizeof truncated, "io-partial");
        partial.io_path = truncated;
        nxobs_perf_sample_now(&partial, &sample);
        check(!(sample.mask & NXOBS_PERF_HAVE_IO),
              "a truncated /proc/self/io was reported as complete");
    }

    /* Milestones: the FIRST mark is the measurement; a repeat is noise. */
    check(nxobs_perf_interval_ms(&perf, NXOBS_PERF_LAUNCHER_START,
                                 NXOBS_PERF_NXEXTRACT_START) == -1,
          "an unmarked interval was reported as zero");
    nxobs_perf_mark(&perf, NXOBS_PERF_LAUNCHER_START);
    {
        struct timespec pause = {0, 20 * 1000 * 1000};
        (void)nanosleep(&pause, NULL);
    }
    nxobs_perf_mark(&perf, NXOBS_PERF_NXEXTRACT_START);
    interval = nxobs_perf_interval_ms(&perf, NXOBS_PERF_LAUNCHER_START,
                                      NXOBS_PERF_NXEXTRACT_START);
    check(interval >= 10 && interval < 5000,
          "the milestone interval is not plausible");
    {
        const int64_t first = interval;
        struct timespec pause = {0, 20 * 1000 * 1000};
        (void)nanosleep(&pause, NULL);
        nxobs_perf_mark(&perf, NXOBS_PERF_NXEXTRACT_START);
        check(nxobs_perf_interval_ms(&perf, NXOBS_PERF_LAUNCHER_START,
                                     NXOBS_PERF_NXEXTRACT_START) == first,
              "a repeated mark moved the measurement");
    }
    check(nxobs_perf_interval_ms(&perf, NXOBS_PERF_NXEXTRACT_START,
                                 NXOBS_PERF_LAUNCHER_START) == -1,
          "a reversed interval was reported as valid");
    check(nxobs_perf_interval_ms(&perf, NXOBS_PERF_LAUNCHER_START,
                                 (nxobs_perf_milestone)99) == -1,
          "an invalid milestone was accepted");
    check(strcmp(nxobs_perf_milestone_name(NXOBS_PERF_MENU_READY),
                 "menu-ready") == 0, "a milestone lost its name");

    /* The frame path never logs and never reads /proc. */
    emit_count = 0u;
    check(nxobs_perf_fps_milli(&perf) == 0u,
          "FPS was claimed with no frames");
    nxobs_perf_present(&perf);
    check(nxobs_perf_fps_milli(&perf) == 0u,
          "FPS was claimed from a single frame");
    for (index = 0u; index < 30u; ++index) {
        struct timespec pause = {0, 2 * 1000 * 1000};
        (void)nanosleep(&pause, NULL);
        nxobs_perf_present(&perf);
    }
    check(emit_count == 0u, "the frame path emitted a log line");
    check(perf.present_count == 31u, "frames were not counted");
    fps = nxobs_perf_fps_milli(&perf);
    check(fps > 0u, "FPS was not measured");

    /* Rate limiting: a burst of ticks emits at most once. */
    emit_count = 0u;
    check(nxobs_perf_tick(&perf, "boot") == 1, "the first tick did not emit");
    for (index = 0u; index < 100u; ++index) {
        (void)nxobs_perf_tick(&perf, "boot");
    }
    check(emit_count == 1u, "the tick rate limit was not honoured");
    check(strstr(last_line, "[nxobs/perf] marker=boot") == last_line,
          "the tick line lost its prefix or marker");
    check(strstr(last_line, "threads=7") != NULL &&
          strstr(last_line, "write_bytes=8192") != NULL &&
          strstr(last_line, "frames=31") != NULL,
          "the tick line lost its metrics");

    /* A hostile marker never reaches the log untouched. */
    perf.have_last = 0;
    (void)nxobs_perf_tick(&perf, "bad marker\nwith newline and = signs");
    check(strchr(last_line + 1, '\n') == NULL,
          "a newline survived into the log line");
    check(strstr(last_line, "marker=bad_marker_with_new") != NULL,
          "the marker was not sanitized");
    {
        const char *begin = strstr(last_line, "marker=");
        const char *end;
        check(begin != NULL, "the tick line lost its marker");
        begin += 7;
        end = strchr(begin, ' ');
        check(end != NULL, "the marker was not delimited");
        check((size_t)(end - begin) == 24u,
              "the marker was not truncated to its 24-byte bound");
    }

    /* VSYNC receipt: requested and effective stay separate. */
    length = nxobs_perf_vsync_receipt(&perf, vsync, sizeof vsync);
    check(length > 0u && strstr(vsync, "VSYNC: declared=0") == vsync,
          "an undeclared swap interval was not reported as such");
    nxobs_perf_declare_swap_interval(&perf, 1, 0);
    length = nxobs_perf_vsync_receipt(&perf, vsync, sizeof vsync);
    check(length > 0u, "the vsync receipt is empty");
    check(strstr(vsync, "declared=1") != NULL &&
          strstr(vsync, "requested=1") != NULL &&
          strstr(vsync, "effective=0") != NULL &&
          strstr(vsync, "honored=0") != NULL,
          "a driver that ignored the request was not made visible");
    check(strstr(vsync, "frames=31") != NULL &&
          strstr(vsync, "fps_milli=") != NULL,
          "the vsync receipt lost its measured frame data");
    nxobs_perf_declare_swap_interval(&perf, 1, 1);
    (void)nxobs_perf_vsync_receipt(&perf, vsync, sizeof vsync);
    check(strstr(vsync, "honored=1") != NULL,
          "an honoured request was not reported");

    /* PERF receipt. */
    length = nxobs_perf_receipt(&perf, receipt, sizeof receipt);
    check(length > 0u && strstr(receipt, "PERF: ") == receipt,
          "the perf receipt lost its prefix");
    check(strstr(receipt, "launcher_to_nxextract_ms=") != NULL &&
          strstr(receipt, "adapter_to_first_frame_ms=-1") != NULL &&
          strstr(receipt, "cancelled_write_bytes=512") != NULL,
          "the perf receipt lost a required metric");
    {
        char small[16];
        size_t needed = nxobs_perf_receipt(&perf, small, sizeof small);
        check(needed == length, "the receipt did not report its full size");
        check(strlen(small) == sizeof small - 1u,
              "the receipt overflowed a small buffer");
    }

    (void)printf("nxobs_perf: ALL PASS fps_milli=%llu\n",
                 (unsigned long long)fps);
    return 0;
}
