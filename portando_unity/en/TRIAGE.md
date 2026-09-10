# Triage a Unity build

[Português](../pt-BR/TRIAGEM.md)

This edition selects `portando_unity` material for NextOS ports admitted to the catalog. Start with the actual build identity; the Unity version alone does not determine renderer, audio or input.

## 1. Identify without guessing

Record game name/version, package ID, complete container set, ABIs, Unity library, Mono/IL2CPP runtime and full engine version. Cross-check asset headers and runtime identification. A file named unity6_shader.c does not prove Unity 6, and “Unity 22” does not replace 2022.x.y with its suffix.

Separate static observation, hypothesis and measured behavior. If the exact patch is unknown, record the gap. Use critical-payload hashes; an external APK filename does not identify the build.

## 2. Inventory boundaries

| Area | Fields to collect |
| --- | --- |
| Boot | Libraries, constructors, JNI_OnLoad, NativeLoader and Activity entry |
| Runtime | Mono/IL2CPP, metadata/assemblies and version |
| Graphics | Built-in/URP, serialized API, shaders/variants, physical context and extensions |
| Textures | Resident format, dimensions, mips, alpha, use and updates |
| Audio | FMOD, OpenSL, AudioTrack, AAudio; calls, format and callbacks |
| Input | InControl, Rewired, Input System, custom code or touch; consumers |
| Data | StreamingAssets, bundles, split/OBB, layout and required preparation |
| State | Preferences, saving, pause/resume and exit |

An APK can contain variants that are not selected. Distinguish stored format from the format actually uploaded to the GPU. Evaluate ETC1/dual in every Mali-450 triage, with documented exceptions in [textures](GRAPHICS-AND-TEXTURES.md).

## 3. Choose references by profile

Use the [Unity index](../README.en.md) and [SOURCE-MAP.json](../SOURCE-MAP.json) hashes. Compare version, ABI, problem and contract, then inspect the public implementation. Do not bring excluded cases into this edition.

Examples: Suzy Cube helps with native GLES2/ETC1 and 2017 lifecycle; Freedom Planet 2 with Vulkan-program translation; Huntdown requires separating its two profiles; Party Hard explains directional ownership; Nameless Cat shows firmware-SDL integration.

Historical solution documentation does not automatically bind a public HEAD to an approved binary. Record the difference and request exact evidence only when necessary for adaptation.

## 4. Preserve Android flow

Document loading order, relocations, constructors, JNI, surface/focus/resume configuration and native frame entry. The adapter follows the build's actual flow. Do not call “similar” methods from another version or skip initialization to reach a scene.

Do not return generic handles for missing libraries. Falsely advertising AAudio or returning an invalid JNI object can postpone failure until audio or loading. Optional absence follows the expected error; required absence becomes an identified blocker.

## 5. Produce an actionable result

Deliver identity, selected references, contract table, first suspected boundary, a measurement to confirm/refute it and the next local change. “Feasible” must state what exists and what remains to implement; it does not mean playable.

Mali-450 uses physical GLES2. `-force-gles20` does not create missing variants, shaders or operations. Without evidence of pixels, sound, actions, saving and exit, the corresponding aspects remain partial. Use [boundary diagnostics](DIAGNOSTICS.md).

## AI mission

```text
Confirm the Unity build and inventory each boundary. Use NextOS references
admitted to the catalog and pin hashes/licenses. Preserve native flow,
firmware SDL and physical GLES2. Plan ETC1/dual textures by use, implement only
the new port adapter, and record measurement, repair and countercheck.
```
