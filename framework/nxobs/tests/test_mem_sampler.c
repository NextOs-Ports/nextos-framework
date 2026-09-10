/* SPDX-License-Identifier: GPL-3.0-only
 * Gate hermetico do nxobs_mem: fixtures sinteticas de /proc, piso de
 * intervalo, supressao por frequencia, sanitizacao de marcador, mascaras de
 * completude e callbacks de heap do adapter. Nao toca hardware nem rede. */
#define _POSIX_C_SOURCE 200809L
#include "nxobs_mem.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char last_line[512];
static int emit_count;

static void capture(void *ctx, const char *line)
{
    (void)ctx;
    emit_count++;
    snprintf(last_line, sizeof last_line, "%s", line);
}

static uint64_t fake_heap_used(void *ctx) { (void)ctx; return 96ull << 20; }
static uint64_t fake_heap_reserved(void *ctx) { (void)ctx; return 128ull << 20; }

static void write_file(const char *path, const char *content)
{
    FILE *file = fopen(path, "w");
    assert(file);
    fputs(content, file);
    fclose(file);
}

int main(void)
{
    char dir[] = "/tmp/nxobs-mem-test-XXXXXX";
    char status_path[128], meminfo_path[128];
    nxobs_mem_sampler sampler;
    nxobs_mem_sample sample;

    assert(mkdtemp(dir));
    snprintf(status_path, sizeof status_path, "%s/status", dir);
    snprintf(meminfo_path, sizeof meminfo_path, "%s/meminfo", dir);
    write_file(status_path,
               "Name: fixture\nVmRSS: 204800 kB\nVmHWM: 307200 kB\n"
               "RssAnon: 153600 kB\nRssFile: 51200 kB\nVmSwap: 1024 kB\n");
    write_file(meminfo_path,
               "MemTotal: 938396 kB\nMemAvailable: 262144 kB\n"
               "SwapTotal: 524288 kB\nSwapFree: 523264 kB\n");

    /* init exige emit e sobe intervalo abaixo do piso */
    memset(&sampler, 0, sizeof sampler);
    assert(nxobs_mem_sampler_init(&sampler) == -1);
    sampler.emit = capture;
    sampler.min_interval_seconds = 0.001; /* abaixo do piso */
    sampler.status_path = status_path;
    sampler.meminfo_path = meminfo_path;
    assert(nxobs_mem_sampler_init(&sampler) == 0);
    assert(sampler.min_interval_seconds >= 1.0);

    /* leitura pontual completa */
    nxobs_mem_sample_now(&sampler, &sample);
    assert(sample.status_mask == NXOBS_MEM_STATUS_ALL);
    assert(sample.meminfo_mask == NXOBS_MEM_INFO_ALL);
    assert(sample.vm_rss_kib == 204800 && sample.mem_available_kib == 262144);

    /* primeiro tick emite; segundo dentro do intervalo e' suprimido */
    assert(nxobs_mem_tick(&sampler, "menu") == 1);
    assert(emit_count == 1);
    assert(strstr(last_line, "[nxobs/mem] marker=menu rss=200MiB"));
    assert(strstr(last_line, "memavail=256MiB"));
    assert(!strstr(last_line, "incompleto"));
    assert(nxobs_mem_tick(&sampler, "menu") == 0);
    assert(emit_count == 1);

    /* marcador hostil e' sanitizado e truncado */
    sampler.have_last = 0;
    assert(nxobs_mem_tick(&sampler,
                          "wave 3/rede:10.0.0.1 e um nome muito comprido") == 1);
    assert(strstr(last_line, "marker=wave_3_rede_10.0.0.1_"));
    assert(!strstr(last_line, " 10.0.0.1"));

    /* heap do adapter e' opaco e aparece so quando registrado */
    sampler.heap_used_bytes = fake_heap_used;
    sampler.heap_reserved_bytes = fake_heap_reserved;
    sampler.have_last = 0;
    assert(nxobs_mem_tick(&sampler, NULL) == 1);
    assert(strstr(last_line, "marker=periodic"));
    assert(strstr(last_line, "heap-used=96MiB heap-reserved=128MiB"));

    /* fixture truncada vira mascara incompleta, nunca zero silencioso */
    write_file(status_path, "Name: fixture\nVmRSS: 1024 kB\n");
    write_file(meminfo_path, "MemTotal: 938396 kB\n");
    nxobs_mem_sample_now(&sampler, &sample);
    assert(sample.status_mask == NXOBS_MEM_STATUS_RSS);
    assert(sample.meminfo_mask == 0);
    sampler.have_last = 0;
    assert(nxobs_mem_tick(&sampler, "trunc") == 1);
    assert(strstr(last_line, "status=incompleto"));
    assert(strstr(last_line, "meminfo=incompleto"));

    /* caminho real de /proc funciona no host (somente leitura) */
    sampler.status_path = NULL;
    sampler.meminfo_path = NULL;
    nxobs_mem_sample_now(&sampler, &sample);
    assert(sample.status_mask & NXOBS_MEM_STATUS_RSS);
    assert(sample.meminfo_mask & NXOBS_MEM_INFO_AVAILABLE);

    unlink(status_path); unlink(meminfo_path); rmdir(dir);
    printf("nxobs-mem sampler gate: PASS emits=%d\n", emit_count);
    return 0;
}
