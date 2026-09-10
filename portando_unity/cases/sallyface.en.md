# Sally Face — Unity 2022.3.62f3

[Português](sallyface.md)

[Unity index](../README.en.md) · [NextOS source card](../../ports/sallyface-nextos/README.en.md)

Selected-note profile: IL2CPP/AArch64. This is an editorial adaptation of the `portando_unity` study linked to public sources. It describes historical causes and solutions; inclusion neither revalidates the snapshot nor certifies every device.

## Symptom and boundary

Magenta rain required scene shaders beyond global ones; cropped scenery/characters came from reduced atlases without matching Sprite rectangles.

## Solution to study

Translate the used program set and preserve complete atlases/rectangles. Reducing dimensions is not a necessary consequence of ETC1.

## Audio and input

Audio required executing the FMODAudioDevice.run thread function and feeding the PCM queue. Read input.c for the actual mapping before copying navigation.

## Limitations and counterchecks

The notes record Episode 1 evidence, not a complete campaign through every episode. Preserve that limitation.

When adapting, recalculate signatures/offsets for the owner's payload. Compare before/after in the same scene, preserve the reference and record the new artifact hash. A host test or visible title does not establish full gameplay.

## Selected public sources

Origin: [sallyface-nextos](https://github.com/NextOs-Ports/sallyface-nextos), commit `39f3d3bd7fed4f160ae20af0f8824ab6b71b79bf`. [Manifest](../../ports/sallyface-nextos/SOURCE-MAP.json); [Unity edition provenance](../SOURCE-MAP.json).

- [README.md](../../ports/sallyface-nextos/upstream/README.md)
- [src/main.c](../../ports/sallyface-nextos/upstream/src/main.c)
- [src/jni.c](../../ports/sallyface-nextos/upstream/src/jni.c)
- [src/input.c](../../ports/sallyface-nextos/upstream/src/input.c)
- [src/egl.c](../../ports/sallyface-nextos/upstream/src/egl.c)

Port/integration author: **NextOS**. Preserve third-party licenses and credits; game data is not included with the sources.
