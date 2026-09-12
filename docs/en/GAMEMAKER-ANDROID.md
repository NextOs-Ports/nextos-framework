# GameMaker Android and the YoYo runner

[Português](../pt-BR/GAMEMAKER-ANDROID.md)

This track adapts the Android build to Linux ARM while retaining its runner and owner-supplied data. Start with [runtime selection](ANDROID-RUNTIMES.md). The reference is the public [Forager](../../ports/forager-nextos/README.en.md) port already included in this collection.

## 1. Inventory the Android build

```sh
python3 tools/inventory_apk.py /private/owner-input/game.apk --output work/gamemaker-inventory.json
python3 tools/find_reference.py --runtime gamemaker
```

Confirm package ID, game version, ABIs, `libyoyo.so`, C++ dependencies, data, texture pages, audio and Java/JNI extensions. Record the runner version separately. GameMaker can compile Android with VM or YYC; identify the owner's variant before choosing an implementation. [Official Android compilation documentation](https://gamemaker.io/en/help/articles/android-compiling-your-app).

Forager 1.0.13 is the reference's Android ARMv7 profile. That number is the game version. Do not extrapolate to another runner or choose ARMv7 for a different game offering `arm64-v8a`. Desktop `data.win` is not a substitute input for this Android recipe.

## 2. Pin sources and licensing

The [SOURCE-MAP](../../ports/forager-nextos/SOURCE-MAP.json) pins `89e5c7107889f6b19a705f006f9f492bb0eb80da`. Read:

- [Bootstrap and limits](../../ports/forager-nextos/upstream/README.md).
- [Runner boundary](../../ports/forager-nextos/upstream/source/overlay/gmloader/libyoyo.cpp) and [RunnerJNILib](../../ports/forager-nextos/upstream/source/overlay/gmloader/classes/RunnerJNILib.cpp).
- [Historical build](../../ports/forager-nextos/upstream/source/build-release.sh), [data preparation](../../ports/forager-nextos/upstream/tools/build_forager_port.py) and [NXExtract recipe](../../ports/forager-nextos/upstream/extractor.json).

The [NOTICE](../../ports/forager-nextos/upstream/NOTICE.md) declares a loader derived from gmloader-next at `c2fca354df73761887c15f44a0b28ec823581cd5`, under GPL-2.0-only. The collection includes GPL-3.0-only code: keep this boundary explicit and do not paste the loader into V5 components as though it had the same license. Preserve notices and assess composition in a separate project using [collection licensing](../../LICENSING.en.md). Original runner and assets come from the owner.

## 3. Preserve runner flow

In the reference, NXExtract prepares `runtime/forager.port`; the loader loads `libc++_shared.so` before `libyoyo.so`, resolves imports and preserves initializers, JNI and the native loop. Map the new build's exact sequence: Java classes, RunnerJNILib calls, surface, dimensions, language, audio, controllers, pause and shutdown.

A `.port` file is a historical adapter choice, not a universal GameMaker or V5 API. Do not call a room, game function or intermediate entrypoint to bypass initialization. Optional extensions need an absence contract and real errors; pretending a required service succeeded hides later failures.

## 4. Build the adapter and prove ABI

Use [ARM compilation](BUILD-ARM.md) to produce the Linux executable in its own project. Read the historical build before running it: recovered sources, image/sysroot, libraries and omitted files need pins. Its old nxbootstrap/NXExtract use does not migrate the reference to V5. Do not rebuild an approved binary to write a guide.

For ARMv7, check ARM/Thumb, calling conventions, doubles/floats, callbacks and structures across Android softfp/Linux ARMHF. Translation must cover the return path, not only imports. C++ requires ownership, exceptions and standard-library version checks; do not pass objects across ABIs without a contract. Unknown imports need verifiable errors, never a generic `return 0` table.

## 5. Diagnose game boundaries

| Symptom | Measurement and negative case |
| --- | --- |
| Failure before first frame | Library order, constructors, JNI and first unresolved call; reject the wrong library/ABI |
| Missing sprites or black surfaces | Actual shader, texture, alpha, FBO and pixels before present; audio without a frame must fail |
| Some buttons do not respond | Mask, press/release and device consumed by the runner; reconnect without duplicate events |
| Language/save disappears after restart | Persistent path outside extracted data; save, exit and reload without destructive extraction |
| Audio breaks after pause | Format, queue, callback and resume; establish consumption and listen on the target |

Use system SDL and the actual GLES provider. Forager's extension, texture or input adjustments remain specific until proven in another port. Pixel, save and audio criteria are in [testing and delivery](TESTING.md).

## 6. Prepare data and delivery

The reference creates a deterministic STORE archive with 24 members. Those members and hashes belong to that build; a new port needs its own inventory. [NXExtract](NXEXTRACT.md) must validate Android identity and critical payloads, prepare the complete layout and execute hooks during clean installation. The reference APK SHA identifies the test without becoming the sole container compatibility gate.

Include PT/EN `INSTALLATION.md` with technical input identity and keep saves outside the data seal. Audit every public Linux ELF for GLIBC ≤ 2.30; preserve the V5 launcher, graphical UI and five-second NXSplash. Forager's historical evidence does not approve another runner, device or rebuilt bytes.

## AI mission

```text
Analyze only the supplied Android build. Identify the GameMaker runner,
VM/YYC, ABI, assets and extensions. Read pinned Forager sources and licensing.
Document native flow and implement a separate adapter respecting V5.
Prove ABI, graphics, audio, input and persistence with targeted checks.
Prepare a complete NXExtract recipe and record incompatible dependencies.
Import no commercial data or historical loader as a V5 default.
```
