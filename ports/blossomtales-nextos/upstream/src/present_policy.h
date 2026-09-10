/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SB_PRESENT_POLICY_H
#define SB_PRESENT_POLICY_H
/* V5 (mission 7A.3 / FV5): the owner's video.aspect policy applied to the
 * REAL present (the full-size FBO bypass) and to the cursor/touch transform.
 * Sources, in the NEXTOS authority elected by nxproject.video: the owner hook
 * export NX_VIDEO_ASPECT (port-env.sh), else NEXTOSSETTINGS.txt
 * (NEXTOS_SETTINGS/2, key video.aspect), else the package default (stretch).
 * `auto` is the Blossom total algorithm: |Rd-Rs| <= 0.01 -> engine (no-op),
 * else preserve. Invalid settings follow video.invalid_policy=package_default
 * (bytes untouched, diagnostic with line, package default applied). */
#include "nxcompat_video.h"

/* Resolve once (idempotent). Reads $GAMEDIR/NEXTOSSETTINGS.txt when GAMEDIR
 * is exported, otherwise ./NEXTOSSETTINGS.txt after nxbootstrap's chdir.
 * Returns the effective REQUESTED policy (may be AUTO). */
nxcompat_video_aspect sb_present_policy_requested(void);
const char *sb_present_policy_source(void);   /* "port-env" | "settings" | "package-default" */

/* The decision for a source FBO of sw×sh presented on a drawable dw×dh;
 * cached per geometry; emits the NX-VIDEO/1 receipt once per change. */
const nxcompat_video_decision *sb_present_policy_decide(int sw, int sh, int dw, int dh);
/* Last decision (NULL until the first present). */
const nxcompat_video_decision *sb_present_policy_last(void);
/* Transform a drawable pixel (cursor/touch) to the surface space the game
 * believes it has (surface_w×surface_h = drawable), through the SAME content
 * rect. Returns 1 inside the rect, 0 outside (clamped). Identity when no
 * decision exists yet or the policy is engine/stretch. */
int sb_present_policy_touch(float dx, float dy, float *sx, float *sy);
#endif
