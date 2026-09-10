# Validation record and reproduction

The public repository contains source, tools and three selected physical
[framebuffer screenshots](../README.md#capturas-reais--real-screenshots).
The recorded device executables, original game data and raw run logs remain private.
The results below describe those tests; cloning or rebuilding the source
does not reproduce their evidence automatically.

## Two distinct executable records

| Tested executable | Observed scope |
| --- | --- |
| `6bf61d62…` | Preserved gameplay reference: original menu/intro, intro completion and native save, first-stage entry, and successive captures showing hero/camera movement through the village using a physical controller |
| `c9b02720…` | Source successor with the Select+Start service change: 18 directed host cases passed; physical menu at about 40 FPS; quit chord exited with status 0 and restored the console timer |

These identifiers are SHA-256 prefixes of privately tested Linux executables,
not Git commit IDs or downloadable binaries. The full gameplay-reference
SHA-256 is:

```text
6bf61d62471d459939200cecdca1cba52b9ad0273b54ca3ca389fcb3b26e287a
```

The original reference remains preserved. Its gameplay result is not a
new gameplay test of the successor. Build environment, debug paths and
toolchain differences can change executable bytes; a new build needs its
own device validation.

Observed menu/intro performance was about **40 FPS**. In the visible first
stage it was about **13–15 FPS**. After playing, the owner said the experience
felt right. This is subjective acceptance of initial gameplay, not a claim
of 40 FPS in-stage, full-stage completion, combat coverage or long-term
stability. Audio playback was initialized physically, but listening-based
audio approval remains unrecorded. A later return to the opening/tutorial
area after controller reset has not been fully diagnosed.

## What the directed tests establish

| Area | Evidence and boundary |
| --- | --- |
| Objective-C/Foundation | Directed tests of implemented signatures, ownership, Blocks, collection operations, parsers, enumeration and invocation; not comprehensive Apple framework conformance |
| Input ordering | Real system SDL virtual controllers verify short down/up pairs before one tick, global event order, axes/Y orientation, held state, snapshot reconciliation and disconnect release |
| Select+Start | 18 fresh-process cases cover button order, one-tick chords, snapshots, initial held state, duplicates, separate controllers, reconnect, synthetic-input separation, status 0 and registered cleanup |
| Video classification | Initial full frame, one-row rotation, same-present full fallback for an off-row lit pixel, 60 full-confirmed black frames, GL-error fatal behavior and pack-alignment restoration |
| Image decoding | The private owner-data run decoded 228 original PNG files, including 186 CgBI; the files are not distributed here |
| Audio | The private data run decoded 69 CAF/AAC files and checked frame timelines; system OpenAL loopback produced nonzero PCM without certifying physical audibility |
| Idle policy | Mocked-syscall tests cover disable/restore/failure paths; device testing also confirmed the original timer value was restored |

Host fixtures do not execute the original ARM64 game on an x86 host. Some
inspect game metadata or compare parsers against owner-supplied data. SDL
virtual-controller tests do not certify the mapping of every physical pad.

## Prepare local data and build

From the repository root, use a compatible owner-supplied IPA:

```sh
python3 tools/prepare_owner_data.py /path/to/owned.ipa --output data
make -C prototype TOOLCHAIN=/path/to/nextos/toolchain -j4
```

The preparation tool checks the supported app/version and ARM64 executable
identity, then writes `data/goblin-sword.macho` and `data/assets/`. Do not
commit the outputs, saves, private diagnostics or generated executables.
Local validation matched all 637 prepared resource hashes and the Mach-O
against the privately tested inputs.
The ARM64 runtime is executed on the compatible target, with framebuffer,
VT and resource-directory access as described in [architecture](architecture.md).

```sh
GOBLIN_DIAGNOSTIC_SECONDS=120 ./prototype/build/goblinsword-ios-nextos data/goblin-sword.macho data/assets
```

Use `0` for no diagnostic alarm limit. The default without that variable is
30 seconds. A timed diagnostic termination is distinct from the tested
Select+Start exit status 0.

## Focused host fixtures without game data

These examples use a native host C++17 compiler and SDL2 development files.
The fixture build directory is separate from the target runtime build:

```sh
mkdir -p build/host
g++ -std=c++17 -O2 -Iprototype tools/host_services_event_fixture.cpp prototype/services.cpp -lSDL2 -pthread -o build/host/services-events
./build/host/services-events
g++ -std=c++17 -O2 -Iprototype tools/host_exit_chord_fixture.cpp -lSDL2 -pthread -o build/host/exit-chord
./build/host/exit-chord
```

The exit fixture includes the service implementation directly and must not
also link a second `services.cpp` object. It uses subprocesses to exercise
real exit and cleanup behavior.

The graphics classification fixture requires EGL/GLES2, PNG and zlib headers
to compile the source, but supplies mocked GL calls and opens no display:

```sh
mkdir -p build/host/proof
g++ -std=c++17 -O2 -ffunction-sections -fdata-sections tools/test_graphics_sample_host.cpp -Wl,--gc-sections -ldl -o build/host/graphics-samples
./build/host/graphics-samples build/host/proof
```

It writes local sample receipts to the supplied directory. Those receipts
prove the fixture's classification decisions, not pixels from the real game.

## Data-dependent fixtures and physical retesting

The runtime, protocol and invocation fixtures use the local Mach-O to
register the real class metadata:

```sh
bash tools/test_prototype_host.sh runtime --macho data/goblin-sword.macho
bash tools/test_protocol_host.sh --macho data/goblin-sword.macho
bash tools/test_invocation_host.sh --macho data/goblin-sword.macho
```

Foundation data comparisons and the PNG/CAF tests require the owner's
resources or IPA. These private inputs are not included in the checkout.
See [testing instructions](testing.md) for the host test entry points;
run only the fixture relevant to the changed component.

For a successor, preserve the last working executable and its identity,
build the new source separately, then inspect actual pixels and physical
input on the target. Confirm the menu, intro/save behavior, first-stage
movement and clean exit as separate observations. Record the exact binary
identity and scene-specific FPS locally. Do not infer a successful game
display from audio, a living PID or an EGL context alone.

This publication is not a packaged release, a universal-device validation,
or a promotion of these shims into a global framework.
