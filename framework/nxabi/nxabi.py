#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""M17 ABI/toolchain gate: machine-readable ELF inventory and audit.

This is a host-side, read-only tool.  It never executes an inspected ELF, never
loads guest code and never touches a device.  GNU readelf and Python 3.7+ are
the only runtime requirements.

Subcommands
-----------
inventory   emit the machine-readable ELF inventory for files or directories
audit       apply the public universal policy and fail on any violation
sdl-table   regenerate sdl2-symbol-floor.tsv from a local SDL2 header tree
toolchain   verify the pinned toolchains described by TOOLCHAIN-PIN.json
"""

from __future__ import print_function

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

TOOL_NAME = "nxabi"
TOOL_VERSION = "0.2.3"
SCHEMA_VERSION = 1
MODULE_DIR = Path(__file__).resolve().parent
DEFAULT_POLICY = MODULE_DIR / "policy-v1.json"
DEFAULT_PIN = MODULE_DIR / "TOOLCHAIN-PIN.json"

# V4-03B: single versioned authority mapping SDL symbol -> minimum SDL
# version.  nxabi and nxrelease must take the same floor decision from the
# same bytes, so both consume this exact file through load_symbol_authority.
SDL_AUTHORITY_ID = "nx-sdl-symbol-floor/1"
SDL_AUTHORITY_TABLE = MODULE_DIR / "sdl2-symbol-floor.tsv"
SDL_AUTHORITY_MAX_BYTES = 4 * 1024 * 1024

GLIBC_RE = re.compile(r"\bGLIBC_(?:[0-9]+(?:\.[0-9]+)+|PRIVATE|ABI_[A-Za-z0-9_]+)\b")
GLIBCXX_RE = re.compile(r"\bGLIBCXX_(?:[0-9]+(?:\.[0-9]+)+)\b")
CXXABI_RE = re.compile(r"\bCXXABI_(?:[0-9]+(?:\.[0-9]+)+)\b")
VERSIONED_UND_RE = re.compile(
    r"^([A-Za-z_][A-Za-z0-9_]*)@@?(GLIBC|GLIBCXX|CXXABI)_([0-9][0-9.]*)$"
)
BIONIC_SONAMES = frozenset((
    "libc.so", "libdl.so", "libm.so", "liblog.so", "libandroid.so",
    "libGLESv2.so", "libEGL.so", "libOpenSLES.so", "libstdc++.so",
))


class AbiError(Exception):
    """A user-facing failure of the tool itself (not a policy finding)."""


# ---------------------------------------------------------------- utilities


def fail(message):
    raise AbiError(message)


def version_tuple(value):
    return tuple(int(part) for part in value.split("."))


def version_gt(left, right):
    return version_tuple(left) > version_tuple(right)


def max_version(values):
    if not values:
        return None
    return sorted(values, key=version_tuple)[-1]


def sha256_file(path):
    digest = hashlib.sha256()
    with open(str(path), "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_elf(path):
    try:
        with open(str(path), "rb") as handle:
            return handle.read(4) == b"\x7fELF"
    except OSError:
        return False


def run_readelf(path, arguments):
    environment = dict(os.environ)
    environment["LC_ALL"] = "C"
    process = subprocess.run(
        ["readelf"] + list(arguments) + ["--", str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
        env=environment,
    )
    if process.returncode != 0:
        detail = (process.stderr or process.stdout).strip()
        fail("readelf {} rejected {}: {}".format(" ".join(arguments), path, detail))
    return process.stdout


# ------------------------------------------------------------- ELF inspection


def inspect_elf(path, logical_path=None):
    """Return every ABI fact the gate needs about one ELF file."""
    logical = logical_path or str(path)
    header = run_readelf(path, ("-hW",))

    def header_field(name):
        match = re.search(
            r"^\s*{}:\s*(.+?)\s*$".format(name), header, re.MULTILINE
        )
        return match.group(1) if match else None

    elf_class = header_field("Class")
    data = header_field("Data")
    elf_type = header_field("Type")
    machine = header_field("Machine")
    flags = header_field("Flags") or ""
    os_abi = header_field("OS/ABI")

    program_headers = run_readelf(path, ("-lW",))
    load_count = len(re.findall(r"^\s*LOAD\s", program_headers, re.MULTILINE))
    interpreters = re.findall(
        r"Requesting program interpreter:\s*([^\]]+)\]", program_headers
    )
    gnu_stack = re.search(
        r"^\s*GNU_STACK\s+\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+([RWE ]+)",
        program_headers,
        re.MULTILINE,
    )

    dynamic = run_readelf(path, ("-dW",))
    needed = re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", dynamic)
    sonames = re.findall(r"\(SONAME\).*?\[([^\]]+)\]", dynamic)
    rpath = re.findall(r"\(RPATH\).*?\[([^\]]*)\]", dynamic)
    runpath = re.findall(r"\(RUNPATH\).*?\[([^\]]*)\]", dynamic)

    versions = run_readelf(path, ("--version-info", "--wide"))
    glibc_tokens = sorted(set(GLIBC_RE.findall(versions)))
    glibc_numeric = sorted(
        {token[len("GLIBC_"):] for token in glibc_tokens
         if re.match(r"^GLIBC_[0-9]", token)},
        key=version_tuple,
    )
    glibcxx_numeric = sorted(
        {token[len("GLIBCXX_"):] for token in set(GLIBCXX_RE.findall(versions))},
        key=version_tuple,
    )
    cxxabi_numeric = sorted(
        {token[len("CXXABI_"):] for token in set(CXXABI_RE.findall(versions))},
        key=version_tuple,
    )
    forbidden_tokens = sorted(
        token for token in glibc_tokens if not re.match(r"^GLIBC_[0-9]", token)
    )

    undefined = undefined_symbols(path)
    glibc_max = max_version(glibc_numeric)
    floor_symbols = sorted(
        name for name, (library, version) in undefined.items()
        if library == "GLIBC" and glibc_max is not None and version == glibc_max
    )

    notes = run_readelf(path, ("-nW",))
    build_id_match = re.search(r"Build ID:\s*([0-9a-f]+)", notes)
    toolchain_note = extract_toolchain_note(path)

    architecture = {"ARM": "armv7", "AArch64": "aarch64"}.get(machine)
    interpreter = interpreters[0].strip() if interpreters else None

    record = {
        "path": logical,
        "sha256": sha256_file(path),
        "size": os.path.getsize(str(path)),
        "build_id": build_id_match.group(1) if build_id_match else None,
        "class": elf_class,
        "data": data,
        "elf_type": elf_type,
        "machine": machine,
        "flags": flags,
        "os_abi": os_abi,
        "architecture": architecture,
        "namespace": classify_namespace(interpreter, needed, glibc_numeric),
        "pt_load_count": load_count,
        "pt_interp": interpreter,
        "pt_interp_count": len(interpreters),
        "pt_gnu_stack": (gnu_stack.group(1).strip() if gnu_stack else None),
        "needed": sorted(needed),
        "needed_raw_count": len(needed),
        "soname": sonames[0] if sonames else None,
        "soname_count": len(sonames),
        "rpath": rpath,
        "runpath": runpath,
        "glibc_versions": glibc_numeric,
        "glibc_max": glibc_max,
        "glibc_floor_symbols": floor_symbols,
        "glibcxx_max": max_version(glibcxx_numeric),
        "cxxabi_max": max_version(cxxabi_numeric),
        "forbidden_version_tokens": forbidden_tokens,
        "undefined_symbols": sorted(undefined),
        "undefined_sdl": sorted(
            name for name in undefined if name.startswith("SDL_")
        ),
        "toolchain_note": toolchain_note,
    }
    return record


def undefined_symbols(path):
    """Map every undefined dynamic symbol to (library, version) when versioned."""
    output = run_readelf(path, ("-sW", "--dyn-syms"))
    result = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 8 or not fields[0].endswith(":"):
            continue
        if fields[6] != "UND":
            continue
        raw = fields[7]
        match = VERSIONED_UND_RE.match(raw)
        if match:
            # readelf can print the same undefined symbol twice (once per
            # symbol table, with @VER and @@VER).  Never let an unversioned
            # or lower entry overwrite a versioned one.
            name, library, version = match.groups()
            known = result.get(name)
            if known is None or known[1] is None or version_gt(version, known[1]):
                result[name] = (library, version)
        else:
            name = raw.split("@")[0]
            if name not in result:
                result[name] = (None, None)
    result.pop("", None)
    return result


def classify_namespace(interpreter, needed, glibc_numeric):
    if interpreter and interpreter.startswith("/system/bin/linker"):
        return "android"
    if glibc_numeric:
        return "linux"
    if any(item in BIONIC_SONAMES for item in needed):
        return "android"
    return "linux"


def extract_toolchain_note(path):
    """Read .comment and the project's own .note.nx.toolchain section, if any."""
    environment = dict(os.environ)
    environment["LC_ALL"] = "C"
    notes = []
    for section in (".comment", ".note.nx.toolchain"):
        process = subprocess.run(
            ["readelf", "-p", section, "--", str(path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            universal_newlines=True,
            env=environment,
        )
        if process.returncode != 0:
            continue
        for line in process.stdout.splitlines():
            match = re.match(r"^\s*\[\s*[0-9a-f]+\]\s+(.*\S)\s*$", line)
            if match:
                notes.append(match.group(1))
    unique = []
    for note in notes:
        if note not in unique:
            unique.append(note)
    return unique or None


# ------------------------------------------------------------------ discovery


def discover(targets, follow_symlinks=False):
    """Return every ELF under the given files/directories, sorted by path."""
    found = []
    seen = set()
    for target in targets:
        base = Path(target)
        if not base.exists():
            fail("missing path: {}".format(target))
        if base.is_dir():
            candidates = sorted(base.rglob("*"))
        else:
            candidates = [base]
        for candidate in candidates:
            if candidate.is_symlink() and not follow_symlinks:
                continue
            if not candidate.is_file():
                continue
            resolved = str(candidate.resolve())
            if resolved in seen:
                continue
            if not is_elf(candidate):
                continue
            seen.add(resolved)
            found.append(candidate)
    return sorted(found, key=lambda item: str(item))


# --------------------------------------------------------------------- policy


def load_policy(path):
    try:
        with open(str(path), "r", encoding="utf-8") as stream:
            policy = json.load(stream)
    except (OSError, ValueError) as error:
        fail("cannot load policy {}: {}".format(path, error))
    if policy.get("schema_version") != 1:
        fail("unsupported policy schema in {}".format(path))
    return policy


SDL_AUTHORITY_SYMBOL_RE = re.compile(r"SDL_[A-Za-z0-9_]+\Z")
SDL_AUTHORITY_VERSION_RE = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+\Z")
SDL_AUTHORITY_DIRECTIVE_RE = re.compile(r"#%\s*authority:\s*(\S+)\s*\Z")


def load_symbol_authority(table_path=None):
    """Strict fail-closed loader of the single SDL symbol->version authority.

    Every consumer of the universal SDL floor (nxabi audit, nxrelease
    preflight) must call this parser over the same bytes.  A missing,
    symlinked, oversized, non-UTF-8, malformed, ambiguously duplicated or
    unidentified table never degrades to an empty allow-everything mapping:
    it raises, and the caller's gate fails closed (V4-PRE-02/03B).
    """
    path = Path(table_path) if table_path else SDL_AUTHORITY_TABLE
    if path.is_symlink():
        fail("SDL symbol authority must not be a symlink: {}".format(path))
    if not path.is_file():
        fail("SDL symbol authority is missing: {}".format(path))
    try:
        raw = path.read_bytes()
    except OSError as error:
        fail("cannot read SDL symbol authority {}: {}".format(path, error))
    if len(raw) > SDL_AUTHORITY_MAX_BYTES:
        fail("SDL symbol authority is unreasonably large: {}".format(path))
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        fail("SDL symbol authority is not UTF-8 ({}): {}".format(error, path))
    authority = None
    table = {}
    for number, line in enumerate(text.splitlines(), 1):
        if line.startswith("#%"):
            directive = SDL_AUTHORITY_DIRECTIVE_RE.match(line)
            if not directive:
                fail("malformed authority directive at {}:{}".format(
                    path, number))
            if authority is not None:
                fail("duplicate authority directive at {}:{}".format(
                    path, number))
            authority = directive.group(1)
            continue
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 3:
            fail("malformed authority row at {}:{} "
                 "(expected symbol<TAB>version<TAB>source)".format(
                     path, number))
        symbol, version, source = parts
        if not SDL_AUTHORITY_SYMBOL_RE.match(symbol):
            fail("invalid SDL symbol name at {}:{}: {!r}".format(
                path, number, symbol))
        if not SDL_AUTHORITY_VERSION_RE.match(version):
            fail("invalid SDL version at {}:{}: {!r}".format(
                path, number, version))
        if not source or any(item.isspace() for item in source):
            fail("invalid source token at {}:{}".format(path, number))
        if symbol in table:
            fail("ambiguous duplicate symbol in authority at {}:{}: {}".format(
                path, number, symbol))
        table[symbol] = (version, source)
    if authority != SDL_AUTHORITY_ID:
        fail("SDL symbol authority id is {!r}, expected {!r}: {}".format(
            authority, SDL_AUTHORITY_ID, path))
    if not table:
        fail("SDL symbol authority has no rows: {}".format(path))
    return {
        "authority": authority,
        "path": str(path),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "symbol_count": len(table),
        "table": table,
    }


def load_sdl_table(policy, policy_path):
    """Symbol->(version, source) mapping from the single authority.

    Membership in a SONAME allowlist or closure never equals meeting the
    floor; the version decision always comes from this table, and a table
    that cannot be trusted fails closed instead of returning {}.
    """
    return load_sdl_authority_for_policy(policy, policy_path)["table"]


def load_sdl_authority_for_policy(policy, policy_path):
    table_name = policy.get("sdl", {}).get("table")
    if not table_name:
        fail("policy {} declares no sdl.table; the universal profile "
             "requires the single SDL symbol authority".format(policy_path))
    table_path = Path(policy_path).resolve().parent / table_name
    return load_symbol_authority(table_path)


def finding(level, record, check, message):
    return {
        "level": level,
        "path": record["path"],
        "check": check,
        "message": message,
    }


def apply_exceptions(findings, record, policy, options=None):
    """Downgrade a finding to a warning when the artifact has a signed waiver.

    A waiver is keyed by the artifact sha256, so it dies the moment the binary
    is rebuilt.  It never hides the finding; it only stops it from failing the
    gate for an artifact that already has physical evidence.

    V4-05A (audit divergence 3): the SDL floor checks are NOT waivable for a
    public/universal profile. A historical waiver stays readable -- the
    finding keeps the waiver metadata -- but the level never moves, so a
    Vendor/Product import above SDL 2.0.4 can never re-enter a new/universal
    candidate through an old signature.
    """
    profile = (options or {}).get("profile", "universal-low-glibc")
    is_public = policy.get("build_profiles", {}).get(
        profile, {}).get("public", True)
    waivers = policy.get("exceptions", {})
    waiver = waivers.get(record["sha256"])
    if not waiver:
        return findings
    waived_checks = set(waiver.get("checks", ()))
    result = []
    for item in findings:
        if item["level"] == "error" and item["check"] in waived_checks:
            item = dict(item)
            if is_public and item["check"] in NON_WAIVABLE_PUBLIC_CHECKS:
                item["waived"] = False
                item["waiver_reason"] = waiver.get("reason", "")
                item["message"] += (
                    " [waiver refused: {} is not waivable for a public/"
                    "universal candidate]".format(item["check"]))
                result.append(item)
                continue
            item["level"] = "warn"
            item["waived"] = True
            item["waiver_reason"] = waiver.get("reason", "")
            item["message"] += " [waived: {}]".format(
                waiver.get("reason", "no reason recorded"))
        result.append(item)
    return result


def decide_sdl_floor(symbols, sdl_table, sdl_floor):
    """THE single SDL2-core floor decision, shared by nxabi and nxrelease.

    V4-05A parity: both consumers call THIS function over the same authority
    bytes, so a symbol can never be a warning in one tool and a fatal in the
    other. For every direct SDL_* import the verdict is:

      ("ok", since)          born at or below the floor;
      ("post-floor", since)  born above the declared floor -- fails closed
                             for a new/universal candidate in both tools;
      ("unknown", None)      absent from the authority -- an unproven symbol
                             cannot meet the floor and fails closed for a
                             new/universal candidate in both tools;
      ("assumed", since)     no \\since annotation, assumed 2.0.0 baseline.
    """
    verdicts = []
    for symbol in sorted(symbols):
        entry = sdl_table.get(symbol)
        if entry is None:
            verdicts.append((symbol, "unknown", None))
            continue
        since, source = entry
        if version_gt(since, sdl_floor):
            verdicts.append((symbol, "post-floor", since))
        elif source == "assumed-baseline":
            verdicts.append((symbol, "assumed", since))
        else:
            verdicts.append((symbol, "ok", since))
    return verdicts


# Checks that guard the universal floor: a signed waiver may annotate them
# but can never downgrade the verdict of a NEW/universal (public) candidate,
# and can never let Vendor/Product (SDL 2.0.6) pass the 2.0.4 floor.
# Historical evidence stays readable in the waiver metadata; the level does
# not move (V4-05A audit divergence 3).
NON_WAIVABLE_PUBLIC_CHECKS = frozenset(("sdl-floor", "sdl-unknown"))


def audit_record(record, policy, sdl_table, options):
    """Apply the whole policy to one ELF and return a list of findings."""
    out = []
    ceilings = policy["ceilings"]
    profile = options.get("profile", "universal-low-glibc")
    profiles = policy.get("build_profiles", {})
    is_public = profiles.get(profile, {}).get("public", True)
    namespace = record["namespace"]

    # M17-006: an Android BYO ELF is never a Linux build of the project.
    if namespace == "android":
        if record["glibc_versions"] or record["forbidden_version_tokens"]:
            out.append(finding(
                "error", record, "android-namespace",
                "tagged Android but references GLIBC symbol versions",
            ))
        expected = policy["android_interpreters"].get(record["architecture"])
        if record["pt_interp"] and record["pt_interp"] != expected:
            out.append(finding(
                "error", record, "android-interpreter",
                "Android ELF has PT_INTERP {} (expected {})".format(
                    record["pt_interp"], expected),
            ))
        out.append(finding(
            "info", record, "android-upstream",
            "Android upstream ELF; not counted as a Linux build of the project",
        ))
        return out

    # M17-007: class, endianness, machine and float ABI.
    if record["data"] is None or "little endian" not in record["data"]:
        out.append(finding("error", record, "endianness",
                           "ELF is not little-endian"))
    if record["elf_type"] not in ("EXEC (Executable file)",
                                  "DYN (Shared object file)",
                                  "DYN (Position-Independent Executable file)"):
        out.append(finding("error", record, "elf-type",
                           "unexpected ELF type {}".format(record["elf_type"])))
    architecture = record["architecture"]
    if architecture is None:
        out.append(finding("error", record, "machine",
                           "unsupported machine {}".format(record["machine"])))
        return out
    spec = policy["architectures"][architecture]
    if record["class"] != spec["elf_class"]:
        out.append(finding("error", record, "elf-class",
                           "class {} disagrees with {}".format(
                               record["class"], architecture)))
    for required in spec["required_flags"]:
        if required not in record["flags"]:
            out.append(finding("error", record, "float-abi",
                               "missing required ELF flag '{}' (flags: {})".format(
                                   required, record["flags"] or "none")))

    # M17-008: PT_LOAD / PT_INTERP / PT_GNU_STACK.
    if record["pt_load_count"] < 1:
        out.append(finding("error", record, "pt-load", "no PT_LOAD segment"))
    if record["pt_interp_count"] > 1:
        out.append(finding("error", record, "pt-interp",
                           "multiple PT_INTERP segments"))
    interpreter = record["pt_interp"]
    if interpreter is not None:
        if not interpreter.startswith("/"):
            out.append(finding("error", record, "pt-interp",
                               "non-absolute PT_INTERP {}".format(interpreter)))
        elif interpreter.startswith("/home/") or interpreter.startswith("/Users/"):
            out.append(finding("error", record, "pt-interp",
                               "PT_INTERP embeds a personal path"))
        elif interpreter != spec["interpreter"]:
            out.append(finding("error", record, "pt-interp",
                               "PT_INTERP must be {}; got {}".format(
                                   spec["interpreter"], interpreter)))
    stack = record["pt_gnu_stack"]
    if stack and "E" in stack:
        out.append(finding("error", record, "pt-gnu-stack",
                           "executable stack ({})".format(stack)))
    if stack is None:
        out.append(finding("warn", record, "pt-gnu-stack",
                           "no PT_GNU_STACK segment; the kernel assumes an "
                           "executable stack"))

    # M17-009: DT_NEEDED / SONAME / RPATH / RUNPATH.
    if record["needed_raw_count"] != len(set(record["needed"])):
        out.append(finding("error", record, "dt-needed",
                           "repeated DT_NEEDED entry"))
    if record["soname_count"] > 1:
        out.append(finding("error", record, "dt-soname",
                           "multiple DT_SONAME values"))
    allowlist = policy["soname_allowlist"]
    forbidden = policy["soname_forbidden"]
    for item in record["needed"]:
        if item in forbidden:
            out.append(finding("error", record, "soname-forbidden",
                               "DT_NEEDED {} is forbidden: {}".format(
                                   item, forbidden[item])))
        elif item not in allowlist:
            out.append(finding("warn", record, "soname-unknown",
                               "DT_NEEDED {} is not in the policy allowlist"
                               .format(item)))
    search = policy["search_paths"]
    for kind, values in (("RPATH", record["rpath"]), ("RUNPATH", record["runpath"])):
        for value in values:
            allowed = search["allow_rpath"] if kind == "RPATH" else search["allow_runpath"]
            if allowed:
                continue
            if search.get("allow_origin_only") and value == "$ORIGIN":
                out.append(finding("warn", record, "search-path",
                                   "{} $ORIGIN accepted by policy exception"
                                   .format(kind)))
                continue
            out.append(finding("error", record, "search-path",
                               "embeds {}=[{}]; universal packages require none"
                               .format(kind, value)))
    for value in record["rpath"] + record["runpath"]:
        if value.startswith("/home/") or value.startswith("/Users/"):
            out.append(finding("error", record, "search-path",
                               "{} embeds a personal path".format(value)))

    # M17-003 / M17-010: GLIBC, GLIBCXX and CXXABI ceilings.
    for token in record["forbidden_version_tokens"]:
        out.append(finding("error", record, "version-token",
                           "requires private/unsupported ABI token {}"
                           .format(token)))
    checks = (
        ("glibc", record["glibc_max"], ceilings["glibc_max"], "GLIBC"),
        ("glibcxx", record["glibcxx_max"], ceilings["glibcxx_max"], "GLIBCXX"),
        ("cxxabi", record["cxxabi_max"], ceilings["cxxabi_max"], "CXXABI"),
    )
    for check, value, ceiling, label in checks:
        if value is None:
            continue
        if version_gt(value, ceiling):
            level = "error" if is_public else "warn"
            out.append(finding(level, record, check + "-ceiling",
                               "requires {}_{} (ceiling {}_{})".format(
                                   label, value, label, ceiling)))
    # M17-002: prefer the lowest viable floor and name what raises it.
    glibc_max = record["glibc_max"]
    preferred = ceilings["glibc_preferred"]
    if glibc_max and version_gt(glibc_max, preferred):
        out.append(finding(
            "warn", record, "glibc-preferred",
            "floor is GLIBC_{} (preferred {}); raised by: {}".format(
                glibc_max, preferred,
                ", ".join(record["glibc_floor_symbols"]) or "unknown"),
        ))

    # M17-015: glibc wrappers newer than the preferred floor remain visible,
    # but preference is not a second hidden ceiling.  Only a wrapper introduced
    # after the public maximum fails; one at or below it is an auditable warning.
    blacklist = policy["wrapper_blacklist"]
    for symbol in record["undefined_symbols"]:
        since = blacklist.get(symbol)
        if since is None:
            continue
        ceiling = ceilings["glibc_max"]
        above_ceiling = version_gt(since, ceiling)
        level = "error" if above_ceiling else "warn"
        detail = (
            "exceeds the public ceiling {}; use syscall() or a shim".format(
                ceiling)
            if above_ceiling else
            "raises the preferred floor {} but remains within the public "
            "ceiling {}; a syscall() or shim is preferred".format(
                preferred, ceiling)
        )
        out.append(finding(
            level, record, "new-libc-api",
            "imports {} (glibc wrapper since {}); {}"
            .format(symbol, since, detail),
        ))

    # M17-014: SDL API floor.
    sdl_floor = options.get("sdl_floor") or policy.get("sdl", {}).get("floor")
    # The floor table is SDL2's (the PortMaster-guaranteed runtime). An ELF
    # whose DT_NEEDED names a bundled SDL3 resolves its SDL_* imports against
    # that bundled library, never against the firmware/PortMaster SDL2 — the
    # SDL2 floor is the wrong namespace for it. The bundle itself is enforced
    # by nxrelease (provider=package or the dependency gate fails).
    if any(item.startswith("libSDL3") for item in record["needed"]):
        out.append(finding(
            "info", record, "sdl3-bundled",
            "SDL_* imports resolve against the bundled SDL3 runtime; the "
            "SDL2 floor table does not apply",
        ))
        sdl_floor = None
    if sdl_floor and record["undefined_sdl"]:
        missing = []
        assumed = []
        for symbol, verdict, since in decide_sdl_floor(
                record["undefined_sdl"], sdl_table, sdl_floor):
            if verdict == "post-floor":
                out.append(finding(
                    "error", record, "sdl-floor",
                    "imports {} (SDL {}) above the declared floor SDL {}"
                    .format(symbol, since, sdl_floor),
                ))
            elif verdict == "unknown":
                missing.append(symbol)
            elif verdict == "assumed":
                assumed.append(symbol)
        if missing:
            # V4-05A parity (audit divergence 1): a symbol the authority does
            # not know cannot PROVE the floor. For a public/universal
            # candidate this fails closed in BOTH consumers -- it was a
            # warning here while nxrelease failed, and two tools disagreeing
            # about the same bytes is exactly what 03B forbade.
            out.append(finding(
                "error" if is_public else "warn", record, "sdl-unknown",
                "{} SDL symbol(s) absent from the floor authority (an "
                "unproven symbol cannot meet the floor): {}".format(
                    len(missing), ", ".join(missing[:8])),
            ))
        if assumed:
            out.append(finding(
                "info", record, "sdl-assumed",
                "{} SDL symbol(s) have no \\since annotation and are assumed "
                "to be SDL 2.0.0 baseline".format(len(assumed)),
            ))

    # M17-018: provenance must be recoverable from the artifact itself.
    if record["build_id"] is None:
        out.append(finding("warn", record, "build-id",
                           "no GNU build-id note; provenance is not verifiable "
                           "from the artifact"))
    if record["toolchain_note"] is None:
        out.append(finding("warn", record, "toolchain-note",
                           "no .comment or .note.nx.toolchain; the producing "
                           "toolchain cannot be recovered from the artifact"))
    return out


# ------------------------------------------------------------------- commands


def build_inventory(targets, policy_path, follow_symlinks=False):
    files = discover(targets, follow_symlinks=follow_symlinks)
    records = [inspect_elf(item) for item in files]
    return {
        "schema_version": SCHEMA_VERSION,
        "tool": {"name": TOOL_NAME, "version": TOOL_VERSION},
        "policy": str(policy_path),
        "count": len(records),
        "elves": records,
    }


def command_inventory(arguments):
    inventory = build_inventory(
        arguments.targets, arguments.policy, arguments.follow_symlinks
    )
    emit_json(inventory, arguments.json)
    if not arguments.json:
        return 0
    print("{}: inventory of {} ELF(s) -> {}".format(
        TOOL_NAME, inventory["count"], arguments.json))
    return 0


def command_audit(arguments):
    policy = load_policy(arguments.policy)
    sdl_authority = load_sdl_authority_for_policy(policy, arguments.policy)
    sdl_table = sdl_authority["table"]
    inventory = build_inventory(
        arguments.targets, arguments.policy, arguments.follow_symlinks
    )
    options = {"profile": arguments.profile, "sdl_floor": arguments.sdl_floor}
    findings = []
    for record in inventory["elves"]:
        findings.extend(apply_exceptions(
            audit_record(record, policy, sdl_table, options), record, policy,
            options))
    if not inventory["elves"]:
        fail("no ELF file found in the given targets")

    errors = [item for item in findings if item["level"] == "error"]
    warnings = [item for item in findings if item["level"] == "warn"]
    report = {
        "schema_version": SCHEMA_VERSION,
        "tool": {"name": TOOL_NAME, "version": TOOL_VERSION},
        "policy_id": policy["policy_id"],
        "profile": arguments.profile,
        "ceilings": policy["ceilings"],
        # Receipt of the single SDL symbol authority this audit consumed;
        # nxrelease records the same identity, so agreement is checkable.
        "sdl_authority": {
            "authority": sdl_authority["authority"],
            "sha256": sdl_authority["sha256"],
            "symbol_count": sdl_authority["symbol_count"],
            "floor": (arguments.sdl_floor or
                      policy.get("sdl", {}).get("floor")),
        },
        "counts": {
            "elves": inventory["count"],
            "errors": len(errors),
            "warnings": len(warnings),
        },
        "findings": findings,
        "inventory": inventory["elves"] if arguments.embed_inventory else [],
        # The catalog rows below are recorded claims; this report is the
        # executed evidence that backs them (M17-016, M17-020).
        "catalog_claims_backed": [
            "ABI-*-015-host-elf-class",
            "ABI-*-016-host-machine",
            "ABI-*-017-host-float-abi",
            "ABI-*-018-host-pt-interp",
            "ABI-*-019-host-glibc-ceiling",
            "ABI-*-020-host-dependencies",
        ],
    }
    emit_json(report, arguments.json)

    if not arguments.quiet:
        for item in findings:
            if item["level"] == "info" and not arguments.verbose:
                continue
            if item["level"] == "warn" and arguments.errors_only:
                continue
            print("{}: {}: {}: {}".format(
                item["level"].upper(), item["path"], item["check"],
                item["message"]))
        print("{}: {} ELF(s), {} error(s), {} warning(s)".format(
            TOOL_NAME, inventory["count"], len(errors), len(warnings)))
    if errors:
        return 1
    if warnings and arguments.strict:
        return 1
    return 0


def command_sdl_table(arguments):
    headers = Path(arguments.headers)
    if not headers.is_dir():
        fail("not a directory: {}".format(headers))
    since_re = re.compile(
        r"\\since This function is available since SDL ([0-9]+\.[0-9]+\.[0-9]+)"
    )
    # SDL declares plenty of functions across several lines, so the scan works
    # on the whole file and pairs each declaration with the nearest preceding
    # "\since" annotation instead of matching line by line.
    decl_re = re.compile(r"SDLCALL\s+(SDL_[A-Za-z0-9_]+)\s*\(", re.MULTILINE)
    table = {}
    for header in sorted(headers.glob("*.h")):
        text = header.read_text(encoding="utf-8", errors="replace")
        annotations = [
            (match.start(), match.group(1)) for match in since_re.finditer(text)
        ]
        previous_end = 0
        for declaration in decl_re.finditer(text):
            name = declaration.group(1)
            offset = declaration.start()
            version = None
            # Only an annotation inside this declaration's own doc block
            # counts; otherwise a documented function would lend its version
            # to the next, undocumented one.
            for position, value in annotations:
                if previous_end <= position < offset:
                    version = value
                elif position >= offset:
                    break
            previous_end = declaration.end()
            source = "sdl2-headers" if version else "assumed-baseline"
            version = version or "2.0.0"
            known = table.get(name)
            if known is None or version_gt(version, known[0]):
                table[name] = (version, source)
    lines = [
        "# SPDX-License-Identifier: GPL-3.0-or-later",
        "#% authority: " + SDL_AUTHORITY_ID,
        "# symbol\tsdl_version\tsource",
        "# Generated by nxabi sdl-table from {}".format(headers),
        "# Symbols without a \\since annotation are recorded as 2.0.0 and the",
        "# audit reports them as unknown rather than silently passing.",
    ]
    for name in sorted(table):
        version, source = table[name]
        lines.append("{}\t{}\t{}".format(name, version, source))
    output = "\n".join(lines) + "\n"
    if arguments.out:
        Path(arguments.out).write_text(output, encoding="utf-8")
        print("{}: wrote {} symbols to {}".format(
            TOOL_NAME, len(table), arguments.out))
    else:
        sys.stdout.write(output)
    return 0


def command_provenance(arguments):
    """Emit a C translation unit that stamps the toolchain into the ELF.

    M17-018: a stripped artifact keeps no .comment, so the producing toolchain
    cannot be recovered from the binary.  Compiling this file into the artifact
    puts an auditable .note.nx.toolchain string inside it.
    """
    pin_path = Path(arguments.pin)
    try:
        with open(str(pin_path), "r", encoding="utf-8") as stream:
            pin = json.load(stream)
    except (OSError, ValueError) as error:
        fail("cannot load toolchain pin {}: {}".format(pin_path, error))
    entry = pin.get("toolchains", {}).get(arguments.toolchain)
    if entry is None:
        fail("unknown toolchain {!r} in {}".format(arguments.toolchain, pin_path))

    fields = [
        ("toolchain", arguments.toolchain),
        ("kind", entry.get("kind", "")),
        ("architecture", entry.get("architecture", "")),
        ("compiler", entry.get("version_full") or entry.get("compiler", "")),
        ("image", entry.get("image", "")),
        ("image_id", entry.get("image_id", "")),
        ("sysroot", entry.get("sysroot", "")),
        ("sysroot_glibc", entry.get("sysroot_glibc", "")),
        ("profile", arguments.profile),
        ("pin_id", pin.get("pin_id", "")),
    ]
    if arguments.source_date_epoch:
        fields.append(("source_date_epoch", arguments.source_date_epoch))
    payload = "; ".join(
        "{}={}".format(key, value) for key, value in fields if value
    )
    if '"' in payload or "\\" in payload:
        fail("toolchain pin contains a character that cannot be embedded")
    text = (
        "/* SPDX-License-Identifier: GPL-3.0-or-later\n"
        " * Generated by {} {} -- do not edit.\n"
        " * Compile this file into the artifact so that\n"
        " *   readelf -p .note.nx.toolchain <artifact>\n"
        " * recovers the toolchain that produced it (M17-018).\n"
        " */\n"
        "__attribute__((used, section(\".note.nx.toolchain\")))\n"
        "static const char nx_toolchain_note[] =\n"
        "    \"{}\";\n"
    ).format(TOOL_NAME, TOOL_VERSION, payload)
    if arguments.out:
        Path(arguments.out).write_text(text, encoding="utf-8")
        print("{}: wrote provenance stamp to {}".format(TOOL_NAME, arguments.out))
    else:
        sys.stdout.write(text)
    return 0


def command_toolchain(arguments):
    pin_path = Path(arguments.pin)
    try:
        with open(str(pin_path), "r", encoding="utf-8") as stream:
            pin = json.load(stream)
    except (OSError, ValueError) as error:
        fail("cannot load toolchain pin {}: {}".format(pin_path, error))
    if pin.get("schema_version") != 1:
        fail("unsupported toolchain pin schema")

    results = []
    failures = 0
    for name, entry in sorted(pin.get("toolchains", {}).items()):
        status, detail = verify_toolchain(name, entry)
        results.append({"toolchain": name, "status": status, "detail": detail})
        # A pin is useful only when the exact toolchain is locally available.
        # "absent" remains a diagnostic from verify_toolchain(), but the
        # release-facing command is fail-closed rather than silently partial.
        if status != "ok":
            failures += 1
        if not arguments.quiet:
            print("{}: {:<24} {:<8} {}".format(TOOL_NAME, name, status, detail))
    emit_json({
        "schema_version": SCHEMA_VERSION,
        "tool": {"name": TOOL_NAME, "version": TOOL_VERSION},
        "pin": str(pin_path),
        "results": results,
    }, arguments.json)
    return 1 if failures else 0


def verify_toolchain(name, entry):
    kind = entry.get("kind")
    if kind == "native-cross":
        compiler = Path(entry["compiler"])
        if not compiler.exists():
            return "fail", "missing compiler {}".format(compiler)
        environment = dict(os.environ)
        environment["LC_ALL"] = "C"
        process = subprocess.run(
            [str(compiler), "--version"], stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, universal_newlines=True, env=environment)
        first = (process.stdout or "").splitlines()
        first = first[0] if first else ""
        expected = entry.get("version_string")
        if expected and expected not in first:
            return "fail", "version drift: {!r} does not contain {!r}".format(
                first, expected)
        sysroot = entry.get("sysroot")
        if sysroot and not Path(sysroot).is_dir():
            return "fail", "missing sysroot {}".format(sysroot)
        for relative, expected_hash in sorted(
                entry.get("sysroot_sha256", {}).items()):
            target = Path(sysroot) / relative
            if not target.exists():
                return "fail", "missing {}".format(target)
            actual = sha256_file(target)
            if actual != expected_hash:
                return "fail", "sha256 drift on {}: {}".format(relative, actual)
        return "ok", first
    if kind == "container":
        docker = entry.get("docker", "docker")
        process = subprocess.run(
            [docker, "image", "inspect", "--format", "{{.Id}}", entry["image"]],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            universal_newlines=True)
        if process.returncode != 0:
            return "absent", "image {} not present locally".format(entry["image"])
        resolved = process.stdout.strip()
        if resolved != entry["image_id"]:
            return "fail", "image drift: {} != {}".format(
                resolved, entry["image_id"])
        return "ok", "{} {}".format(entry["image"], resolved[:19])
    return "fail", "unknown toolchain kind {!r}".format(kind)


def emit_json(payload, destination):
    if not destination:
        return
    text = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    if destination == "-":
        sys.stdout.write(text)
        return
    Path(destination).write_text(text, encoding="utf-8")


# ----------------------------------------------- V3-ABI-01: execution roles
#
# The V3 packaging contract models every executable in a package as a *role*.
# nxrelease models the same idea in validate_execution_role_elfs(); its
# "game" role is this module's "guest" (ROLE_ALIASES keeps the two vocabularies
# convertible), its "extractor"/"splash"/"helpers" map one to one.  The rules
# shared by both sides:
#
#   * a role never inherits the game (or host) ABI implicitly -- each role
#     carries its own declared ABI, and validation is per role;
#   * a mixed closure (AArch64 host + ARMHF guest + a splash/helper of yet
#     another ABI) is valid exactly when every role's own closure is
#     self-consistent;
#   * nothing is assumed by custom: only sonames the caller declares (shipped
#     physical set or an explicit firmware contract) resolve a DT_NEEDED.

EXECUTION_ROLES = ("guest", "loader", "extractor", "splash", "adapter", "helper")

# nxrelease calls the guest "game"; accept it on input, normalize on output.
ROLE_ALIASES = {"game": "guest"}

# Per-architecture ELF facts, identical to policy-v1.json "architectures" and
# to nxrelease's ARCH_CLASSES/LINUX_INTERPRETERS.  Kept as a module constant so
# role validation works on synthetic role descriptors without a policy file.
ROLE_ARCH_SPECS = {
    "armv7": {"elf_class": "ELF32", "interpreter": "/lib/ld-linux-armhf.so.3"},
    "aarch64": {"elf_class": "ELF64",
                "interpreter": "/lib/ld-linux-aarch64.so.1"},
}


def normalize_execution_role(role):
    """Return the canonical role name or fail on an unknown one."""
    canonical = ROLE_ALIASES.get(role, role)
    if canonical not in EXECUTION_ROLES:
        fail("unknown execution role {!r}; valid roles: {}".format(
            role, ", ".join(EXECUTION_ROLES)))
    return canonical


def classify_execution_role(path_or_name, declared_role=None):
    """Validate/derive the execution role of one executable.

    The declaration is authoritative: when ``declared_role`` is given it is
    validated against EXECUTION_ROLES and against the documented naming
    conventions, never guessed around.  Without a declaration only the
    documented conventions apply:

      * ``nxsplash*``      -> splash
      * ``nxextract-ui*``  -> extractor
      * ``*-nextos``       -> loader or guest, *per declaration only* (the
                              name alone cannot pick one, so it fails closed)

    Anything else without a declaration fails closed.
    """
    name = os.path.basename(str(path_or_name))
    if not name:
        fail("empty executable name cannot carry an execution role")
    implied = None
    if name.startswith("nxsplash"):
        implied = "splash"
    elif name.startswith("nxextract-ui"):
        implied = "extractor"

    if declared_role is not None:
        role = normalize_execution_role(declared_role)
        if implied is not None and role != implied:
            fail("declared role {!r} contradicts the documented convention "
                 "for {!r} (which names a {})".format(role, name, implied))
        if name.endswith("-nextos") and role not in ("loader", "guest"):
            fail("{!r} follows the *-nextos convention (loader or guest); "
                 "declared role {!r} is neither".format(name, role))
        return role

    if implied is not None:
        return implied
    if name.endswith("-nextos"):
        fail("{!r} follows the *-nextos convention, which names a loader OR "
             "a guest; the name alone cannot decide -- pass declared_role"
             .format(name))
    fail("cannot classify the execution role of {!r}: no documented naming "
         "convention matches and no role was declared".format(name))


def role_finding(level, role, check, message):
    return {"level": level, "role": role, "check": check, "message": message}


def validate_role_abi(role, elf_info, host_abi):
    """Validate one role's ABI facts *in isolation*; return findings.

    ``elf_info`` is a mapping with (at least) ``architecture``; ``class``,
    ``interpreter`` (or ``pt_interp``) and ``needed`` are checked when
    present.  ``host_abi`` is the packaging host's architecture and exists
    only for the error message: a role that declares no ABI is an error --
    it is NEVER silently given the host (or game) ABI.  A role whose ABI
    differs from ``host_abi`` is NOT an error: mixed closures are valid when
    each role is self-consistent.
    """
    role = normalize_execution_role(role)
    out = []
    architecture = elf_info.get("architecture")
    if architecture is None:
        out.append(role_finding(
            "error", role, "role-abi",
            "role {!r} declares no architecture; a {} never inherits the "
            "host ABI {!r} implicitly".format(role, role, host_abi)))
        return out
    interpreter = elf_info.get("interpreter", elf_info.get("pt_interp"))
    spec = ROLE_ARCH_SPECS.get(architecture)
    if spec is not None:
        elf_class = elf_info.get("class")
        if elf_class is not None and elf_class != spec["elf_class"]:
            out.append(role_finding(
                "error", role, "role-elf-class",
                "role {!r} is {} but its ELF class is {} (expected {})"
                .format(role, architecture, elf_class, spec["elf_class"])))
        if interpreter not in (None, "none") and \
                interpreter != spec["interpreter"]:
            out.append(role_finding(
                "error", role, "role-interpreter",
                "role {!r} ({}) has PT_INTERP {} (expected {})".format(
                    role, architecture, interpreter, spec["interpreter"])))
    else:
        # A third ABI is allowed, but nothing about it is assumed: the caller
        # must declare its ELF class and interpreter explicitly so the role
        # is self-consistent by declaration, not by custom.
        if not elf_info.get("class") or interpreter in (None, ""):
            out.append(role_finding(
                "error", role, "role-abi-declaration",
                "role {!r} uses the undocumented ABI {!r}; declare its ELF "
                "class and interpreter explicitly (nothing is assumed)"
                .format(role, architecture)))
    return out


def audit_mixed_closure(roles, physical_libs, firmware_contract=None):
    """Audit a (possibly mixed-ABI) closure role by role; return findings.

    ``roles``            role name -> {"abi", "interpreter", "needed": [...]}
    ``physical_libs``    the shipped physical set (sonames or basenames)
    ``firmware_contract`` what the caller *declares* the firmware provides:
                         a set of sonames, or a dict keyed by role name and/or
                         ABI mapping to such sets.  No soname (libudev
                         included) is ever assumed by custom.

    Every DT_NEEDED of every role must resolve inside the shipped physical
    set or the declared firmware contract for that role.  A role without an
    interpreter declaration is a named failure ("none" declares a static
    executable explicitly and is accepted).  Roles of different ABIs are
    valid together as long as each role's own closure is self-consistent.
    """
    findings = []
    physical = {os.path.basename(item) for item in physical_libs}
    physical |= set(physical_libs)

    def contract_for(role_name, abi):
        if firmware_contract is None:
            return frozenset()
        if isinstance(firmware_contract, dict):
            merged = set()
            for key in (role_name, abi):
                merged.update(firmware_contract.get(key) or ())
            return merged
        return set(firmware_contract)

    for role_name in sorted(roles):
        descriptor = roles[role_name]
        canonical = ROLE_ALIASES.get(role_name, role_name)
        if canonical not in EXECUTION_ROLES:
            findings.append(role_finding(
                "error", role_name, "role-name",
                "unknown execution role {!r}".format(role_name)))
            continue
        abi = descriptor.get("abi", descriptor.get("architecture"))
        if not abi:
            findings.append(role_finding(
                "error", canonical, "role-abi",
                "role {!r} declares no ABI; roles never inherit one"
                .format(canonical)))
        interpreter = descriptor.get("interpreter")
        if interpreter in (None, ""):
            findings.append(role_finding(
                "error", canonical, "role-interpreter",
                "role {!r} declares no interpreter (declare the dynamic "
                "linker path, or 'none' for a static executable)"
                .format(canonical)))
        contract = contract_for(canonical, abi)
        for soname in descriptor.get("needed", ()):
            if soname in physical or soname in contract:
                continue
            findings.append(role_finding(
                "error", canonical, "closure-unresolved",
                "role {!r} ({}) needs {} which is neither shipped nor in "
                "the declared firmware contract".format(
                    canonical, abi, soname)))
    return findings


# ------------------------------------------- V3-ABI-01: GNU ld script input
#
# A GNU ld script (GROUP/INPUT/OUTPUT_FORMAT, e.g. the glibc "libc.so" of
# every sysroot) is a legitimate linker input, not a broken ELF.  The
# classifier below names it as such so no caller ever reports "invalid ELF"
# for one, and resolve_linker_script() expands it fail-closed.

LD_SCRIPT_COMMAND_RE = re.compile(
    r"(?:^|[\s)])(GROUP|INPUT|OUTPUT_FORMAT)\s*\(")
LD_SCRIPT_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
LD_SCRIPT_MAX_DEPTH = 4
LD_SCRIPT_FORBIDDEN_CHARS = "`$;|&<>"


def _strip_ld_comments(text):
    return LD_SCRIPT_COMMENT_RE.sub(" ", text)


def classify_linker_input(path):
    """Classify one linker input: elf | linker-script | text | symlink | invalid."""
    target = str(path)
    if os.path.islink(target):
        return "symlink"
    try:
        with open(target, "rb") as handle:
            head = handle.read(4096)
    except OSError:
        return "invalid"
    if head.startswith(b"\x7fELF"):
        # Enough e_ident to be a plausible ELF; a chopped-off magic is not.
        if len(head) >= 16 and head[4] in (1, 2) and head[5] in (1, 2):
            return "elf"
        return "invalid"
    if not head:
        return "invalid"
    try:
        text = head.decode("utf-8")
    except UnicodeDecodeError:
        return "invalid"
    if LD_SCRIPT_COMMAND_RE.search(_strip_ld_comments(text)):
        return "linker-script"
    if any(ord(char) < 32 and char not in "\t\n\r\f" for char in text):
        return "invalid"
    return "text"


def require_elf(path):
    """Fail with a *named* reason when a linker input is not an ELF.

    The whole point: a GNU ld script must never be reported as a broken ELF.
    """
    kind = classify_linker_input(path)
    if kind == "elf":
        return kind
    if kind == "linker-script":
        fail("{} is a GNU ld linker script (GROUP/INPUT), not an ELF; "
             "expand it with resolve_linker_script() against the sysroot "
             "search paths".format(path))
    if kind == "symlink":
        fail("{} is a symlink; classify/resolve its target explicitly"
             .format(path))
    if kind == "text":
        fail("{} is plain text, not an ELF and not a GNU ld script"
             .format(path))
    fail("{} has a truncated or unrecognizable header; it is not a "
         "loadable ELF".format(path))


def _ld_script_members(text, path):
    """Return [(member, as_needed)] in order; fail on malformed grammar."""
    tokens = re.findall(r"[()]|[^\s(),]+", text)
    members = []
    stack = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token in ("GROUP", "INPUT", "OUTPUT_FORMAT", "AS_NEEDED"):
            if index + 1 >= len(tokens) or tokens[index + 1] != "(":
                fail("malformed ld script {}: {} without '('".format(
                    path, token))
            if token == "AS_NEEDED" and not (
                    stack and stack[-1] in ("GROUP", "INPUT", "AS_NEEDED")):
                fail("malformed ld script {}: AS_NEEDED outside GROUP/INPUT"
                     .format(path))
            stack.append(token)
            index += 2
            continue
        if token == ")":
            if not stack:
                fail("malformed ld script {}: unbalanced ')'".format(path))
            stack.pop()
            index += 1
            continue
        if token == "(":
            fail("malformed ld script {}: unexpected '('".format(path))
        if not stack:
            fail("malformed ld script {}: token {!r} outside any command"
                 .format(path, token))
        if stack[-1] != "OUTPUT_FORMAT":
            members.append((token, "AS_NEEDED" in stack))
        index += 1
    if stack:
        fail("malformed ld script {}: unclosed {}".format(path, stack[-1]))
    return members


def _resolve_ld_member(member, roots, script_path):
    """Resolve one member against the sysroot-style roots; fail closed."""
    candidates = []
    if member.startswith("/"):
        # Sysroot semantics: an absolute member is re-rooted inside each
        # search root; it must never escape them.
        relative = member.lstrip("/")
        candidates = [(root, root / relative) for root in roots]
    else:
        candidates = [(root, root / member) for root in roots]
    for root, candidate in candidates:
        if not candidate.exists() or candidate.is_dir():
            continue
        resolved = candidate.resolve()
        root_resolved = root.resolve()
        try:
            resolved.relative_to(root_resolved)
        except ValueError:
            fail("ld script {} member {!r} escapes the search root {} "
                 "(resolves to {})".format(
                     script_path, member, root, resolved))
        return resolved, str(root)
    fail("ld script {} member {!r} does not resolve in any search path "
         "({})".format(script_path, member,
                       ", ".join(str(root) for root in roots) or "none"))


def resolve_linker_script(path, search_paths, _depth=0):
    """Expand a GNU ld script into its ordered member closure, fail-closed.

    Returns a list of entries, one per resolved leaf member, in script
    order: {"member", "path", "origin", "as_needed", "via"}.  Nested ld
    scripts recurse up to LD_SCRIPT_MAX_DEPTH; unresolvable members,
    traversal outside the search roots and shell metacharacters/backticks
    in the script all fail closed.
    """
    if _depth > LD_SCRIPT_MAX_DEPTH:
        fail("ld script recursion deeper than {} at {}; refusing (recursion "
             "bomb or cycle)".format(LD_SCRIPT_MAX_DEPTH, path))
    kind = classify_linker_input(path)
    if kind != "linker-script":
        fail("{} is not a GNU ld linker script (classified {})".format(
            path, kind))
    try:
        with open(str(path), "r", encoding="utf-8") as stream:
            text = stream.read()
    except (OSError, UnicodeDecodeError) as error:
        fail("cannot read ld script {}: {}".format(path, error))
    bad = sorted({char for char in text
                  if char in LD_SCRIPT_FORBIDDEN_CHARS})
    if bad:
        fail("ld script {} contains forbidden shell metacharacter(s) {}; "
             "refusing to resolve it".format(path, " ".join(bad)))
    roots = [Path(str(root)) for root in search_paths]
    if not roots:
        fail("resolve_linker_script needs at least one search path")
    closure = []
    for member, as_needed in _ld_script_members(
            _strip_ld_comments(text), path):
        resolved, origin = _resolve_ld_member(member, roots, path)
        if classify_linker_input(resolved) == "linker-script":
            nested = resolve_linker_script(
                resolved, search_paths, _depth + 1)
            for entry in nested:
                item = dict(entry)
                item["as_needed"] = item["as_needed"] or as_needed
                closure.append(item)
            continue
        closure.append({
            "member": member,
            "path": str(resolved),
            "origin": origin,
            "as_needed": as_needed,
            "via": str(path),
        })
    if not closure:
        fail("ld script {} resolves to an empty closure".format(path))
    return closure


# ---------------------------------------------------------------------- entry


def build_parser():
    parser = argparse.ArgumentParser(
        prog=TOOL_NAME,
        description="M17 ABI/toolchain gate (host-side, never executes an ELF)",
    )
    parser.add_argument("--version", action="version",
                        version="{} {}".format(TOOL_NAME, TOOL_VERSION))
    subparsers = parser.add_subparsers(dest="command")

    def add_common(target):
        target.add_argument("targets", nargs="+",
                            help="files or directories to inspect")
        target.add_argument("--policy", default=str(DEFAULT_POLICY),
                            help="policy JSON (default: %(default)s)")
        target.add_argument("--json", default=None,
                            help="write the machine-readable report here "
                                 "('-' for stdout)")
        target.add_argument("--follow-symlinks", action="store_true",
                            help="also inspect symlinked ELFs")

    inventory = subparsers.add_parser(
        "inventory", help="emit the machine-readable ELF inventory")
    add_common(inventory)
    inventory.set_defaults(handler=command_inventory)

    audit = subparsers.add_parser(
        "audit", help="apply the public universal policy")
    add_common(audit)
    audit.add_argument("--profile", default="universal-low-glibc",
                       help="build profile (default: %(default)s)")
    audit.add_argument("--sdl-floor", default=None,
                       help="override the declared SDL floor for this run")
    audit.add_argument("--strict", action="store_true",
                       help="treat warnings as failures")
    audit.add_argument("--errors-only", action="store_true",
                       help="print errors only")
    audit.add_argument("--embed-inventory", action="store_true",
                       help="embed the full inventory in the JSON report")
    audit.add_argument("--verbose", action="store_true",
                       help="also print info findings")
    audit.add_argument("--quiet", action="store_true",
                       help="suppress the human-readable report")
    audit.set_defaults(handler=command_audit)

    sdl_table = subparsers.add_parser(
        "sdl-table", help="regenerate the SDL symbol floor table")
    sdl_table.add_argument("--headers", required=True,
                           help="directory holding the SDL2 headers")
    sdl_table.add_argument("--out", default=None, help="output TSV path")
    sdl_table.set_defaults(handler=command_sdl_table)

    toolchain = subparsers.add_parser(
        "toolchain", help="verify the pinned toolchains")
    toolchain.add_argument("--pin", default=str(DEFAULT_PIN),
                           help="toolchain pin JSON (default: %(default)s)")
    toolchain.add_argument("--json", default=None, help="write a JSON report")
    toolchain.add_argument("--quiet", action="store_true")
    toolchain.set_defaults(handler=command_toolchain)

    provenance = subparsers.add_parser(
        "provenance", help="emit the .note.nx.toolchain stamp for a build")
    provenance.add_argument("--toolchain", required=True,
                            help="toolchain key from the pin file")
    provenance.add_argument("--pin", default=str(DEFAULT_PIN))
    provenance.add_argument("--profile", default="universal-low-glibc")
    provenance.add_argument("--source-date-epoch", default=None)
    provenance.add_argument("--out", default=None)
    provenance.set_defaults(handler=command_provenance)
    return parser


def main(argv=None):
    parser = build_parser()
    arguments = parser.parse_args(argv)
    if not getattr(arguments, "handler", None):
        parser.print_help()
        return 2
    try:
        return arguments.handler(arguments)
    except AbiError as error:
        print("{}: {}".format(TOOL_NAME, error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
