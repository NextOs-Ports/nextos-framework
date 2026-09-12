# Native Android C/C++: JNI, NativeActivity and SDL

[Português](../pt-BR/NATIVE-ANDROID.md)

This track covers native **Android** game libraries, including custom engines, whose adapter will run on Linux ARM. Use [runtime selection](ANDROID-RUNTIMES.md) to identify a specialized track first. Absence of Unity, Godot or Mono does not establish that the remaining game needs no Java.

## 1. Inventory libraries and bootstrap

```sh
python3 tools/inventory_apk.py /private/owner-input/game.apk --output work/native-inventory.json
python3 tools/find_reference.py --runtime native-android --abi arm64-v8a
python3 tools/find_reference.py --runtime native-android --abi armeabi-v7a
```

Record package/version, guest ABIs, ELFs and hashes, `DT_NEEDED`, imports, relocations, TLS, constructors, Java classes and Android services used. [Inventory](ANDROID-INVENTORY.md) does not execute DEX or discover every JNI/dlsym call. Read the Activity and callbacks in the authorized local copy to complete the map.

Prefer AArch64 when available; a 64-bit Linux wrapper does not turn an ARMv7/x86 guest into AArch64. Unknown engine names remain `unknown` in the catalog even when the native family is established. Hybrid engines, such as Off The Road with xGen/Horde3D+bgfx over a Cocos2d-x shell, require separating initialization from rendering.

## 2. Choose a reference by contract

All references below are already selected public sources in this collection. Open manifest, README and license before reuse; historical pins are not a V5 migration.

| Source | Selected commit | Boundary to study |
| --- | --- | --- |
| [Action Squad](../../ports/actionsquad-nextos/README.en.md) · [SOURCE-MAP](../../ports/actionsquad-nextos/SOURCE-MAP.json) | `842efc61a47192cd3ac4d3062dbe9c32e808c791` | AArch64, NativeActivity and input queue |
| [KOTOR](../../ports/kotor-nextos/README.en.md) · [SOURCE-MAP](../../ports/kotor-nextos/SOURCE-MAP.json) | `9d8dc0cdd3cf164a39f480200ab20a9a1117b91b` | ARMv7, Odyssey/SDL2, OBB and engine context |
| [Angry Birds](../../ports/angrybirds-nextos/README.en.md) · [SOURCE-MAP](../../ports/angrybirds-nextos/SOURCE-MAP.json) | `74af40a86d09126e5e2c3b220102411c44407fd5` | ARMv7, Rovio Fusion, JNI and mixer |
| [RCRDX](../../ports/rcrdx-nextos/README.en.md) · [SOURCE-MAP](../../ports/rcrdx-nextos/SOURCE-MAP.json) | `7d79b145f9c271ac5bbf959fb254880482ac99a1` | AArch64, surface callbacks, `nativeInit` and `SDL_main` |

Start with [Action Squad main](../../ports/actionsquad-nextos/upstream/src/main.c), [KOTOR main](../../ports/kotor-nextos/upstream/src/main.c), [Angry Birds main](../../ports/angrybirds-nextos/upstream/src/main_angrybirds.c) or [RCRDX main](../../ports/rcrdx-nextos/upstream/src/main.c). They represent different flows; there is no universal entrypoint list covering all four.

## 3. Distinguish entry paths

| Observed path | Required contract |
| --- | --- |
| Java Activity with JNI methods | Classes, signatures, `JNI_OnLoad`/registration and actual callbacks |
| NativeActivity | `ANativeActivity` structure/callbacks, lifecycle, window, input and saved state |
| `android_native_app_glue` | App thread, commands, looper, synchronization and the build's `android_main` entry |
| Android SDL | Activity/audio/controller JNI, surface/dimensions, `nativeRunMain` thread and SDL loop |

NDK documentation describes `ANativeActivity_onCreate` and Activity callback registration. Reconstruct the observed contract before supplying structures or dispatching events. [NDK concepts](https://developer.android.com/ndk/guides/concepts). Do not replace native order with a direct call into the game.

JNI also requires local/global references, exceptions, strings/arrays, signatures and thread attachment. `JNIEnv` is thread-specific; sharing it across threads breaks that contract. [Official JNI guidance](https://developer.android.com/ndk/guides/jni-tips). Model late callbacks, pause, resume and shutdown too.

## 4. Implement the Bionic/Linux bridge

Use [shims](SHIMS.md), V5 contracts and the [integrated original example](../../examples/first-port/README.en.md). The Linux loader and Android libraries belong to different ABI environments. Do not indiscriminately use glibc `dlopen` to load a Bionic ELF.

Record signature, type, size/alignment, ownership, thread, error value and test for each boundary: libc/libm, C++, pthread/TLS, `errno`, assets, JNI/NDK, audio and EGL/GLES. For ARMv7, prove softfp/hard-float calls, including callbacks. Unknown required imports, TLS symbols or relocations unsupported by V5 need explicit diagnostics; do not “resolve” everything with empty function addresses.

Build in a separate project with the [public SDK](../../toolchains/sdk/README.en.md). Check dependencies and omitted files before using historical scripts. An API available only in V6 is an incompatibility to record; do not change V5 components or automatically migrate references.

## 5. Verify graphics, audio and input

| Boundary | Evidence and negative case |
| --- | --- |
| EGL/GLES | Single window/context/present owner, actual dimensions and pixels before present; dead contexts or black video fail |
| Older GLES | Identify GLES1/1.1 and fixed-function matrices before adapting to GLES2; renaming imports does not turn Swordigo/FF4 into GLES2 |
| Audio | Format, channels, queue, callback and consumption for OpenSL, AudioTrack or native mixer; pause/resume and listening |
| Input | Queue/callback consumed by the game, press/release, axes, focus and lifecycle; an isolated SDL event is insufficient |
| Exit and saves | Native pause, flush/save and shutdown order; reload progress without leftover processes |

Use system SDL. Do not embed private SDL to copy a historical solution. A touch cursor needs correct coordinates, a polished arrow and separate menu/gameplay behavior; do not invent global actions on game buttons.

## 6. Prepare extraction and freeze the bytes

Create the [NXExtract](NXEXTRACT.md) recipe for the owner's complete APK/splits/OBB. Validate Android identity and critical payloads, preserve compatible repackaging and execute hooks during a truly clean installation. Do not distribute original libraries or assets. Include PT/EN `INSTALLATION.md` with technical reference-copy identity, layout and instructions.

Audit every public Linux ELF for GLIBC ≤ 2.30; preserve the V5 launcher, NXExtract UI and NXSplash. Follow [testing and delivery](TESTING.md) to freeze the proven executable and bind delivery to the same bytes. This update has not revalidated these games, and every new target needs its own authorized evidence.

## AI mission

```text
Analyze only the supplied Android game and choose already selected public
references by bootstrap, ABI, renderer, audio and input contracts.
Map constructors, JNI/NativeActivity/SDL and threads without skipping stages.
Implement a separate adapter with real errors and ABI/ownership tests.
Prepare complete NXExtract extraction, persistence and byte-linked evidence.
Preserve V5 and historical sources; record remaining incompatible features.
```
