/*
 * probe_ring.h -- carimbos de tempo sem custo, para caçar o soluço da tela 2.
 *
 * Log formatado NÃO serve aqui: medido, ligar RCR_JNILOG dissolveu o proprio
 * bug (os frames de 2385ms sumiram).  O atraso do log deixa a cadeia do timer
 * do Lime se re-armar a tempo -- ou seja, o instrumento apagava o fenomeno.
 *
 * Este canal escreve so STORES num anel mapeado (mmap MAP_SHARED), sem
 * syscall, sem formatacao e sem lock: dezenas de nanossegundos por registro.
 * Um leitor de fora abre o mesmo arquivo enquanto o jogo roda.
 */
#ifndef RCR_PROBE_RING_H
#define RCR_PROBE_RING_H

#include <stdint.h>

enum {
    RCR_PROBE_SLEEP = 1,   /* a = ms pedidos, b = balde do chamador */
    RCR_PROBE_SWAP  = 2,   /* a = numero do frame, b = ms desde o anterior */
    RCR_PROBE_EVENT = 3,   /* a = tipo do evento empurrado */
};

/* Baldes de chamador do sleep, para separar as duas esperas ja identificadas
 * sem pagar por uma tabela: o laco do WaitEvent (thread principal) e o do
 * SemWaitTimeout (thread de timer do SDL). */
enum {
    RCR_SLEEP_OUTRO = 0,
    RCR_SLEEP_WAITEVENT = 1,
    RCR_SLEEP_TIMER = 2,
};

/* 0 no release: os carimbos ficam em caminhos quentes (nanosleep roda
 * milhares de vezes por segundo), entao o custo tem de ser um teste de
 * inteiro quando o instrumento esta desligado. */
extern int rcr_probe_on;

void rcr_probe_init(const char *dir);
void rcr_probe_note(uint8_t kind, uint32_t a, uint32_t b);
/* Congela o anel preservando o que ja foi gravado.  Chamado quando um frame
 * passa do limite, para o momento raro ficar guardado sem depender de alguem
 * estar olhando na hora. */
void rcr_probe_freeze(void);
int rcr_probe_frozen(void);

#endif
