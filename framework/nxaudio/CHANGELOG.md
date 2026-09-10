# 0.4.0 (2026-08-29)

- Added the provider-neutral `nxaudio_runtime.h` API version 2. A compact token
  binds every callback and worker event to one `run_id`, generation and the
  sanitized API/backend tags; stale or cross-generation facts fail closed.
- Added the explicit state graph `NEW -> OPENED -> LIVE <-> PAUSED`, with
  drain/close and degraded/recovering/failure paths. Invalid ordering,
  transition races and caller-supplied monotonic-time regressions become sticky
  terminal failures.
- Added one-shot `nxaudio_runtime_expect_callbacks()` in `OPENED`. The worker
  fixes the nonzero acceptance threshold before `LIVE`; callbacks can update
  only the observed count and cannot move their own goalpost.
- Added generation-safe reopen. It requires the previous generation to be
  CLOSED or FAILED, its exact identity and one fully emitted terminal receipt;
  it increments without wrap and starts all measurements empty.
- Added bounded lock-free 32-bit atomic accounting: the worker declares
  expected callbacks once, while callback-facing operations update only
  observed callbacks, frames, bytes, nonzero samples, peak, underruns, silence
  and device loss. Counters saturate instead of wrapping; a stable nonzero
  producer ID prevents two callback producers from mixing facts.
  `callback_active` enforces one callback-facing operation at a time, close
  waits for quiescence and `callback_epoch` prevents mixed snapshots.
- Integrated the existing 0.3.1 single-cycle backend recovery record with the
  exact runtime identity/fault. Worker and device-loss callback compete through
  one atomic fault claim, so the loser cannot clear the winner. The record is
  one-shot and validates the exact recover/reopen step-status combination for
  every outcome. `RECOVERED` may
  resume the same generation; `REOPENED` terminalizes the old generation as
  `reopen-required`, requires its receipt and starts work only through
  `nxaudio_runtime_reopen()` in generation+1. Failed/exhausted recovery and
  shutdown timeout are terminal failures.
- Added worker snapshots, pause-aware stall detection and the exact one-shot
  `nxaudio-runtime-v2` receipt, now including `fault=`. Resume and recovered
  transitions reset the stall baseline. Format mismatch is an explicit
  terminal `FAIL reason=format-mismatch`. Every line is `class=FIXTURE
  physical=0 human=0`; a short buffer emits nothing and can be retried. Host
  PCM facts can prove only `coherent-fixture`, never physical audibility or
  device support.
- Expanded directed host coverage for transitions, identity/generation
  isolation, single-producer concurrency, PCM/liveness, recovery, shutdown,
  time/order/overflow guards, hostile inputs, snapshots and exact receipt
  semantics. GCC/Clang ASAN/UBSAN coverage and zero provider imports remain
  required; TSAN is reported only when actually available and run.
- Preserved the complete 0.3.1 API/ABI and behavior: legacy public layouts,
  numeric values, receipt, SPSC queue and backend policy remain unchanged. The
  M14 contract is owner-resealed for component 0.4.0 with 0.3.1 recorded as its
  integrated parent, runtime API 2 additive, evidence class FIXTURE and no new
  physical claim. Physical validation and concrete adapter wiring remain
  pending.

# 0.3.1 (2026-08-27)

- Added executable, adapter-owned recovery through
  `nxaudio_backend_recovery_run()`: one measured cycle only, native
  recover/prepare first and a same-backend reopen only when needed.
- Added narrow `EPIPE`/`ESTRPIPE` classification plus explicit
  `xrun-epipe`, `device-lost` and `callback-stalled` faults. Unknown errors
  fail closed instead of triggering a speculative restart.
- Added exact `AUDIO-RECOVERY` receipts recording attempt, both steps and the
  terminal recovered/reopened/failed/exhausted result.
- Added hermetic GCC/Clang sanitizer coverage proving successful recover,
  recover-to-reopen fallback, total failure, one-attempt exhaustion, callback
  non-reentry, errno classification and malformed/truncated receipt rejection.
- No provider import and no API 1 numeric/layout change. The helper owns no
  clock/thread/device; concrete SDL/ALSA/OpenSL/FMOD work remains in the port
  adapter and must be physically receipted before a public claim.

# 0.3.0 (2026-08-26)

V3-AUDIO-01 — additive only; every 0.2.0 layout, entry point and numeric
value is unchanged and the M14 host gate stays green.

- New `include/nxaudio_receipt.h` + `src/nxaudio_receipt.c` (strict C99, no
  thread, no clock, no environment read, no device open, no process exit).
- `nxaudio_receipt`: versioned receipt of one measured open — api/bridge
  ("sdl2", "openal", "opensl", "aaudio", "fmod-bridge"), backend/device
  string, requested vs obtained {rate, channels, sample format}, callback
  window expected/observed, bytes delivered, peak absolute sample, underrun
  count. `nxaudio_receipt_format()` emits the single machine-parsable line
  `AUDIO-RECEIPT: api=%s backend=%s req=%dHz/%dch/%s got=%dHz/%dch/%s
  callbacks=%u bytes=%llu peak=%.3f underruns=%u`; a granted format that
  diverges from the request is recorded, never substituted.
- `nxaudio_backend_probe_order()`: the documented capability order —
  inherited environment → measured open success → declared fallback. No
  selection by device/brand name anywhere in nxaudio; the new runner greps
  the sources for brand tokens and fails on any hit.
- Callback liveness: `nxaudio_liveness_init/tick`, pure
  `nxaudio_liveness_dead(now_ns, budget_ns)` (a callback that never fires is
  dead once the budget elapses) and the
  `nxaudio_liveness_underrun/underruns` counter API.
- Safe-exit contract: `nxaudio_safe_exit_pump(state, deadline_ns, poll_fn,
  user)` keeps polling the backend's own shutdown confirmation until it
  confirms (`NXAUDIO_OK`/CONFIRMED) or the deadline passes
  (`NXAUDIO_UNSUPPORTED`/TIMEOUT); it never calls `_exit` — the port keeps
  its own final `_exit(0)`.
- New hermetic gate `tests/test_receipt.c` + `tests/run-receipt-host.sh`
  (GCC+Clang, C99 for the new module, ASan/UBSan, provider-import nm check,
  brand-token source grep, exact-line receipt match, both safe-exit paths).
- CMake: `nxaudio_receipt.c` joins the static library, the header is
  installed and `nxaudio-receipt` joins ctest, so
  `tests/run-host.sh` also builds and runs the new module under sanitizers.
- `NXAUDIO_VERSION` in `nxaudio.h` is now "0.3.0", aligned with the CMake
  project version and this changelog (auditoria V3, ponto 5: um componente
  fala uma versão só). The M14 audit reference `component_version` is resealed
  to "0.3.0" to match, and the new receipt module carries its own
  `NXAUDIO_RECEIPT_VERSION "0.3.0"`. The equality gate
  `framework/tests/test_component_versions.py` now guards this invariant.
