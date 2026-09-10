#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Directed author-payload composition and GPTK tuning gate for 0.2.18."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile


REPOSITORY = Path(__file__).resolve().parents[3]
ROOT = REPOSITORY / "framework" / "nxgenerator"
TOOL = ROOT / "nxgenerator.py"
SCHEMA_V3 = ROOT / "schema" / "nxproject-v3.schema.json"
NXINPUT = REPOSITORY / "framework" / "nxinput"


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def load_tool():
    specification = importlib.util.spec_from_file_location(
        "nxgenerator_package_payload_under_test", TOOL
    )
    require(specification is not None and specification.loader is not None,
            "cannot load nxgenerator")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def sha256(payload):
    return hashlib.sha256(payload).hexdigest()


def clone(value):
    return json.loads(json.dumps(value))


def write_file(root, relative, payload, mode=0o644):
    target = root / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(payload)
    os.chmod(target, mode)
    return target


def record(path, payload, mode="0644", kind="payload"):
    return {
        "path": path,
        "mode": mode,
        "sha256": sha256(payload),
        "kind": kind,
    }


def project_document():
    return {
        "schema_version": 3,
        "nxport": {
            "schema_version": 2,
            "id": "author-fixture",
            "title": "Author Payload Fixture",
            "launcher_name": "Author Payload Fixture.sh",
            "architecture": "aarch64",
            "executable": "bin/author-fixture-nextos",
            "argument_mode": "game-dir-and-passthrough",
            "home_mode": "preserve",
            "nxextract": {"mode": "no", "version": "1.3.0"},
            "required_files": ["bin/author-fixture-nextos"],
            "private_library_paths": [],
            "prepare_script": "",
            "required_capabilities": ["host.portmaster"],
            "enabled_quirks": [],
            "runtime_report": "log-and-logo",
        },
        "nxextract_recipe": None,
        "adapter": {"skeleton": "contract-only"},
        "portmaster": {
            "metadata_version": 4,
            "min_glibc": "2.17",
            "runtime": [],
        },
        "license": {"spdx_id": "GPL-3.0-only", "source": "LICENSE"},
        "documentation": {"status": "scaffold", "proven_support": []},
        "language_access": {
            "mode": "none", "supported": [], "fallback": "", "sinks": [],
        },
        "controls": {
            "actions": [
                {
                    "id": "fixture.accept", "kind": "button",
                    "sinks": ["adapter.menu.accept"],
                },
                {
                    "id": "fixture.pause", "kind": "button",
                    "sinks": ["adapter.game.pause"],
                },
            ],
            "contexts": {
                "menu": {"A": "fixture.accept"},
                "gameplay": {"START": "fixture.pause"},
            },
        },
        "graphics": {"uses_gl": False},
    }


def expect_error(tool, document, source_root, token):
    try:
        tool.validate_project(document, source_root)
    except tool.ProjectError as error:
        require(token in str(error),
                "unexpected error for %r: %s" % (token, error))
        return
    raise GateError("invalid project passed: %s" % token)


def snapshot(root):
    result = {}
    for path in sorted(root.rglob("*")):
        if path.is_file():
            result[path.relative_to(root).as_posix()] = (
                stat.S_IMODE(path.stat().st_mode), path.read_bytes()
            )
    return result


def authored_project(source_root):
    payloads = {
        "INSTALLATION.md": b"# Install\n\nAuthor instructions.\n",
        "README.md": b"# Author Fixture\n\nFinished documentation.\n",
        "docs/NOTICE.txt": b"Third-party notice.\n",
        "tools/inspect.sh": b"#!/bin/sh\nexit 0\n",
    }
    for path, payload in payloads.items():
        write_file(source_root, path, payload,
                   0o755 if path.startswith("tools/") else 0o644)
    document = project_document()
    document["documentation"]["status"] = "authored"
    document["package_payload"] = [
        record("INSTALLATION.md", payloads["INSTALLATION.md"]),
        record("README.md", payloads["README.md"]),
        record("docs/NOTICE.txt", payloads["docs/NOTICE.txt"],
               kind="license-notice"),
        record("tools/inspect.sh", payloads["tools/inspect.sh"], mode="0755"),
    ]
    return document, payloads


