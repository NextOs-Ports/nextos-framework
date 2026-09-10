# Prizefighters 2 — Unity 2022.3.62f2

[Português](pf2.md)

[Unity index](../README.en.md) · [NextOS source card](../../ports/prizefighters2-nextos/README.en.md)

Selected-note profile: IL2CPP/AArch64. This is an editorial adaptation of the `portando_unity` study linked to public sources. It describes historical causes and solutions; inclusion neither revalidates the snapshot nor certifies every device.

## Symptom and boundary

The build needs compatible GLES2 shader variants and has menus consuming managed Mouse input.

## Solution to study

Translate required programs and preserve Built-in/Gamma. Convert cursor Y position and Y delta exactly once into the actual sink space; do not hard-code 720 in a variable-resolution port.

## Audio and input

Input System uses specific StateEvent/GamepadState layouts; mouse input is contextual. fmodGetInfo/fmodProcess and the PCM queue must preserve the mixer.

## Limitations and counterchecks

This is not a URP.renderScale recipe. Payload identity and hooks require their own validation.

When adapting, recalculate signatures/offsets for the owner's payload. Compare before/after in the same scene, preserve the reference and record the new artifact hash. A host test or visible title does not establish full gameplay.

## Selected public sources

Origin: [prizefighters2-nextos](https://github.com/NextOs-Ports/prizefighters2-nextos), commit `53371b37dc5d01e0a8c09d683536ee1c48f35d6c`. [Manifest](../../ports/prizefighters2-nextos/SOURCE-MAP.json); [Unity edition provenance](../SOURCE-MAP.json).

- [README.md](../../ports/prizefighters2-nextos/upstream/README.md)
- [src/main.c](../../ports/prizefighters2-nextos/upstream/src/main.c)
- [src/jni.c](../../ports/prizefighters2-nextos/upstream/src/jni.c)
- [src/input.c](../../ports/prizefighters2-nextos/upstream/src/input.c)
- [src/egl.c](../../ports/prizefighters2-nextos/upstream/src/egl.c)

Port/integration author: **NextOS**. Preserve third-party licenses and credits; game data is not included with the sources.
