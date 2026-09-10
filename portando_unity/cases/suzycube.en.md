# Suzy Cube — Unity 2017.4.40f1

[Português](suzycube.md)

[Unity index](../README.en.md) · [NextOS source card](../../ports/suzycube-nextos/README.en.md)

Selected-note profile: IL2CPP/AArch64. This is an editorial adaptation of the `portando_unity` study linked to public sources. It describes historical causes and solutions; inclusion neither revalidates the snapshot nor certifies every device.

## Symptom and boundary

This build already includes GLES2 and ETC1. Pre-render blockers were boot/JNI contracts, including actual String.length and __stack_chk_guard as an object.

## Solution to study

Preserve Unity loading libil2cpp during initJni. The documented sequence uses nativeRecreateGfxState, focus, resume and nativeRender; do not impose Unity 2022 callbacks.

## Audio and input

FMOD retains the mixer; InControl receives the normalized gamepad. Check axes already transformed by SDL, focus and hotplug without stuck directions.

## Limitations and counterchecks

Historical evidence for this build does not cover every data variant. Do not start shader/texture conversion when the native path already supplies required formats.

When adapting, recalculate signatures/offsets for the owner's payload. Compare before/after in the same scene, preserve the reference and record the new artifact hash. A host test or visible title does not establish full gameplay.

## Selected public sources

Origin: [suzycube-nextos](https://github.com/NextOs-Ports/suzycube-nextos), commit `c3db13c749173e08d33d57839aca961d0a2ab319`. [Manifest](../../ports/suzycube-nextos/SOURCE-MAP.json); [Unity edition provenance](../SOURCE-MAP.json).

- [README.md](../../ports/suzycube-nextos/upstream/README.md)
- [src/main.c](../../ports/suzycube-nextos/upstream/src/main.c)
- [src/jni.c](../../ports/suzycube-nextos/upstream/src/jni.c)
- [src/input.c](../../ports/suzycube-nextos/upstream/src/input.c)
- [src/egl.c](../../ports/suzycube-nextos/upstream/src/egl.c)

Port/integration author: **NextOS**. Preserve third-party licenses and credits; game data is not included with the sources.
