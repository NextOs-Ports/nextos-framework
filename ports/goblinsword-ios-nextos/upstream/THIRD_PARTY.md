# Third-party dependencies and technical references

This repository publishes the compatibility bridge and its development tools.
A project-code license has not yet been selected. Any future project license
will apply only to the project's own code, not to Goblin Sword, Apple software,
third-party libraries, or their trademarks.

The source review found no vendored library implementations, Apple SDK headers,
game executable, game assets, embedded codec tables, or third-party binaries.
Dependencies below are obtained separately from the build environment or host
system. The upstream license of the exact dependency version remains applicable.

## Dependencies

| Component | Use in this project | Upstream license/reference |
|---|---|---|
| SDL2 | Controller enumeration, system mappings, events, and virtual-controller fixtures | [zlib license; Sam Lantinga and contributors](https://github.com/libsdl-org/SDL/blob/SDL2/LICENSE.txt) |
| zlib | DEFLATE decompression for CgBI PNG images | [zlib license; Jean-loup Gailly and Mark Adler](https://zlib.net/zlib_license.html) |
| libpng | Standard PNG decoding | [PNG Reference Library license; version-specific notices](https://github.com/pnggroup/libpng/blob/libpng16/LICENSE) |
| FFmpeg: libavformat, libavcodec, libavutil, libswresample | CAF demuxing, AAC decoding, and PCM conversion | [LGPL-2.1-or-later by default; optional components can change the build's license](https://ffmpeg.org/legal.html) |
| OpenAL implementation, normally OpenAL Soft | Playback through the host's `libopenal.so.1`; loopback audio fixture | [OpenAL Soft LGPL-2.0-or-later and separate component notices](https://github.com/kcat/openal-soft/blob/master/README.md#license) |
| FreeType | Listed in the prototype build's link dependencies; no FreeType source or fonts are bundled | [FreeType License or GPLv2 alternative](https://freetype.org/license.html) |
| EGL/OpenGL ES implementation | Contexts and rendering through the host's Mali/EGL/GLES libraries | Driver/provider license; [EGL registry](https://registry.khronos.org/EGL/) and [OpenGL ES registry](https://registry.khronos.org/OpenGL/index_es.php) document the interfaces |
| Capstone | Optional Python static-analysis and disassembly tools | [Capstone BSD license](https://github.com/capstone-engine/capstone/blob/5.0.6/LICENSE.TXT) and [LLVM-derived component notices](https://github.com/capstone-engine/capstone/blob/5.0.6/LICENSE_LLVM.TXT) |

The build also uses the platform's C/C++ runtime, Linux userspace headers, Python
standard library, and compiler tools. System headers are included by reference,
not copied into this tree. Current upstream EGL and GLES2 headers have distinct
license identifiers: [EGL Apache-2.0](https://github.com/KhronosGroup/EGL-Registry/blob/main/api/EGL/egl.h)
and [GLES2 MIT](https://github.com/KhronosGroup/OpenGL-Registry/blob/main/api/GLES2/gl2.h).
Those header licenses do not describe the host GPU driver's license.

This is a source-only publication. A future binary distribution must identify
the dependency builds it actually uses and provide the notices, corresponding
source, or other materials required by their licenses. In particular, the
FFmpeg label above must not be treated as proof that an arbitrary host build is
LGPL-only: its configuration and enabled components matter.

## Implementation provenance

The source review and implementation records identify the loader, Objective-C
bridge, Foundation subset, services, graphics/audio adapters, assembly call
bridges, and fixtures as project implementations. Public API/ABI descriptions
and observations of an owner-supplied executable guided the compatibility
contracts. No vendored upstream loader, Objective-C runtime, BlocksRuntime,
cocos2d, or Box2D implementation was identified in this review.

### Darwin character classification

`prototype/darwin_libc.cpp` reconstructs the LP64 `_RuneLocale` memory layout
and character-category masks needed by guest imports. It generates its C-locale
table from ASCII ranges; it does not embed the upstream 256-entry source table
or copy the upstream classification function bodies. Its byte-range checks and
failure behavior are specific to this bridge.

The technical references are:

- [Apple Libc `runetype.h`](https://github.com/apple-oss-distributions/Libc/blob/main/include/runetype.h): layout reference; upstream copyright is held by the Regents of the University of California, with a four-clause Berkeley notice and a contribution credit to Paul Borman at Krystal Technologies.
- [Apple Libc `_ctype.h`](https://github.com/apple-oss-distributions/Libc/blob/main/include/_ctype.h): category bit values; upstream carries Apple Public Source License 2.0 and Berkeley/USL notices.
- [Apple Libc `locale/FreeBSD/table.c`](https://github.com/apple-oss-distributions/Libc/blob/main/locale/FreeBSD/table.c): default locale values used for comparison; upstream carries the Regents' three-clause Berkeley notice and Paul Borman contribution credit.

These are references to upstream files and their notices, not copies of those
files or a claim that their licenses were replaced by this repository's license.

### Other interface and format references

- Mach-O load commands and classic fixups: [Apple cctools `mach-o/loader.h`](https://github.com/apple-oss-distributions/cctools/blob/main/include/mach-o/loader.h). The header carries APSL-2.0; this project's bounded parser and fixup engine are separate implementations.
- Blocks layouts, helper callbacks, and forwarding cells: [Clang's Block ABI specification](https://clang.llvm.org/docs/Block-ABI-Apple.html), plus Apple/Swift [BlocksRuntime `runtime.c`](https://github.com/apple-oss-distributions/libdispatch/blob/main/src/BlocksRuntime/runtime.c) and [private layout definitions](https://github.com/apple-oss-distributions/libdispatch/blob/main/src/BlocksRuntime/Block_private.h). These upstream source files carry Apache-2.0 with the Runtime Library Exception. This project supplies its own limited implementation and tests.
- PNG filtering, including Paeth: [W3C PNG specification, filtering](https://www.w3.org/TR/png-3/#9Filters). The CgBI handling and framebuffer-copy shaders were written here; no libpng, stb, or other decoder source is vendored. DEFLATE is delegated to zlib.
- CAF chunk layout: [Apple CAF specification](https://developer.apple.com/library/archive/documentation/MusicAudio/Reference/CAFSpec/CAF_overview/CAF_overview.html). Chunk parsing is implemented here; compressed audio decoding is delegated to FFmpeg.
- Virtual-console blanking behavior: [Linux v4.9 `drivers/tty/vt/vt.c`](https://github.com/torvalds/linux/blob/v4.9/drivers/tty/vt/vt.c). `prototype/idle.cpp` calls the kernel's userspace interfaces; no kernel implementation was copied.

Selectors, symbol names, type encodings, numeric ABI constants, and isolated
reference addresses identify compatibility contracts. They are not bundled game
code or game content. Tests that inspect native metadata or decode game files
require owner-supplied inputs; those inputs and generated disassembly, images,
audio, and raw diagnostic logs are not part of this source publication.

Reference and dependency review: 2026-09-08. Links to moving upstream branches
are attribution references, not build pins or guarantees about every version.
