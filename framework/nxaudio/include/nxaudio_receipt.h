/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef NXAUDIO_RECEIPT_H
#define NXAUDIO_RECEIPT_H

/* V3-AUDIO-01 (additive, C99): capability-measured audio receipt, callback
 * liveness, underrun accounting and the safe-exit pump contract. Nothing in
 * this header creates a thread, reads the environment, opens a device or
 * changes any existing nxaudio entry point. */

#include <stddef.h>
#include <stdint.h>

#include "nxaudio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NXAUDIO_RECEIPT_API_VERSION 1u
#define NXAUDIO_RECEIPT_VERSION "0.3.1"
/* Worst-case formatted line is well under this. */
#define NXAUDIO_RECEIPT_LINE_MAX 256u

/* One negotiated open, measured. `api` is the bridge that talked to the
 * device ("sdl2", "openal", "opensl", "aaudio", "fmod-bridge"); `backend` is
 * the backend/device string that api reported ("alsa", "pulseaudio", ...).
 * Neither field ever carries a device brand or model name; selection and
 * reporting are capability-measured only. */
typedef struct nxaudio_receipt {
  uint32_t api_version;
  size_t struct_size;
  char api[NXAUDIO_NAME_MAX];
  char backend[NXAUDIO_NAME_MAX];
  /* Requested vs obtained negotiation. A divergence is recorded, never
   * silently substituted (see nxaudio_classify_granted_format). */
  uint32_t requested_rate;
  uint16_t requested_channels;
  nxaudio_sample_format requested_sample_format;
  uint32_t obtained_rate;
  uint16_t obtained_channels;
  nxaudio_sample_format obtained_sample_format;
  /* Callback liveness window: how many device callbacks the window budget
   * expected vs how many actually arrived. The formatted line publishes the
   * OBSERVED count; `callbacks_expected` stays in the struct for the
   * liveness verdict. */
  uint32_t callbacks_expected;
  uint32_t callbacks_observed;
  uint64_t bytes_delivered;
  /* Peak absolute sample over the window, normalized (1.0 = full scale). */
  double peak_abs;
  uint32_t underrun_count;
} nxaudio_receipt;

/* Zeroes the receipt and stamps api_version/struct_size. */
void nxaudio_receipt_init(nxaudio_receipt *receipt);

/* Formats the single-line, machine-parsable receipt:
 *   AUDIO-RECEIPT: api=%s backend=%s req=%dHz/%dch/%s got=%dHz/%dch/%s
 *     callbacks=%u bytes=%llu peak=%.3f underruns=%u
 * (one line, no wrap; callbacks= publishes callbacks_observed; the sample
 * format prints as "s16le"/"f32le"). Rejects an unversioned receipt, empty
 * or unbounded api/backend, names containing whitespace or '=', an invalid
 * sample format and a negative or NaN peak, so the line always parses. */
nxaudio_result nxaudio_receipt_format(const nxaudio_receipt *receipt,
                                      char *line, size_t line_size);

/* DOCUMENTED capability probe order, in priority order:
 *   1. "inherited-environment"  — an explicitly inherited backend selection
 *      (environment/config) is honored first and never overridden;
 *   2. "measured-open-success"  — otherwise candidates are ranked by a real,
 *      measured device open on this machine (open + obtained spec + live
 *      callbacks), never by any device or brand name;
 *   3. "declared-fallback"      — last, the adapter's statically declared
 *      fallback, still subject to the fail-closed retry policy.
 * No stage matches a device/brand token; the receipt runner greps the
 * nxaudio sources to keep it that way. Returns the number of stages and, if
 * out_stages is non-NULL, a pointer to the static stage-name array. */
size_t nxaudio_backend_probe_order(const char *const **out_stages);

/* Callback liveness. The adapter calls nxaudio_liveness_tick() from its
 * device callback with a monotonic timestamp; nxaudio_liveness_dead() is a
 * pure check the worker may run at any time. No thread is created here. */
typedef struct nxaudio_liveness {
  uint32_t api_version;
  size_t struct_size;
  uint64_t last_tick_ns;
  uint32_t tick_count;
  uint32_t underrun_count;
} nxaudio_liveness;

/* Arms the window: `now_ns` becomes the baseline so a callback that never
 * fires is reported dead once the budget elapses. */
void nxaudio_liveness_init(nxaudio_liveness *liveness, uint64_t now_ns);
nxaudio_result nxaudio_liveness_tick(nxaudio_liveness *liveness,
                                     uint64_t now_ns);
/* Pure: 1 when more than `budget_ns` elapsed since the last tick (or since
 * init when no tick ever arrived), 0 when alive, -1 on invalid input. */
int nxaudio_liveness_dead(const nxaudio_liveness *liveness, uint64_t now_ns,
                          uint64_t budget_ns);
/* Underrun counter API: count one underrun; read the count. */
nxaudio_result nxaudio_liveness_underrun(nxaudio_liveness *liveness);
uint32_t nxaudio_liveness_underruns(const nxaudio_liveness *liveness);

