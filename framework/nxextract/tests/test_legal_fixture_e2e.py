#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""APK-COMPAT-01: the generated pipeline against a REAL legal container.

The V3 audit closed with one honest gap: every APK-compatibility test used
synthetic blobs. Synthetic fixtures cannot catch the failure that opened this
front -- a published artifact rejecting the very data its own documentation
declared compatible -- because the synthetic case is built to match.

This gate runs the REAL `nxextract` pipeline (`plan`, the whole resolution
path, not isolated functions) against an owner-provided legal copy, and proves
the metamorphic property the contract actually claims:

    the identity of the container never decides compatibility.

The same bytes are presented under a different file name, repacked with a
different member order, compression method, timestamps and extra fields, and
wrapped in a bundle container. Every variant must resolve the SAME payload set.
Then the negatives: another game's package, a container missing a required
payload, and an ABI the recipe does not declare.

The proprietary artifact NEVER enters the repository. Nothing is discovered:
both paths must be supplied explicitly, and the receipt records only technical
identity -- package family, ABI, payload count and sizes -- never the original
file name, never the source path, never the origin.
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[3]
NXEXTRACT = (REPOSITORY / "suportando_outros_devices" / "extrator-universal" /
             "nxextract.py")
SKIP = 77


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def skip(message):
    print("nxextract legal-fixture gate: SKIP (%s)" % message)
    print("nxextract legal-fixture gate: this is a LIMIT OF CLAIM, not a pass. "
          "Supply NXEXTRACT_LEGAL_APK and NXEXTRACT_LEGAL_RECIPE to exercise "
          "it.")
    raise SystemExit(SKIP)


def run_plan(recipe, game_dir, extra=()):
    command = [sys.executable, "-B", str(NXEXTRACT), "plan",
               "--recipe", str(recipe), "--game-dir", str(game_dir)]
    command.extend(extra)
    completed = subprocess.run(command, capture_output=True, text=True,
                               timeout=1800)
    return completed


def parse_plan(stdout):
    """The plan prints a human log before its JSON document; take the JSON."""
    start = stdout.find("{")
    require(start >= 0, "the plan produced no JSON document")
    return json.loads(stdout[start:])


def payload_fingerprint(plan):
    """The resolved payload set, independent of where it came from."""
    return sorted(
        (item["destination"], item["size"], item["crc32"])
        for item in plan["items"]
    )


def new_game_dir(root, name, recipe_source):
    game = Path(root) / name
    (game / "gamedata").mkdir(parents=True)
    shutil.copy2(str(recipe_source), str(game / "extractor.json"))
    return game


def repack(source, target, method):
    """Rewrite the container with everything a repacker is free to change."""
    with zipfile.ZipFile(str(source)) as original:
        entries = list(original.infolist())
        entries.reverse()
        with zipfile.ZipFile(str(target), "w", method) as rebuilt:
            for entry in entries:
                info = zipfile.ZipInfo(entry.filename, (2107, 1, 1, 0, 0, 0))
                info.compress_type = method
                info.create_system = 0
                info.external_attr = 0x20
                info.extra = b"\xef\xbe\x04\x00spam"
                rebuilt.writestr(info, original.read(entry.filename))


def _strip_flat(payload, victim):
    """Return the bytes of an archive rebuilt without one member."""
    import io
    source = io.BytesIO(payload)
    output = io.BytesIO()
    with zipfile.ZipFile(source) as original:
        with zipfile.ZipFile(output, "w", zipfile.ZIP_STORED) as rebuilt:
            for entry in original.infolist():
                if entry.filename == victim:
                    continue
                rebuilt.writestr(entry.filename, original.read(entry.filename))
    return output.getvalue()


