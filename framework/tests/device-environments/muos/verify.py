#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Verifica o ambiente muOS preparado a partir da imagem oficial.

Confere, sem inicializar vídeo e sem tocar em DRM, Mali, framebuffer, áudio ou
input:

- proveniência: recibo, contrato e SHA-256 da imagem de origem;
- as duas raízes reais do aparelho, AArch64 e ARMHF;
- **isolamento**: nenhum ELF de uma ABI dentro da raiz da outra;
- o interpretador Python que a imagem entrega (versão medida, não suposta);
- os provedores de renderer presentes em cada ABI;
- quando há clang e qemu, a lista de drivers de vídeo que **a própria SDL da
  imagem** compilou, para as duas ABIs.
"""

import argparse
import hashlib
import importlib.util
import json
import os
import posixpath
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parents[3]
CONTRACT = HERE / "contract-v1.json"
PROBE = REPOSITORY / "framework/tests/probes/sdl-drivers.c"
ARCH_ELF = {"aarch64": (2, 183), "armv7": (1, 40)}
CLANG_TARGET = {
    "aarch64": ["--target=aarch64-linux-gnu"],
    "armv7": ["--target=armv7a-linux-gnueabihf", "-march=armv7-a",
              "-mfloat-abi=hard"],
}
QEMU = {"aarch64": "qemu-aarch64-static", "armv7": "qemu-arm-static"}
PYTHON_VERSION_RE = re.compile(r"^3\.\d+$")
PYTHON_OUTPUT_RE = re.compile(r"^Python (\d+\.\d+\.\d+)$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
TRUSTED_TOOL_PATH = os.defpath

_PREPARER_SPEC = importlib.util.spec_from_file_location(
    "muos_prepare_for_verify", HERE / "prepare.py")
require_preparer = (_PREPARER_SPEC is not None and
                    _PREPARER_SPEC.loader is not None)
if not require_preparer:
    raise RuntimeError("cannot load the canonical muOS ELF parser")
PREPARER = importlib.util.module_from_spec(_PREPARER_SPEC)
_PREPARER_SPEC.loader.exec_module(PREPARER)


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def load_json(path):
    def no_duplicates(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate JSON key: %s" % key)
            result[key] = value
        return result

    with Path(path).open("r", encoding="utf-8") as stream:
        return json.load(stream, object_pairs_hook=no_duplicates)


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 22), b""):
            digest.update(chunk)
    return digest.hexdigest()


def clean_subprocess_env():
    """Keep host probes independent of inherited loader/display/QEMU state."""
    return {
        "PATH": TRUSTED_TOOL_PATH,
        "LANG": "C",
        "LC_ALL": "C",
    }


def resolve_tool(name, required=True):
    """Resolve once and return the executable's canonical absolute path."""
    found = shutil.which(name, path=TRUSTED_TOOL_PATH)
    if found is None:
        if required:
            raise GateError("required tool is missing: %s" % name)
        return None
    try:
        resolved = Path(found).resolve(strict=True)
    except OSError as error:
        raise GateError("cannot resolve %s: %s" % (name, error))
    require(resolved.is_file() and os.access(resolved, os.X_OK),
            "tool is not executable: %s" % resolved)
    return str(resolved)


def outside_repository(path, label):
    resolved = Path(path).resolve(strict=True)
    try:
        resolved.relative_to(REPOSITORY.resolve())
    except ValueError:
        return resolved
    raise GateError("%s must stay outside the repository: %s" %
                    (label, resolved))


