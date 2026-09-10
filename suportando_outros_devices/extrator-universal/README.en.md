# NXExtract

[![CI](https://github.com/NextOs-Ports/NXExtract/actions/workflows/ci.yml/badge.svg)](https://github.com/NextOs-Ports/NXExtract/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/NextOs-Ports/NXExtract)](https://github.com/NextOs-Ports/NXExtract/releases/latest)
[![MIT License](https://img.shields.io/badge/license-MIT-2ea44f.svg)](LICENSE)

NXExtract is a content-driven, transactional first-run data installer and bake
framework for Android-to-Linux game ports.

[Português](README.md) · [MIT license](LICENSE) ·
[Architecture](docs/ARCHITECTURE.md) ·
[Recipe reference](docs/RECIPE.md) ·
[Device matrix](docs/DEVICE-COMPATIBILITY.md)

## Download

Get the source from the [latest release](https://github.com/NextOs-Ports/NXExtract/releases/latest).
The release also provides a prebuilt
[`nxextract-ui-aarch64`](https://github.com/NextOs-Ports/NXExtract/releases/latest/download/nxextract-ui-aarch64)
requiring only GLIBC 2.17. The Python engine, generic launcher and recipe
example are in that release's source tree.

To develop or adapt NXExtract:

```bash
git clone https://github.com/NextOs-Ports/NXExtract.git
cd NXExtract
./tools/check-release.sh
```

## Real device UI

<p align="center">
  <img src="docs/images/nxextract-mali-1280x720.png"
       alt="NXExtract using the Mali backend at 1280 by 720" width="49%">
  <img src="docs/images/nxextract-kmsdrm-640x480.png"
       alt="NXExtract using the KMSDRM backend at 640 by 480" width="49%">
</p>

<p align="center">
  <sub>Real captures of the same binary: Mali/fbdev at 1280×720 and
  KMSDRM at 640×480. The displayed payload is entirely synthetic.</sub>
</p>

## Support and contribute

NXExtract is free software under MIT. Code, new-device testing, synthetic
recipes and documentation are welcome:

- 🐛 **Bugs and proposals**: [GitHub Issues](https://github.com/NextOs-Ports/NXExtract/issues);
- 🔀 **Code**: [Pull Requests](https://github.com/NextOs-Ports/NXExtract/pulls) and
  the [contribution guide](CONTRIBUTING.md);
- 💬 **Community**: [NextOS Discord](https://discord.gg/DHfY62eDNN).

External filenames are never game identifiers. A user may rename an APK,
APKM, APKS or XAPK freely; NXExtract inspects its contents, Android package,
splits, ABI, internal paths and validators.

## Supported inputs

- merged APK;
- loose split APK set;
- APKM, APKS and XAPK containers;
- companion ZIP archives;
- OBBs and validated loose files;
- extensionless ZIP/APK input in the primary `gamedata` directory.

Each port supplies a small trusted `extractor.json`. Game-specific texture
conversion or other baking is an optional hook. Adding a new port does not
require another extractor implementation.

## Safety model

- ZIP traversal, absolute paths, symlinks, encrypted members, duplicates and
  case-folding destination collisions are rejected.
- Different Android packages are never merged into one split set.
- Different matching payloads are rejected as ambiguous.
- Files copy into a same-filesystem resumable stage.
- Hooks write into the stage, never live data.
- Full validation completes before publication.
- A journal, backups, rename and fsync provide crash recovery and rollback.
- The user’s legal source package is never deleted.
- A validated marker makes the second run fast and source-independent.
- An atomic terminal JSON reports the final phase and stable support code
  without retaining the external package filename or origin.

## Port integration

Ship these files together:

```text
extractor.json
nxextract.py
nxextract-ui
nxextract-runtime-env.sh
run-extractor.sh
```

Then call NXExtract in the foreground:

```bash
GAMEDIR="/storage/roms/ports/my-port"
cd "$GAMEDIR"

./run-extractor.sh || exit 1
exec ./my-loader "$GAMEDIR"
```

On completion, `nxextract-result.json` follows the strict
[`terminal-result-schema-v1.json`](docs/terminal-result-schema-v1.json)
contract. `nxextract.log` keeps milestones, summarized misses and the terminal
cause; `nxextract-detail.log` keeps the per-file list and complete hook output.
For an explicit diagnostic run, `--verbose-log` or
`NXEXTRACT_VERBOSE_LOG=1` mirrors detail into the compact log too.

Do not stop the frontend, force a display driver, use `setsid`, or background
the extractor in a distributed launcher.

The packaged runner requires a private graphical attestation before source
scanning. The UI first tries inherited/default SDL and compatible advertised
backends. After a real failure, and only when no provider was explicitly
selected, it retries `libEGL.so`/`libGLESv2.so`; explicit provider choices are
never replaced. If a window remains impossible, it reuses the exact same
graphical renderer on a direct framebuffer software surface. The private
graphical proof has a fail-closed 40-second deadline. If neither renderer opens,
installation fails before owner data is examined or changed. An ASCII/TTY proof
is rejected and public ports have no UI opt-out.

The UI handshake never lives on disk at all. FAT, exFAT and some FUSE
filesystems report `0777` even after a requested `chmod`, and a login session
with `Linger=no` can recycle `XDG_RUNTIME_DIR` in the middle of a long
extraction. The engine therefore opens a private session channel — two pipes,
validated by owner, type, mode and dev/inode identity before the spawn — and
hands the UI only inherited descriptors (`fd:N`). Readiness and stop travel
through those descriptors, so removing any directory during extraction cannot
invalidate a data transaction that already validated. The diagnostic `ui.log`
remains persistent in the workspace but cannot authorize extraction, and
persistent game-filesystem state must not depend on POSIX private-mode
semantics.

`run-extractor.sh` automatically starts NXExtract inside a native child
environment. Firmware library directories precede inherited compatibility
paths, and every `LD_LIBRARY_PATH` entry inside `NXEXTRACT_GAME_DIR` is
removed. This prevents game-private SDL2/EGL/DRM libraries from interposing on
the UI. A valid inherited SDL backend remains unchanged; when none is set,
SDL remains free to autodetect.

A launcher may append known firmware or system-runtime directories only:

```bash
NXEXTRACT_FIRMWARE_LIBRARY_PATH="/opt/system/Tools/PortMaster/libs" \
  ./run-extractor.sh
```

The helper applies the same game-directory boundary to that override.

The default never changes inherited SDL drivers. Only when the launcher has
already proved those overrides invalid on the current device may it request
clean autodetection inside the child:

```bash
NXEXTRACT_SDL_AUTODETECT=1 ./run-extractor.sh
```

This opt-in only removes `SDL_VIDEODRIVER`, `SDL_VIDEO_DRIVER` and
`SDL_AUDIODRIVER` from the extractor process. It does not select a backend or
change the game's environment.

To replace an already valid payload from a newer source, pass
`--force-source`. The current live data remains in place until the new source
has been fully extracted and validated:

```bash
./run-extractor.sh --force-source --input gamedata/new-file.apk
```

When an expected internal path is present but fails size, hash, CRC or ELF
validation, the planner reports matched-but-rejected candidates and one
example. This separates a different game build from a genuinely missing file.

## Authoring and diagnostics

Start with `examples/recipe-minimal.json`:

```bash
python3 nxextract.py --version
python3 nxextract.py recipe-check --recipe extractor.json
python3 nxextract.py scan --game-dir .
python3 nxextract.py plan --recipe extractor.json --game-dir .
python3 nxextract.py install --recipe extractor.json --game-dir .
python3 nxextract.py verify --recipe extractor.json --game-dir .
```

See [the full recipe reference](docs/RECIPE.md) for source kinds, templates,
validators, commit roots and resumable bake hooks.

Set `elf_machine` to a literal Android ABI or to `{abi}` when one recipe must
validate the architecture currently being evaluated.

## Compatibility build

NXExtract’s Python core supports Python 3.7 or newer and uses only the standard
library. Every distributed Linux ELF in the kit must require GLIBC 2.30 or
older.

Build the UI:

```bash
./ui/tools/build-release.sh
```

The release build uses pinned Zig 0.16.0 and emits separate AArch64, ARMv7,
x86_64 and i386 artifacts. The canonical manifest pins each architecture,
mode, size and SHA-256:

```bash
./ui/tools/release-manifest.py --verify
```

All four current UI artifacts require at most GLIBC 2.17 and dynamically load
the SDL2 supplied by the target firmware.

## Tests

```bash
./tools/check-release.sh
./tools/check-release.sh --require-ui
```

The 64-test synthetic suite covers merged and renamed APKs, binary Android
manifests, loose splits, APKM/APKS/XAPK, OBB selection, ambiguity, traversal,
global Unicode/case collisions, linked filesystem attacks, resume, hooks and
fourteen power-loss publication boundaries plus mandatory visible-readiness
success/failure. Separate gates test the isolated
runtime, exact whole-kit pins and every ELF in the release tree. The source
release rejects APK, OBB and ZIP payload data.

A recipe that copies an APK as a `container` requires `input.packages` and
cannot carry external SHA-256, CRC32 or exact-size predicates in any
quantity (V3, NXA0001..NXA0003): container identity documents the tested
copy in `reference_build`, never decides acceptance. The contract combines
package identity with internal payload/tree/hook validation and accepts
compatible repackaging. Regressions
preserve the published Angry Birds, ScourgeBringer and Retro Highway recipe
shapes, while a synthetic repackaging fixture rejects a wrong package and an
incompatible internal library.

## License

NXExtract is released under the [MIT License](LICENSE). Anyone may use, fork,
modify and redistribute it under those terms. Android packages and game data
are not part of NXExtract and remain subject to their respective owners’
licenses.

See [CONTRIBUTING.md](CONTRIBUTING.md), the
[device validation matrix](docs/DEVICE-COMPATIBILITY.md),
[SECURITY.md](SECURITY.md) and [CHANGELOG.md](CHANGELOG.md).
