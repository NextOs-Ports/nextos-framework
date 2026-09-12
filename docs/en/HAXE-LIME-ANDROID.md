# Haxe, hxcpp and Lime on Android

[Português](../pt-BR/HAXE-LIME-ANDROID.md)

This track covers libraries built for Android and run through a Linux ARM adapter. Selected [Tightrope Theatre](../../ports/tightrope-nextos/README.en.md) sources identify the family and document its limits. Also consult [runtime selection](ANDROID-RUNTIMES.md).

## 1. Recognize the actual composition

```sh
python3 tools/inventory_apk.py /private/owner-input/game.apk --output work/haxe-inventory.json
python3 tools/find_reference.py --runtime haxe-lime --abi arm64-v8a
```

Look for `liblime.so` and `libApplicationMain.so` in the same ABI, imports, entrypoints and Java bootstrap. Name-based detection is a hypothesis. Confirm hxcpp and record Haxe, hxcpp, Lime and SDL versions only with evidence; `unknown` is better than an invented version. OpenFL may be part of the application, but Lime alone does not establish it. This track is not a recipe for HashLink, Neko or a desktop executable.

The [Haxe/C++ manual](https://haxe.org/manual/target-cpp-getting-started.html) describes the C++ target and hxcpp. This helps distinguish generated native code from the Java/Android runtime initializing it; it does not guarantee interchangeability with another library version.

## 2. Respect reference status

The [SOURCE-MAP](../../ports/tightrope-nextos/SOURCE-MAP.json) pins `75eb5da65994995e38057c758440faafd63d87e8`. The [README](../../ports/tightrope-nextos/upstream/README.md) identifies **1.0.5-test.1**, a ROCKNIX input candidate with exact physical acceptance pending. It records earlier 1.0.4 evidence, which does not automatically approve the selected bytes.

Use this snapshot for inventory and diagnostics. Behaviors under test are negative references, not an approved implementation to copy. Code reuse requires recovering and checking an approved public reference for the same boundary, with matching commit, hash and evidence. The [source guide](PINS-AND-SOURCES.md) explains recovery without replacing the snapshot. Read [NOTICE](../../ports/tightrope-nextos/upstream/NOTICE.md) and [LICENSE](../../ports/tightrope-nextos/upstream/LICENSE).

## 3. Map callbacks and threads

Selected [src/main.c](../../ports/tightrope-nextos/upstream/src/main.c) documents the observed Android chain: Application/Activity; loading Lime and ApplicationMain; JNI setup for SDL, audio and controllers; surface creation/resizing; `nativeRunMain` on the SDLMain thread, leading to `hxcpp_main`. The UI thread delivers events separately.

Use this chain as a question when analyzing the new build: which callbacks, signatures, arguments and threads does it actually require? Do not call `hxcpp_main` directly before required stages, force a scene/level or invent another game startup route. Source inventory does not authorize copying build-specific transformations or protected-build mechanisms.

## 4. Prove TLS, GC and SDL boundaries

The [hxcpp threads/stacks manual](https://haxe.org/manual/target-cpp-ThreadsAndStacks.html) describes the relationship between threads, stacks and garbage collection. Check thread registration, roots, blocking and resumption for the actual version. A TLS key always returning `NULL` or an empty mutex can break the runtime after initialization.

Selected [bionic.c](../../ports/tightrope-nextos/upstream/src/bionic.c) and [pthread_bridge.c](../../ports/tightrope-nextos/upstream/src/pthread_bridge.c) help locate these contracts; [sdl_java.c](../../ports/tightrope-nextos/upstream/src/sdl_java.c) shows the Java/SDL boundary. Distinguish SDL contained in the Android guest from firmware Linux SDL. Structures, events and callbacks cannot cross versions/ABIs merely because their names match.

Define window, context, surface and present ownership. Retain the flow and threads required by the engine; do not create another window to hide a missing callback. Unsupported imports must produce a verifiable diagnosis.

## 5. Prepare the build and targeted diagnostics

Use [ARM compilation](BUILD-ARM.md), [shims](SHIMS.md) and a separate project to implement proven contracts. Check included/omitted files in the manifest before executing historical README commands; a cited script may be absent from the selection. Pin toolchain and recovered sources. Prefer AArch64 when present and audit GLIBC ≤ 2.30 for every public Linux ELF.

| Failure | Test distinguishing the cause |
| --- | --- |
| Freeze after allocation/scene load | Per-thread TLS, GC, condition/mutex and stack owner; exercise workers and shutdown |
| Surface ready, no image | Callback order, dimensions, current context and pixels before present |
| Event logged, no action | Java/SDL callback and Lime consumer; press/release, menus and gameplay separately |
| Audio fails on resume | OpenSL/SDL boundary, format, queue and callback thread; measure consumption and listen |
| Restart loses progress | Persistent directory, flush, save/reload and shutdown without leftover processes |

## 6. Extract and deliver with evidence

The [selected recipe](../../ports/tightrope-nextos/upstream/extractor.json) identifies libraries and assets for that profile. For another port, inventory the entire Android input and critical payloads. [NXExtract](NXEXTRACT.md) must prepare the complete copy with real hooks, accept compatible repackaging and preserve saves. Do not use another game's prepared data as a success fixture.

Include bilingual `INSTALLATION.md`, preserve UI/NXSplash and V5 pins, and follow [testing and delivery](TESTING.md). Host TLS/queue tests do not approve the game or physical controls. Do not migrate ports or incorporate Tightrope's pending adjustment into V5.

## AI mission

```text
Analyze only the Android build. Confirm hxcpp/Lime, ABI and bootstrap.
Read pinned Tightrope sources for diagnostics, keeping pending acceptance
explicit; do not use the candidate as an approved implementation.
Map threads, TLS/GC, SDL/JNI, surface, audio and the input consumer.
Implement in a separate project with approved public references and pins.
Prepare complete extraction and targeted tests without changing frozen V5.
```
