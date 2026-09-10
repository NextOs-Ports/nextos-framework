# Hitman GO — Unity 2022.3.67f2

[Português](hitmango.md)

[Unity index](../README.en.md) · [NextOS source card](../../ports/hitmango-nextos/README.en.md)

Selected-note profile: IL2CPP/AArch64. This is an editorial adaptation of the `portando_unity` study linked to public sources. It describes historical causes and solutions; inclusion neither revalidates the snapshot nor certifies every device.

## Symptom and boundary

The native GLES2 path requires selective handling of unsupported formats and final alpha compatible with the compositor.

## Solution to study

Preserve supported ETC1; decode ETC2/EAC only where required. Check RGB/alpha pixels before applying a backbuffer correction.

## Audio and input

A native thread reproduces FMODAudioDevice/AudioTrack without replacing the mixer. InControl governs board movement; cursor and shortcuts are contextual.

## Limitations and counterchecks

Do not turn this gamepad path into global touch emulation. Revalidate chapters, saves and exit in the new artifact.

When adapting, recalculate signatures/offsets for the owner's payload. Compare before/after in the same scene, preserve the reference and record the new artifact hash. A host test or visible title does not establish full gameplay.

## Selected public sources

Origin: [hitmango-nextos](https://github.com/NextOs-Ports/hitmango-nextos), commit `3f735165348e17d4f9aa20f1d227a9b1ecb4efb8`. [Manifest](../../ports/hitmango-nextos/SOURCE-MAP.json); [Unity edition provenance](../SOURCE-MAP.json).

- [README.md](../../ports/hitmango-nextos/upstream/README.md)
- [hitmango/README.md](../../ports/hitmango-nextos/upstream/hitmango/README.md)
- [src/main.c](../../ports/hitmango-nextos/upstream/src/main.c)
- [src/jni.c](../../ports/hitmango-nextos/upstream/src/jni.c)
- [src/input.c](../../ports/hitmango-nextos/upstream/src/input.c)
- [src/egl.c](../../ports/hitmango-nextos/upstream/src/egl.c)

Port/integration author: **NextOS**. Preserve third-party licenses and credits; game data is not included with the sources.
