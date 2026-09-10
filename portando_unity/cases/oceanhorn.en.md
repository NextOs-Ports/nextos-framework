# Oceanhorn: Chronos Dungeon — Unity 2022.3.61f1

[Português](oceanhorn.md)

[Unity index](../README.en.md) · [NextOS source card](../../ports/oceanhorn-nextos/README.en.md)

Selected-note profile: IL2CPP/AArch64. This is an editorial adaptation of the `portando_unity` study linked to public sources. It describes historical causes and solutions; inclusion neither revalidates the snapshot nor certifies every device.

## Symptom and boundary

Boot and sprites depended on distinguishing a native JNIBridge pointer from a GCHandle and using the correct atlas alpha source.

## Solution to study

Preserve native GLES2 variants and framebuffer framing. Do not enable experimental render reduction as this reference default.

## Audio and input

Rewired consumes KeyEvent/MotionEvent. FMOD fallback to resident loading must depend on the asynchronous-worker failure, with measured memory and audio.

## Limitations and counterchecks

This title is Chronos Dungeon. Do not confuse versions or solutions from other games in the franchise.

When adapting, recalculate signatures/offsets for the owner's payload. Compare before/after in the same scene, preserve the reference and record the new artifact hash. A host test or visible title does not establish full gameplay.

## Selected public sources

Origin: [oceanhorn-nextos](https://github.com/NextOs-Ports/oceanhorn-nextos), commit `7cb05cca86afbb4b8a6e76aa251c8f0f8397bc9e`. [Manifest](../../ports/oceanhorn-nextos/SOURCE-MAP.json); [Unity edition provenance](../SOURCE-MAP.json).

- [README.md](../../ports/oceanhorn-nextos/upstream/README.md)
- [src/main.c](../../ports/oceanhorn-nextos/upstream/src/main.c)
- [src/jni_shim.c](../../ports/oceanhorn-nextos/upstream/src/jni_shim.c)
- [src/egl_shim.c](../../ports/oceanhorn-nextos/upstream/src/egl_shim.c)

Port/integration author: **NextOS**. Preserve third-party licenses and credits; game data is not included with the sources.