/* Safe-exit contract. Rule: a shutdown NEVER abandons the backend
 * mid-flight — the caller keeps pumping/polling the backend's own
 * confirmation until it confirms or the deadline passes, and only then
 * proceeds with its own exit path (the port's final `_exit(0)` remains the
 * PORT's decision; this helper never calls _exit, exit or abort and never
 * sleeps or spawns a thread — time only advances by `poll_interval_ns`
 * between polls, driven by the caller's clock model). */
typedef enum nxaudio_safe_exit_status {
  NXAUDIO_SAFE_EXIT_PENDING = 0,
  NXAUDIO_SAFE_EXIT_CONFIRMED = 1,
  NXAUDIO_SAFE_EXIT_TIMEOUT = 2
} nxaudio_safe_exit_status;

/* poll_fn returns nonzero once the backend confirmed shutdown. */
typedef int (*nxaudio_safe_exit_poll_fn)(void *user);

typedef struct nxaudio_safe_exit {
  uint32_t api_version;
  size_t struct_size;
  uint64_t now_ns;           /* caller's current monotonic time */
  uint64_t poll_interval_ns; /* > 0; simulated cost of one poll round */
  uint32_t polls;            /* out: polls performed */
  nxaudio_safe_exit_status status; /* out */
} nxaudio_safe_exit;

/* Pumps poll_fn until it confirms (NXAUDIO_OK, status CONFIRMED) or
 * `deadline_ns` passes (NXAUDIO_UNSUPPORTED, status TIMEOUT). At least one
 * poll is attempted when now_ns <= deadline_ns. NXAUDIO_INVALID on bad
 * arguments; the struct is untouched in that case except status. */
nxaudio_result nxaudio_safe_exit_pump(nxaudio_safe_exit *state,
                                      uint64_t deadline_ns,
                                      nxaudio_safe_exit_poll_fn poll_fn,
                                      void *user);

/* Backend recovery is adapter-owned but no longer merely documentary. The
 * adapter classifies a measured backend fault, then supplies the real
 * recover/prepare and reopen operations for its API (SDL, ALSA, OpenSL,
 * FMOD bridge, ...). This helper executes at most ONE recovery cycle and
 * records exactly what happened. It never guesses a device/provider, sleeps,
 * creates a thread or retries forever.
 *
 * A step callback follows the common C convention: return 0 on success and a
 * nonzero backend error on failure. `recover_step` may implement the native
 * prepare/recover operation. When it is absent or fails, `reopen_step` may
 * close/quiesce and reopen the same declared backend. Both callbacks run on
 * the adapter's non-realtime worker, never from an audio callback. */
typedef enum nxaudio_recovery_fault {
  NXAUDIO_RECOVERY_FAULT_NONE = 0,
  NXAUDIO_RECOVERY_FAULT_XRUN_EPIPE = 1,
  NXAUDIO_RECOVERY_FAULT_DEVICE_LOST = 2,
  NXAUDIO_RECOVERY_FAULT_CALLBACK_STALLED = 3
} nxaudio_recovery_fault;

typedef enum nxaudio_recovery_step_status {
  NXAUDIO_RECOVERY_STEP_NOT_ATTEMPTED = 0,
  NXAUDIO_RECOVERY_STEP_OK = 1,
  NXAUDIO_RECOVERY_STEP_FAILED = 2
} nxaudio_recovery_step_status;

typedef enum nxaudio_recovery_outcome {
  NXAUDIO_RECOVERY_PENDING = 0,
  NXAUDIO_RECOVERY_RECOVERED = 1,
  NXAUDIO_RECOVERY_REOPENED = 2,
  NXAUDIO_RECOVERY_FAILED = 3,
  NXAUDIO_RECOVERY_EXHAUSTED = 4
} nxaudio_recovery_outcome;

typedef int (*nxaudio_recovery_step_fn)(void *user);

typedef struct nxaudio_backend_recovery {
  uint32_t api_version;
  size_t struct_size;
  uint32_t attempts;
  nxaudio_recovery_fault fault;
  nxaudio_recovery_step_status recover_status;
  nxaudio_recovery_step_status reopen_status;
  nxaudio_recovery_outcome outcome;
} nxaudio_backend_recovery;

void nxaudio_backend_recovery_init(nxaudio_backend_recovery *state);

/* Executes one bounded cycle. A successful recover ends the cycle. Otherwise
 * the optional reopen is attempted. Any later call on the same state is
 * rejected as EXHAUSTED without invoking either callback. */
nxaudio_result nxaudio_backend_recovery_run(
    nxaudio_backend_recovery *state, nxaudio_recovery_fault fault,
    nxaudio_recovery_step_fn recover_step,
    nxaudio_recovery_step_fn reopen_step, void *user);

/* Maps a backend errno value (positive or negative) to the narrow generic
 * XRUN/EPIPE class. Unknown errors stay NONE and must not trigger recovery. */
nxaudio_recovery_fault nxaudio_recovery_fault_from_errno(int backend_error);

/* Exact machine receipt, for example:
 * AUDIO-RECOVERY: fault=xrun-epipe attempt=1 recover=failed reopen=ok
 * result=reopened */
nxaudio_result nxaudio_backend_recovery_format(
    const nxaudio_backend_recovery *state, char *line, size_t line_size);

#ifdef __cplusplus
}
#endif

#endif
