# Port Godot games from Android to Linux ARM

[Português](../pt-BR/GODOT.md)

The [original Godot project](../../examples/engines/README.en.md) includes complete sources and a headless logic test. Rendering and ARM export need their own templates and validation; porting an Android build still follows the investigation below.

First identify the version, project format and extensions required by the runtime. A Godot APK may allow a compatible Linux-engine route, but an isolated PCK does not establish independence from Android plugins, C# or native libraries.

## 1. Inventory the project

Record Godot major/minor/patch and any custom build; GDScript or C#; PCK/exported project; imported resources; Android plugins; GDExtension/GDNative; renderer; and audio, video and input dependencies. Confirm the version from data/runtime rather than a directory name.

| Finding | Consequence |
| --- | --- |
| Godot 3 with GLES2 | Investigate a compatible template from that generation |
| Godot 4 Compatibility | Do not assume physical GLES2 in the default driver |
| Forward+/Mobile | Features may exceed the target's GL path |
| C# | Pin engine, assemblies and compatible .NET runtime |
| Android plugin | Adapt/implement its contract or record a blocker |
| Native extension | Requires a compatible Linux/ABI build or authorized equivalent |

Godot separates rendering method from graphics driver; Compatibility uses the OpenGL family, while Forward+/Mobile use RenderingDevice. Switching methods can change features and appearance. [Official renderer overview](https://docs.godotengine.org/en/stable/tutorials/rendering/renderers.html).

## 2. Choose a compatible route

If you own the source project, prepare a Linux export for the correct architecture with matching template and resources. The export system combines project content in a PCK with an executable; select the architecture explicitly. [Godot Linux export](https://docs.godotengine.org/en/stable/tutorials/export/exporting_for_linux.html).

If you only have the owner's Android copy, inventory exported content and determine whether an exact Linux engine can consume it. Do not invent sources, keys, plugins or format compatibility. Compiled GDScript, texture imports and assemblies may tie a build to a specific version.

## 3. Read the public reference

[Tearscape](../../ports/tearscape-nextos/README.en.md) contains Godot 4.6.1/.NET integration sources, video providers, input and data preparation. Read the snapshot README, `build_low_glibc.sh`, `src/shim/` and files listed in the manifest before reusing anything.

The snapshot records physical validation pending for recent revisions. Inclusion here does not promote it to an approved universal baseline. Study the architecture and preserve any already approved artifact in the port you are working on. A physically proven GLES2 route must enter the official build and pins; do not leave it in an experimental directory ignored by packaging.

## 4. Prepare engine and build

Separate the Linux engine, Linux libraries, .NET runtime where needed, and owner data. Pin engine commit and patches, SDK/sysroot, build settings and dependency hashes. Check architecture and GLIBC of every Linux ELF using the [ARM guide](BUILD-ARM.md).

Do not run Tearscape's build merely as an experiment: its recipe has frozen inputs and a one-build rule. For a new port, first read build-system options for the exact version and create an independent recipe. The collection does not include a ready-made universal ARM Godot SDK or a commercial demonstration PCK.

For an **original project** already prepared and a compatible editor installation, this is how to inspect and use the CLI; `Linux ARM64` must exist as a preset with the correct template installed:

```sh
export NEXTOS_GODOT=/opt/godot/godot
"$NEXTOS_GODOT" --version
"$NEXTOS_GODOT" --help
mkdir -p work/godot-export
"$NEXTOS_GODOT" --headless --path "$PWD/work/godot-project" \
  --export-release "Linux ARM64" "$PWD/work/godot-export/demo-nextos"
```

This command requires that project/preset: it does not run directly from an unprepared APK. CLI export is different from compiling the engine. [Official command-line reference](https://docs.godotengine.org/en/stable/tutorials/editor/command_line_tutorial.html).

## 5. Preserve window, viewport and image

Measure panel/drawable size, window, logical viewport and content rectangle separately. Changing logical height can change the game's camera zoom. Letterbox or stretch must respect approved behavior and an explicit owner setting.

On Mali-450, preserve physical GLES2. A translation facade requires actual coverage of shaders, textures, FBOs, blending and copy operations; changing a GL string does not implement a renderer. Compare title, gameplay, transitions, transparency and effects. A black screen with audio is a terminal graphics failure.

## 6. Input, audio, C# and saves

Input actions must reach actual `InputMap` sinks. If a map is advertised as editable, prove that editing an action changes runtime behavior; shipping the file is insufficient. Preserve controller identity, co-op and context transitions.

Test audio provider and shutdown. For C#, validate .NET version, BCL, assemblies and P/Invoke; see [Mono/.NET](MONO-ANDROID.md) to distinguish runtimes. Preserve `user://` and save migrations; prepared data and saves have different lifecycles.

## 7. Verifiable delivery

Create an [NXExtract recipe](NXEXTRACT.md) for the exported project, assemblies and strictly necessary transformations. Test clean installation with complete input, then the initial scene, progress, transitions, audio, input, save/reload and exit. Record engine version, executable SHAs, data profile and devices actually tested.

## AI mission

```text
Identify Godot and the exported format, renderer, C#/extensions and Android
plugins. Choose a compatible Linux engine and pin sources, patches and runtime.
Implement providers/adaptations in the new port while preserving the approved
GLES2 route, viewport and InputMap. Document inputs, build, NXExtract and real proof.
```
