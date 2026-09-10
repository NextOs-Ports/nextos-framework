# Architecture and execution requirements

This is a game-specific iOS compatibility prototype for AArch64 Linux.
The original ARM64 instructions execute directly. The host loader supplies
Mach-O loading and the subset of Apple framework behavior needed by the
tested Goblin Sword build. It is not a general iOS runtime or an engine port
compiled from the game's source.

## Supported study target

The reference data is Goblin Sword 2.6.9, bundle
`com.gelatogames.goblinsword`, with an unencrypted ARM64 Mach-O executable.
The observed engine stack is Objective-C cocos2d, Box2D and GLES2. Physical
evidence covers NextOS/Amlogic-old with a Mali-450 fbdev driver.

Compatibility with another IPA build, Apple ABI variant, graphics backend
or firmware must be established separately. In particular, this work does
not demonstrate support for Metal or for arbitrary iOS games.

## Source map

| Source | Responsibility |
| --- | --- |
| `prototype/macho_loader.cpp` | Mach-O segments, relocation/binding, symbols and entry information |
| `prototype/main.cpp` | Host registration, loading, original initialization and entry, diagnostic timeout and signal handling |
| `prototype/runtime.cpp`, `runtime-arm64.S` | Objective-C classes/messages, ARM64 call boundaries, Blocks and runtime support |
| `prototype/foundation.cpp`, `foundation-arm64.S` | Strings, collections, parsers, persistence, run-loop/queue helpers and the implemented Foundation call contracts |
| `prototype/darwin_libc.cpp` | Required Darwin libc compatibility boundaries |
| `prototype/graphics.cpp` | UIKit geometry/lifecycle, CoreGraphics image handling, EAGL over EGL/GLES2, presentation and pixel evidence |
| `prototype/services.cpp` | GameController through system SDL2, ordered events, quit chord and explicitly offline GameKit |
| `prototype/audio.cpp` | CoreAudio/AVAudioPlayer subset, CAF parsing and system FFmpeg/OpenAL integration |
| `prototype/idle.cpp` | Scoped Linux console-blanking inhibition and restoration |

## Native game flow

The host registers its framework contracts, loads and binds the Mach-O,
registers the game's class metadata, and runs the original initialization
callbacks before entering the original `main`. The guest then reaches
`UIApplicationMain` and its application delegate.

UIKit window/appearance/layout callbacks drive the original cocos2d view
and renderer. The renderer's resize invokes the director's projection
reshape; the game's delegate creates the menu scene. CADisplayLink drives
the original main loop. Intro completion, save writes and entry into the
first stage remain decisions made by the original game code.

Missing framework calls fail with a diagnostic. A method accepting one tested
signature does not establish general support for every iOS overload or ABI.
The same limitation applies to Foundation reflection, `NSInvocation`, Blocks,
collections and resource formats.

## Graphics and display ownership

The current profile exposes 568×320 logical points at Retina scale 2,
creating a 1136×640 guest drawable. On the tested display, the physical
backbuffer is 1280×720. The guest image is fitted with its aspect ratio
preserved. No lower-resolution profile has been validated.

EAGL creates system EGL/GLES2 contexts and an fbdev native window. A small
GLES2 copy pass transfers the guest color attachment to the default
backbuffer while preserving guest state. Opaque output alpha is required
by the tested Amlogic compositor. The only shader-source adaptation moves
a specifically recognized unconditional derivatives directive before the
cocos2d uniform prelude, matching the observed Mali compiler failure.

Before every swap, pixel evidence reads the real default backbuffer:

1. Until the first nonblack frame, read the entire image. The first nonblack
   proof also writes a full PPM to a private diagnostic directory.
2. Afterwards, read one complete horizontal row, rotating through `h/4`,
   `h/2` and `3*h/4` on successive frames.
3. If that row is black, immediately read the entire framebuffer in the same
   present. Only a full confirmation increments the consecutive-black count.
4. A GL error is fatal. Sixty consecutive full-confirmed black presents are
   also fatal. There is no success based solely on a live process or audio.

Receipts distinguish `full_initial`, `rotating_row` and `full_fallback`,
including the sampled row and tested/read pixel counts. These diagnostics
have measurable performance cost; they are not a release-health framework.

The process needs the real framebuffer and active VT. It must not share the
display/audio/input session with a running frontend or another game instance.
The services layer initializes SDL input only; it does not create an SDL
video window or provide a desktop/Wayland/DRM backend.

## Input and shutdown

System SDL2 maps the controller. Controller button/axis events are dispatched
in queue order to the original GameController handlers; a final snapshot
repairs missed state without duplicating delivered edges. A complete press
and release between rendered frames therefore remains visible to callbacks.
Apple-positive Y points up. Disconnect releases held state before notification.

Start preserves the original pause callback. The source successor adds
Select+Start on the same physical controller as a host quit request. It exits
the current process with status 0 and runs registered cleanup. The chord is
kept separate from synthetic diagnostic input and from two different
controllers. It does not change the game's individual jump/attack bindings.

When the game disables the idle timer, the VT bridge verifies the real
console-blanking state and saves the prior value. It restores that value on
normal exit and through the installed signal cleanup. SIGKILL cannot execute
process cleanup; after a forced kill, verify the console policy before
another test. No console-mode switch is used to manufacture inhibition.

## Data, audio and dependencies

`tools/prepare_owner_data.py` prepares local owner-supplied data. The executable
and resource root are passed separately to the runtime. Foundation reads
resources from that root and stores local application state under its
`.ipa-study/` directory, which must be writable.

PNG and iOS CgBI handling is implemented for the observed image paths.
CAF metadata/timelines are parsed locally; AAC decoding uses system FFmpeg.
AVAudioPlayer and the required OpenAL calls use the system OpenAL library.
Game Center remains unauthenticated and reports an offline error through
the original callback rather than pretending that online services succeeded.

Build and runtime dependencies include system EGL/GLES2/Mali, SDL2, libpng,
zlib, FreeType, FFmpeg and OpenAL, plus the target libc/toolchain. No private
SDL, Mali or OpenAL library is bundled. The build targets the chosen NextOS
sysroot; this repository does not provide a portable binary or claim an
older-glibc compatibility ceiling.
