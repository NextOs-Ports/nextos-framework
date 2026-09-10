#!/usr/bin/env python3
"""Regenerate a port's launcher and refresh every hash the release manifest pins.

A port is meant to be built by the framework, not hand-assembled. Today the
opposite happens: someone edits the generated launcher, the release gate
rejects it for diverging from the canonical template, and closing the package
then takes a chain of manual hash edits -- the launcher contract version, the
nxrelease tool version and its own SHA, and one source hash per file that moved.
Every one of those is derivable, and a human deriving them by hand is how a
stale pin reaches a release.

This tool derives them. It never invents a value: each hash is computed from
the file it pins, and each version is read from the framework component that
owns it.

Usage:
  nx-refresh-pins.py --port-dir DIR [--framework-root DIR] [--check]

--check reports what would change and exits non-zero if anything is stale,
which is what a release gate wants; without it the files are rewritten.
"""

import argparse
import hashlib
import importlib.util
import json
import re
import stat
import subprocess
import sys
from pathlib import Path, PurePosixPath


PACKAGE_PAYLOAD_FILE_LIMIT = 4 * 1024 * 1024
PACKAGE_PAYLOAD_TOTAL_LIMIT = 16 * 1024 * 1024


def sha256_of(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_version(path: Path) -> str:
    return path.read_text(encoding="utf-8").strip()


def manifest_source_for(manifest: dict, packaged_path: str) -> str:
    """Translate a path inside the package to the file that produces it.

    A manifest names the launcher config by where it lands in the package, and
    that is not always where it lives in the source tree; the files table
    already carries the mapping.
    """
    for entry in manifest.get("files", []):
        if entry.get("target") == packaged_path and entry.get("source"):
            return entry["source"]
    return packaged_path


def _safe_project_relative(value: object, context: str,
                           allow_dot: bool = False) -> str:
    if not isinstance(value, str) or not value or "\\" in value:
        raise SystemExit(f"{context} is not a safe relative path")
    if allow_dot and value == ".":
        return value
    logical = PurePosixPath(value)
    if (logical.is_absolute() or logical.as_posix() != value or
            any(part in ("", ".", "..") for part in logical.parts)):
        raise SystemExit(f"{context} is not a safe relative path")
    return value


def _directory_beneath(root: Path, relative: str, context: str) -> Path:
    """Resolve a declared directory without following any symlink member."""
    current = root
    if relative == ".":
        return current
    for part in PurePosixPath(relative).parts:
        current = current / part
        try:
            metadata = current.lstat()
        except OSError as error:
            raise SystemExit(f"{context} is missing: {error}")
        if not stat.S_ISDIR(metadata.st_mode):
            raise SystemExit(f"{context} contains a non-directory or symlink")
    return current


def _regular_beneath(root: Path, relative: str, context: str) -> Path:
    """Open a declared regular-file path without an intermediate symlink."""
    parts = PurePosixPath(relative).parts
    current = root
    for part in parts[:-1]:
        current = current / part
        try:
            metadata = current.lstat()
        except OSError as error:
            raise SystemExit(f"{context} source is missing: {error}")
        if not stat.S_ISDIR(metadata.st_mode):
            raise SystemExit(
                f"{context} source path contains a non-directory or symlink"
            )
    source = current / parts[-1]
    try:
        metadata = source.lstat()
    except OSError as error:
        raise SystemExit(f"{context} source is missing: {error}")
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
        raise SystemExit(f"{context} source is not a private regular file")
    return source


def _canonical_nxsplash(framework_root: Path, nxport: dict) -> tuple[str, str]:
    generator_path = framework_root / "nxbootstrap" / "tools" / \
        "generate-port.py"
    spec = importlib.util.spec_from_file_location(
        "nx_refresh_pins_bootstrap", generator_path
    )
    if spec is None or spec.loader is None:
        raise SystemExit("cannot load canonical nxbootstrap generator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    architecture = nxport.get("architecture")
    roles = nxport.get("execution_roles")
    if isinstance(roles, dict) and isinstance(roles.get("splash"), dict):
        architecture = roles["splash"].get("architecture")
    try:
        _path, digest = module.nxsplash_artifact(architecture)
    except (OSError, ValueError, module.ManifestError) as error:
        raise SystemExit(f"cannot resolve canonical nxsplash: {error}")
    return "nxsplash-nextos", digest


def refresh_generation_runtime(project_path: Path, port_dir: Path,
                               framework_root: Path) -> tuple[str, list[str]]:
    """Re-derive schema-3 runtime and authored package hashes.

    This updates only the declarative source manifest.  nxgenerator remains
    the sole authority that creates launcher/live/store/GENERATION bytes in a
    fresh package-shaped candidate.
    """
    try:
        project = json.loads(project_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise SystemExit(f"cannot read nxproject.json: {error}")
    nxport = project.get("nxport")
    if not isinstance(nxport, dict) or nxport.get("schema_version") != 3:
        return project_path.read_text(encoding="utf-8"), []
    runtime = nxport.get("generation_runtime")
    if not isinstance(runtime, list) or not runtime:
        raise SystemExit("nxport schema 3 requires generation_runtime")
    runtime_root_value = _safe_project_relative(
        project.get("runtime_root"), "nxproject.runtime_root", allow_dot=True
    )
    runtime_root = _directory_beneath(
        port_dir, runtime_root_value, "nxproject.runtime_root"
    )

    changes = []
    splash_path, splash_sha256 = _canonical_nxsplash(framework_root, nxport)
    seen = set()
    for index, member in enumerate(runtime):
        context = f"nxproject generation_runtime[{index}]"
        if not isinstance(member, dict) or set(member) != {
                "role", "path", "mode", "sha256"}:
            raise SystemExit(f"{context} is malformed")
        role = member.get("role")
        path = _safe_project_relative(member.get("path"), context + ".path")
        if path in seen:
            raise SystemExit("nxproject generation_runtime has duplicate paths")
        seen.add(path)
        mode = member.get("mode")
        if mode not in ("0644", "0755"):
            raise SystemExit(f"{context}.mode is invalid")
        if role == "nxsplash":
            if path != splash_path or mode != "0755":
                raise SystemExit("nxproject nxsplash record is not canonical")
            digest = splash_sha256
        else:
            source = _regular_beneath(runtime_root, path, context)
            metadata = source.lstat()
            actual_mode = "%04o" % stat.S_IMODE(metadata.st_mode)
            if actual_mode != mode:
                raise SystemExit(
                    f"{context} source mode is {actual_mode}, expected {mode}"
                )
            digest = sha256_of(source)
        old = member.get("sha256")
        if old != digest:
            if not isinstance(old, str):
                old = "<invalid>"
            changes.append(
                f"generation runtime {path}: {old[:12]} -> {digest[:12]}"
            )
            member["sha256"] = digest

    package_payload = project.get("package_payload", [])
    if not isinstance(package_payload, list) or len(package_payload) > 128:
        raise SystemExit("nxproject package_payload is malformed")
    package_seen = set()
    previous_package_path = None
    package_total_size = 0
    for index, member in enumerate(package_payload):
        context = f"nxproject package_payload[{index}]"
        if not isinstance(member, dict) or set(member) != {
                "path", "mode", "sha256", "kind"}:
            raise SystemExit(f"{context} is malformed")
        path = _safe_project_relative(member.get("path"), context + ".path")
        folded = path.casefold()
        if folded in package_seen:
            raise SystemExit("nxproject package_payload has duplicate paths")
        if previous_package_path is not None and path <= previous_package_path:
            raise SystemExit("nxproject package_payload is not ordered by path")
        package_seen.add(folded)
        previous_package_path = path
        mode = member.get("mode")
        if mode not in ("0644", "0755"):
            raise SystemExit(f"{context}.mode is invalid")
        if mode == "0755" and PurePosixPath(path).parts[0] != "tools":
            raise SystemExit(
                f"{context}.mode 0755 is allowed only below tools/"
            )
        if member.get("kind") not in ("payload", "license-notice"):
            raise SystemExit(f"{context}.kind is invalid")
        source = _regular_beneath(port_dir, path, context)
        metadata = source.lstat()
        if metadata.st_size > PACKAGE_PAYLOAD_FILE_LIMIT:
            raise SystemExit(f"{context} exceeds the 4 MiB file limit")
        package_total_size += metadata.st_size
        if package_total_size > PACKAGE_PAYLOAD_TOTAL_LIMIT:
            raise SystemExit(
                "nxproject package_payload exceeds the 16 MiB total limit"
            )
        actual_mode = "%04o" % stat.S_IMODE(metadata.st_mode)
        if actual_mode != mode:
            raise SystemExit(
                f"{context} source mode is {actual_mode}, expected {mode}"
            )
        digest = sha256_of(source)
        old = member.get("sha256")
        if old != digest:
            if not isinstance(old, str):
                old = "<invalid>"
            changes.append(
                f"package payload {path}: {old[:12]} -> {digest[:12]}"
            )
            member["sha256"] = digest
    rendered = (
        json.dumps(project, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
        if changes else project_path.read_text(encoding="utf-8")
    )
    return rendered, changes


def regenerate_launcher(port_dir: Path, framework_root: Path,
                        manifest: dict) -> Path | None:
    """Rebuild the launcher from the canonical template.

    The launcher is generated output. Editing the copy in the port is what the
    release gate is there to catch, so regenerate rather than patch.
    """
    contract = manifest.get("package", {}).get("launcher_contract", {})
    config_path = contract.get("config_path")
    launcher_name = manifest.get("package", {}).get("launcher")
    if not config_path or not launcher_name:
        return None

    generator = framework_root / "nxbootstrap" / "tools" / "generate-port.py"
    if not generator.is_file():
        raise SystemExit(f"generator not found: {generator}")

    out_dir = port_dir / ".build" / "launcher-regen"
    if out_dir.exists():
        subprocess.run(["rm", "-rf", str(out_dir)], check=True)
    config_source = manifest_source_for(manifest, config_path)
    subprocess.run(
        [sys.executable, "-B", str(generator), str(port_dir / config_source),
         "--output", str(out_dir), "--force"],
        check=True, stdout=subprocess.DEVNULL,
    )
    generated = out_dir / launcher_name
    if not generated.is_file():
        raise SystemExit(f"generator produced no {launcher_name}")
    return generated


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port-dir", required=True)
    parser.add_argument("--framework-root")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    port_input = Path(args.port_dir)
    if port_input.is_symlink() or not port_input.is_dir():
        raise SystemExit(f"port directory is missing or unsafe: {port_input}")
    port_dir = port_input.resolve()

    framework_root = (Path(args.framework_root).resolve()
                      if args.framework_root
                      else port_dir.parent / "nextos_ports_android" / "framework")
    if not framework_root.is_dir():
        raise SystemExit(f"framework root not found: {framework_root}")

    changes: list[str] = []
    project_path = port_dir / "nxproject.json"
    project_schema = None
    if project_path.is_symlink():
        raise SystemExit("nxproject.json cannot be a symlink")
    if project_path.is_file():
        try:
            project_document = json.loads(
                project_path.read_text(encoding="utf-8")
            )
            project_schema = project_document.get("nxport", {}).get(
                "schema_version"
            )
        except (UnicodeError, json.JSONDecodeError, AttributeError) as error:
            raise SystemExit(f"cannot determine nxproject schema: {error}")
    if project_schema == 3:
        original_project = project_path.read_text(encoding="utf-8")
        refreshed_project, runtime_changes = refresh_generation_runtime(
            project_path, port_dir, framework_root
        )
        changes.extend(runtime_changes)
        if refreshed_project != original_project and not args.check:
            project_path.write_text(refreshed_project, encoding="utf-8")

        if not changes:
            print("nx-refresh-pins: every schema-3 runtime/package pin already matches its file")
            return 0
        for change in changes:
            print(f"nx-refresh-pins: {change}")
        if args.check:
            print("nx-refresh-pins: schema-3 runtime/package pins are stale")
            return 1
        print(f"nx-refresh-pins: refreshed {len(changes)} schema-3 pin(s)")
        return 0

    manifest_path = port_dir / "nxrelease.json"
    if manifest_path.is_symlink() or not manifest_path.is_file():
        raise SystemExit(f"no safe nxrelease.json under {port_dir}")
    raw = manifest_path.read_text(encoding="utf-8")
    manifest = json.loads(raw)

    # The launcher first: regenerating it changes its hash, so it has to happen
    # before any hash is computed. Schema 3 is different: nxgenerator creates
    # launcher + complete immutable store together in a fresh candidate, so a
    # launcher-only regeneration would manufacture a hybrid installation.
    generated = regenerate_launcher(port_dir, framework_root, manifest)
    launcher_name = manifest.get("package", {}).get("launcher")
    if generated is not None:
        installed = port_dir / launcher_name
        if not installed.is_file() or \
                sha256_of(installed) != sha256_of(generated):
            changes.append(f"launcher {launcher_name} regenerated from template")
            if not args.check:
                installed.write_bytes(generated.read_bytes())
                installed.chmod(0o755)

    # Versions come from the component that owns them, never from memory.
    bootstrap_version = read_version(framework_root / "nxbootstrap" / "VERSION")
    contract = manifest.get("package", {}).get("launcher_contract", {})
    if contract.get("version") and contract["version"] != bootstrap_version:
        changes.append(
            f"launcher contract {contract['version']} -> {bootstrap_version}")
        raw = raw.replace(f'"version": "{contract["version"]}"',
                          f'"version": "{bootstrap_version}"', 1)

    # Replace hashes in the raw text rather than re-serializing the manifest:
    # a reformatted file would be an unreviewable diff, and a sha256 is unique
    # enough to substitute safely.
    source_root = port_dir / manifest.get("source_root", ".")

    def repin(rel_source: str, old: str, label: str) -> None:
        nonlocal raw
        target = (source_root / rel_source).resolve()
        if not target.is_file():
            changes.append(f"MISSING {label}: {rel_source}")
            return
        new = sha256_of(target)
        if new != old:
            changes.append(f"{label} {rel_source}: {old[:12]} -> {new[:12]}")
            raw = raw.replace(old, new)

    for entry in manifest.get("files", []):
        if entry.get("sha256") and entry.get("source"):
            repin(entry["source"], entry["sha256"], "file")

    if contract.get("config_sha256") and contract.get("config_path"):
        repin(manifest_source_for(manifest, contract["config_path"]),
              contract["config_sha256"], "config")

    # The nxextract entry deliberately stays untouched: that payload is injected
    # from the framework at package time and is not a file in the port tree, so
    # nxrelease owns its pin.

    # The port's build script pins the release tool it was verified against.
    build_script = port_dir / "package" / "build-package.sh"
    if build_script.is_file():
        text = build_script.read_text(encoding="utf-8")
        tool = framework_root / "nxrelease" / "nxrelease.py"
        tool_version = read_version(framework_root / "nxrelease" / "VERSION")
        tool_sha = sha256_of(tool)
        # Ports spell the same pin differently -- quoted with the tool name, or
        # bare -- so handle both rather than making one port the odd one out.
        updated = re.sub(r"NXRELEASE_VERSION='nxrelease [0-9.]+'",
                         f"NXRELEASE_VERSION='nxrelease {tool_version}'", text)
        updated = re.sub(r"^NXRELEASE_VERSION=[0-9.]+$",
                         f"NXRELEASE_VERSION={tool_version}", updated,
                         flags=re.MULTILINE)
        updated = re.sub(r"NXRELEASE_SHA256=[0-9a-f]{64}",
                         f"NXRELEASE_SHA256={tool_sha}", updated)

        # Other framework components a port may pin the same way.
        for name, component in (("NXBOOTSTRAP", "nxbootstrap"),
                                ("NXGENERATOR", "nxgenerator"),
                                ("NXOBS", "nxobs"),
                                ("NXGL", "nxgl")):
            version_file = framework_root / component / "VERSION"
            if not version_file.is_file():
                continue
            version = read_version(version_file)
            updated = re.sub(rf"^{name}_VERSION=[0-9.]+$",
                             f"{name}_VERSION={version}", updated,
                             flags=re.MULTILINE)
            updated = re.sub(rf"{name}_VERSION='{component} [0-9.]+'",
                             f"{name}_VERSION='{component} {version}'", updated)
        # A port that pins a component's own script by named constant
        #   NXGENERATOR="$FRAMEWORK_ROOT/nxgenerator/nxgenerator.py"
        #   NXGENERATOR_SHA256=<64hex>
        # is as derivable as the nxrelease pin above, and leaving it out is how
        # a bump stops the port's build with "SHA-256 drifted" on a file the
        # tool could have re-derived.
        constant_paths = dict(re.findall(
            r'^([A-Z_]+)="?(\$[A-Z_]+/[^"\s]+)"?$', updated, flags=re.MULTILINE))
        for name, shell_path in constant_paths.items():
            if not re.search(rf"^{name}_SHA256=[0-9a-f]{{64}}$", updated,
                             flags=re.MULTILINE):
                continue
            target = shell_path.replace("$FRAMEWORK_ROOT", str(framework_root))
            if "$" in target:
                continue
            candidate = Path(target)
            if not candidate.is_file() or candidate.is_symlink():
                continue
            updated = re.sub(rf"^{name}_SHA256=[0-9a-f]{{64}}$",
                             f"{name}_SHA256={sha256_of(candidate)}", updated,
                             flags=re.MULTILINE)

        # Some ports pin inline instead of through named constants:
        #   [[ $(<"$NXBOOTSTRAP_ROOT/VERSION") == 0.6.15 ]]
        #   require_pinned_file "$NXBOOTSTRAP_ROOT/tools/x.py" <sha> 'Label'
        # Both are derivable, so resolve the shell roots and recompute.
        # Collect every shell variable assigned from another variable plus a
        # path, quoted or not, and resolve them transitively from FRAMEWORK_ROOT
        # (and NXEXTRACT_ROOT when the port names the canonical extractor tree).
        assignments = dict(re.findall(
            r'^([A-Z_]+)="?(\$[A-Z_]+/[^"\s]+)"?$', updated, flags=re.MULTILINE))
        known = {"FRAMEWORK_ROOT": framework_root}
        nxextract_root = framework_root.parent / "suportando_outros_devices" / \
            "extrator-universal"
        if nxextract_root.is_dir():
            known["NXEXTRACT_ROOT"] = nxextract_root

        def resolve(shell_path: str) -> Path | None:
            m = re.match(r'\$([A-Z_]+)/(.*)$', shell_path)
            if not m:
                m2 = re.match(r'\$([A-Z_]+)$', shell_path)
                if not m2:
                    return None
                var, rest = m2.group(1), ""
            else:
                var, rest = m.group(1), m.group(2)
            if var in known:
                return known[var] / rest if rest else known[var]
            if var in assignments:
                base = resolve(assignments[var])
                if base is None:
                    return None
                return base / rest if rest else base
            return None

        def fix_inline_version(match: re.Match) -> str:
            resolved = resolve(match.group(1))
            if resolved is None or not resolved.is_file():
                return match.group(0)
            return match.group(0).replace(match.group(2),
                                          read_version(resolved))

        updated = re.sub(
            r'\$\(<"([^"]+/VERSION)"\)\s*==\s*([0-9][0-9.]*)',
            fix_inline_version, updated)

        def fix_pinned_file(match: re.Match) -> str:
            resolved = resolve(match.group(1))
            if resolved is None or not resolved.is_file():
                return match.group(0)
            return match.group(0).replace(match.group(2), sha256_of(resolved))

        updated = re.sub(
            r'require_pinned_file\s*\\?\s*\n?\s*"([^"]+)"\s*\\?\s*\n?\s*([0-9a-f]{64})',
            fix_pinned_file, updated)

        if updated != text:
            changes.append(f"build-package.sh pinned to nxrelease {tool_version}")
            if not args.check:
                build_script.write_text(updated, encoding="utf-8")

    if raw != manifest_path.read_text(encoding="utf-8") and not args.check:
        manifest_path.write_text(raw, encoding="utf-8")

    # A build script may pin the manifest's own hash, and the manifest was
    # just rewritten above. Re-derive that one pin now that the manifest is
    # final, so a single run converges instead of needing two.
    if build_script.is_file():
        text2 = build_script.read_text(encoding="utf-8")
        known2 = {"MANIFEST": manifest_path, "PORT_DIR": port_dir}
        assignments2 = dict(re.findall(
            r'^([A-Z_]+)="?(\$[A-Z_]+/[^"\s]+)"?$', text2, flags=re.MULTILINE))

        def resolve2(shell_path: str) -> Path | None:
            m = re.match(r'\$([A-Z_]+)(?:/(.*))?$', shell_path)
            if not m:
                return None
            var, rest = m.group(1), m.group(2) or ""
            if var in known2:
                return known2[var] / rest if rest else known2[var]
            if var in assignments2:
                base = resolve2(assignments2[var])
                return (base / rest if rest else base) if base else None
            return None

        def fix_manifest_pin(match: re.Match) -> str:
            resolved = resolve2(match.group(1))
            if resolved is None or not resolved.is_file():
                return match.group(0)
            return match.group(0).replace(match.group(2), sha256_of(resolved))

        text3 = re.sub(
            r'require_pinned_file\s*\\?\s*\n?\s*"([^"]+)"\s*\\?\s*\n?\s*([0-9a-f]{64})',
            fix_manifest_pin, text2)
        # One port seals a derived view of the manifest with an inline digest
        # (`expected = "<sha>"` right after computing it from the manifest with
        # a fixed substitution). Recompute that seal the same way the script
        # does, so the port's own convention keeps working without a hand edit.
        seal = re.search(
            r'needle = \'([^\']+)\'\s*\n.*?sealed = source\.replace\(needle, \'([^\']+)\'\)'
            r'.*?expected = "([0-9a-f]{64})"',
            text3, flags=re.S)
        if seal:
            needle, replacement, old_seal = seal.group(1), seal.group(2), seal.group(3)
            manifest_text = manifest_path.read_text(encoding="utf-8")
            if manifest_text.count(needle) == 1:
                new_seal = hashlib.sha256(
                    manifest_text.replace(needle, replacement).encode("utf-8")
                ).hexdigest()
                if new_seal != old_seal:
                    text3 = text3.replace(f'expected = "{old_seal}"',
                                          f'expected = "{new_seal}"', 1)
                    changes.append("build-package.sh manifest seal re-derived")

        if text3 != text2:
            changes.append("build-package.sh re-pinned to the refreshed manifest")
            if not args.check:
                build_script.write_text(text3, encoding="utf-8")

    if not changes:
        print("nx-refresh-pins: every pin already matches its file")
        return 0

    for change in changes:
        print(f"nx-refresh-pins: {change}")
    if args.check:
        print("nx-refresh-pins: pins are stale")
        return 1
    print(f"nx-refresh-pins: refreshed {len(changes)} pin(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
