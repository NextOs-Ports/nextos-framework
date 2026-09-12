# Ren'Py Android: Python, SDL and bootstrap

[Português](../pt-BR/RENPY-ANDROID.md)

This track starts from the Android version and studies the public [Summertime Saga](../../ports/summertimesaga-nextos/README.en.md) sources already selected in the collection. The goal is to run Android flow on Linux ARM with a dedicated adapter. First consult [runtime selection](ANDROID-RUNTIMES.md).

## 1. Identify the engine and complete data

```sh
python3 tools/inventory_apk.py /private/owner-input/game.apk --output work/renpy-inventory.json
python3 tools/find_reference.py --runtime renpy --abi arm64-v8a
```

Confirm `librenpython.so`, ABI, dependencies, embedded Python, Ren'Py version, standard library, native modules, scripts, archives and assets. Separate game version, runtime script version and ELF identity. The snapshot contains Ren'Py 8.5.3 sources; this does not certify any `librenpython.so` or Ren'Py game.

Do not automatically mix scripts/bytecode, extensions and interpreters from different versions. The recipe must start from the APK and all required splits. Copying a desktop edition directory or replacing the interpreter with host Python does not establish Android compatibility.

## 2. Read the exact reference

The [SOURCE-MAP](../../ports/summertimesaga-nextos/SOURCE-MAP.json) pins `bf5929bfdcdb03e17a27d053a91c36477b207032`. Read the [historical README](../../ports/summertimesaga-nextos/upstream/README.md), [license](../../ports/summertimesaga-nextos/upstream/LICENSE) and these files before adapting:

| Boundary | Selected source |
| --- | --- |
| Loader, SDL/JNI and entry | [src/main.c](../../ports/summertimesaga-nextos/upstream/src/main.c), [jni_shim.c](../../ports/summertimesaga-nextos/upstream/src/jni_shim.c) |
| Python bootstrap | [main.py](../../ports/summertimesaga-nextos/upstream/main.py), [android/apk.py](../../ports/summertimesaga-nextos/upstream/android/apk.py), [jnius](../../ports/summertimesaga-nextos/upstream/jnius/__init__.py) |
| Assets and transformation | [prepare_summertime_data.py](../../ports/summertimesaga-nextos/upstream/tools/prepare_summertime_data.py), [extractor.json](../../ports/summertimesaga-nextos/upstream/extractor.json) |
| Graphics provider and audio | [egl_shim.c](../../ports/summertimesaga-nextos/upstream/src/egl_shim.c), [audio_backend_policy.c](../../ports/summertimesaga-nextos/upstream/src/audio_backend_policy.c) |

Preserve per-file licenses, including Ren'Py and dependencies. The README describes historical results by target; they neither replace profile limits nor approve the new build.

## 3. Reproduce Android bootstrap

The selected `src/main.c` loads the Android ELF with appropriate imports, prepares JavaVM/JNI, registers SDL integration, supplies environment/dimensions and enters through `nativeRunMain`/`SDL_main`. `main.py` drives Ren'Py bootstrap. Map the actual build's constructors, `JNI_OnLoad`, callbacks and threads before calling them.

Ren'Py Android allows Java calls through Pyjnius and access to `PythonSDLActivity`. Inventory calls made by scripts after the initial menu too. [Official Android/Pyjnius documentation](https://www.renpy.org/doc/html/android.html#pyjnius). A replacement `jnius` module must implement the required signature, returned object and errors; an object accepting every method does not establish compatibility.

## 4. Separate layout, runtime and persistence

Historical preparation removes asset prefixes, builds a deterministic index and applies game-specific compatibility modules. Read the rules before reuse: do not indiscriminately strip prefixes from every name. Detect transformed-name collisions, invalid paths, missing required files and incompatible payloads before committing the transactional directory.

Keep pinned scripts/runtime separate from saves, preferences and user persistence. Test save/reload, rollback where used, text/accents, name entry and updates without losing progress. The two `runtime-overrides/` modules belong to Summertime Saga; they are not general patches for every Ren'Py game.

## 5. Build and diagnose

Use [ARM compilation](BUILD-ARM.md) and the [public SDK](../../toolchains/sdk/README.en.md) for the Linux loader. The [historical universal build](../../ports/summertimesaga-nextos/upstream/build_universal.sh) helps inventory sources, sysroot, libraries and omitted dependencies; it is not a guaranteed command in the selected clone. Check every public Linux ELF against GLIBC ≤ 2.30 and use firmware SDL. SDL embedded in the owner's Android ELF does not authorize redistributing private Linux SDL.

| Symptom | Next evidence |
| --- | --- |
| Import or bytecode error | Interpreter/module version, paths and input files; reject incompatible versions |
| Menu opens, scene fails | Complete assets, index, late imports and actual Java/Pyjnius calls |
| Live audio and black screen | Context/surface, shader, texture and pixels before present; invalidate black video |
| Sound disappears after pause | Negotiated backend, format/queue and resume; do not pin another device's configuration |
| Touch/cursor misses its target | Viewport, coordinate transform and Ren'Py consumer; test edges and resolution changes |

## 6. Install from scratch and record limits

Implement the [NXExtract](NXEXTRACT.md) recipe for complete Android input, package/version/ABI identity and critical payload hashes. Accept compatible repackaging without using the whole-container SHA as the sole condition. PT/EN `INSTALLATION.md` identifies the reference copy through technical fields and explains file destinations.

Preserve canonical extractor UI and NXSplash. Record real extraction, final hashes, graphics, audio, input, persistence and exit following [testing and delivery](TESTING.md). A host Python test does not approve the Android chain, Mali-450 rendering or another device. Specific changes remain in the adapter; V5 stays frozen.

## AI mission

```text
Inventory only the supplied Ren'Py Android input. Pin Python, Ren'Py, ELF,
modules and assets; read Summertime Saga at its selected commit.
Map SDL/JNI/Python bootstrap and Pyjnius calls, including late calls.
Implement contracts and transactional preparation in a separate project.
Test errors, persistence and complete extraction; record physical limits.
Do not mix desktop runtimes or include game data in the publication.
```
