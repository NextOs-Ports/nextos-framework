# NextOS guides in English

[Português](../pt-BR/README.md)

[Repository](../../README.en.md) · [46-title catalog](../../catalog/README.en.md) · [Porting Unity](../../portando_unity/README.en.md)

Start with the host C example and advance to the [integrated first port](../../examples/first-port/README.en.md), using the [public SDK](../../toolchains/sdk/README.en.md). Then choose the engine. Guides provide commands, contracts and criteria for AI-led implementation; compatible owner-supplied data remains private.

| Guide | Contents |
| --- | --- |
| [Getting started](GETTING-STARTED.md) | Clone, tools and first test |
| [Architecture](ARCHITECTURE.md) | Components, responsibilities and pins |
| [AI porting](AI-PORTING.md) | Mission, inventory and autonomous implementation |
| [Build ARM](BUILD-ARM.md) | Host, AArch64, ARMv7, NDK and ELF |
| [Pins and sources](PINS-AND-SOURCES.md) | Exported composition and public-file recovery |
| [Android inventory](ANDROID-INVENTORY.md) | Manifest, imports, ABI, blockers and reference search |
| [Diagnostic laboratory](DIAGNOSTIC-LAB.md) | Good/bad logs and executable negative cases |
| [Shims](SHIMS.md) | ABI, JNI, ownership and tests |
| [Mono Android](MONO-ANDROID.md) | Mono/.NET, MonoGame/FNA and bootstrap |
| [Godot](GODOT.md) | Engine, export, renderer, C# and input |
| [Cocos2d-x](COCOS2D-X.md) | JNI, assets, text, rendering and audio |
| [NXExtract](NXEXTRACT.md) | Recipe, owner data and clean installation |
| [Testing and delivery](TESTING.md) | Evidence, candidate and release |
| [Troubleshooting](TROUBLESHOOTING.md) | Symptoms and targeted diagnostics |

## Specialized tracks

- [Unity](../../portando_unity/README.en.md): 15 public-source cases, two community records and synthetic tools.
- [Freedom Planet 2 — Vulkan to GLES2](../../portando_unity/en/FP2-VULKAN-GLES2.md): per-program conversion and data preservation.
- [Engine exercises](../../examples/engines/README.en.md): Mono, Godot, Unity and Cocos2d-x, with explicit build status.
- [Shader laboratory](../../examples/shader-lab/README.en.md): original shaders, public pins and a compute negative case.
- [Shim example](../../examples/shims-reference/README.en.md): small buildable contract.
- [Licensing and credits](../../LICENSING.en.md), [contributing](../../CONTRIBUTING.en.md), [validation](../../publication/VALIDATION.en.md).

## Languages and historical sources

All editorial documentation in this publication has equivalent PT/EN versions. Top links switch languages. Identifiers, paths, schemas and commands remain stable; prompts and explanations are translated.

Historical files in `framework/`, the NXExtract tree and `ports/*/upstream/` retain original bytes, licenses and languages. Bilingual cards and guides live outside those immutable trees. Code comments, old tool messages and normative legal texts have not been translated as if they were a new license.
