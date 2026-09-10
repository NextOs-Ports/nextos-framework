# nxaudio

`nxaudio` 0.4.0 is the small common audio contract for M14. It does not choose a
firmware by name and does not replace an engine mixer. Backend discovery and a
real SDL device-open receipt remain owned by `nxcompat`; engine callbacks,
formats and teardown remain in the exact adapter that proved them.

The common layer provides:

- a lock-free single-producer/single-consumer PCM queue: a worker performs
  guest calls, conversion and heavy mixing before submitting PCM; the realtime
  consumer only copies or zero-fills bytes;
- explicit ready/running/paused/device-lost/closed states, underrun accounting
  and deterministic caller-quiesced close;
- distinct reasons for an absent server, a device-open failure and a mixer that
  opened but stopped producing PCM;
- an isolated-HOME plan that exposes an existing `.asoundrc` through
  `ALSA_CONFIG_PATH`, without inventing or copying one;
- a finite adapter allowlist. OpenSL ES, OpenAL, FMOD, FMOD Ex and Wwise are
  accepted only with a source-proven contract. AAudio has no generic contract
  and therefore remains rejected until an approved guest proves one;
- an audibility gate that rejects synthetic/API-only success. It accepts only
  current physical evidence or source-pinned imported physical evidence with a
  real open, nonzero PCM and human confirmation;
- an additive, fail-closed backend retry policy. An adapter may retry exactly
  once after a real server/open/device-loss failure, and only when its supplied
  fallback is both available and compiled, differs from the current backend
  and is neither `dummy` nor `disk`. An explicit backend or backend environment
  is always preserved.

## Backend order and receipts

`nxcompat_sdl2_negotiate_audio_v2()` tries the inherited/default backend first,
rejects `dummy` and `disk`, then permits one normal SDL autodetect attempt. A
receipt is publishable only after a nonzero device ID, a valid obtained spec,
close and verified cleanup. Frequency, sample format, channels, period and
derived latency are copied into the engine adapter's `nxaudio_format`; nxaudio
never silently substitutes them.

An unavailable Pulse/PipeWire server is not called an underrun. Conversely, a
server and device that opened but whose mixer callbacks produce no frames is
reported as `mixer-starved`. Every fallback keeps the stable reason in the
receipt/log; raw backend errors are diagnostic only.

`nxaudio_plan_backend_retry()` does not discover or select a backend. The
adapter supplies the current and fallback names, the proven failure reason,
compile-time/availability facts and `retry_count`. The plan permits a retry
only when `retry_count == 0`, returning `next_retry_count == 1`; every later
request is rejected. It reads no environment variable, has no firmware-name
table and opens no device. Callers mark both explicit API/config selection and
an inherited backend environment so either remains untouched.

## Adapter boundaries

| Stack | Approved contract | Boundary |
|---|---|---|
| SDL2 | `nxcompat-sdl2-audio-v2` | common real-open probe |
| OpenSL ES | `tasm2-opensl-sdl-v1`, `castle-opensl-sdl-v1` | exact engine queue/callback |
| AAudio | none | fail closed until a guest proves the API/lifecycle |
| OpenAL | `bully2-openal-v1` | use the firmware provider; never bundle an incompatible one |
| FMOD | `horizon-fmod-sdl-v1` | exact Unity/FMOD PCM bridge |
| FMOD Ex | `castle-fmodex-v1` | exact Castle bridge |
| FMOD Ex | `titansouls-fmodex-opensl-sdl-v1` | guest FMOD Ex -> adapter OpenSL ES queue -> real SDL device; external provider forbidden |
| FMOD Ex | `titansouls-fmodex-opensl-sdl-v2` | v1 boundary plus the generic fail-closed, one-retry backend policy; v1 remains valid |
| Wwise | `sor4-wwise-openal-glibc230-v1` | canonical `NextOs-Ports/sor4-nextos/port/wwise-native/build-glibc230.sh` recipe only |

