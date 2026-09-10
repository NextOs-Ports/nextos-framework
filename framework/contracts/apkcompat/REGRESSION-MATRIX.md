# apkcompat 1.1.0 / NXExtract 1.2.20 — regression matrix

| Guarantee | Test |
|---|---|
| Container sha256 whitelist of ANY length rejected (NXA0001) | shared fixtures `container-single-sha-reject`, `container-sha-whitelist-of-two-reject`, `container-sha-whitelist-of-three-reject`; nxextract `test_container_recipe_rejects_whole_apk_identity_lock` |
| Container crc32/exact size rejected (NXA0002/NXA0003) | fixtures `container-crc-list-reject`, `container-exact-size-reject` |
| No rule/final/checkpoint validator can hide a whole-container SHA (NXA0001) | fixtures `container-source-validate-sha-reject`, `container-output-validate-sha-reject`, `container-global-output-sha-reject`; nxextract `test_container_recipe_rejects_whole_apk_identity_lock`; generator/release bypass negatives |
| External filename cannot gate through validators, `source.patterns` or destination `{basename}` (NXA0005) | fixtures `container-filename-reject`, `container-source-pattern-filename-reject`, `container-basename-destination-reject`; renamed runtime APK/APKM/APKS/XAPK installs |
| Signing/certificate identity cannot anchor members, rules or packaged hook source (NXA0004/NXA0055) | fixtures `required-signing-member-reject`, `required-signing-extract-reject`; nxrelease packaged-hook-source negative |
| Structural container accepted (bounded size + magic + package) | fixture `container-structural-accept`; nxextract repackaging test |
| reference_build is documentation only | fixture `reference-build-documentation-accept` |
| compatibility block coherent with input.packages/abi_order (NXA0023/NXA0025) | fixture `compatibility-package-divergence-reject` |
| compatibility carries no hashes (NXA0027) | fixture `compatibility-carries-hash-reject` |
| Member roles have runtime semantics over logical base+splits | nxextract `test_compatibility_roles_runtime_apk/apkm/apks/xapk`, core/optional/variant focused tests; named synthetic fixture `offtheroad-arm64-2-roles.json` installs without `AVConfig.json`, requires both AArch64 libraries plus `data_001.xpk`, and rejects package/ABI/ELF/data/library mismatches |
| Patch profile requires authenticated internal SHA, linked selector and common fallback (NXA0034..37) | fixtures `patch-profile-with-fallback-accept`, `patch-profile-without-fallback-reject`; runtime known/unknown/absent + ambiguous-profile test |
| Patch choice/fallback enters plan fingerprint, hook checkpoint/env and install marker | nxextract `test_patch_selection_fallback_fingerprint_env_and_checkpoint`; four-format marker assertions |
| hook contract mandatory fields, identity-only predicates refused (NXA0040..47) | nxextract `test_hook_inline_contract_shapes` |
| runtime identity-only rejection refused (NXA0046) | nxextract `test_hook_identity_predicate_cannot_reject_at_runtime` |
| Terraria .4/.49 metamorphic regression + authenticated runtime profile + static defence (NXA0051) | nxextract `test_terraria_metamorphic_regression` and four-format role tests; fixture `terraria_regression` |
| Embedded copy byte-identical to canonical | nxextract `test_apkcompat_embedded_module_is_byte_identical_to_canonical` |
