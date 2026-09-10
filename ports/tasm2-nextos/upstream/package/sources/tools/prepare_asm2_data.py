#!/usr/bin/env python3
"""Prepare validated ASM2 1.2.7d/1.2.8d data inside an NXExtract stage.

NXExtract stages the exact expansion files before this hook runs.  The hook
then identifies one of the audited owner-supplied APK/container profiles,
creates a libzip-readable runtime APK, recovers the byte-exact native game
libraries and builds the two offline shop files.  One known 1.2.8d installer
contains ARMv7 game code only; that profile is deliberately rejected for an
x86 target.  Source files are never executed, modified or deleted.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

from extract_apk import physical_headers, unpack_entry


@dataclass(frozen=True)
class SourceProfile:
    identifier: str
    description: str
    source_size: int
    source_sha256: str
    runtime_mode: str
    runtime_size: int
    runtime_sha256: str
    runtime_member: str | None = None
    has_x86: bool = True


SOURCE_PROFILES = (
    SourceProfile(
        identifier="android-127-recovery",
        description="validated Android 1.2.7d APK (recovery layout)",
        source_size=31_774_872,
        source_sha256=(
            "4188a463432b921dfb767a3ddf316e970655789a7bdf806298757f45071a8c87"
        ),
        runtime_mode="rebuild",
        runtime_size=67_667_020,
        runtime_sha256=(
            "a3f7c790f2ad0a3f62826668f8466f398aa7e15237287c8f693310a232fa44fc"
        ),
    ),
    SourceProfile(
        identifier="android-127-standard",
        description="validated Android 1.2.7d APK (standard ZIP layout)",
        source_size=31_774_872,
        source_sha256=(
            "2878fec3235a91a0487ee0a3ffdbcb5c534e0d052a573941a10489024b2b1868"
        ),
        runtime_mode="copy",
        runtime_size=31_774_872,
        runtime_sha256=(
            "2878fec3235a91a0487ee0a3ffdbcb5c534e0d052a573941a10489024b2b1868"
        ),
    ),
    SourceProfile(
        identifier="android-128-universal",
        description="validated Android 1.2.8d APK (ARMv7 plus x86)",
        source_size=31_519_080,
        source_sha256=(
            "6211d194cb06c6cbb32c2491adef59554eef4d97763a2fbc1e4bbb52d9fcae9b"
        ),
        runtime_mode="copy",
        runtime_size=31_519_080,
        runtime_sha256=(
            "6211d194cb06c6cbb32c2491adef59554eef4d97763a2fbc1e4bbb52d9fcae9b"
        ),
    ),
    SourceProfile(
        identifier="android-128-arm32-installer",
        description="validated Android 1.2.8d ARM32 installer",
        source_size=659_684_888,
        source_sha256=(
            "42d1a3ac86708549fb425b8e36338ece56ea384fb2e30062c7a7da6ca34689e3"
        ),
        runtime_mode="member",
        runtime_member="assets/app.png",
        runtime_size=15_702_180,
        runtime_sha256=(
            "95ffd25a6623e731e80156df82066e4a2b1475466adb337389b93aeed0f1ea71"
        ),
        has_x86=False,
    ),
)

ARM_LIBRARY_ENTRY = "lib/armeabi-v7a/libtasm2.so"
ARM_LIBRARY_SIZE = 20_241_296
ARM_LIBRARY_SHA256 = (
    "d091fe95c56af681f1a06453e9868622935ecbb759c05c340fc16bf8df2ae62e"
)
X86_LIBRARY_ENTRY = "lib/x86/libtasm2.so"
X86_LIBRARY_SIZE = 32_152_072
X86_LIBRARY_SHA256 = (
    "d146d38574c19a105df8a46e523f626c06004c8f71bbeed5cf77e919dbf81a12"
)
X86_UNAVAILABLE_PAYLOAD = b"ASM2_ARM32_ONLY\n"
X86_UNAVAILABLE_SHA256 = (
    "70b25ed02e2a774054be10f9c777458ad0329ad70350fe12022c22c8939281da"
)
PACKAGE = "com.gameloft.android.ANMP.GloftASHM"
OBB_DIRECTORY = Path("gamefiles/Android/obb") / PACKAGE
MAIN_OBB = "main.12032.com.gameloft.android.ANMP.GloftASHM.obb"
PATCH_12438 = "patch.12438.com.gameloft.android.ANMP.GloftASHM.obb"
PATCH_12723 = "patch.12723.com.gameloft.android.ANMP.GloftASHM.obb"

REQUIRED_OUTPUTS = {
    "libtasm2.so": (
        ARM_LIBRARY_SIZE,
        ARM_LIBRARY_SHA256,
    ),
    str(OBB_DIRECTORY / MAIN_OBB): (
        1_236_510_228,
        "276c413051b3349e7738afb23521f972d085a186cb22ab18db230906aab46981",
    ),
    str(OBB_DIRECTORY / PATCH_12723): (
        98_576_965,
        "0faae1e92ab998b8808e3984e4cdafbe732a87c26da58a44ad11c633e643cb80",
    ),
    "assets/IapLocalData_Google.json": (
        9_577,
        "93aecb98da97508352af354f04d42e8b5b4b3b50d993dc9b300002941c77721f",
    ),
    "assets/IapStoreItems_Offline.json": (
        4_717,
        "c857e6f3b2685bf514dc53af345db43059b2e4c44fe3d4add28059e973c029b9",
    ),
}
OPTIONAL_OUTPUTS = {
    str(OBB_DIRECTORY / PATCH_12438): (
        24_099_405,
        "58d9ed565ad67ee7362a2376a74387316535975460110eccf3df7eb3b6503981",
    ),
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            block = stream.read(8 * 1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def validate_file(path: Path, size: int, digest: str, label: str) -> None:
    if path.is_symlink() or not path.is_file():
        raise RuntimeError(f"{label} is missing, linked or not a regular file")
    actual_size = path.stat().st_size
    if actual_size != size:
        raise RuntimeError(
            f"{label} has size {actual_size}, expected {size}"
        )
    actual_digest = sha256_file(path)
    if actual_digest != digest:
        raise RuntimeError(
            f"{label} failed SHA-256 validation ({actual_digest})"
        )


def find_source_package(
    game_dir: Path, target_abi: str
) -> tuple[SourceProfile, Path]:
    profiles_by_size: dict[int, list[SourceProfile]] = {}
    for profile in SOURCE_PROFILES:
        profiles_by_size.setdefault(profile.source_size, []).append(profile)

    candidates: list[tuple[SourceProfile, Path]] = []
    candidate_count = 0
    hashed_count = 0
    accepted_file_count = 0
    skipped_file_count = 0
    skipped_link_count = 0
    read_error_count = 0
    supported_sizes = ",".join(
        str(size) for size in sorted(profiles_by_size)
    )

    print(
        f"ASM2 source scan begin: target_abi={target_abi} "
        f"supported_profiles={len(SOURCE_PROFILES)}",
        flush=True,
    )
    for location, directory in (
        ("gamedata", game_dir / "gamedata"),
        ("port-root", game_dir),
    ):
        try:
            if directory.is_symlink():
                print(
                    f"ASM2 source scan location={location} "
                    "status=skipped-symlink-directory",
                    flush=True,
                )
                continue
            if not directory.is_dir():
                print(
                    f"ASM2 source scan location={location} "
                    "status=missing-directory",
                    flush=True,
                )
                continue
            entries = sorted(
                directory.iterdir(), key=lambda item: item.name.casefold()
            )
        except OSError:
            read_error_count += 1
            print(
                f"ASM2 source scan location={location} "
                "status=directory-read-error",
                flush=True,
            )
            continue

        for entry in entries:
            try:
                if entry.is_symlink():
                    skipped_link_count += 1
                    continue
                if not entry.is_file():
                    continue
                actual_size = entry.stat().st_size
            except OSError:
                read_error_count += 1
                continue

            profiles = profiles_by_size.get(actual_size, [])
            is_apk_name = entry.suffix.casefold() == ".apk"
            if not profiles and not is_apk_name:
                skipped_file_count += 1
                continue

            candidate_count += 1
            candidate_label = f"{location}#{candidate_count}"
            try:
                actual_digest = sha256_file(entry)
            except OSError:
                read_error_count += 1
                print(
                    f"ASM2 source candidate={candidate_label} "
                    f"size={actual_size} status=read-error",
                    flush=True,
                )
                continue
            hashed_count += 1
            print(
                f"ASM2 source candidate={candidate_label} "
                f"size={actual_size} sha256={actual_digest}",
                flush=True,
            )

            if not profiles:
                print(
                    f"ASM2 source candidate={candidate_label} status=rejected "
                    f"reason=unsupported-size supported_sizes={supported_sizes}",
                    flush=True,
                )
                continue

            matched_profiles = [
                profile
                for profile in profiles
                if actual_digest == profile.source_sha256
            ]
            if not matched_profiles:
                print(
                    f"ASM2 source candidate={candidate_label} status=rejected "
                    "reason=sha256-mismatch size_matched_profiles="
                    + ",".join(profile.identifier for profile in profiles),
                    flush=True,
                )
                continue

            accepted_file_count += 1
            print(
                f"ASM2 source candidate={candidate_label} status=accepted "
                "profiles="
                + ",".join(profile.identifier for profile in matched_profiles),
                flush=True,
            )
            for profile in matched_profiles:
                candidates.append((profile, entry))

    print(
        f"ASM2 source scan summary: candidates={candidate_count} "
        f"hashed={hashed_count} accepted={accepted_file_count} "
        f"skipped_non_candidates={skipped_file_count} "
        f"skipped_links={skipped_link_count} read_errors={read_error_count}",
        flush=True,
    )

    if not candidates:
        for profile in SOURCE_PROFILES:
            print(
                f"ASM2 supported source profile={profile.identifier} "
                f"size={profile.source_size} sha256={profile.source_sha256} "
                f"abi_scope={'ARMv7+x86' if profile.has_x86 else 'ARM32-only'}",
                flush=True,
            )
        raise RuntimeError(
            "no supported owner-supplied ASM2 Android 1.2.7d/1.2.8d "
            "APK or installer was found; "
            f"inspected {candidate_count} candidate(s), accepted 0 "
            "(see ASM2 source scan lines)"
        )

    if target_abi == "x86":
        universal = [candidate for candidate in candidates if candidate[0].has_x86]
        if not universal:
            raise RuntimeError(
                "the recognized ASM2 1.2.8d source contains ARM32 game code "
                "only; it works on ARM32/multilib devices but not the X5M "
                "x86/Box32 route. Supply a supported APK containing the x86 "
                "game library"
            )
        candidates = universal

    profile_order = {
        profile.identifier: index for index, profile in enumerate(SOURCE_PROFILES)
    }
    candidates.sort(
        key=lambda candidate: (
            profile_order[candidate[0].identifier],
            str(candidate[1]).casefold(),
        )
    )
    selected_profile, selected_path = candidates[0]
    print(
        f"ASM2 source accepted: {selected_profile.description}; "
        f"ABI scope={'ARMv7+x86' if selected_profile.has_x86 else 'ARM32-only'}"
        + (
            f" ({len(candidates)} supported source files found)"
            if len(candidates) > 1
            else ""
        )
    )
    return selected_profile, selected_path


def atomic_write(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            prefix=f".{path.name}.",
            suffix=".new",
            dir=path.parent,
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.chmod(0o644)
        os.replace(temporary, path)
        temporary = None
        directory_fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        if temporary is not None:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass


def atomic_copy_stream(
    source,
    destination: Path,
    expected_size: int,
    expected_digest: str,
    label: str,
) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        digest = hashlib.sha256()
        written = 0
        with tempfile.NamedTemporaryFile(
            prefix=f".{destination.name}.",
            suffix=".new",
            dir=destination.parent,
            delete=False,
        ) as output:
            temporary = Path(output.name)
            while True:
                block = source.read(8 * 1024 * 1024)
                if not block:
                    break
                output.write(block)
                digest.update(block)
                written += len(block)
            output.flush()
            os.fsync(output.fileno())
        if written != expected_size:
            raise RuntimeError(
                f"{label} has size {written}, expected {expected_size}"
            )
        actual_digest = digest.hexdigest()
        if actual_digest != expected_digest:
            raise RuntimeError(
                f"{label} failed SHA-256 validation ({actual_digest})"
            )
        temporary.chmod(0o644)
        os.replace(temporary, destination)
        temporary = None
        directory_fd = os.open(destination.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        if temporary is not None:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass


def prepare_runtime_apk(
    profile: SourceProfile,
    source_package: Path,
    runtime_apk: Path,
    rebuild_tool: Path,
) -> None:
    if profile.runtime_mode == "rebuild":
        run_tool(rebuild_tool, source_package, runtime_apk, "--force")
    elif profile.runtime_mode == "copy":
        with source_package.open("rb") as source:
            atomic_copy_stream(
                source,
                runtime_apk,
                profile.runtime_size,
                profile.runtime_sha256,
                "runtime APK source",
            )
    elif profile.runtime_mode == "member":
        if not profile.runtime_member:
            raise RuntimeError("runtime member profile is incomplete")
        with zipfile.ZipFile(source_package, "r") as archive:
            info = archive.getinfo(profile.runtime_member)
            if info.flag_bits & 0x1:
                raise RuntimeError("nested runtime APK is encrypted")
            if info.file_size != profile.runtime_size:
                raise RuntimeError("nested runtime APK has an unexpected size")
            with archive.open(info, "r") as source:
                atomic_copy_stream(
                    source,
                    runtime_apk,
                    profile.runtime_size,
                    profile.runtime_sha256,
                    "nested runtime APK",
                )
    else:
        raise RuntimeError(f"unsupported runtime mode: {profile.runtime_mode}")

    validate_file(
        runtime_apk,
        profile.runtime_size,
        profile.runtime_sha256,
        "runtime base.apk",
    )
    with zipfile.ZipFile(runtime_apk, "r") as archive:
        bad_member = archive.testzip()
        if bad_member is not None:
            raise RuntimeError(f"runtime APK CRC test failed: {bad_member}")


def recover_library(
    source_apk: Path,
    destination: Path,
    entry_name: str,
    expected_size: int,
    expected_digest: str,
) -> None:
    blob = source_apk.read_bytes()
    headers = physical_headers(blob)
    with zipfile.ZipFile(source_apk, "r") as archive:
        info = archive.getinfo(entry_name)
        offset = headers.get(entry_name)
        if offset is None:
            raise RuntimeError(
                f"physical {entry_name} local header was not found"
            )
        payload, _actual_crc, _repair = unpack_entry(blob, info, offset)
    if len(payload) != expected_size:
        raise RuntimeError(
            f"recovered {entry_name} has an unexpected size"
        )
    if hashlib.sha256(payload).hexdigest() != expected_digest:
        raise RuntimeError(
            f"recovered {entry_name} failed SHA-256 validation"
        )
    atomic_write(destination, payload)


def run_tool(tool: Path, *arguments) -> None:
    subprocess.run(
        [sys.executable, str(tool), *(str(argument) for argument in arguments)],
        check=True,
    )


def progress(value: int, message: str) -> None:
    print(f"NXEXTRACT_PROGRESS {value} 100 {message}", flush=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", required=True, type=Path)
    parser.add_argument("--game-dir", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    stage = args.stage.resolve(strict=True)
    game_dir = args.game_dir.resolve(strict=True)
    tools = Path(__file__).resolve().parent
    target_abi = os.environ.get("NXEXTRACT_ABI", "armeabi-v7a")
    profile, source_package = find_source_package(game_dir, target_abi)

    progress(5, "VALIDATING OWNER-SUPPLIED ASM2 1.2.7d/1.2.8d FILES")
    for relative in (
        str(OBB_DIRECTORY / MAIN_OBB),
        str(OBB_DIRECTORY / PATCH_12723),
    ):
        size, digest = REQUIRED_OUTPUTS[relative]
        validate_file(stage / relative, size, digest, relative)
    for relative, (size, digest) in OPTIONAL_OUTPUTS.items():
        path = stage / relative
        if path.exists() or path.is_symlink():
            validate_file(path, size, digest, relative)

    progress(15, "PREPARING VALIDATED RUNTIME APK")
    runtime_apk = stage / "gamefiles/base.apk"
    prepare_runtime_apk(
        profile,
        source_package,
        runtime_apk,
        tools / "rebuild_runtime_apk.py",
    )

    progress(50, "RECOVERING VALIDATED NATIVE GAME LIBRARIES")
    arm_library = stage / "libtasm2.so"
    recover_library(
        runtime_apk,
        arm_library,
        ARM_LIBRARY_ENTRY,
        ARM_LIBRARY_SIZE,
        ARM_LIBRARY_SHA256,
    )
    validate_file(
        arm_library,
        ARM_LIBRARY_SIZE,
        ARM_LIBRARY_SHA256,
        "libtasm2.so",
    )
    x86_library = stage / "libtasm2-x86.so"
    if profile.has_x86:
        recover_library(
            runtime_apk,
            x86_library,
            X86_LIBRARY_ENTRY,
            X86_LIBRARY_SIZE,
            X86_LIBRARY_SHA256,
        )
        validate_file(
            x86_library,
            X86_LIBRARY_SIZE,
            X86_LIBRARY_SHA256,
            "libtasm2-x86.so",
        )
    else:
        atomic_write(x86_library, X86_UNAVAILABLE_PAYLOAD)
        validate_file(
            x86_library,
            len(X86_UNAVAILABLE_PAYLOAD),
            X86_UNAVAILABLE_SHA256,
            "ARM32-only x86 marker",
        )

    progress(75, "BUILDING OFFLINE SHOP CATALOG")
    assets = stage / "assets"
    run_tool(
        tools / "extract_shop_assets.py",
        stage / OBB_DIRECTORY / MAIN_OBB,
        assets,
    )

    progress(90, "VERIFYING COMPLETE ASM2 DATA SET")
    validate_file(
        runtime_apk,
        profile.runtime_size,
        profile.runtime_sha256,
        "gamefiles/base.apk",
    )
    for relative, (size, digest) in REQUIRED_OUTPUTS.items():
        validate_file(stage / relative, size, digest, relative)
    for relative, (size, digest) in OPTIONAL_OUTPUTS.items():
        path = stage / relative
        if path.exists() or path.is_symlink():
            validate_file(path, size, digest, relative)
    if profile.has_x86:
        validate_file(
            x86_library,
            X86_LIBRARY_SIZE,
            X86_LIBRARY_SHA256,
            "libtasm2-x86.so",
        )
    else:
        validate_file(
            x86_library,
            len(X86_UNAVAILABLE_PAYLOAD),
            X86_UNAVAILABLE_SHA256,
            "ARM32-only x86 marker",
        )

    progress(100, "ASM2 1.2.7d/1.2.8d DATA PREPARED")
    obb_count = 2 + int((stage / OBB_DIRECTORY / PATCH_12438).is_file())
    print(
        f"OK: profile={profile.identifier} "
        f"abi_scope={'ARMv7+x86' if profile.has_x86 else 'ARM32-only'} "
        f"obb_files={obb_count} runtime outputs validated in NXExtract stage"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(2)