def assert_schema_contract():
    schema = json.loads(SCHEMA_V3.read_text(encoding="utf-8"))
    payload = schema["properties"].get("package_payload")
    require(payload.get("type") == "array" and payload.get("maxItems") == 128,
            "schema lacks the bounded package_payload array")
    item = payload["items"]
    require(item.get("additionalProperties") is False and
            item.get("required") == ["path", "mode", "sha256", "kind"] and
            item["properties"]["kind"].get("enum") ==
            ["payload", "license-notice"],
            "schema package_payload record is not exact")
    require(schema["properties"]["documentation"]["properties"]["status"]
            .get("enum") == ["scaffold", "authored"],
            "schema documentation.status lacks authored")
    tuning = schema["properties"]["controls"]["properties"].get("tuning")
    require(tuning.get("additionalProperties") is False and
            set(tuning["properties"]) == {"cursor", "camera"},
            "schema controls.tuning is not closed")


def assert_positive_composition(tool, work, source_root):
    document, payloads = authored_project(source_root)
    output = work / "authored-output"
    generated, config = tool.generate_project(document, output, source_root)
    require(generated == output and
            [entry["kind"] for entry in config["package_payload"]] ==
            ["payload", "payload", "license-notice", "payload"],
            "returned config lost package_payload kind/order")
    port = output / document["nxport"]["id"]
    for path, payload in payloads.items():
        target = port / path
        expected_mode = 0o755 if path.startswith("tools/") else 0o644
        require(target.is_file() and not target.is_symlink() and
                target.read_bytes() == payload and
                stat.S_IMODE(target.stat().st_mode) == expected_mode,
                "author payload changed during composition: %s" % path)
    project = json.loads((port / "nxproject.json").read_text(encoding="utf-8"))
    require(project["package_payload"] == document["package_payload"],
            "nxproject.json did not retain the exact author records")
    receipt = json.loads((port / "GENERATION.json").read_text(encoding="utf-8"))
    inventory = {item["path"]: item for item in receipt["artifacts"]}
    for path, payload in payloads.items():
        key = document["nxport"]["id"] + "/" + path
        require(inventory.get(key) == {
            "path": key,
            "mode": "0755" if path.startswith("tools/") else "0644",
            "sha256": sha256(payload),
        }, "GENERATION.json did not honestly bind %s" % path)

    second = work / "authored-output-2"
    tool.generate_project(document, second, source_root)
    require(snapshot(output) == snapshot(second),
            "author payload composition is not deterministic")

    # validate_project retains the descriptor-audited bytes. Mutating the
    # source after that boundary cannot switch the copied payload underneath
    # its already checked record.
    original_bootstrap_state = tool.bootstrap_source_state
    original_readme = payloads["README.md"]

    def mutate_after_validation():
        write_file(source_root, "README.md", b"changed after validation\n")
        return original_bootstrap_state()

    tool.bootstrap_source_state = mutate_after_validation
    retained_output = work / "retained-output"
    try:
        tool.generate_project(document, retained_output, source_root)
    finally:
        tool.bootstrap_source_state = original_bootstrap_state
        write_file(source_root, "README.md", original_readme)
    require((retained_output / document["nxport"]["id"] / "README.md")
            .read_bytes() == original_readme,
            "source mutation replaced retained validated bytes")


