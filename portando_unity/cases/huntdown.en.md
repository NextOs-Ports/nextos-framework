# Huntdown — Unity 2022.3.47f1 / 6000.2.6f2

[Português](huntdown.md)

[Unity index](../README.en.md) · [NextOS source card](../../ports/huntdown-nextos/README.en.md)

Selected-note profile: IL2CPP/AArch64. This is an editorial adaptation of the `portando_unity` study linked to public sources. It describes historical causes and solutions; inclusion neither revalidates the snapshot nor certifies every device.

## Symptom and boundary

There are two data profiles: 200023/Unity 2022 and 200036/Unity 6. The first profile gameplay evidence does not transfer to the second.

## Solution to study

For the 2022 profile, measure EGL timestamp capabilities before Swappy fallback and validate signatures before patching. For profile 6, proxy JNI, typed callbacks and facade/ESSL have limited title/menu evidence in selected notes.

## Audio and input

Input follows profile-specific GamePad methods and firmware mapping. Audio also differs between FMOD/OpenSL and AAudio; two declared slots do not prove physical co-op.

## Limitations and counterchecks

Do not apply a 2022 Swappy patch to Unity 6 or treat log-based configuration repairs as physically approved devices.

When adapting, recalculate signatures/offsets for the owner's payload. Compare before/after in the same scene, preserve the reference and record the new artifact hash. A host test or visible title does not establish full gameplay.

## Selected public sources

Origin: [huntdown-nextos](https://github.com/NextOs-Ports/huntdown-nextos), commit `7eef080e69a66a09efc005bf3472035eef09248b`. [Manifest](../../ports/huntdown-nextos/SOURCE-MAP.json); [Unity edition provenance](../SOURCE-MAP.json).

- [README.md](../../ports/huntdown-nextos/upstream/README.md)
- [huntdown/README.md](../../ports/huntdown-nextos/upstream/huntdown/README.md)
- [src/main.c](../../ports/huntdown-nextos/upstream/src/main.c)
- [src/jni_shim.c](../../ports/huntdown-nextos/upstream/src/jni_shim.c)
- [src/egl_shim.c](../../ports/huntdown-nextos/upstream/src/egl_shim.c)
- [src/gles3.c](../../ports/huntdown-nextos/upstream/src/gles3.c)
- [src/unity6_shader.c](../../ports/huntdown-nextos/upstream/src/unity6_shader.c)

Port/integration author: **NextOS**. Preserve third-party licenses and credits; game data is not included with the sources.
