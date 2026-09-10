#!/usr/bin/env python3
"""Render a port's nxrelease.json from nxproject.json and the files on disk.

nxgenerator produces the launcher, port.json, docs and the NXSplash helper from
nxproject.json, but not the release manifest -- so every port grew its own
render script, or worse, a hand-typed nxrelease.json that drifted the first time
a hash changed. Three ports had three different renderers on the same day.

This is the one renderer. Every value is derived: hashes from the files, the
loader's DT_NEEDED from readelf, component versions from the framework tree,
the port id and launcher from nxproject.json. Nothing is typed.

Usage:
  nx-render-manifest.py --port-dir DIR [--framework-root DIR]
                        [--source-url URL] [--source-date-epoch N]
                        [--max-glibc 2.30] [--public-final] [--check]

Layout it expects in the port directory (the nxgenerator layout):
  <Launcher>.sh, nxport.json, nxproject.json, port.json, gameinfo.xml,
  <executable>, nxsplash-nextos, LICENSE, INSTALLATION.md, README.md,
  optional NOTICE.md, extractor.json + nxextract/ when NXExtract is enabled,
  optional gamedata/README.txt, optional cover.png.
"""

import argparse
import hashlib
import json
import re
import stat
import subprocess
import sys
from pathlib import Path, PurePosixPath


def sha256_of(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def is_elf(path: Path) -> bool:
    with path.open("rb") as stream:
        return stream.read(4) == b"\x7fELF"


def read_version(root: Path, component: str) -> str:
    return (root / component / "VERSION").read_text(encoding="utf-8").strip()


def elf_needed(path: Path) -> list[str]:
    result = subprocess.run(["readelf", "-dW", str(path)], text=True,
                            capture_output=True, check=False)
    if result.returncode != 0:
        raise SystemExit(f"readelf failed for {path}: {result.stderr.strip()}")
    needed = []
    for line in result.stdout.splitlines():
        if "(NEEDED)" in line and "[" in line and "]" in line:
            needed.append(line[line.index("[") + 1:line.index("]")])
    return sorted(needed)


def elf_soname(path: Path) -> str | None:
    result = subprocess.run(["readelf", "-dW", str(path)], text=True,
                            capture_output=True, check=False)
    for line in result.stdout.splitlines():
        if "(SONAME)" in line and "[" in line and "]" in line:
            return line[line.index("[") + 1:line.index("]")]
    return None


def elf_defined_symbols(path: Path) -> set[str]:
    result = subprocess.run(["readelf", "--syms", "--wide", str(path)],
                            text=True, capture_output=True, check=False)
    if result.returncode != 0:
        raise SystemExit(f"readelf symbols failed for {path}: "
                         f"{result.stderr.strip()}")
    symbols = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) < 8 or fields[6] in ("UND", "Name"):
            continue
        name = fields[7].split("@", 1)[0]
        if name and name != "Name":
            symbols.add(name)
    return symbols


def provider_for(soname: str) -> str:
    """Who supplies a shared object on the device.

    glibc's own objects come with the C library; SDL and OpenAL are what
    PortMaster guarantees; everything else is the firmware's problem.
    """
    if re.match(r"^(libc|libm|libdl|libpthread|librt|libresolv|libutil)\.so", soname):
        return "glibc-base"
    if soname.startswith("ld-linux"):
        return "firmware"
    if re.match(r"^(libSDL2|libSDL2_|libopenal)", soname):
        return "portmaster"
    return "firmware"


GENERATION_V2_BOOTSTRAP_VERSION = "0.8.4"
GENERATION_V2_CONTROL_FILES = (
    "commit",
    "components.sha256",
    "components.v2",
    "format",
    "identity-runtime.v2",
    "identity.json",
    "manifest.json",
)
GENERATION_V2_RUNTIME_ROLES = (
    "executable",
    "private-library",
    "runtime-data",
    "runtime-hook",
    "nxextract-recipe",
    "nxextract-engine",
    "nxextract-runner",
    "nxextract-runtime-env",
    "nxextract-ui",
    "nxextract-helper",
    "nxextract-spec",
    "nxsplash",
)
GENERATION_V2_RUNTIME_ROLE_ORDER = {
    role: index for index, role in enumerate(GENERATION_V2_RUNTIME_ROLES)
}
GENERATION_V2_JSON_LIMIT = 1024 * 1024
GENERATION_V2_CONTROL_LIMIT = 256 * 1024
GENERATION_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
GENERATION_VERSION_RE = re.compile(r"^[0-9]{1,4}\.[0-9]{1,4}\.[0-9]{1,4}$")
PACKAGE_PAYLOAD_KINDS = ("payload", "license-notice")
PACKAGE_PAYLOAD_FILE_LIMIT = 4 * 1024 * 1024
PACKAGE_PAYLOAD_TOTAL_LIMIT = 16 * 1024 * 1024


def _safe_generation_path(value: object, context: str) -> str:
    if (not isinstance(value, str) or not value or len(value) > 512 or
            "\\" in value or any(
                ord(character) < 0x20 or 0x7f <= ord(character) <= 0x9f
                for character in value)):
        raise SystemExit(f"{context} is not a safe relative path")
    path = PurePosixPath(value)
    if (path.is_absolute() or path.as_posix() != value or
            any(part in ("", ".", "..") for part in path.parts)):
        raise SystemExit(f"{context} is not a safe relative path")
    return value


def _generation_regular_bytes(path: Path, context: str, expected_mode: str,
                              limit: int | None = None) -> bytes:
    try:
        metadata = path.lstat()
    except OSError as error:
        raise SystemExit(f"{context} is missing or unreadable: {error}")
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
        raise SystemExit(f"{context} is not a private regular file")
    actual_mode = stat.S_IMODE(metadata.st_mode)
    if actual_mode != int(expected_mode, 8):
        raise SystemExit(
            f"{context} mode is {actual_mode:04o}, expected {expected_mode}"
        )
    if limit is not None and metadata.st_size > limit:
        raise SystemExit(f"{context} exceeds its size limit")
    try:
        return path.read_bytes()
    except OSError as error:
        raise SystemExit(f"{context} cannot be read: {error}")


