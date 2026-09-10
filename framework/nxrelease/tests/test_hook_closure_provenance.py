#!/usr/bin/env python3
"""Focused gate for digest-free packaged hook closures."""

import importlib.util
import json
from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parents[3]
TOOL = ROOT / "framework" / "nxrelease" / "nxrelease.py"
DIGEST = "a" * 64


def load_tool():
    spec = importlib.util.spec_from_file_location(
        "nxrelease_hook_closure_provenance", TOOL
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load " + str(TOOL))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def contract(fallback="generic-symbolic-path"):
    return {
        "schema": "org.nextos.apk-compat.hook-contract",
        "schema_version": 1,
        "hook_id": "prepare",
        "inputs": ["assets/payload.bin"],
        "predicates": [{
            "class": "compatibility",
            "checks": "payload exposes the semantic interface",
        }],
        "fallback": fallback,
        "error_codes": ["NXH0001"],
    }


def recipe(hook_fallback="generic-symbolic-path",
           profile_fallback="generic-symbolic-path"):
    return {
        "abi_order": ["arm64-v8a"],
        "input": {"packages": ["org.example.fixture"]},
        "compatibility": {
            "package_families": ["org.example.fixture"],
            "abis": ["arm64-v8a"],
            "required_members": [{
                "member": "assets/payload.bin",
                "role": "patch_selector",
            }],
        },
        "patch_profiles": [{
            "id": "known-payload",
            "match_internal_payload": {
                "path": "assets/payload.bin",
                "sha256": DIGEST,
            },
            "fallback": profile_fallback,
        }],
        "hooks": [{
            "id": "prepare",
            "argv": ["python3", "{game_dir}/hooks/prepare.py"],
            "cwd": "{game_dir}/hooks",
            "contract": contract(hook_fallback),
        }],
    }


def write_closure(root, suffix, leaf_document, include_binary=False):
    prepare = root / "prepare.py"
    nested = root / "nested.py"
    index = root / ("index" + suffix)
    leaf = root / ("leaf" + suffix)
    unrelated = root / ("unrelated" + suffix)
    prepare.write_text(
        "import subprocess\n"
        "subprocess.run(['python3', 'nested.py'], check=True)\n",
        encoding="utf-8",
    )
    nested.write_text(
        "import json, os\n"
        "index = json.load(open('index%s', encoding='utf-8'))\n" % suffix +
        "spec = json.load(open(index['spec'], encoding='utf-8'))\n"
        "selected_profile = os.environ.get(\n"
        "    spec.get('profile_environment', 'NXEXTRACT_PATCH_PROFILE_ID'),\n"
        "    spec.get('fallback', 'generic-symbolic-path'))\n",
        encoding="utf-8",
    )
    links = {"spec": "leaf" + suffix}
    if include_binary:
        links.update({
            "runtime_artifact": "binary.blob",
            "nul_artifact": "nul.blob",
            "non_utf8_artifact": "non-utf8.blob",
            "non_json_artifact": "plain.blob",
            "json_scalar_artifact": "scalar.blob",
        })
    index.write_text(json.dumps(links) + "\n", encoding="utf-8")
    leaf.write_text(json.dumps(leaf_document) + "\n", encoding="utf-8")
    unrelated.write_text(
        json.dumps({"sha256": "f" * 64}) + "\n", encoding="utf-8"
    )
    records = [
        {"target": "fixture/hooks/prepare.py", "actual_path": prepare,
         "kind": "script"},
        {"target": "fixture/hooks/nested.py", "actual_path": nested,
         "kind": "script"},
        {"target": "fixture/hooks/index" + suffix, "actual_path": index,
         "kind": "payload"},
        {"target": "fixture/hooks/leaf" + suffix, "actual_path": leaf,
         "kind": "payload"},
        {"target": "fixture/hooks/unrelated" + suffix,
         "actual_path": unrelated, "kind": "payload"},
    ]
    if include_binary:
        rejected = {
            "binary.blob": b"\x7fELF\x02\x01\x00" + b"a" * 64,
            "nul.blob": b"plain\x00" + b"a" * 64,
            "non-utf8.blob": b"\xff\xfe" + b"a" * 64,
            "plain.blob": b"digest=" + b"a" * 64 + b"\n",
            "scalar.blob": json.dumps("a" * 64).encode("utf-8") + b"\n",
        }
        for name, data in rejected.items():
            artifact = root / name
            artifact.write_bytes(data)
            records.append({
                "target": "fixture/hooks/" + name,
                "actual_path": artifact,
                "kind": "payload",
            })
    return records


def expect_failure(module, candidate, records, needle, label):
    try:
        module.validate_recipe_hooks_static(
            candidate, records, "fixture/extractor.json"
        )
    except module.ReleaseError as error:
        message = str(error)
        if needle not in message:
            raise AssertionError(
                "{} failed for the wrong reason: {}".format(label, message)
            )
        return message
    raise AssertionError(label + " unexpectedly passed")


def main():
    module = load_tool()
    if module.TOOL_VERSION != "0.4.11":
        raise AssertionError("NXRelease version authority is not 0.4.11")

    with tempfile.TemporaryDirectory(prefix="nxrelease-hook-closure-") as raw:
        root = Path(raw)

        # An arbitrary source key cannot turn a digest into an output-integrity
        # checkpoint. The digest also equals the authenticated patch selector,
        # proving that neither former exception survives.
        for suffix in (".json", ".blob"):
            case = root / ("output" + suffix[1:])
            case.mkdir()
            records = write_closure(
                case, suffix, {"output_sha256": DIGEST}
            )
            message = expect_failure(
                module, recipe(), records, "NXA0050",
                "output_sha256 digest in " + suffix,
            )
            if "leaf" + suffix not in message:
                raise AssertionError("failure lost transitive target: " + message)

        source_case = root / "source-profile"
        source_case.mkdir()
        records = write_closure(
            source_case, ".blob", {"profile": "known-payload"}
        )
        nested = source_case / "nested.py"
        nested.write_text(
            nested.read_text(encoding="utf-8") +
            "authenticated_profile_digest = %r\n" % DIGEST,
            encoding="utf-8",
        )
        expect_failure(
            module, recipe(), records, "NXA0050",
            "patch profile digest repeated by hook source",
        )

        uppercase_case = root / "uppercase"
        uppercase_case.mkdir()
        records = write_closure(
            uppercase_case, ".blob", {"output_sha256": DIGEST.upper()}
        )
        expect_failure(
            module, recipe(), records, "NXA0050", "uppercase digest",
        )

        positive_case = root / "positive"
        positive_case.mkdir()
        records = write_closure(
            positive_case,
            ".blob",
            {
                "profile_environment": "NXEXTRACT_PATCH_PROFILE_ID",
                "selected_profile": "known-payload",
                "fallback": "generic-symbolic-path",
            },
            include_binary=True,
        )
        positive = recipe()
        module.validate_apk_variant_policy(positive)
        closure_targets = {
            record["target"] for record, _text in
            module._packaged_hook_closure(
                positive["hooks"][0], records, "fixture/extractor.json"
            )
        }
        expected_targets = {
            "fixture/hooks/prepare.py",
            "fixture/hooks/nested.py",
            "fixture/hooks/index.blob",
            "fixture/hooks/leaf.blob",
        }
        if closure_targets != expected_targets:
            raise AssertionError(
                "extensionless JSON/binary closure differs: {!r}".format(
                    sorted(closure_targets)
                )
            )
        module.validate_recipe_hooks_static(
            positive, records, "fixture/extractor.json"
        )

        expect_failure(
            module, recipe(hook_fallback="none"), records,
            "rejecting fallback", "rejecting hook fallback",
        )
        expect_failure(
            module, recipe(profile_fallback="none"), records,
            "rejecting fallback", "rejecting patch-profile fallback",
        )

    print(
        "nxrelease 0.3.25 hook closure provenance: "
        "digest_free=1 output_key_rejected=1 patch_profile_not_whitelist=1 "
        "json_blob_parity=1 uppercase=1 transitive_depth=4 "
        "profile_id_positive=1 rejected_artifacts_skipped=5 "
        "rejecting_fallbacks=2"
    )


if __name__ == "__main__":
    main()
