# Create a port with AI leading the work

[Português](../pt-BR/PORTAR-COM-IA.md)

Before inventing new tools, use [Android inventory](ANDROID-INVENTORY.md), [pins and sources](PINS-AND-SOURCES.md), and the [diagnostic laboratory](DIAGNOSTIC-LAB.md). The [first port](../../examples/first-port/README.en.md) gives the assistant a demonstrated run for comparing each boundary.

The assistant should investigate, write code, compile, run authorized checks and leave reproducible results. The owner supplies compatible inputs, chooses the target, provides necessary physical feedback and approves publication. A question is not needed for every reversible local adjustment.

## 1. Give a concrete mission

Copy this template and fill in what you know. An unknown field becomes an investigation task, not permission to invent data:

```text
Read AGENTS.md and docs/en/README.md in this clone.
Game and version: [provide]
Local Android copy supplied by me: [private path]
Target for this stage: [system, CPU/GPU and userland ABI]
New repository/directory: [destination]
First objective: identify the build and reach its native flow on the target.

Autonomously handle inventory, public-reference selection, adapter
implementation, build and relevant local tests.
Prefer AArch64. Preserve V5 and every reference port.
For Unity, read portando_unity/README.en.md and use only its public cases.
Read docs/en/ANDROID-RUNTIMES.md and choose one of the eight Android tracks.
Use only already selected public ports, with pinned commit and evidence.

Keep sources, pins, contracts, private logs and results organized.
Do not invent offsets, signatures, support, licenses or successful missing APIs.
Implement and test what you can; describe any blocker precisely.
Do not upload game data to GitHub. Do not access a device without an address
authorized for this task. Prepare reviewable material before requesting publication.
```

## 2. Inventory before choosing a loader

Create a private report with game/version, package ID, reference container size/SHA, ABIs, critical libraries and hashes, actual engine/runtime, dependencies, asset paths, shaders/textures, audio and input. For split packages, identify every required container. Do not confuse game version and engine version.

This command only lists libraries inside an explicitly supplied APK; it neither extracts nor executes content. Package ID and version need a suitable Android binary-manifest reader; `strings` alone does not establish those fields.

```sh
export NEXTOS_OWNER_APK=/private/owner-input/game.apk
python3 - <<'PY'
import os
from zipfile import ZipFile
with ZipFile(os.environ['NEXTOS_OWNER_APK']) as apk:
    for item in apk.infolist():
        if item.filename.startswith('lib/') and item.filename.endswith('.so'):
            print(item.filename, item.file_size)
PY
```

Do not commit raw output containing private information. Read commercial data only in the authorized environment; do not attach it to external AI services.

## 3. Choose references by contract

Open the catalog and `SOURCE-MAP.json`. Record why the reference fits: compatible engine/build, ABI, JNI, audio, renderer, input and license. The same function may have a different signature in another build. Check whether the cited implementation is in the selected files and whether its evidence refers to the same artifact.

| Finding | Track |
| --- | --- |
| Unity (Mono or IL2CPP) | [Porting Unity](../../portando_unity/README.en.md) |
| Mono/.NET Android, including MonoGame/FNA from the Android build | [Mono Android](MONO-ANDROID.md) |
| Godot project/runtime | [Godot](GODOT.md) |
| Cocos2d-x and Android callbacks | [Cocos2d-x](COCOS2D-X.md) |
| GameMaker Android | [GameMaker Android](GAMEMAKER-ANDROID.md) |
| Ren’Py Android | [Ren’Py Android](RENPY-ANDROID.md) |
| Haxe/hxcpp/Lime Android | [Haxe/hxcpp/Lime Android](HAXE-LIME-ANDROID.md) |
| C/C++ Android | [C/C++ Android](NATIVE-ANDROID.md) |

## 4. Organize small, verifiable deliverables

In the **new** port repository, maintain an inventory, contract table, adapter source, build recipe, NXExtract recipe and test record. Suggested names: `docs/inventory.md`, `docs/contracts.md`, `src/`, `recipes/`, `docs/validation.md`. These names are organizational suggestions; actual generator schemas remain authoritative.

For each contract, record import/signature, source origin, license, memory/thread owner, error behavior and test. For each run, record commit, ELF SHA, data profile, target and outcome. Private information stays outside publishable documentation.

## 5. Preserve native order and fix one boundary at a time

Start with libraries/relocations and constructors, then initialization JNI/callbacks and lifecycle, followed by context/surface, frames, audio, input, persistence and exit. The exact order comes from the engine under investigation, not a universal list of function names.

On failure, record a hypothesis, discriminating measurement, minimal repair and countercheck. For example, live audio with a black screen requires measuring the graphics boundary; it is not partial video success. An unknown import needs its contract implemented; a generic `return 0` hides the problem.

## 6. Automate without losing evidence

AI can generate inventories, compare hashes, locate symbols, write targeted tests and prepare packages. Preserve an approved executable; documentation or translation does not authorize rebuilding it. Finish changes before the final test batch, without creating a new ZIP for every development error.

A useful checkpoint records what changed, the command, result, remaining work and next independent step. When resuming, the assistant should read that checkpoint and compare files rather than rediscover proven corrections.

## 7. Finish with accurately scoped claims

Deliver source and recipe, provenance, reproducible build, missing-contract diagnostics, [clean installation](NXEXTRACT.md) and [final-byte evidence](TESTING.md). “Built”, “opened the menu” and “complete gameplay” are different results. Report only observations.

If an input or device is missing, finish independent work and identify the exact untested boundary. This repository remains private until NextOS explicitly approves publication.
