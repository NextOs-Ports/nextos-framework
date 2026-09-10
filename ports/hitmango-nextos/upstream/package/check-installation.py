#!/usr/bin/env python3
"""Prove that public owner-data instructions match the Hitman GO recipe."""

import json
import pathlib
import sys


def fail(message: str) -> None:
    raise SystemExit("hitmango installation gate: " + message)


if len(sys.argv) != 5:
    fail("expected recipe, nxport, INSTALLATION.md and nxrelease manifest")

recipe_path, nxport_path, installation_path, release_path = map(
    pathlib.Path, sys.argv[1:]
)
for path in (recipe_path, nxport_path, installation_path, release_path):
    if path.is_symlink() or not path.is_file():
        fail("missing or unsafe input: " + str(path))

recipe = json.loads(recipe_path.read_text(encoding="utf-8"))
nxport = json.loads(nxport_path.read_text(encoding="utf-8"))
release = json.loads(release_path.read_text(encoding="utf-8"))
installation = installation_path.read_text(encoding="utf-8")

expected_package = "com.squareenixmontreal.hitmango"
expected_version = "1.18.1"
expected_abi = "arm64-v8a"
expected_recipe_version = "1.18.1-universal-2"
reference_size = 512169180
reference_sha256 = (
    "d88c418ddb72f23842138cf4ac2476a30687b9d16138be3ea7a4bfec66ef096a"
)
engine_sha256 = "b1b46ecdf1336b1412d7d3a3d291220aca4834a47730a5545afb382dae6036b5"
runner_sha256 = "c931427c7226d22d7e30eee8549b50f0621dca1c9d0336634aca08631f454d7a"
runtime_env_sha256 = "332919a9960d4317563b647f9932d1a4367da147a425fe2f78eafd706f01563f"
ui_sha256 = "7ca901d8515ab9a084be81e05888e1fd03cec80fb03896df6331c1c95698ef56"
critical_payloads = {
    "lib/libmain.so": (6728, "d9c2bb9a4b819270989f2ed8ff93dd47b7e38538c98ad784eb7b29c0a61a8917", "aarch64"),
    "lib/libunity.so": (17144392, "105c61a07c2cc1317c786e825d7e3e0311aa51c7488450cb23cd7f81d33c5fe5", "aarch64"),
    "lib/libil2cpp.so": (32927536, "bf0b5a8f3b033f2c3929244866a49d3ecf166d84cf5a085de3d763f143a63d2f", "aarch64"),
    "lib/libFirebaseCppApp-12_10_1.so": (6040520, "177ed0095d067af7123bbf6e704f819422a3ed923966479e6bd966459aa3d148", "aarch64"),
    "assets/bin/Data/boot.config": (212, "e3979e348d51f2f5296167cb7aa5cd3f6704855458655f8464bf04702ac03d04", None),
    "assets/bin/Data/globalgamemanagers": (540892, "b74032bb67f53e6497300c7641debc0d9a146fc7e7c902f62de7108c5bc3cbb6", None),
    "assets/bin/Data/Managed/Metadata/global-metadata.dat": (7065868, "846ec2aaa2c050ac9193909692df1570cd107f2b49aa11c04e442f1d907a646c", None),
}

if recipe.get("input", {}).get("packages") != [expected_package]:
    fail("package ID differs from the accepted owner data")
if recipe.get("abi_order") != [expected_abi]:
    fail("ABI differs from the accepted owner data")
if recipe.get("version") != expected_recipe_version:
    fail("recipe version does not identify the flexible 1.18.1 contract")
if reference_sha256 in json.dumps(recipe, sort_keys=True):
    fail("recipe must not bind compatibility to one whole-file APK hash")

extract_rules = recipe.get("extract", [])
if len(extract_rules) != 2:
    fail("recipe must contain exactly the owner assets and ARM64 library trees")
