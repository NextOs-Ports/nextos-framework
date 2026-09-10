# nxandroid 0.5.0 regression matrix

| Risk | Focused proof | Gate | Expected |
|---|---|---|---|
| JNI slot 220 accidentally uses byte/MUTF-8 semantics | exact slot constants; ASCII, BMP and surrogate code units | `tests/run-host.sh` GCC + Clang | 220/221 distinct; verbatim UTF-16 |
| Region writes a terminator or crosses bounds | sentinels, zero-at-end, negative/out-of-range and `INT32_MAX` cases | ASan/UBSan/LSan | destination unchanged on error; no NUL write |
| Region overlap invokes undefined behavior | overlapping in-place fixture | GCC + Clang sanitizers | memmove semantics |
| `getAll()` stringifies typed values | six-family snapshot fixture and float-bit comparison | GCC + Clang | exact tag/value preserved |
| Same-session/source cache masks persistence defects | fresh-PID rule in device checklist | per-port hardware acceptance | read occurs in new PID before any write |
| A later store write mutates an older map | mutate source after snapshot A; create B | ASan/UBSan/LSan | A unchanged; B sees new source |
| Reused global map invalidates another iterator | independent A/B lifetime and destroy-A-first fixture | ASan/LSan | B remains valid |
| Partial snapshot escapes on invalid input | invalid view/type/bool, duplicate key/set member, public limits | host C test | error with output NULL |
| New source/header omitted from release or analyzers | installed-header/link smoke; Clang analyzer; GCC `-fanalyzer` | `tests/run-host.sh` | both compiler suites pass |
| Helper is mistaken for a universal fake JNI | header/README/contract and evidence boundary | review + JSON validation | no VM/vtable/dispatch/default |
| One semantic action reaches a sink twice | ordered multi-sink dispatch fixture | `tests/run-input-sinks-host.sh` GCC + Clang | exactly one delivery per registered sink/action |
| Touch coordinates escape rotation or safe area | normalized coordinate/rotation fixtures | `tests/run-input-sinks-host.sh` GCC + Clang | bounded pixel result or fail-closed error |
| Virtual SDL gamepad duplicates the physical source | exclusive-source fixture | `tests/run-input-sinks-host.sh` GCC + Clang | only the declared authority feeds each logical action |
| Clean public checkout lacks proprietary M11 guests | pure all-absent classifier + sealed inventory invocation | `tests/test_m11_audit.py` + isolated suite | explicit zero-execution SKIP; host matrix continues |
| Incomplete owner dataset hides a missing or stale ELF | pure partial-set classifier; per-file symlink/hash/ABI checks | `tests/test_m11_audit.py` + inventory | partial set always fails closed |
| GPTK is read twice or V1 absence is guessed | real C6 parser bridge and 54 authority decisions | directed C7 gate | one frozen V2 snapshot; V1 rejected |
| `null` leaks into key, axis, JNI, native or touch | A/B press/release plus pull/read-policy checks | `nxandroid.android-c7` | zero consumer events and no pull route |
| One control fans out like the historical v1 helper | duplicate route negatives and shared-action hold aggregation | `nxandroid.android-c7` | exactly one declared sink per action/control/context |
| Trigger loses analog range or chatters | 0.2/0.7/0.8/0.3 sequence and disconnect center | `nxandroid.android-c7` | every analog value delivered; one enter/exit edge |
| Hotplug or duplicate GUID crosses pad state | same GUID, distinct instance/generation, cancel/reconnect | `nxandroid.android-c7` | only target state released; new generation on reconnect |
| Focus/pause resurrects stale input | held key release, inactive input, resume | `nxandroid.android-c7` | release precedes lifecycle; no synthetic re-press |
| Touch escapes content/safe rect or rotates incorrectly | four rotations, invalid geometry, edge coordinates | `nxandroid.android-c7` | bounded pixels or fail closed |
| Cursor steals gameplay/D-pad or is FPS-dependent | explicit cursor context, right-stick radial/time-based fixture | `nxandroid.android-c7` | only declared right-stick route moves cursor |
| Receipt claims an attempted rather than accepted call | malformed ack fail-stop and explicit JNI pull read | `nxandroid.android-c7` | receipt advances only after callback/read |
| Exit chord reaches game or accepts wrong controls/pads | real C6 chord + GPTK bridge | directed C7 gate | same-pad SELECT+START only; three negatives |
| Pilot provenance or 18-control inventory drifts | commit/tree/blob/R2 receipt recomputation | `test_c7_consumer_ledger.py` | 3 recent consumers, 54 controls, classes remain honest |
| Unity is treated as one compatible ABI | five distinct profile kinds and family-specific full signatures | `nxandroid.unity-c8` | cross-family or name-only API rejected |
| Mock/enqueue is called a consumer receipt | producer return followed by separate low-level and action returns | `nxandroid.unity-c8` | complete count changes only after both real-return boundaries |
| Native Unity startup/thread is skipped | exact initial lifecycle plus pause/recreate/shutdown and thread-token negatives | `nxandroid.unity-c8` | out-of-order or wrong-thread call fail-stops |
| A pending profile silently ships | evidence-class deployment gate | `nxandroid.unity-c8` | `PENDING`, `UNPROVEN` and `N/A` cannot initialize |
| `null` or `native` gains a fallback route | C7 suppression/native authority feeding all five C8 fixtures | `nxandroid.unity-c8` | A/B null produce zero calls; native produces one route |
| Unity reconnect crosses pads/generations | same GUID, two instances, remove/reconnect and per-event receipts | `nxandroid.unity-c8` | independent state and new generation |
| Exit chord becomes a Unity/keyboard action | accepted nxinput chord linked at the C8 boundary | `test_unity_exit_chord.c` | same-pad SELECT+START only; zero Unity/keyboard events |
| Historical Unity lead becomes support without exact proof | 14 candidates, 18 direct Git blobs/trees and R2 receipt recomputation over the exact last-30 cut | `test_c8_unity_ledger.py` | five profiles classified; zero deployable real profiles |

The host matrix executes no guest, initializer, `JNI_OnLoad`, device, signal or
network path. Physical `getAll()` migration evidence is intentionally pending
for each adopting adapter; no existing port is silently migrated.

The C7 host fixtures likewise do not claim an Android device or a migrated
pilot. Blossom Tales, Off The Road and Geometry Dash SubZero remain
`PENDING_PHYSICAL` for the new bytes despite their separately recorded physical
history as published artifacts.

The C8 fixtures likewise make zero real-plugin, real-port or physical claims.
All 90 fixture controls are exercised, while every real last-30 candidate stays
`PENDING` or `UNPROVEN` and reports `reachable=0` for the new bytes.