def safe_relative(root, logical, *, must_exist=True):
    require(isinstance(logical, str) and logical.startswith("/") and
            not logical.startswith("//") and "\x00" not in logical and
            "\n" not in logical and "\r" not in logical,
            "unsafe prepared path: %r" % logical)
    require(posixpath.normpath(logical) == logical and
            ".." not in PurePosixPath(logical).parts,
            "non-canonical prepared path: %r" % logical)
    candidate = root.joinpath(*PurePosixPath(logical).parts[1:])
    resolved_root = root.resolve(strict=True)
    try:
        resolved = candidate.resolve(strict=must_exist)
    except (OSError, RuntimeError) as error:
        raise GateError("cannot resolve prepared path %s: %s" %
                        (logical, error))
    try:
        resolved.relative_to(resolved_root)
    except ValueError:
        raise GateError("prepared path escapes the environment: %s" % logical)
    return candidate


def elf_identity(path):
    with Path(path).open("rb") as stream:
        header = stream.read(20)
    if len(header) < 20 or header[:4] != b"\x7fELF":
        return None
    return header[4], struct.unpack_from("<H", header, 18)[0]


def check_provenance(environment, contract):
    receipt_path = environment / "environment-receipt.json"
    require(receipt_path.is_file() and not receipt_path.is_symlink(),
            "prepared environment receipt is missing")
    receipt = load_json(receipt_path)
    require(receipt.get("schema") == "nxframework-prepared-device-environment-v1",
            "unexpected receipt schema")
    require(receipt.get("environment_id") == contract["id"],
            "receipt belongs to another environment")
    require(receipt["contract_sha256"] == sha256_file(CONTRACT),
            "the contract changed after preparation; prepare again")
    source = receipt["source_image"]
    firmware = contract["firmware"]
    require(source["sha256"] == firmware["image_sha256"]
            and source["size"] == firmware["image_size"],
            "receipt does not describe the official image of the contract")
    require(source["rootfs_offset"] == contract["image_layout"]["rootfs_offset"],
            "receipt rootfs offset differs from the contract")
    evidence = receipt["evidence"]
    require(evidence["hardware_ran"] is False
            and evidence["device_access"] is False
            and evidence["physical_runtime_claim"] is False,
            "the receipt claims hardware evidence it cannot have")
    require(not firmware["stored_in_repository"]
            and not firmware["redistributed_by_framework"],
            "the contract must keep the image out of the repository")
    require(receipt.get("layout") == {
                "root": "root", "receipt": "environment-receipt.json"},
            "prepared layout is not the fixed v1 layout")
    return receipt


