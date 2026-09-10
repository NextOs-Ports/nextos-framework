# First port: an original NextOS training game

[Português](README.md)

This exercise connects an Android AArch64 guest to the real V5 `nxloader`. Its square, target and sound are generated in code. No commercial game is required. CPU execution and extraction were tested; physical graphics/audio/controllers remain unvalidated.

## 1. Prepare and build

Build the [public SDK](../../toolchains/sdk/README.en.md) first. Run from the collection root with a new output directory:

```sh
docker run --rm --network none --user "$(id -u):$(id -g)"   -v "$PWD:/src:ro" -v "$PWD/work:/src/work"   nextos-public-sdk:1 python3 examples/first-port/build.py   --output work/my-first-port
```

The script materializes collection pins, compiles `guest.c` for freestanding Android ARM64 and links `adapter.c` with static nxloader and system SDL2/GLES2. It audits GLIBC, produces an original input and generates a project with nxgenerator. Preserve logs and artifacts on failure. Do not erase evidence to present a repeated result as new.

This is not an Android-installable APK: `training-input.apk` is an exercise container with a manifest, original library and seed. The guest does not use complete Bionic, C++ or Java services. For ordinary Android projects, use the NDK as described in [Build ARM](../../docs/en/BUILD-ARM.md).

## 2. Understand the code and ordering

| File | Contract |
| --- | --- |
| [guest.c](guest.c) | Constructor, JNI_OnLoad, create/resume/step/render/audio/pause/save/destroy |
| [demo.h](demo.h) | Original ABI and only the JNI GetEnv/GetVersion slots actually used |
| [adapter.c](adapter.c) | Explicit registry, SDL2/GLES2, gamepad, PCM queue and persistence |
| [prepare_seed.py](prepare_seed.py) | Transactional hook converting `seed=7` to `7` |
| [build.py](build.py) | Build, byte identity and generation manifest |
| [test_pipeline.py](test_pipeline.py) | Positive and negative tests of the real flow |

Order: map → relocate → imports → protection → constructor → JNI_OnLoad → create → resume → frames/audio/input → pause → save → destroy. A missing mandatory import prevents the constructor. `GetEnv` accepts only the owner thread and declared version; other JNI functions are unimplemented. There is no JVM.

The registry exports explicitly implemented `__android_log_write` and `__errno`. Rendering produces RGBA; the adapter uploads/presents through GLES2 and rejects a black center pixel before present. That check fits this exercise's nonblack image and is not a universal detector. Audio is mono S16 PCM at 48 kHz. Pause/focus and hotplug are handled by the adapter but require physical evidence.

## 3. Test installation and CPU execution

```sh
docker run --rm --network none --user "$(id -u):$(id -g)"   -v "$PWD:/src:ro" -v "$PWD/work:/src/work"   nextos-public-sdk:1 python3 examples/first-port/test_pipeline.py   work/my-first-port
```

Expected result: `PASS: 11 integrated checks`. Tests perform clean extraction, the hook, output validation, AArch64 execution under QEMU, missing-import rejection before constructors, repacking acceptance, and rejection of wrong package/payload/ABI or missing payload. A failing hook preserves previously committed data and its marker.

Tests explicitly use `--ui none` **only in the host laboratory**. This does not certify graphical UI or approve a release. Read `pipeline-tests/RESULT.json` and individual logs. `COMPILED.json` binds native inputs and hashes to avoid rebuilding when only documentation preparation changes.

## 4. Inspect the output

```sh
python3 tools/inventory_apk.py work/my-first-port/training-input.apk   --output work/training-inventory.json
```

`generated/` contains the canonical launcher, NXSplash, graphical NXExtract and runtime members pinned by the generator. The guest and seed remain only in the original input. Do not modify that tree after generation. `runtime/` is a separate development installation for CPU tests, with bilingual INSTALLATION and exact input identity. `project/` holds full manifests and the recipe.

The generated adapter remains scaffold/nonrelease because no promotion or physical testing occurred. The [teaching implementation contract](adapter-contract.json) describes the implemented code. The example starts directly in gameplay; the schema-required `menu` section is reserved and has no menu test. Mapping is static; the example does not promise live GPTK editing.

## 5. Continue to hardware and your game

In a session with explicitly authorized hardware, validate graphical installation through the generated launcher, the five-second NXSplash, renderer, controls, audio, save/reload and shutdown. A headless installation is not evidence of graphical extraction. D-pad/left stick/arrows move the square; Start/Back/Escape saves and exits this exercise. These bindings are not defaults for other games.

The example is not a release ZIP and has not passed PortMaster/firmware gates. To publish a port, complete contracts, authored package documentation, required framework/runtime and final-byte evidence following [testing and delivery](../../docs/en/TESTING.md). Preserve UI, pins and frozen sources. Develop your own port in `work/ports/` or another repository.
