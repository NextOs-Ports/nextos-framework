# Android inventory and reference selection

[Português](../pt-BR/INVENTARIO-ANDROID.md)

Start with compatible owner-provided input. The tool produces technical information without executing guest code and excludes external APK filenames and computer paths from its JSON.

## 1. Run the inventory

Use Python 3.11+, `readelf` and `aapt` to obtain versions from binary manifests too. Replace the paths; provide only actual APKs in the set, omitting the second for a single-APK game:

```sh
python3 tools/inventory_apk.py /private/owner-input/base.apk   /private/owner-input/config.arm64_v8a.apk   --output work/inventory.json
python3 tools/find_reference.py --engine Unity --abi arm64-v8a --renderer GLES2
```

The public SDK includes the tools. To use them in the container, explicitly mount input read-only at `/private/owner-input`, alongside the collection and `work/`. Do not send input to CI or external services. Output must be a new file.

## 2. Interpret the fields

The report includes package/split, version when available, container size/hash, ABI libraries, hashes, imports with type/binding/version, dependencies and engine hints. Missing versions remain `null`; engine identification remains a hypothesis. The catalog contains 41 source profiles, explicit unknown fields and evidence links. ABI means a historical recipe declaration, not execution validated in this edition.

The `arm64-v8a` preference is local to the APK containing that ABI. Examine the whole split set; an asset split does not establish architecture. ARMv7 builds require review of the softfp boundary.

## 3. Respect the limits

Inventory rejects unsafe paths, collisions, symlinks, encrypted members, members over 512 MiB and sets over 8 GiB. Nested APKM/APKS/XAPK are not automatically expanded: prepare the set locally in a private area. The tool does not guarantee split completeness or convert files.

TLS, TLSDESC, IFUNC, IRELATIVE, RELR and packed Android relocations require attention before selecting V5 nxloader. Markers provide static triage, not comprehensive relocation certification. Imports through dlsym/JNI and buffer/thread contracts still need investigation.

## 4. Give AI a concrete next step

Select a reference by profile, open `SOURCE-MAP.json` and read the implementation matching the identified boundary. Record each import's signature, type, ownership, error and test in your contract table. The [integrated example](../../examples/first-port/README.en.md) provides an original input for practice without commercial data.
