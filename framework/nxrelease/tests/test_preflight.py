#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V4-05A directed gates: the aggregate READ-ONLY preflight.

Synthetic fixtures only -- no stage, no ZIP, no device, no global battery.
Covers the mission's mandatory classes: multiple independent errors together
in stable order; blocked-not-pass dependencies; zero mutation on success and
failure; the receipt schema nxledger really accepts, bound to commit/tree;
staleness on any byte change; the DEV-receipt refusal on the public path;
profile validation; and the sanitized exit codes."""

import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

COMPONENT = pathlib.Path(__file__).resolve().parent.parent
FRAMEWORK = COMPONENT.parent
REPO = FRAMEWORK.parent


def require(condition, message):
    if not condition:
        raise SystemExit("nxrelease preflight gate FAILED: %s" % message)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, str(path))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def git(root, *arguments):
    return subprocess.run(
        ["git", "-C", str(root), *arguments], stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        check=True).stdout.strip()


def snapshot(root):
    """Every path+mtime+size under root, so mutation is provable."""
    entries = []
    for path in sorted(pathlib.Path(root).rglob("*")):
        if ".git" in path.parts:
            # `git status` inside the identity derivation refreshes the git
            # index mtime; that is the derivation's own bookkeeping, not a
            # mutation of the certified bytes.
            continue
        try:
            meta = path.lstat()
        except OSError:
            continue
        entries.append((str(path), meta.st_mtime_ns, meta.st_size))
    return entries


def build_repo(base):
    """A minimal real git repository with a synthetic project."""
    root = pathlib.Path(base) / "repo"
    project = root / "ports" / "example"
    (project / "defaults").mkdir(parents=True)
    (project / "adapter").mkdir()
    (project / "recipes").mkdir()
    (project / "nxproject.json").write_text(json.dumps({
        "schema_version": 3,
        "nxport": {"executable": "example-nextos"},
        "promotion": {"claims": {"physical_support_proven": False}},
        "documentation": {"proven_support": []},
        "controls": {"controller_profiles": {
            "enabled": True, "bundle": "controllers.nxb",
            "sha256": __import__("hashlib").sha256(
                b"NXCONTROLLER_PROFILES/1\n").hexdigest()}},
    }, indent=1))
    (project / "INSTALLATION.md").write_text(
        "# Installation / Instalação\nEnglish steps.\nPassos em português.\n")
    (project / "defaults" / "NEXTOSCONTROLLERS.gptk").write_text(
        "format = NEXTOS_CONTROLLERS/3\nFACE_LAYOUT = auto\n"
        "[menu]\nA = ui.a\n[gameplay]\nB = ui.b\n")
    (project / "launcher.sh").write_text("#!/bin/sh\necho ok\n")
    (project / "controllers.nxb").write_text("NXCONTROLLER_PROFILES/1\n")
    # authorities the nxledger derive demands: reuse the REAL framework tree
    # via a shallow copy of only the VERSION files and the contract.
    for name, rel in load("nxpf_ledger_probe",
                          FRAMEWORK / "nxledger" / "nxledger.py"
                          ).AUTHORITY_VERSION_FILES:
        source = REPO / rel
        target = root / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(source.read_text())
    contract = REPO / "framework/contracts/declarative-v1.json"
    target = root / "framework/contracts/declarative-v1.json"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(contract.read_text())
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    git(root, "config", "user.email", "gate@nextos.local")
    git(root, "config", "user.name", "NX Gate")
    git(root, "add", "-A")
    git(root, "commit", "-qm", "synthetic preflight fixture")
    return root, project


def make_manifest(base, root):
    manifest = pathlib.Path(base) / "inputs.json"
    manifest.write_text(json.dumps({
        "schema": "org.nextos.v4.oneshot-inputs",
        "schema_version": 1,
        "elves": [{
            "logical_path": "bin/example-nextos",
            "file": str(FRAMEWORK / "nxsplash" / "release" / "aarch64" /
                        "nxsplash-nextos"),
        }],
    }))
    return manifest


def run_preflight(module, root, project, manifest, profile, out=None):
    argv = ["--profile", profile, "--source", str(root),
            "--project", str(project), "--inputs-manifest", str(manifest)]
    if out:
        argv += ["--out", str(out)]
    import contextlib
    import io
    stdout = io.StringIO()
    with contextlib.redirect_stdout(stdout):
        code = module.main(argv)
    return code, stdout.getvalue()


def main():
    module = load("nxpf", COMPONENT / "nxpreflight.py")
    work = tempfile.mkdtemp(prefix="nxpreflight-gate.")
    try:
        root, project = build_repo(work)
        manifest = make_manifest(work, root)

        # ---- profile discipline: no default, invalid refused ------------
        code, _ = run_preflight(module, root, project, manifest, "DEV")
        require(code == module.EXIT_USAGE, "an invalid profile must be usage")

        # ---- a PASS run is read-only and deterministic ------------------
        before = snapshot(root)
        code, text_a = run_preflight(module, root, project, manifest,
                                     "DEV/device-matrix")
        require(code == module.EXIT_PASS,
                "the synthetic fixture must PASS (got %d):\n%s" %
                (code, text_a))
        require("READ-ONLY" in text_a and "effects: none" in text_a,
                "the header does not announce the read-only contract")
        require(snapshot(root) == before,
                "a PASS run mutated the repository (read-only violated)")
        code, text_b = run_preflight(module, root, project, manifest,
                                     "DEV/device-matrix")
        require(text_b == text_a, "the report is not deterministic")

        # ---- receipt: schema, binding, ledger acceptance ----------------
        out = pathlib.Path(work) / "receipt.json"
        code, _ = run_preflight(module, root, project, manifest,
                                "PUBLIC-FINAL", out=out)
        require(code == module.EXIT_PASS and out.is_file(),
                "the PUBLIC-FINAL receipt was not produced")
        require(oct(out.stat().st_mode & 0o777) == "0o444",
                "the receipt is not sealed 0444")
        receipt = json.loads(out.read_text())
        require(receipt["schema"] == "org.nextos.v4.preflight-receipt" and
                receipt["schema_version"] == 1,
                "receipt schema is not the one nxledger accepts")
        require(receipt["commit"] == git(root, "rev-parse", "HEAD") and
                receipt["dirty"] is False,
                "receipt is not bound to the exact commit")
        require(len(receipt["authorities"]) == 18,
                "receipt does not carry the 18-authority map")
        require(receipt["sdl_authority"]["floor"] == "2.0.4" and
                receipt["sdl_authority"]["sha256"],
                "receipt does not pin the SDL authority")
        require([c["id"] for c in receipt["categories"]] ==
                list(module.CATEGORIES),
                "categories are not reported in the stable order")
        nxledger = load("nxpf_ledger", FRAMEWORK / "nxledger" / "nxledger.py")
        loaded, _digest = nxledger._load_receipt_document(
            str(out), nxledger.PREFLIGHT_SCHEMA,
            nxledger.PREFLIGHT_SCHEMA_VERSION, "preflight receipt")
        require(loaded["result"] == "PASS",
                "nxledger's own loader rejects the receipt")

        # ---- the public-final guard: exact chain only -------------------
        module.require_preflight_receipt(str(out), root, nxledger)
        # DEV receipt never authorizes a public candidate.
        dev_out = pathlib.Path(work) / "receipt-dev.json"
        run_preflight(module, root, project, manifest, "DEV/device-matrix",
                      out=dev_out)
        try:
            module.require_preflight_receipt(str(dev_out), root, nxledger)
            require(False, "a DEV receipt authorized the public path")
        except module.PreflightUsage as refusal:
            require("PUBLIC-FINAL" in str(refusal),
                    "the DEV refusal does not name the required profile")
        # FAIL receipt refused.
        broken = json.loads(out.read_text())
        broken["result"] = "FAIL"
        fail_out = pathlib.Path(work) / "receipt-fail.json"
        fail_out.write_text(json.dumps(broken))
        try:
            module.require_preflight_receipt(str(fail_out), root, nxledger)
            require(False, "a FAIL receipt authorized the public path")
        except module.PreflightUsage:
            pass
        # stale receipt: one byte of source changes the commit/tree.
        (project / "INSTALLATION.md").write_text(
            "# Installation / Instalação\nEnglish.\nPortuguês (edited).\n")
        git(root, "add", "-A")
        git(root, "commit", "-qm", "mutate one byte")
        try:
            module.require_preflight_receipt(str(out), root, nxledger)
            require(False, "a stale receipt survived a source change")
        except module.PreflightUsage as refusal:
            require("stale" in str(refusal), "staleness is not named")

        # ---- multiple independent errors together, blocked-not-pass -----
        (project / "nxproject.json").write_text("{not json")
        (project / "launcher.sh").write_text("#!/bin/sh\nstat /tmp/x\n")
        (project / "INSTALLATION.md").unlink()
        bad_manifest = pathlib.Path(work) / "bad-inputs.json"
        bad_manifest.write_text("{}")
        git(root, "add", "-A")
        git(root, "commit", "-qm", "break independently")
        before = snapshot(root)
        code, text = run_preflight(module, root, project, bad_manifest,
                                   "DEV/device-matrix")
        require(code == module.EXIT_FINDINGS,
                "independent failures must exit with the findings code")
        require("strict-json" in text and "external-stat" in text and
                "installation-missing" in text and
                "inputs-manifest" in text,
                "independent errors were not all reported together:\n%s" %
                text)
        require("[BLOCKED" in text and "depends" not in text.split(
                "[BLOCKED", 1)[0],
                "a dependent gate did not surface as BLOCKED")
        require("Traceback" not in text, "a traceback leaked to the report")
        require(snapshot(root) == before,
                "a FAILING run mutated the repository")

        # ---- --out discipline: refuse existing, refuse in-repo ----------
        try:
            module.write_receipt(str(out), {"x": 1}, root)
            require(False, "an existing --out was overwritten")
        except module.PreflightUsage:
            pass
        try:
            module.write_receipt(str(root / "receipt-in-repo.json"),
                                 {"x": 1}, root)
            require(False, "an in-repo, non-ignored --out was accepted")
        except module.PreflightUsage:
            pass

        print("nxrelease 0.3.23 aggregate preflight: PASS "
              "read_only=2 receipt=1 ledger_accepts=1 guard=4 "
              "independent_errors=4 blocked=1 out_discipline=2")
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
