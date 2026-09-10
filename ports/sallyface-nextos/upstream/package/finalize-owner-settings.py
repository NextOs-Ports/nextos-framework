#!/usr/bin/env python3
"""Preserve Sally's reviewed owner settings after canonical generation."""

import argparse
import importlib.util
import json
from pathlib import Path
import sys


EXPECTED_GENERATOR_VERSION = "0.2.18"
SETTINGS_RELATIVE = Path("defaults/NEXTOSSETTINGS.txt")


def fail(message):
    raise SystemExit("sallyface settings finalizer: " + message)


def load_generator(framework_root):
    version_path = framework_root / "nxgenerator/VERSION"
    if version_path.read_text(encoding="ascii") != EXPECTED_GENERATOR_VERSION + "\n":
        fail("nxgenerator version is not " + EXPECTED_GENERATOR_VERSION)
    module_path = framework_root / "nxgenerator/nxgenerator.py"
    specification = importlib.util.spec_from_file_location(
        "sallyface_pinned_nxgenerator", module_path
    )
    if specification is None or specification.loader is None:
        fail("cannot load pinned nxgenerator")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def validate_settings(payload):
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError:
        fail("NEXTOSSETTINGS.txt is not UTF-8")
    lines = text.splitlines()
    if not lines or lines[0] != "# NEXTOS_SETTINGS/1":
        fail("NEXTOSSETTINGS.txt has the wrong magic")
    if "# quality: auto|low|medium|high" not in text:
        fail("NEXTOSSETTINGS.txt does not document all quality profiles")
    values = {}
    for line in lines[1:]:
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            fail("NEXTOSSETTINGS.txt contains a malformed setting")
        key, value = line.split("=", 1)
        if key in values:
            fail("NEXTOSSETTINGS.txt contains a duplicate setting: " + key)
        if key not in ("language", "quality"):
            fail("NEXTOSSETTINGS.txt contains an unknown setting: " + key)
        values[key] = value
    if values != {"language": "auto", "quality": "auto"}:
        fail("NEXTOSSETTINGS.txt defaults must be language=auto, quality=auto")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port-dir", type=Path, required=True)
    parser.add_argument("--generator-root", type=Path, required=True)
    parser.add_argument("--framework-root", type=Path, required=True)
    arguments = parser.parse_args()

    port_dir = arguments.port_dir.resolve()
    generator_root = arguments.generator_root.resolve()
    framework_root = arguments.framework_root.resolve()
    if generator_root.is_symlink() or not generator_root.is_dir():
        fail("generator root is unsafe")
    project = json.loads((port_dir / "nxproject.json").read_text(encoding="utf-8"))
    port_id = project["nxport"]["id"]
    if port_id != "sallyface":
        fail("unexpected port id")

    source = port_dir / SETTINGS_RELATIVE
    target = generator_root / port_id / SETTINGS_RELATIVE
    receipt_path = generator_root / port_id / "GENERATION.json"
    for path, label in ((source, "source"), (target, "generated target"),
                        (receipt_path, "generation receipt")):
        if path.is_symlink() or not path.is_file():
            fail(label + " is not a safe regular file")

    generator = load_generator(framework_root)
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    before = generator.artifact_inventory(generator_root)
    if receipt.get("artifacts") != before:
        fail("generation receipt was stale before settings finalization")

    payload = source.read_bytes()
    validate_settings(payload)
    generator.atomic_write_bytes(target, payload, 0o644)
    after = generator.artifact_inventory(generator_root)
    before_by_path = {item["path"]: item for item in before}
    after_by_path = {item["path"]: item for item in after}
    if set(before_by_path) != set(after_by_path):
        fail("settings finalization changed the generated file set")
    changed = {
        path for path in before_by_path
        if before_by_path[path] != after_by_path[path]
    }
    expected = (Path(port_id) / SETTINGS_RELATIVE).as_posix()
    if changed not in (set(), {expected}):
        fail("settings finalization changed an unrelated artifact")

    receipt["artifacts"] = after
    generator.atomic_write_bytes(
        receipt_path, generator.canonical_json(receipt), 0o644
    )
    if target.read_bytes() != payload:
        fail("generated settings differ from the reviewed source")
    if json.loads(receipt_path.read_text(encoding="utf-8")).get("artifacts") != \
            generator.artifact_inventory(generator_root):
        fail("generation receipt is stale after settings finalization")
    print("sallyface settings finalizer: PASS quality=auto receipt=refreshed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
