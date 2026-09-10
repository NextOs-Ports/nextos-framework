# Freedom Planet 2 — Unity 2018.4.36f1

[Português](fp2.md)

[Unity index](../README.en.md) · [NextOS source card](../../ports/fp2-nextos/README.en.md)

Selected-note profile: IL2CPP/AArch64. This is an editorial adaptation of the `portando_unity` study linked to public sources. It describes historical causes and solutions; inclusion neither revalidates the snapshot nor certifies every device.

## Symptom and boundary

The build uses Vulkan SMOL-V/SPIR-V programs. The public route prepares GLES2/ESSL 1.00 programs during installation, preserving the required game rendering logic.

## Solution to study

Use eligible exact translation and surgical serialized-file writes. StencilDraw/StencilInvert require a narrow ARGB→RGBA swizzle/alpha correction, identified by SHA.

## Audio and input

FMOD delivers PCM through firmware SDL; input follows the game Android contract. Check title, number 2, tutorial, next loading step, HUD and exit.

## Limitations and counterchecks

This is not a universal Vulkan driver. Unrepresentable variants have an explicit policy; do not replace every shader with a generic template. See the in-depth Vulkan study in this edition.

When adapting, recalculate signatures/offsets for the owner's payload. Compare before/after in the same scene, preserve the reference and record the new artifact hash. A host test or visible title does not establish full gameplay.

Also read [FP2: Vulkan to GLES2](../en/FP2-VULKAN-GLES2.md).

## Selected public sources

Origin: [fp2-nextos](https://github.com/NextOs-Ports/fp2-nextos), commit `f8caab9d946c39041fa5cc25cb52621590f82adc`. [Manifest](../../ports/fp2-nextos/SOURCE-MAP.json); [Unity edition provenance](../SOURCE-MAP.json).

- [README.md](../../ports/fp2-nextos/upstream/README.md)
- [src/main.c](../../ports/fp2-nextos/upstream/src/main.c)
- [src/jni.c](../../ports/fp2-nextos/upstream/src/jni.c)
- [src/input.c](../../ports/fp2-nextos/upstream/src/input.c)
- [src/egl.c](../../ports/fp2-nextos/upstream/src/egl.c)
- [src/gles3.c](../../ports/fp2-nextos/upstream/src/gles3.c)
- [src/unity6_shader.c](../../ports/fp2-nextos/upstream/src/unity6_shader.c)

Port/integration author: **NextOS**. Preserve third-party licenses and credits; game data is not included with the sources.
