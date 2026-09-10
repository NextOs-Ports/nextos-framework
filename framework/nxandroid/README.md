# nxandroid

`nxandroid` 0.5.0 is a small C99 contract layer for Android-native port
adapters. It validates and executes an adapter-declared lifecycle profile and
resolves imports through an explicit ABI catalog. Version 0.2.0 introduced two
passive, opt-in JNI adapter primitives: UTF-16 region copying and immutable
typed preference snapshots. Version 0.4.0 added the strict C7 Android input
boundary; version 0.5.0 adds the separate opt-in C8 Unity consumer boundary.
It is not a Java runtime, a JNI implementation, a loader, an EGL backend or a
generic engine implementation.

The adapter remains the sole owner of `JavaVM`, `JNIEnv`, Activity objects,
registered native methods, surfaces, graphics, input, save calls and engine
entry points. The core sees only ordered phase records and two generic
callbacks. Consequently there is no universal fake JNI behavior to inherit in
a new port.

## Lifecycle contract

An `nxandroid_profile` contains all modules used by that lifecycle plus the
exact ordered `nxandroid_step` list selected by the adapter. The validator does
not reorder or synthesize a phase. Its common safety constraints are:

- every declared module runs `MODULE_INITIALIZED` once and, when declared,
  `MODULE_JNI` once after its own initializer and before `ACTIVITY_CREATE`;
- `ACTIVITY_CREATE` precedes `GRAPHICS_REQUEST` and guest surface phases;
- `GRAPHICS_REQUEST` precedes both `GL_READY` and `SURFACE_UP` for a surface
  generation. Approved adapters may place host `GL_READY` before the guest
  surface callbacks (Bully-like) or after them (Unity-like);
- resume/pause and focus-gain/loss are independent adapter-ordered epoch pairs.
  Their matching events must use the same non-zero `cycle_id`; they do not
  pretend that Android always delivers them at one fixed surface point;
- `ENTRY` requires an active guest surface by default. A source-proven adapter
  whose native flow creates GL, enters the engine and creates objects before its
  guest Surface callback may opt into
  `NXANDROID_PROFILE_ALLOW_ENTRY_BEFORE_SURFACE_CALLBACK`. That exception still
  requires `GRAPHICS_REQUEST` and `GL_READY` for the same pending generation;
  both `ENTRY` and `OBJECTS_READY` must complete before `SURFACE_UP`, while
  `INPUT_ENABLE` and the blocking `RUN_LOOP` remain forbidden until that Surface
  is active. The opt-in is rejected when a profile does not actually use it or
  attempts to mix it with the default Surface-first relation;
- `OBJECTS_READY` belongs to that one entry and its generation. A driven profile
  then enters one synchronous, blocking `RUN_LOOP`; only after it returns may
  input be disabled and shutdown continue. Recreated surfaces and repeated
  resume/focus cycles do not rerun entry or object creation;
- the closing path contains pause and save in that state order. When a profile
  declared `FOCUS_GAIN`, its matching `FOCUS_LOSS` is mandatory before pause;
  adapters without a proven focus callback do not invent one. Normal return
  additionally requires `NATIVE_SHUTDOWN` immediately before `TERMINAL`. An
  adapter-owned terminal may follow save without a native shutdown only with
  `NXANDROID_PROFILE_ALLOW_ADAPTER_TERMINAL` and a specific terminal contract
  ID.

Normal return also requires `SURFACE_DOWN` before native shutdown, so the core
does not assume that an opaque shutdown callback released graphics ownership.
The narrow adapter-terminal alternative may intentionally end with the surface
still active when its adapter contract proves why native teardown is unsafe.

`TERMINAL` is still only an adapter callback carrying a policy and contract ID.
The core never calls `exit`, `_exit`, raises a signal or launches a process.

The profile is copied into each context. There is no process-global state, so
sequential and independent contexts do not share module, surface, callback or
import state. Lazy modules loaded after Activity are deliberately not inferred
in version 0.2.0; a future contract must add an explicit phase/flag rather than
silently weakening the declared-module gate.

Some approved runtimes expose one blocking owner such as KOTOR's `SDL_main`:
that call bundles entry, window/GL, objects, input, loop and terminal internally.
It must not be threaded or split into fictitious phases just to fit the driven
profile. `RUNTIME_DELEGATED` is the narrow alternative: it must be the final
step, requires `NXANDROID_PROFILE_ALLOW_DELEGATED_RUNTIME` plus a specific
contract ID, and is called synchronously. The core waits for its return and
explicitly makes no claim about lifecycle hidden inside it.

## Callbacks, rollback and reentrancy

