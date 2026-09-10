# Merchant of the Skies — Unity 6000.4.2f1

[Português](merchantskies.md)

[Unity index](../README.en.md) · [NextOS source card](../../ports/merchantskies-nextos/README.en.md)

Selected-note profile: IL2CPP/AArch64. This is an editorial adaptation of the `portando_unity` study linked to public sources. It describes historical causes and solutions; inclusion neither revalidates the snapshot nor certifies every device.

## Symptom and boundary

Block-shaped TextMeshPro glyphs exposed a channel contract: R8 converted to LUMINANCE produced constant alpha.

## Solution to study

In the demonstrated GLES2 path, LUMINANCE_ALPHA representation must preserve r/a reads. On physical GLES3, R8 storage, RED upload and swizzle must remain coherent; do not convert only one side.

## Audio and input

A missing libaaudio must fail as absent to avoid selecting a false mixer route. Rewired uses normalized gamepad input; A/B actions depend on the measured contract.

## Limitations and counterchecks

Do not copy keycode tables or audio fallback merely by similarity. Preserve pause/save before a shutdown deadline when nativeDone does not return.

When adapting, recalculate signatures/offsets for the owner's payload. Compare before/after in the same scene, preserve the reference and record the new artifact hash. A host test or visible title does not establish full gameplay.

## Selected public sources

Origin: [merchantskies-nextos](https://github.com/NextOs-Ports/merchantskies-nextos), commit `213a3d9875158164bb532ef031651a052ab8360e`. [Manifest](../../ports/merchantskies-nextos/SOURCE-MAP.json); [Unity edition provenance](../SOURCE-MAP.json).

- [README.md](../../ports/merchantskies-nextos/upstream/README.md)
- [src/main.c](../../ports/merchantskies-nextos/upstream/src/main.c)
- [src/jni.c](../../ports/merchantskies-nextos/upstream/src/jni.c)
- [src/input.c](../../ports/merchantskies-nextos/upstream/src/input.c)
- [src/egl.c](../../ports/merchantskies-nextos/upstream/src/egl.c)
- [src/gles3.c](../../ports/merchantskies-nextos/upstream/src/gles3.c)
- [src/unity6_shader.c](../../ports/merchantskies-nextos/upstream/src/unity6_shader.c)

Port/integration author: **NextOS**. Preserve third-party licenses and credits; game data is not included with the sources.
