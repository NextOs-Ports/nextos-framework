/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_GODOT_SEAM_H
#define NXINPUT_GODOT_SEAM_H

/*
 * nxinput_godot_seam -- V4-CONTROLLERS-03 / C5B: the PRODUCTION seam that is
 * compiled and linked INTO the Godot binary.
 *
 * WHY THIS EXISTS
 * ---------------
 * C5A ran the consumer offline, after the engine had exited, with ops backed
 * by a file. The external audit refused it, correctly: a real engine that is
 * merely present does not make a separate program the engine's setter,
 * readback and announce. This file is the answer. It is called from inside
 * JoypadLinux::open_joypad(), on the engine's own thread, with the engine's
 * own fd, immediately BEFORE input->joy_connection_changed() -- so a pad that
 * is not admitted is never announced, never reaches InputMap, `_input` or
 * polling, and never reaches the game.
 *
 * ORDER, enforced here and nowhere else:
 *   exact device fd -> C3 authority -> authenticated mapping+origin ->
 *   engine setter -> engine readback -> independent probes -> announce
 *
 * The engine-side pieces arrive through nxinput_godot_seam_engine, which the
 * per-major glue fills with the real API of the executed version. This file
 * never talks to Godot directly, so it stays C and is testable, but it is
 * only ever RUN inside the engine process.
 */

#include "nxinput_godot.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_GODOT_SEAM_API_VERSION 1u
#define NXINPUT_GODOT_SEAM_MAX_PROBES 32u

/* The engine operations, bound to the executed major. Every one of these must
 * be the real API of that engine; the seam refuses a partial table. */
typedef struct nxinput_godot_seam_engine {
  uint32_t api_version;
  size_t struct_size;
  void *userdata;
  uint8_t major;                 /* nxinput_godot_engine */
  /* Real setter of this engine (Input/InputDefault::add_joy_mapping). */
  int (*add_joy_mapping)(void *userdata, const char *mapping);
  /* Does the engine now hold a mapping for this joy id? */
  int (*has_mapping)(void *userdata, int joy_id);
  /* The engine's OWN resolution: logical button for a physical button index,
   * or -1. Read from the engine's stored mapping, never recomputed. */
  int (*readback_logical_button)(void *userdata, int joy_id,
                                 int physical_button);
  /* Monotonic nanoseconds, PID and TID, from the engine process. */
  uint64_t (*monotonic_ns)(void *userdata);
  long (*pid)(void *userdata);
  long (*tid)(void *userdata);
  /* Append one receipt line durably. Must not go through $WORK. */
  void (*receipt)(void *userdata, const char *line);
} nxinput_godot_seam_engine;

/* What the seam was told about this pad, measured by the caller from the
 * ENGINE's own fd for this joy id. */
typedef struct nxinput_godot_seam_device {
  uint32_t api_version;
  size_t struct_size;
  int joy_id;
  int fd;                        /* the engine's fd for THIS pad */
  char guid[40];                 /* as the engine computed it */
  char name[128];
  char devpath[256];
  const unsigned long *key_bits;
  size_t key_bit_count;
  const unsigned long *abs_bits;
  size_t abs_bit_count;
  /* 116B: EVIOCGABS answers for THIS fd, indexed by ABS code. Without them
   * an axis binding cannot be validated and the pad is blocked. */
  const nxinput_godot_absinfo *abs_info;
  size_t abs_info_count;
} nxinput_godot_seam_device;

typedef enum nxinput_godot_seam_result {
  NXINPUT_GODOT_SEAM_ADMIT = 0,     /* the engine may announce this pad */
  NXINPUT_GODOT_SEAM_NO_DECLARATION, /* nothing declared: pass-through */
  NXINPUT_GODOT_SEAM_BLOCK_ORIGIN,
  NXINPUT_GODOT_SEAM_BLOCK_IDENTITY,
  NXINPUT_GODOT_SEAM_BLOCK_MAPPING,
  NXINPUT_GODOT_SEAM_BLOCK_SETTER,
  NXINPUT_GODOT_SEAM_BLOCK_READBACK,
  NXINPUT_GODOT_SEAM_BLOCK_PROBES,
  NXINPUT_GODOT_SEAM_BLOCK_ENGINE_TABLE
} nxinput_godot_seam_result;

/* Decide, inside the engine, whether this pad may be announced.
 *
 * Returns ADMIT only after the engine accepted the mapping AND its own
 * readback agreed on every probe. Any other value means the caller MUST NOT
 * announce the pad. NO_DECLARATION means the port declared no mapping for
 * this run; the engine keeps its native behaviour (the seam adds nothing and
 * takes nothing away).
 *
 * `declaration_path` is where the authenticated C3 declaration lives; its
 * CONTENTS are what authorize anything -- the path alone authorizes nothing.
 */
nxinput_godot_seam_result nxinput_godot_seam_admit(
    const nxinput_godot_seam_engine *engine,
    const nxinput_godot_seam_device *device, const char *declaration_path);

const char *nxinput_godot_seam_result_name(nxinput_godot_seam_result r);

#ifdef __cplusplus
}
#endif

#endif /* NXINPUT_GODOT_SEAM_H */
