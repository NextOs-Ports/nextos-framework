# Choose the runtime for an Android game

[Português](../pt-BR/ANDROID-RUNTIMES.md)

This track always starts from an owner-supplied **Android** build and targets Linux ARM. Game examples come exclusively from public sources already selected in the [catalog](../../catalog/ports.json). These guides add instructions; they import no other ports or commercial data.

## 1. Identify before choosing

Run commands from the clone root with Python 3.11+, `readelf` and, for binary manifests, `aapt`. The [inventory guide](ANDROID-INVENTORY.md) explains separate inputs, limits and privacy. Replace the path and use a new output file:

```sh
python3 tools/inventory_apk.py /private/owner-input/game.apk --output work/android-inventory.json
python3 tools/find_reference.py --runtime gamemaker
python3 tools/find_reference.py --runtime renpy --abi arm64-v8a
python3 tools/find_reference.py --runtime haxe-lime --abi arm64-v8a
python3 tools/find_reference.py --runtime native-android --abi arm64-v8a
```

Search returns profiles with **Android source by default**, including PT/EN paths in `guides`. `--engine` searches names; `--runtime` selects the family even when the engine name remains `unknown`. ABI and renderer are additional filters, never evidence substitutes. Empty results call for contract investigation; they do not prove impossibility. `--platform all` explicitly searches the complete historical catalog, outside this track.

## 2. Follow the corresponding track

| Family for `--runtime` | Clues in the Android input | Guide and selected reference |
| --- | --- | --- |
| `unity` | `libunity.so`; Mono or IL2CPP/metadata | [Unity](../../portando_unity/README.en.md), version-specific cases |
| `mono-android` | Mono/.NET Android, assemblies and Java bootstrap | [Mono Android](MONO-ANDROID.md), Stardew Valley, ScourgeBringer, Blossom Tales; SOR4 for host adaptation |
| `godot` | Godot library, PCK/export and matching version | [Godot](GODOT.md), Tearscape |
| `cocos2d-x` | JNI/Cocos2d-x, scheduler and assets | [Cocos2d-x](COCOS2D-X.md), Chrono Trigger and Geometry Dash |
| `gamemaker` | `libyoyo.so`, runner and files it consumes | [GameMaker Android](GAMEMAKER-ANDROID.md), Forager |
| `renpy` | `librenpython.so`, Python/Ren'Py bootstrap and assets | [Ren'Py Android](RENPY-ANDROID.md), Summertime Saga |
| `haxe-lime` | `liblime.so` and `libApplicationMain.so` in the same ABI; confirm hxcpp | [Haxe/hxcpp/Lime Android](HAXE-LIME-ANDROID.md), Tightrope Theatre |
| `native-android` | C/C++ libraries, JNI, NativeActivity or Android SDL | [Android C/C++](NATIVE-ANDROID.md), Action Squad, KOTOR, Angry Birds and RCRDX |

MonoGame/FNA applies only when it belongs to the Android build being examined. Inventory does not identify every runtime: renamed libraries, contained assemblies and hybrid engines require further reading. `liblime.so` alone does not establish an hxcpp application; missing known signatures does not establish C/C++. A predominantly Java/Kotlin APK requires runtime investigation; these guides demonstrate no generic ART support.

## 3. Understand what classification proves

[Profiles](../../catalog/profiles.json) separate `platform`, `runtime_family`, engine, version, ABI, renderer and status. New classifications provide `classification_evidence` with a snapshot path and SHA-256. The [track registry](../../catalog/android-runtimes.json) supplies bilingual links. There are 38 Android source profiles; community cards without imported source do not enter search.

Open each result's `SOURCE-MAP.json` and check commit, included/omitted files and license. A reference may contain pins older or newer than V5 and partial validation. The selected Tightrope source is `1.0.5-test.1`, with exact physical acceptance pending: use it to study classification and diagnose limits, not as an approved baseline. Do not promote its workaround to a default.

## 4. Implement within V5 scope

Use the [SDK](../../toolchains/sdk/README.en.md), [pins](PINS-AND-SOURCES.md), [original first port](../../examples/first-port/README.en.md) and a separate adapter project. Prefer AArch64 when the Android build offers it. Do not confuse a Linux wrapper's CPU with guest ABI. For ARMv7, prove the softfp/ARMHF bridge.

V5 remains at `framework-v5`, commit `657fb65a23b5c3b20040e76307b27e6470b1d17c`. Check loader contracts before integrating a reference. A V6-only feature does not become available because its snapshot appears in the catalog; record the incompatible dependency. This documentation adds no shared runtime.

## 5. Finish the new port

Preserve native order and keep specific classes, callbacks and adaptations in the adapter. Use system SDL and audit every public Linux ELF for GLIBC ≤ 2.30. [NXExtract](NXEXTRACT.md) and [testing and delivery](TESTING.md) describe clean installation, owner data, bilingual `INSTALLATION.md` and evidence for the same final artifact.

NXExtract/NXSplash screens remain canonical. Graphics, audio, controls, save/reload and exit need appropriate evidence; reference search and host tests do not approve a device. Preserve approved bytes and gather required checks before an authorized physical launch.
