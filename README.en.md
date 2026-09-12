# NextOS Framework

[Português](README.md)

iOS ports, studies and examples are outside this collection’s scope.

V5 framework for Android-to-Linux ARM ports, with **expanded English and Portuguese guides**, selected sources for **44 titles**, and **2 additional community catalog ports**. AI can lead most investigation, coding, building and verification.

**Private review repository. Public visibility requires explicit NextOS approval.** This collection contains no commercial game data and does not represent 46 certified installable packages.

Collection/integration author: **NextOS** · [official GitHub](https://github.com/NextOs-Ports).

## Start here

1. [Getting started](docs/en/GETTING-STARTED.md) and [complete guide index](docs/en/README.md).
2. [Let AI lead the port](docs/en/AI-PORTING.md).
3. [Public ARM SDK](toolchains/sdk/README.en.md) and [integrated first port](examples/first-port/README.en.md).
4. [Shims](docs/en/SHIMS.md), [NXExtract](docs/en/NXEXTRACT.md) and [testing](docs/en/TESTING.md).

## Engine guides

[Choose Android runtime](docs/en/ANDROID-RUNTIMES.md). Game references use only public ports already selected in the collection.

| Track | What it teaches |
| --- | --- |
| [Unity](portando_unity/README.en.md) | Triage, lifecycle, GLES2, ETC1/dual, input, audio and 15 public cases |
| [Mono Android](docs/en/MONO-ANDROID.md) | Mono/.NET, MonoGame/FNA from the Android build, assemblies and bootstrap |
| [Godot](docs/en/GODOT.md) | Engine/export, renderer, C#, viewport and InputMap |
| [Cocos2d-x](docs/en/COCOS2D-X.md) | C++ library, JNI, assets, text, audio and native loop |
| [GameMaker Android](docs/en/GAMEMAKER-ANDROID.md) | YoYo runner, VM/YYC, ABI and data |
| [Ren’Py Android](docs/en/RENPY-ANDROID.md) | Python, SDL/JNI, assets and persistence |
| [Haxe/hxcpp/Lime Android](docs/en/HAXE-LIME-ANDROID.md) | Bootstrap, threads, TLS/GC and limits |
| [C/C++ Android](docs/en/NATIVE-ANDROID.md) | JNI, NativeActivity, Android SDL and Bionic |
| [Freedom Planet 2: Vulkan → GLES2](portando_unity/en/FP2-VULKAN-GLES2.md) | SMOL-V/SPIR-V, program translation, surgical writes and stencil/alpha |

## First example

```sh
cmake -S examples/shims-reference -B work/host -DCMAKE_BUILD_TYPE=Release
cmake --build work/host --parallel 2
ctest --test-dir work/host --output-on-failure
```

This is the introductory C test on the computer. Next, the [integrated original minigame](examples/first-port/README.en.md) teaches ARM64 compilation, loading an Android guest, NXExtract installation and 11 checks under CPU emulation. The [public SDK](toolchains/sdk/README.en.md) supplies its build environment; physical graphics, sound and controls still require device testing.

For another game, use the [executable inventory](docs/en/ANDROID-INVENTORY.md), [pins and sources](docs/en/PINS-AND-SOURCES.md), [engine exercises](examples/engines/README.en.md) and [shader laboratory](examples/shader-lab/README.en.md).

## Catalog and sources

[46 NextOS titles](catalog/README.en.md): 44 with source selections from 40 public repositories and 2 community cards — Stranger Things 3 and AVGN I & II Deluxe, without imported code yet. Freedom Planet 2 is already among the 44. Each reference retains its own status, origin and limitations.

| Directory | Contents |
| --- | --- |
| `framework/` | Preserved V5 components/templates |
| `suportando_outros_devices/extrator-universal/` | Pinned NXExtract: engine, runner and UI |
| `ports/` | Code snapshots, PT/EN cards and manifests |
| `portando_unity/` | Selected bilingual edition, cases and generic tools |
| `docs/pt-BR/`, `docs/en/` | Expanded matching guides |
| `examples/`, `toolchains/` | Integrated examples, engines, shaders and public SDK |
| `publication/` | Integrity, validation, languages and pending work |

V5: `framework-v5` @ `657fb65a23b5c3b20040e76307b27e6470b1d17c`. [Exported hashes](publication/v5-export.json). Twelve historical tests with private dependencies were omitted; runtime bytes were preserved. Existing ports retain V3/V4/V5/V6 pins; no migration occurred.

## Contribute and redistribute

Read [AGENTS.md](AGENTS.md), [contributing](CONTRIBUTING.en.md) and [licensing/credits](LICENSING.en.md). Prepare data only from the owner's copy; never upload APK, IPA, OBB, original libraries, assets or saves to GitHub/CI. ZIPs follow each component's license and preserve NextOS/third-party credits. The noncommercial proposal remains under review and does not replace existing GPL/MIT permissions.

Editorial guides are bilingual; historical source, comments and normative licenses retain their original language. [Update and checks](publication/ONBOARDING-UPDATE.en.md) · [Work before publication](publication/REVIEW.en.md).