def assert_payload_negatives(tool, work, source_root):
    base_payload = b"plain author payload\n"
    write_file(source_root, "notes/plain.txt", base_payload)
    valid = record("notes/plain.txt", base_payload)

    not_array = project_document()
    not_array["package_payload"] = None
    expect_error(tool, not_array, source_root, "must be an array")

    old_schema = project_document()
    old_schema["schema_version"] = 2
    old_schema.pop("language_access")
    old_schema.pop("controls")
    old_schema.pop("graphics")
    old_schema["package_payload"] = [valid]
    expect_error(tool, old_schema, source_root, "requires nxproject schema_version 3")

    for label, changed, token in (
            ("missing-kind", {key: value for key, value in valid.items()
                              if key != "kind"}, "must contain exactly"),
            ("extra", dict(valid, extra=True), "must contain exactly"),
            ("bad-mode", dict(valid, mode="0600"), "exactly 0644 or 0755"),
            ("bad-kind", dict(valid, kind="binary"),
             "payload or license-notice"),
            ("bad-sha", dict(valid, sha256="0" * 64),
             "SHA-256 differs")):
        candidate = project_document()
        candidate["package_payload"] = [changed]
        expect_error(tool, candidate, source_root, token)

    executable = project_document()
    executable["package_payload"] = [dict(valid, mode="0755")]
    expect_error(tool, executable, source_root, "only below tools/")

    wrong_source_mode = project_document()
    wrong_source_mode["package_payload"] = [dict(valid, mode="0755",
                                                  path="tools/plain.txt")]
    write_file(source_root, "tools/plain.txt", base_payload, 0o644)
    expect_error(tool, wrong_source_mode, source_root,
                 "mode differs from its declaration")

    for unsafe in ("../escape", "./notes/plain.txt", "notes//plain.txt",
                   "notes/./plain.txt", "notes\\plain.txt", ".hidden"):
        candidate = project_document()
        candidate["package_payload"] = [dict(valid, path=unsafe)]
        expect_error(tool, candidate, source_root,
                     "canonical non-hidden" if "\\" not in unsafe
                     else "forward slashes")

    unordered = project_document()
    unordered["package_payload"] = [
        dict(valid, path="z.txt"), dict(valid, path="a.txt")
    ]
    expect_error(tool, unordered, source_root, "lexicographically ordered")

    casefold = project_document()
    casefold["package_payload"] = [
        dict(valid, path="Docs/a.txt"), dict(valid, path="docs/A.txt")
    ]
    expect_error(tool, casefold, source_root, "casefold-unique")

    parent_collision = project_document()
    parent_collision["package_payload"] = [
        dict(valid, path="notes"), dict(valid, path="notes/plain.txt")
    ]
    expect_error(tool, parent_collision, source_root, "paths collide")

    too_many = project_document()
    too_many["package_payload"] = [
        dict(valid, path="many/%03d.txt" % index) for index in range(129)
    ]
    expect_error(tool, too_many, source_root, "128-file limit")

    for reserved in (
            "GENERATION.json", "LICENSE", "cover.png", "adapter/extra.txt",
            "defaults/extra.txt", "gamedata/extra.txt", "lib/extra.txt",
            "nxextract/extra.txt", "saves/extra.txt", "userdata/extra.txt",
            ".nxrelease/extra.txt", "port.json/extra.txt"):
        candidate = project_document()
        candidate["package_payload"] = [dict(valid, path=reserved)]
        expect_error(tool, candidate, source_root,
                     "canonical non-hidden" if reserved.startswith(".")
                     else ("reserved" if "/extra" not in reserved or
                           reserved.split("/", 1)[0] in
                           {"adapter", "defaults", "gamedata", "lib",
                            "nxextract", "saves", "userdata"}
                           else "collides with generation"))

    for runtime_path in (
            "bin/author-fixture-nextos", "Author Payload Fixture.sh"):
        candidate = project_document()
        candidate["package_payload"] = [dict(valid, path=runtime_path)]
        expect_error(tool, candidate, source_root, "collides with runtime")

    scaffold_docs = project_document()
    scaffold_docs["package_payload"] = [
        dict(valid, path="README.md", sha256=sha256(base_payload))
    ]
    write_file(source_root, "README.md", base_payload)
    expect_error(tool, scaffold_docs, source_root,
                 "scaffold documentation must not replace")

    authored_missing = project_document()
    authored_missing["documentation"]["status"] = "authored"
    authored_missing["package_payload"] = [
        dict(valid, path="README.md", sha256=sha256(base_payload))
    ]
    expect_error(tool, authored_missing, source_root,
                 "authored documentation requires")

    authored_absent = project_document()
    authored_absent["documentation"]["status"] = "authored"
    expect_error(tool, authored_absent, source_root,
                 "authored documentation requires")

    for documentation_path in ("INSTALLATION.md", "README.md"):
        wrong_kind, _payloads = authored_project(source_root)
        selected = next(
            item for item in wrong_kind["package_payload"]
            if item["path"] == documentation_path
        )
        selected["kind"] = "license-notice"
        expect_error(
            tool, wrong_kind, source_root,
            "documentation member %s must use kind payload" %
            documentation_path,
        )

    symlink_payload = b"symlink victim\n"
    write_file(source_root, "links/victim.txt", symlink_payload)
    (source_root / "links/link.txt").symlink_to("victim.txt")
    symlink_case = project_document()
    symlink_case["package_payload"] = [
        record("links/link.txt", symlink_payload)
    ]
    expect_error(tool, symlink_case, source_root, "non-symlink file")

    hard_payload = b"hardlink victim\n"
    hard_source = write_file(source_root, "links/hard-a.txt", hard_payload)
    os.link(hard_source, source_root / "links/hard-b.txt")
    hardlink_case = project_document()
    hardlink_case["package_payload"] = [
        record("links/hard-a.txt", hard_payload)
    ]
    expect_error(tool, hardlink_case, source_root, "single-link regular")

    large_payload = b"x" * (4 * 1024 * 1024 + 1)
    write_file(source_root, "large/one.dat", large_payload)
    large_case = project_document()
    large_case["package_payload"] = [record("large/one.dat", large_payload)]
    expect_error(tool, large_case, source_root, "4 MiB file limit")

    total_case = project_document()
    total_records = []
    for index in range(5):
        payload = bytes([65 + index]) * 3400000
        path = "total/%d.dat" % index
        write_file(source_root, path, payload)
        total_records.append(record(path, payload))
    total_case["package_payload"] = total_records
    expect_error(tool, total_case, source_root, "16 MiB total limit")

    forbidden_cases = {
        "blocked/game.apk": b"not even a zip\n",
        "blocked/libguest.so.1": b"not even ELF\n",
        "blocked/renamed.bin": b"PK\x03\x04renamed zip\n",
        "blocked/tool.bin": b"\x7fELFrenamed elf\n",
        "blocked/archive.dat": b"x" * 257 + b"ustar" + b"archive\n",
    }
    for path, payload in forbidden_cases.items():
        write_file(source_root, path, payload)
        candidate = project_document()
        candidate["package_payload"] = [record(path, payload)]
        expect_error(tool, candidate, source_root,
                     "forbidden ELF/game/archive payload")


