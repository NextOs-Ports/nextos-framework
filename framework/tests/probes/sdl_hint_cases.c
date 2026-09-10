/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Casos de hint SDL de um ambiente local, decididos pelo sanitizador REAL do
 * nxgl contra a lista de drivers que a SDL daquela firmware realmente publica.
 * O arquivo e compartilhado entre ambientes: nada aqui conhece aparelho.
 *
 * A lista chega por argv, medida no proprio ambiente pelo probe sob qemu;
 * nada aqui inventa driver, inicializa video ou toca DRM/Mali/framebuffer.
 *
 *   sdl_hint_cases KMSDRM,dummy <hint-incompativel>
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "nxgl.h"
#include "nxgl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *drivers[NXGL_SDL_VIDEO_DRIVER_MAX];
static char driver_storage[NXGL_SDL_VIDEO_DRIVER_LIST_MAX];
static int driver_count;
static int failures;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                             \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

/* Seam do nxgl: aqui ele devolve a lista MEDIDA da firmware. */
int nxgl_test_sdl_get_num_video_drivers(void) { return driver_count; }

const char *nxgl_test_sdl_get_video_driver(int index) {
  if (index < 0 || index >= driver_count)
    return NULL;
  return drivers[index];
}

Uint32 nxgl_test_sdl_was_init(Uint32 flags) { (void)flags; return 0u; }

const char *nxgl_test_sdl_current_video_driver(void) { return NULL; }

static int load_drivers(const char *list) {
  char *cursor;
  if (strlen(list) + 1u > sizeof(driver_storage))
    return -1;
  memcpy(driver_storage, list, strlen(list) + 1u);
  cursor = driver_storage;
  while (*cursor) {
    char *comma = strchr(cursor, ',');
    if ((unsigned int)driver_count >= NXGL_SDL_VIDEO_DRIVER_MAX)
      return -1;
    if (comma)
      *comma = '\0';
    if (*cursor == '\0')
      return -1;
    drivers[driver_count++] = cursor;
    if (!comma)
      break;
    cursor = comma + 1;
  }
  return driver_count > 0 ? 0 : -1;
}

static nxgl_sdl_video_hint_receipt_v2 sanitize(const char *hint) {
  nxgl_sdl_video_hint_options_v2 options;
  nxgl_sdl_video_hint_receipt_v2 receipt;
  nxgl_sdl_video_hint_options_v2_init(&options);
  nxgl_sdl_video_hint_receipt_v2_init(&receipt);
  options.enabled = 1;
  if (hint)
    CHECK(setenv("SDL_VIDEODRIVER", hint, 1) == 0);
  else
    CHECK(unsetenv("SDL_VIDEODRIVER") == 0);
  CHECK(nxgl_sanitize_sdl_video_hint_v2(&options, &receipt) == NXGL_SUCCESS);
  return receipt;
}

int main(int argc, char **argv) {
  nxgl_sdl_video_hint_receipt_v2 receipt;
  char line[NXGL_SDL_VIDEO_RECEIPT_TEXT_MAX];

  if (argc != 3) {
    (void)fprintf(stderr, "usage: sdl_hint_cases DRIVER,LIST INCOMPATIBLE\n");
    return 2;
  }
  if (load_drivers(argv[1]) != 0) {
    (void)fprintf(stderr, "invalid measured driver list\n");
    return 2;
  }

  /* Caso 1 -- sem hint: a autodeteccao da SDL da firmware fica intacta. */
  receipt = sanitize(NULL);
  CHECK(receipt.inherited_hint_present == 0);
  CHECK(receipt.hint_removed == 0);
  CHECK(receipt.video_driver_count == driver_count);
  CHECK(getenv("SDL_VIDEODRIVER") == NULL);
  CHECK(nxgl_format_sdl_video_hint_receipt_v2(&receipt, line, sizeof(line)) ==
        NXGL_SUCCESS);
  (void)printf("case=no-hint action=%s drivers=%s removed=%d\n",
               nxgl_sdl_video_hint_action_name_v2(receipt.action),
               receipt.compiled_video_drivers, receipt.hint_removed);

  /* Caso 2 -- hint incompativel herdado do frontend 64-bit: e REMOVIDO, e a
   * SDL 32-bit volta a escolher entre os drivers que ela mesma compilou. */
  receipt = sanitize(argv[2]);
  CHECK(receipt.inherited_hint_present == 1);
  CHECK(receipt.inherited_hint_supported == 0);
  CHECK(receipt.hint_removed == 1);
  CHECK(strcmp(receipt.inherited_hint, argv[2]) == 0);
  CHECK(getenv("SDL_VIDEODRIVER") == NULL);
  CHECK(nxgl_format_sdl_video_hint_receipt_v2(&receipt, line, sizeof(line)) ==
        NXGL_SUCCESS);
  (void)printf("case=incompatible-hint inherited=%s action=%s removed=%d\n",
               receipt.inherited_hint,
               nxgl_sdl_video_hint_action_name_v2(receipt.action),
               receipt.hint_removed);

  /* Controle: um driver que a PROPRIA firmware publica nunca e removido. */
  receipt = sanitize(drivers[0]);
  CHECK(receipt.inherited_hint_present == 1);
  CHECK(receipt.inherited_hint_supported == 1);
  CHECK(receipt.hint_removed == 0);
  CHECK(getenv("SDL_VIDEODRIVER") != NULL);
  (void)printf("case=firmware-hint inherited=%s action=%s removed=%d\n",
               receipt.inherited_hint,
               nxgl_sdl_video_hint_action_name_v2(receipt.action),
               receipt.hint_removed);

  if (failures != 0) {
    (void)fprintf(stderr, "SDL hint cases: %d failure(s)\n", failures);
    return 1;
  }
  (void)printf("SDL hint cases: PASS measured_drivers=%d "
               "hardware_ran=0 device_access=0 video_initialized=0\n",
               driver_count);
  return 0;
}
