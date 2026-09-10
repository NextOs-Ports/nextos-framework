/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXINPUT_LIVEDB_H
#define NXINPUT_LIVEDB_H

/*
 * nxinput_livedb -- nxinput 0.10.0: bounded acquisition of the CFW's LIVE
 * controller database (authority 2) when the PortMaster environment did not
 * hand one over.
 *
 * THE FIELD FACT THIS EXISTS FOR: on the affected CFW the runtime database
 * /usr/lib/gamecontrollerdb.txt is a SYMLINK that the boot recreates in
 * background (sdl_map.sh &). A port that starts early sees a dead link -- and
 * 0.9.0 then silently lost authorities 1 AND 2, leaving the frozen bundle to
 * decide a layout that is a USER PREFERENCE on that CFW.
 *
 * CONTRACT (mission 5.5):
 *   1. an explicitly declared SDL_GAMECONTROLLERCONFIG_FILE is used first;
 *      when it is unreadable the authority simply yields -- an explicit path
 *      NEVER authorizes searching anywhere else;
 *   2. with no declared path, the canonical runtime paths
 *      /usr/lib/gamecontrollerdb.txt and /usr/lib32/gamecontrollerdb.txt are
 *      consulted, selected by a FACT of the running process (its pointer
 *      width), never by a CFW name, model or GUID;
 *   3. a dead/missing canonical link is retried at most
 *      NXINPUT_LIVEDB_MAX_ATTEMPTS times of NXINPUT_LIVEDB_RETRY_NS each,
 *      under the absolute NXINPUT_LIVEDB_BUDGET_NS monotonic ceiling,
 *      EINTR-safe;
 *   4. clock, sleep and filesystem access are INJECTED so tests measure the
 *      exact attempts/elapsed with fake time and never sleep for real;
 *   5. the content is a STABLE SNAPSHOT: opened once, read from that fd, and
 *      the identity (inode/device/size) re-verified after the read, so a
 *      modern<->retro swap during the wait can never produce a mixed view;
 *   6. symlink loops, FIFOs, directories, devices, oversize, embedded NUL
 *      and a swap during the read all yield without ever blocking;
 *   7. the receipt records only the path CLASS, the sanitized link target
 *      (modern|retro|other), the attempts and the elapsed time -- never a
 *      full personal path.
 *
 * This acquisition runs once per admission/hotplug, never in a frame loop.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXINPUT_LIVEDB_API_VERSION 1u
#define NXINPUT_LIVEDB_MAX_ATTEMPTS 20u
#define NXINPUT_LIVEDB_RETRY_NS 25000000ull   /* 25 ms between attempts */
#define NXINPUT_LIVEDB_BUDGET_NS 500000000ull /* absolute monotonic ceiling */
#define NXINPUT_LIVEDB_TARGET_MAX 8u          /* "modern"/"retro"/"other" */

#define NXINPUT_LIVEDB_PATH_LIB "/usr/lib/gamecontrollerdb.txt"
#define NXINPUT_LIVEDB_PATH_LIB32 "/usr/lib32/gamecontrollerdb.txt"

typedef enum nxinput_livedb_path_class {
  NXINPUT_LIVEDB_PATH_NONE = 0, /* nothing engaged */
  NXINPUT_LIVEDB_PATH_DECLARED, /* SDL_GAMECONTROLLERCONFIG_FILE */
  NXINPUT_LIVEDB_PATH_CANONICAL /* the runtime path for this process */
} nxinput_livedb_path_class;

/* What one probe of the path found. */
typedef enum nxinput_livedb_probe {
  NXINPUT_LIVEDB_PROBE_ABSENT = 0, /* missing, or a link with a dead target */
  NXINPUT_LIVEDB_PROBE_READY,      /* a regular file is reachable */
  NXINPUT_LIVEDB_PROBE_UNSAFE      /* FIFO/dir/device/loop/oversize/... */
} nxinput_livedb_probe;

typedef struct nxinput_livedb_receipt {
  uint32_t api_version;
  size_t struct_size;
  uint8_t path_class;  /* nxinput_livedb_path_class */
  char target[NXINPUT_LIVEDB_TARGET_MAX]; /* sanitized: modern|retro|other */
  unsigned int attempts;
  uint64_t elapsed_ns;
  int acquired; /* 1 when content_out holds a stable snapshot */
} nxinput_livedb_receipt;

/* Injected effects. Every member is mandatory. */
typedef struct nxinput_livedb_ops {
  uint32_t api_version;
  size_t struct_size;
  void *userdata;
  uint64_t (*monotonic_ns)(void *userdata);
  /* Sleep for `ns`, absorbing EINTR internally. Returns 0, or -1 to abort
   * the wait early (the acquisition then yields). */
  int (*sleep_ns)(void *userdata, uint64_t ns);
  /* Probe `path` without reading it. Fills *probe and, when the path is a
   * symlink, copies its target's BASENAME (bounded, NUL-terminated) into
   * link_target; otherwise link_target becomes "". Returns 0, -1 on a
   * structural failure (treated as UNSAFE). */
  int (*probe_fn)(void *userdata, const char *path, int *probe,
                  char *link_target, size_t cap);
  /* Read a stable snapshot of `path` into out (NUL-terminated). Must verify
   * regular file, size below cap, identity unchanged across the read, no
   * embedded NUL. Returns 0, or -1 when no coherent snapshot exists. */
  int (*snapshot_fn)(void *userdata, const char *path, char *out, size_t cap);
} nxinput_livedb_ops;

/* The production ops (real clock, real nanosleep, real filesystem). */
const nxinput_livedb_ops *nxinput_livedb_default_ops(void);

/* Acquire the live database once for this admission.
 *
 * `declared_path` is the value of SDL_GAMECONTROLLERCONFIG_FILE (NULL/""
 * when absent). `pointer_bits` is sizeof(void*)*8 of the RUNNING process.
 * On success returns 0 and content_out holds the snapshot; on any yield
 * returns -1 (the ladder simply proceeds to the next authority). The
 * receipt is always filled. */
int nxinput_livedb_acquire(const nxinput_livedb_ops *ops,
                           const char *declared_path, int pointer_bits,
                           char *content_out, size_t cap,
                           nxinput_livedb_receipt *receipt);

const char *nxinput_livedb_path_class_name(int path_class);

#ifdef __cplusplus
}
#endif

#endif /* NXINPUT_LIVEDB_H */
