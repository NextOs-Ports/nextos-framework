/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SWORDIGO_CRASH_DIAG_H
#define SWORDIGO_CRASH_DIAG_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
  CRASH_PHASE_BOOT = 0,
  CRASH_PHASE_RELOCATE,
  CRASH_PHASE_RESOLVE,
  CRASH_PHASE_FINALIZE,
  CRASH_PHASE_CONSTRUCTORS,
  CRASH_PHASE_GL,
  CRASH_PHASE_AUDIO,
  CRASH_PHASE_JNI,
  CRASH_PHASE_SET_DIRS,
  CRASH_PHASE_APP_LAUNCH,
  CRASH_PHASE_MUSIC,
  CRASH_PHASE_NATIVE_INTERFACE,
  CRASH_PHASE_APP_SETUP,
  CRASH_PHASE_VIEW_SIZE,
  CRASH_PHASE_APP_ACTIVE,
  CRASH_PHASE_EVENTS,
  CRASH_PHASE_CONTROLS,
  CRASH_PHASE_JNI_UPDATE,
  CRASH_PHASE_UPDATE,
  CRASH_PHASE_DRAW,
  CRASH_PHASE_CURSOR,
  CRASH_PHASE_PRESENT_ALPHA,
  CRASH_PHASE_FRAME_PROBE,
  CRASH_PHASE_FINISH,
  CRASH_PHASE_SWAP,
  CRASH_PHASE_SHUTDOWN
} CrashPhase;

/* Installs a bounded, async-signal-safe fatal reporter. It reports only and
 * exits with the conventional 128+signal status; it never skips a fault or
 * changes the game's lifecycle. Repeating the call is supported so the
 * adapter can reclaim the reporter after constructors/SDL initialization. */
void crash_diag_install(uintptr_t guest_base, size_t guest_size);
void crash_diag_set_phase(CrashPhase phase);
void crash_diag_set_frame(unsigned frame);

#endif