`nxandroid_context_step()` executes one declared step and
`nxandroid_context_run()` executes the remainder. An invoke callback is
attempted at most once. A rollback callback is attempted only for a step whose
profile record has a non-empty `rollback_contract_id`, and at most once in
reverse invocation order. No missing rollback record is replaced with guessed
engine teardown.

Rollbackable acquisitions also carry a numeric `rollback_group`. An explicitly
declared forward teardown step may close that group. Once the close callback
succeeds, earlier rollback callbacks in the group are consumed and will never
run again. Thus a later save/terminal failure cannot call resume, focus, input or
surface teardown twice. A failed close does not consume its group.

All same-context API calls, including getters and destroy, are prohibited while
an invoke or rollback callback is active. They return a documented sentinel,
mark a sticky violation and force the outer operation to fail before advancing
the step ledger. This prevents a callback from ignoring `EREENTRANT` and making
the outer run commit anyway. Calls involving another context remain independent.
Each context is driven by one host thread; the callback guard is not a claim of
general concurrent thread safety and deliberately does not hide adapter locking.

Callback code and `ops.userdata` are borrowed from the adapter. They must remain
alive and callable until context destruction completes, including all forward
invocations and rollback attempts. The context never assumes ownership of them.

On a callback failure, the context is fail-stopped and cannot retry later
steps. Every explicitly declared rollback is still attempted even if an earlier
rollback fails. `nxandroid_context_destroy()` is result-bearing so a reentrant
destroy cannot become a silent use-after-free.

## Explicit import policy

`nxandroid_import_catalog` owns a sorted snapshot of provider descriptors, not
the provider code or data. Every provider and request names all of these
fields:

- symbol name and Bionic architecture;
- Bionic, JNI or NDK domain;
- function or data kind;
- optional or critical classification;
- semantic `contract_id`;
- implementation or explicit stub.

An explicit stub also requires bounded human-readable semantics and the import
request must opt into stubs. There is no lookup callback, `dlsym`, host-name
fallback or generic zero stub. A missing strong import fails. A missing critical
weak import also fails. Only an import declared both non-critical and weak may
bind to zero.

Catalog creation uses a deterministic merge sort and resolution uses binary
search. Public counts and strings are bounded by the constants in
`include/nxandroid.h`; module duplicate validation is intentionally bounded to
at most 64 declarations. Binding output is transactional: an error leaves the
entire caller array untouched.

Binding `contract_id` and `stub_semantics` strings are catalog-owned. Provider
addresses are borrowed: their code or data must outlive every binding consumer
and the catalog. The required destruction order is consumers first, catalog
second, provider code/data last; destroying the catalog does not unload or free
a provider.

The catalog describes compatibility decisions; it does not implement Bionic
layouts, JNI tables or NDK objects. Those stay in ABI- and adapter-specific
providers.

## Passive JNI adapter primitives

`include/nxandroid_jni_adapter.h` is an optional support surface. Including it
does not install a `JNIEnv` vtable, create Java objects, register methods or
change an existing port. The lifecycle/import API remains at
`NXANDROID_API_VERSION=1`; this separate surface starts at
`NXANDROID_JNI_ADAPTER_API_VERSION=1`.

`nxandroid_utf16_copy_region()` accepts an adapter-owned canonical UTF-16 view
and `jsize`-shaped start/length values. It copies exactly that many 16-bit code
units, writes no terminator, permits a zero-length region at the end and treats
surrogates as code units, including a region that selects only one half of a
pair. Bounds use subtraction rather than `start + length`, so signed inputs and
integer wrap cannot bypass validation. `EBOUNDS` lets the adapter raise its own
`StringIndexOutOfBoundsException`; `EINVAL` reports a malformed view or output.
The constants for JNI slots 220 (`GetStringRegion`) and 221
(`GetStringUTFRegion`) are intentionally distinct. UTF-8/MUTF-8 conversion and
the string object's storage remain adapter-owned: a byte-backed fake string
cannot be made UTF-16-correct by widening bytes at region-read time.

`nxandroid_prefs_snapshot_create()` deep-copies one typed view of a preferences
store for an adapter-owned `SharedPreferences.getAll()` implementation. The six
preserved families are String, StringSet, signed 32-bit integer, signed 64-bit
integer, 32-bit float and boolean. Each call returns a new independent snapshot;
keys, strings, the set array and every set member are snapshot-owned until
destroy. Later writes to the source store cannot change an older snapshot.
Duplicate keys, duplicate StringSet members, invalid views/types and booleans
outside 0/1 fail before publishing output. Float bits are copied without
normalization or promotion.

The adapter must hold its store lock while snapshot creation reads the borrowed
input and must prevent destroy from racing readers. Map/Set/Iterator/Entry Java
objects, boxing, signatures, exceptions and iteration policy remain entirely in
that adapter. Snapshot enumeration order is not a Java Map/Set guarantee. There
is no process-global store, singleton map or automatic default; ports adopt the
helpers explicitly and keep their own persistence format and migration rules.
The detailed ownership and error contract is in `CONTRACT.md`.

