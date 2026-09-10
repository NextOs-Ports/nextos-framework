# Changelog

## 0.5.0 — 2026-08-30 (V4-CONTROLLERS-03 / C8)

- Reconcile the previously integrated 0.3.0 input-sink/M11 baseline before C9,
  retain the C7/C8 sources byte-for-byte, and align the public header plus
  hash-pinned M11 contract with the component's actual 0.5.0 version.
- Add a separate, opt-in Unity input contract for Legacy Input, New Input
  System, Rewired, InControl and raw Android `InputDevice`; a profile must pin
  the exact Unity/plugin/build identities and full API signatures.
- Require the native Unity lifecycle in order before pad registration, bind
  registration/producer/consumer calls to declared runtime threads, and reject
  copied RVAs, wildcard identities and unproved profiles before deployment.
- Consume only C7's frozen event stream. Producer acknowledgement remains
  separate from low-level and action consumer returns, so enqueue/injection,
  callback count or a mock cannot become a consumer receipt.
- Cover 90 fixture controls, same-GUID pads, reconnect generations,
  focus/pause/resume, analog triggers/sticks, `null`, `native`, callback failure
  and the sovereign same-pad exit chord without keyboard events.
- Classify all 14 Unity candidates from the user-selected last-30 R2 cut once,
  recomputing 18 source blobs. No candidate closes every exact-version/API/
  provenance/new-byte boundary, so real Unity profiles and physical evidence
  remain `PENDING`/`UNPROVEN`; no port changed.
- Keep host build scratch under the canonical persistent temporary root while
  preserving the same isolated cleanup and two-worker limit.

## 0.4.0 — 2026-08-30 (V4-CONTROLLERS-03 / C7)

- Add the opt-in strict Android input API and GPTK V2 bridge. `ACTION` selects
  exactly one sink, `null` suppresses every sink and `native` preserves one
  declared per-control path; the historical v1 fan-out helper remains intact.
- Carry instance/device, hotplug generation, source, context, sequence,
  timestamp, key/axis identity and acknowledged handled/return results across
  the adapter boundary. Callback failures and malformed acknowledgments
  fail-stop.
- Separate JNI push from measured JNI pull reads. Add KeyEvent down/up,
  MotionEvent vectors/triggers, native-gamepad, touch/content/safe/rotation,
  time-based cursor, cancel, focus/pause, hotplug and same-GUID multipad tests.
- Pin a read-only three-consumer allowlist from the requested last-30 R2 cut,
  with 54/54 control declarations and immutable source/tree/artifact receipts.
  New C7 runtime adoption and physical evidence remain pending per port.
- Compile and install the previously published input-sink v1 implementation and
  headers as additive library surface; existing ports remain pinned and are not
  regenerated.

## 0.3.0 — 2026-08-26 (V3-CONTROLLERS-01, lado Android)

- Interface versionada NOVA e aditiva de sinks de input
  (`nxandroid_input_sinks.h`, NXANDROID_INPUT_SINK_API_VERSION 1): o adapter
  registra um ou mais destinos por ação semântica (AKeyEvent/AMotionEvent,
  equivalentes Java, callback JNI do adapter, API interna da engine, touch);
  entrega em ordem de registro, exatamente uma vez por sink por ação lógica
  (regressão Action Squad). nxandroid continua sem possuir JavaVM/JNIEnv.
- `nxandroid_touch_resolve`: coordenadas normalizadas → pixel no drawable com
  rotação 0/90/180/270 e safe area, fail-closed; nenhum pixel fixo vem de
  arquivo do usuário.
- Flag de exclusividade para o caminho de gamepad virtual SDL (nunca dupla
  leitura do device físico).
- Gate `tests/run-input-sinks-host.sh` (gcc+clang, ASan/UBSan) verde.

## 0.2.1 — 2026-08-14

- Make the hash-pinned M11 owner-guest inventory usable from a clean public
  checkout: an entirely absent proprietary set is recorded as an explicit
  zero-execution skip and returns success to the surrounding host matrix.
- Keep the evidence boundary fail-closed: any partial set, symlink,
  non-regular path, wrong hash or ABI mismatch remains an error.
- Add `--require-guests` for evidence runs that must reject an absent set;
  `--emit` also continues to require every guest.
- Add process-free classifier regressions for complete, entirely absent and
  partial owner-data sets.

The C ABI, lifecycle, imports and JNI adapter primitives are unchanged. Ports
remain on their pinned version until an explicit migration.

## 0.2.0 — 2026-08-13

- Add the opt-in `nxandroid_jni_adapter.h` support surface without changing the
  lifecycle/import ABI.
- Add a UTF-16 code-unit region copier for adapter-owned JNI slot 220, with
  overflow-safe bounds, no implicit terminator and distinct slot 221 constants.
- Add immutable, independent, fully deep-copied typed preference snapshots for
  adapter-owned `SharedPreferences.getAll()` bridges across String, StringSet,
  int32, int64, float and boolean.
- Add focused GCC/Clang sanitizer tests, install smoke coverage and static
  analyzers for the new source.
- Add the contract, evidence ledger, regression matrix and fresh-process
  PlayerPrefs/SharedPreferences validation rule.

No VM, JNIEnv, Java collection dispatch, locking policy or port-specific
PlayerPrefs workaround became a framework default. Existing ports remain
unchanged and pinned until explicitly migrated.
