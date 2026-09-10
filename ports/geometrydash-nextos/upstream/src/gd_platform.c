/*
 * gd_platform.c -- instancia unica travada no PROPRIO BINARIO.
 *
 * Garantia que o script do launcher nao consegue dar sozinho: trava so' no
 * script cai junto com o script. O shell morre, a trava some, e o jogo segue
 * dono do display e do audio -- ai' a segunda abertura entra por cima da
 * primeira e as duas brigam pelo mesmo contexto.
 *
 * A varredura de /proc que o run.sh faz antes de abrir continua valendo: ela
 * limpa instancia comprovadamente antiga. Esta e' a trava final.
 *
 * Molde: ports/chrono/src/ct_platform.c (release publicada e validada no R36S).
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "gd_platform.h"
#include "util.h"

/* ------------------------------------------------------- instancia unica --- */

static int g_lock_fd = -1;

int gd_single_instance_lock(void) {
  /* O caminho e' o do executavel em /proc: vale mesmo se o binario for
   * chamado por caminho relativo, por symlink ou de outro diretorio. */
  char self[1024];
  ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
  if (n <= 0) {
    debugPrintf("[trava] nao consegui resolver o proprio caminho (%s); "
                "seguindo sem trava\n", strerror(errno));
    return 0;
  }
  self[n] = 0;

  g_lock_fd = open(self, O_RDONLY | O_CLOEXEC);
  if (g_lock_fd < 0) {
    debugPrintf("[trava] nao consegui abrir %s (%s); seguindo sem trava\n",
                self, strerror(errno));
    return 0;
  }
  if (flock(g_lock_fd, LOCK_EX | LOCK_NB) < 0) {
    debugPrintf("[trava] ja ha uma instancia deste jogo rodando (%s)\n",
                strerror(errno));
    close(g_lock_fd);
    g_lock_fd = -1;
    return -1;
  }
  debugPrintf("[trava] instancia unica adquirida em %s\n", self);
  return 0;
}

void gd_single_instance_unlock(void) {
  if (g_lock_fd >= 0) {
    flock(g_lock_fd, LOCK_UN);
    close(g_lock_fd);
    g_lock_fd = -1;
  }
}
