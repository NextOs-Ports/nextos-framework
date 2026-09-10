# Original exercises by engine

[Português](README.md)

Small examples for investigating a boundary in each family. These are training projects, not commercial ports or replacements for the [integrated ARM64 example](../first-port/README.en.md). Copy editor projects into `work/` before opening them to keep caches and builds outside source directories.

## Mono: layout, callbacks and P/Invoke

[Program.cs](mono/Program.cs) calls original [native.c](mono/native.c) with sequential layout, Cdecl, a retained callback and error propagation. Prepare Mono/mcs and a C compiler on the host:

```sh
mkdir -p work/mono-training
cc -shared -fPIC -O2 examples/engines/mono/native.c   -o work/mono-training/libnextos_training.so
mcs -out:work/mono-training/Training.exe examples/engines/mono/Program.cs
mono work/mono-training/Training.exe
```

Observed result: `PASS: managed/native layout, callback lifetime and error propagation`. This exercise is a managed Linux host. **It does not implement Mono Android/Xamarin, assembly stores, Bionic or Activity.** Those steps belong to the [Mono Android guide](../../docs/en/MONO-ANDROID.md); distinguishing the two routes is part of the lesson.

## Godot: an original project with testable logic

Use [official Godot 3.5.3](https://godotengine.org/download/archive/3.5.3-stable/). Set `NEXTOS_GODOT` to your executable path. The headless version tests logic; the graphical editor/runner opens the project using GLES2.

```sh
mkdir -p work/engine-training
cp -R examples/engines/godot work/engine-training/godot
"$NEXTOS_GODOT" --path work/engine-training/godot --script test_model.gd
```

Observed result: movement, delta bounds and collection logic PASS. To play, open the copy in Godot 3.5.3 or run the graphical executable with `--path` and without `--script`. Arrows move; Escape saves/exits. Do not use headless execution to claim graphics. [project.godot](godot/project.godot) selects GLES2; Linux/ARM export requires corresponding templates and the tests described in the [Godot guide](../../docs/en/GODOT.md).

## Unity: create your own Android input

Install and license Unity 2019.4.40f1 with Android/IL2CPP modules and supported tools. Set `NEXTOS_UNITY_EDITOR` to the editor path. The project contains only original scripts:

```sh
mkdir -p work/engine-training
cp -R examples/engines/unity work/engine-training/unity
"$NEXTOS_UNITY_EDITOR" -batchmode -quit   -projectPath "$PWD/work/engine-training/unity"   -executeMethod TrainingBuild.Android -logFile -
```

[TrainingBuild.cs](unity/Assets/Editor/TrainingBuild.cs) creates the scene, selects IL2CPP/AArch64/GLES2 and generates `Build/training.apk` in the work copy. [Training.cs](unity/Assets/Training.cs) draws the game and saves through PlayerPrefs. **This editor build was not executed in this review.** Installing/licensing the editor and modules is outside the C SDK. Porting this input still requires Unity investigation and its adapter; it is not the first lesson's freestanding guest.

## Cocos2d-x: a scene to integrate with the template

[TrainingScene.cpp](cocos/TrainingScene.cpp) and its [header](cocos/TrainingScene.h) implement drawing, keyboard input, collection and UserDefault. Use the upstream C++ Cocos2d-x 3.17.2 template, commit `1528ea01d2749b4ef65e97f79cffa6135fe13c4d`, in a new project with its dependencies. The [upstream CMake recipe](https://github.com/cocos2d/cocos2d-x/blob/cocos2d-x-3.17.2/templates/cpp-template-default/CMakeLists.txt) identifies target sources.

Copy both files into `Classes/`; add `Classes/TrainingScene.cpp` to `GAME_SOURCE` and the header to `GAME_HEADER`. Include `TrainingScene.h` in `AppDelegate.cpp` and replace initial scene creation with `TrainingScene::create()`. Use design resolution 640×480 and preserve the template's GLView/lifecycle setup. Build through the template's CMake project; Android also requires that engine version's Gradle/NDK project.

**Cocos integration was not compiled in this review.** It needs upstream checkout/dependencies and its own graphical testing. The [Cocos2d-x guide](../../docs/en/COCOS2D-X.md) covers Android adaptation, JNI, text and audio not demonstrated by an isolated original scene.
