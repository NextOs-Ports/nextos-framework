/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_GPTK_LOADER_H
#define NXINPUT_GPTK_LOADER_H

/* Safe owner/default selection for NEXTOSCONTROLLERS.gptk.
 *
 * The launcher owns persistence: it materializes the owner's editable file
 * atomically and never overwrites a customized copy.  This loader owns the
 * runtime half of that contract.  It reads only the fixed basename
 * NEXTOSCONTROLLERS.gptk through caller-opened directory descriptors, refuses
 * symlinks and non-regular files, enforces the parser's 64 KiB limit, validates
 * every action against the adapter allowlist and selects the immutable default
 * for this session when the owner file is absent or rejected.  It never writes,
 * renames, repairs or deletes either file.
 *
 * Directory descriptors make the API independent of a host path, CFW layout or
 * current working directory.  Open the game directory and its defaults/
 * directory with O_DIRECTORY|O_NOFOLLOW, then pass the descriptors here.
 */

#include "nxinput_gptk.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_GPTK_LOAD_API_VERSION 1u
#define NXINPUT_GPTK_LOAD_RECEIPT_SCHEMA "nxinput-gptk-load-evidence/1"
#define NXINPUT_GPTK_SHA256_HEX_SIZE 65u
#define NXINPUT_GPTK_LOAD_ERROR_MAX 160u

typedef enum nxinput_gptk_load_source {
  NXINPUT_GPTK_LOAD_NONE = 0,
  NXINPUT_GPTK_LOAD_OWNER,
  NXINPUT_GPTK_LOAD_DEFAULT_OWNER_MISSING,
  NXINPUT_GPTK_LOAD_DEFAULT_OWNER_REJECTED
} nxinput_gptk_load_source;

typedef struct nxinput_gptk_load_receipt {
  uint32_t api_version;
  uint8_t source;
  uint8_t owner_present;
  uint16_t reserved;
  int result_code;
  int owner_error_code;
  int default_error_code;
  size_t owner_bytes;
  size_t default_bytes;
  size_t selected_bytes;
  char owner_sha256[NXINPUT_GPTK_SHA256_HEX_SIZE];
  char default_sha256[NXINPUT_GPTK_SHA256_HEX_SIZE];
  char selected_sha256[NXINPUT_GPTK_SHA256_HEX_SIZE];
  char owner_error[NXINPUT_GPTK_LOAD_ERROR_MAX];
  char default_error[NXINPUT_GPTK_LOAD_ERROR_MAX];
  /* C4 (additive member, receipt schema unchanged): which NEXTOSCONTROLLERS
   * format the SELECTED map declared -- 1, 2, or 0 when nothing was
   * selected. A reader that predates C4 simply ignores it. */
  uint32_t selected_gptk_schema;
} nxinput_gptk_load_receipt;

/* Load and validate the immutable default first, then the owner's editable
 * file.  `owner_dir_fd` names the game directory; `defaults_dir_fd` names its
 * defaults/ directory.  Both reads use the fixed basename
 * NEXTOSCONTROLLERS.gptk.  `allowed` is the adapter's semantic-action allowlist
 * and may not be empty when the map contains bindings.
 *
 * Returns 0 when either owner or default was selected.  A rejected owner is
 * preserved byte-for-byte and produces a DEFAULT_OWNER_REJECTED receipt.  If
 * no valid default exists, returns the positive NXI code, clears *out and sets
 * source=NONE.  The function never mutates the filesystem.
 */
int nxinput_gptk_load_at(int owner_dir_fd, int defaults_dir_fd,
                         const char *const *allowed, size_t allowed_count,
                         nxinput_gptk *out,
                         nxinput_gptk_load_receipt *receipt);

const char *nxinput_gptk_load_source_name(nxinput_gptk_load_source source);

/* Serialize the bounded, path-free load receipt.  Returns 0, or -1 for NULL,
 * an invalid receipt or an undersized output buffer.  The JSON never includes
 * a host path, device name, controller identity or contents from the mapping.
 */
int nxinput_gptk_load_receipt_json(const nxinput_gptk_load_receipt *receipt,
                                   char *json, size_t json_size);

#ifdef __cplusplus
}
#endif

#endif
