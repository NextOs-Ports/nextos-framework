#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Focused NXExtract private-library renderer/public-final closure gate."""

import hashlib
import importlib.util
import inspect
import json
from pathlib import Path
import tempfile


TESTS = Path(__file__).resolve().parent
NXRELEASE = TESTS.parent
RENDER_FIXTURE = TESTS / "test_generator_root.py"
TOOL = NXRELEASE / "nxrelease.py"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    require(spec is not None and spec.loader is not None,
            "cannot load " + str(path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    fixture = load(RENDER_FIXTURE, "nxrelease_private_library_fixture")
    release = load(TOOL, "nxrelease_private_library_public_final")
    relative = "nxextract/lib/aarch64/libfoo.so.1"
    pid = "generator-root-fixture"
    require("_public_final_validate_live_nxextract" in inspect.getsource(
                release._public_final_generation_v2),
            "public-final generation-v2 is not wired to NXExtract closure")

    # This is the exact role selection used by public-final's comparison of
    # the live NXExtract tree against generation_runtime.
    selected = release._public_final_nxextract_runtime_paths([
        {"role": "private-library", "path": "lib/libgame.so"},
        {"role": "private-library", "path": relative},
        {"role": "nxextract-helper", "path": "nxextract/helpers/tool"},
        {"role": "runtime-data", "path": "nxextract/not-authorized"},
    ])
    require(selected == {relative, "nxextract/helpers/tool"},
            "public-final NXExtract role closure is not narrow and complete")

    runtime = [
        {"role": "nxextract-recipe", "path": "extractor.json"},
        {"role": "nxextract-engine", "path": "nxextract/nxextract.py"},
        {"role": "nxextract-runner",
         "path": "nxextract/run-extractor.sh"},
        {"role": "nxextract-runtime-env",
         "path": "nxextract/nxextract-runtime-env.sh"},
        {"role": "nxextract-ui", "path": "nxextract/nxextract-ui"},
        {"role": "private-library", "path": relative},
    ]
    inventory = {
        pid + "/" + member["path"]: {}
        for member in runtime
    }
    config = {"port_dir": pid, "inventory": inventory}
    release._public_final_validate_live_nxextract(
        {"nxextract": {"mode": "yes"}}, runtime, config
    )

    for label, nxport, candidate_config in (
        (
            "undeclared live member",
            {"nxextract": {"mode": "yes"}},
            {"port_dir": pid,
             "inventory": {**inventory,
                           pid + "/nxextract/extra.so": {}}},
        ),
        (
            "disabled NXExtract private library",
            {"nxextract": {"mode": "no"}}, config,
        ),
    ):
        try:
            release._public_final_validate_live_nxextract(
                nxport, runtime, candidate_config
            )
        except release.ReleaseError:
            pass
        else:
            raise AssertionError(label + " passed public-final closure")

    # Build one real generator root, render only nxrelease.json, and inspect
    # the live/store records. No validate, stage, release or ZIP runs here.
    with tempfile.TemporaryDirectory(
            prefix="nxrelease-nxextract-private-library-") as raw:
        candidate = fixture.build_candidate(Path(raw))
        fixture.render(candidate)
        document = json.loads(
            (candidate / "nxrelease.json").read_text(encoding="utf-8")
        )
        records = {record["source"]: record for record in document["files"]}
        live_path = pid + "/" + relative
        live = records[live_path]
        expected_hash = hashlib.sha256(
            (candidate / live_path).read_bytes()).hexdigest()
        require(live["target"] == live_path and
                live["kind"] == "third-party-linux" and
                live["mode"] == "0644" and
                live["sha256"] == expected_hash and
                live["architecture"] == "aarch64" and
                live["build_profile"] == "universal-low-glibc" and
                live["provenance"] ==
                "declared private generation library",
                "renderer changed NXExtract private-library identity")

        stores = [
            record for source, record in records.items()
            if source.endswith("/files/runtime/" + relative)
        ]
        require(len(stores) == 1 and
                stores[0]["kind"] == "nxruntime-generation-linux" and
                stores[0]["mode"] == live["mode"] and
                stores[0]["sha256"] == live["sha256"],
                "immutable store changed NXExtract private-library mode/hash")

    print(
        "nxrelease 0.4.3 NXExtract private-library gate: "
        "third_party_linux=1 mode_hash=1 store=1 "
        "public_final_closure=1 public_final_negatives=2"
    )


if __name__ == "__main__":
    main()
