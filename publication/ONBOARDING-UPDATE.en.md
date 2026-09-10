# Updated path to a first port

[Português](ONBOARDING-UPDATE.md)

Date: 2026-09-10. This update implements the training path identified by the [earlier simulation](ONBOARDING-SIMULATION.en.md). The repository remains private; no game release or visibility change occurred.

## What was added

- [Public ARM SDK](../toolchains/sdk/README.en.md), built from pinned public sources, with Linux compilers, SDL2/GLES2 development libraries, Clang and QEMU.
- [Integrated first port](../examples/first-port/README.en.md): original minigame, Android AArch64 guest, V5 nxloader, two shims, JNI/lifecycle, GLES2 renderer, PCM, controls and saving. Its NXExtract recipe prepares the original training input from scratch.
- [Pins and source recovery](../docs/en/PINS-AND-SOURCES.md): all 15 components admitted by the helper, preserved historical provenance and a separate export of contracts required by the tools.
- [Android inventory](../docs/en/ANDROID-INVENTORY.md), [41 searchable profiles](../catalog/profiles.json), and [diagnostics with negative cases](../docs/en/DIAGNOSTIC-LAB.md).
- [Mono, Godot, Unity and Cocos2d-x exercises](../examples/engines/README.en.md), documenting each family's limits, and a [SMOL-V/SPIR-V → ESSL 1.00 laboratory](../examples/shader-lab/README.en.md) without commercial shaders.
- [Manual CI workflow](../.github/workflows/onboarding.yml) for the collection and original example. It publishes no artifacts and executes no commercial games; it has not run on GitHub in this revision.

All new guides have PT/EN versions. No frozen V5 runtime, NXExtract or upstream snapshot file changed. The 47 titles remain catalog references, rather than 47 newly tested ports.

## Checks performed

| Stage | Result | Limit |
| --- | --- | --- |
| Public SDK build | PASS | Docker linux/amd64 host; public dependencies |
| Cross C example AArch64 / ARMv7 | PASS, GLIBC 2.17 / 2.4 | Build/audit, no Android ARMv7 proof |
| Collection pins | Create/materialize/verify of 15 components PASS | Exported selection; omitted historical tests remain omitted |
| First port | 11 integrated checks PASS | Host extraction and AArch64 CPU execution in QEMU |
| Inventory/catalog tools | 10 tests PASS | Includes long ELF imports, identity, paths and symbol kinds |
| Shader laboratory | 5 checks PASS | Host translation/validation, no physical GLES2 driver |
| Mono | P/Invoke, layout, callback and error PASS | Managed Linux host; not Mono Android bootstrap |
| Godot 3.5.3 | Headless logic PASS | Movement/collection; no rendering or ARM export |
| Unity / Cocos2d-x | Exercise sources added | Unity editor build and Cocos integration not yet compiled |

The 11 integrated checks cover clean installation, a real hook, verified outputs, guest execution, a missing import before constructors, compatible repackaging, incompatible package/payload/ABI, missing payload and hook rollback. The pipeline records logs and results in a new area. It explicitly disables UI on the host; it does not replace canonical graphical installation.

Stages were checked during development. Manifest/generation corrections preserved the previously compiled native bytes; the executable was not rebuilt merely to update documentation. None of these checks certifies device graphics, sound or controls.

## Development artifact identities

| Artifact | Identity |
| --- | --- |
| Local SDK image | `sha256:041f653673aab41aa0c53adc557f31a295a8ac32585357ebfea926383b831094` |
| Exported composition pin | `f411e522a861e92486fb307dcfb87d0facbad8c0fde1ebf65c491ffe8cc41832` |
| Teaching AArch64 executable | `a6c9142a393cd4d05624840cdc92f186f9354fdc4958e7432bdb6ed3de8aca62` |
| Original Android AArch64 guest | `ddab9a2004d1b51f8d7cb86e6e9a1c57f5f643badebf926bfd011a929a5fd35a` |
| Original training input | `53b00ecaeadf32058f40848874751efc776aa714fb504ead931085102138cf63` |

These hashes identify the recorded local run. The recipe pins sources/dependencies, but a rebuilt image may have a different digest due to build timestamps. Record your own results; do not copy these hashes to claim you tested your bytes. Builds, inputs and logs remain in the ignored `work/` area.

## What remains open

The generated launcher/NXExtract/NXSplash were prepared but not physically tested in this task. The generation adapter remains marked as scaffold/nonrelease. Graphical extraction, pixels, audio, controls, persistence and exit need proof on an explicitly authorized target before delivering a port.

The recovered public SMOL-V matches FP2's required hashes. FP2's historical SPIRV-Cross commit could not be obtained from Khronos upstream in this revision; its exact origin remains unresolved. The laboratory uses another declared public pin and does not certify an identical rebuild of the approved translator.

Unity and Cocos need the documented editor/engine builds; historical references may still require external files and individual adaptations. There is no claim that the shims cover most games. Licensing, editorial review and approval to become public remain in [REVIEW.en.md](REVIEW.en.md).
