# Public development SDK

[Português](README.md)

This SDK builds Linux AArch64/ARMv7 code against older libraries and runs CPU tests. Its recipe uses public sources and requires no private NextOS image. Container host target: `linux/amd64`. Device GPU behavior remains outside this environment.

## 1. Build the environment

On Linux, prepare Git, Python 3.11 or newer, Docker and access to the private repository. Clone complete history; `--depth 1` may omit the pinned commit. Run from the root:

```sh
mkdir -p work
docker build --platform linux/amd64 -t nextos-public-sdk:1 toolchains/sdk
docker image inspect nextos-public-sdk:1 --format '{{.Id}}'
```

Building requires network access for public dependencies; execution below uses `--network none`. Record the image ID. Publishing the image to a registry is unnecessary. The first build compiles tools and takes longer; Docker reuses its layers afterward.

## 2. Understand the pins

[sources.json](sources.json) and [Dockerfile](Dockerfile) pin the Debian Buster digest, archived Debian/security index hashes, CMake 3.22.6 wheel, Git 2.45.2 source and Python 3.11.9. APT still validates package signatures and hashes. Disabling time validity applies to the historical archive and does not disable signatures.

Newer Git resolves the object format required by the V5 helper. Newer Python interprets AST literals used by recipe authority; Python 3.7 does not support that step. SDK Git is built for local operations without HTTP transport: clone/download sources on the host before mounting the checkout.

All package versions are recorded in `/opt/nextos-sdk-packages.tsv` inside the image. AArch64 SDL2/EGL/GLES2 headers/libraries belong to the SDK and are not copied into a port. Runtime uses firmware libraries.

## 3. Build and audit AArch64

```sh
docker run --rm --network none --user "$(id -u):$(id -g)"   -v "$PWD:/src:ro" -v "$PWD/work:/src/work"   -e NEXTOS_AARCH64_CC=/usr/bin/aarch64-linux-gnu-gcc   -e NEXTOS_SYSROOT=/ nextos-public-sdk:1 bash -c '
set -eu
cmake -S examples/shims-reference -B work/sdk-arm64   -DCMAKE_TOOLCHAIN_FILE=/src/toolchains/linux-aarch64.cmake
cmake --build work/sdk-arm64 --parallel 2
python3 tools/audit_elf.py work/sdk-arm64/shim-reference-nextos
'
```

Here `NEXTOS_SYSROOT=/` describes the **multiarch container's** root, with its cross compiler selecting ARM libraries. It does not mean using your computer's root or a card copy as an SDK. The tested example required GLIBC 2.17.

For ARMv7, use `NEXTOS_ARMV7_CC=/usr/bin/arm-linux-gnueabihf-gcc`, `toolchains/linux-armv7.cmake`, another build directory and `tools/audit_elf.py --machine ARM`. The tested ARMv7 example required GLIBC 2.4. This does not prove Android ARMv7 execution or a softfp bridge.

## 4. Continue to the first port

Follow the [integrated example](../../examples/first-port/README.en.md). It builds a freestanding Android guest with Clang, without libc/NDK, and the Linux loader with GCC. This small profile does not replace the NDK for Android projects needing its headers, libc or C++ runtimes.

References: [Debian archives](https://www.debian.org/distrib/archive.en.html), [official Debian image](https://hub.docker.com/_/debian), [CMake](https://cmake.org/), [Python sources](https://www.python.org/downloads/release/python-3119/). See [validation and limits](../../publication/ONBOARDING-UPDATE.en.md).
