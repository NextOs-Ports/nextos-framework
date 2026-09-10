# Horizon Chase — Unity 2022.3.33f1

[Português](horizonchase.md)

[Unity index](../README.en.md) · [NextOS source card](../../ports/horizonchase-nextos/README.en.md)

Selected-note profile: IL2CPP/AArch64. This is an editorial adaptation of the `portando_unity` study linked to public sources. It describes historical causes and solutions; inclusion neither revalidates the snapshot nor certifies every device.

## Symptom and boundary

Sprites and scanout require distinguishing texture alpha, UV precision and final backbuffer alpha.

## Solution to study

Preserve existing ETC1 RGB/Alpha8. Fix the affected material/precision; when RGB is correct but final alpha invalidates scanout, change only alpha before swap and restore state.

## Audio and input

FMOD retains music/effects; input uses firmware mapping. Stream→sample is conditioned on a specific failure, not a preventive default.

## Limitations and counterchecks

KMS/SDL must retain a single context/surface owner. Evidence from one provider does not certify all backends.

When adapting, recalculate signatures/offsets for the owner's payload. Compare before/after in the same scene, preserve the reference and record the new artifact hash. A host test or visible title does not establish full gameplay.

## Selected public sources

Origin: [horizonchase-nextos](https://github.com/NextOs-Ports/horizonchase-nextos), commit `59d2d38dc496ae0a71726181d3ccc80923e4144d`. [Manifest](../../ports/horizonchase-nextos/SOURCE-MAP.json); [Unity edition provenance](../SOURCE-MAP.json).

- [README.md](../../ports/horizonchase-nextos/upstream/README.md)
- [third_party/NXExtract/README.md](../../ports/horizonchase-nextos/upstream/third_party/NXExtract/README.md)
- [src/main.c](../../ports/horizonchase-nextos/upstream/src/main.c)
- [src/jni_shim.c](../../ports/horizonchase-nextos/upstream/src/jni_shim.c)
- [src/egl_shim.c](../../ports/horizonchase-nextos/upstream/src/egl_shim.c)

Port/integration author: **NextOS**. Preserve third-party licenses and credits; game data is not included with the sources.
