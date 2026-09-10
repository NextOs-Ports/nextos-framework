# Cocos2d-x Android on Linux ARM

[Português](../pt-BR/COCOS2D-X.md)

Cocos2d-x often concentrates engine and game logic in a C++ library, but also depends on Java/JNI for lifecycle, assets, text, audio, services and input. Reconstruct these boundaries while preserving the game's native loop and order.

## 1. Confirm engine and ABI

Look for `cocos2d` symbols, `org.cocos2dx.lib` classes, renderer JNI methods and C++ dependencies. Confirm version/fork, ABI, main library, `libc++_shared.so`, assets and audio. Cocos Creator games or JavaScript-based forks may have a different architecture; do not automatically classify them as the same native Cocos2d-x path.

Use `readelf -h`, `readelf -d` and `readelf -Ws` on local libraries. Include imports and callbacks registered dynamically. Prefer AArch64 when present; ARMv7 requires review of ABI, structures and floating-point calls.

## 2. Study a public reference with a defined scope

[Chrono Trigger](../../ports/chrono-nextos/README.en.md) documents Cocos2d-x 3.14.1/GLES2, an AArch64 loader, `Cocos2dxBitmap`, OpenSL ES and native input. [Geometry Dash/SubZero](../../ports/geometrydash-nextos/README.en.md) provides another source selection with distinct profiles.

Read Chrono's `SOURCE-MAP.json`, README, `src/main.c`, `src/jni_shim.c`, `src/text_render.c`, `src/opensles_shim.c` and `src/ct_framework.c`. Specific names, hooks and pins belong to that port. Catalog inclusion does not automatically turn the combination into V5.

## 3. Reconstruct boot order

In the documented Chrono profile: load and initialize the C++ dependency; load/relocate the game library; install proven hooks; run constructors exactly once; call `JNI_OnLoad`; publish context/assets; `nativeInit`; resume; event/render loop; pause/save and exit.

Check the target build's Java/JNI order. The [upstream Cocos2dxRenderer](https://github.com/cocos2d/cocos2d-x/blob/v3/cocos/platform/android/java/src/org/cocos2dx/lib/Cocos2dxRenderer.java) illustrates Android callback driving; the game's vendored version takes precedence. Do not call `nativeInit` before making its required context available or run constructors twice.

## 4. Build the adapter by boundary

| Boundary | Implementation to prove |
| --- | --- |
| Assets | Correct path/case lookup, compressed files, read/seek/close |
| JNI | Actual classes, signatures, strings/arrays and callbacks; explicit missing errors |
| Text | Font, size, alignment, Unicode, bitmap pitch/format and alpha |
| GLES | Correct context on the render thread, state and single present |
| Audio | OpenSL objects/vtables, BufferQueue and buffer lifetime |
| Input | Gamepad/touch/keyboard actually consumed by the game |
| Persistence | Writable directory, preferences, pause, save and shutdown |

`Cocos2dxBitmap` deserves its own test: invisible text can result from dimensions, stride, alpha channel or upload callback even when other sprites appear. Do not use an empty string or fixed bitmap to hide a missing method.

## 5. Build without mixing C++ runtimes

The loader is Linux and follows the [ARM guide](BUILD-ARM.md); the original library remains Android. The guest C++ dependency must follow the build's contract with controlled resolution to avoid host-runtime collisions. Do not arbitrarily replace `libc++_shared.so` with Linux `libstdc++`.

Read Chrono's `build_universal.sh` as a specific recipe, checking images/SDKs and missing sources before execution. In the new port, preserve audited flags, architecture, relocations, visibility and GLIBC. Do not package while still discovering imports.

## 6. Diagnose the first image

If the engine does not draw, confirm native render was reached with complete data and a current context. If it draws but appears black, compare shader/link, texture/alpha, FBO and pixels immediately before present. A black object with the correct silhouette may have incorrect sampler/wrap state; do not impose global CLAMP on all materials.

If RGB is correct but scanout becomes transparent/black in a compositor, test final alpha without destroying RGB or scissor/clear state. Condition repairs on measured capabilities/defects rather than a device name alone.

## 7. Test complete behavior

Test text in available languages, menus, level loading, gameplay, transitions, audio, pause/resume, native input, hotplug, save/reload and exit. Do not overlay a keyboard mapper that takes over native input. For touch, transform coordinates according to the content rectangle with coherent press/move/release and context.

Prepare [NXExtract](NXEXTRACT.md) for critical libraries and assets from the owner's copy, with real validation and no bundled game data. Record [final-artifact proof](TESTING.md). Translated menus or a first frame do not establish every scene.

## AI mission

```text
Identify the Cocos2d-x fork, ABI, C++ library, JNI and renderer lifecycle.
Use Chrono and Geometry Dash as public references by contract. Implement
assets, text, audio and input in the new adapter, preserving constructors and
lifecycle. Build for Linux ARM, run targeted tests and create a data-free NXExtract recipe.
```
