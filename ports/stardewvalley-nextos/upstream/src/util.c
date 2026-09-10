/*
 * util.c -- misc utility functions
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 * Adapted for Syberia ARM64 port
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

int debugPrintf(const char *text, ...) {
  va_list list;

  va_start(list, text);
  vfprintf(stderr, text, list);
  va_end(list);

  return 0;
}

int ret0(void) { return 0; }
int ret1(void) { return 1; }
int retm1(void) { return -1; }

/* Dir onde o binario mora = o GAMEDIR do port, seja qual for o CFW.
 *
 * Idioma do PortMaster: o launcher resolve o GAMEDIR (SCRIPT_DIR, depois
 * `$directory` do control.txt, depois /roms e /storage/roms), faz `cd` e nada
 * carrega caminho absoluto de aparelho. Aqui o binario fecha o circulo: le o
 * proprio caminho em /proc/self/exe, entao assets/libs/saves saem certos
 * mesmo se alguem rodar o executavel na mao, sem launcher e sem env.
 *
 * Antes os fallbacks eram "/storage/roms/ports/stardewvalley/..." cravado — o
 * caminho do NextOS/Mali-450 — e em qualquer outro CFW o jogo fechava no
 * "Saving..." com DirectoryNotFoundException. */
const char *sdv_base_dir(void) {
  static char dir[4096];
  if (dir[0]) return dir;
  ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
  if (n > 0) {
    dir[n] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) { *slash = '\0'; return dir; }
  }
  /* sem /proc: o launcher ja fez cd pro GAMEDIR */
  if (!getcwd(dir, sizeof dir)) snprintf(dir, sizeof dir, ".");
  return dir;
}

/* Monta "<base>/<sub>" num buffer estatico por chamador. */
const char *sdv_path_in_base(char *buf, size_t bufsz, const char *sub) {
  snprintf(buf, bufsz, "%s/%s", sdv_base_dir(), sub);
  return buf;
}
