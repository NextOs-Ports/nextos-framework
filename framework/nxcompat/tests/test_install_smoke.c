/* SPDX-License-Identifier: GPL-3.0-only */
/* Link-only smoke for the installed public M12 surface.  Never execute it. */
#include "nxcompat.h"
#include "nxcompat_sdl2.h"
#include "nxgl.h"
#include "nxgl_nxcompat.h"
#include "nxinput.h"
#include "nxinput_nxcompat.h"

static const char *(*volatile reason_name_fn)(nxcompat_reason_code) =
    nxcompat_reason_name;
static nxcompat_result_code (*volatile audio_receipt_fn)(
    const nxcompat_sdl2_options *, uint64_t, nxcompat_backend_result_v2 *,
    nxcompat_audio_receipt *) = nxcompat_sdl2_negotiate_audio_v2;
static void (*volatile nxgl_init_fn)(nxgl_open_options *) =
    nxgl_open_options_init;
static nxcompat_result_code (*volatile graphics_receipt_fn)(
    nxcompat_registry *, nxgl_context *, uint64_t,
    nxcompat_graphics_receipt *) = nxgl_nxcompat_publish_context;
static void (*volatile nxinput_init_fn)(nxinput_config *) =
    nxinput_config_init;
static nxcompat_result_code (*volatile input_receipt_fn)(
    nxcompat_registry *, const nxinput_context *, nxcompat_input_receipt *) =
    nxinput_nxcompat_publish_context;

int main(void) {
  return reason_name_fn != NULL && audio_receipt_fn != NULL &&
                 nxgl_init_fn != NULL && graphics_receipt_fn != NULL &&
                 nxinput_init_fn != NULL && input_receipt_fn != NULL
             ? 0
             : 1;
}
