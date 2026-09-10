#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Focused current nxgenerator provider/video contract gate.

This is a host-only composition test.  Pixel sampling and fatal BLACK handling
belong to nxgl/nxbootstrap; this gate proves that a project's closed opt-ins
reach the canonical nxport and launcher without changing unrelated port bytes.
"""

import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parents[1]
TOOL = ROOT / "nxgenerator.py"
BOOTSTRAP_ROOT = REPOSITORY / "framework" / "nxbootstrap"
BOOTSTRAP_GENERATOR = BOOTSTRAP_ROOT / "tools" / "generate-port.py"
BOOTSTRAP_TEMPLATE = BOOTSTRAP_ROOT / "templates" / "launcher.sh.in"
FIXTURE = ROOT / "tests" / "fixtures" / "c3-baseline"
MANIFEST = FIXTURE / "nxproject.json"
ADAPTER_GOLDEN = FIXTURE / "adapter-contract.golden.json"
PORT_ID = "nxexample-aarch64"
EXPECTED_GENERATOR_VERSION = "0.4.5"
EXPECTED_BOOTSTRAP_VERSION = "0.8.4"
EXPECTED_BOOTSTRAP_GENERATOR_SHA256 = "0bffca8e6d83fa72aab2e00db395f7f55df5cdb7e268119201c35b81c146b758"
EXPECTED_BOOTSTRAP_TEMPLATE_SHA256 = "c276bfac69667fc9fa649d0ad24f65c0b5e4a268bcaa9fe891582c3bb65aed0b"


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_tool():
    specification = importlib.util.spec_from_file_location(
        "nxgenerator_video_provider_under_test", TOOL
    )
    require(specification is not None and specification.loader is not None,
            "cannot load nxgenerator")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def write_manifest(path, document):
    path.write_text(
        json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True)
        + "\n",
        encoding="utf-8",
    )


def generate(manifest, output):
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    result = subprocess.run(
        [sys.executable, "-B", str(TOOL), str(manifest),
         "--output", str(output)],
        cwd=str(REPOSITORY), env=environment, stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        check=False,
    )
    require(result.returncode == 0,
            "generation failed: %s" % result.stderr.strip())
    return output / PORT_ID


def expect_invalid(tool, baseline, field, value, work, label):
    candidate = copy.deepcopy(baseline)
    candidate["nxport"][field] = value
    output = work / ("invalid-" + label)
    try:
        tool.generate_project(candidate, output, REPOSITORY)
    except tool.ProjectError:
        require(not output.exists(),
                "invalid %s published a partial output" % label)
        return
    raise GateError("invalid %s passed" % label)


def main():
    tool = load_tool()
    require((ROOT / "VERSION").read_text(encoding="ascii").strip() ==
            EXPECTED_GENERATOR_VERSION,
            "nxgenerator version is not %s" % EXPECTED_GENERATOR_VERSION)
    require((BOOTSTRAP_ROOT / "VERSION").read_text(
                encoding="ascii").strip() == EXPECTED_BOOTSTRAP_VERSION,
            "nxgenerator is not composed with nxbootstrap 0.8.4")
    require(sha256(BOOTSTRAP_GENERATOR) ==
            EXPECTED_BOOTSTRAP_GENERATOR_SHA256 and
            sha256(BOOTSTRAP_TEMPLATE) ==
            EXPECTED_BOOTSTRAP_TEMPLATE_SHA256,
            "nxbootstrap 0.8.4 source bytes differ from the frozen pin")

    baseline = json.loads(MANIFEST.read_text(encoding="utf-8"))
    original = copy.deepcopy(baseline)
    baseline_config = tool.validate_project(baseline, REPOSITORY)
    require(baseline == original, "validation mutated the undeclared project")
    canonical_undeclared = json.loads(
        tool.BOOTSTRAP_GENERATOR.canonical_manifest(
            baseline_config["nxport"])
    )
    require("sdl_provider" not in canonical_undeclared and
            "video_proof" not in canonical_undeclared,
            "undeclared provider/video fields were serialized")

    work = Path(tempfile.mkdtemp(prefix="nxgen-video-provider."))
    try:
        baseline_port = generate(MANIFEST, work / "baseline")
        require((baseline_port / "adapter" / "adapter-contract.json")
                .read_bytes() == ADAPTER_GOLDEN.read_bytes(),
                "undeclared fields changed the literal adapter golden")
        emitted_baseline = json.loads(
            (baseline_port / "nxport.json").read_text(encoding="utf-8")
        )
        require("sdl_provider" not in emitted_baseline and
                "video_proof" not in emitted_baseline,
                "undeclared fields appeared in generated nxport.json")

        declared = copy.deepcopy(baseline)
        declared["nxport"]["sdl_provider"] = "system"
        declared["nxport"]["video_proof"] = "required"
        declared_path = work / "declared.json"
        write_manifest(declared_path, declared)
        declared_port = generate(declared_path, work / "declared")

        emitted_project = json.loads(
            (declared_port / "nxproject.json").read_text(encoding="utf-8")
        )
        emitted_nxport = json.loads(
            (declared_port / "nxport.json").read_text(encoding="utf-8")
        )
        for document, context in (
                (emitted_project["nxport"], "nxproject"),
                (emitted_nxport, "nxport")):
            require(document.get("sdl_provider") == "system" and
                    document.get("video_proof") == "required",
                    "%s lost the closed provider/video declarations" %
                    context)

        launcher = (work / "declared" /
                    declared["nxport"]["launcher_name"]).read_text(
                        encoding="utf-8")
        require("NXBOOTSTRAP_SDL_PROVIDER=system" in launcher,
                "launcher did not activate the system SDL provider")
        require("NXBOOTSTRAP_VIDEO_REQUIRED=1" in launcher,
                "launcher did not require the video receipt")
        require(re.search(
            r"(?m)^[ \t]*(?:export[ \t]+)?SDL_VIDEODRIVER=", launcher
        ) is None, "system provider launcher forced SDL_VIDEODRIVER")

        baseline_receipt = json.loads(
            (baseline_port / "GENERATION.json").read_text(encoding="utf-8")
        )
        declared_receipt = json.loads(
            (declared_port / "GENERATION.json").read_text(encoding="utf-8")
        )
        expected_pin = {
            "version": EXPECTED_BOOTSTRAP_VERSION,
            "source_files": {
                "templates/launcher.sh.in":
                    EXPECTED_BOOTSTRAP_TEMPLATE_SHA256,
                "tools/generate-port.py":
                    EXPECTED_BOOTSTRAP_GENERATOR_SHA256,
            },
        }
        require(declared_receipt["source_pins"]["nxbootstrap"] ==
                expected_pin, "receipt does not pin exact nxbootstrap 0.8.4")
        require(declared_receipt["generator"] == {
            "name": "nxgenerator", "version": EXPECTED_GENERATOR_VERSION,
        }, "receipt does not identify nxgenerator 0.4.5")
        require(baseline_receipt["generation_id"] !=
                declared_receipt["generation_id"],
                "provider/video opt-ins reused the undeclared identity")

        unchanged = (
            "adapter/adapter-contract.json",
            "defaults/NEXTOSCONTROLLERS.gptk",
            "defaults/NEXTOSSETTINGS.txt",
            "port.json",
            "gameinfo.xml",
            "INSTALLATION.md",
            "README.md",
            "LICENSE",
            "extractor.json",
            "nxextract/nxextract.py",
            "nxextract/run-extractor.sh",
            "nxextract/nxextract-runtime-env.sh",
            "nxextract/nxextract-ui",
            "gamedata/README.txt",
            "nxsplash-nextos",
        )
        for relative in unchanged:
            left = baseline_port / relative
            right = declared_port / relative
            require(left.read_bytes() == right.read_bytes(),
                    "provider/video opt-ins changed unrelated bytes: %s" %
                    relative)

        for field, value, label in (
            ("sdl_provider", "private", "private-provider"),
            ("sdl_provider", "", "empty-provider"),
            ("sdl_provider", None, "null-provider"),
            ("sdl_provider", True, "boolean-provider"),
            ("video_proof", "optional", "optional-video"),
            ("video_proof", "", "empty-video"),
            ("video_proof", None, "null-video"),
            ("video_proof", True, "boolean-video"),
        ):
            expect_invalid(tool, baseline, field, value, work, label)

        print(
            "nxgenerator 0.4.5 video/provider gate passed: "
            "nxbootstrap=0.8.4 omitted=2 roundtrip=2 launcher=2 "
            "unrelated_bytes=%d negatives=8 no_device=1" % len(unchanged)
        )
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
