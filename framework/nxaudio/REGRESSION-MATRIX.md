# nxaudio 0.4.0 regression matrix

| Gate | Expected result | Evidence class |
|---|---|---|
| M14 core stream | SPSC queue, state machine, underrun/silence accounting and caller-quiesced close behave exactly as 0.2.0 | Hermetic host (`tests/run-host.sh`) |
| M14 backend classification | dummy/disk rejection, server-unavailable vs device-open-failed vs mixer-starved reasons unchanged | Hermetic host |
| M14 fail-closed retry | One retry only after a real failure, explicit backend/environment preserved, dummy/disk and same-backend fallbacks refused | Hermetic host |
| M14 adapter allowlist + audibility | Contract table (AAudio fail-closed) and physical-evidence gate unchanged | Hermetic host + process-free audit |
| Receipt canonical line | `AUDIO-RECEIPT: api=sdl2 backend=alsa req=44100Hz/2ch/s16le got=44100Hz/2ch/s16le callbacks=512 bytes=12345678 peak=0.708 underruns=3` byte-exact; single line; no device brand | Hermetic host (`tests/run-receipt-host.sh`) |
| Receipt conversion recorded | requested 44100/1ch/s16le vs obtained 48000/2ch/f32le appears verbatim in the line and maps to `rate-changed` in the 0.2.x classifier; never silently substituted | Hermetic host |
| Receipt fail-closed inputs | NULL/short buffer, wrong api_version, empty or whitespace/`=` names, zero rates/channels, invalid sample format and negative/NaN peak are rejected with an empty line | Hermetic host |
| Probe order stability | Exactly `inherited-environment` → `measured-open-success` → `declared-fallback`, same static array every call | Hermetic host |
| No brand-name selection | Source grep over `src/` + `include/` for device/brand tokens finds nothing; selection stays capability-measured | Hermetic host (static grep) |
| Callback liveness | Alive inside budget, dead past it (including a callback that never fired), monotonic-time and zero-budget inputs rejected; underrun counter API counts and reads | Hermetic host |
| Safe-exit confirmed path | Fake backend confirming on the Nth poll returns `NXAUDIO_OK`/CONFIRMED with exact poll count and simulated time | Hermetic host (fake poll_fn) |
| Safe-exit timeout path | A backend that never confirms is pumped until the deadline, then `NXAUDIO_UNSUPPORTED`/TIMEOUT; helper never calls `_exit`/`abort` and creates no thread (source-grep enforced) | Hermetic host (fake poll_fn) |
| Native recovery succeeds | One real adapter callback runs, reopen is skipped and the exact receipt reports `recover=ok ... result=recovered` | Hermetic host (fake backend) |
| Recover fails, reopen succeeds | Recover and reopen each run once; exact receipt reports `recover=failed reopen=ok result=reopened` | Hermetic host (fake backend) |
| Recovery fails/exhausts | A failed cycle reports failure; a second request invokes no callback and reports `result=exhausted` with attempt count still one | Hermetic host (fake backend) |
| XRUN classification | Positive/negative `EPIPE` (and `ESTRPIPE` where defined) map narrowly to `xrun-epipe`; unknown values map to NONE and cannot trigger recovery | Hermetic host |
| Recovery fail-closed inputs | Invalid state/fault, no real operation, pending/truncated receipt are rejected; no provider is opened or guessed | Hermetic host |
| V4 identity token | Every worker/callback event must match API version, struct size, run ID, generation and API/backend tags returned by open; stale and forged tokens fail closed and set the sticky identity guard | Hermetic host (`tests/test_runtime.c`) |
| V4 generation reopen | Reopen requires a CLOSED or FAILED, fully receipted predecessor plus its exact identity; it preserves run ID, increments generation without wrap, resets every counter and rejects reuse of the previous token | Hermetic host (`tests/test_runtime.c`) |
| V4 valid state graph | `NEW -> OPENED -> LIVE <-> PAUSED -> DRAINING -> CLOSED` and `LIVE -> DEGRADED -> RECOVERING -> LIVE` execute with monotonic worker time | Hermetic host (`tests/test_runtime.c`) |
| V4 expected declared by worker | `nxaudio_runtime_expect_callbacks()` accepts one nonzero threshold only in OPENED before LIVE; callback APIs update observed only and cannot declare or revise expected | Hermetic host (`tests/test_runtime.c`) |
| V4 invalid order/terminal stability | Skipped, duplicate, raced and post-terminal transitions fail closed; sticky order/time guards prevent a later green receipt | Hermetic host (`tests/test_runtime.c`) |
| V4 realtime atomics | Callback updates use always-lock-free 32-bit atomics only, with no allocation, lock, formatting, I/O, environment, provider or guest call | Compile-time assertion + static/dynamic host gate |
| V4 single producer | The first nonzero callback producer owns the generation; producer zero or a second producer is rejected and makes the violation terminal | Hermetic host (`tests/test_runtime.c`) |
| V4 callback/worker concurrency | `callback_active` admits one callback-facing operation, DRAINING blocks new entries and close waits for quiescence; `callback_epoch` makes a snapshot retry instead of returning mixed callback fields | Hermetic host (`tests/test_runtime.c`) |
| V4 counters and peak | callbacks/frames/bytes/nonzero/underrun/silence/device-loss saturate with sticky overflow; NaN, out-of-range or PCM-inconsistent peak is rejected; peak is stored as 0..1000 | Hermetic host (`tests/test_runtime.c`) |
| V4 liveness vs pause/recovery | A callback that never starts or misses its expected window fails; a true LIVE deadline overrun stalls, while PAUSED is not stalled. Resume and same-generation RECOVERED reset the baseline; time regression is rejected | Hermetic host (`tests/test_runtime.c`) |
| V4 recovery record coherence | Exactly one record is accepted. `recovered` requires recover=OK/reopen=not-attempted; `reopened` requires recover!=OK/reopen=OK; `failed` requires no OK plus a failed step; `exhausted` requires neither step attempted | Hermetic host with fake backend record |
| V4 degradation race | Worker degradation and callback device-loss compete through one atomic fault claim; exactly one cause wins and the loser cannot clear it | Hermetic pthread race (`tests/test_runtime.c`) |
| V4 recovered same generation | Same-identity, same-fault, one-attempt RECOVERED may return RECOVERING to LIVE after resetting the stall baseline | Hermetic host with fake backend record |
| V4 reopened new generation | REOPENED terminalizes the old generation as FAILED/`reopen-required`; only after its receipt may `nxaudio_runtime_reopen()` create generation+1. It never resumes the old generation | Hermetic host with fake backend record |
| V4 recovery failure | `failed` or `exhausted` recovery becomes terminal FAILED and can never yield a PASS receipt; pending, duplicate, malformed or mismatched records are rejected | Hermetic host with fake backend record |
| V4 shutdown/quiescence | Only LIVE/PAUSED may drain; DRAINING blocks new callbacks, active callback makes drain/close retry, confirmed shutdown closes, pending is rejected and timeout becomes FAILED/`shutdown-timeout` | Hermetic host (`tests/test_runtime.c`) |
| V4 format mismatch | Requested/obtained divergence remains recorded by the legacy classifier but terminalizes as FAIL/`format-mismatch`; no conversion is silently treated as coherent | Hermetic host (`tests/test_runtime.c`) |
| V4 FIXTURE receipt | Exact line starts `AUDIO-RUNTIME-RECEIPT: schema=nxaudio-runtime-v2 class=FIXTURE physical=0 human=0`; it correlates identity, formats, liveness/PCM, `fault=`, recovery and shutdown | Hermetic host (`tests/test_runtime.c`) |
| V4 terminal decision | PASS requires CLOSED, exact format match, confirmed shutdown, completed non-reopen recovery where applicable, no sticky violation, observed >= separately declared expected callbacks and nonzero frames/bytes/samples/peak; every other case has a stable FAIL reason | Hermetic host (`tests/test_runtime.c`) |
| V4 formatter one-shot | Non-terminal formatting is rejected; a short buffer emits nothing and is retryable; after one complete terminal line every duplicate is rejected | Hermetic host (`tests/test_runtime.c`) |
| V4 hostile inputs | NULL/version/size errors, zero IDs, unbounded or unsafe names, invalid formats, stale identity, zero producer, generation overflow and invalid reason values fail closed | Hermetic host (`tests/test_runtime.c`) |
| No real provider import | `nm -u` on all core/runtime fixtures shows no SDL/ALSA/Pulse/PipeWire/OpenAL/FMOD/Wwise symbol | Hermetic host |
| Toolchains and sanitizers | Core/runtime tests compile with strict warnings under GCC and Clang and run under ASAN/UBSAN/LSAN; TSAN is claimed only when an actual local attempt and result are recorded | Hermetic host (`tests/run-host.sh`) |
| Legacy API/ABI | 0.3.1 public layouts, entry points, enum values, receipt behavior and provider policy remain byte/behavior compatible; V4 is a separately versioned additive API. The M14 reference is owner-resealed to 0.4.0 while explicitly preserving 0.3.1 as parent and adding no physical claim | Static comparison + all 0.3.1 host gates |
| Physical boundary | Every runtime receipt is FIXTURE with `physical=0 human=0`; no host result claims audibility, universal support or a physical device. Concrete adapter/artifact evidence remains `PHYSICAL=PENDING` | Receipt exact-match + process-free audit |

All rows are host-only: no audio device is opened, no guest code runs and no
network is used. `coherent-fixture` means only that synthetic facts for one
generation are internally consistent. Physical audibility evidence remains
governed by the M14 imported-approved-physical gate; nothing in 0.4.0 claims
new device support until a concrete adapter executes the callbacks on the
exact artifact and supplies separately approved physical evidence.
