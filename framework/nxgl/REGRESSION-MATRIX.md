# nxgl 0.3.5 regression matrix

## 0.3.5 — fatal lock-free

| Caso | Prova | Gate |
|---|---|---|
| poll de `is_fatal` durante leitura legítima na thread de render | `tests/test_frame_proof_fatal_race.c` fase A: 40/40 leituras, 0 falsos fatais (0.3.4: ~1e6 falsos, 38-39 leituras) | `run-v3-graphics-host.sh` fatal_race |
| fatal conclusivo ainda arma no cronograma before-present | fase B: 599 presents pretos sem fatal, 600º arma; consumo único; persistente | idem |


| Gate | Expected result | Evidence class |
|---|---|---|
| Godot hook order | launch → resolver → context → before-present → publish | `tests/run-godot-frame-proof.sh` |
| Godot no synthetic frame | wrapper has no clear/draw/window/swap operation | source contract + directed stub gate |
| Godot misuse | pre-context present, duplicate launch and post-stop calls fail | `tests/run-godot-frame-proof.sh` |
| Godot fatal loop | third conclusive BLACK and DEAD-CONTEXT return `-2`; no later call reaches a fourth sample/present; close is one-shot, health remains forbidden and exit is 72 | `tests/run-godot-frame-proof.sh` |
| Fatal receipt lifecycle | persistent fatal consumes once, safely revokes pre-existing health and cannot be cleared by a late OK; OK consumes nothing | `tests/test-video-receipt.sh` |
| GLES pack enum identity | row-length=`0x0D02`, skip-rows=`0x0D03`, skip-pixels=`0x0D04`; each incompatible state is rejected before readback | `tests/test-video-receipt.sh` |

