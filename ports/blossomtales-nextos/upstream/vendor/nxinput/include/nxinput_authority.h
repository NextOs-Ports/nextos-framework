/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * nxinput_authority -- V4-CONTROLLERS-03 / C3 (mission 114A): the PRODUCTION
 * adapter that makes nxinput_sovereign the only mapping decision in the path
 * the game actually executes.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * `nxinput_sovereign` is pure: it decides, but it touches nothing. Before
 * mission 114A the production `nxinput.c` still loaded SDL mappings by hand
 * (SDL_GAMECONTROLLERCONFIG_FILE + SDL_GAMECONTROLLERCONFIG, applied blind),
 * so the sovereign order existed only in tests. This adapter closes that gap:
 * it collects the real sources, measures the real pad, injects the real
 * setter+readback and turns the decision into the runtime's state. There is
 * no second path: `nxinput.c` no longer applies a mapping itself.
 *
 * The SDL boundary is a vtable so the adapter can be exercised hermetically
 * against a scripted runtime (connect/disconnect, hostile readback, missing
 * database) without a device. The functions under test are the SAME ones the
 * port runs.
 */
#ifndef NXINPUT_AUTHORITY_H
#define NXINPUT_AUTHORITY_H

#include "nxinput_sovereign.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_AUTHORITY_API_VERSION 1u
/* Largest CFW database / port bundle the adapter will read. A source bigger
 * than this is treated as unreadable and yields to the next authority; it is
 * never truncated into a decision. */
#define NXINPUT_AUTHORITY_SOURCE_MAX (4u * 1024u * 1024u)
#define NXINPUT_AUTHORITY_MAX_DEVICES 8u

/* The three source names of the PortMaster/CFW environment. They are read,
 * never written, and none of them decides anything by itself. */
#define NXINPUT_AUTHORITY_ENV_MAPPING "SDL_GAMECONTROLLERCONFIG"
#define NXINPUT_AUTHORITY_ENV_DATABASE "SDL_GAMECONTROLLERCONFIG_FILE"
#define NXINPUT_AUTHORITY_ENV_BUNDLE "NXCONTROLLER_PROFILES"

/* The runtime seam. Every member is mandatory. */
typedef struct nxinput_authority_runtime {
  uint32_t api_version;
  size_t struct_size;
  void *userdata;
  /* Environment lookup; NULL or "" means the source is absent. */
  const char *(*getenv_fn)(void *userdata, const char *name);
  /* Read a whole text file, NUL-terminated. 0 on success, -1 when the file
   * is missing, unreadable or larger than `cap`. */
  int (*read_text_fn)(void *userdata, const char *path, char *out,
                      size_t cap);
  /* The pad's stable GUID (32 lowercase hex + NUL). 0 on success. */
  int (*device_guid_fn)(void *userdata, int device_index, char *out,
                        size_t cap);
  /* MEASURED physical counts of the opened pad. 0 on success. */
  int (*device_caps_fn)(void *userdata, int device_index, int *buttons,
                        int *axes, int *hats);
  /* Apply one mapping line to the real runtime. 0 on success. */
  int (*apply_mapping_fn)(void *userdata, const char *line);
  /* What the runtime EFFECTIVELY reports for `guid` right now. 0 on
   * success. This is the readback: an apply whose readback is not
   * semantically identical never wins. */
  int (*mapping_for_guid_fn)(void *userdata, const char *guid, char *out,
                             size_t cap);
  /* 1 when the runtime carries its own mapping database (authority 4). */
  int runtime_has_builtin;
  /* 1 only when the consumer declared it understands raw pads (authority 5). */
  int consumer_accepts_raw;
} nxinput_authority_runtime;

/* One admitted pad. `line` is the winning mapping, BYTE-INTACT. */
typedef struct nxinput_authority_entry {
  int in_use;
  int32_t instance_id;
  char guid[NXINPUT_SOVEREIGN_GUID_MAX];
  int buttons;
  int axes;
  int hats;
  nxinput_sovereign_source source;
  char line[NXINPUT_SOVEREIGN_LINE_MAX];
  uint32_t resolved_generation; /* bumped on every fresh resolution */
} nxinput_authority_entry;

typedef struct nxinput_authority {
  uint32_t api_version;
  size_t struct_size;
  nxinput_authority_runtime runtime;
  nxinput_authority_entry entries[NXINPUT_AUTHORITY_MAX_DEVICES];
  uint32_t resolutions; /* how many times the sovereign order ran */
  uint32_t admitted;    /* pads currently admitted */
  uint32_t refused;     /* pads refused before gameplay */
  uint32_t forgotten;   /* disconnections that invalidated an entry */
  uint32_t generation;
} nxinput_authority;

int nxinput_authority_init(nxinput_authority *authority,
                           const nxinput_authority_runtime *runtime);

/* Run the sovereign order for one device and, when it wins, install the
 * decision in the runtime and record it under `instance_id`.
 *
 * Any previous entry for `instance_id` is DISCARDED first: a reconnection is
 * always re-resolved from the pad's CURRENT GUID and CURRENT measured
 * capabilities, and never inherits the previous device's mapping.
 *
 * Returns 0 when the pad is admitted (gameplay may proceed for it) and -1
 * when the decision is FAIL_EXPLICIT -- the caller MUST NOT reach gameplay
 * with that pad. `decision` receives the full outcome when non-NULL. */
int nxinput_authority_admit(nxinput_authority *authority, int device_index,
                            int32_t instance_id,
                            nxinput_sovereign_decision *decision);

/* Disconnection: the entry is erased, so no stale GUID, capability or
 * mapping survives into the next device that takes the slot. */
void nxinput_authority_forget(nxinput_authority *authority,
                              int32_t instance_id);

const nxinput_authority_entry *nxinput_authority_find(
    const nxinput_authority *authority, int32_t instance_id);

#ifdef __cplusplus
}
#endif

#endif /* NXINPUT_AUTHORITY_H */
