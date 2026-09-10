/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_GODOT_CONSUMER_H
#define NXINPUT_GODOT_CONSUMER_H

/*
 * nxinput_godot_consumer -- V4-CONTROLLERS-03 / C5 (audit 116A), SUPERSEDED
 * by nxinput_godot_seam for the Godot line.
 *
 * WHAT THE 116B AUDIT SETTLED ABOUT THIS FILE
 * -------------------------------------------
 * This module describes the right ORDER, but it is only a decision helper:
 * it is driven by ops the caller supplies, so on its own it proves nothing
 * about any engine. The 116A delivery called it the production path and ran
 * it OUT of the engine's process, after the engine had exited, with the ops
 * answered by a file -- and the audit refused that, correctly.
 *
 * The production path for Godot 3 and Godot 4 is nxinput_godot_seam.h, which
 * is compiled and LINKED INTO the engine binary and runs inside
 * JoypadLinux::open_joypad(), on the engine's own thread, before
 * joy_connection_changed(). Use that. This header stays for adapters that
 * genuinely embed it in their own runtime, and its claim class on its own is
 * FIXTURE_HOST: only a real engine executing it can be REAL_API_HOST.
 *
 * WHY THIS EXISTS
 * ---------------
 * `nxinput_godot_serve()` only computes a proposal. Computing a proposal and
 * re-reading it with the same functions proves nothing about the engine --
 * that was the audit's finding. This module is the part that must actually
 * run inside a port, in this exact order, BEFORE the joypad is announced:
 *
 *   1. measure the pad (EV_KEY/EV_ABS) through the host ops;
 *   2. resolve the mapping with the DECLARED origin (nxinput_godot_serve);
 *   3. hand the winning line to the ENGINE's real setter;
 *   4. read the effective state back from the ENGINE's real API;
 *   5. compare -- and only if the engine agrees, announce the joypad and
 *      emit the consumer receipt;
 *   6. otherwise BLOCK the announcement and open the doctor.
 *
 * Step 4 is the only readback that authorizes anything. The converter's own
 * self-check is internal consistency and is deliberately not accepted here.
 *
 * THREAD/LIFECYCLE: every op is called on the caller's thread, in the order
 * above, exactly once per admission. The module owns no thread, starts none,
 * and never calls back into the engine after `admit` returns.
 */

#include "nxinput_godot.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_GODOT_CONSUMER_API_VERSION 1u
#define NXINPUT_GODOT_CONSUMER_MAX_PROBES 24u

/* One button the caller wants proved end to end: a physical evdev code and
 * the logical index the SERVED mapping says the engine must report. */
typedef struct nxinput_godot_probe {
  unsigned int evdev_code;
  int expected_logical; /* -1 when the mapping binds nothing to it */
} nxinput_godot_probe;

/* The ENGINE's real operations. Every one of these must be backed by the
 * engine's own API; a stub that echoes the input defeats the purpose and is
 * refused by the gate that drives this module. */
typedef struct nxinput_godot_engine_ops {
  uint32_t api_version;
  size_t struct_size;
  void *userdata;
  /* The engine's mapping setter (Godot: Input.add_joy_mapping). 0 on ok. */
  int (*set_mapping)(void *userdata, const char *mapping);
  /* What the ENGINE reports as the pad's identity after the setter ran.
   * 0 on ok. */
  int (*read_identity)(void *userdata, char *guid, size_t guid_size,
                       char *name, size_t name_size);
  /* Which LOGICAL button the engine reports for a physical evdev code,
   * or -1 when the engine binds nothing to it. This is the effective
   * readback; it must come from the engine, never be recomputed here. */
  int (*read_logical_for_code)(void *userdata, unsigned int evdev_code);
  /* Announce the joypad to the game. Called ONLY after the engine agreed.
   * 0 on ok. */
  int (*announce)(void *userdata);
  /* Open the doctor with a stable reason. Called on every block. */
  void (*doctor)(void *userdata, const char *reason);
} nxinput_godot_engine_ops;

typedef enum nxinput_godot_admit_result {
  NXINPUT_GODOT_ADMIT_OK = 0,       /* engine agreed; joypad announced */
  NXINPUT_GODOT_ADMIT_BLOCKED_MAP,  /* serve() blocked before the engine */
  NXINPUT_GODOT_ADMIT_BLOCKED_SETTER,   /* the engine refused the mapping */
  NXINPUT_GODOT_ADMIT_BLOCKED_READBACK, /* engine state != what we served */
  NXINPUT_GODOT_ADMIT_BLOCKED_ANNOUNCE,
  NXINPUT_GODOT_ADMIT_INVALID
} nxinput_godot_admit_result;

typedef struct nxinput_godot_consumer_receipt {
  uint32_t api_version;
  size_t struct_size;
  uint8_t result;           /* nxinput_godot_admit_result */
  uint8_t announced;        /* 1 only when the engine agreed first */
  uint8_t engine_readback;  /* 1 when the ENGINE answered the readback */
  uint8_t reserved;
  unsigned int probes_checked;
  unsigned int probes_agreed;
  char engine_guid[40];
  char engine_name[64];
  nxinput_godot_evidence mapping_evidence;
  char reason[160];
} nxinput_godot_consumer_receipt;

/* Run the whole ordered admission. Returns the result and fills `receipt`.
 * The joypad is announced only on NXINPUT_GODOT_ADMIT_OK; on every other
 * outcome the doctor op is called and nothing is announced. */
nxinput_godot_admit_result nxinput_godot_consumer_admit(
    nxinput_godot_engine engine, const nxinput_godot_engine_ops *ops,
    const nxinput_godot_origin *origin, const nxinput_godot_caps *caps,
    const char *mapping, const nxinput_godot_probe *probes,
    unsigned int probe_count, nxinput_godot_consumer_receipt *receipt);

const char *nxinput_godot_admit_result_name(nxinput_godot_admit_result r);
int nxinput_godot_consumer_receipt_line(
    const nxinput_godot_consumer_receipt *receipt, char *out,
    size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* NXINPUT_GODOT_CONSUMER_H */