The Titan Souls v1 and v2 contracts are deliberately narrow: the Android FMOD
Ex guest remains the decoder, the port's exact OpenSL ES buffer-queue adapter
feeds an already opened SDL device, and no bundled host FMOD provider may
replace that path. V2 additionally binds SDL recovery to the generic one-retry
policy above and permits the adapter's separate FMOD output recovery only for
the guest-reported FMOD Ex 4.44.17 ABI. The common layer knows no FMOD enums and
does not choose a firmware backend. Registering either contract does not claim
audibility or device support; those still require the normal real-device
receipt and physical evidence gate.

The imported physical cross-check remains deliberately limited to approved
evidence from Horizon Chase, TASM2 1.2.7d, Streets of Rage 4 and Castle of
Illusion. The Titan v1 addition in 0.1.1 and additive v2 contract in 0.2.0 are
source-scoped and host-contract-tested; historical WIP notes never satisfy the
gate.

## Host gate

```sh
bash framework/nxaudio/tests/run-host.sh
python3 -B framework/nxaudio/tests/test_m14_audio_contract.py
```

Both are host-only. They open no SDL/audio device, execute no guest code and do
not use the network. Audible physical evidence is imported from the approved,
source-pinned port records; it is not fabricated by the host test.

## V3 (0.3.0): measured receipt, liveness and safe exit

V3-AUDIO-01 adds `include/nxaudio_receipt.h` + `src/nxaudio_receipt.c`. It is
purely additive: no existing layout, entry point or numeric value changes, and
the module stays strict C99 with no thread, no clock, no environment read and
no device open.

- **Capability-measured selection, documented once.**
  `nxaudio_backend_probe_order()` returns the only sanctioned order:
  `inherited-environment` → `measured-open-success` → `declared-fallback`.
  An inherited/explicit backend is sovereign; otherwise candidates are ranked
  by a real measured open (open + obtained spec + live callbacks) on this
  machine; the declared fallback comes last and stays behind the fail-closed
  one-retry policy. No nxaudio function selects by device or brand name —
  `tests/run-receipt-host.sh` greps the sources for brand tokens and fails on
  any hit.
- **Receipt.** `nxaudio_receipt` records api/bridge ("sdl2", "openal",
  "opensl", "aaudio", "fmod-bridge"), the backend/device string, requested vs
  obtained {rate, channels, sample format}, the callback liveness window
  (expected/observed), bytes delivered, peak absolute sample and underruns.
  `nxaudio_receipt_format()` emits exactly one machine-parsable line:
  `AUDIO-RECEIPT: api=%s backend=%s req=%dHz/%dch/%s got=%dHz/%dch/%s
  callbacks=%u bytes=%llu peak=%.3f underruns=%u` (no device brand, ever). A
  granted format that differs from the request is recorded, never silently
  substituted — the same divergence the 0.2.x
  `nxaudio_classify_granted_format()` names.
- **Liveness + underruns.** `nxaudio_liveness_tick(now_ns)` is called from the
  adapter's device callback; `nxaudio_liveness_dead(now_ns, budget_ns)` is a
  pure check for a callback that stopped (or never started) beyond the budget;
  `nxaudio_liveness_underrun()`/`nxaudio_liveness_underruns()` count
  underruns. The framework creates no thread for any of it.
- **Safe-exit contract.** `nxaudio_safe_exit_pump()` implements the shutdown
  rule: keep pumping/polling the backend's own confirmation until it confirms
  or the deadline passes, and only report `confirmed` or `timeout`. It never
  calls `_exit`/`exit`/`abort` and never sleeps — the port keeps its own
  final `_exit(0)` after saves and unlocks, exactly as before.

Standalone V3 gate (in addition to the M14 gates above, which stay green and
unchanged):

```sh
bash framework/nxaudio/tests/run-receipt-host.sh
```

## V3 (0.3.1): bounded backend recovery

The old state transition `nxaudio_stream_recover()` remains unchanged, but it
is no longer the only recovery surface. It records that the generic PCM queue
may resume; it cannot repair a real ALSA/SDL/OpenSL/FMOD device by itself.

