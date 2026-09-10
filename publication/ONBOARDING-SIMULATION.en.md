# First-port onboarding simulation

[Português](ONBOARDING-SIMULATION.md)

**Historical record:** the gaps below were observed at the stated commit. The [subsequent update](ONBOARDING-UPDATE.en.md) records the implemented SDK, examples and tools, their checks and current open work.

Assessment dated 2026-09-10 against commit `2e7234265f8602d8a31117f65a23874769e13e59`. Scenario: a person or AI receives only this collection and their own Android game copy, without knowing the internal NextOS environment.

**Conclusion: the collection supports initial investigation and compiling the host example. A complete reproducible path from clone to a demonstrable Android port is still missing.** The reference material is useful, but the AI must still discover and implement the connections between several steps. “Complete guides” described the editorial objective with excessive confidence.

## 1. What was actually simulated

A clean export of tracked files, created with `git archive` under `work/`, was used to test the initial commands. It contains none of the original workspace's ignored files or builds. The existing Linux computer with GCC 16.2.1 was used; this was neither a freshly installed container nor an ARM machine. Pin tests used the collection repository's own Git objects.

| Step | Observed result | Scope |
| --- | --- | --- |
| `publication/verify.py` | PASS: 7,725 hashes | Selection integrity |
| CMake/build/CTest for `examples/shims-reference` | PASS: 1/1 | C example on the host |
| Generator with `nxproject-aarch64.example.json` | Output directory generated successfully | Project skeleton |
| `recipe-check` with `recipe-minimal.json` | OK | Recipe structure |
| `nxloader` pin using the historical V5 commit | Failed: `Git object query failed: fatal: Needed a single revision` | Historical object unavailable in this collection |
| `nxloader` pin using the collection commit above | Create/materialize/verify PASS | One component, without build or release |

The generated adapter contains `status: unimplemented_nonrelease`, `release_ready: false`, an empty lifecycle, and no JNI methods, audio callbacks or input actions. The declared game executable was not created. This matches the generator's intended contract; the next exercise that implements this skeleton is missing.

No commercial APK, game, device, ARM/NDK build, complete extraction, historical suite or release packaging was executed. No V5 runtime or port snapshot was modified.

## 2. Priority blockers

| Priority | Where a newcomer stops | Evidence and required material |
| --- | --- | --- |
| P0 | Obtain a reproducible ARM environment | [Build ARM](../docs/en/BUILD-ARM.md) requires an external compiler/sysroot and uses illustrative `/opt/...` paths. A public recipe with versions, origin, hashes, C/C++/SDL/EGL dependencies and a low-glibc check is missing. |
| P0 | Pin components from this new repository | The [pin helper](../framework/nxgenerator/framework_pin.py) requires local Git objects. The original V5 SHA identifies provenance but does not exist in the new history. Document and validate a complete composition from the exported commit while preserving original provenance separately. |
| P0 | Move from the C example to an Android guest | [shims-reference](../examples/shims-reference/README.en.md) does not load Android ELF. The NDK exercise builds an addition function but does not show how the Linux loader loads it, resolves imports and calls it. An integrated demonstration project is missing. |
| P0 | Install example data through NXExtract | The minimal recipe passes the parser but has no accompanying reproducible original input and extraction/hooks/validation/rollback tutorial. Connect that recipe to the same integrated example. |
| P1 | Build the selected reference | The selection is not a complete checkout of each port. The [Chrono build](../ports/chrono-nextos/upstream/build_universal.sh) requires the local `playfetch-builder:buster` image, headers from a NextOS build and `fonts/NotoSans-Regular.ttf`, which is absent from the selection. The [historical FP2 README](../ports/fp2-nextos/upstream/README.md) instructs running `build_universal.sh`, also absent. List missing dependencies/files explicitly and explain how to recover each permitted public source. |

Do not recreate or move the historical V5 tag to solve the pin issue. The successful exported-commit experiment proves that a route exists with the current helper, but only `nxloader` was materialized in this assessment. Test components had omissions; do not describe the whole exported composition as identical to the complete historical tree.

