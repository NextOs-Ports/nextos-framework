/* Observador de memoria do Sally Face.
 *
 * Por que existe: no Mali-450 a memoria do DRIVER sai da mesma RAM do sistema e
 * NAO entra no RSS de ninguem.  Quando ela estoura, a caixa nao mata o jogo por
 * OOM: ela entra em swap-thrash, o sshd para de fechar handshake ("Connection
 * timed out during banner exchange"), o aparelho responde a ping e mais nada.
 * Foi exatamente o que aconteceu em 09/08/2026 assim que os shaders passaram a
 * funcionar e as texturas passaram a subir de verdade.
 *
 * O vigia mora DENTRO do processo de proposito: durante o afogamento nao existe
 * ssh para medir de fora, e ficar sondando por ssh so' piora o thrash.
 *
 * Os limites abaixo sao exclusivamente telemetria. Um aparelho suportado nao
 * pode ser derrubado pelo port por causa de MemAvailable ou swap arbitrarios.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sf.h"

static long read_kv(const char *path, const char *key)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[256];
    size_t n = strlen(key);
    long value = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, n) == 0) {
            const char *p = line + n;
            while (*p && (*p == ':' || *p == ' ' || *p == '\t'))
                p++;
            value = strtol(p, NULL, 10);
            break;
        }
    }
    fclose(f);
    return value;
}

/* "Mali mem usage: <bytes>" -- memoria do driver, a que nao aparece em lugar
 * nenhum de /proc.  So' existe com debugfs montado; ausencia nao e' erro. */
static long mali_kb(void)
{
    FILE *f = fopen("/sys/kernel/debug/mali/gpu_memory", "r");
    if (!f)
        return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof line, f)) {
        const char *p = strstr(line, "mem usage:");
        if (p) {
            kb = strtol(p + 10, NULL, 10) / 1024;
            break;
        }
    }
    fclose(f);
    return kb;
}

static int guard_enabled = 1;
static long swap_ceiling_kb = 350 * 1024;   /* teto de swap, receita do FF1 */
static long avail_floor_kb  = 60 * 1024;    /* piso de MemAvailable */
static unsigned sample_ms = 1000;

static void *guard_thread(void *unused)
{
    (void)unused;
    unsigned long tick = 0;
    int pressure_samples = 0;
    for (;;) {
        usleep(sample_ms * 1000);
        long rss   = read_kv("/proc/self/status", "VmRSS");
        long swap  = read_kv("/proc/self/status", "VmSwap");
        long avail = read_kv("/proc/meminfo", "MemAvailable");
        long mali  = mali_kb();

        /* amostra periodica no log do port: durante o afogamento e' a UNICA
         * testemunha, porque ssh nao entra */
        if (++tick % 5 == 0 || swap > swap_ceiling_kb / 2) {
            if (mali >= 0)
                fprintf(stderr,
                        "[sf/mem] rss=%ldMB swap=%ldMB avail=%ldMB "
                        "mali=%ldMB\n",
                        rss / 1024, swap / 1024, avail / 1024, mali / 1024);
            else
                fprintf(stderr,
                        "[sf/mem] rss=%ldMB swap=%ldMB avail=%ldMB "
                        "mali=n/d\n",
                        rss / 1024, swap / 1024, avail / 1024);
        }

        int over = (swap > 0 && swap > swap_ceiling_kb) ||
                   (avail >= 0 && avail < avail_floor_kb);
        if (!over) {
            if (pressure_samples >= 2)
                fprintf(stderr, "[sf/mem] pressao de memoria normalizada\n");
            pressure_samples = 0;
            continue;
        }
        /* Duas amostras seguidas evitam um alerta por pico isolado. A partir
         * daqui o observador continua registrando, mas nunca pede saida. */
        if (++pressure_samples < 2)
            continue;
        if (pressure_samples == 2 || pressure_samples % 30 == 0)
            fprintf(stderr,
                    "[sf/mem] PRESSAO observada rss=%ldMB swap=%ldMB "
                    "avail=%ldMB mali=%ldMB -- somente telemetria\n",
                    rss / 1024, swap / 1024, avail / 1024,
                    mali >= 0 ? mali / 1024 : -1);
    }
    return NULL;
}

void sf_mem_guard_start(void)
{
    const char *v;
    if ((v = getenv("SF_MEM_GUARD")) && *v == '0')
        guard_enabled = 0;
    if (!guard_enabled) {
        fprintf(stderr, "[sf/mem] vigia DESLIGADO por SF_MEM_GUARD=0\n");
        return;
    }
    if ((v = getenv("SF_SWAP_CEILING_MB")) && *v)
        swap_ceiling_kb = strtol(v, NULL, 10) * 1024;
    if ((v = getenv("SF_AVAIL_FLOOR_MB")) && *v)
        avail_floor_kb = strtol(v, NULL, 10) * 1024;

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, guard_thread, NULL) != 0)
        fprintf(stderr, "[sf/mem] nao consegui criar o vigia: %s\n",
                strerror(errno));
    else
        fprintf(stderr,
                "[sf/mem] observador ativo (alerta swap>%ldMB ou avail<%ldMB; "
                "nunca encerra o jogo)\n",
                swap_ceiling_kb / 1024, avail_floor_kb / 1024);
    pthread_attr_destroy(&attr);
}
