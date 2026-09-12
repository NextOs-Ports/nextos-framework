# Pins, provenance and source recovery

[Português](../pt-BR/PINS-E-FONTES.md)

A historical tag identifies origin. This new repository has its own history and lacks the original V5 commit object. The procedure preserves both identities without recreating the tag.

## 1. Materialize the frozen selection

Use Python 3.11+ and Git 2.29+ or the public SDK. Clone complete history and run from the root:

```sh
python3 tools/pin_collection.py --destination work/my-collection-pin
```

The helper pins `c5a5c827ed1918572a7cc01fc5a1740341b7a92d`, already published in this collection. It does not use a moving `HEAD`. The result is a 15-component composition with commits, hashes and receipt. Original provenance remains V5 `657fb65a23b5c3b20040e76307b27e6470b1d17c`.

## 2. Understand the two trees

`source/` follows the V5 `framework_pin.py` contract exactly. nxloader compilation uses this tree. `tool-source/` contains the complete selected export, verified file by file against the pinned manifest. Generation uses this second tree because it also needs `framework/contracts/apkcompat`, which the V5 pin's closed registry cannot admit.

Do not add directories inside `source/`: doing so invalidates verification. `TOOL-SOURCE.json` records the second tree's files; no canonical component was modified. Omitted private tests remain omitted. This composition does not promise the complete historical suite or change existing ports' baselines.

## 3. Recover an omitted public source

```sh
python3 tools/recover_reference.py fp2-nextos --path build_universal.sh   --destination work/recovered-fp2
```

The file is retrieved at the catalog commit with its hash recorded, only under `work/`. If already included in the selection, the download must match the manifest SHA-256. Special path characters are preserved in the URL; both the file and its provenance record must be new and use mode `0600`. The command does not execute the script or modify `ports/*/upstream/`. Examine paths, dependencies and license before use. A public copy may contain historical paths unsuitable for new documentation.

FP2 has this script in its public repository even though the selection omitted it. Chrono's build requires a local image and an excluded TTF; the new SDK does not automatically make that historical script portable. Consult the [profile catalog](../../catalog/profiles.json) and prepare your own recipe with explicit dependencies.

## 4. Check the scope

Do not remove hashes to accept a different library. Distinguish collection pins, component pins, the port commit and owner-input identity. A new build has its own provenance and requires its own testing. The [shader laboratory](../../examples/shader-lab/README.en.md) demonstrates that distinction for FP2 dependencies.
