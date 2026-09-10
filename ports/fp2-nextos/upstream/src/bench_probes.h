/* Sondas de BANCADA do FP2 — compiladas só com -DFP2_BENCH_PROBES (variante
 * build/bench/, nunca empacotada). Lêem a engine por NOME (IL2CPP) e escrevem
 * no log; não alteram nada no jogo. */
#ifndef FP2_BENCH_PROBES_H
#define FP2_BENCH_PROBES_H
void fp2_bench_probes(unsigned long frame);
#endif