if {rule.get("destination") for rule in extract_rules} != {"assets", "lib"}:
    fail("owner extraction destinations drifted")
if any(rule.get("source", {}).get("kind") != "entries" for rule in extract_rules):
    fail("owner extraction must select package entries, not pin one container")

final_by_path = {item.get("path"): item for item in recipe.get("validate", [])}
if set(final_by_path) != set(critical_payloads):
    fail("critical final validation set drifted")
for path, (size, sha256, machine) in critical_payloads.items():
    expected = {"path": path, "type": "file", "size": size, "sha256": sha256}
    if machine is not None:
        expected["elf_machine"] = machine
    if final_by_path.get(path) != expected:
        fail("critical payload identity drifted: " + path)

for token, label in (
    (expected_version, "game version"),
    (expected_package, "package ID"),
    (expected_abi, "ABI"),
    (str(reference_size), "reference APK size"),
    (reference_sha256, "reference APK SHA-256"),
    *[(value[1], path + " SHA-256") for path, value in critical_payloads.items()],
):
    if installation.count(token) < 2:
        fail(label + " is not present in both languages")

for forbidden_reference in ("apkvision", "5play", "hitman-go-mod_"):
    if forbidden_reference in installation.lower():
        fail("public installation instructions expose a modified-APK source")

if nxport.get("nxextract") != {"mode": "yes", "version": "1.2.10"}:
    fail("nxport does not opt into the exact NXExtract 1.2.10 set")
required = nxport.get("required_files", [])
if required[:2] != ["bin/aarch64/hitmango-nextos", "nxsplash-nextos"]:
    fail("nxport must pin the canonical executable and mandatory splash first")
for path in critical_payloads:
    if path not in required:
        fail("required owner payload is missing: " + path)

if release.get("package", {}).get("version") != "1.2.2":
    fail("release version differs from the port version")
release_files = release.get("files", [])
release_targets = [item.get("target") for item in release_files]
for target in (
    "hitmango/INSTALLATION.md",
    "hitmango/bin/aarch64/hitmango-nextos",
    "hitmango/nxsplash-nextos",
):
    if release_targets.count(target) != 1:
        fail("release must contain exactly one " + target)
if any(target == "hitmango/run.sh" for target in release_targets):
    fail("retired intermediate run.sh entered the public package")

canonical_nxextract_files = {
    "hitmango/nxextract/nxextract.py": ("nxextract", "0644", engine_sha256),
    "hitmango/nxextract/run-extractor.sh": ("nxextract-runner", "0644", runner_sha256),
    "hitmango/nxextract/nxextract-runtime-env.sh": ("nxextract-runtime-env", "0644", runtime_env_sha256),
    "hitmango/nxextract/nxextract-ui": ("nxextract-ui-linux", "0755", ui_sha256),
}
for target, (kind, mode, sha256) in canonical_nxextract_files.items():
    entries = [item for item in release_files if item.get("target") == target]
    if len(entries) != 1:
        fail("release must contain one canonical NXExtract file: " + target)
    if (entries[0].get("kind"), entries[0].get("mode"), entries[0].get("sha256")) != (kind, mode, sha256):
        fail("NXExtract release identity drifted: " + target)

release_nxextract = release.get("nxextract", {})
if (
    release_nxextract.get("version") != "1.2.10"
    or release_nxextract.get("minimum_version") != "1.2.10"
    or release_nxextract.get("sha256") != engine_sha256
    or release_nxextract.get("runner_sha256") != runner_sha256
    or release_nxextract.get("runtime_env_sha256") != runtime_env_sha256
    or release_nxextract.get("ui_sha256") != ui_sha256
):
    fail("NXExtract manifest contract drifted")

if any(str(target).lower().endswith((".apk", ".apkm", ".apks", ".xapk", ".obb", ".dex")) for target in release_targets):
    fail("release allowlist contains proprietary Android data")

print("hitmango installation gate: PASS")
