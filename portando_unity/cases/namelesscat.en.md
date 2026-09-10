# Nameless Cat — Unity 6000.3.11f1

[Português](namelesscat.md)

[Unity index](../README.en.md) · [NextOS source card](../../ports/namelesscat-nextos/README.en.md)

Selected-note profile: IL2CPP/AArch64. This is an editorial adaptation of the `portando_unity` study linked to public sources. It describes historical causes and solutions; inclusion neither revalidates the snapshot nor certifies every device.

## Symptom and boundary

A private SDL changed video integration and produced a black screen despite a live GL context.

## Solution to study

Use firmware SDL and admit/configure the controller at the canonical boundary before SDL_Init. Preserve the proven graphics facade and provider.

## Audio and input

GameController uses actual GUID/mapping; pause and cursor remain contextual. Keep the pinned profile FMOD/AudioTrack/OpenSL backend.

## Limitations and counterchecks

The local note does not bind every historical source to one approved commit. The public snapshot is an identified reference, not automatic proof of that binary.

When adapting, recalculate signatures/offsets for the owner's payload. Compare before/after in the same scene, preserve the reference and record the new artifact hash. A host test or visible title does not establish full gameplay.

## Selected public sources

Origin: [namelesscat-nextos](https://github.com/NextOs-Ports/namelesscat-nextos), commit `fb489c406495e30822d50c8250c76810845414d5`. [Manifest](../../ports/namelesscat-nextos/SOURCE-MAP.json); [Unity edition provenance](../SOURCE-MAP.json).

- [README.md](../../ports/namelesscat-nextos/upstream/README.md)
- [generated/namelesscat/README.md](../../ports/namelesscat-nextos/upstream/generated/namelesscat/README.md)
- [src/main.c](../../ports/namelesscat-nextos/upstream/src/main.c)
- [src/jni.c](../../ports/namelesscat-nextos/upstream/src/jni.c)
- [src/input.c](../../ports/namelesscat-nextos/upstream/src/input.c)
- [src/egl.c](../../ports/namelesscat-nextos/upstream/src/egl.c)
- [src/gles3.c](../../ports/namelesscat-nextos/upstream/src/gles3.c)
- [src/unity6_shader.c](../../ports/namelesscat-nextos/upstream/src/unity6_shader.c)

Port/integration author: **NextOS**. Preserve third-party licenses and credits; game data is not included with the sources.
