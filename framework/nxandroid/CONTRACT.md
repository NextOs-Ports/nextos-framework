# nxandroid 0.5.0 contract

This contract is additive and opt-in. It does not alter the 0.1.0 lifecycle or
import structs, whose ABI remains `NXANDROID_API_VERSION=1`. The adapter-support
surface is `NXANDROID_JNI_ADAPTER_API_VERSION=1`.

The additive Android input surface is
`NXANDROID_ANDROID_INPUT_API_VERSION=1`. It never changes the lifecycle/import
ABI and no existing adapter opts in automatically.

The additive Unity surface is `NXANDROID_UNITY_INPUT_API_VERSION=1`. It
consumes, but does not replace or weaken, the C7 Android input authority.

## C7 Android input ownership

The adapter owns `JavaVM`, `JNIEnv`, local/global references, Java class and
method resolution, thread attachment, exception policy, Android objects, the
physical input poller and the actual consumer invocation. `nxandroid` owns only
the copied authority/routes, four bounded pad slots, exclusive delivery state,
touch/cursor arithmetic and a redacted receipt counter.

The adapter supplies a GPTK V2 snapshot. For each present context and each of
the 18 controls, exactly one decision applies: `ACTION`, `SUPPRESS` or `NATIVE`.
An action name resolves to one route. A native control resolves to one native
route. Duplicate action/native routes, undeclared routes, incompatible signal
shapes, V1 maps and absent-context decisions fail before initialization.
`SUPPRESS` never falls through. The exit chord is not a route in this API and
must be evaluated by the sovereign same-pad nxinput chord before game delivery.

Input timestamps are non-zero and monotonic within a context. Each connection
gets a new non-zero generation even when instance or GUID is reused. Callback
events include source, active context, instance/device, generation, global
sequence, timestamp, control, sink, signal, codes, analog values and touch
coordinates. A callback must return zero and acknowledge that exact sequence
with a valid handled bit and consumer return value. Otherwise the context
fail-stops and advances no delivered receipt.

JNI pull is state, not a hidden push callback. A route explicitly selects it;
physical updates remain queued and `nxandroid_android_pull()` records a read
only when the consumer asks. `SUPPRESS` and every non-pull route return
`ENOTFOUND`. Receipts exclude GUID/device/instance identifiers.

Trigger values are clamped to [0,1] while their digital edge uses hysteresis.
Stick vectors are clamped to [-1,1]. Disconnect, cancel, context change, focus
loss and pause emit all required releases/centers before lifecycle completion,
then clear ownership. Callback failure during release fail-stops instead of
pretending cleanup succeeded. No state is copied between pads, including pads
with identical GUIDs.

Touch coordinates are accepted only for declared touch actions and are mapped
through the proven content/safe rectangle and rotation. A cursor action accepts
only a stick vector, applies radial deadzone, response curve, frame delta and
smoothing, and emits touch moves only while that pad owns a declared cursor
contact. Rendering the polished arrow and choosing menu/gameplay/cursor
contexts remain adapter-owned source contracts.

## C8 Unity consumer ownership

Unity consumer profiles are independent contracts. Legacy Input, New Input
System, Rewired, InControl and raw Android input may not borrow one another's
version, class, signature, device identity or thread assumptions. The adapter
owns runtime discovery and the real calls. It must pin the Unity build, plugin
and consumer versions, ABI, assembly, metadata/runtime/artifact hashes, source
commit/tree, license and full method signatures. Unproved classifications can
be validated for ledger purposes but cannot initialize a runtime context.

The native startup sequence is mandatory: initializers, `JNI_OnLoad` for main,
runtime and Unity, player initialization, surface create/change, resume, focus
and frame loop. Calls must occur on the declared player or Android-input thread;
the context binds non-zero thread tokens and fail-stops on drift. Pause,
surface recreation, reconnect and shutdown never skip or synthesize a phase.
The API deliberately contains no RVA, offset, patch or signal-recovery path.

