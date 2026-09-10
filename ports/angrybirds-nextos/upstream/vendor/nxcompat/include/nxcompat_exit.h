/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxcompat_exit.h -- a SAIDA TERMINAL de um port, escrita uma vez (onda v2).
 *
 * Regra da casa aprendida a ferro: no Mali-450/fbdev, `return` do main roda
 * os atexit da glibc, o SDL desmonta o GL ali e o driver TRAVA NO KERNEL (so'
 * a tomada resolve). A receita provada e': salvar -> soltar locks -> _exit(0),
 * sem nunca passar por SDL_Quit/DeleteContext no caminho de saida. Cada port
 * copiava a receita a mao (~20 copias, com prazos de watchdog divergindo e
 * um deles cortando o save). Aqui ela mora UMA vez.
 *
 * USO (no fim do caminho de saida, depois do save e dos locks):
 *   nx_port_exit_now(0);
 *
 * Com watchdog (arma um teto ANTES da parte que pode travar):
 *   nx_port_exit_watchdog(20);   // SIGALRM: _exit(status_pendente) em 20s
 *   ... save / teardown seguro ...
 *   nx_port_exit_now(0);         // caminho normal; cancela o watchdog
 *
 * Header-only, sem dependencias. flush de stdio ANTES do _exit para o log
 * nao perder as ultimas linhas (o _exit nao drena FILE*).
 */
#ifndef NXCOMPAT_EXIT_H
#define NXCOMPAT_EXIT_H

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

static volatile sig_atomic_t nx_port_exit_pending_status_;

static void nx_port_exit_alarm_(int signal_number) {
  (void)signal_number;
  /* Handler async-signal-safe: so' o _exit. O flush ja' aconteceu (ou nao
   * havia o que salvar de log que valha travar o aparelho). */
  _exit((int)nx_port_exit_pending_status_);
}

/* Arma um teto em segundos: se o teardown travar (o caso classico e' o
 * desmonte de GL no fbdev), o processo sai mesmo assim com `status`. */
static inline void nx_port_exit_watchdog(unsigned seconds_limit) {
  struct sigaction action;
  action.sa_handler = nx_port_exit_alarm_;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  sigaction(SIGALRM, &action, NULL);
  alarm(seconds_limit);
}

/* Saida terminal: flush do log e _exit imediato. NUNCA retorna. Chamar so'
 * DEPOIS do save e da liberacao de locks proprios. */
static inline void nx_port_exit_now(int status) {
  nx_port_exit_pending_status_ = status;
  fflush(NULL);
  fsync(STDOUT_FILENO);
  fsync(STDERR_FILENO);
  _exit(status);
}

#ifdef __cplusplus
}
#endif

#endif /* NXCOMPAT_EXIT_H */