def _strict_generation_json(payload: bytes, context: str) -> object:
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON member {key!r}")
            result[key] = value
        return result

    def finite_constant(value):
        raise ValueError(f"non-finite JSON number {value}")

    try:
        text = payload.decode("utf-8", "strict")
        return json.loads(
            text,
            object_pairs_hook=unique_object,
            parse_constant=finite_constant,
        )
    except (UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise SystemExit(f"invalid {context}: {error}")


def _canonical_generation_json(value: object, ensure_ascii: bool) -> bytes:
    return (json.dumps(
        value, indent=2, sort_keys=True, ensure_ascii=ensure_ascii
    ) + "\n").encode("utf-8")


def _generation_component(value: object, context: str) -> dict[str, str]:
    if not isinstance(value, dict) or set(value) != {
            "role", "path", "mode", "sha256"}:
        raise SystemExit(f"{context} is malformed")
    role = value.get("role")
    if not isinstance(role, str):
        raise SystemExit(f"{context}.role is malformed")
    path = _safe_generation_path(value.get("path"), context + ".path")
    mode = value.get("mode")
    digest = value.get("sha256")
    if (mode not in ("0644", "0755") or not isinstance(digest, str) or
            not GENERATION_SHA256_RE.fullmatch(digest)):
        raise SystemExit(f"{context} mode/hash is malformed")
    return {"role": role, "path": path, "mode": mode, "sha256": digest}


def _package_payload_component(value: object, context: str) -> dict[str, str]:
    if not isinstance(value, dict) or set(value) != {
            "path", "mode", "sha256", "kind"}:
        raise SystemExit(f"{context} is malformed")
    path = _safe_generation_path(value.get("path"), context + ".path")
    mode = value.get("mode")
    digest = value.get("sha256")
    kind = value.get("kind")
    if (mode not in ("0644", "0755") or
            not isinstance(digest, str) or
            not GENERATION_SHA256_RE.fullmatch(digest) or
            kind not in PACKAGE_PAYLOAD_KINDS):
        raise SystemExit(f"{context} kind/mode/hash is malformed")
    if mode == "0755" and PurePosixPath(path).parts[0] != "tools":
        raise SystemExit(f"{context} mode 0755 is allowed only below tools/")
    return {"path": path, "mode": mode, "sha256": digest, "kind": kind}


def _validate_generation_receipt_closure(port: Path, files: list[dict],
                                         port_id: str) -> None:
    """Bind renderer inputs to the immutable pre-overlay generator receipt."""
    receipt_path = port / "GENERATION.json"
    payload = _generation_regular_bytes(
        receipt_path, "GENERATION.json", "0644", GENERATION_V2_JSON_LIMIT
    )
    receipt = _strict_generation_json(payload, "GENERATION.json")
    if not isinstance(receipt, dict):
        raise SystemExit("nx-render-manifest: GENERATION.json is malformed")
    project_digest = _generation_sha256((port / "nxproject.json").read_bytes())
    if receipt.get("project_manifest_sha256") != project_digest:
        raise SystemExit(
            "nx-render-manifest: GENERATION.json project_manifest_sha256 "
            "differs from nxproject.json"
        )
    raw_artifacts = receipt.get("artifacts")
    if not isinstance(raw_artifacts, list):
        raise SystemExit(
            "nx-render-manifest: GENERATION.json artifacts is malformed"
        )
    actual = []
    previous = None
    for index, raw in enumerate(raw_artifacts):
        context = f"GENERATION.json artifacts[{index}]"
        if not isinstance(raw, dict) or set(raw) != {
                "path", "mode", "sha256"}:
            raise SystemExit(f"nx-render-manifest: {context} is malformed")
        path = _safe_generation_path(raw.get("path"), context + ".path")
        mode = raw.get("mode")
        digest = raw.get("sha256")
        if (mode not in ("0644", "0755") or
                not isinstance(digest, str) or
                not GENERATION_SHA256_RE.fullmatch(digest)):
            raise SystemExit(f"nx-render-manifest: {context} is malformed")
        if previous is not None and path <= previous:
            raise SystemExit(
                "nx-render-manifest: GENERATION.json artifacts are not "
                "strictly ordered"
            )
        actual.append({"path": path, "mode": mode, "sha256": digest})
        previous = path
    generation_target = f"{port_id}/GENERATION.json"
    expected = sorted((
        {"path": record["target"], "mode": record["mode"],
         "sha256": record["sha256"]}
        for record in files if record["target"] != generation_target
    ), key=lambda record: record["path"])
    if actual != expected:
        raise SystemExit(
            "nx-render-manifest: GENERATION.json artifact closure is stale"
        )


def _generation_sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def generation_store_modes(generation_root: Path,
                           framework_root: Path | None = None) -> dict[str, str]:
    """Return exact package modes for one committed generation.

    Generation v1 carried only control files, so its historical layout keeps
    the launcher-directory convention. V2 carries executable bytes and must
    derive every mode from its authenticated component list; guessing from a
    dirname silently turned the rollback ELF into 0644.
    """
    format_path = generation_root / "format"
    if not format_path.exists() and not format_path.is_symlink():
        modes = {}
        for source in sorted(generation_root.rglob("*")):
            if source.is_symlink():
                raise SystemExit(f"generation store contains symlink: {source}")
            if source.is_file():
                relative = source.relative_to(generation_root).as_posix()
                modes[relative] = (
                    "0755" if source.parent.name == "launcher" else "0644"
                )
        return modes
    generation_id = generation_root.name
    if not GENERATION_SHA256_RE.fullmatch(generation_id):
        raise SystemExit("generation-v2 directory name is not a SHA-256")

    controls = {
        name: _generation_regular_bytes(
            generation_root / name,
            f"generation-v2 control {name}",
            "0644",
            GENERATION_V2_JSON_LIMIT if name.endswith(".json") else
            GENERATION_V2_CONTROL_LIMIT,
        )
        for name in GENERATION_V2_CONTROL_FILES
    }
    if controls["format"] != b"nxruntime-generation-v2\n":
        raise SystemExit("generation store format marker is malformed")

    manifest = _strict_generation_json(
        controls["manifest.json"], "generation-v2 manifest"
    )
    if (not isinstance(manifest, dict) or
            set(manifest) != {"schema", "schema_version", "generation_id",
                             "identity_basis", "components"} or
            manifest.get("schema") != "nxruntime-generation-v2" or
            manifest.get("schema_version") != 2 or
            manifest.get("generation_id") != generation_id):
        raise SystemExit("generation-v2 manifest identity is malformed")
    if controls["manifest.json"] != _canonical_generation_json(
            manifest, ensure_ascii=True):
        raise SystemExit("generation-v2 manifest is not canonical JSON")
    components = manifest.get("components")
    # Closure size is structural, never a game-specific/fixed member ceiling.
    # JSON/control byte limits above bound resource use independently.
    if not isinstance(components, list) or len(components) < 4:
        raise SystemExit("generation-v2 components are incomplete")

    modes = {name: "0644" for name in GENERATION_V2_CONTROL_FILES}
    normalized_components = []
    component_lines = []
    sha_lines = []
    seen = set()
    component_payloads = []
    runtime_previous_key = None
    executable_count = 0
    nxsplash_count = 0
    for index, component in enumerate(components):
        context = f"generation-v2 components[{index}]"
        record = _generation_component(component, context)
        normalized_components.append(record)
        role = record["role"]
        path = record["path"]
        mode = record["mode"]
        digest = record["sha256"]
        if index == 0:
            if role != "launcher" or mode != "0755" or "/" in path:
                raise SystemExit(
                    "generation-v2 first component is not the canonical launcher"
                )
            internal = "files/launcher/" + path
            checksum_path = "launcher/" + path
        elif index == 1:
            if role != "nxport" or path != "nxport.json" or mode != "0644":
                raise SystemExit(
                    "generation-v2 second component is not canonical nxport"
                )
            internal = "files/nxport.json"
            checksum_path = "nxport.json"
        elif role in GENERATION_V2_RUNTIME_ROLE_ORDER:
            order_key = (GENERATION_V2_RUNTIME_ROLE_ORDER[role], path)
            if runtime_previous_key is not None and \
                    order_key <= runtime_previous_key:
                raise SystemExit(
                    "generation-v2 runtime records are not in canonical order"
                )
            runtime_previous_key = order_key
            if role == "executable":
                executable_count += 1
                if mode != "0755":
                    raise SystemExit(
                        "generation-v2 executable mode must be 0755"
                    )
            elif role == "nxsplash":
                nxsplash_count += 1
                if path != "nxsplash-nextos" or mode != "0755":
                    raise SystemExit(
                        "generation-v2 nxsplash identity is not canonical"
                    )
            internal = "files/runtime/" + path
            checksum_path = "runtime/" + path
        else:
            raise SystemExit(f"{context}.role is unsupported")
        if internal in seen:
            raise SystemExit("generation-v2 components contain duplicate paths")
        seen.add(internal)
        source = generation_root / PurePosixPath(internal)
        payload = _generation_regular_bytes(source, context + ".file", mode)
        if _generation_sha256(payload) != digest:
            raise SystemExit(f"generation-v2 component bytes differ: {internal}")
        modes[internal] = mode
        component_payloads.append(payload)
        component_lines.append(f"{role}\t{mode}\t{digest}\t{path}\n")
        sha_lines.append((checksum_path, f"{digest}  {checksum_path}\n"))

    if executable_count != 1 or nxsplash_count != 1 or \
            normalized_components[-1]["role"] != "nxsplash":
        raise SystemExit(
            "generation-v2 requires one executable and one final nxsplash"
        )

    nxport = _strict_generation_json(
        component_payloads[1], "generation-v2 nxport"
    )
    if not isinstance(nxport, dict) or nxport.get("schema_version") != 3:
        raise SystemExit("generation-v2 nxport is not schema 3")
    if component_payloads[1] != _canonical_generation_json(
            nxport, ensure_ascii=False):
        raise SystemExit("generation-v2 nxport is not canonical JSON")
    if nxport.get("launcher_name") != normalized_components[0]["path"]:
        raise SystemExit("generation-v2 nxport launcher is not bound")
    if nxport.get("executable") != next(
            record["path"] for record in normalized_components[2:]
            if record["role"] == "executable"):
        raise SystemExit("generation-v2 nxport executable is not bound")
    if nxport.get("generation_runtime") != normalized_components[2:]:
        raise SystemExit("generation-v2 nxport runtime closure is not bound")

    identity = _strict_generation_json(
        controls["identity.json"], "generation-v2 identity.json"
    )
    if controls["identity.json"] != _canonical_generation_json(
            identity, ensure_ascii=False):
        raise SystemExit("generation-v2 identity.json is not canonical JSON")
    if identity != manifest.get("identity_basis"):
        raise SystemExit(
            "generation-v2 manifest and identity.json disagree"
        )
    if _generation_sha256(controls["identity.json"]) != generation_id:
        raise SystemExit(
            "generation-v2 identity.json is not bound to its directory"
        )
    identity_keys = {
        "schema", "schema_version", "nxport_sha256",
        "launcher_preimage_sha256", "runtime_records_sha256",
        "nxbootstrap", "components", "nxsplash",
    }
    if (not isinstance(identity, dict) or set(identity) != identity_keys or
            identity.get("schema") !=
            "org.nextos.nxruntime.generation-identity" or
            identity.get("schema_version") != 2):
        raise SystemExit("generation-v2 identity basis is malformed")

    identity_components_raw = identity.get("components")
    if not isinstance(identity_components_raw, list) or \
            len(identity_components_raw) != len(normalized_components):
        raise SystemExit("generation-v2 identity components are malformed")
    identity_components = [
        _generation_component(value, f"generation-v2 identity components[{index}]")
        for index, value in enumerate(identity_components_raw)
    ]
    launcher_identity = identity_components[0]
    launcher_component = normalized_components[0]
    launcher_preimage_sha256 = identity.get("launcher_preimage_sha256")
    if (not isinstance(launcher_preimage_sha256, str) or
            not GENERATION_SHA256_RE.fullmatch(launcher_preimage_sha256) or
            launcher_identity != {
                "role": "launcher",
                "path": launcher_component["path"],
                "mode": launcher_component["mode"],
                "sha256": launcher_preimage_sha256,
            }):
        raise SystemExit("generation-v2 launcher preimage identity is malformed")
    if identity_components[1:] != normalized_components[1:]:
        raise SystemExit(
            "generation-v2 identity components differ from the manifest"
        )
    if identity.get("nxport_sha256") != normalized_components[1]["sha256"]:
        raise SystemExit("generation-v2 nxport identity hash is stale")

    identity_runtime = "".join(component_lines[2:]).encode("utf-8")
    if controls["identity-runtime.v2"] != identity_runtime:
        raise SystemExit("generation-v2 identity-runtime.v2 is stale")
    if identity.get("runtime_records_sha256") != _generation_sha256(
            identity_runtime):
        raise SystemExit("generation-v2 runtime records hash is stale")

    bootstrap = identity.get("nxbootstrap")
    if (not isinstance(bootstrap, dict) or set(bootstrap) != {
            "version", "generator_sha256", "launcher_template_sha256"} or
            bootstrap.get("version") != GENERATION_V2_BOOTSTRAP_VERSION or
            any(not isinstance(bootstrap.get(key), str) or
                not GENERATION_SHA256_RE.fullmatch(bootstrap[key])
                for key in ("generator_sha256", "launcher_template_sha256"))):
        raise SystemExit("generation-v2 nxbootstrap identity is malformed")
    nxsplash = identity.get("nxsplash")
    if (not isinstance(nxsplash, dict) or set(nxsplash) != {
            "version", "architecture", "sha256"} or
            not isinstance(nxsplash.get("version"), str) or
            not GENERATION_VERSION_RE.fullmatch(nxsplash["version"]) or
            nxsplash.get("architecture") not in (
                "aarch64", "armv7", "x86_64", "i386") or
            nxsplash.get("sha256") != normalized_components[-1]["sha256"]):
        raise SystemExit("generation-v2 nxsplash identity is malformed")
    if framework_root is not None:
        framework_root = Path(framework_root).resolve()
        bootstrap_root = framework_root / "nxbootstrap"
        generator_path = bootstrap_root / "tools" / "generate-port.py"
        launcher_template = bootstrap_root / "templates" / "launcher.sh.in"
        version_path = bootstrap_root / "VERSION"
        for path, label in (
                (generator_path, "generator"),
                (launcher_template, "launcher template"),
                (version_path, "VERSION")):
            if path.is_symlink() or not path.is_file():
                raise SystemExit(
                    f"generation-v2 canonical nxbootstrap {label} is unsafe"
                )
        if (version_path.read_text(encoding="utf-8").strip() !=
                GENERATION_V2_BOOTSTRAP_VERSION or
                bootstrap["generator_sha256"] != sha256_of(generator_path) or
                bootstrap["launcher_template_sha256"] !=
                sha256_of(launcher_template)):
            raise SystemExit(
                "generation-v2 nxbootstrap source identity is stale"
            )
        splash_path = (
            framework_root / "nxsplash" / "release" /
            nxsplash["architecture"] / "nxsplash-nextos"
        )
        splash_version = framework_root / "nxsplash" / "VERSION"
        if (splash_path.is_symlink() or not splash_path.is_file() or
                splash_version.is_symlink() or not splash_version.is_file() or
                splash_version.read_text(encoding="utf-8").strip() !=
                nxsplash["version"] or
                sha256_of(splash_path) != nxsplash["sha256"]):
            raise SystemExit("generation-v2 nxsplash source identity is stale")

    generation_id_bytes = generation_id.encode("ascii")
    launcher_payload = component_payloads[0]
    if launcher_payload.count(generation_id_bytes) != 1:
        raise SystemExit(
            "generation-v2 launcher does not contain exactly one generation id"
        )
    launcher_preimage = launcher_payload.replace(
        generation_id_bytes, b"0" * 64, 1
    )
    if _generation_sha256(launcher_preimage) != launcher_preimage_sha256:
        raise SystemExit("generation-v2 launcher preimage hash is stale")

    actual_files = set()
    for source in generation_root.rglob("*"):
        if source.is_symlink():
            raise SystemExit(f"generation store contains symlink: {source}")
        if source.is_file():
            actual_files.add(source.relative_to(generation_root).as_posix())
        elif not source.is_dir():
            raise SystemExit(f"generation store contains non-regular entry: {source}")
    if actual_files != set(modes):
        raise SystemExit(
            "generation-v2 store is not closed: missing=%s extra=%s" % (
                sorted(set(modes) - actual_files),
                sorted(actual_files - set(modes)),
            )
        )
    if controls["commit"] != (generation_id + "\n").encode("ascii"):
        raise SystemExit("generation-v2 commit marker is stale")
    if controls["components.v2"] != \
            "".join(component_lines).encode("utf-8"):
        raise SystemExit("generation-v2 components.v2 is stale")
    if controls["components.sha256"] != \
            "".join(line for _path, line in sorted(sha_lines)).encode("utf-8"):
        raise SystemExit("generation-v2 components.sha256 is stale")
    return modes


CONTROLS_RUNTIME_MARKER = b"nxinput-gptk-runtime/3"
CONTROLS_RUNTIME_EVIDENCE = b"nxinput-gptk-event-evidence/1"
CONTROLS_RUNTIME_CONTRACT = {
    "schema": "nxinput-gptk-live/1",
    "context_initial": "unproven",
    "unproven_policy": "native-passthrough",
    "sink_coverage": "all-actions-before-activation",
    "delivery_ack": "required",
}
CONTROLS_RUNTIME_SYMBOLS = {
    "nxinput_gptk_load_at",
    "nxinput_gptk_load_receipt_json",
    "nxinput_gptk_parse",
    "nxinput_gptk_decide",
    "nxinput_gptk_live_init",
    "nxinput_gptk_live_register",
    "nxinput_gptk_live_seal",
    "nxinput_gptk_live_set_context",
    "nxinput_gptk_live_clear_context",
    "nxinput_gptk_live_should_consume",
    "nxinput_gptk_live_feed",
    "nxinput_gptk_runtime_marker",
    "nxinput_gptk_event_evidence_schema",
}
GPTK_EDITABLE_CLAIMS = ("This file is YOURS", "Este arquivo \u00e9 SEU")


def _validate_controls_runtime(project, port):
    """TEARSCAPE-CONTROLS-LIVE gate: an editable NEXTOSCONTROLLERS.gptk is a
    RUNTIME claim, never documentation.

    A port that declares controls.runtime_mapping = "nxinput-gptk" must ship
    an executable that really links the nxinput GPTK loader/dispatcher (the
    binary carries the version marker) and a parseable default mapping.  A
    port that does NOT declare the mode may not ship a mapping file that
    promises the player it is editable -- that promise was proven false in
    the field (Tearscape 0.2.x).  Approved published ZIPs are untouched;
    this boundary applies to each NEW candidate only.
    """
    controls = project.get("controls")
    declared = (
        isinstance(controls, dict) and
        controls.get("runtime_mapping") == "nxinput-gptk"
    )
    gptk_path = port / "defaults" / "NEXTOSCONTROLLERS.gptk"
    gptk_text = ""
    if gptk_path.is_file() and not gptk_path.is_symlink():
        gptk_text = gptk_path.read_text(encoding="utf-8", errors="replace")
    if not declared:
        if any(claim in gptk_text for claim in GPTK_EDITABLE_CLAIMS):
            raise SystemExit(
                "nx-render-manifest: NEXTOSCONTROLLERS.gptk claims to be "
                "editable but the port does not declare "
                "controls.runtime_mapping = nxinput-gptk; either integrate "
                "the live runtime or stop shipping the editable claim"
            )
        return
    executable_name = project.get("nxport", {}).get("executable")
    if not isinstance(executable_name, str) or not executable_name:
        raise SystemExit(
            "nx-render-manifest: runtime_mapping declared without an "
            "executable"
        )
    executable = port / executable_name
    if executable.is_symlink() or not executable.is_file():
        raise SystemExit(
            "nx-render-manifest: runtime_mapping declared but the packaged "
            "executable is absent"
        )
    executable_bytes = executable.read_bytes()
    if (CONTROLS_RUNTIME_MARKER not in executable_bytes or
            CONTROLS_RUNTIME_EVIDENCE not in executable_bytes):
        raise SystemExit(
            "nx-render-manifest: controls.runtime_mapping = nxinput-gptk "
            "declared but the packaged executable does not link the nxinput "
            "GPTK live boundary/evidence schema"
        )
    symbols = elf_defined_symbols(executable)
    required_symbols = set(CONTROLS_RUNTIME_SYMBOLS)
    actions = controls.get("actions")
    contexts = controls.get("contexts")
    if not isinstance(actions, list) or not isinstance(contexts, dict):
        raise SystemExit(
            "nx-render-manifest: live controls actions/contexts are malformed"
        )
    if any(isinstance(action, dict) and action.get("kind") == "vector"
           for action in actions):
        required_symbols.update(("nxinput_gptk_live_register_vector",
                                 "nxinput_gptk_live_feed_vector"))
    missing_symbols = sorted(required_symbols - symbols)
    if missing_symbols:
        raise SystemExit(
            "nx-render-manifest: GPTK live executable lacks defined "
            "boundary symbol(s): " + ", ".join(missing_symbols)
        )
    adapter_path = port / "adapter" / "adapter-contract.json"
    if adapter_path.is_symlink() or not adapter_path.is_file():
        raise SystemExit(
            "nx-render-manifest: live GPTK requires a regular promoted "
            "adapter-contract.json"
        )
    try:
        adapter = json.loads(adapter_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, ValueError) as error:
        raise SystemExit(
            "nx-render-manifest: live GPTK requires a readable promoted "
            f"adapter-contract.json: {error}"
        )
    if not isinstance(adapter, dict):
        raise SystemExit(
            "nx-render-manifest: live GPTK adapter contract must be an object"
        )
    adapter_input = adapter.get("input")
    if (adapter.get("status") != "implemented_release" or
            adapter.get("release_ready") is not True or
            not isinstance(adapter_input, dict)):
        raise SystemExit(
            "nx-render-manifest: live GPTK requires a release-ready promoted "
            "adapter contract"
        )
    if (adapter_input.get("actions") != actions or
            adapter_input.get("contexts") != contexts or
            adapter_input.get("runtime_mapping") != "nxinput-gptk" or
            adapter_input.get("runtime_contract") !=
            CONTROLS_RUNTIME_CONTRACT):
        raise SystemExit(
            "nx-render-manifest: adapter live-input contract differs from "
            "nxproject or relaxes the fail-safe policy"
        )
    if "format = NEXTOS_CONTROLLERS/" not in gptk_text:
        raise SystemExit(
            "nx-render-manifest: runtime_mapping declared but the default "
            "NEXTOSCONTROLLERS.gptk is absent or lacks its magic"
        )


def main() -> int:
    ap = argparse.ArgumentParser()
    source = ap.add_mutually_exclusive_group(required=True)
    source.add_argument(
        "--port-dir",
        help="legacy flat source tree containing launcher and port payload",
    )
    source.add_argument(
        "--generator-root",
        help=("package-shaped, no-overwrite nxgenerator output containing "
              "<Launcher>.sh plus <port-id>/"),
    )
    ap.add_argument("--framework-root")
    ap.add_argument("--source-url", default="")
    ap.add_argument("--source-date-epoch", type=int, default=1786492800)
    ap.add_argument("--max-glibc", default="2.30")
    ap.add_argument(
        "--public-final", action="store_true",
        help=("include the generated root GENERATION.json required by new "
              "nxrelease public-final artifacts"),
    )
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    generator_root = None
    if args.generator_root:
        candidate = Path(args.generator_root)
        if candidate.is_symlink() or not candidate.is_dir():
            raise SystemExit("nx-render-manifest: generator root is unsafe")
        generator_root = candidate.resolve()
        projects = [
            path for path in generator_root.glob("*/nxproject.json")
            if path.is_file() and not path.is_symlink() and
            path.parent.parent == generator_root
        ]
        if len(projects) != 1:
            raise SystemExit(
                "nx-render-manifest: generator root must contain exactly one "
                "<port-id>/nxproject.json"
            )
        port = projects[0].parent
    else:
        candidate = Path(args.port_dir)
        if candidate.is_symlink() or not candidate.is_dir():
            raise SystemExit("nx-render-manifest: port directory is unsafe")
        port = candidate.resolve()
    fw = (Path(args.framework_root).resolve() if args.framework_root
          else ((Path(__file__).resolve().parents[1])
                if generator_root is not None else
                port.parent / "nextos_ports_android" / "framework"))
    if not (fw / "nxbootstrap" / "VERSION").is_file():
        raise SystemExit(f"framework root not found: {fw}")

    project = json.loads((port / "nxproject.json").read_text(encoding="utf-8"))
    nxport = project["nxport"]
    _validate_controls_runtime(project, port)
    packaged_nxport = _strict_generation_json(
        (port / "nxport.json").read_bytes(), "packaged nxport.json"
    )
    if not isinstance(packaged_nxport, dict):
        raise SystemExit("nx-render-manifest: packaged nxport.json is not an object")
    pid = nxport["id"]
    package_payload_raw = project.get("package_payload", [])
    if not isinstance(package_payload_raw, list) or \
            len(package_payload_raw) > 128:
        raise SystemExit(
            "nx-render-manifest: package_payload must be an array of at most "
            "128 records"
        )
    package_payload = []
    payload_paths = set()
    previous_payload_path = None
    for index, value in enumerate(package_payload_raw):
        record = _package_payload_component(
            value, f"nxproject package_payload[{index}]"
        )
        path = record["path"]
        folded = path.casefold()
        if folded in payload_paths:
            raise SystemExit(
                "nx-render-manifest: package_payload has a duplicate path"
            )
        if previous_payload_path is not None and path <= previous_payload_path:
            raise SystemExit(
                "nx-render-manifest: package_payload is not ordered by path"
            )
        payload_paths.add(folded)
        previous_payload_path = path
        package_payload.append(record)
    if generator_root is not None and port.name != pid:
        raise SystemExit(
            "nx-render-manifest: generator directory differs from nxport.id"
        )
    launcher = nxport["launcher_name"]
    executable = nxport["executable"]
    arch = nxport.get("architecture", "aarch64")
    version_file = port / "version.txt"
    version = (version_file.read_text(encoding="utf-8").strip()
               if version_file.is_file() else project.get("version", "0.0.0"))
    nxextract_on = nxport.get("nxextract", {}).get("mode") in ("yes", "auto")
    if generator_root is not None:
        launcher_path = generator_root / PurePosixPath(launcher)
        if (launcher_path.is_symlink() or not launcher_path.is_file() or
                "/" in launcher):
            raise SystemExit(
                "nx-render-manifest: generated launcher is missing or unsafe"
            )
        allowed_root = {launcher, pid, "nxrelease.json"}
        actual_root = {path.name for path in generator_root.iterdir()}
        if not actual_root <= allowed_root or not {launcher, pid} <= actual_root:
            raise SystemExit(
                "nx-render-manifest: generator root contains unexpected entries"
            )
    # nxgenerator completes the runtime closure in the materialized nxport
    # (notably the canonical NXSplash).  That packaged document is therefore
    # the authority for live/store matching; nxproject remains the authorial
    # input and is independently bound by GENERATION.json.
    generation_runtime = packaged_nxport.get("generation_runtime")
    generation_runtime_by_path = {}
    if generation_runtime is not None:
        if not isinstance(generation_runtime, list) or not generation_runtime:
            raise SystemExit(
                "nx-render-manifest: generation_runtime is malformed"
            )
        for index, member in enumerate(generation_runtime):
            record = _generation_component(
                member, f"nxport generation_runtime[{index}]"
            )
            if record["path"] in generation_runtime_by_path:
                raise SystemExit(
                    "nx-render-manifest: generation_runtime has a duplicate "
                    "path: " + record["path"]
                )
            live = port / PurePosixPath(record["path"])
            payload = _generation_regular_bytes(
                live, f"live generation_runtime[{index}]",
                record["mode"],
            )
            if _generation_sha256(payload) != record["sha256"]:
                raise SystemExit(
                    "nx-render-manifest: live generation_runtime bytes differ: "
                    + record["path"]
                )
            generation_runtime_by_path[record["path"]] = record
    # The NXExtract version comes from the port's own pin file first: a port
    # built from a materialized framework snapshot has no monorepo beside it,
    # and the file it ships is what the runtime will honour anyway.
    pinned = port / "nxextract-version.txt"
    canonical = fw.parent / "suportando_outros_devices" / "extrator-universal" / "VERSION"
    if pinned.is_file():
        nxextract_version = pinned.read_text(encoding="utf-8").strip()
    elif canonical.is_file():
        nxextract_version = canonical.read_text(encoding="utf-8").strip()
    else:
        nxextract_version = nxport.get("nxextract", {}).get("version", "")

    def entry(source: str, target: str, kind: str, mode: str = "0644", **extra):
        if generator_root is not None and source == launcher:
            p = generator_root / PurePosixPath(source)
            logical_source = source
        else:
            p = port / PurePosixPath(source)
            logical_source = (
                PurePosixPath(pid, source).as_posix()
                if generator_root is not None else source
            )
        if p.is_symlink() or not p.is_file():
            raise SystemExit(f"manifest source missing: {source}")
        e = {"source": logical_source, "target": target, "kind": kind,
             "mode": mode,
             "sha256": sha256_of(p)}
        e.update(extra)
        return e

    def linux(source: str, target: str, kind: str, provenance: str,
              build_profile: str = "universal-low-glibc",
              mode: str = "0755"):
        p = ((generator_root / PurePosixPath(source))
             if generator_root is not None and source == launcher else
             port / PurePosixPath(source))
        return entry(source, target, kind, mode, architecture=arch,
                     build_profile=build_profile, provenance=provenance,
                     needed=elf_needed(p), soname=elf_soname(p))

    def generic_runtime_entry(record: dict[str, str], target: str):
        """Classify only the one generic role allowed to carry a Linux ELF.

        Schema 3 deliberately lets a port name arbitrary helper/spec paths,
        so suffixes cannot be trusted.  An executable NXExtract helper built
        by the port is a project Linux artifact and must enter the ordinary
        ABI/GLIBC/reproducibility audit.  Every other generic runtime role is
        data: accepting an ELF there would recreate the payload bypass this
        classification closes.
        """
        path = record["path"]
        role = record["role"]
        mode = record["mode"]
        source = port / PurePosixPath(path)
        source_is_elf = is_elf(source)
        if source_is_elf:
            if role != "nxextract-helper":
                raise SystemExit(
                    "nx-render-manifest: generation role {} cannot carry an "
                    "ELF; executable generic runtime members must be declared "
                    "as nxextract-helper".format(role)
                )
            if mode != "0755":
                raise SystemExit(
                    "nx-render-manifest: ELF nxextract-helper mode must be 0755"
                )
            return linux(
                path, target, "project-linux",
                "port-built NXExtract helper declared by generation_runtime",
                mode=mode,
            )
        return entry(path, target, "payload", mode=mode)

    package_payload_by_path = {
        record["path"]: record for record in package_payload
    }

    def authored_entry(source: str, target: str, default_kind: str,
                       default_mode: str = "0644"):
        """Use an explicit author declaration before historical inference."""
        declared = package_payload_by_path.get(source)
        if declared is None:
            return entry(source, target, default_kind, default_mode)
        return entry(
            source, target, declared["kind"], declared["mode"]
        )

    project_provenance = "port build script; framework pinned by FRAMEWORK-PIN.json"
    declared_payload_paths = {record["path"] for record in package_payload}
    if {"FRAMEWORK-PIN.json", "tools/BUILD-INPUTS.json"} <= \
            declared_payload_paths:
        project_provenance += "; build inputs pinned by tools/BUILD-INPUTS.json"

    files = [
        entry(launcher, launcher, "launcher", "0755"),
        entry("nxport.json", f"{pid}/nxport.json", "nxbootstrap-config"),
        entry("nxproject.json", f"{pid}/nxproject.json", "payload"),
        linux(executable, f"{pid}/{executable}", "project-linux",
              project_provenance),
        authored_entry("README.md", f"{pid}/README.md", "payload"),
        authored_entry(
            "INSTALLATION.md", f"{pid}/INSTALLATION.md", "payload"
        ),
        entry("LICENSE", f"{pid}/LICENSE", "license-notice"),
        entry("port.json", f"{pid}/port.json", "portmaster-metadata"),
        entry("gameinfo.xml", f"{pid}/gameinfo.xml", "portmaster-metadata"),
        linux("nxsplash-nextos", f"{pid}/nxsplash-nextos", "nxsplash-linux",
              f"NXSplash {read_version(fw, 'nxsplash')} canonical {arch} artifact"),
    ]
    # public-final is an explicit opt-in. Existing/dev manifests remain byte
    # compatible, while every newly promoted artifact carries its generation
    # identity inside the ZIP instead of depending on an out-of-band file.
    if args.public_final or generator_root is not None:
        generation = port / "GENERATION.json"
        if generation.is_symlink() or not generation.is_file():
            raise SystemExit(
                "nx-render-manifest: --public-final requires a regular "
                "GENERATION.json in the port directory"
            )
        files.append(entry("GENERATION.json", f"{pid}/GENERATION.json", "payload"))
    for optional, kind in (("NOTICE.md", "license-notice"),
                           ("sensitivity.txt", "payload"),
                           ("version.txt", "payload"),
                           ("FRAMEWORK-PIN.json", "payload"),
                           ("gamecontrollerdb.txt", "payload"),
                           ("alsoft.conf", "payload"),
                           ("port-env.sh", "payload"),
                           ("adapter-env.sh", "payload"),
                           ("OWNERSHIP.json", "payload"),
                           # V5 (M1c 1.2): the expected host closure the
                           # generator derives from the project.
                           ("CONTROLS-CLOSURE.json", "payload"),
                           ("config.json", "payload"),
                           ("prepare.sh", "payload")):
        if (port / optional).is_file():
            files.append(authored_entry(optional, f"{pid}/{optional}", kind))
    # gmloader-style ports carry a few extra payload trees next to the loader:
    # engine config defaults, a declarative adapter contract, and a build tool
    # the extraction recipe runs. Bundle them when present; other ports have
    # none of these and are unaffected.
    for extra_dir in ("defaults", "adapter", "tools"):
        d = port / extra_dir
        if d.is_dir():
            for f in sorted(d.rglob("*")):
                if f.is_file() and not f.is_symlink() and \
                        "__pycache__" not in f.parts and f.suffix != ".pyc":
                    rel = f.relative_to(port).as_posix()
                    mode = "0755" if f.suffix in (".sh", ".py") else "0644"
                    # V5 owner runtime: the hook SEED is data (never executed
                    # from defaults/), materialized 0644 by the launcher.
                    if rel == "defaults/port-env.sh":
                        mode = "0644"
                    files.append(authored_entry(
                        rel, f"{pid}/{rel}", "payload", mode
                    ))
    # V3-UPDATE-01: a generation-aware launcher only reaches pending->active
    # on the device if the committed generation store ships WITH the port.
    # Without these files the runtime falls back to legacy mode forever and
    # the health/rollback machinery never engages. Dot-path on purpose.
    gen_store = port / ".nxruntime" / "generations"
    generation_stores = []
    if gen_store.is_dir():
        for generation_root in sorted(gen_store.iterdir()):
            if generation_root.is_symlink() or not generation_root.is_dir():
                raise SystemExit(
                    f"generation store contains invalid root: {generation_root}"
                )
            generation_stores.append((
                generation_root,
                generation_store_modes(generation_root, fw),
            ))
    # V4-REPACK-01: the visible runtime seed. It is the AUTHORITY the launcher
    # rebuilds `.nxruntime` from when a personal rezip drops every dotdir, so
    # it has to be packaged like any other sealed member -- a seed that does
    # not ship turns the whole front off silently.
    for generation_root, _store_modes in generation_stores:
        seed_name = "nxruntime-%s.nxb" % generation_root.name
        seed = port / seed_name
        if seed.is_symlink() or not seed.is_file():
            raise SystemExit(
                "nx-render-manifest: the visible runtime seed is missing or "
                "not a regular file: " + seed_name
            )
        files.append(entry(seed_name, f"{pid}/{seed_name}",
                           "nxruntime-seed", mode="0644"))

    # GAMEDATA-DIR-01: with NXExtract active the owner-data marker is
    # mandatory — it is the only physical guarantee that gamedata/ exists
    # after installation. Missing, symlinked, non-regular or empty fails the
    # render; without NXExtract the renderer never promises gamedata/.
    gamedata_marker = port / "gamedata" / "README.txt"
    if nxextract_on:
        if gamedata_marker.is_symlink() or not gamedata_marker.is_file():
            raise SystemExit(
                "nx-render-manifest: NXExtract is active but "
                "gamedata/README.txt is missing or not a regular file"
            )
        if gamedata_marker.stat().st_size == 0:
            raise SystemExit(
                "nx-render-manifest: gamedata/README.txt must not be empty"
            )
        files.append(entry("gamedata/README.txt", f"{pid}/gamedata/README.txt",
                           "payload"))
    elif gamedata_marker.is_file():
        files.append(entry("gamedata/README.txt", f"{pid}/gamedata/README.txt",
                           "payload"))
    # Bundled third-party shared objects next to the loader, and in the
    # conventional lib/ directory (SDL3-style bundles: the CFW never provides
    # the soname, the port ships it and LD_LIBRARY_PATH points at lib/).
    for so in sorted(port.glob("*.so*")) + sorted(port.glob("lib/*.so*")):
        if so.is_file() and not so.is_symlink():
            rel = so.relative_to(port).as_posix()
            runtime_record = generation_runtime_by_path.get(rel)
            # Schema-3 private libraries already passed the regular-file,
            # mode and SHA-256 checks above.  Preserve that authenticated mode
            # during the first conventional lib/ classification instead of
            # replacing 0644 with linux()'s historical 0755 default.  The
            # later live/store equality remains exact and fail-closed.
            mode = (
                runtime_record["mode"]
                if runtime_record is not None and
                runtime_record["role"] == "private-library"
                else "0755"
            )
            files.append(linux(rel, f"{pid}/{rel}", "third-party-linux",
                               "bundled runtime dependency", mode=mode))
    for lic in sorted((port / "licenses").glob("*")) if (port / "licenses").is_dir() else []:
        if lic.is_file():
            rel = f"licenses/{lic.name}"
            files.append(authored_entry(
                rel, f"{pid}/{rel}", "license-notice"
            ))

    nxextract_block = None
    if nxextract_on:
        files += [
            entry("extractor.json", f"{pid}/extractor.json", "nxextract-recipe"),
            entry("nxextract/nxextract.py", f"{pid}/nxextract/nxextract.py", "nxextract"),
            entry("nxextract/run-extractor.sh", f"{pid}/nxextract/run-extractor.sh",
                  "nxextract-runner"),
            entry("nxextract/nxextract-runtime-env.sh",
                  f"{pid}/nxextract/nxextract-runtime-env.sh", "nxextract-runtime-env"),
            linux("nxextract/nxextract-ui", f"{pid}/nxextract/nxextract-ui",
                  "nxextract-ui-linux",
                  f"NXExtract UI {nxextract_version} canonical {arch} artifact"),
        ]
        # Fontes de hook/spec do proprio port (a receita as invoca por
        # {game_dir}/nxextract/<path>). No schema 3, a lista autenticada da
        # geracao e a unica autoridade e inclui caminhos aninhados; nunca
        # redescobrir apenas o primeiro nivel nem aprender nomes de um jogo.
        canonical_nxextract = {
            "nxextract.py", "run-extractor.sh",
            "nxextract-runtime-env.sh", "nxextract-ui",
        }
        declared_extras = None
        if isinstance(generation_runtime, list):
            declared_extras = {}
            for index, member in enumerate(generation_runtime):
                record = _generation_component(
                    member, f"nxport generation_runtime[{index}]"
                )
                if record["role"] in ("nxextract-helper", "nxextract-spec"):
                    declared_extras[record["path"]] = record
        if declared_extras is not None:
            for declared_path in sorted(declared_extras):
                extra = port / PurePosixPath(declared_path)
                if extra.is_symlink() or not extra.is_file():
                    raise SystemExit(
                        "nx-render-manifest: declared NXExtract member is "
                        "missing or unsafe: " + declared_path
                    )
                rel = extra.relative_to(port).as_posix()
                files.append(generic_runtime_entry(
                    declared_extras[rel], f"{pid}/{rel}"
                ))
        else:
            for extra in sorted((port / "nxextract").rglob("*")):
                if (extra.is_file() and not extra.is_symlink() and
                        extra.name not in canonical_nxextract and
                        extra.suffix != ".pyc" and
                        "__pycache__" not in extra.parts):
                    rel = extra.relative_to(port).as_posix()
                    mode = (
                        "0755" if extra.suffix in (".sh", ".py") else "0644"
                    )
                    files.append(entry(
                        rel, f"{pid}/{rel}", "payload", mode=mode
                    ))
        if (port / "nxextract-version.txt").is_file():
            files.append(entry("nxextract-version.txt", f"{pid}/nxextract-version.txt",
                               "payload"))
        nxextract_block = {
            "path": f"{pid}/nxextract/nxextract.py",
            "version": nxextract_version, "minimum_version": nxextract_version,
            "sha256": sha256_of(port / "nxextract/nxextract.py"),
            "runner_path": f"{pid}/nxextract/run-extractor.sh",
            "runner_sha256": sha256_of(port / "nxextract/run-extractor.sh"),
            "runtime_env_path": f"{pid}/nxextract/nxextract-runtime-env.sh",
            "runtime_env_sha256": sha256_of(port / "nxextract/nxextract-runtime-env.sh"),
            "ui_path": f"{pid}/nxextract/nxextract-ui",
            "ui_sha256": sha256_of(port / "nxextract/nxextract-ui"),
            "recipe_path": f"{pid}/extractor.json",
            "recipe_sha256": sha256_of(port / "extractor.json"),
        }

    # Every declared schema-3 live member must enter the release inventory,
    # including nested/private paths that do not match a historical renderer
    # convention.  Existing specialized records keep their stronger kind;
    # missing generic hooks/helpers/specs are added from the declaration.
    if isinstance(generation_runtime, list):
        by_target = {}
        for file_record in files:
            target = file_record["target"]
            if target in by_target:
                raise SystemExit(
                    "nx-render-manifest: duplicate release target " + target
                )
            by_target[target] = file_record
        generic_roles = {
            "runtime-data", "runtime-hook", "nxextract-helper",
            "nxextract-spec",
        }
        for index, member in enumerate(generation_runtime):
            record = _generation_component(
                member, f"nxport generation_runtime[{index}]"
            )
            path = record["path"]
            target = f"{pid}/{path}"
            if target not in by_target:
                if record["role"] == "private-library":
                    added = linux(
                        path, target, "third-party-linux",
                        "declared private generation library",
                        mode=record["mode"],
                    )
                elif record["role"] in generic_roles:
                    added = generic_runtime_entry(record, target)
                else:
                    raise SystemExit(
                        "nx-render-manifest: specialized generation member "
                        "was not packaged: " + path
                    )
                files.append(added)
                by_target[target] = added
            packaged = by_target[target]
            if (packaged.get("mode") != record["mode"] or
                    packaged.get("sha256") != record["sha256"]):
                raise SystemExit(
                    "nx-render-manifest: release member differs from "
                    "generation_runtime: " + path
                )

    # The immutable store mirrors the live generation.  Non-ELF controls and
    # payloads remain ordinary generation data.  Every ELF mirror receives a
    # distinct Linux class and the *exact* metadata of its authenticated live
    # counterpart; the store is never allowed to invent an ABI/provenance.
    if generation_stores:
        live_by_target = {record["target"]: record for record in files}
        runtime_by_path = {}
        for index, member in enumerate(generation_runtime or []):
            component = _generation_component(
                member, f"nxport generation_runtime[{index}]"
            )
            runtime_by_path[component["path"]] = component
        elf_metadata_fields = (
            "architecture", "build_profile", "provenance", "needed", "soname",
        )
        for generation_root, store_modes in generation_stores:
            for generation_relative, mode in sorted(store_modes.items()):
                source = generation_root / PurePosixPath(generation_relative)
                rel = source.relative_to(port).as_posix()
                target = f"{pid}/{rel}"
                if not is_elf(source):
                    files.append(entry(
                        rel, target, "nxruntime-generation", mode=mode
                    ))
                    continue
                runtime_prefix = "files/runtime/"
                if not generation_relative.startswith(runtime_prefix):
                    raise SystemExit(
                        "nx-render-manifest: generation store ELF has no "
                        "authenticated live runtime path: " + generation_relative
                    )
                logical_path = generation_relative[len(runtime_prefix):]
                component = runtime_by_path.get(logical_path)
                live = live_by_target.get(f"{pid}/{logical_path}")
                if component is None or live is None:
                    raise SystemExit(
                        "nx-render-manifest: generation store ELF has no live "
                        "counterpart: " + logical_path
                    )
                if (component["path"] != logical_path or
                        component["mode"] != mode or
                        component["sha256"] != sha256_of(source) or
                        live.get("mode") != mode or
                        live.get("sha256") != component["sha256"]):
                    raise SystemExit(
                        "nx-render-manifest: generation store ELF differs from "
                        "its live path/mode/SHA-256: " + logical_path
                    )
                if any(field not in live for field in elf_metadata_fields):
                    raise SystemExit(
                        "nx-render-manifest: live generation ELF lacks complete "
                        "metadata: " + logical_path
                    )
                metadata = {field: live[field] for field in elf_metadata_fields}
                files.append(entry(
                    rel, target, "nxruntime-generation-linux", mode=mode,
                    **metadata,
                ))

    # Schema-3 authored package files are declared inputs, never an overlay.
    # nxgenerator has already materialized them before writing GENERATION.json;
    # independently bind every declaration to the package-shaped tree and to
    # the release inventory.  Conventional files (README, version, NOTICE,
    # tools, licenses) may already have stronger renderer entries; unknown but
    # declared non-runtime paths are added without rediscovery.
    if package_payload:
        by_target = {}
        for file_record in files:
            target = file_record["target"]
            if target in by_target:
                raise SystemExit(
                    "nx-render-manifest: duplicate release target " + target
                )
            by_target[target] = file_record
        package_payload_size = 0
        for index, record in enumerate(package_payload):
            path = record["path"]
            source = port / PurePosixPath(path)
            payload = _generation_regular_bytes(
                source, f"package_payload[{index}] file", record["mode"],
                PACKAGE_PAYLOAD_FILE_LIMIT,
            )
            package_payload_size += len(payload)
            if package_payload_size > PACKAGE_PAYLOAD_TOTAL_LIMIT:
                raise SystemExit(
                    "nx-render-manifest: package_payload exceeds the 16 MiB "
                    "total limit"
                )
            digest = _generation_sha256(payload)
            if digest != record["sha256"]:
                raise SystemExit(
                    "nx-render-manifest: package_payload bytes differ: " + path
                )
            target = f"{pid}/{path}"
            packaged = by_target.get(target)
            if packaged is None:
                packaged = entry(
                    path, target, record["kind"], mode=record["mode"]
                )
                files.append(packaged)
                by_target[target] = packaged
            if (packaged.get("kind") != record["kind"] or
                    packaged.get("mode") != record["mode"] or
                    packaged.get("sha256") != record["sha256"]):
                raise SystemExit(
                    "nx-render-manifest: release member differs from "
                    "package_payload: " + path
                )

    images = []
    if (port / "cover.png").is_file():
        files.append(entry("cover.png", f"{pid}/cover.png", "portmaster-image"))
        images.append({"path": f"{pid}/cover.png", "role": "cover",
                       "sha256": sha256_of(port / "cover.png")})

    # The closed author-composition contract exists only for the package root
    # emitted by nxgenerator. Legacy direct-port rendering remains a migration
    # input; nxrelease independently validates any receipt it later stages.
    if generator_root is not None:
        _validate_generation_receipt_closure(port, files, pid)

    needed_all = sorted({n for f in files if f.get("needed") for n in f["needed"]})
    bundled_sonames = {
        f.get("soname"): f["target"]
        for f in files
        if f.get("kind") == "third-party-linux" and f.get("soname")
    }
    manifest = {
        "schema_version": 2,
        "source_root": ".",
        "package": {
            "id": pid, "version": version, "profile": "universal-portmaster",
            "launcher": launcher, "launcher_chain": [launcher],
            "launcher_contract": {
                "generator": "nxbootstrap",
                "version": read_version(fw, "nxbootstrap"),
                "config_path": f"{pid}/nxport.json",
                "config_sha256": sha256_of(port / "nxport.json"),
            },
            "port_dir": pid,
            "license": {"spdx_id": project.get("license", {}).get("spdx_id", "GPL-3.0-only"),
                        "source_url": args.source_url,
                        "file": f"{pid}/LICENSE"},
        },
        "release": {"source_date_epoch": args.source_date_epoch,
                    "max_glibc": args.max_glibc, "compression": "deflated"},
        "dependencies": [
            # A soname the port itself bundles (third-party-linux with the
            # matching soname) is provider=package at the bundled path; the
            # generic table only answers for what the port does NOT carry.
            ({"namespace": "linux", "architecture": arch, "soname": s,
              "provider": "package", "path": bundled_sonames[s]}
             if s in bundled_sonames else
             {"namespace": "linux", "architecture": arch, "soname": s,
              "provider": provider_for(s)})
            for s in needed_all],
        "portmaster_metadata": {
            "port_json": {"path": f"{pid}/port.json", "sha256": sha256_of(port / "port.json")},
            "gameinfo_xml": {"path": f"{pid}/gameinfo.xml", "sha256": sha256_of(port / "gameinfo.xml")},
            "images": images,
        },
        "files": files,
        "exceptions": [],
    }
    if nxextract_block:
        manifest["nxextract"] = nxextract_block

    rendered = json.dumps(manifest, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
    out = ((generator_root / "nxrelease.json")
           if generator_root is not None else port / "nxrelease.json")
    if args.check:
        if out.is_file() and out.read_text(encoding="utf-8") == rendered:
            print("nx-render-manifest: nxrelease.json is current")
            return 0
        print("nx-render-manifest: nxrelease.json is stale")
        return 1
    out.write_text(rendered, encoding="utf-8")
    print(f"nx-render-manifest: wrote {out} ({len(files)} files, "
          f"{len(needed_all)} dependencies)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
