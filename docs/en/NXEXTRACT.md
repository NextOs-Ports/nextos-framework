# NXExtract: owner data and clean installation

[Português](../pt-BR/NXEXTRACT.md)

A BYO-data port distributes permitted code/runtime, the complete framework, a recipe and NXExtract with its canonical graphical UI. Data comes from the owner's compatible copy. This also applies to test ZIPs in private repositories.

## 1. Define input identity

Record game name/version, package ID, ABIs, and the tested reference APK's size and SHA-256. Identify critical libraries, metadata, bundles and companion files. Split inputs must be complete before testing; a fixture APK missing required payloads does not prove extraction.

The whole-container SHA identifies the tested copy. Recipe compatibility must also validate package ID, version contract, ABI, structure and critical payloads. Do not reject solely because a container's signature, member order or external filename changed when its compatible content remains the same.

## 2. Write the port recipe

Read the [canonical minimal example](../../suportando_outros_devices/extrator-universal/examples/recipe-minimal.json) and the `Recipe` class in `nxextract.py`. The minimal example is structural; a real game's identity needs additional validation.

| Field/area | Decision to document |
| --- | --- |
| `id`, `version`, `title` | Stable recipe identity and revision |
| `abi_order` | Only implemented ABIs; AArch64 first |
| `input.search_dirs` | `gamedata` first, following generator policy |
| `extract` | Internal paths, destinations and per-payload validation |
| Hooks | Required transformations, sources, tools and hashes |
| `validate` | Final checks of data consumed by the runtime |
| `commit` and marker | Transactional publication and installation identity |

Do not invent key names from this table: use the pinned version's schema/validator and examples. Hooks must be defined, reviewed executables without arbitrary shell hidden in JSON. Shader/texture conversions must preserve untouched objects and produce verifiable outputs.

## 3. Check structure before using a device

After creating `work/ports/demo/extractor.json` with your port's actual information, inspect available commands and validate the recipe:

```sh
python3 framework/nxgenerator/nxgenerator.py --help
python3 framework/nxrelease/nxrelease.py --help
python3 suportando_outros_devices/extrator-universal/nxextract.py recipe-check \
  --recipe work/ports/demo/extractor.json
```

`recipe-check` validates the recipe; it does not install data or create graphical proof. Generate the new port tree with `nxgenerator.py <nxproject.json> --source-root <port-root> --output <new-directory>` after completing the manifest, runtime and pins. Output must be new; never generate over an approved port.

## 4. Describe installation in both languages

Every port ZIP includes `<port-id>/INSTALLATION.md`, with Portuguese and English in the same file. Explain where the launcher and port directory belong, where to place the owner's copy, compatible version/ABI, reference size/SHA, the first-launch screens and where to find errors.

Do not publish an original APK filename revealing its download origin, or download sites, groups or distributors. Identify content through technical fields. Do not include APK/IPA/OBB, original libraries, assets, saves or converted game data in the ZIP.

## 5. Test from scratch with the final candidate

Use the same immutable ZIP that will be delivered and an isolated installation without prepared data or an old marker. Preserve existing installations and saves. Supply the complete input, open the canonical launcher and observe discovery, graphical UI, extraction, hooks, validation, receipt and NXSplash before runtime.

Compare final hashes against the approved reference. Record ZIP/SHA, recipe, complete input, UI version, visible renderer and outcome. Existing-data adoption, an old receipt, a live PID or an isolated `ui.ready` file does not prove this flow. Applicable proofs must preserve the UI layout/colors at 640×480 and 1280×720.

Temporary UI controls belong in a private runtime session, not on the game's FAT/exFAT card. Do not weaken permissions to accept an unsafe handshake. The recipe must not rebuild the approved executable.

## 6. Separate diagnosis from acceptance

Record identity, space/structure, hook, renderer and validation errors separately. A success receipt must belong to that attempt. See [testing and delivery](TESTING.md) to bind proof to final bytes. This documentation does not claim a newly tested commercial installation.