def strip_member(source, target, victim):
    """Remove one member, descending one nesting level for bundles.

    A bundle carries the payload inside an inner APK, so removing an entry from
    the wrapper would not remove the payload at all -- and a negative that does
    not remove what it claims to remove proves nothing.
    """
    with zipfile.ZipFile(str(source)) as original:
        names = set(original.namelist())
        with zipfile.ZipFile(str(target), "w", zipfile.ZIP_STORED) as rebuilt:
            for entry in original.infolist():
                payload = original.read(entry.filename)
                if entry.filename == victim:
                    continue
                if victim not in names and \
                        entry.filename.lower().endswith(".apk"):
                    try:
                        inner = zipfile.ZipFile(__import__("io").BytesIO(
                            payload))
                    except zipfile.BadZipFile:
                        inner = None
                    if inner is not None:
                        has_victim = victim in inner.namelist()
                        inner.close()
                        if has_victim:
                            payload = _strip_flat(payload, victim)
                rebuilt.writestr(entry.filename, payload)


def wrap_bundle(source, target):
    """A bundle container: the APK becomes a member of an .xapk wrapper."""
    with zipfile.ZipFile(str(target), "w", zipfile.ZIP_STORED) as bundle:
        bundle.write(str(source), "base.apk")


def foreign_container(target):
    with zipfile.ZipFile(str(target), "w", zipfile.ZIP_STORED) as archive:
        archive.writestr("AndroidManifest.xml", b"not a real manifest")
        archive.writestr("lib/arm64-v8a/libforeign.so", b"\x7fELF" + b"\0" * 60)
        archive.writestr("classes.dex", b"dex\n035\0" + b"\0" * 32)


