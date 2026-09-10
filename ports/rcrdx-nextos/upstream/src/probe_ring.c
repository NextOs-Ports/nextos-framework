/*
 * probe_ring.c -- ver probe_ring.h.
 *
 * Formato do arquivo:
 *   offset 0 : cabecalho (32 bytes) -- magica, capacidade, escrita, congelado
 *   offset 32: registros de 16 bytes
 *
 * A escrita e um contador que so cresce; o leitor descobre o inicio valido por
 * (escrita % capacidade).  Sem lock: varias threads escrevem, e um registro
 * perdido numa corrida nao muda a conclusao -- o que importa e o padrao de
 * milhares deles.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "probe_ring.h"

#define RCR_PROBE_MAGIC   0x50524254u   /* "TBRP" */
#define RCR_PROBE_RECORDS (1u << 16)    /* 65536 registros = 1 MiB */

typedef struct {
    uint32_t us;        /* microssegundos monotonicos, truncados: ~71min */
    uint16_t tid;       /* 16 bits baixos bastam para separar as threads */
    uint8_t  kind;
    uint8_t  pad;
    uint32_t a;
    uint32_t b;
} rcr_rec;

typedef struct {
    uint32_t magic;
    uint32_t records;
    uint64_t written;
    uint32_t frozen;
    uint32_t reserved[3];
} rcr_hdr;

int rcr_probe_on;
static rcr_hdr *g_hdr;
static rcr_rec *g_recs;
static struct timespec g_t0;

void rcr_probe_init(const char *dir)
{
    if (g_hdr)
        return;
    char path[512];
    snprintf(path, sizeof path, "%s/probe.bin", dir);
    size_t bytes = sizeof(rcr_hdr) + (size_t)RCR_PROBE_RECORDS * sizeof(rcr_rec);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    if (ftruncate(fd, (off_t)bytes) != 0) {
        close(fd);
        return;
    }
    void *m = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return;
    memset(m, 0, sizeof(rcr_hdr));
    g_hdr = m;
    g_recs = (rcr_rec *)((char *)m + sizeof(rcr_hdr));
    g_hdr->records = RCR_PROBE_RECORDS;
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    __sync_synchronize();
    g_hdr->magic = RCR_PROBE_MAGIC;   /* so no fim: magica = pronto para ler */
    rcr_probe_on = 1;
    fprintf(stderr, "[rcr] probe ring em %s (%u registros)\n", path,
            RCR_PROBE_RECORDS);
}

void rcr_probe_note(uint8_t kind, uint32_t a, uint32_t b)
{
    if (!g_hdr || g_hdr->frozen)
        return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);   /* vDSO: sem syscall */
    uint64_t slot = __sync_fetch_and_add(&g_hdr->written, 1);
    rcr_rec *r = &g_recs[slot & (RCR_PROBE_RECORDS - 1)];
    r->us = (uint32_t)((now.tv_sec - g_t0.tv_sec) * 1000000u +
                       (now.tv_nsec - g_t0.tv_nsec) / 1000u);
    r->tid = (uint16_t)syscall(SYS_gettid);
    r->kind = kind;
    r->pad = 0;
    r->a = a;
    r->b = b;
}

void rcr_probe_freeze(void)
{
    if (g_hdr && !g_hdr->frozen) {
        __sync_synchronize();
        g_hdr->frozen = 1;
        fprintf(stderr, "[rcr] probe ring CONGELADO apos %llu registros\n",
                (unsigned long long)g_hdr->written);
    }
}

int rcr_probe_frozen(void)
{
    return g_hdr && g_hdr->frozen;
}
