#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V4-05A: the aggregate READ-ONLY preflight boundary of nxrelease.

One explicit command evaluates every release category in a deterministic
order and reports EVERY independent error before any stage, copy, build or
ZIP exists. It mutates nothing except the one explicitly authorized `--out`
receipt file. A gate whose input already failed is BLOCKED/NOT_RUN with its
dependency named -- never a green PASS or SKIP.

On success it emits exactly the receipt schema nxledger accepts,
`org.nextos.v4.preflight-receipt/1`, bound to the repository commit/tree,
the inputs manifest, the SDL authority and the identities of the three tools
that produced it. nxledger refuses a PUBLIC-FINAL `package-candidate`
reservation without a PASS receipt bound to the exact commit/tree, and the
nxrelease public-final path independently refuses a receipt whose profile is
not PUBLIC-FINAL (a DEV receipt never authorizes a public candidate).
"""

import argparse
import hashlib
import importlib.util
import json
import os
import re
import sys
from pathlib import Path

MODULE_DIR = Path(__file__).resolve().parent
FRAMEWORK_DIR = MODULE_DIR.parent
NXABI_PATH = FRAMEWORK_DIR / "nxabi" / "nxabi.py"
NXABI_POLICY = FRAMEWORK_DIR / "nxabi" / "policy-v1.json"
NXLEDGER_PATH = FRAMEWORK_DIR / "nxledger" / "nxledger.py"

RECEIPT_SCHEMA = "org.nextos.v4.preflight-receipt"
RECEIPT_SCHEMA_VERSION = 1
SDL_PUBLIC_FLOOR = "2.0.4"

# Exit codes (documented; no traceback ever reaches the user).
EXIT_PASS = 0
EXIT_USAGE = 2
EXIT_FINDINGS = 3
EXIT_BLOCKED = 4

CATEGORIES = (
    "schema", "pins", "privacy", "closure", "abi", "sdl-authority",
    "claims", "installation", "controls", "shell-stat", "profile",
)

PERSONAL_RE = re.compile(
    r"(/home/[A-Za-z0-9._-]+|/storage/roms|/roms/ports|"
    r"\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b|"
    r"apkpure|apkmirror|apkvision|5play)", re.IGNORECASE)
STAT_RE = re.compile(r"(^|[;&|()`]|\s)stat(\s|$)")
GPTK_FACE_RE = re.compile(r"^FACE_LAYOUT[ \t]*=[ \t]*(.+?)[ \t]*$")


class PreflightUsage(Exception):
    pass


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, str(path))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def sha256_file(path):
    digest = hashlib.sha256()
    with open(str(path), "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def strict_json_bytes(raw, label):
    def unique(pairs):
        seen = {}
        for key, value in pairs:
            if key in seen:
                raise ValueError("duplicate JSON key %r in %s" % (key, label))
            seen[key] = value
        return seen
    return json.loads(raw.decode("utf-8"), object_pairs_hook=unique)


class Category:
    def __init__(self, identifier):
        self.identifier = identifier
        self.state = "PASS"
        self.findings = []
        self.depends_on = None

    def error(self, code, message):
        self.state = "ERROR"
        self.findings.append({"code": code, "message": str(message)[:400]})

    def blocked(self, dependency, message):
        self.state = "BLOCKED"
        self.depends_on = dependency
        self.findings.append({"code": "blocked", "message": message})

    def as_receipt(self):
        entry = {
            "id": self.identifier,
            "state": self.state,
            "findings": [item["code"] for item in self.findings],
            "count": len(self.findings),
        }
        if self.depends_on:
            entry["depends_on"] = self.depends_on
        return entry


def run_preflight(arguments):
    project = Path(arguments.project).resolve()
    root = Path(arguments.source).resolve()
    nxledger = load_module("nxpreflight_nxledger", NXLEDGER_PATH)
    nxabi = load_module("nxpreflight_nxabi", NXABI_PATH)

    if arguments.profile not in nxledger.PROFILES:
        raise PreflightUsage(
            "profile must be one of %s (no default exists)" %
            (nxledger.PROFILES,))
    if not project.is_dir():
        raise PreflightUsage("--project must be an existing directory")
    if not (root / ".git").exists():
        raise PreflightUsage("--source must be the repository checkout")

    # ---- repository identity FIRST: everything else binds to it ---------
    ledger = nxledger.derive(str(root))
    repo = ledger["repo"]
    authorities = ledger["components"]
    contract = nxledger.read_contract_identity(str(root), with_sha256=True)

    header = [
        "nxrelease preflight (READ-ONLY aggregate boundary)",
        "  profile: %s" % arguments.profile,
        "  commit:  %s" % repo["head_commit"],
        "  tree:    %s" % repo["tree"],
        "  dirty:   %s" % repo["dirty"],
        "  effects: none (no stage, no ZIP, no copy, no hook, no ledger "
        "mutation; only the explicit --out receipt)",
        "  steps:   %s" % ", ".join(CATEGORIES),
    ]
    print("\n".join(header))

    categories = {name: Category(name) for name in CATEGORIES}

    if repo["dirty"]:
        categories["pins"].error(
            "repo-dirty", "working tree is dirty; a preflight binds exact "
            "bytes and can never certify unversioned edits")

    # ---- inputs manifest (nxledger authority, read-only) ----------------
    inputs = None
    inputs_error = None
    try:
        inputs = nxledger.load_inputs_manifest(
            arguments.inputs_manifest, str(root), repo)
    except Exception as error:  # LedgerError/IOFailure: sanitized message
        inputs_error = str(error)
        categories["profile"].error("inputs-manifest", inputs_error)

    # 1. schema -----------------------------------------------------------
    category = categories["schema"]
    project_document = None
    for name in ("nxproject.json",):
        path = project / name
        if not path.is_file() or path.is_symlink():
            category.error("missing-%s" % name, "%s absent" % name)
            continue
        try:
            project_document = strict_json_bytes(path.read_bytes(), name)
        except (ValueError, OSError) as error:
            category.error("strict-json", error)
    for name in ("adapter/adapter-contract.json", "extractor.json"):
        path = project / name
        if path.is_file() and not path.is_symlink():
            try:
                strict_json_bytes(path.read_bytes(), name)
            except (ValueError, OSError) as error:
                category.error("strict-json", error)

    # 2. pins / versions / git identity -----------------------------------
    category = categories["pins"]
    try:
        nxrelease_version = (
            FRAMEWORK_DIR / "nxrelease" / "VERSION").read_text().strip()
        if authorities.get("nxrelease") != nxrelease_version:
            category.error(
                "authority-vs-version",
                "nxledger authority says nxrelease %s, VERSION says %s" %
                (authorities.get("nxrelease"), nxrelease_version))
    except OSError as error:
        category.error("version-io", error)
    public_pin = project / "FRAMEWORK-PIN.json"
    private_pin = project / "recipes" / "framework-release-pin-v1.json"
    if public_pin.is_file() and private_pin.is_file():
        try:
            if strict_json_bytes(public_pin.read_bytes(), "FRAMEWORK-PIN") != \
                    strict_json_bytes(private_pin.read_bytes(), "recipe pin"):
                category.error(
                    "pin-divergence",
                    "public and recipe framework pins differ")
        except (ValueError, OSError) as error:
            category.error("pin-json", error)

    # 3. privacy ----------------------------------------------------------
    category = categories["privacy"]
    scanned = 0
    for path in sorted(project.rglob("*")):
        if scanned >= 400:
            break
        if not path.is_file() or path.is_symlink():
            continue
        if path.suffix not in (".md", ".json", ".sh", ".txt", ".cfg"):
            continue
        relative = path.relative_to(project).as_posix()
        if relative.startswith((".nxrelease/", "gamedata/", "game/")):
            continue
        scanned += 1
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        match = PERSONAL_RE.search(text)
        if match:
            category.error(
                "private-content",
                "%s carries a private path/IP/store token (%r)" %
                (relative, match.group(0)[:24]))

    # 4. closure / private SDL -------------------------------------------
    category = categories["closure"]
    for path in sorted(project.rglob("libSDL*")):
        relative = path.relative_to(project).as_posix()
        if relative.startswith("src/"):
            continue  # vendored SOURCE, not a shipped provider
        category.error(
            "private-sdl", "packaged private SDL provider: %s" % relative)

    # 5. abi (nxabi audit over the declared ELFs) ------------------------
    category = categories["abi"]
    if inputs is None:
        category.blocked(
            "profile", "inputs manifest failed; the ELF set is undefined")
    else:
        try:
            policy = nxabi.load_policy(NXABI_POLICY)
            table = nxabi.load_sdl_authority_for_policy(
                policy, NXABI_POLICY)["table"]
            options = {"profile": "universal-low-glibc", "sdl_floor": None}
            declared = strict_json_bytes(
                Path(arguments.inputs_manifest).read_bytes(),
                "inputs manifest")
            for entry in declared.get("elves", []):
                file_path = Path(entry["file"])
                if not file_path.is_absolute():
                    file_path = root / file_path
                elf_record = nxabi.inspect_elf(
                    file_path, logical_path=entry["logical_path"])
                findings = nxabi.apply_exceptions(
                    nxabi.audit_record(elf_record, policy, table, options),
                    elf_record, policy, options)
                for item in findings:
                    if item["level"] == "error":
                        category.error(item["check"], item["message"])
        except Exception as error:
            category.error("abi-audit", error)

    # 6. the single SDL authority ----------------------------------------
    category = categories["sdl-authority"]
    sdl_authority = None
    try:
        policy = nxabi.load_policy(NXABI_POLICY)
        sdl_authority = nxabi.load_sdl_authority_for_policy(
            policy, NXABI_POLICY)
        declared = policy.get("sdl", {}).get("floor")
        if declared != SDL_PUBLIC_FLOOR:
            category.error(
                "floor-drift", "policy floor %r != %r" %
                (declared, SDL_PUBLIC_FLOOR))
        verdicts = dict(
            (symbol, verdict) for symbol, verdict, _ in
            nxabi.decide_sdl_floor(
                ("SDL_Init", "SDL_JoystickGetVendor", "SDL_NotARealSymbol"),
                sdl_authority["table"], SDL_PUBLIC_FLOOR))
        if verdicts.get("SDL_JoystickGetVendor") != "post-floor" or \
                verdicts.get("SDL_NotARealSymbol") != "unknown":
            category.error(
                "decision-drift",
                "the shared floor decision no longer classifies the field "
                "case and the unknown case correctly")
    except Exception as error:
        category.error("sdl-authority", error)

    # 7. claims -----------------------------------------------------------
    category = categories["claims"]
    if project_document is None:
        category.blocked("schema", "nxproject.json failed strict parsing")
    else:
        claims = (project_document.get("promotion") or {}).get("claims") or {}
        if claims.get("physical_support_proven") is not False:
            category.error(
                "physical-claim",
                "physical_support_proven must be exactly false before any "
                "physical round")
        proven = (project_document.get("documentation") or {}).get(
            "proven_support")
        if proven:
            category.error(
                "proven-support",
                "documentation.proven_support must stay empty for a "
                "candidate")

    # 8. installation / names --------------------------------------------
    category = categories["installation"]
    installation = project / "INSTALLATION.md"
    if not installation.is_file() or installation.is_symlink():
        category.error("installation-missing", "INSTALLATION.md absent")
    else:
        text = installation.read_text(encoding="utf-8", errors="replace")
        lowered = text.lower()
        if not (("português" in lowered or "portugues" in lowered or
                 "instalação" in lowered or "instalacao" in lowered) and
                ("english" in lowered or "installation" in lowered)):
            category.error(
                "installation-bilingual",
                "INSTALLATION.md does not read as bilingual")
    if project_document is not None:
        executable = ((project_document.get("nxport") or {})
                      .get("executable") or "")
        if executable and not executable.endswith("-nextos"):
            category.error(
                "canonical-name",
                "public executable %r is not <port>-nextos" % executable)

    # 9. controls / GPTK / bundles ---------------------------------------
    category = categories["controls"]
    gptk = project / "defaults" / "NEXTOSCONTROLLERS.gptk"
    if gptk.is_file() and not gptk.is_symlink():
        text = gptk.read_text(encoding="utf-8", errors="replace")
        magics = [line for line in text.splitlines()
                  if line.startswith("format = NEXTOS_CONTROLLERS/")]
        if len(magics) != 1 or magics[0].rsplit("/", 1)[-1] not in (
                "1", "2", "3"):
            category.error("gptk-magic", "default GPTK magic is not /1..3")
        elif magics[0].endswith("/3"):
            face_lines = [GPTK_FACE_RE.match(line)
                          for line in text.splitlines()
                          if GPTK_FACE_RE.match(line)]
            if len(face_lines) != 1 or face_lines[0].group(1) not in (
                    "auto", "modern", "retro"):
                category.error(
                    "face-layout",
                    "a V3 default requires exactly one lowercase "
                    "FACE_LAYOUT = auto|modern|retro line")
    if project_document is not None:
        profiles = ((project_document.get("controls") or {})
                    .get("controller_profiles") or {})
        checks = []
        if profiles.get("enabled"):
            checks.append((profiles.get("bundle"), profiles.get("sha256")))
            for name, entry in (profiles.get("face_layout_variants") or
                                {}).items():
                checks.append((entry.get("bundle"), entry.get("sha256")))
        for bundle_name, digest in checks:
            bundle = project / str(bundle_name)
            if not bundle.is_file() or bundle.is_symlink():
                category.error(
                    "bundle-missing",
                    "pinned bundle %s absent or unsafe" % bundle_name)
            elif sha256_file(bundle) != digest:
                category.error(
                    "bundle-hash",
                    "pinned bundle %s diverges from its SHA-256" %
                    bundle_name)

    # 10. external stat in shells ----------------------------------------
    category = categories["shell-stat"]
    for path in sorted(project.rglob("*.sh")):
        relative = path.relative_to(project).as_posix()
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line in text.splitlines():
            stripped = line.split("#", 1)[0]
            if "/proc/" in stripped:
                continue
            if STAT_RE.search(stripped):
                category.error(
                    "external-stat",
                    "%s invokes the external stat command" % relative)
                break

    # 11. profile / one-shot eligibility ---------------------------------
    category = categories["profile"]
    if inputs is not None and not inputs["elves"]:
        category.error("no-elves", "inputs manifest declares no ELF")
    if arguments.profile == "PUBLIC-FINAL":
        # Read-only note: the reservation itself will also demand the
        # physical families and a host-battery PASS; the preflight only
        # certifies that this repository state may attempt it.
        pass

    ordered = [categories[name] for name in CATEGORIES]
    errors = sum(1 for item in ordered if item.state == "ERROR")
    blocked = sum(1 for item in ordered if item.state == "BLOCKED")
    result = "PASS" if errors == 0 and blocked == 0 and not repo["dirty"] \
        else ("BLOCKED" if blocked and not errors else "FAIL")

    receipt = {
        "schema": RECEIPT_SCHEMA,
        "schema_version": RECEIPT_SCHEMA_VERSION,
        "profile": arguments.profile,
        "commit": repo["head_commit"],
        "tree": repo["tree"],
        "dirty": repo["dirty"],
        "authorities": authorities,
        "declarative_contract": contract,
        "inputs_manifest_schema": "org.nextos.v4.oneshot-inputs/1",
        "inputs_manifest_sha256":
            inputs["manifest_sha256"] if inputs else None,
        "sdl_authority": {
            "id": sdl_authority["authority"] if sdl_authority else None,
            "sha256": sdl_authority["sha256"] if sdl_authority else None,
            "floor": SDL_PUBLIC_FLOOR,
        },
        "categories": [item.as_receipt() for item in ordered],
        "result": result,
        "tools": {
            "nxrelease": {
                "version": authorities.get("nxrelease"),
                "sha256": sha256_file(MODULE_DIR / "nxrelease.py"),
            },
            "nxabi": {
                "version": authorities.get("nxabi"),
                "sha256": sha256_file(NXABI_PATH),
            },
            "nxledger": {
                "version": authorities.get("nxledger"),
                "sha256": sha256_file(NXLEDGER_PATH),
            },
        },
    }

    for item in ordered:
        line = "  [%s] %s" % (item.state.ljust(7), item.identifier)
        if item.findings:
            line += " (%d finding%s: %s)" % (
                len(item.findings), "s" if len(item.findings) != 1 else "",
                ", ".join(f["code"] for f in item.findings[:6]))
        print(line)
        for found in item.findings[:12]:
            print("      - %s: %s" % (found["code"], found["message"]))
    print("preflight result: %s" % result)

    if arguments.out:
        write_receipt(arguments.out, receipt, root)
    if arguments.json:
        print(json.dumps(receipt, indent=1, sort_keys=True))
    if result == "PASS":
        return EXIT_PASS
    return EXIT_BLOCKED if result == "BLOCKED" else EXIT_FINDINGS


def write_receipt(out, receipt, root):
    """Atomic, refuse-existing, and only outside the repo (or git-ignored)."""
    path = Path(out)
    if path.exists() or path.is_symlink():
        raise PreflightUsage("--out must name a NEW file")
    if not path.is_absolute():
        raise PreflightUsage("--out must be absolute")
    try:
        inside = path.resolve().is_relative_to(root)
    except AttributeError:  # pragma: no cover (py<3.9)
        inside = str(path.resolve()).startswith(str(root) + os.sep)
    if inside:
        import subprocess
        probe = subprocess.run(
            ["git", "-C", str(root), "check-ignore", "-q", str(path)],
            stdin=subprocess.DEVNULL)
        if probe.returncode != 0:
            raise PreflightUsage(
                "--out inside the repository must be provably git-ignored; "
                "a receipt may never dirty the tree it certifies")
    encoded = (json.dumps(receipt, indent=1, sort_keys=True) + "\n").encode()
    temporary = path.with_name("." + path.name + ".tmp")
    descriptor = os.open(
        str(temporary), os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
        0o600)
    try:
        os.write(descriptor, encoded)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    os.rename(str(temporary), str(path))
    os.chmod(str(path), 0o444)
    print("preflight receipt: %s" % path)


def require_preflight_receipt(path, root, nxledger=None):
    """The public-final guard: PASS + PUBLIC-FINAL + exact commit/tree.

    A DEV receipt, a FAIL/BLOCKED receipt, a wrong schema or a stale
    commit/tree refuses BEFORE the first mutation. Returns the receipt.
    """
    if nxledger is None:
        nxledger = load_module("nxpreflight_nxledger", NXLEDGER_PATH)
    if path is None:
        raise PreflightUsage("preflight receipt missing")
    try:
        receipt, _digest = nxledger._load_receipt_document(
            path, RECEIPT_SCHEMA, RECEIPT_SCHEMA_VERSION,
            "preflight receipt")
    except Exception as error:  # LedgerError/IOFailure: sanitized
        raise PreflightUsage(str(error)[:300])
    if receipt.get("result") != "PASS":
        raise PreflightUsage("preflight receipt result is not PASS")
    if receipt.get("profile") != "PUBLIC-FINAL":
        raise PreflightUsage(
            "a %r preflight receipt never authorizes a public candidate; "
            "PUBLIC-FINAL requires its own receipt" % receipt.get("profile"))
    ledger = nxledger.derive(str(root))
    repo = ledger["repo"]
    if receipt.get("commit") != repo["head_commit"] or \
            receipt.get("tree") != repo["tree"]:
        raise PreflightUsage(
            "preflight receipt is stale: it is not bound to this exact "
            "commit/tree")
    if repo["dirty"]:
        raise PreflightUsage("working tree is dirty; the receipt cannot "
                             "certify unversioned bytes")
    return receipt


def main(argv=None):
    parser = argparse.ArgumentParser(prog="nxpreflight")
    parser.add_argument("--profile", required=True)
    parser.add_argument("--source", required=True,
                        help="repository checkout root")
    parser.add_argument("--project", required=True,
                        help="the port directory under audit")
    parser.add_argument("--inputs-manifest", required=True,
                        help="org.nextos.v4.oneshot-inputs/1 manifest")
    parser.add_argument("--out", help="NEW external receipt path (atomic)")
    parser.add_argument("--json", action="store_true",
                        help="also print the stable JSON receipt")
    arguments = parser.parse_args(argv)
    try:
        return run_preflight(arguments)
    except PreflightUsage as error:
        print("nxpreflight: %s" % error, file=sys.stderr)
        return EXIT_USAGE
    except Exception as error:  # sanitized: no traceback
        print("nxpreflight: blocked: %s" % str(error)[:400], file=sys.stderr)
        return EXIT_BLOCKED


if __name__ == "__main__":
    sys.exit(main())