def check_roots_and_isolation(root, contract, receipt):
    """Verify every pinned file and prove that ABI roots do not cross."""
    require(set(receipt.get("inventory", {})) == set(ARCH_ELF),
            "receipt inventory must contain exactly both supported ABIs")
    require(set(receipt.get("files", {})) == set(ARCH_ELF),
            "prepared receipt predates per-file pins; prepare it again")
    require(set(receipt.get("counts", {})) == set(ARCH_ELF),
            "receipt counts must contain exactly both supported ABIs")
    require(set(receipt.get("soname_aliases", {})) == set(ARCH_ELF),
            "receipt aliases must contain exactly both supported ABIs")
    counted = {"aarch64": 0, "armv7": 0}
    regular_paths = set()
    alias_targets = {}
    alias_candidates = {}
    record_by_path = {}
    for abi, directories in contract["topology"]["roots"].items():
        expected = ARCH_ELF[abi]
        records = receipt["files"][abi]
        require(isinstance(records, list) and records,
                "no %s runtime files were recorded" % abi)
        require(receipt["inventory"][abi] ==
                [record.get("path") if isinstance(record, dict) else None
                 for record in records],
                "%s legacy inventory differs from per-file pins" % abi)
        require(receipt["counts"][abi] == len(records),
                "%s receipt count differs from its inventory" % abi)
        for record in records:
            require(isinstance(record, dict) and set(record) == {
                        "path", "abi", "size", "sha256", "elf", "soname",
                        "needed", "aliases"},
                    "malformed %s inventory record" % abi)
            logical = record["path"]
            require(record["abi"] == abi,
                    "inventory record has the wrong ABI: %s" % logical)
            require(any(logical == directory or
                        logical.startswith(directory.rstrip("/") + "/")
                        for directory in directories) or
                    logical in contract["seeds"][abi],
                    "%s file is outside the declared %s closure" %
                    (logical, abi))
            require(logical not in regular_paths,
                    "duplicate inventory path: %s" % logical)
            local = safe_relative(root, logical)
            require(local.is_file() and not local.is_symlink(),
                    "prepared regular file is missing: %s" % logical)
            require(isinstance(record["size"], int) and record["size"] > 0 and
                    local.stat().st_size == record["size"],
                    "prepared file size differs: %s" % logical)
            require(isinstance(record["sha256"], str) and
                    SHA256_RE.fullmatch(record["sha256"]) and
                    sha256_file(local) == record["sha256"],
                    "prepared file SHA-256 differs: %s" % logical)
            identity = elf_identity(local)
            require(identity == expected and record["elf"] == {
                        "class": expected[0], "machine": expected[1]},
                    "prepared file has the wrong ELF identity: %s" % logical)
            require(isinstance(record["needed"], list) and
                    all(isinstance(name, str) and name not in ("", ".", "..")
                        and "/" not in name for name in record["needed"]),
                    "unsafe DT_NEEDED list in %s" % logical)
            require(record["soname"] is None or
                    (isinstance(record["soname"], str) and
                     record["soname"] not in ("", ".", "..") and
                     "/" not in record["soname"]),
                    "unsafe SONAME in %s" % logical)
            require(isinstance(record["aliases"], list),
                    "malformed aliases in %s" % logical)
            actual_needed, actual_soname = PREPARER.elf_dynamic(local)
            require(record["needed"] == actual_needed,
                    "DT_NEEDED differs from the pinned ELF: %s" % logical)
            require(record["soname"] == actual_soname,
                    "DT_SONAME differs from the pinned ELF: %s" % logical)
            expected_alias = None
            if actual_soname and actual_soname != PurePosixPath(logical).name:
                expected_alias = posixpath.join(
                    posixpath.dirname(logical), actual_soname)
                alias_candidates.setdefault(expected_alias, []).append(logical)
            allowed_aliases = ([[]] if expected_alias is None else
                               [[], [expected_alias]])
            require(record["aliases"] in allowed_aliases,
                    "SONAME alias is not derived from its ELF: %s" % logical)
            for alias in record["aliases"]:
                require(alias not in alias_targets,
                        "duplicate SONAME alias: %s" % alias)
                alias_targets[alias] = logical
            regular_paths.add(logical)
            record_by_path[logical] = record
            counted[abi] += 1

        aliases = sorted(alias for record in records
                         for alias in record["aliases"])
        require(aliases == receipt["soname_aliases"][abi],
                "%s SONAME aliases differ from inventory" % abi)
        available_names = {
            PurePosixPath(record["path"]).name for record in records}
        available_names.update(PurePosixPath(alias).name for alias in aliases)
        unresolved = sorted({name for record in records
                             for name in record["needed"]
                             if name not in available_names})
        require(not unresolved,
                "%s receipt has unresolved DT_NEEDED entries: %s" %
                (abi, ",".join(unresolved)))

    for alias, candidates in sorted(alias_candidates.items()):
        if alias in regular_paths:
            require(alias not in alias_targets,
                    "SONAME alias collides with a regular file: %s" % alias)
            continue
        require(alias in alias_targets,
                "missing alias for ELF SONAME: %s" % alias)
        owner = alias_targets[alias]
        require(all(record_by_path[path]["sha256"] ==
                    record_by_path[owner]["sha256"] for path in candidates),
                "one SONAME alias represents different ELF payloads: %s" %
                alias)

    for alias, target in sorted(alias_targets.items()):
        local = safe_relative(root, alias)
        require(local.is_symlink(), "SONAME alias is not a symlink: %s" % alias)
        require(local.resolve(strict=True) ==
                safe_relative(root, target).resolve(strict=True),
                "SONAME alias points to the wrong file: %s" % alias)

    found_regular = set()
    found_aliases = set()
    for path in root.rglob("*"):
        logical = "/" + path.relative_to(root).as_posix()
        if path.is_symlink():
            found_aliases.add(logical)
        elif path.is_file():
            found_regular.add(logical)
        else:
            require(path.is_dir(), "special file in prepared root: %s" % logical)
    require(found_regular == regular_paths,
            "prepared regular-file set differs from the pinned inventory")
    require(found_aliases == set(alias_targets),
            "prepared symlink set differs from the pinned aliases")

    # A fronteira que interessa: as duas raizes existem e nao se cruzam.
    aarch64_roots = {d for d in contract["topology"]["roots"]["aarch64"]}
    armv7_roots = {d for d in contract["topology"]["roots"]["armv7"]}
    require(not aarch64_roots & armv7_roots,
            "the contract mixes 32-bit and 64-bit roots")
    return counted


