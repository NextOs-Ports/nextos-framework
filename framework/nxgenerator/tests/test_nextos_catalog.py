#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Directed gate for the NextOS website/app catalog exporter."""

import base64
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parents[1]
TOOL = ROOT / "catalog_export.py"
SCHEMA = ROOT / "schema" / "nextos-port-catalog-v1.schema.json"
EXAMPLE = ROOT / "examples" / "nextos-port-catalog.example.json"
NXPROJECT = ROOT / "tests" / "fixtures" / "c3-baseline" / "nxproject.json"


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def load_tool():
    specification = importlib.util.spec_from_file_location(
        "nextos_catalog_under_test", TOOL
    )
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def run(catalog, output, expected=0, nxproject=NXPROJECT, assets_root=None):
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    result = subprocess.run(
        [sys.executable, "-B", str(TOOL), str(catalog),
         "--nxproject", str(nxproject), "--output", str(output),
         "--source-root", str(REPOSITORY)] +
        ([] if assets_root is None else
         ["--assets-root", str(assets_root)]),
        cwd=str(REPOSITORY), env=environment, stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        check=False,
    )
    require(result.returncode == expected,
            "catalog exporter returned %d, expected %d: %s" %
            (result.returncode, expected, result.stderr.strip()))
    return result


def write_json(path, document):
    path.write_text(
        json.dumps(document, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def expect_invalid(tool, document, config, label):
    try:
        tool.validate_catalog(document, config)
    except tool.CatalogError:
        return
    raise GateError("catalog accepted %s" % label)


def ronax_reference():
    return {
        "id": "chrono-nextos",
        "name": "CHRONO TRIGGER",
        "description": (
            "Chrono Trigger running natively on handhelds. A NextOS port "
            "with no emulator or Android layer: the mobile release runs "
            "directly on handheld Linux. Travel through prehistory, the "
            "middle ages, the present, and a post-apocalyptic future to stop "
            "Lavos. Includes the remastered art and soundtrack plus DS bonus "
            "content."
        ),
        "build": {"version": "1.1.0", "type": "beta"},
        "links": {
            "source": "https://github.com/NextOs-Ports/chrono-nextos",
            "latest_release": (
                "https://github.com/NextOs-Ports/chrono-nextos/releases/"
                "latest/download/chrono.zip"
            ),
        },
        "images": {
            "cover": "port-json/chrono-nextos/cover.svg",
            "screenshots": [
                "port-json/chrono-nextos/screenshot.svg"
            ],
            "placeholder": True,
        },
        "installation": (
            "Extract the ZIP into your firmware's ports/ folder. Put your "
            "own Chrono Trigger APK in chrono/gamedata/. Launch Chrono "
            "Trigger from the Ports menu; it installs itself on first run."
        ),
    }


def main():
    tool = load_tool()
    require((ROOT / "VERSION").read_text(encoding="ascii").strip() == "0.4.5",
            "catalog exporter component version is not 0.4.5")
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    require(schema["additionalProperties"] is False and
            schema["required"] == list(tool.CATALOG_KEYS),
            "catalog schema is not closed or ordered like Ronax v1")
    tool.audit_catalog_schema(schema)
    drifted_schema = copy.deepcopy(schema)
    drifted_schema["properties"]["images"]["additionalProperties"] = True
    try:
        tool.audit_catalog_schema(drifted_schema)
    except tool.CatalogError:
        pass
    else:
        raise GateError("nested catalog schema drift was not rejected")

    # The exact object supplied by Ronax remains accepted without injecting a
    # schema marker, bindings or any other field his current consumer did not
    # request.  Technical binding remains outside the editorial output.
    chrono_config = {
        "nxport": {"id": "chrono", "title": "Chrono Trigger"},
        "recipe_source": Path("fixture"),
        "owner_data": {"directory": "gamedata"},
    }
    reference = ronax_reference()
    normalized = tool.validate_catalog(reference, chrono_config)
    require(normalized == reference,
            "Ronax reference changed while being normalized")
    require(list(normalized) == list(tool.CATALOG_KEYS),
            "Ronax top-level presentation order changed")
    rendered_reference = tool.render_catalog(normalized)
    require(json.loads(rendered_reference) == reference and
            rendered_reference.endswith(b"\n"),
            "Ronax reference is not deterministic UTF-8 JSON")

    project = tool.load_strict_json(NXPROJECT, "nxproject fixture")
    config = tool.CORE.validate_project(project, REPOSITORY)
    example = tool.load_strict_json(EXAMPLE, "catalog example")
    work = Path(tempfile.mkdtemp(prefix="nxgenerator-catalog."))
    try:
        # Exact Ronax bytes traverse the complete CLI: strict file loader,
        # real nxproject validation, schema audit, semantic binding and
        # no-replace publication.
        chrono_project = copy.deepcopy(project)
        chrono_project["nxport"]["id"] = "chrono"
        chrono_project["nxport"]["title"] = "Chrono Trigger"
        chrono_project["nxport"]["launcher_name"] = "Chrono Trigger.sh"
        chrono_project_path = work / "chrono-nxproject.json"
        chrono_catalog_path = work / "chrono-catalog.json"
        chrono_output = work / "chrono-nextos.json"
        write_json(chrono_project_path, chrono_project)
        write_json(chrono_catalog_path, reference)
        run(chrono_catalog_path, chrono_output, nxproject=chrono_project_path)
        require(chrono_output.read_bytes() == rendered_reference,
                "Ronax reference changed in the end-to-end exporter")

        first = work / "first.json"
        second = work / "second.json"
        result = run(EXAMPLE, first)
        require("generated NextOS catalog" in result.stdout,
                "catalog exporter did not report its output")
        run(EXAMPLE, second)
        require(first.read_bytes() == second.read_bytes(),
                "two catalog exports differ")
        require(stat.S_IMODE(first.stat().st_mode) == 0o644,
                "catalog output mode is not 0644")
        require(json.loads(first.read_text(encoding="utf-8")) == example,
                "catalog output differs from the validated example")
        before = hashlib.sha256(first.read_bytes()).hexdigest()
        run(EXAMPLE, first, expected=1)
        require(hashlib.sha256(first.read_bytes()).hexdigest() == before,
                "no-overwrite failure changed existing catalog bytes")

        # Non-placeholder assets are never accepted on faith.  They must be
        # present below an explicit website root and match their declared
        # image type. Placeholder references stay metadata-only by design.
        real = copy.deepcopy(example)
        real["images"] = {
            "cover": "port-json/nxexample-aarch64-nextos/cover.png",
            "screenshots": [
                "port-json/nxexample-aarch64-nextos/screenshot.webp",
                "port-json/nxexample-aarch64-nextos/screenshot.jpg",
                "port-json/nxexample-aarch64-nextos/screenshot.svg",
            ],
            "placeholder": False,
        }
        real_path = work / "real-images.json"
        write_json(real_path, real)
        run(real_path, work / "missing-assets.json", expected=1)
        assets_root = work / "site"
        assets_dir = assets_root / "port-json" / "nxexample-aarch64-nextos"
        assets_dir.mkdir(parents=True)
        valid_png = base64.b64decode(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lE"
            "QVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="
        )
        valid_webp = base64.b64decode(
            "UklGRkAAAABXRUJQVlA4WAoAAAAQAAAAAAAAAAAAQUxQSAIAAAAA"
            "AFZQOCAYAAAAMAEAnQEqAQABAAIANCWkAANwAP77/VAA"
        )
        valid_jpeg = base64.b64decode(
            "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgK"
            "DBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0"
            "Hyc5PTgyPC4zNDL/wAALCAABAAEBAREA/8QAFAABAAAAAAAAAAAA"
            "AAAAAAAAB//EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQEAAD8A"
            "f3//2Q=="
        )
        valid_svg = (
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1 1">'
            '<defs><linearGradient id="g"><stop offset="0" '
            'stop-color="#fff"/></linearGradient></defs>'
            '<rect width="1" height="1" fill="url(#g)"/></svg>'
        ).encode("utf-8")
        (assets_dir / "cover.png").write_bytes(valid_png)
        (assets_dir / "screenshot.webp").write_bytes(valid_webp)
        (assets_dir / "screenshot.jpg").write_bytes(valid_jpeg)
        (assets_dir / "screenshot.svg").write_bytes(valid_svg)
        real_result = run(
            real_path, work / "real-output.json", assets_root=assets_root
        )
        require("assets_verified=4" in real_result.stdout,
                "catalog assets did not pass bounded format checks")
        require(hashlib.sha256(valid_png).hexdigest() in real_result.stdout and
                hashlib.sha256(valid_webp).hexdigest() in real_result.stdout and
                hashlib.sha256(valid_jpeg).hexdigest() in real_result.stdout and
                hashlib.sha256(valid_svg).hexdigest() in real_result.stdout,
                "point-in-time asset hashes were not emitted")
        (assets_dir / "cover.png").write_bytes(b"\x89PNG\r\n\x1a\nfixture")
        run(real_path, work / "fake-image-output.json", expected=1,
            assets_root=assets_root)
        (assets_dir / "cover.png").write_bytes(valid_png)
        unsafe = copy.deepcopy(real)
        unsafe["images"] = {
            "cover": "port-json/nxexample-aarch64-nextos/unsafe.svg",
            "screenshots": [
                "port-json/nxexample-aarch64-nextos/screenshot.webp"
            ],
            "placeholder": False,
        }
        unsafe_svg_variants = (
            '<svg><script>alert(1)</script></svg>',
            '<svg><a href="javascript:alert(1)"><text>x</text></a></svg>',
            '<svg><style>path{fill:url(https://example.test/x)}</style></svg>',
            '<svg><image href="relative-external.svg"/></svg>',
            '<svg><animate attributeName="href" to="https://example.test"/></svg>',
            '<svg><use id="loop" href="#loop"/></svg>',
        )
        for source in unsafe_svg_variants:
            (assets_dir / "unsafe.svg").write_text(source, encoding="utf-8")
            try:
                tool.validate_catalog_assets(unsafe, assets_root)
            except tool.CatalogError:
                pass
            else:
                raise GateError("unsafe SVG was accepted: %s" % source)

        for build_type, version in (
            ("alpha", "1.0.0-alpha.1"),
            ("beta", "1.0.0"),
            ("rc", "1.0.0-rc.1"),
            ("stable", "1.0.0+build.7"),
        ):
            positive = copy.deepcopy(example)
            positive["build"] = {"version": version, "type": build_type}
            positive["description"] += " Catálogo com acentuação válida."
            tool.validate_catalog(positive, config)
        no_owner_config = dict(config)
        no_owner_config["recipe_source"] = None
        no_owner = copy.deepcopy(example)
        no_owner["installation"] = "Extract the ZIP and launch the port."
        tool.validate_catalog(no_owner, no_owner_config)

        duplicate = work / "duplicate.json"
        duplicate.write_text('{"id":"x","id":"y"}\n', encoding="utf-8")
        run(duplicate, work / "duplicate-out.json", expected=1)
        bom = work / "bom.json"
        bom.write_bytes(b"\xef\xbb\xbf{}\n")
        run(bom, work / "bom-out.json", expected=1)
        constant = work / "nan.json"
        constant.write_text('{"id":NaN}\n', encoding="utf-8")
        run(constant, work / "nan-out.json", expected=1)
        surrogate_document = copy.deepcopy(example)
        surrogate_document["description"] = "isolated \ud800 surrogate"
        surrogate = work / "surrogate.json"
        surrogate.write_text(
            json.dumps(surrogate_document, ensure_ascii=True) + "\n",
            encoding="ascii",
        )
        surrogate_output = work / "surrogate-out.json"
        surrogate_result = run(surrogate, surrogate_output, expected=1)
        require("Traceback" not in surrogate_result.stderr and
                not surrogate_output.exists(),
                "isolated surrogate did not fail cleanly")
        invalid_utf8 = work / "invalid-utf8.json"
        invalid_utf8.write_bytes(b"{\"id\":\xff}\n")
        run(invalid_utf8, work / "utf8-out.json", expected=1)
        oversized = work / "oversized.json"
        oversized.write_bytes(b" " * (tool.MAX_INPUT_BYTES + 1))
        run(oversized, work / "oversized-out.json", expected=1)
        symlinked = work / "catalog-symlink.json"
        symlinked.symlink_to(EXAMPLE)
        run(symlinked, work / "symlink-out.json", expected=1)
        hard_source = work / "hard-source.json"
        hard_copy = work / "hard-copy.json"
        shutil.copyfile(EXAMPLE, hard_source)
        os.link(hard_source, hard_copy)
        run(hard_source, work / "hardlink-out.json", expected=1)
        output_target = work / "output-target.json"
        output_target.write_text("sentinel\n", encoding="utf-8")
        output_link = work / "output-link.json"
        output_link.symlink_to(output_target)
        run(EXAMPLE, output_link, expected=1)
        require(output_target.read_text(encoding="utf-8") == "sentinel\n",
                "output symlink failure changed its target")
        real_parent = work / "real-parent"
        real_parent.mkdir()
        linked_parent = work / "linked-parent"
        linked_parent.symlink_to(real_parent, target_is_directory=True)
        run(EXAMPLE, linked_parent / "output.json", expected=1)

        # Once renameat2 commits the full file, a directory-fsync failure is
        # rolled back through the already pinned directory descriptor.  The
        # CLI must never report failure while silently leaving an output.
        rollback_output = work / "rollback-output.json"
        original_fsync = tool.os.fsync
        directory_fsync_calls = [0]

        def fail_second_directory_fsync(descriptor):
            if stat.S_ISDIR(os.fstat(descriptor).st_mode):
                directory_fsync_calls[0] += 1
                if directory_fsync_calls[0] == 2:
                    raise OSError(5, "injected directory fsync failure")
            return original_fsync(descriptor)

        tool.os.fsync = fail_second_directory_fsync
        try:
            try:
                tool.publish_no_replace(first.read_bytes(), rollback_output)
            except tool.CatalogError:
                pass
            else:
                raise GateError("post-rename fsync failure was accepted")
        finally:
            tool.os.fsync = original_fsync
        require(not rollback_output.exists() and
                not list(work.glob(".nextos-catalog.*")),
                "failed publication left output or staging residue")

        negatives = []
        bad = copy.deepcopy(example); bad["extra"] = True
        negatives.append((bad, "an unknown root field"))
        bad = copy.deepcopy(example); del bad["description"]
        negatives.append((bad, "a missing root field"))
        bad = copy.deepcopy(example); bad["id"] = "../escape"
        negatives.append((bad, "an unsafe id"))
        bad = copy.deepcopy(example); bad["id"] = "unrelated-nextos"
        bad["links"]["source"] = \
            "https://github.com/NextOs-Ports/unrelated-nextos"
        bad["links"]["latest_release"] = \
            "https://github.com/NextOs-Ports/unrelated-nextos/releases/" \
            "latest/download/nxexample-aarch64.zip"
        bad["images"]["cover"] = "port-json/unrelated-nextos/cover.svg"
        bad["images"]["screenshots"] = [
            "port-json/unrelated-nextos/screenshot.svg"
        ]
        negatives.append((bad, "an id unrelated to nxport"))
        bad = copy.deepcopy(example); bad["id"] = "nxexample-nextos-"
        negatives.append((bad, "a slug with a trailing hyphen"))
        bad = copy.deepcopy(example); bad["name"] = "Different title"
        negatives.append((bad, "a title divergent from nxport"))
        bad = copy.deepcopy(example); bad["name"] += "\n"
        negatives.append((bad, "edge whitespace/control in name"))
        bad = copy.deepcopy(example); bad["build"]["extra"] = True
        negatives.append((bad, "an unknown nested build field"))
        bad = copy.deepcopy(example); del bad["links"]["source"]
        negatives.append((bad, "a missing nested link field"))
        bad = copy.deepcopy(example); bad["description"] = "<script>x</script>"
        negatives.append((bad, "markup in description"))
        bad = copy.deepcopy(example); bad["description"] = "from APKPure"
        negatives.append((bad, "a forbidden package origin"))
        bad = copy.deepcopy(example); bad["description"] = "/home/example-user/x"
        negatives.append((bad, "a private host path"))
        bad = copy.deepcopy(example); bad["description"] = "/mnt/private/x"
        negatives.append((bad, "a private mount path"))
        bad = copy.deepcopy(example); bad["description"] = "/root/private/x"
        negatives.append((bad, "a root-private path"))
        bad = copy.deepcopy(example); bad["description"] = "/tmp/private/x"
        negatives.append((bad, "a temporary host path"))
        bad = copy.deepcopy(example); bad["description"] = "host fe80::1"
        negatives.append((bad, "an IPv6 host literal"))
        bad = copy.deepcopy(example); bad["description"] = "isolated \ud800"
        negatives.append((bad, "an isolated Unicode surrogate"))
        bad = copy.deepcopy(example); bad["build"]["version"] = "v1.0"
        negatives.append((bad, "a non-SemVer build"))
        bad = copy.deepcopy(example); bad["build"]["version"] = "1.0.0-01"
        negatives.append((bad, "a SemVer numeric prerelease with leading zero"))
        bad = copy.deepcopy(example); bad["build"] = {
            "version": "1.0.0-rc.1", "type": "stable"
        }
        negatives.append((bad, "a stable prerelease"))
        bad = copy.deepcopy(example); bad["build"]["type"] = "nightly"
        negatives.append((bad, "an unknown build type"))
        bad = copy.deepcopy(example); bad["links"]["source"] = \
            "http://github.com/NextOs-Ports/nxexample-aarch64-nextos"
        negatives.append((bad, "an HTTP source"))
        bad = copy.deepcopy(example); bad["links"]["source"] = \
            "https://user@github.com/NextOs-Ports/nxexample-aarch64-nextos"
        negatives.append((bad, "URL credentials"))
        bad = copy.deepcopy(example); bad["links"]["source"] = \
            "https://localhost/NextOs-Ports/nxexample-aarch64-nextos"
        negatives.append((bad, "a local hostname"))
        bad = copy.deepcopy(example); bad["links"]["source"] = \
            "https://127.0.0.1/NextOs-Ports/nxexample-aarch64-nextos"
        negatives.append((bad, "an IP hostname"))
        bad = copy.deepcopy(example); bad["links"]["source"] = \
            "https://github..com/NextOs-Ports/nxexample-aarch64-nextos"
        negatives.append((bad, "an invalid DNS hostname"))
        bad = copy.deepcopy(example); bad["links"]["source"] = \
            "https://github.com:443/NextOs-Ports/nxexample-aarch64-nextos"
        negatives.append((bad, "an explicit URL port"))
        bad = copy.deepcopy(example); bad["links"]["source"] += "?tracking=1"
        negatives.append((bad, "a URL query"))
        bad = copy.deepcopy(example); bad["links"]["source"] += "#fragment"
        negatives.append((bad, "a URL fragment"))
        bad = copy.deepcopy(example); bad["links"]["source"] += "/"
        negatives.append((bad, "a noncanonical source trailing slash"))
        bad = copy.deepcopy(example); bad["links"]["source"] = \
            "https://GITHUB.COM/NextOs-Ports/nxexample-aarch64-nextos"
        negatives.append((bad, "a noncanonical source hostname spelling"))
        bad = copy.deepcopy(example); bad["links"]["source"] = \
            "https://github.com/Other/nxexample-aarch64-nextos"
        bad["links"]["latest_release"] = \
            "https://github.com/Other/nxexample-aarch64-nextos/releases/" \
            "latest/download/nxexample-aarch64.zip"
        negatives.append((bad, "a repository outside NextOs-Ports"))
        bad = copy.deepcopy(example); bad["links"]["latest_release"] = \
            "https://github.com/Other/repo/releases/latest/download/" \
            "nxexample-aarch64.zip"
        negatives.append((bad, "a release outside the source repository"))
        bad = copy.deepcopy(example); bad["links"]["latest_release"] = \
            "https://github.com/NextOs-Ports/nxexample-aarch64-nextos/" \
            "releases/latest/download/wrong.zip"
        negatives.append((bad, "a release asset divergent from nxport"))
        bad = copy.deepcopy(example); bad["links"]["latest_release"] = \
            "https://github.com/NextOs-Ports/nxexample-aarch64-nextos/" \
            "releases/not-latest/download/nxexample-aarch64.zip"
        negatives.append((bad, "a release path that is not latest"))
        bad = copy.deepcopy(example); bad["images"]["cover"] = "../cover.svg"
        negatives.append((bad, "image traversal"))
        bad = copy.deepcopy(example); bad["images"]["cover"] = \
            "/port-json/nxexample-aarch64-nextos/cover.svg"
        negatives.append((bad, "an absolute image path"))
        bad = copy.deepcopy(example); bad["images"]["cover"] = \
            "port-json\\nxexample-aarch64-nextos\\cover.svg"
        negatives.append((bad, "a backslash image path"))
        bad = copy.deepcopy(example); bad["images"]["cover"] = \
            "port-json/other-nextos/cover.svg"
        negatives.append((bad, "an image under another id"))
        bad = copy.deepcopy(example); bad["images"]["cover"] = \
            "port-json/nxexample-aarch64-nextos/.cover.svg"
        negatives.append((bad, "a hidden image path"))
        bad = copy.deepcopy(example); bad["images"]["cover"] = \
            "port-json/nxexample-aarch64-nextos/cover.gif"
        negatives.append((bad, "an unsupported image extension"))
        bad = copy.deepcopy(example); bad["images"]["screenshots"] = []
        negatives.append((bad, "an empty screenshot list"))
        bad = copy.deepcopy(example); bad["images"]["screenshots"] = [
            bad["images"]["cover"]
        ]
        negatives.append((bad, "a duplicate cover/screenshot"))
        bad = copy.deepcopy(example); bad["images"]["screenshots"] = [
            "port-json/nxexample-aarch64-nextos/shot-%02d.png" % index
            for index in range(13)
        ]
        negatives.append((bad, "more than twelve screenshots"))
        bad = copy.deepcopy(example); bad["images"]["placeholder"] = 1
        negatives.append((bad, "a non-boolean placeholder"))
        bad = copy.deepcopy(example); bad["installation"] = \
            "Extract it without naming the owner-data directory."
        negatives.append((bad, "installation missing canonical gamedata"))
        for document, label in negatives:
            expect_invalid(tool, document, config, label)

        print("nxgenerator NextOS catalog gate passed: ronax_e2e=1 "
              "schema_exact=1 deterministic=1 no_overwrite=1 mode=0644 "
              "cross_binding=5 strict_boundaries=12 format_checked_assets=4 "
              "asset_hash_receipt=4 unsafe_svg=6 surrogate_cli=1 "
              "rollback_after_rename=1 "
              "positive_variants=5 negatives=%d" %
              len(negatives))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
