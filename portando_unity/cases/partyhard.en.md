# Party Hard GO — Unity 6000.3.10f1

[Português](partyhard.md)

[Unity index](../README.en.md) · [NextOS source card](../../ports/partyhard-nextos/README.en.md)

Selected-note profile: IL2CPP/AArch64. This is an editorial adaptation of the `portando_unity` study linked to public sources. It describes historical causes and solutions; inclusion neither revalidates the snapshot nor certifies every device.

## Symptom and boundary

Incorrect data layout blocked scenes; later, simultaneous KeyEvent and HAT D-pad delivery caused stuck movement.

## Solution to study

Preserve assets/bin/Data and lifecycle. Use one directional authority and release actions on context/focus changes or disconnect. SharedPreferences.apply may batch writes while keeping commit/pause/exit durable.

## Audio and input

InControl governs gameplay; right-stick/R3 cursor serves the control-selection dialog. FMOD/AudioTrack retains mixer and frame counts.

## Limitations and counterchecks

The note distinguishes public 1.0.6/V5 closure from earlier builds. Check the snapshot revision before assigning that evidence to the observed HEAD.

When adapting, recalculate signatures/offsets for the owner's payload. Compare before/after in the same scene, preserve the reference and record the new artifact hash. A host test or visible title does not establish full gameplay.

## Selected public sources

Origin: [partyhard-nextos](https://github.com/NextOs-Ports/partyhard-nextos), commit `0a03b8483ac9c84d3be63c8dd898cf5acd305576`. [Manifest](../../ports/partyhard-nextos/SOURCE-MAP.json); [Unity edition provenance](../SOURCE-MAP.json).

- [README.md](../../ports/partyhard-nextos/upstream/README.md)
- [src/main.c](../../ports/partyhard-nextos/upstream/src/main.c)
- [src/jni.c](../../ports/partyhard-nextos/upstream/src/jni.c)
- [src/input.c](../../ports/partyhard-nextos/upstream/src/input.c)
- [src/egl.c](../../ports/partyhard-nextos/upstream/src/egl.c)
- [src/gles3.c](../../ports/partyhard-nextos/upstream/src/gles3.c)
- [src/unity6_shader.c](../../ports/partyhard-nextos/upstream/src/unity6_shader.c)

Port/integration author: **NextOS**. Preserve third-party licenses and credits; game data is not included with the sources.