def tuned_controls(document):
    document["controls"] = {
        "actions": [
            {"id": "fixture.accept", "kind": "button",
             "sinks": ["adapter.menu.accept"]},
            {"id": "fixture.camera", "kind": "vector",
             "sinks": ["adapter.game.camera"]},
            {"id": "fixture.cursor_click", "kind": "button",
             "sinks": ["adapter.cursor.click"]},
            {"id": "cursor.move", "kind": "vector",
             "sinks": ["adapter.cursor.move"]},
        ],
        "contexts": {
            "menu": {"A": "fixture.accept"},
            "gameplay": {"RIGHT_STICK": "fixture.camera"},
            "cursor": {
                "R3": "fixture.cursor_click", "RIGHT_STICK": "cursor.move",
            },
        },
        "tuning": {
            "cursor": {
                "speed": 5e-2,
                "deadzone": 0.9,
                "response_curve": 1.625,
                "acceleration": 4,
                "smoothing_ms": 70.5,
            },
            "camera": {
                "sensitivity_x": 0.05,
                "sensitivity_y": 8,
                "deadzone": 0,
                "response_curve": 4,
                "invert_x": True,
                "invert_y": False,
                "authority": "native",
            },
        },
    }
    return document


def compile_parser(work):
    compiler = shutil.which("cc")
    require(compiler is not None, "host C compiler is required")
    source = work / "parse-generated-gptk.c"
    source.write_text(
        r"""#include "nxinput_gptk.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  FILE *f; long n; char *p; nxinput_gptk g; char e[256]; int rc;
  if (argc != 2 || !(f = fopen(argv[1], "rb"))) return 2;
  if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 ||
      fseek(f, 0, SEEK_SET)) return 3;
  p = (char *)malloc((size_t)n + 1u); if (!p) return 4;
  if (fread(p, 1, (size_t)n, f) != (size_t)n) return 5;
  fclose(f);
  rc = nxinput_gptk_parse(p, (size_t)n, &g, e, sizeof e); free(p);
  if (rc) { fprintf(stderr, "%d %s\n", rc, e); return 6; }
  return 0;
}
""",
        encoding="utf-8",
    )
    binary = work / "parse-generated-gptk"
    subprocess.run([
        compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
        "-I", str(NXINPUT / "include"), str(source),
        str(NXINPUT / "src" / "nxinput_gptk.c"),
        str(NXINPUT / "src" / "nxinput_gptk_motion.c"),
        "-lm", "-o", str(binary),
    ], check=True)
    return binary