def main():
    apk = os.environ.get("NXEXTRACT_LEGAL_APK", "").strip()
    recipe = os.environ.get("NXEXTRACT_LEGAL_RECIPE", "").strip()
    receipt_path = os.environ.get("NXEXTRACT_LEGAL_RECEIPT", "").strip()
    if not apk or not recipe:
        skip("no legal container was supplied")
    apk_path = Path(apk)
    recipe_path = Path(recipe)
    if not apk_path.is_file() or apk_path.is_symlink():
        skip("the supplied legal container is not a regular file")
    if not recipe_path.is_file():
        skip("the supplied recipe is not a regular file")

    digest = hashlib.sha256()
    with open(str(apk_path), "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    container_sha256 = digest.hexdigest()
    container_size = apk_path.stat().st_size

    is_bundle = apk_path.suffix.lower() in (".apkm", ".apks", ".xapk")
    root = tempfile.mkdtemp(prefix="nxextract-legal.")
    variants = []
    try:
        # 1. As provided, under a neutral name. The original file name is
        #    never used: it may carry a distributor, and it must never be a
        #    compatibility condition anyway.
        baseline = new_game_dir(root, "baseline", recipe_path)
        shutil.copy2(str(apk_path),
                     str(baseline / "gamedata" / ("base" + apk_path.suffix)))
        completed = run_plan(baseline / "extractor.json", baseline)
        require(completed.returncode == 0,
                "the legal reference copy was REJECTED by its own recipe: %s"
                % (completed.stderr.strip() or completed.stdout.strip()))
        plan = parse_plan(completed.stdout)
        expected = payload_fingerprint(plan)
        require(expected, "the plan resolved no payload at all")
        variants.append(("as-provided", "accepted"))

        # 2. Renamed container: identity of the file name decides nothing.
        renamed = new_game_dir(root, "renamed", recipe_path)
        shutil.copy2(str(apk_path),
                     str(renamed / "gamedata" /
                         ("totally-different-name" + apk_path.suffix)))
        completed = run_plan(renamed / "extractor.json", renamed)
        require(completed.returncode == 0,
                "a renamed container was rejected: %s" % completed.stderr)
        require(payload_fingerprint(parse_plan(completed.stdout)) == expected,
                "a renamed container resolved a different payload set")
        variants.append(("renamed", "accepted"))

        # 3. Repacked: reversed member order, stored instead of deflated,
        #    far-future timestamps, DOS attributes and foreign extra fields.
        repacked = new_game_dir(root, "repacked", recipe_path)
        repack(apk_path, repacked / "gamedata" / ("base" + apk_path.suffix),
               zipfile.ZIP_STORED)
        completed = run_plan(repacked / "extractor.json", repacked)
        require(completed.returncode == 0,
                "a repacked container was rejected: %s" % completed.stderr)
        require(payload_fingerprint(parse_plan(completed.stdout)) == expected,
                "a repacked container resolved a different payload set")
        variants.append(("repacked-stored-reordered", "accepted"))

        # 4. A different supported container format carrying the same
        #    content. A plain APK is wrapped into an .xapk bundle; a reference
        #    copy that is ALREADY a bundle is re-presented under the sibling
        #    bundle extension instead, because a bundle inside a bundle is a
        #    different (and correctly refused) shape, not a rename.
        bundled = new_game_dir(root, "bundled", recipe_path)
        if is_bundle:
            variant_name = "bundle-extension-swap"
            target = bundled / "gamedata" / (
                "owner.apks" if apk_path.suffix.lower() != ".apks"
                else "owner.apkm")
            shutil.copy2(str(apk_path), str(target))
        else:
            variant_name = "bundle-xapk"
            wrap_bundle(apk_path, bundled / "gamedata" / "owner.xapk")
        completed = run_plan(bundled / "extractor.json", bundled)
        require(completed.returncode == 0,
                "a %s container was rejected: %s"
                % (variant_name, completed.stderr))
        require(payload_fingerprint(parse_plan(completed.stdout)) == expected,
                "a %s container resolved a different payload set"
                % variant_name)
        variants.append((variant_name, "accepted"))

        # --- negatives -------------------------------------------------
        foreign = new_game_dir(root, "foreign", recipe_path)
        foreign_container(foreign / "gamedata" / "base.apk")
        completed = run_plan(foreign / "extractor.json", foreign)
        require(completed.returncode != 0,
                "another game's container was accepted")
        variants.append(("foreign-package", "refused"))

        # Strip a payload the recipe REQUIRES, not merely the first resolved
        # one: the claim is that a missing required payload fails closed and
        # names the technical property.
        required = [item for item in plan["items"]
                    if "engine-library" in str(item.get("rule", ""))]
        if not required:
            required = [max(plan["items"], key=lambda item: item["size"])]
        victim = required[0]["source_entry"]
        incomplete = new_game_dir(root, "incomplete", recipe_path)
        strip_member(apk_path,
                     incomplete / "gamedata" / ("base" + apk_path.suffix),
                     victim)
        completed = run_plan(incomplete / "extractor.json", incomplete)
        require(completed.returncode != 0,
                "a container missing a required payload was accepted")
        combined = completed.stderr + completed.stdout
        require(any(token in combined for token in
                    ("required payload", "was not found",
                     "no input set matches")),
                "the refusal did not name the missing technical requirement")
        variants.append(("missing-required-payload", "refused"))

        undeclared = new_game_dir(root, "undeclared-abi", recipe_path)
        shutil.copy2(str(apk_path),
                     str(undeclared / "gamedata" / ("base" + apk_path.suffix)))
        completed = run_plan(undeclared / "extractor.json", undeclared,
                             ("--abi", "mips64"))
        require(completed.returncode != 0,
                "an ABI the recipe never declared was accepted")
        variants.append(("undeclared-abi", "refused"))

        document = {
            "schema": "org.nextos.nxextract.legal-fixture-receipt",
            "schema_version": 1,
            "sanitized": True,
            "recipe": {"id": plan.get("recipe"),
                       "version": plan.get("recipe_version")},
            "reference_copy": {
                "container_size": container_size,
                "container_sha256": container_sha256,
                "note": ("documentation only: identity of the container never "
                         "decides compatibility"),
            },
            "resolved": {
                "abi": plan.get("abi"),
                "payload_count": len(plan["items"]),
                "total_bytes": plan.get("total_bytes"),
            },
            "variants": [{"variant": name, "result": result}
                         for name, result in variants],
        }
    finally:
        shutil.rmtree(root, ignore_errors=True)

    if receipt_path:
        Path(receipt_path).write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
    print(json.dumps(document, indent=2, sort_keys=True))
    print("nxextract legal-fixture gate passed: positives=4 negatives=3 "
          "payloads=%d" % len(plan["items"]))


if __name__ == "__main__":
    try:
        main()
    except GateError as error:
        print("nxextract legal-fixture gate FAILED: %s" % error,
              file=sys.stderr)
        raise SystemExit(1)
