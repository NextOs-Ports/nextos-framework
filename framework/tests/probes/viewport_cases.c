/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Escolha de viewport do nxgl exercitada com fatos MEDIDOS de um ambiente
 * local. O programa não abre janela, não inicializa vídeo e não toca em
 * DRM, Mali ou framebuffer: ele só alimenta o seletor puro e imprime o que
 * ele decidiu.
 *
 *   viewport_cases <sdl_desktop_w> <sdl_desktop_h> <drm_w> <drm_h>
 *                  <fbdev_w> <fbdev_h>
 *
 * Zero em um par significa "essa fonte não sabe".
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "nxgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  nxgl_resolution_sources sources;
  nxgl_resolution resolution;
  int result;

  if (argc != 7) {
    (void)fprintf(stderr,
                  "usage: viewport_cases desktop_w desktop_h drm_w drm_h "
                  "fbdev_w fbdev_h\n");
    return 2;
  }
  nxgl_resolution_sources_init(&sources);
  sources.sdl_desktop_width = atoi(argv[1]);
  sources.sdl_desktop_height = atoi(argv[2]);
  sources.drm_width = atoi(argv[3]);
  sources.drm_height = atoi(argv[4]);
  sources.fbdev_width = atoi(argv[5]);
  sources.fbdev_height = atoi(argv[6]);

  memset(&resolution, 0, sizeof(resolution));
  result = nxgl_choose_resolution(&sources, &resolution);
  if (result != NXGL_SUCCESS) {
    (void)printf("chosen=none result=%d\n", result);
    return 0;
  }
  (void)printf("chosen=%dx%d source=%s\n", resolution.width,
               resolution.height,
               nxgl_resolution_source_name(resolution.source));
  return 0;
}
