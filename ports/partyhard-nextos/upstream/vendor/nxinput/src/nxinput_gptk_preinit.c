/* SPDX-License-Identifier: GPL-3.0-only */
/* nxinput_gptk_preinit -- see include/nxinput_gptk_preinit.h. */
#define _POSIX_C_SOURCE 200809L

#include "nxinput_gptk_preinit.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

int nxinput_gptk_preinit_load(const char *gamedir,
                              const char *const *allowed_actions,
                              size_t allowed_count,
                              nxinput_gptk_preinit_result *out) {
  int owner_fd;
  int defaults_fd;
  int rc;

  if (out == 0) {
    return -1;
  }
  memset(out, 0, sizeof *out);
  out->api_version = NXINPUT_GPTK_PREINIT_API_VERSION;
  out->struct_size = sizeof *out;
  out->face_layout = (uint8_t)NXINPUT_GPTK_FACE_LAYOUT_AUTO;
  if (gamedir == 0 || gamedir[0] == '\0') {
    return -1;
  }
  /* Real directories only, links refused: the same discipline the loader
   * applies to the files themselves. */
  owner_fd = open(gamedir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (owner_fd < 0) {
    out->rc = NXINPUT_GPTK_ERR_IO;
    return 0; /* the port stays native */
  }
  defaults_fd = openat(owner_fd, "defaults",
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (defaults_fd < 0) {
    (void)close(owner_fd);
    out->rc = NXINPUT_GPTK_ERR_IO;
    return 0;
  }
  rc = nxinput_gptk_load_at(owner_fd, defaults_fd, allowed_actions,
                            allowed_count, &out->map, &out->receipt);
  (void)close(defaults_fd);
  (void)close(owner_fd);
  out->rc = rc;
  if (rc != 0) {
    return 0;
  }
  out->loaded = 1;
  out->face_layout = (uint8_t)nxinput_gptk_face_layout_of(&out->map);
  return 0;
}