`nxaudio_backend_recovery_run()` closes that gap without importing a provider.
The adapter supplies its real non-realtime operations: first a native
recover/prepare step and, if that is absent or fails, a close/quiesce/reopen
step for the same declared backend. The framework executes at most one cycle.
A second request is rejected as `exhausted` without calling the backend again.

The measured fault is explicit: `xrun-epipe`, `device-lost` or
`callback-stalled`. `nxaudio_recovery_fault_from_errno()` narrowly recognizes
positive or negative `EPIPE`/`ESTRPIPE`; unknown errors never start recovery.
Every cycle emits a provider-neutral line such as:

```text
AUDIO-RECOVERY: fault=xrun-epipe attempt=1 recover=failed reopen=ok result=reopened
```

This is an execution contract, not a promise that every adapter is already
wired. A public-final port claiming recovery must link the module, execute the
real backend callbacks and attach this receipt to the exact runtime artifact.
The helper never selects by firmware/device name, retries indefinitely, opens
a device itself or runs from the realtime callback.

## V4 (0.4.0): generation-bound runtime attestation

V4 adds the separately versioned `nxaudio_runtime.h` API version 2. It composes
facts supplied by an adapter; it does not discover or open a provider, read the
environment, allocate memory, create a thread, call guest code or claim that a
sample reached a speaker. The 0.3.1 public entry points, layouts, enum values,
receipt API and provider policy remain unchanged.

### Identity, generations and state

`nxaudio_runtime_open()` returns a compact identity containing API version,
struct size, `run_id`, `generation` and tags for the sanitized API/backend
names. Every worker and callback event must present that exact identity. A
stale generation or a token from another API/backend sets a sticky identity
violation and fails closed.

The legal state flow is:

```text
NEW -> OPENED -> LIVE <-> PAUSED -> DRAINING -> CLOSED
                    |
                    +-> DEGRADED -> RECOVERING -- RECOVERED --> LIVE
                                             \-- REOPENED/FAILED/EXHAUSTED
                                                                --> FAILED
```

Explicit failure can terminate a non-terminal generation as `FAILED`.
Transitions outside this graph, concurrent transition races and monotonic-time
regressions set sticky violations. `nxaudio_runtime_reopen()` accepts only a
`CLOSED` or `FAILED` generation whose exact identity is supplied and whose one
terminal receipt was fully emitted; it preserves the run ID, increments the
generation without wrap and starts with empty counters. Facts from the old
generation cannot enter the new one.

Before the first transition from `OPENED` to `LIVE`, the worker must call
`nxaudio_runtime_expect_callbacks()` exactly once with a nonzero expected
callback count. The callback path increments only the observed count; it can
neither invent nor revise its own acceptance threshold. Missing declaration,
duplicate declaration or an attempt to declare it after `LIVE` fails closed.

### Realtime boundary

The callback-facing update, underrun, silence and device-lost functions use
bounded lock-free 32-bit `__atomic` operations. A compile-time assertion
requires those operations to be always lock-free. They do not allocate,
format, lock, perform I/O or invoke recovery/guest code. The first nonzero
`producer_id` owns the generation; a second producer fails closed. An atomic
`callback_active` guard allows only one callback-facing operation at a time,
and each completed callback advances `callback_epoch`. Callback and worker
clocks are caller-supplied monotonic milliseconds. Counters saturate at
`UINT32_MAX`, make overflow sticky and never wrap silently;
`peak_abs` is validated and stored as `peak_milli` in the range 0..1000.

`nxaudio_runtime_is_stalled()` reports a real stall only while `LIVE`; a
legitimate `PAUSED` session is never called stalled. Resume and successful
same-generation recovery reset the callback-time baseline before exposing
`LIVE`, so their legitimate downtime cannot become a false stall. Snapshots
are accepted only while no callback is active and when `callback_epoch` is
unchanged across the copy; otherwise the caller retries instead of receiving
a mixed callback snapshot. A valid snapshot carries the bound identity,
negotiated formats, callbacks, frames/bytes/nonzero PCM, peak, underruns,
silence, device loss, timestamps, recovery/shutdown facts and every sticky
violation for worker-side inspection.