def assert_tuning(tool, work, source_root):
    baseline = tool.validate_project(project_document(), source_root)["controls"]
    require("tuning" not in baseline and
            tool.render_controls_sections(baseline) ==
            "[menu]\nA = fixture.accept\n\n"
            "[gameplay]\nSTART = fixture.pause\n",
            "schema3 without tuning changed its historical GPTK bytes")

    document = tuned_controls(project_document())
    config = tool.validate_project(document, source_root)
    rendered = tool.render_controls_sections(config["controls"])
    expected = (
        "[menu]\nA = fixture.accept\n\n"
        "[gameplay]\nRIGHT_STICK = fixture.camera\n\n"
        "[cursor]\nR3 = fixture.cursor_click\nRIGHT_STICK = cursor.move\n"
        "speed = 0.050000001\n"
        "deadzone = 0.899999976\nresponse_curve = 1.625\n"
        "acceleration = 4\nsmoothing_ms = 70.5\n\n"
        "[camera]\nsensitivity_x = 0.050000001\n"
        "sensitivity_y = 8\n"
        "deadzone = 0\nresponse_curve = 4\ninvert_x = true\n"
        "invert_y = false\nauthority = native\n"
    )
    require(rendered == expected,
            "tuning render order/decimal format drifted:\n%s" % rendered)
    numeric_keys = (set(tool.CURSOR_TUNING_RANGES) |
                    set(tool.CAMERA_TUNING_RANGES))
    for line in rendered.splitlines():
        if " = " in line and line.split(" = ", 1)[0] in numeric_keys:
            require("e" not in line.split(" = ", 1)[1].casefold(),
                    "tuning renderer emitted exponent notation")

    output = work / "tuning-output"
    tool.generate_project(document, output, source_root)
    gptk = (output / document["nxport"]["id"] / "defaults" /
            "NEXTOSCONTROLLERS.gptk")
    require(expected in gptk.read_text(encoding="utf-8"),
            "generated GPTK lost deterministic tuning sections")
    parser = compile_parser(work)
    subprocess.run([str(parser), str(gptk)], check=True)

    camera_only = project_document()
    camera_only["controls"]["tuning"] = {
        "camera": {"authority": "nextos"}
    }
    tool.validate_project(camera_only, source_root)

    # A real port may already expose its cursor in menu/gameplay rather than
    # switching to a dedicated context. Preserve those bindings and append a
    # tuning-only [cursor] section for the canonical parser.
    contextual_cursor = project_document()
    contextual_cursor["controls"]["actions"].extend([
        {"id": "cursor.move", "kind": "vector",
         "sinks": ["adapter.cursor.move"]},
        {"id": "fixture.cursor_click", "kind": "button",
         "sinks": ["adapter.cursor.click"]},
    ])
    contextual_cursor["controls"]["contexts"]["menu"].update({
        "RIGHT_STICK": "cursor.move", "R3": "fixture.cursor_click",
    })
    contextual_cursor["controls"]["tuning"] = {
        "cursor": {"speed": 1.25}
    }
    contextual_config = tool.validate_project(contextual_cursor, source_root)
    contextual_render = tool.render_controls_sections(
        contextual_config["controls"]
    )
    require("[menu]\n" in contextual_render and
            "R3 = fixture.cursor_click" in contextual_render and
            contextual_render.endswith("[cursor]\nspeed = 1.25\n"),
            "contextual cursor bindings were rewritten or tuning-only "
            "[cursor] was not appended")

    missing_cursor = project_document()
    missing_cursor["controls"]["tuning"] = {"cursor": {"speed": 1}}
    expect_error(tool, missing_cursor, source_root,
                 "requires a bound cursor.* vector action")

    wrong_r3_kind = tuned_controls(project_document())
    click = next(item for item in wrong_r3_kind["controls"]["actions"]
                 if item["id"] == "fixture.cursor_click")
    click["kind"] = "axis"
    expect_error(tool, wrong_r3_kind, source_root,
                 "R3 must be button")

    for tuning, token in (
            ({}, "non-empty object"),
            ({"unknown": {}}, "non-empty object"),
            ({"camera": {}}, "non-empty object"),
            ({"camera": {"unknown": 1}}, "known tuning keys"),
            ({"camera": {"invert_x": 1}}, "must be a boolean"),
            ({"camera": {"authority": "both"}}, "nextos or native"),
            ({"camera": {"sensitivity_x": True}}, "finite number")):
        candidate = project_document()
        candidate["controls"]["tuning"] = tuning
        expect_error(tool, candidate, source_root, token)

    numeric_ranges = {
        "cursor": tool.CURSOR_TUNING_RANGES,
        "camera": tool.CAMERA_TUNING_RANGES,
    }
    for section, ranges in numeric_ranges.items():
        for key, (minimum, maximum) in ranges.items():
            for value in (minimum - 0.01, maximum + 0.01):
                candidate = tuned_controls(project_document())
                candidate["controls"]["tuning"][section][key] = value
                expect_error(tool, candidate, source_root, "finite number")


def main():
    tool = load_tool()
    assert_schema_contract()
    with tempfile.TemporaryDirectory(
            prefix="nxgenerator-package-payload-") as temporary:
        work = Path(temporary)
        source_root = work / "source"
        source_root.mkdir()
        write_file(source_root, "LICENSE", (REPOSITORY / "LICENSE").read_bytes())
        assert_positive_composition(tool, work, source_root)
        assert_payload_negatives(tool, work, source_root)
        assert_tuning(tool, work, source_root)
    print(
        "nxgenerator package-payload tests passed: authored=1 receipt=1 "
        "deterministic=1 retained_bytes=1 payload_negatives=46 "
        "tuning_parser=1 tuning_negatives=25 legacy_bytes=1"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (GateError, OSError, ValueError, KeyError,
            subprocess.CalledProcessError) as error:
        print("nxgenerator package-payload tests failed: %s" % error)
        raise SystemExit(1)
