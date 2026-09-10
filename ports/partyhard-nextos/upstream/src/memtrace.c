/*
 * memtrace.c -- quem esta' segurando memoria, por endereco de retorno.
 *
 * Existe porque o [heap] deste port crescia ~300 KB/s em gameplay (10 KB por
 * quadro, com regularidade de relogio) enquanto TODOS os suspeitos normais
 * saiam limpos na medicao: o balanco de malloc/free do convidado ficava
 * parado em 7,5 MB, o coletor do IL2CPP estava saudavel (heap gerenciado
 * firme em 29 MB, coletando), os decodificadores de ETC2/ASTC davam free, o
 * shim de JNI criava 825 objetos no total, e `malloc_trim` nao devolvia nada
 * -- ou seja, a memoria estava VIVA, nao fragmentada.
 *
 * Sobrava so' um jeito honesto de descobrir: interpor o alocador e guardar o
 * endereco de retorno de quem pediu cada bloco que continua vivo.  Como o
 * executavel e' PIE com -rdynamic, definir malloc/free aqui captura TODAS as
 * alocacoes -- as nossas e as do convidado, que chegam pelo shim de bionic.
 *
 * So' liga com ST_MEMTRACE_RA=1, e so' registra blocos grandes (>= 2 KB), que
 * e' onde 10 KB por quadro tem de aparecer.  Fora isso o custo e' um teste e
 * um desvio.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>

extern void *__libc_malloc(size_t);
extern void __libc_free(void *);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);

#define MT_SLOTS   (1u << 17)   /* 131072 blocos grandes vivos */
static size_t mt_min = 2048;

typedef struct {
    void *ptr;
    size_t size;
    void *ra;
} mt_entry;

static mt_entry mt_table[MT_SLOTS];
static int mt_on = -1;
static __thread int mt_busy;

static int mt_enabled(void)
{
    if (mt_on < 0) {
        const char *v = getenv("ST_MEMTRACE_RA");
        mt_on = v && *v && *v != '0';
        const char *m = getenv("ST_MEMTRACE_MIN");
        if (m && *m) {
            long long parsed = atoll(m);
            if (parsed > 0)
                mt_min = (size_t)parsed;
        }
    }
    return mt_on;
}

static size_t mt_hash(void *p)
{
    uintptr_t x = (uintptr_t)p >> 4;
    x *= 0x9E3779B97F4A7C15ull;
    return (size_t)(x >> 40) & (MT_SLOTS - 1);
}

static void mt_insert(void *p, size_t n, void *ra)
{
    size_t i = mt_hash(p);
    for (size_t k = 0; k < 64; k++) {
        size_t j = (i + k) & (MT_SLOTS - 1);
        if (!mt_table[j].ptr) {
            mt_table[j].ptr = p;
            mt_table[j].size = n;
            mt_table[j].ra = ra;
            return;
        }
    }
}

static void mt_remove(void *p)
{
    size_t i = mt_hash(p);
    for (size_t k = 0; k < 64; k++) {
        size_t j = (i + k) & (MT_SLOTS - 1);
        if (mt_table[j].ptr == p) {
            mt_table[j].ptr = NULL;
            mt_table[j].size = 0;
            mt_table[j].ra = NULL;
            return;
        }
    }
}

void *malloc(size_t n)
{
    void *p = __libc_malloc(n);
    if (p && n >= mt_min && !mt_busy && mt_enabled()) {
        mt_busy = 1;
        mt_insert(p, n, __builtin_return_address(0));
        mt_busy = 0;
    }
    return p;
}

void free(void *p)
{
    if (p && !mt_busy && mt_enabled()) {
        mt_busy = 1;
        mt_remove(p);
        mt_busy = 0;
    }
    __libc_free(p);
}

void *calloc(size_t n, size_t m)
{
    void *p = __libc_calloc(n, m);
    if (p && n && m && n * m >= mt_min && !mt_busy && mt_enabled()) {
        mt_busy = 1;
        mt_insert(p, n * m, __builtin_return_address(0));
        mt_busy = 0;
    }
    return p;
}

void *realloc(void *old, size_t n)
{
    void *p = __libc_realloc(old, n);
    if (!mt_busy && mt_enabled()) {
        mt_busy = 1;
        if (old)
            mt_remove(old);
        if (p && n >= mt_min)
            mt_insert(p, n, __builtin_return_address(0));
        mt_busy = 0;
    }
    return p;
}

/* Agrega os blocos vivos por endereco de retorno e imprime os maiores. */
void st_memtrace_report(void)
{
    if (!mt_enabled())
        return;
    struct { void *ra; size_t bytes; unsigned long count; } top[12];
    memset(top, 0, sizeof top);
    size_t live = 0;
    unsigned long blocks = 0;

    mt_busy = 1;
    for (size_t i = 0; i < MT_SLOTS; i++) {
        if (!mt_table[i].ptr)
            continue;
        live += mt_table[i].size;
        blocks++;
        size_t k = 0;
        for (; k < 12; k++)
            if (top[k].ra == mt_table[i].ra)
                break;
        if (k == 12) {
            for (k = 0; k < 12; k++)
                if (!top[k].ra)
                    break;
        }
        if (k < 12) {
            top[k].ra = mt_table[i].ra;
            top[k].bytes += mt_table[i].size;
            top[k].count++;
        }
    }
    mt_busy = 0;

    fprintf(stderr, "[st/ra] vivos>=%zuB: %zu kB em %lu blocos\n",
            mt_min, live / 1024, blocks);
    for (size_t k = 0; k < 12; k++) {
        if (!top[k].ra)
            continue;
        fprintf(stderr, "[st/ra]   ra=%p %zu kB em %lu blocos\n",
                top[k].ra, top[k].bytes / 1024, top[k].count);
    }
    fflush(stderr);
}