## Strict Android input boundary (C7)

`include/nxandroid_android_input.h` is the additive replacement for using the
historical fan-out sink helper as a runtime dispatcher. The old
`nxandroid_input_sinks` API remains ABI-compatible and available as a fixture;
it is not the C7 authority. The strict API accepts one frozen GPTK V2 authority
snapshot and validates these rules before a pad can be announced:

- an `ACTION` has exactly one declared KeyEvent, MotionEvent, JNI push, JNI
  pull, native-gamepad or touch sink;
- `SUPPRESS` (`null`) reaches no callback, pull API or touch fallback;
- `NATIVE` has exactly one per-control route, and a native route can never be a
  synthetic touch route;
- menu/gameplay are complete 18-control contexts. An optional cursor context is
  explicit and cannot silently steal the gameplay right stick;
- every accepted callback carries instance/device identity, a hotplug
  generation, source, context, monotonic sequence, caller-provided monotonic
  timestamp, key/axis identity and the returned handled/value acknowledgment;
- an event receipt advances only after the real adapter callback returns a
  matching acknowledgment. JNI pull receipt state advances only when the
  consumer actually calls `nxandroid_android_pull()`, not when input is queued.

`include/nxandroid_android_gptk.h` is the optional bridge to the accepted
`nxinput_gptk.h`. Its ordinal assertions fail the build on vocabulary drift and
it calls the C3/C4 authority once for each context/control pair. It rejects V1
rather than guessing whether an absent binding means native or disabled.

Triggers keep analog values and use separate 0.60 press / 0.40 release edges.
Focus loss, pause, explicit cancel, context replacement and hot-unplug release
only owned state; resume/reconnect never resurrects an old press. Pads with the
same GUID remain independent by instance and generation. Touch is resolved
inside the intersection of drawable, content rect and safe area, with explicit
0/90/180/270 rotation. Cursor motion is time-based, radial-deadzone, curved and
smoothed; the adapter still owns the approved polished arrow renderer and may
enable cursor/R3 touch only in a source-proven context.

`references/c7-android-consumers-v1.json` bounds the evidence to Blossom Tales,
Off The Road and Geometry Dash SubZero from the user-selected last-30 R2 cut.
Their published artifacts and source contracts are immutable references, not
silent adopters of 0.4.0. The new API is host-fixture proven; each pilot remains
`PENDING` until an opt-in port release exercises these exact bytes physically.

## Strict Unity consumer boundary (C8)

`include/nxandroid_unity_input.h` treats five independent consumers: built-in
Legacy Input, Unity's New Input System, Rewired, InControl and a raw Android
`InputDevice` player. A deployable profile pins exact Unity/plugin/consumer
versions, ABI, assembly, metadata/runtime/artifact hashes, source commit/tree,
license and full registration/enumeration/producer/read/action signatures.
Wildcard versions, name-plus-arity lookup, missing provenance and an identity
override without artifact-bound A/B proof fail before context creation. The API
has no address or RVA field.

The adapter first records `init_array`, each real `JNI_OnLoad`, player init,
surface, resume, focus and frame-loop phases on the declared player thread.
Only then may the declared registration API announce a pad. Input arrives only
through C7's already-frozen route; C8 never polls SDL, evdev or Android again.
The producer callback must return a matching acknowledgement, but that does not
claim consumption. A complete receipt exists only after the adapter separately
reports both the exact low-level API return and the exact game-action API
return for the same pad/generation/control/event sequence.

`references/c8-unity-consumers-v1.json` is limited to the exact last-30 R2 cut
selected for C7 and classifies all 14 Unity candidates exactly once. Sonic
Runners leaves Legacy Input `PENDING`; no candidate proves New Input System;
Cat Quest, Stranger Things 3, Super Mombo and Kingdom are Rewired historical
leads while Daggerhood is a measured no-action-map negative; Suzy Cube, Neon
Shadow and Party Hard bound the InControl evidence; Vector and Stranger Things
1984 remain negative for raw player input. No tuple closes exact plugin
identity, all API returns, exclusive C7 routing and these new bytes, so none is
deployable and no approved port was edited. The five-profile/90-control host
harness is explicitly `FIXTURE`, never a plugin, port or physical-device claim.

## M16 approved-adapter ledger

`references/m16-adapter-contract-v1.json` is the read-only evidence ledger for
the five approved references: Bully 2, Sonic 4 Episode II, Horizon Chase,
KOTOR and `asm2_127`. It records file/line evidence, provenance, reusable
mechanisms, adapter-owned data, known limits and suggested tests for M16-001
through M16-020. It deliberately excludes WIP sources and never promotes
offsets, callbacks, JNI bindings, lifecycle, save or shutdown behavior into a
universal default.

