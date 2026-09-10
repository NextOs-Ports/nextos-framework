/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_GPTK_UPGRADE_H
#define NXINPUT_GPTK_UPGRADE_H

/* V4-CONTROLLERS-03 / C4: offering a new NEXTOSCONTROLLERS.gptk to an owner
 * who already edited theirs.
 *
 * THE RULE: the owner's file is never overwritten, never repaired, never
 * renamed and never deleted. A port update that carries a different default
 * writes a SIBLING, `NEXTOSCONTROLLERS.gptk.new`, plus a bounded semantic
 * diff, and stops there. The old file keeps working until the owner adopts
 * the new one explicitly -- adoption is a human act, not a side effect of
 * launching the game.
 *
 * Every filesystem operation is relative to a caller-opened directory
 * descriptor and O_NOFOLLOW, so no path in this module can be redirected by
 * a symlink, and nothing outside the authorized root can be reached or
 * written: there is no path string to traverse. The `.new` file is created
 * through a temporary sibling and renameat(), so a reader never observes a
 * half-written file, and renameat() replaces the LINK, never following one.
 */

#include "nxinput_gptk.h"
#include "nxinput_gptk_loader.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_GPTK_UPGRADE_API_VERSION 1u
#define NXINPUT_GPTK_UPGRADE_SUFFIX ".new"
#define NXINPUT_GPTK_UPGRADE_DIFF_MAX 2048u
#define NXINPUT_GPTK_UPGRADE_SHA_SIZE NXINPUT_GPTK_SHA256_HEX_SIZE

typedef enum nxinput_gptk_upgrade_status {
  /* Nothing to do: the owner already holds these exact bytes. No `.new`. */
  NXINPUT_GPTK_UPGRADE_UP_TO_DATE = 0,
  /* The owner has no file yet: materializing the first copy belongs to the
   * launcher, not to an upgrade. Nothing is written here. */
  NXINPUT_GPTK_UPGRADE_OWNER_MISSING,
  /* A `.new` was written and the owner's file was left untouched. */
  NXINPUT_GPTK_UPGRADE_OFFERED,
  /* Refused, fail-closed. The owner's file is untouched. */
  NXINPUT_GPTK_UPGRADE_REFUSED
} nxinput_gptk_upgrade_status;

typedef struct nxinput_gptk_upgrade_report {
  uint32_t api_version;
  int status;         /* nxinput_gptk_upgrade_status */
  int owner_present;
  int owner_parsed;   /* 0 or the NXI code the owner's file failed with */
  int candidate_code; /* 0 or the NXI code the candidate failed with */
  size_t owner_bytes;
  size_t candidate_bytes;
  /* Semantic diff counts over (context, control). */
  unsigned int changed;
  unsigned int enabled;   /* was null/absent -> now an action or native */
  unsigned int disabled;  /* was an action/native -> now null */
  char owner_sha256[NXINPUT_GPTK_UPGRADE_SHA_SIZE];
  char candidate_sha256[NXINPUT_GPTK_UPGRADE_SHA_SIZE];
  /* Bounded, sanitized, PATH-FREE: control/context/decision names only,
   * never a host path, a device name or the owner's identity. */
  char diff[NXINPUT_GPTK_UPGRADE_DIFF_MAX];
  char error[NXINPUT_GPTK_LOAD_ERROR_MAX];
} nxinput_gptk_upgrade_report;

/* Offer `candidate` to the owner whose editable copy lives in the directory
 * named by `owner_dir_fd`.
 *
 * Never touches NEXTOSCONTROLLERS.gptk. Writes at most
 * NEXTOSCONTROLLERS.gptk.new, atomically. Returns 0 when the report is
 * meaningful (including UP_TO_DATE / OWNER_MISSING) and -1 on a structural
 * argument error. A REFUSED status is reported, not silently swallowed:
 * a candidate that does not parse, an owner file that is a symlink or not a
 * regular file, or an existing `.new` that is a symlink all refuse. */
int nxinput_gptk_upgrade_offer_at(int owner_dir_fd, const char *candidate,
                                  size_t candidate_length,
                                  nxinput_gptk_upgrade_report *report);

const char *nxinput_gptk_upgrade_status_name(int status);

#ifdef __cplusplus
}
#endif

#endif