## 3. Guides and examples still needed

1. **Executable Android inventory.** The AI guide lists APK libraries but leaves the reader to choose how to interpret the binary manifest. Add pinned tools/commands for package ID, version, splits, ABI, engine, imports, symbol types and relocations. Produce a private report and a publishable output without personal paths. Explain early that TLS/TLSDESC, RELR, IFUNC and packed relocations are outside the [V5 nxloader](../framework/nxloader/README.md) contract, avoiding discovery of this incompatibility after writing the adapter.
2. **Shim integration by contract.** The example registers two symbols (`__errno`, `__android_log_write`) and a teaching property; it does not cover most games. Show integration with the real registry, a missing mandatory import, ownership and errors. Add separate exercises for assets/reads, Bionic structures, threads, JNI and thread-bound references, lifecycle, audio and input. Reuse existing implementations only when their contracts match.
3. **Starter project for each family.** Unity, Mono Android, Godot and Cocos2d-x already have investigation guides. Small original exercises with complete files, dependencies, build commands and expected output are still missing. Mono must distinguish a Linux managed host from an Android bootstrap; Godot must distinguish exporting an original project from adapting the owner's Android build. None should promise that copying a reference is sufficient.
4. **Catalog searchable by profile.** The 41 source records contain provenance/hashes, but none has structured `engine`, `abi`, `renderer`, `build_status` or `tested_devices` fields. Some of this information exists in cards and the 15 Unity cases. Consolidate proven data and use `unknown` for the rest, allowing AI to select a base by contract instead of title.
5. **Diagnostics with demonstrated results.** A troubleshooting table already exists. Add good/bad outputs: rejected relocation, missing import, incorrect ABI, unregistered JNI, invalid context, audio without video, save and shutdown. Each case needs the next measurement and a success condition.
6. **Complete example delivery recipe.** Provide `nxproject.json`, an implemented contract, NXExtract recipe, PT/EN `INSTALLATION.md` and a command coordinating the final checks. Physical evidence remains target-specific; a synthetic project does not certify commercial games.

Teaching examples and documentation can live outside the frozen trees. A gap requiring shared V5 code or behavior changes belongs to separate V6 development, not a baseline hotfix.

## 4. Specific attention to Freedom Planet 2

The [Vulkan → GLES2 study](../portando_unity/en/FP2-VULKAN-GLES2.md) explains the transformation and limitations. Making it reproducible as a lesson requires a laboratory independent of commercial data:

- Show how to obtain and build the [translator's dependencies](../ports/fp2-nextos/upstream/tools/build_exact_shader_tool.sh), respecting the SPIRV-Cross commit, SPIRV-Tools version and SMOL-V hashes already enforced by the script. The collection does not provide all of them.
- Use simple original shaders to exercise SMOL-V/SPIR-V → ESSL 1.00, bindings, alpha and rejection of an incompatible operation, with commands and expected results.
- Separate host translation testing from GLES2 compilation/linking and physical output. An original sample does not prove transformation of the entire FP2 build.
- Explain how to recover public build files omitted from the selection and apply the recipe to the owner's copy without distributing commercial shaders or data.

This laboratory should teach the conversion method. It should not copy FP2 offsets, variant counts or exceptions as rules for other games.

## 5. Recommended implementation order

1. Complete the public ARM environment and collection pinning procedure.
2. Create a **small original training game** with code-generated graphics and sound: Android AArch64 guest, Linux loader, a few explicit shims, JNI/lifecycle, controls, save and shutdown. Publish all original sources.
3. Teach its clean NXExtract installation and launcher generation, preserving the canonical UI and NXSplash.
4. Automate this project's reproducible host and cross tests and document physical evidence separately. ARMv7 becomes a later exercise with an explicit softfp boundary.
5. Expand into engine examples, the FP2 shader laboratory and a catalog coverage matrix.

Completion criterion: someone in an independent environment can follow PT or EN, compile, prepare the original input, generate the project and locate every missing dependency without consulting private files. Every step has a verifiable result and a stated limit. The collection remains private while review continues.