The pure host gate is:

```sh
python3 -B framework/nxandroid/tests/test_m16_adapter_contract.py
```

It validates the exact adapter scope, evidence paths, line bounds, provenance,
specificity counters and fail-closed defaults. The state is
`closed_for_framework`: M16-013 and M16-014 combine the prior acceptance of the
five finalized ports with additional sanitized technical observations, while
M16-020 closes the contract gate.

`references/m16-runtime-receipt-v1.json` states the evidence boundary explicitly.
The approved ports already carry the human save/load and gameplay acceptance;
the agent does not claim to have replayed campaigns or reached every checkpoint.
The additional observations cover native returns, save boundaries and clean
shutdown where available. None of that promotes an offset, callback, JNI map,
lifecycle sequence, save schema or terminal action to a universal default. A new
adapter still needs its own acceptance before release.

## Build and deterministic host gate

```sh
cmake -S framework/nxandroid -B /tmp/nxandroid-build \
  -DNXANDROID_ENABLE_SANITIZERS=ON
nice -n 10 cmake --build /tmp/nxandroid-build --parallel 2
ctest --test-dir /tmp/nxandroid-build --output-on-failure
```

`tests/test_nxandroid.c` covers normal execution, invalid order, every callback
failure point, sticky invoke/rollback reentrancy, explicit rollback, 1,000 fresh
contexts, surface and resume cycles, both approved GL/surface orderings, the
source-proven Sonic-style pre-Surface entry opt-in and its fail-closed cases,
blocking run-loop order, a KOTOR-like delegated owner, terminal opt-in, catalog
mismatch, critical weak rejection, transactional resolution, UTF-16 region
bounds/code-unit behavior and typed deep-copy preference snapshots. It uses
only host-owned mocks: no guest ELF, initializer,
`JNI_OnLoad`, device, network or signal is used.

`tests/test_android_input.c` separately covers the C7 exclusive dispatcher,
acknowledged down/up and axes, JNI push/pull, native gamepad, touch geometry,
cursor, cancel/focus/pause, same-GUID multipad, hotplug generations and receipt
redaction. `tests/test_android_gptk_bridge.c` is built by the directed C7 gate
against the hash-pinned C6 nxinput owner and proves SELECT+START remains
out-of-band with L2+R2, GUIDE+START and cross-pad negatives.

`tests/test_unity_input.c` exercises all five C8 fixture families and all 90
declared controls through the real C7 dispatcher, with separate producer and
two-stage consumer returns. `tests/test_unity_exit_chord.c` links the accepted
C6 chord directly, while `tests/test_c8_unity_ledger.py` recomputes every
selected Git blob/tree and R2 receipt without executing a guest. These are
host contracts only; physical adoption remains per-port and opt-in.

## Owner-guest inventory availability

The M11 inventory contains hashes and ABI expectations for Android ELFs that
remain owner-provided and therefore are not present in a clean public checkout.
Inside the required sealed namespace, `tools/inventory_m11_guests.py` handles
that boundary in three states:

- all expected guests present: hash, ELF and ABI inventory runs normally;
- all expected guests absent: the tool records an explicit zero-execution
  `SKIP` and lets the host matrix continue;
- any partial set, symlink, non-regular file, hash or ABI mismatch: the gate
  fails closed.

`--require-guests` turns the all-absent state into an error for evidence runs
that require the full owner dataset. `--emit` always requires the full set.
Neither path loads guest code, calls initializers/JNI or accesses a device.

## Isolated external-signal gate

`tests/test_signal.c` is deliberately excluded from CTest and must never be
invoked directly. Its only supported entry point is:

```sh
bash framework/nxandroid/tests/run-signal-isolated.sh
```

The runner reuses nxbootstrap's sealed user/PID/mount namespace guard and
watchdog and has no host fallback. The fixture opens a Linux pidfd for each
direct child before releasing it; if pidfd authority is unavailable it exits
77 inside the namespace without substituting a raw PID signal. `SIGTERM` is
sent only through the target child's pidfd. A separately identified sibling is
kept alive throughout both cases and receives no signal.

The active case blocks in the declared `RUN_LOOP`; its async-signal-safe handler
does only a `sig_atomic_t` update and a nonblocking self-pipe write. The same
forward lifecycle then performs input disable, focus loss, pause, save, surface
down, native shutdown and terminal exactly once. The early case receives the
signal before entry and proves an explicitly declared rollback without starting
guest code. Both GCC and Clang builds are compiled and run in the sealed suite;
the output includes source/binary hashes plus explicit zero guest initializer,
`JNI_OnLoad`, device, network and hardware claims.
