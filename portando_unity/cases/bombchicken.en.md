# Bomb Chicken — Unity 2022.3.39f1

[Português](bombchicken.md)

[Unity index](../README.en.md) · [NextOS source card](../../ports/bombchicken-gunbrick-nextos/README.en.md)

Selected-note profile: IL2CPP/AArch64. This is an editorial adaptation of the `portando_unity` study linked to public sources. It describes historical causes and solutions; inclusion neither revalidates the snapshot nor certifies every device.

## Symptom and boundary

Black output could result from a LevelStart.Awake exception caused by an incorrect JNI return; another end-of-level failure came from malformed Progress.

## Solution to study

Preserve GLES2/stencil8 and repair only the demonstrated JNI/persistence contract. Do not change the renderer to treat a logic exception.

## Audio and input

The Unity/FMOD mixer uses the OpenSL/SDL bridge. IL2CPP input calls need the correct overload, typedef and MethodInfo for this build.

## Limitations and counterchecks

This card covers Bomb Chicken; the same repository also contains Gunbrick with its own profile. Do not transfer offsets between them.

When adapting, recalculate signatures/offsets for the owner's payload. Compare before/after in the same scene, preserve the reference and record the new artifact hash. A host test or visible title does not establish full gameplay.

## Selected public sources

Origin: [bombchicken-gunbrick-nextos](https://github.com/NextOs-Ports/bombchicken-gunbrick-nextos), commit `53df6041da4ecc4905af5ecd24551034eea21175`. [Manifest](../../ports/bombchicken-gunbrick-nextos/SOURCE-MAP.json); [Unity edition provenance](../SOURCE-MAP.json).

- [README.md](../../ports/bombchicken-gunbrick-nextos/upstream/README.md)
- [docs/images/README.md](../../ports/bombchicken-gunbrick-nextos/upstream/docs/images/README.md)
- [ports/bombchicken/src/main.c](../../ports/bombchicken-gunbrick-nextos/upstream/ports/bombchicken/src/main.c)
- [ports/gunbrick/src/main.c](../../ports/bombchicken-gunbrick-nextos/upstream/ports/gunbrick/src/main.c)
- [ports/bombchicken/src/jni.c](../../ports/bombchicken-gunbrick-nextos/upstream/ports/bombchicken/src/jni.c)
- [ports/gunbrick/src/jni.c](../../ports/bombchicken-gunbrick-nextos/upstream/ports/gunbrick/src/jni.c)
- [ports/bombchicken/src/input.c](../../ports/bombchicken-gunbrick-nextos/upstream/ports/bombchicken/src/input.c)
- [ports/gunbrick/src/input.c](../../ports/bombchicken-gunbrick-nextos/upstream/ports/gunbrick/src/input.c)
- [ports/bombchicken/src/egl.c](../../ports/bombchicken-gunbrick-nextos/upstream/ports/bombchicken/src/egl.c)
- [ports/gunbrick/src/egl.c](../../ports/bombchicken-gunbrick-nextos/upstream/ports/gunbrick/src/egl.c)

Port/integration author: **NextOS**. Preserve third-party licenses and credits; game data is not included with the sources.