| Gate | Expected result | Evidence class |
|---|---|---|
| V4-DISPLAY-01 content rect | `game` is a no-op; `preserve`/`fill`/`adaptive`/`stretch` produce the exact rect at 480p, 720p and ultrawide | `tests/test_v4_display.c` |
| V4-DISPLAY-01 input inverse | exact panel<->content round trip at 1:1 or upscale; a touch on a bar is discarded, never clamped | `tests/test_v4_display.c` |
| V4-DISPLAY-01 finite policies | unknown policy, invalid drawable, invalid internal size and incoherent limits fail closed | `tests/test_v4_display.c` |
| V4-GRAPHICS-03 correct binding | GLVND split, monolithic Mali and the already-global path bind the DSO that owns the current context | `tests/test_v4_egl_binding.c` |
| V4-GRAPHICS-03 negatives | absent resolver, mute dladdr, missing import, null context, lookalike SONAME, failed promotion and malformed inventory never reach the global namespace | `tests/test_v4_egl_binding.c` |
| V4-GRAPHICS-03 declared inventory | an EGL import outside the declared inventory fails before init_array | `tests/test_v4_egl_binding.c` + `tests/run-v4-host.sh` |
| V4-GRAPHICS-03 real ELF audit | JUMP_SLOT/GLOB_DAT inventory read from a shared object built on the spot; DT_NEEDED libEGL refused; GLIBC ceiling checked | `tests/run-v4-host.sh` |
| V4-GRAPHICS-04 pure state machine | UNINITIALIZED refuses after_present; 1x1 AND large pre-present drawables are only AWAITING_FIRST_PRESENT; API/profile/version/shader divergence terminal before pending; dead provider is provider-nominal-only; token/provider change, 0x0, unreadable, absurd, broken clock fail closed; the deadline starts at the first present, never restarts, and 1x1 fails exactly at it; PROVED/REJECTED are stable one-shots; reset invalidates everything | `tests/test_v4_graphics_present_gate.c` (GCC, Clang, ASAN, UBSAN) |
| V4-GRAPHICS-04 lifecycle adapter | Fake Wayland lifecycle (drawable materializes only on the guest's real swap): preflight pending on 1x1 with control returned; spy proves real swap strictly before observation; zero clear/draw/swap fabricated; one receipt under two concurrent presenters; SDL2 `SDL_GL_GetDrawableSize` and SDL3 `SDL_GetWindowSizeInPixels`; a discarded probe context never authorizes the real one; no resolver work per frame after PROVED | `tests/test_v4_graphics_lifecycle_adapter.c` (GCC, Clang, ASAN, UBSAN) |
| V4-GRAPHICS-04 legacy literal | The 0.3.0 one-shot API keeps rejecting a stuck 1x1 with the same reason and receipt shape; `nxgl_graphics_drawable_usable(1,1)` stays false; the reason enum only appends | `tests/test_v4_graphics_lifecycle_adapter.c` + `tests/run-v3-graphics-host.sh` |
| V4-GRAPHICS-04 purity/no-brand | The present gate core touches no dl/env/GL/SDL/clock/IO and no device, CFW, GPU or game name; the adapter resolves no clear/draw/swap symbol | `tests/run-v4-host.sh` static audit |
| Nothing selected by name | no device, CFW or GPU name is a condition in the new sources | `tests/run-v4-host.sh` |
| nxloader provider default-off | `flags=0` leaves the real registry addresses and priorities unchanged | Hermetic host, real nxloader registry |
| nxloader provider allowlist/originals | Enabled install captures the selected originals and raises only MakeCurrent, GetProcAddress and glCreateShader; unrelated GL symbols retain their exact address | Hermetic host, real nxloader registry |
| Post-MakeCurrent barrier | Original MakeCurrent succeeds first; context/drawable/shader JSON evidence is sunk exactly once before shader creation forwards | Hermetic host with effectful fake EGL/GL |
| Fail-closed shader/receipt | A failed graphics proof or receipt sink blocks MakeCurrent/shader creation permanently for that process; no public verify-only bypass exists | Hermetic host |
| ROCKNIX / Vector Unit BBR1 | Requested GLES2/profile ES/exact 2.0/ESSL100 accepts real ES2 and rejects desktop GL, 1x1 and shader failure | Hermetic named field fixture |
| ROCKNIX / Vector Unit BBR2 | Requested GLES3/profile ES/minimum 3.0/ESSL300 accepts real ES3 and rejects desktop GL, 1x1 and shader failure | Hermetic named field fixture |
| Single-channel semantics | Native full swizzles and fallback bytes preserve RED `(r,0,0,1)`, ALPHA `(1,1,1,a)`, LUMINANCE `(l,l,l,1)`, or an explicitly declared alpha mask; unrepresentable RED fallback rejects | Hermetic host |
| Explicit RED coverage compatibility | `RED_COVERAGE_COMPAT=3` leaves native ES3 at R8/RED with the existing alpha-mask swizzle and rewrites only a measured legacy fallback to LUMINANCE_ALPHA `(R,R)`; exact fixture `00 7f ff -> 00 00 7f 7f ff ff` passes for TexImage and TexSubImage | Hermetic host, GCC + Clang |
| Legacy semantic/numeric stability | `PRESERVE=1`, `ALPHA_MASK=2`, transforms 0..3, API-1 quarantine and all old byte layouts remain literal; ALPHA_MASK fallback is still `(255,R)` | Hermetic host, exact numeric/byte fixtures |
| Planner/observer boundary | Capability measurement may resolve `glTexStorage2D` for availability, but measurement and storage/image/subimage planning never invoke, wrap or replace the fake TexStorage function | Hermetic host with effectful fake GL |
| Single-channel state/layout | Bindings are context/thread/unit/target scoped; objects are share-group/target/id scoped; unpack alignment/row-length/skips, subrects, PBO, type and all size arithmetic fail closed | Hermetic host |
| RGBA/compressed isolation | Unknown, multi-channel, RGBA and compressed storage/upload plans are returned unchanged even when the adapter is enabled | Hermetic host |
| Target-SDL sanitizer disabled | Initializer keeps `enabled=0`; environment and target SDL inventory remain untouched | Hermetic host |
| No inherited `SDL_VIDEODRIVER` | Exact target-SDL drivers are recorded and autodetection remains unmodified | Hermetic host |
| Supported inherited hint | `KMSDRM`/`dummy` capability match is case-insensitive, the original value is preserved byte-for-byte, and the selected target-SDL backend is recorded after init | Hermetic host |
| Unsupported inherited hint | `mali` against a target SDL compiled with only `KMSDRM,dummy` removes only `SDL_VIDEODRIVER`; no replacement or provider variable is introduced | Hermetic host |
| Other supported backend | `wayland` against a target SDL compiled with `wayland,x11` is preserved and selected; KMSDRM/fbdev is never forced | Hermetic host |
| Sanitizer fail-closed boundaries | Malformed hint, unavailable driver inventory and pre-initialized video publish bounded failure receipts without mutation; BUSY and invalid calls are byte-atomic | Hermetic host |
| API-v1 nxcompat receipt bridge | Existing window/GLES/EGL receipt tests pass unchanged | Hermetic host |
| API-v2 SDL/EGL receipt bridge | Frozen v2 handles publish all strong graphics capabilities; raw-EGL/mismatched handles fail closed | Hermetic host |
| Pre-context provider plan | Only exhausted matching window-create/window-failed or context-create/context-failed, no inherited provider hint, same object, both symbol sets and compatible transport authorize one retry | Hermetic host |
| Opt-in provider recovery archive | Disabled init performs no load; invalid/BUSY outputs are byte-atomic; explicit fake candidates prove same-DSO EGL/GLES ownership, live EGL init/terminate refuses missing teardown attestation, terminate failure retains the handle and poisons every later otherwise-valid enabled probe/authorized re-exec without another EGL call, loaded/mixed/missing rejection, unchanged file identity, teardown + positive-plan gates, inherited-env/marker refusal, exact coherent pair, non-empty `LD_PRELOAD` preservation and partial-setenv/failed-exec rollback | Hermetic host with fake DSOs; no real EGL/GPU/device |
| Disproven inherited hint | In the reactive path, a provider hint whose own context measured a NULL renderer is unbound, the override is recorded and the coherent-pair re-exec proceeds; on a failed exec the hinted values are restored; the pre-context path keeps the inherited hint sovereign | Hermetic host |
| Applied-pair rollback | In a re-exec'd process whose applied provider pair failed the real window attempt, one rollback unbinds both SDL provider variables and the caller retries through the firmware stack; a second call is inert, and without the repair marker the environment (including an inherited CFW hint) is never touched | Hermetic host |
| GLES1 liveness selection | Mirrored fake providers: dArkOS shape (live blob + dead versioned Mesa) selects the blob, ROCKNIX shape (dead orphan blob + live versioned Mesa) selects the Mesa, injected primary resolver outranks the chain, all-dead falls back to v1 first-complete with a `liveness=dead` receipt, empty environment fails closed | Hermetic host |
| Failed-open backend report | The real started backend survives a context-create failure; private provider detail remains absent | Hermetic host |
| API/ABI/install | API-v1 canary, GCC/Clang sanitizers, analyzers and installed-link symbol gate pass | Hermetic host |
| Angry Birds / ArkOS / RK3326 family | KMSDRM, coherent Mali G31 provider, GLES 3.2, EGL config, drawable, audio, input and native shutdown | Physical, port-scoped; not a fleet claim |
| Existing Mali-450 baseline | No default provider override; ordinary first pass and prior port behavior remain unchanged | Imported approved port baseline; migration regression pending final package gate |

The physical row authorizes only the Angry Birds adapter behavior proven on
the named hardware family. Both the pure plans and the separately linked
provider helper remain opt-in; they do not turn that row into universal device
support. Swordigo and Bomb Chicken are implementation provenance for the
common pattern, not an automatic migration or a fleet claim.
| Frame proof classifier | UNKNOWN on zero samples is never a pass; BLACK below the threshold; OK at or above it; a caller-supplied stricter minimum is honored; out-of-range inputs rejected | Hermetic host |
| Launch-context classifier | Frontend beats a parallel remote login; remote and unknown launches are inconclusive; console with a real VT is conclusive; a drawn frame stays OK on any launch | Hermetic host |
| GLES1 client-array bridge gate | Enabled only for the exact wayland + Mali-G52 + g24p0 tuple; Mali-G31/KMSDRM, Mali-450/mali and Mali-G310/KMSDRM (whose version string contains "wayland") stay on the direct path; partial tuples never enable it | Hermetic host, tuples measured on real hardware 2026-08-16 |
| Frame proof on device | Swordigo 1.0.15: OK 76.1% (RK3326/dArkOS Mali-G31, ES-CM 1.1), OK 78.6% (NextOS Mali-450), OK 78.7% (Amlogic X5M Mali-G310); Hitman GO 1.2.2: OK 98.1% (RK3326/dArkOS) | Physical, port-scoped; not a fleet claim |
| V3 EGLConfig request default | `nxgl_config_request_default()` is all-don't-care and satisfied by any observed config; RGBA8888 remains a per-adapter declaration and never a global default | Hermetic host (`run-v3-graphics-host.sh`) |
| V3 EGLConfig matcher/receipt | EGL at-least semantics per size attribute, native-visual requirement, malformed calls fail closed, one-line `EGLCONFIG:` receipt names the first violated attribute | Hermetic host |
| V3 mirrored candidate scenarios | Dead/orphan candidate ordered BEFORE a live one loses in both directions (dead blob → live Mesa wins; dead Mesa → live blob wins); the receipt trace names the chosen provider and every candidate's failure reason (live/dead/incomplete/absent) in probe order | Hermetic host with fake DSOs |
| V3 healthy-first no extra rung | With a live first candidate the later rung is never probed: the trace ends at the winner and the second provider present on disk never appears in it | Hermetic host |
| V3 no-draw provider | A provider that accepts every call, reports a healthy renderer and never draws is failed by the frame proof (BLACK, all-black) — the case the discovery receipt alone cannot catch | Hermetic host |
| V3 frame-proof sample point | `sample_point=` remains APPENDED to the `VIDEO:` receipt; `before_present()` records before-present, while AFTER and legacy `sample()` remain diagnostic but now fail closed as `INCONCLUSIVE/presentation-point-unproved` rather than falsely authorizing OK | Hermetic host |
| V4 machine video receipt | Only a visible (RGB nonblack AND literal alpha nonzero) default-framebuffer sample at the explicit before-present boundary writes the exact run/generation/port-bound `OK/non-black` JSON; RGB!=0/A=0 is BLACK, alpha=1 exercises the explicit nonzero boundary without claiming perceptual brightness, and AFTER/unspecified samples cannot authorize | `tests/test-video-receipt.sh`, hermetic host |
| V4 readback integrity/state | Zero draw/read FBO, zero pack PBO, compatible pack state, proved queries, bounded/overflow-checked stride and <=64 MiB are mandatory; two initialized sentinels reject absent/partial writes, a guard rejects overflow, and a pending fake GL error remains byte-for-byte unconsumed because the adapter never calls `glGetError` | `tests/test-video-receipt.sh`, hermetic fake GL |
| V4 proof image inspection | The exact first sample eligible for OK is written as private RGBA PNG by exclusive-temp + fsync + atomic rename + directory fsync and decoded chunk/CRC/zlib/pixel-by-pixel; requested-write failure blocks OK, remains retryable and does not unlink a foreign temp collision | `tests/test-video-receipt.sh` + `tests/inspect-proof-png.py`, hermetic host |
| V4 audio is not video | A live audio marker cannot mask three sequential before-present BLACK/DEAD observations; one manual black remains inconclusive, fatal replaces an earlier OK, cannot be replaced by later colour, and never creates generation health | `tests/test-video-receipt.sh`, hermetic host |
| V4 fatal replacement/retry | Failed fatal replacement revokes the prior OK into an unparsable empty receipt, enters irreversible pending state, preserves a planted foreign O_EXCL temp and succeeds on a later retry without re-emitting historical best colour as OK; path/tuple drift after OK is ignored in favor of the captured original contract | `tests/test-video-receipt.sh`, hermetic host |
| V4 receipt fail-closed boundaries | No samples and inconclusive launches publish no fatal authority; explicit frontend wins parallel SSH but `NXLAUNCH_FRONTEND=0` is not frontend; stale/unsafe tuples, non-private parent, pre-existing 0644 target and symlink target are refused without mutation or adapter temp residue; concurrent/reentrant calls are refused without deadlock | `tests/test-video-receipt.sh`, hermetic host |
| V3 single clean retry | Pure state machine: first real failure authorizes at most ONE retry, second failure is terminal, finished machines refuse every event (exhaustive walk of all report sequences up to length 4); malformed calls refused | Hermetic host |
| V3 no brand-name selection | New V3 sources contain no device/brand token at all; across src/ + include/ the non-comment occurrences stay at the frozen measured baseline (filename dlopen candidates proven by liveness, measured bridge tuple, caller-declared quirk); any growth fails | Hermetic host static gate |
