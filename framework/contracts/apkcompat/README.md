# NXCOMPAT-APK — canonical APK compatibility contract (V3)

Single source of truth for the APK-COMPAT-01 rule: **identity of the
owner-provided container never decides compatibility**. Not the SHA-256, not
the CRC32, not the exact size, not the file name, signature, packaging tool,
member order/timestamps, exact `versionCode` or literal version text — alone
or in a list of any length. Those values may only appear in the
documentation-only `reference_build` block, `INSTALLATION.md` and receipts.

Compatibility is decided by runtime contracts (`compatibility` block +
extract-rule validators): package family, ABI, required members/structure,
engine/metadata format, ELF architecture and consumed symbols/interfaces.
Internal payload hashes may only select a `patch_profiles` entry that really
depends on those bytes, and every profile declares a generic/symbolic
`fallback`; an unknown compatible build follows the fallback and is never
rejected for missing a whitelist. When a real contract fails, the error names
the incompatible technical property — never "wrong SHA/version".

`required_members` is enforced by NXExtract over the logical APK set (base +
splits, never the APKM/APKS/XAPK wrapper):

- a plain path is `core_required` and must exist for every selected ABI;
- `optional` records presence without gating;
- `variant_required` gates only when `variant` equals the resolved `abi_order`
  token;
- `patch_selector` never gates. Its internal payload SHA-256 selects exactly
  one linked profile, while absent or unknown bytes select the common declared
  fallback. The authenticated decision enters the plan fingerprint, hook
  environment and installation marker.

Container rules reject identity in rule, final-output and hook-checkpoint
validation, and must keep `source.patterns` at the name-agnostic default
`["*"]` plus a fixed destination (never `{basename}`). Android signing members
(`META-INF/*.RSA`, `.SF`, `.DSA`, `.EC` and
`MANIFEST.MF`) cannot be required members, extract anchors, patch selectors or
release-hook predicates.

## Consumers

| Layer | How |
|---|---|
| NXExtract (`suportando_outros_devices/extrator-universal/nxextract.py`) | embedded verbatim copy between `BEGIN/END APKCOMPAT CANONICAL` markers; byte identity enforced by `test_apkcompat_embedded_module_is_byte_identical_to_canonical` |
| NXGenerator (`framework/nxgenerator/nxgenerator.py`) | imported by file path |
| NXRelease (`framework/nxrelease/nxrelease.py`) | imported by file path |

All three layers run the shared fixtures
`fixtures/apk-compat-cases-v3.json`, including the Terraria regression
(documented `1.4.5.6.4`, internal `1.4.5.6.49`).

## Surface

- `validate_recipe_apk_compat(recipe, fail)` — entry point over a recipe dict.
- `validate_container_rule_identity` / `validate_container_source_identity` —
  NXA0001..NXA0006, including both phase validators and filename patterns.
- `validate_reference_build` / `validate_compatibility` /
  `normalize_required_members` / `validate_patch_profiles` — NXA0010..NXA0037.
- `validate_hook_contract` — org.nextos.apk-compat.hook-contract/1,
  NXA0040..NXA0047 (predicates classified
  `reference_identity`/`compatibility`/`patch_selection`; identity-only
  predicates refused).
- `scan_static_suspects` — NXA0050..NXA0055 static defence (64/40-hex
  equality, signing-member references, literal dotted version tokens, size
  tables, absolute offsets). Findings are
  advisory to the caller; exceptions must be explicit and narrow and never
  replace the dynamic metamorphic tests.

## Editing

Edit `apkcompat.py` here, then re-embed the byte-identical copy in
`nxextract.py` (the sync gate fails otherwise), update the fixtures and run
the three consumer suites. The module is dependency-free on purpose: `re`
only, `fail`-callback error reporting, no filesystem access.
