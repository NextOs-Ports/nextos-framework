# Build and targeted tests

The repository contains bridge source and synthetic host fixtures. It does not
contain the IPA, the original Mach-O executable, game assets, private device
logs, or a prebuilt Linux executable. Host tests do not prove physical gameplay.

## Target build

Use an AArch64 Linux C++17 toolchain whose sysroot matches the intended device.
The sysroot needs the development files for SDL2, zlib, libpng16, FreeType,
FFmpeg (`avformat`, `avcodec`, `avutil`, `swresample`), EGL/GLES2 and OpenAL.
SDL2 must come from the target system; the project does not vendor a private SDL.

```sh
make -C prototype TOOLCHAIN=/path/to/NextOS/toolchain
# Or select another compatible cross compiler explicitly:
make -C prototype CXX=/path/to/aarch64-linux-gnu-g++
```

`TOOLCHAIN` selects `bin/aarch64-libreelec-linux-gnu-g++` below that directory.
An explicit `CXX` takes precedence. `SYSROOT` always comes from the compiler's
`-print-sysroot`; a conflicting override is rejected to avoid mixing headers
with a different link sysroot. Select a compiler configured for the target.
GNU make's implicit native `g++` is rejected; a host compiler
cannot assemble the AArch64 bridge. Compiler paths should identify an executable;
set compilation options separately, not inside a quoted executable pathname.

The output is `prototype/build/goblinsword-ios-nextos`. The Makefile retains the
validated optimization settings: C++17, `-O0`, debug symbols and frame pointers,
with only `runtime.cpp` and `graphics.cpp` built at `-O2 -fno-strict-aliasing`.
There is no LTO or fast-math. A rebuild creates new bytes and needs its own
physical validation; matching source and flags alone do not certify a new binary.

For configuration review without building:

```sh
make -n -C prototype TOOLCHAIN=/path/to/NextOS/toolchain
```

This prints the commands and queries the compiler's sysroot. It does not compile
or relink the executable.

## Four fixtures without game data

These checks require Linux, Bash, a C++17 host compiler and GNU-compatible linker.
The controller checks additionally need `pkg-config` and system SDL2 development
files, version 2.0.14 or later, for the virtual-controller API.

```sh
bash tools/run-host-tests.sh
# Select individual components during development:
bash tools/run-host-tests.sh idle rune
```

| Fixture | Boundary tested |
| --- | --- |
| `idle` | Mocked VT timer disable/restore; never opens a real console |
| `rune` | Darwin character classification layout against the host C locale |
| `services-events` | Real system SDL2 virtual input, edge order and callbacks |
| `exit-chord` | SELECT+START, subprocess-only exit and registered cleanup |

`HOST_CXX` selects the host compiler, independently of the cross-build `CXX`.
`GOBLIN_HOST_TEST_BUILD` selects the build output directory; its default is
`build/host-tests`. Each compile and run prints progress. Expected negative
subprocess cases may print failure diagnostics; the fixture's final result and
exit status determine whether the check passed. Core dumps are disabled by the
runner. No display, audio output, target device or game executable is used by
this subset. Controller fixtures should run on a development host without
unrelated game sessions competing for SDL input.

## Tests requiring owner data

Prepare your own compatible data as described in the main README. The following
fixtures map original ARM64 metadata and use synthetic host callbacks; they
explicitly avoid executing guest instructions. Each requires the Mach-O path:

```sh
bash tools/test_prototype_host.sh runtime --macho data/goblin-sword.macho
bash tools/test_protocol_host.sh --macho data/goblin-sword.macho
bash tools/test_invocation_host.sh --macho data/goblin-sword.macho
```

The older standalone services check needs only the host compiler and system
SDL2 headers/library: `bash tools/test_prototype_host.sh services`.

Other fixtures are retained as focused development sources, not silently run
as part of the four-test subset:

| Source | Additional requirements / inputs |
| --- | --- |
| `host_byref_fixture.cpp` | Clang with `-fblocks`; runtime fixture stubs, loader and Darwin libc linkage |
| `host_mainqueue_fixture.cpp` | Clang sanitizers/LSan headers; runtime fixture stubs, loader and Darwin libc linkage |
| `host_diagnostic_input_fixture.cpp` | System SDL2 virtual controller; private temporary FIFO |
| `benchmark_runtime_host.cpp` | Runtime fixture stubs, loader and Darwin libc linkage; synthetic CPU timings only |
| `test_graphics_sample_host.cpp` | EGL/GLES2, PNG and FreeType headers; mocked GL readback; no real display |
| `test_graphics_host.cpp` | Graphics development headers/libraries and one or more owner-supplied PNG paths, including CgBI where applicable |
| `test_audio_decode_host.cpp` | FFmpeg and OpenAL development files; one or more owner-supplied CAF paths |
| `test_audio_loopback_host.cpp` | The same audio dependencies plus system OpenAL Soft loopback; one CAF path |
| `test_foundation_data.cpp` | Synthetic strings/collections/defaults modes, or plist/XML bytes on stdin; no IPA dependency for synthetic modes |

PNG/CAF decode fixtures must receive actual input paths; an invocation with no
files is not asset validation. Sources that include an entire bridge `.cpp`
need its dependencies or section garbage collection (`-ffunction-sections
-fdata-sections -Wl,--gc-sections`) when only isolated functions are tested.
Do not execute ARM64 guest entrypoints to make an x86 host fixture pass.

For the Foundation parser comparison, compile the harness and pass its path
explicitly; the Python driver reads assets directly from the owner-supplied IPA:

```sh
mkdir -p build/host-tests
g++ -std=c++17 -O2 -Iprototype tools/test_foundation_data.cpp -o build/host-tests/foundation-data
python3 tools/test_foundation_data.py /path/to/owner.ipa --harness "$PWD/build/host-tests/foundation-data"
```

`--scratch-parent DIRECTORY` chooses an existing directory for temporary defaults
tests. Otherwise Python uses the system temporary directory. Reports and data
produced from an IPA remain local and must not be committed to the source repo.

## Read-only inspection tools

Python analysis tools need Python 3 and the `capstone` package, except
`inspect_ipa.py`, which uses the standard library. They accept explicit inputs:

```sh
python3 tools/inspect_runtime.py data/goblin-sword.macho
python3 tools/disassemble.py 0x100161ad8 --bytes 128 --macho data/goblin-sword.macho
python3 tools/import_callers.py --macho data/goblin-sword.macho
python3 tools/inspect_rune_xrefs.py --macho data/goblin-sword.macho
python3 tools/inspect_ipa.py /path/to/owner.ipa --output /path/to/new-private-inspection
```

The IPA inspector writes inventory plus a copy of the original executable into
the new output directory. Its output is private owner data, not redistributable
source. None of these scripts accepts a device IP or starts a remote session.
