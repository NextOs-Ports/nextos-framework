#!/usr/bin/env python3
"""Reproduce the M11 Android guest import inventory without loading guest code.

This tool only hashes regular files and invokes GNU readelf without a shell.  It
does not map an ELF, resolve an import, run an initializer, call JNI_OnLoad, or
contact a device or the network.  The checked-in JSON is both the allowlisted
input manifest and the expected deterministic result.  Execution is refused
unless the canonical runner has sealed a private user/PID/mount namespace,
because timeout cleanup may signal only the direct readelf child there.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import selectors
import shutil
import subprocess
import sys
import time
from typing import Any, Iterable


DEFAULT_MANIFEST = "framework/nxandroid/references/m11-android-guests-v1.json"
MAX_READELF_OUTPUT = 64 * 1024 * 1024
READELF_TIMEOUT_SECONDS = 30

EXPECTED_PORT_MODULES = {
    "bully2": ("bully2-libgame",),
    "sonic4ep2": ("sonic4ep2-libfox",),
    "horizonchase": (
        "horizonchase-libunity",
        "horizonchase-libil2cpp",
        "horizonchase-libmain",
    ),
    "kotor": (
        "kotor-lzma",
        "kotor-miniz",
        "kotor-freetype",
        "kotor-fmod",
        "kotor-hidapi",
        "kotor-android-port",
        "kotor-main",
    ),
    "tasm2_127": ("tasm2_127-generator", "tasm2_127-main"),
}

EXPECTED_ABIS = {
    "bully2-libgame": ("ELF64", "AArch64", 0),
    "sonic4ep2-libfox": ("ELF64", "AArch64", 0),
    "horizonchase-libunity": ("ELF64", "AArch64", 0),
    "horizonchase-libil2cpp": ("ELF64", "AArch64", 0),
    "horizonchase-libmain": ("ELF64", "AArch64", 0),
    "kotor-lzma": ("ELF32", "ARM", 0x5000200),
    "kotor-miniz": ("ELF32", "ARM", 0x5000200),
    "kotor-freetype": ("ELF32", "ARM", 0x5000200),
    "kotor-fmod": ("ELF32", "ARM", 0x5000200),
    "kotor-hidapi": ("ELF32", "ARM", 0x5000200),
    "kotor-android-port": ("ELF32", "ARM", 0x5000200),
    "kotor-main": ("ELF32", "ARM", 0x5000200),
    "tasm2_127-generator": ("ELF32", "ARM", 0x5000000),
    "tasm2_127-main": ("ELF32", "ARM", 0x5000000),
}

EXPECTED_SAFETY_KEYS = frozenset(
    {
        "guest_code_executed",
        "guest_mapped",
        "imports_resolved",
        "guest_initializers_executed",
        "guest_jni_onload_executed",
        "guest_lifecycle_executed",
        "device_access",
        "network_access",
        "guest_files_copied",
        "legacy_tasm2_used",
    }
)

JNI_PREFIXES = (
    "Android_JNI_",
    "JNI_",
    "Java_",
    "SDL_Android",
)

ANDROID_NDK_PREFIXES = (
    "AAsset",
    "AConfiguration",
    "AChoreographer",
    "AHardwareBuffer",
    "AImage",
    "AInput",
    "AKeyEvent",
    "ALooper",
    "AMedia",
    "AMotionEvent",
    "ANative",
    "ASensor",
    "ATrace",
    "SL_IID_",
    "__android_log_",
    "android_",
    "slCreateEngine",
)

BIONIC_PREFIXES = (
    "__aeabi_",
    "__atomic_",
    "__ctype_",
    "__errno",
    "__fread_",
    "__fwrite_",
    "__get_tls",
    "__mem",
    "__open_",
    "__poll_",
    "__read_",
    "__recvfrom_",
    "__register_atfork",
    "__sF",
    "__snprintf_",
    "__sprintf_",
    "__stack_chk_",
    "__str",
    "__system_property_",
    "__vsnprintf_",
    "__write_",
    "dladdr",
    "dlclose",
    "dlerror",
    "dl_iterate_phdr",
    "dlopen",
    "dlsym",
    "fstat",
    "lstat",
    "pthread_",
    "sem_",
    "sigaction",
    "sigaddset",
    "sigdelset",
    "sigemptyset",
    "sigfillset",
    "sigismember",
    "sigprocmask",
    "stat",
)

# Conservative POSIX/libc/libm names whose Android representation or calling
# convention is relevant to an adapter.  The list is an audit label, not a
# resolver allowlist; names outside it remain explicit in other_dependency.
BIONIC_EXACT = frozenset(
    """
    _exit _Exit abort abs access acos acosf alarm aligned_alloc asctime asin
    asinf assert atan atan2 atan2f atanf atexit atof atoi atol atoll basename
    bsearch calloc ceil ceilf chdir chmod chown clearerr clock clock_gettime
    close closedir connect cos cosf cosh crypt ctime difftime dirfd dirname
    dup dup2 environ erf erfc execv execve execvp exit exp exp2 exp2f expf
    fabs fabsf fclose fcntl fdopen feof ferror fflush fgetc fgetpos fgets
    fileno floor floorf fmod fmodf fopen fork fprintf fputc fputs fread free
    freopen frexp fscanf fseek fseeko fsetpos ftell ftello ftruncate fwrite
    getauxval getcwd getegid geteuid getgid getpid getppid getpwnam getpwuid
    getrlimit gettimeofday getuid gmtime hypot hypotf ioctl isalnum isalpha
    isatty isdigit isgraph islower isprint isspace isupper kill ldexp link
    localeconv localtime log log10 log10f log2 log2f logf longjmp lseek
    malloc malloc_usable_size mbrtowc memalign memchr memcmp memcpy memmove
    memset mkdir mmap modf mprotect munmap nanosleep nearbyint nearbyintf
    open openat opendir perror pipe poll posix_memalign pow powf prctl printf
    putchar puts qsort raise rand random read readdir readlink realloc recv
    recvfrom remove rename rewind rmdir round roundf scandir sched_yield select
    send sendto setbuf setenv setjmp setlocale setrlimit setsockopt setvbuf
    shutdown sin sincos sincosf sinf sinh snprintf socket sprintf sqrt sqrtf
    srand sscanf stderr stdin stdout strchr strcmp strcpy strcspn strdup
    strerror strerror_r strftime strlen strncasecmp strncmp strncpy strndup
    strnlen strpbrk strrchr strspn strstr strtod strtof strtok strtok_r strtol
    strtold strtoll strtoul strtoull symlink sync sysconf system tan tanf
    tcgetattr tcsetattr time times tmpfile trunc truncf truncate tzset umask
    uname unlink unsetenv usleep vasprintf vfprintf vfscanf vprintf vsnprintf
    vsprintf vsscanf waitpid wcscmp wcslen wcsncmp wcsncpy wcrtomb write
    __assert2 __cxa_atexit __cxa_finalize
    """.split()
)

HEADER_FIELDS = {
    "Class": "class",
    "Data": "data",
    "OS/ABI": "osabi",
    "ABI Version": "abi_version",
    "Type": "type",
    "Machine": "machine",
    "Flags": "flags",
}


class InventoryError(RuntimeError):
    pass


class IsolationUnavailable(InventoryError):
    pass


def require_sealed_namespace() -> None:
    if os.environ.get("NXBOOTSTRAP_TEST_PRIVATE_PID_NS") != "1":
        raise IsolationUnavailable("sealed private namespace marker is absent")
    namespaces = (
        ("PID", "pid"),
        ("USER", "user"),
        ("MOUNT", "mnt"),
    )
    for environment_name, proc_name in namespaces:
        fd_text = os.environ.get(
            f"NXBOOTSTRAP_TEST_HOST_{environment_name}_NS_FD", ""
        )
        expected = os.environ.get(
            f"NXBOOTSTRAP_TEST_HOST_{environment_name}_NS", ""
        )
        if not fd_text.isdecimal() or not expected:
            raise IsolationUnavailable(
                f"sealed host {proc_name} namespace descriptor is absent"
            )
        sealed = os.readlink(f"/proc/self/fd/{int(fd_text)}")
        current = os.readlink(f"/proc/self/ns/{proc_name}")
        if sealed != expected or current == sealed:
            raise IsolationUnavailable(
                f"private {proc_name} namespace identity is not sealed"
            )


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def checked_repo_path(repo_root: Path, relative: str) -> Path:
    posix = PurePosixPath(relative)
    if not relative or posix.is_absolute() or ".." in posix.parts:
        raise InventoryError(f"unsafe non-relative path: {relative!r}")
    if posix.parts[:2] == ("ports", "tasm2"):
        raise InventoryError("legacy ports/tasm2 is forbidden; use ports/asm2_127")
    unresolved = repo_root
    for part in posix.parts:
        unresolved /= part
        if unresolved.is_symlink():
            raise InventoryError(f"symlink is forbidden in evidence path: {relative!r}")
    candidate = unresolved.resolve()
    try:
        candidate.relative_to(repo_root)
    except ValueError as exc:
        raise InventoryError(f"path escapes repository: {relative!r}") from exc
    if not candidate.is_file():
        raise InventoryError(f"missing regular file: {relative}")
    return candidate


def run_readelf(readelf: str, args: Iterable[str], elf: Path) -> str:
    command = [readelf, "-W", *args, os.fspath(elf)]
    clean_env = {"LC_ALL": "C", "LANG": "C", "PATH": os.defpath}
    process = subprocess.Popen(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=clean_env,
    )
    if process.stdout is None or process.stderr is None:
        process.kill()
        process.wait()
        raise InventoryError("failed to create bounded readelf pipes")

    output = bytearray()
    errors = bytearray()
    streams = {process.stdout: output, process.stderr: errors}
    selector = selectors.DefaultSelector()
    for stream in streams:
        os.set_blocking(stream.fileno(), False)
        selector.register(stream, selectors.EVENT_READ)
    deadline = time.monotonic() + READELF_TIMEOUT_SECONDS
    failure: str | None = None
    try:
        while selector.get_map():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                failure = f"readelf timed out for {elf.name}"
                break
            events = selector.select(remaining)
            if not events:
                failure = f"readelf timed out for {elf.name}"
                break
            for key, _mask in events:
                stream = key.fileobj
                chunk = os.read(stream.fileno(), 65536)
                if not chunk:
                    selector.unregister(stream)
                    continue
                destination = streams[stream]
                if len(destination) + len(chunk) > MAX_READELF_OUTPUT:
                    failure = f"readelf output limit exceeded for {elf.name}"
                    break
                destination.extend(chunk)
            if failure:
                break
    finally:
        selector.close()

    if failure:
        process.terminate()
        try:
            process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        raise InventoryError(failure)
    return_code = process.wait()
    if return_code != 0:
        detail = errors.decode("utf-8", "replace")[:4096].strip()
        raise InventoryError(f"readelf failed for {elf.name}: {detail}")
    return output.decode("utf-8", "strict")


def parse_header(output: str) -> dict[str, Any]:
    values: dict[str, str] = {}
    for line in output.splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        key = key.strip()
        if key in HEADER_FIELDS:
            values[HEADER_FIELDS[key]] = value.strip()
    missing = sorted(set(HEADER_FIELDS.values()) - set(values))
    if missing:
        raise InventoryError(f"ELF header fields missing: {', '.join(missing)}")
    values["abi_version"] = int(values["abi_version"], 0)
    flags_match = re.match(r"0x([0-9a-fA-F]+)", values["flags"])
    if not flags_match:
        raise InventoryError(f"cannot parse ELF flags: {values['flags']!r}")
    values["flags_value"] = int(flags_match.group(1), 16)
    return values


def parse_program_interpreters(output: str) -> list[str]:
    return re.findall(r"Requesting program interpreter:\s*([^\]]+)\]", output)


def parse_dynamic(output: str, pointer_size: int) -> dict[str, Any]:
    needed = re.findall(r"\(NEEDED\).*?Shared library: \[([^\]]+)\]", output)
    sizes = [int(value) for value in re.findall(r"\(INIT_ARRAYSZ\)\s+(\d+) \(bytes\)", output)]
    dt_init_values = re.findall(r"\(INIT\)\s+(0x[0-9a-fA-F]+)", output)
    init_array_values = re.findall(r"\(INIT_ARRAY\)\s+(0x[0-9a-fA-F]+)", output)
    if len(sizes) > 1:
        raise InventoryError("multiple DT_INIT_ARRAYSZ entries")
    if len(dt_init_values) > 1:
        raise InventoryError("multiple DT_INIT entries")
    if len(init_array_values) > 1:
        raise InventoryError("multiple DT_INIT_ARRAY entries")
    byte_count = sizes[0] if sizes else 0
    if byte_count % pointer_size:
        raise InventoryError("DT_INIT_ARRAYSZ is not pointer-aligned")
    if byte_count and not init_array_values:
        raise InventoryError("DT_INIT_ARRAYSZ exists without DT_INIT_ARRAY")
    return {
        "needed": needed,
        "dt_init": {
            "present": bool(dt_init_values),
            "value": dt_init_values[0].lower() if dt_init_values else None,
        },
        "init_array": {
            "present": bool(init_array_values),
            "value": init_array_values[0].lower() if init_array_values else None,
            "bytes": byte_count,
            "entry_size": pointer_size,
            "entries": byte_count // pointer_size,
        },
    }


def split_symbol_version(raw_name: str) -> tuple[str, str | None]:
    match = re.match(r"^([^@]+)@{1,2}([^@]+)$", raw_name)
    if not match:
        return raw_name, None
    return match.group(1), match.group(2)


def symbol_category(name: str, version: str | None) -> str:
    if name.startswith(JNI_PREFIXES):
        return "jni_boundary"
    if name.startswith(ANDROID_NDK_PREFIXES):
        return "android_ndk"
    if version in {"LIBC", "LIBM", "LIBDL"}:
        return "bionic_abi"
    if name.startswith(BIONIC_PREFIXES) or name in BIONIC_EXACT:
        return "bionic_abi"
    return "other_dependency"


def parse_dynamic_symbols(output: str) -> dict[str, Any]:
    count_match = re.search(r"Symbol table '\.dynsym' contains (\d+) entries", output)
    if not count_match:
        raise InventoryError("dynamic symbol table count missing")

    undefined: dict[str, dict[str, set[str]]] = {}
    jni_exports: dict[str, dict[str, str]] = {}
    for line in output.splitlines():
        columns = line.split(None, 7)
        if len(columns) < 8 or not columns[0].endswith(":"):
            continue
        _, _value, _size, sym_type, bind, visibility, ndx, raw_tail = columns
        raw_name = raw_tail.split(None, 1)[0]
        if not raw_name:
            continue
        name, version = split_symbol_version(raw_name)
        if ndx == "UND":
            record = undefined.setdefault(
                name,
                {"bindings": set(), "types": set(), "versions": set()},
            )
            record["bindings"].add(bind)
            record["types"].add(sym_type)
            if version:
                record["versions"].add(version)
            continue
        if name == "JNI_OnLoad" or name.startswith("Java_"):
            jni_exports[name] = {
                "binding": bind,
                "type": sym_type,
                "visibility": visibility,
            }

    categories: dict[str, list[str]] = {
        "bionic_abi": [],
        "android_ndk": [],
        "jni_boundary": [],
        "other_dependency": [],
    }
    weak_names: list[str] = []
    versioned: dict[str, list[str]] = {}
    binding_counts: dict[str, int] = {}
    type_counts: dict[str, int] = {}
    for name in sorted(undefined):
        record = undefined[name]
        bindings = sorted(record["bindings"])
        types = sorted(record["types"])
        versions = sorted(record["versions"])
        category = symbol_category(name, versions[0] if len(versions) == 1 else None)
        categories[category].append(name)
        if bindings == ["WEAK"]:
            weak_names.append(name)
        for binding in bindings:
            binding_counts[binding] = binding_counts.get(binding, 0) + 1
        for sym_type in types:
            type_counts[sym_type] = type_counts.get(sym_type, 0) + 1
        if versions:
            versioned[name] = versions

    java_names = sorted(name for name in jni_exports if name.startswith("Java_"))
    onload = jni_exports.get("JNI_OnLoad")
    return {
        "dynamic_symbol_table_entries": int(count_match.group(1)),
        "undefined": {
            "unique_count": len(undefined),
            "category_counts": {key: len(value) for key, value in categories.items()},
            "by_category": categories,
            "binding_name_counts": dict(sorted(binding_counts.items())),
            "type_name_counts": dict(sorted(type_counts.items())),
            "weak_only_names": weak_names,
            "version_requirements": versioned,
        },
        "jni_exports": {
            "jni_onload_present": onload is not None,
            "jni_onload": onload,
            "java_count": len(java_names),
            "java_names": java_names,
            "total_jni_boundary_exports": len(java_names) + int(onload is not None),
        },
    }


def inventory_elf(readelf: str, elf: Path) -> dict[str, Any]:
    header = parse_header(run_readelf(readelf, ["-h"], elf))
    pointer_size = 8 if header["class"] == "ELF64" else 4 if header["class"] == "ELF32" else 0
    if not pointer_size:
        raise InventoryError(f"unsupported ELF class: {header['class']}")
    program_headers = run_readelf(readelf, ["-l"], elf)
    dynamic = parse_dynamic(run_readelf(readelf, ["-d"], elf), pointer_size)
    symbols = parse_dynamic_symbols(run_readelf(readelf, ["--dyn-syms"], elf))
    return {
        "file_size": elf.stat().st_size,
        "header": header,
        "pt_interp": parse_program_interpreters(program_headers),
        **dynamic,
        **symbols,
    }


def walk_modules(manifest: dict[str, Any]) -> Iterable[dict[str, Any]]:
    for port in manifest.get("ports", []):
        for module in port.get("modules", []):
            yield module


def classify_guest_presence(present: Iterable[bool]) -> str:
    """Classify the all-or-nothing owner-data set without weakening hashes."""
    states = tuple(present)
    expected = len(EXPECTED_ABIS)
    if len(states) != expected:
        raise InventoryError(
            f"guest presence count changed: expected {expected}, got {len(states)}"
        )
    count = sum(1 for state in states if state)
    if count == expected:
        return "complete"
    if count == 0:
        return "absent"
    raise InventoryError(
        f"partial owner guest set is forbidden: present={count} "
        f"missing={expected - count}"
    )


def optional_regular_repo_file(repo_root: Path, relative: str) -> bool:
    """Return presence while rejecting unsafe or non-regular partial inputs."""
    posix = PurePosixPath(relative)
    if not relative or posix.is_absolute() or ".." in posix.parts:
        raise InventoryError(f"unsafe non-relative path: {relative!r}")
    unresolved = repo_root
    for part in posix.parts:
        unresolved /= part
        if unresolved.is_symlink():
            raise InventoryError(
                f"symlink is forbidden in evidence path: {relative!r}"
            )
    candidate = unresolved.resolve()
    try:
        candidate.relative_to(repo_root)
    except ValueError as exc:
        raise InventoryError(f"path escapes repository: {relative!r}") from exc
    if not candidate.exists():
        return False
    if not candidate.is_file():
        raise InventoryError(f"guest evidence path is not regular: {relative}")
    return True


def guest_payload_availability(manifest: dict[str, Any], repo_root: Path) -> str:
    modules = list(walk_modules(manifest))
    return classify_guest_presence(
        optional_regular_repo_file(repo_root, module["path"])
        for module in modules
    )


def reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise InventoryError(f"duplicate JSON key: {key!r}")
        result[key] = value
    return result


def load_manifest_strict(text: str) -> dict[str, Any]:
    value = json.loads(text, object_pairs_hook=reject_duplicate_keys)
    if not isinstance(value, dict):
        raise InventoryError("manifest root must be an object")
    return value


def validate_manifest_contract(manifest: dict[str, Any]) -> None:
    if manifest.get("schema") != "nxandroid-m11-android-guests-v1":
        raise InventoryError("unexpected M11 manifest schema")
    if manifest.get("schema_version") != 1:
        raise InventoryError("unexpected M11 manifest schema_version")
    if manifest.get("milestone") != "M11":
        raise InventoryError("unexpected M11 manifest milestone")
    ports = manifest.get("ports")
    if not isinstance(ports, list):
        raise InventoryError("ports must be an array")
    actual: dict[str, tuple[str, ...]] = {}
    for port in ports:
        if not isinstance(port, dict) or not isinstance(port.get("id"), str):
            raise InventoryError("each port requires a string id")
        if port["id"] in actual:
            raise InventoryError(f"duplicate port id: {port['id']}")
        modules = port.get("modules")
        if not isinstance(modules, list):
            raise InventoryError(f"modules must be an array for {port['id']}")
        module_ids: list[str] = []
        for module in modules:
            if not isinstance(module, dict) or not isinstance(module.get("id"), str):
                raise InventoryError(f"each module requires a string id for {port['id']}")
            module_ids.append(module["id"])
        actual[port["id"]] = tuple(module_ids)
    if actual != EXPECTED_PORT_MODULES:
        raise InventoryError(f"unexpected port/module IDs: {actual!r}")
    safety = manifest.get("safety")
    if not isinstance(safety, dict) or set(safety) != EXPECTED_SAFETY_KEYS:
        raise InventoryError("unexpected M11 safety execution/access flag set")
    if any(value is not False for value in safety.values()):
        raise InventoryError("all M11 safety execution/access flags must be false")


def privacy_check(manifest_text: str) -> None:
    forbidden = [r"/home/", r"/mnt/", r"/tmp/", r"\b(?:\d{1,3}\.){3}\d{1,3}\b"]
    for pattern in forbidden:
        if re.search(pattern, manifest_text):
            raise InventoryError(f"published manifest contains forbidden private pattern: {pattern}")


def update_inventory(
    manifest: dict[str, Any], repo_root: Path, readelf: str
) -> dict[str, Any]:
    validate_manifest_contract(manifest)
    source_paths: set[str] = set()
    for source in manifest.get("source_files", []):
        if source["path"] in source_paths:
            raise InventoryError(f"duplicate source path: {source['path']}")
        source_paths.add(source["path"])
        path = checked_repo_path(repo_root, source["path"])
        actual_hash = sha256_file(path)
        if actual_hash != source["sha256"]:
            raise InventoryError(
                f"source hash mismatch for {source['path']}: {actual_hash}"
            )

    seen_ids: set[str] = set()
    seen_paths: set[str] = set()
    for module in walk_modules(manifest):
        module_id = module["id"]
        relative = module["path"]
        if module_id in seen_ids or relative in seen_paths:
            raise InventoryError(f"duplicate module id/path: {module_id} {relative}")
        seen_ids.add(module_id)
        seen_paths.add(relative)
        path = checked_repo_path(repo_root, relative)
        actual_hash = sha256_file(path)
        if actual_hash != module["sha256"]:
            raise InventoryError(f"guest hash mismatch for {relative}: {actual_hash}")
        module["elf"] = inventory_elf(readelf, path)
        expected_abi = EXPECTED_ABIS[module_id]
        header = module["elf"]["header"]
        actual_abi = (header["class"], header["machine"], header["flags_value"])
        if actual_abi != expected_abi or not header["type"].startswith("DYN"):
            raise InventoryError(
                f"ABI mismatch for {module_id}: {actual_abi!r}, type={header['type']!r}"
            )
    return manifest


def parse_args(argv: list[str]) -> argparse.Namespace:
    script_repo = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=script_repo)
    parser.add_argument("--manifest", default=DEFAULT_MANIFEST)
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument(
        "--emit",
        action="store_true",
        help="emit a canonical manifest with recomputed ELF inventories to stdout",
    )
    parser.add_argument(
        "--require-guests",
        action="store_true",
        help="fail instead of recording an explicit skip when all owner guest files are absent",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    require_sealed_namespace()
    repo_root = args.repo_root.resolve()
    manifest_path = checked_repo_path(repo_root, args.manifest)
    manifest_text = manifest_path.read_text(encoding="utf-8")
    privacy_check(manifest_text)
    manifest = load_manifest_strict(manifest_text)
    validate_manifest_contract(manifest)
    availability = guest_payload_availability(manifest, repo_root)
    if availability == "absent":
        if args.emit or args.require_guests:
            raise InventoryError(
                "all owner guest files are absent but this invocation requires them"
            )
        print(
            "M11 guest inventory SKIP owner_data_absent=all "
            f"modules={len(EXPECTED_ABIS)} partial_sets_fail=1 "
            "guest_code_executed=0 device_access=0 network_access=0"
        )
        return 0
    readelf = shutil.which(args.readelf)
    if not readelf or Path(readelf).name not in {"readelf", "llvm-readelf"}:
        raise InventoryError("--readelf must resolve to readelf or llvm-readelf")
    expected = load_manifest_strict(manifest_text)
    actual = update_inventory(manifest, repo_root, readelf)
    if args.emit:
        sys.stdout.write(json.dumps(actual, indent=2, ensure_ascii=False) + "\n")
        return 0
    if actual != expected:
        print(
            "M11 inventory mismatch; run with --emit and review the hash-pinned diff",
            file=sys.stderr,
        )
        return 1
    module_count = sum(1 for _ in walk_modules(actual))
    print(
        "M11 guest inventory PASS "
        f"modules={module_count} guest_code_executed=0 initializers_executed=0 "
        "jni_onload_executed=0 device_access=0 network_access=0"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except IsolationUnavailable as exc:
        print(f"M11 inventory SKIP: {exc}", file=sys.stderr)
        raise SystemExit(77)
    except (InventoryError, KeyError, json.JSONDecodeError, OSError, UnicodeError) as exc:
        print(f"M11 inventory ERROR: {exc}", file=sys.stderr)
        raise SystemExit(2)