C8 accepts only events emitted by the C7 exclusive dispatcher. It does not
read a physical device, mapping, GPTK file, keyboard, touch fallback or raw
route. A registration or producer callback advances only after the declared
API returns a matching sequence acknowledgement. It is still not a consumer
receipt. Each input transition remains pending until the exact low-level read
API and higher-level action API both return matching pad, generation, control,
shape and state. A new transition, hot-unplug or forged/duplicate return while
one is incomplete fail-stops instead of claiming delivery.

Unreachable controls carry neither route nor consumer identity. A physical
identity is preserved unless a profile carries an exact A/B evidence hash for
an override. `null` never reaches C8 because C7 suppresses it; `native` reaches
only the one route frozen by C7. The exit chord remains in accepted nxinput,
out of the Unity semantic action stream, and recognizes only same-pad
SELECT+START.

Host callbacks and identities are fixtures. They prove state ownership,
lifecycle, thread, multipad, hotplug and receipt invariants but never prove a
proprietary plugin or device. A real profile remains `PENDING` or `UNPROVEN`
until an opt-in artifact supplies the full tuple and physical proof.

## Ownership boundary

`nxandroid` owns no `JavaVM`, `JNIEnv`, Java reference, vtable, class, method,
exception, Activity, persistent store or lock. It does not dispatch a Java
signature. An adapter may use the primitives below while retaining all of those
objects and decisions.

The primitives were introduced in 0.2.0. Version 0.2.1 changes only the
host-side availability handling of the owner-guest inventory; this C ABI and
its behavior are unchanged. No existing port adopts either version without an
explicit port change and that port's own release and device evidence.

## UTF-16 region primitive

`nxandroid_utf16_copy_region()` requires a canonical UTF-16 view whose length is
in 16-bit code units. A non-empty view has non-NULL storage. `start` and
`length` are signed 32-bit `jsize`-shaped values.

- negative values, start beyond the view, or length beyond the remaining view
  return `NXANDROID_JNI_ADAPTER_EBOUNDS`;
- malformed storage or a NULL output for a non-empty copy returns
  `NXANDROID_JNI_ADAPTER_EINVAL`;
- start equal to the view length with zero length succeeds, and the output may
  be NULL;
- success copies exactly `length` code units, supports overlap and appends no
  terminator;
- bounds are checked as `length <= unit_count - start`, never by overflowing an
  addition;
- code units are copied verbatim. Surrogate validation, Unicode normalization,
  UTF-8/MUTF-8 conversion and Java exception creation are not performed.

The adapter maps `EBOUNDS` to its pending
`StringIndexOutOfBoundsException`. JNI slot 220 names UTF-16
`GetStringRegion`; slot 221 names the separate MUTF-8 `GetStringUTFRegion`.

## Typed preferences snapshot

`nxandroid_prefs_snapshot_create()` borrows its source only during the call and
publishes output only after a complete deep copy. `entries=NULL,count=0` creates
a valid empty snapshot. At most `NXANDROID_PREFS_MAX_ENTRIES` entries and
`NXANDROID_PREFS_MAX_STRING_SET_MEMBERS` members per set are accepted.

Every key and StringSet member is unique under exact UTF-16 code-unit equality.
Empty keys, strings, sets and set members are valid. The supported value types
are exactly:

- UTF-16 String;
- set of UTF-16 Strings;
- `int32_t`;
- `int64_t`;
- 32-bit `float`, copied bit-for-bit;
- boolean represented only by integer 0 or 1.

The snapshot owns all copied keys, strings, set-view arrays and set-member
storage. Accessors return borrowed immutable views valid until destroy. Each
creation has an independent lifetime; source mutation and destruction of a
different snapshot cannot change it. A real destroy NULLs the caller pointer.
NULL accessors report zero/no entry.

The adapter holds its store lock throughout creation and serializes destroy
against readers. Snapshot enumeration order is an implementation detail. The
adapter implements Java Map/Set/Iterator/Entry objects, boxing, signatures,
exception behavior and any persistence or migration format.

## Evidence boundary

Host tests prove only the C ownership, type, bounds and transactional contracts.
They do not execute a guest, install a JNI vtable or prove a physical
`SharedPreferences.getAll()` migration. `references/jni-adapter-primitives-v1.json`
records the approved positive sources and their limits. In particular, Bomb
Chicken's released resume workaround stays game-specific and is not part of
this component.