### Recovery, shutdown and terminal receipt

A recovery starts only after a measured non-`NONE` fault moves `LIVE` to
`DEGRADED`, then `RECOVERING`. The adapter supplies a versioned 0.3.1
`nxaudio_backend_recovery` for the same identity, fault and generation. The
worker and device-loss callback claim that fault atomically, so a competing
degradation event cannot erase or replace the winner's measured cause. The
record is one-shot and must describe exactly one attempt with coherent step
statuses: `recovered` requires recover OK and reopen not attempted; `reopened`
requires recover not-OK and reopen OK; `failed` requires no successful step
and at least one failed step; `exhausted` requires both steps not attempted.
Pending, duplicate, malformed or fault-mismatched records are rejected.

Only `RECOVERED` may return to `LIVE` in the same generation. `REOPENED` does
not transfer new-backend facts into the old identity: it terminalizes the old
generation as `FAILED reason=reopen-required`. The adapter must emit that
generation's receipt and then call `nxaudio_runtime_reopen()` to create
`generation+1`. `FAILED` and `EXHAUSTED` also terminalize the generation as
recovery failures.

Shutdown is equally fail-closed. Only `LIVE` or `PAUSED` may enter
`DRAINING`, which blocks new callback entries. If an already-entered callback
is still active, drain/close returns a retryable quiescence result; close never
races that callback. After quiescence, `confirmed` closes the generation,
while `timeout` moves it to `FAILED` and returns unsupported. A pending
shutdown never yields success.

`nxaudio_runtime_format_receipt()` runs outside realtime and emits at most one
terminal line per generation. A short buffer emits nothing and remains
retryable; a successful or failed line consumes the generation's formatter.
The schema is always explicit:

```text
AUDIO-RUNTIME-RECEIPT: schema=nxaudio-runtime-v2 class=FIXTURE physical=0 human=0 ... fault=... recovery=... result=PASS|FAIL reason=...
```

`PASS reason=coherent-fixture` requires a closed generation, exact requested
and granted format match, confirmed shutdown, no pending/failed/reopen-required
recovery, no sticky identity/producer/time/order/overflow violation, observed
callbacks meeting the separately declared expected count, and nonzero frames,
bytes, samples and peak. A negotiated-format divergence is always
`FAIL reason=format-mismatch`; it remains visible through `format=...` in the
receipt rather than being silently accepted. The `fault=` field keeps the
measured recovery trigger separate from its terminal outcome. A coherent
fixture proves only that the supplied host facts are internally consistent.
It never means `PHYSICAL`, human-confirmed
audibility, universal backend support or device compatibility; those remain
`PENDING` until an exact adapter/artifact produces separate approved physical
evidence.

### Directed host coverage

`tests/test_runtime.c`, built by `tests/run-host.sh`, covers the V4 boundary in
addition to all legacy 0.3.1 gates:

- the complete valid transition graph plus invalid order and terminal-state
  transitions;
- stale identity, API/backend-tag mismatch, reopen isolation and generation
  overflow;
- separate pre-LIVE expected declaration, stable single producer,
  second-producer rejection, callback-active quiescence, callback epoch and
  callback/worker concurrency;
- callback missing/short, zero PCM, live PCM, underrun, silence, device loss,
  pause-not-stall and true stall;
- recovered in-generation, reopened-to-new-generation, failed, exhausted,
  pending, duplicate and step-status-incoherent recovery;
- confirmed, pending and timeout shutdown;
- monotonic-time regression, NaN/out-of-range peak, hostile names and
  saturating counters;
- requested/granted format mismatch, exact FIXTURE receipt including `fault=`,
  epoch-stable snapshot, one-shot formatting, retry after a short buffer and
  terminal reason names.

The host runner uses strict warnings with GCC and Clang plus ASAN/UBSAN/LSAN,
checks the provider-import boundary with `nm -u` and records TSAN separately
only if a real TSAN attempt is available. All such execution remains
`FIXTURE`: `hardware_ran=0`, `device_access=0`, `network_access=0` and
`PHYSICAL=PENDING`.
