# Build for ARM and AArch64

[Português](../pt-BR/COMPILAR-ARM.md)

The goal is to identify the compiler for each component, produce an ELF with the correct ABI, and interpret its requirements. The host example has been tested; the cross commands below require a toolchain/sysroot you supply and verify. This collection does not yet distribute a certified public low-glibc environment.

## 1. Understand the three builds

| Product | Build environment | Runs on |
| --- | --- | --- |
| C contract test | Computer's native compiler | Linux host |
| `<port-id>-nextos` loader/adapter | Linux cross compiler and sysroot | Device's ARM Linux |
| Original Android library example | NDK and Bionic sysroot | Android or compatible Android loader |

A commercial library is already compiled inside the APK; creating a port does not rebuild it. Prefer `arm64-v8a` when available. `armeabi-v7a` requires an ARM32 process and may need a calling-convention bridge to an ARMHF host. No flag converts ELF32 into ELF64. Android ARMv7 uses `softfp` for floating-point argument passing, whereas the Linux ARMHF interface may use VFP registers. [Android ABI documentation](https://developer.android.com/ndk/guides/abis).

## 2. Check the host example first

```sh
cmake -S examples/shims-reference -B work/host -DCMAKE_BUILD_TYPE=Release
cmake --build work/host --parallel 2
ctest --test-dir work/host --output-on-failure
```

This isolates example and basic tooling errors before cross-compilation. The resulting binary has your computer's architecture, regardless of whether the directory is named `work/host` or the executable ends with `nextos`.

## 3. Prepare a consistent Linux toolchain

The sysroot needs headers, link startup files, libc and development libraries for the same architecture and target distribution. An incomplete copy of a device card usually lacks development files. Record the SDK/sysroot origin, version and digest; a private image name is not a public installation recipe.

The `/opt/...` paths below are examples to replace with real locations. The script does not download or install tools. Public releases must require no more than `GLIBC_2.30`; using recent GCC and recent libraries does not produce older compatibility by changing the manifest text.

## 4. Build for AArch64

```sh
export NEXTOS_AARCH64_CC=/opt/nextos-toolchain/bin/aarch64-linux-gnu-gcc
export NEXTOS_SYSROOT=/opt/nextos-sysroots/aarch64
test -x "$NEXTOS_AARCH64_CC"
test -d "$NEXTOS_SYSROOT"
"$NEXTOS_AARCH64_CC" --version
"$NEXTOS_AARCH64_CC" -dumpmachine
"$NEXTOS_AARCH64_CC" --sysroot="$NEXTOS_SYSROOT" -print-sysroot
cmake -S examples/shims-reference -B work/aarch64 \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchains/linux-aarch64.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build work/aarch64 --parallel 2
```

[linux-aarch64.cmake](../../toolchains/linux-aarch64.cmake) selects Linux/AArch64, the explicit compiler and sysroot; it finds libraries/headers on the target and build programs on the host. That is the purpose of these [CMake toolchain](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html) search settings. The example uses C; C++ projects also need a matching C++ compiler and libraries in their own toolchain file.

Use a fresh build directory when changing ABI, compiler or sysroot. CMake's cache may retain the old compiler. Do not combine ARM headers with x86 libraries or bundle a private SDL to work around detection failures.

## 5. Audit before execution

```sh
readelf -h work/aarch64/shim-reference-nextos
readelf -l work/aarch64/shim-reference-nextos
readelf -d work/aarch64/shim-reference-nextos
readelf --version-info work/aarch64/shim-reference-nextos
sha256sum work/aarch64/shim-reference-nextos
```

| Output | Check |
| --- | --- |
| `-h` | ELF64 and `Machine: AArch64`; filenames do not prove architecture |
| `-l` | A Linux interpreter available on the target, if `INTERP` exists |
| `-d` | Every `NEEDED` dependency and any RPATH/RUNPATH |
| `--version-info` | Required GLIBC versions; C++ also needs GLIBCXX/CXXABI review |
| SHA-256 | Identity of the bytes you will test |

Audit **every** shipped Linux ELF, including helpers and libraries. `GLIBC_2.30` is a publication ceiling, not sufficient proof of compatibility: architecture, kernel, SDL, EGL and audio still matter. Do not use `ldd` to analyze an unknown Android ELF through execution; inspect its static information first.

Do not run this ELF on x86 as a native program. This CMake project does not register CTest tests during cross-compilation: “no tests found” does not mean ARM passed. QEMU execution, when explicitly prepared, must be reported as CPU emulation. Physical GPU/audio/input tests belong to the [validation track](TESTING.md).

## 6. Use ARMv7 when necessary

```sh
export NEXTOS_ARMV7_CC=/opt/nextos-toolchain/bin/arm-linux-gnueabihf-gcc
export NEXTOS_SYSROOT=/opt/nextos-sysroots/armhf
test -x "$NEXTOS_ARMV7_CC"
test -d "$NEXTOS_SYSROOT"
"$NEXTOS_ARMV7_CC" -dumpmachine
cmake -S examples/shims-reference -B work/armv7 \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchains/linux-armv7.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build work/armv7 --parallel 2
readelf -h -A work/armv7/shim-reference-nextos
```

Check ELF32/ARM and calling attributes. An ARMHF loader and Android ARMv7 guest may share a CPU without sharing every function ABI. Read `framework/nxloader/include/nxloader_softfp.h` before adapting callbacks with `float`/`double`; structure layouts and `pthread` also require ABI-specific review.

## 7. Build an original Android library

This exercise is separate from the Linux example and teaches only the NDK build. `nextos_guest_add` accepts values whose sum fits in `int`. It does not demonstrate JNI, loader relocations or guest execution on a handheld.

```sh
mkdir -p work/android-guest
cat > work/android-guest/CMakeLists.txt <<'CMAKE'
cmake_minimum_required(VERSION 3.16)
project(nextos_android_guest LANGUAGES C)
add_library(nextos_guest SHARED guest.c)
CMAKE
cat > work/android-guest/guest.c <<'C'
int nextos_guest_add(int a, int b) { return a + b; }
C
export NEXTOS_NDK=/opt/android-ndk
test -f "$NEXTOS_NDK/build/cmake/android.toolchain.cmake"
cmake -S work/android-guest -B work/android-arm64 \
  -DCMAKE_TOOLCHAIN_FILE="$NEXTOS_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build work/android-arm64 --parallel 2
readelf -h -d work/android-arm64/libnextos_guest.so
```

Use a version-identified NDK installation. API 21 is used for this ARM64 exercise; another project's API level must follow its actual APIs. `ANDROID_ABI`, `ANDROID_PLATFORM` and `android.toolchain.cmake` follow the [NDK CMake guide](https://developer.android.com/ndk/guides/cmake). Do not use that toolchain for the Linux loader.

## 8. Apply this to a real port

Read the reference recipe before executing any script. Identify missing inputs, external tools, flags, libraries and generated files. Write the new port's recipe with verifiable pins and its own output directory. Record the full command, compiler version, sysroot, commit and final SHA. A successful build still does not establish extraction, runtime behavior or installation.

For `crt1.o`, `cannot find -lc`, “wrong format”, “Exec format error” and GLIBC failures, see [troubleshooting](TROUBLESHOOTING.md). Do not remove an audit to hide an incompatibility.