def check_interpreters(root, contract):
    for abi, interpreter in contract["topology"]["interpreters"].items():
        local = safe_relative(root, interpreter)
        require(local.is_file(), "missing %s interpreter: %s"
                % (abi, interpreter))
        require(elf_identity(local) == ARCH_ELF[abi],
                "%s interpreter has the wrong ABI" % abi)


def check_python(root, contract):
    runtime = contract["interpreter_runtime"]
    local = safe_relative(root, runtime["python_path"])
    require(local.is_file(), "the image's python is missing from the environment")
    require(elf_identity(local) == ARCH_ELF["aarch64"],
            "the image's python is not the host ABI")
    version = runtime["python_version"]
    require(PYTHON_VERSION_RE.fullmatch(version),
            "declared python version is not a version: %r" % version)
    require(version in runtime["python_path"],
            "the declared python version does not match the recorded path")
    require(len(runtime.get("note", "")) > 40,
            "the python entry must state what this environment does and does "
            "not prove")
    qemu = resolve_tool(QEMU["aarch64"], required=False)
    require(qemu is not None,
            "%s is required to measure the image's Python" % QEMU["aarch64"])
    interpreter = contract["topology"]["interpreters"]["aarch64"]
    search = [str(safe_relative(root, directory, must_exist=False))
              for directory in contract["topology"]["roots"]["aarch64"]]
    run = subprocess.run(
        [qemu, str(safe_relative(root, interpreter)), "--library-path",
         ":".join(search), str(local), "--version"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
        env=clean_subprocess_env(), timeout=20)
    output = run.stdout.decode("utf-8", "replace").strip()
    require(run.returncode == 0,
            "the image's Python did not execute under qemu (%d): %s" %
            (run.returncode, output))
    match = PYTHON_OUTPUT_RE.fullmatch(output)
    require(match is not None and match.group(1).startswith(version + "."),
            "measured Python version differs from the contract: %s" % output)
    return match.group(1)


def check_renderer(root, contract):
    present = {}
    for key, abi in (("aarch64_providers", "aarch64"),
                     ("armv7_providers", "armv7")):
        found = []
        for provider in contract["renderer"][key]:
            local = safe_relative(root, provider, must_exist=False)
            if not local.exists():
                continue
            require(elf_identity(local) == ARCH_ELF[abi],
                    "renderer provider has the wrong ABI: %s" % provider)
            found.append(provider)
        require(found, "no %s renderer provider was prepared" % abi)
        present[abi] = found
    return present


def query_sdl_drivers(root, contract, abi):
    clang = resolve_tool("clang", required=False)
    qemu = resolve_tool(QEMU[abi], required=False)
    if clang is None or qemu is None:
        return None
    library = None
    for provider in contract["renderer"]["%s_providers"
                                         % ("aarch64" if abi == "aarch64"
                                            else "armv7")]:
        if "libSDL2" in provider:
            library = safe_relative(root, provider, must_exist=False)
            break
    require(library is not None and library.is_file(),
            "no SDL library prepared for %s" % abi)
    interpreter = contract["topology"]["interpreters"][abi]
    search = [str(safe_relative(root, d, must_exist=False)) for d in
              contract["topology"]["roots"][abi]]
    with tempfile.TemporaryDirectory(prefix="muos-sdl-probe.") as temporary:
        probe = Path(temporary) / ("sdl-drivers-%s" % abi)
        build = subprocess.run(
            [clang] + CLANG_TARGET[abi] + [
                "-O2", "-fuse-ld=lld", "-nostdlib", "-fno-builtin",
                "-fno-stack-protector",
                "-Wl,--dynamic-linker=%s" % interpreter,
                "-Wl,-e,_start", "-Wl,--no-as-needed",
                "-Wl,--allow-shlib-undefined",
                str(PROBE), "-L%s" % library.parent,
                "-l:%s" % library.name, "-o", str(probe),
            ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
            cwd=temporary, env=clean_subprocess_env(), timeout=30)
        require(build.returncode == 0,
                "%s SDL probe failed to build:\n%s"
                % (abi, build.stdout.decode("utf-8", "replace")))
        require(elf_identity(probe) == ARCH_ELF[abi],
                "the compiled %s probe has the wrong ABI" % abi)
        run = subprocess.run(
            [qemu, str(safe_relative(root, interpreter)),
             "--library-path", ":".join(search), str(probe)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
            cwd=temporary, env=clean_subprocess_env(), timeout=30)
        output = run.stdout.decode("utf-8", "replace")
        require(run.returncode == 0,
                "%s SDL probe failed (%d):\n%s" % (abi, run.returncode, output))
    lines = dict(line.split("=", 1) for line in output.strip().splitlines()
                 if "=" in line)
    require(lines.get("sdl") == contract["sdl"]["expected_version"],
            "%s SDL version differs from the contract: %s"
            % (abi, lines.get("sdl")))
    drivers = [item for item in lines.get("drivers", "").split(",") if item]
    require(drivers, "the %s SDL reports no compiled video driver" % abi)
    return drivers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--environment", required=True)
    parser.add_argument("--skip-sdl-query", action="store_true",
                        help="skip the clang/qemu SDL query")
    arguments = parser.parse_args()

    raw_environment = Path(arguments.environment)
    require(raw_environment.is_absolute(), "--environment must be absolute")
    require(raw_environment.is_dir() and not raw_environment.is_symlink(),
            "environment directory not found or is a symlink")
    environment = outside_repository(raw_environment, "environment")
    contract = load_json(CONTRACT)
    receipt = check_provenance(environment, contract)
    root = environment / "root"
    require(root.is_dir() and not root.is_symlink(),
            "prepared root is missing or is a symlink")

    counted = check_roots_and_isolation(root, contract, receipt)
    check_interpreters(root, contract)
    python_version = check_python(root, contract)
    renderer = check_renderer(root, contract)

    drivers = {}
    if not arguments.skip_sdl_query:
        for abi in ("aarch64", "armv7"):
            result = query_sdl_drivers(root, contract, abi)
            if result is not None:
                drivers[abi] = result

    print("muos environment gate passed: aarch64_elfs=%d armv7_elfs=%d "
          "isolation=clean python=%s renderer=%s sdl=%s "
          "hardware_ran=0 device_access=0"
          % (counted["aarch64"], counted["armv7"], python_version,
             "+".join("%s:%d" % (abi, len(found))
                      for abi, found in sorted(renderer.items())),
             ";".join("%s:%s" % (abi, ",".join(found))
                      for abi, found in sorted(drivers.items())) or "skipped"))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except GateError as error:
        print("muos environment gate failed: %s" % error, file=sys.stderr)
        sys.exit(1)
    except Exception as error:  # noqa: BLE001 - fronteira do gate
        # Falha de contencao do imagefs (byte trocado, sobra, symlink)
        # chega como excecao propria dele; o gate reporta igual.
        print("muos environment gate failed: %s" % error, file=sys.stderr)
        sys.exit(1)
