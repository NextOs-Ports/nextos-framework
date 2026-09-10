# Terraria — Unity 2021.3.56f2

[Português](terraria.md)

[Unity index](../README.en.md) · [NextOS source card](../../ports/terraria-nextos/README.en.md)

Selected-note profile: IL2CPP/AArch64. This is an editorial adaptation of the `portando_unity` study linked to public sources. It describes historical causes and solutions; inclusion neither revalidates the snapshot nor certifies every device.

## Symptom and boundary

The engine could emit no GL calls when eglGetProcAddress failed to route EGL functions and the requested configuration was rejected on target.

## Solution to study

Preserve EGL identity and a valid SDL-context configuration, with nativeRender driven by the host. Measure GfxDevice creation and first present.

## Audio and input

The rate reported by fmodGetInfo and DirectByteBuffer size must match actually produced frames. InControl needs one gamepad route, neutralized during text keyboard entry where required.

## Limitations and counterchecks

Do not promote historical jobs/GC workarounds into the framework. This classification comes from the examined Android build, not other game editions.

When adapting, recalculate signatures/offsets for the owner's payload. Compare before/after in the same scene, preserve the reference and record the new artifact hash. A host test or visible title does not establish full gameplay.

## Selected public sources

Origin: [terraria-nextos](https://github.com/NextOs-Ports/terraria-nextos), commit `e04f6fe7d3be9591e659f6ecd42355d1b5d2caf6`. [Manifest](../../ports/terraria-nextos/SOURCE-MAP.json); [Unity edition provenance](../SOURCE-MAP.json).

- [README.md](../../ports/terraria-nextos/upstream/README.md)
- [package/universal/README.md](../../ports/terraria-nextos/upstream/package/universal/README.md)
- [src/main.c](../../ports/terraria-nextos/upstream/src/main.c)
- [src/jni_shim.c](../../ports/terraria-nextos/upstream/src/jni_shim.c)
- [src/egl_shim.c](../../ports/terraria-nextos/upstream/src/egl_shim.c)

Port/integration author: **NextOS**. Preserve third-party licenses and credits; game data is not included with the sources.
