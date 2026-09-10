/* SPDX-License-Identifier: GPL-3.0-only
 *
 * nxobs_mem — telemetria PASSIVA de pressao de memoria (P5).
 *
 * Amostrador somente leitura de /proc/self/status e /proc/meminfo com
 * intervalo de parede limitado. Contrato:
 *
 *   - OBSERVAR NUNCA AUTORIZA AGIR. Este modulo nao chama GC, nao descarrega
 *     asset, nao apaga textura, nao faz malloc_trim e nao conhece classe,
 *     offset ou estado interno de jogo nenhum. Qualquer acao de memoria
 *     continua fora do framework e presa ao gate fisico proprio (Wave 20 em
 *     1 GB + regressao em 2 GB) antes de virar comportamento.
 *   - O custo e' limitado por construcao: nenhuma leitura de /proc acontece
 *     mais rapido que o intervalo minimo (piso de 1 segundo), independente da
 *     frequencia com que o adapter chame o tick.
 *   - O adapter PODE acrescentar heap usado/reservado (callbacks opacos) e um
 *     marcador curto por amostra; o framework nao interpreta nenhum dos dois.
 *   - Funciona sem controle conectado e nunca decide por nome de aparelho ou
 *     firmware: as unicas fontes sao os dois arquivos de /proc.
 *   - Campos ausentes viram mascara de completude, nunca zero silencioso.
 */
#ifndef NXOBS_MEM_H
#define NXOBS_MEM_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NXOBS_MEM_STATUS_RSS = 1u << 0,
    NXOBS_MEM_STATUS_HWM = 1u << 1,
    NXOBS_MEM_STATUS_ANON = 1u << 2,
    NXOBS_MEM_STATUS_FILE = 1u << 3,
    NXOBS_MEM_STATUS_SWAP = 1u << 4,
    NXOBS_MEM_STATUS_ALL = (1u << 5) - 1u,

    NXOBS_MEM_INFO_AVAILABLE = 1u << 0,
    NXOBS_MEM_INFO_SWAP_TOTAL = 1u << 1,
    NXOBS_MEM_INFO_SWAP_FREE = 1u << 2,
    NXOBS_MEM_INFO_ALL = (1u << 3) - 1u
};

typedef struct nxobs_mem_sample {
    uint64_t vm_rss_kib;
    uint64_t vm_hwm_kib;
    uint64_t rss_anon_kib;
    uint64_t rss_file_kib;
    uint64_t vm_swap_kib;
    uint64_t mem_available_kib;
    uint64_t swap_total_kib;
    uint64_t swap_free_kib;
    unsigned status_mask;   /* NXOBS_MEM_STATUS_* realmente lidos */
    unsigned meminfo_mask;  /* NXOBS_MEM_INFO_* realmente lidos */
} nxobs_mem_sample;

typedef struct nxobs_mem_sampler {
    /* Configuracao (preencher antes de nxobs_mem_sampler_init). */
    double min_interval_seconds;         /* piso 1.0; valores menores sobem */
    void (*emit)(void *ctx, const char *line); /* obrigatorio: recebe a linha */
    void *emit_ctx;
    /* Opcional (adapter): heap da engine, opaco para o framework. */
    uint64_t (*heap_used_bytes)(void *ctx);
    uint64_t (*heap_reserved_bytes)(void *ctx);
    void *heap_ctx;
    /* Somente testes: caminhos alternativos (NULL = /proc reais). */
    const char *status_path;
    const char *meminfo_path;
    /* Estado interno. */
    struct timespec last_sample;
    int have_last;
    int initialized;
} nxobs_mem_sampler;

/* Valida e normaliza a configuracao. Retorna 0 em sucesso, -1 se emit for
 * NULL. Nao aloca nada; o chamador e' dono da struct. */
int nxobs_mem_sampler_init(nxobs_mem_sampler *sampler);

/* Le os dois arquivos AGORA (sem limite de frequencia — uso pontual do
 * adapter, ex.: antes/depois de um marco proprio). Retorna as mascaras de
 * completude preenchidas; arquivo ilegivel resulta em mascara vazia. */
void nxobs_mem_sample_now(const nxobs_mem_sampler *sampler,
                          nxobs_mem_sample *sample);

/* Tick periodico: se o intervalo minimo ja passou, amostra e emite UMA linha
 * "[nxobs/mem] marker=<m> rss=..." via emit. Retorna 1 quando emitiu, 0
 * quando suprimiu pelo intervalo. O marcador e' truncado em 24 bytes e
 * caracteres fora de [A-Za-z0-9._+-] viram '_' (linha de log nao carrega
 * caminho, endereco nem nome privado). */
int nxobs_mem_tick(nxobs_mem_sampler *sampler, const char *marker);

/* Formata uma amostra ja colhida na MESMA linha canonica do tick. Retorna o
 * numero de bytes escritos (como snprintf). */
int nxobs_mem_format(const nxobs_mem_sampler *sampler,
                     const nxobs_mem_sample *sample, const char *marker,
                     char *buffer, unsigned capacity);

#ifdef __cplusplus
}
#endif

#endif /* NXOBS_MEM_H */
