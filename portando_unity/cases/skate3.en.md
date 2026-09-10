# Skateboard Party 3 — Unity 2022.3.45f1

[Português](skate3.md)

[Unity index](../README.en.md) · [NextOS source card](../../ports/skate3-nextos/README.en.md)

Selected-note profile: IL2CPP/AArch64. This is an editorial adaptation of the `portando_unity` study linked to public sources. It describes historical causes and solutions; inclusion neither revalidates the snapshot nor certifies every device.

## Symptom and boundary

The build retains GLES2 and ETC1, with a residual ETC2 subset requiring selective fallback.

## Solution to study

Confirm actual backend selection and each format upload before conversion. Analytics logs do not establish a graphics hang.

## Audio and input

FMOD/OpenSL delivers audio to SDL; preserve native controls and firmware mapping. Validate entry, age menus, tricks and exit.

## Limitations and counterchecks

The repository is named skate3-nextos, but the game is Skateboard Party 3. Its name does not automatically identify InControl or Rewired.

When adapting, recalculate signatures/offsets for the owner's payload. Compare before/after in the same scene, preserve the reference and record the new artifact hash. A host test or visible title does not establish full gameplay.

## Selected public sources

Origin: [skate3-nextos](https://github.com/NextOs-Ports/skate3-nextos), commit `18621e90ce7458c3166aa50d5cc508a13e65ee84`. [Manifest](../../ports/skate3-nextos/SOURCE-MAP.json); [Unity edition provenance](../SOURCE-MAP.json).

- [README.md](../../ports/skate3-nextos/upstream/README.md)
- [src/main.c](../../ports/skate3-nextos/upstream/src/main.c)
- [src/jni.c](../../ports/skate3-nextos/upstream/src/jni.c)
- [src/input.c](../../ports/skate3-nextos/upstream/src/input.c)
- [src/egl.c](../../ports/skate3-nextos/upstream/src/egl.c)

Port/integration author: **NextOS**. Preserve third-party licenses and credits; game data is not included with the sources.
