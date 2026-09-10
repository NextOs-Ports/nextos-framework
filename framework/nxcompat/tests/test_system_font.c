/* SPDX-License-Identifier: GPL-3.0-only */
/* Sonda do gate. Dois modos:
 *   find <preferred> <env_name> <extra_root>   -> found=N path=...
 *   signature <path>                           -> signature=N               */
#include "nxcompat_system_font.h"

int main(int argc, char **argv) {
  char path[NXCOMPAT_SYSTEM_FONT_PATH_MAX];

  if (argc > 2 && strcmp(argv[1], "signature") == 0) {
    printf("signature=%d\n",
           nxcompat_system_font_accept(argv[2], path, sizeof path));
    return 0;
  }
  {
    const char *preferred = (argc > 2 && argv[2][0] != '\0') ? argv[2] : NULL;
    const char *env_name = (argc > 3 && argv[3][0] != '\0') ? argv[3] : NULL;
    const char *roots[1];
    size_t root_count = 0u;
    int found;

    if (argc > 4 && argv[4][0] != '\0') {
      roots[0] = argv[4];
      root_count = 1u;
    }
    found = nxcompat_system_font_find(preferred, env_name,
                                      root_count ? roots : NULL, root_count,
                                      path, sizeof path);
    printf("found=%d path=%s\n", found, found ? path : "-");
    /* Invariante: o que a busca devolve TEM de passar na checagem de
     * assinatura. E' o que separa "achou uma fonte" de "achou um arquivo com
     * nome de fonte" -- e o segundo caso devolve o jogo mudo. */
    if (found) {
      char again[NXCOMPAT_SYSTEM_FONT_PATH_MAX];
      printf("valid=%d\n", nxcompat_system_font_accept(path, again,
                                                        sizeof again));
    }
  }
  return 0;
}
