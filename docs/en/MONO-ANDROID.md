# Mono Android, MonoGame and .NET runtimes

[Português](../pt-BR/MONO-ANDROID.md)

This guide explains how to choose an execution route for Android games with managed code, preserve bootstrap order and diagnose native bridges. No single “copy the DLLs and run mono” recipe covers all these builds.

## 1. Identify the actual runtime

| Evidence in the local copy | Hypothesis to confirm | Next decision |
| --- | --- | --- |
| `libmonodroid`, `libmonosgen`, assembly store, `mono.android` classes | Mono/.NET for Android | Study the Android runtime chain |
| `libunity` and managed assemblies | Unity with Mono | Use the Unity guide and exact version |
| `libunity` + `libil2cpp` + metadata | Unity IL2CPP | Managed code converted to native; a different route |
| MonoGame/FNA assemblies and compatible desktop dependencies | Possible managed Linux host | Prove the entrypoint and every dependency |
| Godot with C# | Godot/.NET | Follow Godot's version and .NET host |

These names are clues. Confirm versions, ABIs, dependencies and assembly storage. Assemblies may be contained, compressed or paired with AOT images; a `.dll` extension alone does not establish enough IL for JIT.

## 2. Choose between retaining Android runtime and using a Linux host

**Route A: Android compatibility.** Load the native chain belonging to the owner's build, implement Bionic/JNI/Android, and let the original bootstrap initialize the managed runtime and Activity. [Stardew Valley](../../ports/stardewvalley-nextos/README.en.md) and [ScourgeBringer](../../ports/scourgebringer-nextos/README.en.md) provide public code for investigating this route.

**Route B: managed Linux host.** Valid only when the entrypoint, assemblies, BCL and native libraries actually work outside Android with documented adaptations. [SOR4](../../ports/sor4-nextos/README.en.md) and its MonoGame patches show other boundaries; do not transplant one game's bootstrap merely because both use C#.

The [Mono embedding manual](https://www.mono-project.com/docs/advanced/embedding/) describes initialization, assembly loading and managed/native calls for compatible applications. That API does not automatically replace `Java_mono_android_Runtime_init`, Java class registration, Activity or Android-build packaging.

## 3. Map initialization before calling Game.Run

Document library order and constructors/JNI, runtime configuration, assembly storage, type/assembly registration, Activity/OnCreate, view creation and loop startup. The selected Stardew README describes `libmonosgen-2.0.so → libxamarin-app.so → libmonodroid.so` and Android bootstrap; that order belongs to that profile.

Calling `Game.Run()` early can skip required initialization. Load dependencies through the correct loader: do not blindly hand a Bionic ELF to glibc `dlopen`. If a reference rejects AOT to use JIT, verify that the new runtime permits this fallback and has the required IL before reusing that decision.

## 4. Resolve common failure boundaries

| Boundary | Measure | Diagnostic example |
| --- | --- | --- |
| Bionic/glibc | Constants, structures, alignment and errno | Wrong `sysconf` enum reports an invalid page size |
| Threads/GC | Semaphores, TLS, suspension and ABI-specific signals | Incorrect `sem_t` size corrupts memory |
| JNI/Java | Signatures, references, classes and callbacks | Reaching Activity does not prove late Java services |
| P/Invoke | Actual name, ABI and resolution of each library | Wrong provider appears only when opening a menu feature |
| Assemblies | Identity, version, integrity and dependencies | Mixed BCL/runtime breaks types or methods |
| EGL/GL | Actual context and desktop GL/GLES classification | Incorrect desktop-library discovery changes MonoGame's route |

Start with Stardew's `src/bionic_shims.c`, `src/pthread_bridge.c`, `src/jni_shim.c` and `src/sdv_egl_bridge.c`, and ScourgeBringer's shims/AAudio bridge. Check included files in the manifests. Do not replace all of `sysconf` with another device's constants or disable GC/jobs as a general repair.

## 5. Make the first build

Follow [ARM compilation](BUILD-ARM.md) for the Linux executable; then read the selected port's build script and list its toolchain, generated sources and missing dependencies. A C# source project may have a separate managed build; that does not rebuild the owner's proprietary assemblies or authorize committing them.

Pin runtime/BCL and native-library versions. Audit GLIBC for every redistributed Linux dependency. For public SOR4, the reference recipe is `port/wwise-native/build-glibc230.sh` in `sor4-nextos`; a historical recipe from another tree is not equivalent.

## 6. Check graphics, audio and input

Preserve the graphics API actually selected by the runtime and a single context/present owner. Audio requires its specific backend: XACT/OpenAL, FMOD/AAudio or another route are not interchangeable. Record sample rate, format, channels, callbacks and consumed queue, then listen on the device.

Deliver gamepad input to the actual Android/MonoGame path and observe the managed consumer. An enqueued SDL event does not prove an action. Exercise horizontal/vertical menus, gameplay, pause, on-screen keyboard, hotplug and exit; avoid duplicating mouse/keyboard/gamepad without a contract.

## 7. Prepare data and close the evidence

NXExtract must prepare assemblies/store, libraries and content from the owner's complete copy. Required store migration must also occur on a clean installation and preserve saves during updates. [Recipe and clean test](NXEXTRACT.md).

Record runtime profile, artifact/SHA, Activity entry, first frame, audio, input, save/reload and shutdown. The ScourgeBringer snapshot records glibc 2.30 validation limitations: a host test or another firmware cannot establish acceptance on that target. Always read the selected commit's status.

## AI mission

```text
Identify the managed runtime and its complete inputs. Compare Android and
Linux routes by contract, not by the .NET name. Document bootstrap, Bionic,
JNI, P/Invoke, assemblies and AOT/JIT. Implement the adapter in a separate
project, preserve data and references, build for ARM and run targeted tests.
Record unproven contracts and the NXExtract installation flow.
```
