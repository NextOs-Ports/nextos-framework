#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Verify the opt-in ROCKNIX PC environment without touching device hardware."""

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath


DEVICE_DIR = Path(__file__).resolve().parent
REPOSITORY = DEVICE_DIR.parents[3]
CONTRACT_PATH = DEVICE_DIR / "contract-v1.json"
PROBE_PATH = REPOSITORY / "framework/tests/probes/rocknix-sdl-drivers.c"
LATE_BIND_PROBE_PATH = (
    REPOSITORY / "framework/tests/probes/rocknix-egl-gles-dlopen.c"
)
PROVIDER_FALLBACK_SOURCE = DEVICE_DIR / "provider_fallback_case.c"
NXGL_ROOT = REPOSITORY / "framework/nxgl"
PRIVATE_PATH = re.compile(r"(?:/home/|/Users/|[A-Za-z]:\\\\)")
PRIVATE_ADDRESS = re.compile(
    r"(?:10\.|127\.|169\.254\.|192\.168\.|"
    r"172\.(?:1[6-9]|2[0-9]|3[01])\.)"
)
SDL_SYMBOL = re.compile(r"\b(SDL_[A-Za-z0-9_]+)\s*\(")
QEMU_CLOSURE = (
    "usr/lib/libm.so.6",
    "usr/lib/libasound.so.2",
    "usr/lib/libasound.so.2.0.0",
)
QEMU_PROVIDER_CLOSURE = (
    "usr/lib/libGLdispatch.so.0",
    "usr/lib/libGLdispatch.so.0.0.0",
)
TRUSTED_TOOL_PATH = os.defpath


HINT_CASE_SOURCE = r'''/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "nxgl.h"
#include "nxgl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *drivers[NXGL_SDL_VIDEO_DRIVER_MAX];
static char storage[NXGL_SDL_VIDEO_DRIVER_LIST_MAX];
static int driver_count;
static int failures;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                    #condition);                                             \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

int nxgl_test_sdl_get_num_video_drivers(void) { return driver_count; }

const char *nxgl_test_sdl_get_video_driver(int index) {
  if (index < 0 || index >= driver_count)
    return NULL;
  return drivers[index];
}

Uint32 nxgl_test_sdl_was_init(Uint32 flags) { (void)flags; return 0u; }
const char *nxgl_test_sdl_current_video_driver(void) { return NULL; }

static int load_drivers(const char *list) {
  char *cursor;
  size_t length = strlen(list);
  if (length + 1u > sizeof(storage))
    return -1;
  memcpy(storage, list, length + 1u);
  cursor = storage;
  while (*cursor) {
    char *comma = strchr(cursor, ',');
    if (driver_count >= (int)NXGL_SDL_VIDEO_DRIVER_MAX)
      return -1;
    if (comma)
      *comma = '\0';
    if (*cursor == '\0')
      return -1;
    drivers[driver_count++] = cursor;
    if (!comma)
      break;
    cursor = comma + 1;
  }
  return driver_count > 0 ? 0 : -1;
}

static nxgl_sdl_video_hint_receipt_v2 sanitize(const char *hint) {
  nxgl_sdl_video_hint_options_v2 options;
  nxgl_sdl_video_hint_receipt_v2 receipt;
  nxgl_sdl_video_hint_options_v2_init(&options);
  nxgl_sdl_video_hint_receipt_v2_init(&receipt);
  options.enabled = 1;
  if (hint)
    CHECK(setenv("SDL_VIDEODRIVER", hint, 1) == 0);
  else
    CHECK(unsetenv("SDL_VIDEODRIVER") == 0);
  CHECK(nxgl_sanitize_sdl_video_hint_v2(&options, &receipt) == NXGL_SUCCESS);
  return receipt;
}

int main(int argc, char **argv) {
  nxgl_sdl_video_hint_receipt_v2 receipt;
  if (argc != 4 || load_drivers(argv[1]) != 0)
    return 2;

  receipt = sanitize(NULL);
  CHECK(receipt.action == NXGL_SDL_VIDEO_HINT_V2_NO_HINT);
  CHECK(receipt.inherited_hint_present == 0);
  CHECK(receipt.hint_removed == 0);
  CHECK(getenv("SDL_VIDEODRIVER") == NULL);
  (void)printf("case=no-hint action=%s removed=%d\n",
               nxgl_sdl_video_hint_action_name_v2(receipt.action),
               receipt.hint_removed);

  receipt = sanitize(argv[2]);
  CHECK(receipt.action == NXGL_SDL_VIDEO_HINT_V2_CLEARED_UNSUPPORTED);
  CHECK(receipt.inherited_hint_present == 1);
  CHECK(receipt.inherited_hint_supported == 0);
  CHECK(receipt.hint_removed == 1);
  CHECK(getenv("SDL_VIDEODRIVER") == NULL);
  (void)printf("case=unsupported-hint hint=%s action=%s removed=%d\n",
               argv[2], nxgl_sdl_video_hint_action_name_v2(receipt.action),
               receipt.hint_removed);

  receipt = sanitize(argv[3]);
  CHECK(receipt.action == NXGL_SDL_VIDEO_HINT_V2_PRESERVED_SUPPORTED);
  CHECK(receipt.inherited_hint_present == 1);
  CHECK(receipt.inherited_hint_supported == 1);
  CHECK(receipt.hint_removed == 0);
  CHECK(getenv("SDL_VIDEODRIVER") != NULL);
  (void)printf("case=supported-hint hint=%s action=%s removed=%d\n",
               argv[3], nxgl_sdl_video_hint_action_name_v2(receipt.action),
               receipt.hint_removed);

  if (failures != 0)
    return 1;
  (void)printf("rocknix SDL hint cases: PASS measured_drivers=%d "
               "hardware_ran=0 device_access=0 video_initialized=0\n",
               driver_count);
  return 0;
}
'''


class VerificationError(Exception):
    """The contract or prepared environment is invalid."""


def require(condition, message):
    if not condition:
        raise VerificationError(message)


def resolve_tool(name, required=True):
    found = shutil.which(name, path=TRUSTED_TOOL_PATH)
    if found is None:
        if required:
            raise VerificationError("required tool is missing: %s" % name)
        return None
    try:
        resolved = Path(found).resolve(strict=True)
    except OSError as error:
        raise VerificationError("cannot resolve %s: %s" % (name, error))
    require(resolved.is_file() and os.access(resolved, os.X_OK),
            "tool is not executable: %s" % resolved)
    return str(resolved)


def clean_subprocess_env():
    return {"PATH": TRUSTED_TOOL_PATH, "LANG": "C", "LC_ALL": "C"}


def read_json(path):
    require(path.is_file() and not path.is_symlink(),
            "JSON file is missing or unsafe: %s" % path)
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def inside(path, root):
    candidate = path.resolve(strict=False)
    boundary = root.resolve()
    return os.path.commonpath((str(candidate), str(boundary))) == str(boundary)


def safe_relative(root, relative):
    logical = PurePosixPath(relative)
    require(not logical.is_absolute() and ".." not in logical.parts,
            "unsafe relative path: %s" % relative)
    path = root.joinpath(*logical.parts)
    resolved_root = root.resolve()
    resolved = path.resolve(strict=False)
    require(os.path.commonpath((str(resolved_root), str(resolved))) ==
            str(resolved_root),
            "path escaped prepared environment: %s" % relative)
    return path


def validate_regular(path, expected, label):
    require(path.is_file() and not path.is_symlink(),
            "%s is missing or unsafe: %s" % (label, path))
    require(path.stat().st_size == expected["size"],
            "%s size mismatch" % label)
    require(sha256_file(path) == expected["sha256"],
            "%s SHA-256 mismatch" % label)


def elf_identity(path):
    with path.open("rb") as stream:
        header = stream.read(64)
    require(len(header) >= 20 and header[:4] == b"\x7fELF",
            "not an ELF: %s" % path)
    require(header[5] in (1, 2), "unsupported ELF byte order: %s" % path)
    endian = "<" if header[5] == 1 else ">"
    return header[4], struct.unpack(endian + "H", header[18:20])[0]


def validate_squashfs(contract, path):
    expected = contract["system_payload"]["filesystem"]
    with path.open("rb") as stream:
        header = stream.read(96)
    require(len(header) == 96 and header[:4] == b"hsqs",
            "SYSTEM is not little-endian SquashFS")
    inode_count = struct.unpack("<I", header[4:8])[0]
    block_size = struct.unpack("<I", header[12:16])[0]
    compression = struct.unpack("<H", header[20:22])[0]
    major = struct.unpack("<H", header[28:30])[0]
    bytes_used = struct.unpack("<Q", header[40:48])[0]
    require(major == expected["version"] and compression == 6 and
            inode_count == expected["inode_count"] and
            block_size == expected["block_size"] and
            bytes_used == expected["bytes_used"],
            "SYSTEM SquashFS geometry mismatch")


def by_id(contract, artifact_id):
    matches = [item for item in contract["runtime_files"]
               if item["id"] == artifact_id]
    require(len(matches) == 1, "runtime id is missing or duplicated: %s" % artifact_id)
    return matches[0]


def validate_contract(contract):
    require(contract.get("schema") == "nxframework-device-environment-v1" and
            contract.get("schema_version") == 1 and
            contract.get("id") == "rocknix-rk3566-specific-20260801",
            "wrong ROCKNIX environment contract identity")
    text = json.dumps(contract, sort_keys=True)
    require(PRIVATE_PATH.search(text) is None,
            "contract contains a personal path")
    require(PRIVATE_ADDRESS.search(text) is None,
            "contract contains a private address")
    firmware = contract["firmware"]
    require(firmware["stored_in_repository"] is False and
            firmware["redistributed_by_framework"] is False and
            firmware["compression"] == "gzip",
            "contract would redistribute firmware")
    require(contract["disk"]["partition_table"] == "gpt" and
            contract["disk"]["crc_state"] == "valid",
            "disk integrity contract changed")
    require(contract["system_payload"]["contiguous"] is True,
            "SYSTEM is no longer safe for bounded stream extraction")

    graphics = contract["topology"]["graphics"]
    require(contract["topology"]["host_architecture"] == "aarch64",
            "ROCKNIX host architecture changed")
    require(graphics["evidence_scope"] ==
            "image-backed-userspace-inventory-only" and
            graphics["session"]["physical_selected_backend"] == "not-measured" and
            graphics["sdl"]["physical_selected_video_driver"] == "not-measured" and
            graphics["mesa"]["physical_driver_bound"] == "not-measured",
            "PC gate crossed the physical graphics boundary")
    require(graphics["framework_policy"] == {
                "backend_selected_by_firmware_or_device_name": False,
                "absent_sdl_video_hint": "leave-absent-for-autodetection",
                "supported_inherited_sdl_video_hint": "preserve",
                "unsupported_inherited_sdl_video_hint":
                    "remove-without-replacement",
                "egl_gles_provider_override": False,
                "ld_preload_override": False,
            }, "ROCKNIX graphics policy changed")
    require(graphics["sdl"]["compiled_video_drivers_state"] ==
            "measured-under-qemu-without-video-init" and
            graphics["sdl"]["compiled_video_drivers"] ==
            ["x11", "wayland", "KMSDRM", "offscreen", "dummy", "evdev"],
            "compiled SDL metadata evidence changed")

    ids = [item["id"] for item in contract["runtime_files"]]
    paths = [item["path"] for item in contract["runtime_files"]]
    require(len(ids) == len(set(ids)) and len(paths) == len(set(paths)),
            "runtime file ids/paths are not unique")
    required_ids = {
        "sway-profile", "sway-service", "os-release", "aarch64-loader",
        "libc", "sdl2", "egl-dispatch", "gles2-dispatch", "egl-mesa",
        "gbm", "drm", "wayland-client", "wayland-egl",
        "panfrost-dri-link", "dril-dri", "gallium",
        "panfrost-kernel-module", "vulkan-panfrost", "sway",
    }
    require(set(ids) == required_ids,
            "pinned runtime inventory changed")


def validate_repository_boundary():
    forbidden_suffixes = (".img", ".gz", ".squashfs", ".sqsh")
    for path in DEVICE_DIR.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        require(not path.name.endswith(forbidden_suffixes),
                "firmware payload entered the repository: %s" % path)
        with path.open("rb") as stream:
            magic = stream.read(4)
        require(magic != b"\x7fELF" and not magic.startswith(b"hsqs") and
                not magic.startswith(b"\x1f\x8b"),
                "binary firmware payload entered the repository: %s" % path)
    for probe in (PROBE_PATH, LATE_BIND_PROBE_PATH,
                  PROVIDER_FALLBACK_SOURCE):
        require(probe.is_file() and not probe.is_symlink(),
                "focused probe source is missing: %s" % probe)
        with probe.open("rb") as stream:
            require(stream.read(4) != b"\x7fELF",
                    "compiled probe entered the repository: %s" % probe)


def validate_runtime_files(contract, rootfs):
    checked = []
    for expected in contract["runtime_files"]:
        path = safe_relative(rootfs, expected["path"])
        if expected["kind"] == "symlink":
            require(path.is_symlink(), "%s symlink is missing" % expected["id"])
            require(os.readlink(path) == expected["target"],
                    "%s symlink target mismatch" % expected["id"])
            require(inside(path.parent / expected["target"], rootfs),
                    "%s symlink escapes rootfs" % expected["id"])
        else:
            validate_regular(path, expected, expected["id"])
            if "elf_class" in expected or "elf_machine" in expected:
                require(elf_identity(path) ==
                        (expected["elf_class"], expected["elf_machine"]),
                        "%s ELF identity mismatch" % expected["id"])
        checked.append(expected["id"])
    return checked


def validate_text_evidence(contract, rootfs):
    graphics = contract["topology"]["graphics"]
    observed = graphics["session"]["observed_environment"]
    os_release = safe_relative(rootfs, by_id(contract, "os-release")["path"])
    profile = safe_relative(rootfs, by_id(contract, "sway-profile")["path"])
    service = safe_relative(rootfs, by_id(contract, "sway-service")["path"])
    os_text = os_release.read_text(encoding="utf-8")
    profile_text = profile.read_text(encoding="utf-8")
    service_text = service.read_text(encoding="utf-8")
    require('OS_NAME="ROCKNIX"' in os_text and
            'OS_VERSION="%s"' % contract["firmware"]["version"] in os_text and
            'HW_ARCH="aarch64"' in os_text and 'HW_DEVICE="RK3566"' in os_text,
            "os-release semantic identity mismatch")
    require("export WAYLAND_DISPLAY=%s" % observed["WAYLAND_DISPLAY"] in profile_text and
            "export SDL_VIDEODRIVER=%s" % observed["SDL_VIDEODRIVER"] in profile_text,
            "Sway profile environment mismatch")
    require("Environment=WLR_BACKENDS=%s" % observed["WLR_BACKENDS"] in service_text,
            "Sway service backend list mismatch")
    gallium = safe_relative(rootfs, by_id(contract, "gallium")["path"])
    with gallium.open("rb") as stream:
        gallium_data = stream.read()
    require(b"panfrost" in gallium_data and b"DRM_IOCTL_PANFROST" in gallium_data,
            "Gallium provider lacks pinned Panfrost markers")


def validate_environment(contract, environment):
    layout = contract["prepared_layout"]
    receipt = read_json(safe_relative(environment, layout["receipt"]))
    firmware = contract["firmware"]
    payload = contract["system_payload"]
    require(receipt.get("schema") ==
            "nxframework-prepared-device-environment-v1" and
            receipt.get("schema_version") == 1 and
            receipt.get("environment_id") == contract["id"],
            "prepared receipt identity mismatch")
    require(receipt.get("contract_sha256") == sha256_file(CONTRACT_PATH),
            "prepared receipt was made from another contract")
    require(receipt.get("source_archive") == {
                "name": firmware["archive_name"],
                "size": firmware["archive_size"],
                "sha256": firmware["archive_sha256"],
                "compression": firmware["compression"],
                "uncompressed_size": firmware["uncompressed_size"],
            }, "prepared source archive receipt mismatch")
    require(receipt.get("system_payload") == {
                "path": layout["system_image"],
                "size": payload["size"],
                "sha256": payload["sha256"],
            }, "prepared SYSTEM receipt mismatch")
    require(receipt.get("layout") == layout, "prepared layout receipt mismatch")
    require(receipt.get("evidence") == {
                "kind": "prepared-host-environment",
                "hardware_ran": False,
                "device_access": False,
                "physical_graphics_claim": False,
            }, "prepared receipt crossed the PC evidence boundary")

    system_image = safe_relative(environment, layout["system_image"])
    rootfs = safe_relative(environment, layout["rootfs"])
    require(rootfs.is_dir() and not rootfs.is_symlink(),
            "prepared rootfs is missing or unsafe")
    validate_regular(system_image, payload, "prepared SYSTEM")
    validate_squashfs(contract, system_image)
    checked = validate_runtime_files(contract, rootfs)
    require(receipt.get("checks", {}).get("runtime_files") == checked,
            "prepared runtime receipt mismatch")
    validate_text_evidence(contract, rootfs)
    return system_image, rootfs, len(checked)


def run_command(arguments, label, env=None, timeout=60):
    require(arguments and Path(arguments[0]).is_absolute(),
            "%s executable must use an absolute path" % label)
    if env is None:
        env = clean_subprocess_env()
    try:
        result = subprocess.run(
            arguments, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, check=False, env=env, timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        raise VerificationError("%s timed out" % label) from error
    require(result.returncode == 0,
            "%s failed (%d):\n%s" %
            (label, result.returncode, result.stdout[-4000:]))
    return result.stdout


def validate_probe_source():
    source = PROBE_PATH.read_text(encoding="utf-8")
    symbols = set(SDL_SYMBOL.findall(source))
    require(symbols == {
                "SDL_GetNumVideoDrivers", "SDL_GetVideoDriver", "SDL_GetVersion",
            }, "firmware probe calls something other than SDL_Get* metadata APIs")
    require("SDL_Init" not in source and "SDL_WasInit" not in source,
            "firmware probe may initialize an SDL subsystem")

    late_bind = LATE_BIND_PROBE_PATH.read_text(encoding="utf-8")
    require("dlopen(\"libEGL.so.1\"" in late_bind and
            "dlopen(\"libGLESv2.so.2\"" in late_bind and
            "dlsym(egl, \"eglInitialize\")" in late_bind and
            "dlsym(gles, \"glCreateShader\")" in late_bind,
            "late-binding probe lost required target symbols")
    for forbidden in ("eglInitialize(", "eglGetDisplay(",
                      "glCreateShader(", "glGetString(", "SDL_Init"):
        require(forbidden not in late_bind,
                "late-binding probe may execute graphics API: %s" % forbidden)

    hint_implementation = (
        NXGL_ROOT / "src/nxgl_sdl_hint.c"
    ).read_text(encoding="utf-8").lower()
    for selector in ("rocknix", "rk3566", "rgds", "panfrost", "mali-g"):
        require(selector not in hint_implementation,
                "SDL hint sanitizer selects by device/firmware name: %s" %
                selector)


def qemu_sdl_probe(contract, system_image, rootfs):
    clang = resolve_tool("clang", required=False)
    qemu = (resolve_tool("qemu-aarch64-static", required=False) or
            resolve_tool("qemu-aarch64", required=False))
    unsquashfs = resolve_tool("unsquashfs", required=False)
    require(clang is not None, "clang is required for the AArch64 SDL probe")
    require(qemu is not None, "qemu-aarch64 is required for the SDL probe")
    require(unsquashfs is not None, "unsquashfs is required for the SDL closure")
    validate_probe_source()

    loader = safe_relative(rootfs, by_id(contract, "aarch64-loader")["path"])
    sdl = safe_relative(rootfs, by_id(contract, "sdl2")["path"])
    with tempfile.TemporaryDirectory(prefix="rocknix-sdl-probe.") as temporary:
        work = Path(temporary)
        qroot = work / "closure"
        run_command([
            unsquashfs, "-quiet", "-no-progress", "-f", "-d", str(qroot),
            str(system_image), *QEMU_CLOSURE, *QEMU_PROVIDER_CLOSURE,
        ], "authenticated SDL dependency extraction")
        qlib = qroot / "usr/lib"
        require((qlib / "libm.so.6").is_file() and
                not (qlib / "libm.so.6").is_symlink(),
                "authenticated libm closure is missing")
        require((qlib / "libasound.so.2").is_symlink() and
                os.readlink(qlib / "libasound.so.2") == "libasound.so.2.0.0" and
                (qlib / "libasound.so.2.0.0").is_file(),
                "authenticated ALSA closure is invalid")
        require((qlib / "libGLdispatch.so.0").is_symlink() and
                os.readlink(qlib / "libGLdispatch.so.0") ==
                "libGLdispatch.so.0.0.0" and
                (qlib / "libGLdispatch.so.0.0.0").is_file(),
                "authenticated GLdispatch closure is invalid")
        os.symlink(str(sdl), qlib / "libSDL2-2.0.so.0")

        probe = work / "rocknix-sdl-drivers"
        run_command([
            clang, "--target=aarch64-linux-gnu", "-march=armv8-a", "-O2",
            "-fuse-ld=lld", "-nostdlib", "-fno-builtin",
            "-fno-stack-protector",
            "-Wl,--dynamic-linker=/usr/lib/ld-linux-aarch64.so.1",
            "-Wl,-e,_start", "-Wl,--no-as-needed",
            "-Wl,--allow-shlib-undefined", str(PROBE_PATH),
            "-L%s" % sdl.parent, "-l:%s" % sdl.name,
            "-o", str(probe),
        ], "AArch64 SDL probe compilation")
        require(elf_identity(probe) == (2, 183),
                "compiled SDL probe is not AArch64")

        clean_env = clean_subprocess_env()
        for inherited in (
                "WAYLAND_DISPLAY", "DISPLAY", "XDG_RUNTIME_DIR",
                "SDL_VIDEODRIVER", "SDL_VIDEO_EGL_DRIVER",
                "SDL_VIDEO_GL_DRIVER", "LD_PRELOAD", "QEMU_LD_PREFIX"):
            require(inherited not in clean_env,
                    "host graphics variable leaked into target probe: %s" %
                    inherited)
        output = run_command([
            qemu, str(loader), "--library-path",
            "%s:%s" % (qlib, rootfs / "usr/lib"), str(probe),
        ], "AArch64 SDL metadata query", env=clean_env, timeout=30)

        egl = safe_relative(rootfs, by_id(contract, "egl-dispatch")["path"])
        gles = safe_relative(rootfs, by_id(contract, "gles2-dispatch")["path"])
        os.symlink(str(egl), qlib / "libEGL.so.1")
        os.symlink(str(gles), qlib / "libGLESv2.so.2")
        late_probe = work / "rocknix-egl-gles-dlopen"
        run_command([
            clang, "--target=aarch64-linux-gnu", "-march=armv8-a", "-O2",
            "-fuse-ld=lld", "-nostdlib", "-fno-builtin",
            "-fno-stack-protector",
            "-Wl,--dynamic-linker=/usr/lib/ld-linux-aarch64.so.1",
            "-Wl,-e,_start", "-Wl,--no-as-needed",
            "-Wl,--allow-shlib-undefined", str(LATE_BIND_PROBE_PATH),
            "-L%s" % rootfs.joinpath("usr/lib"), "-l:libc.so.6",
            "-o", str(late_probe),
        ], "AArch64 EGL/GLES late-binding probe compilation")
        require(elf_identity(late_probe) == (2, 183),
                "compiled EGL/GLES late-binding probe is not AArch64")
        late_output = run_command([
            qemu, str(loader), "--library-path",
            "%s:%s" % (qlib, rootfs / "usr/lib"), str(late_probe),
        ], "AArch64 EGL/GLES symbols-only late discovery", env=clean_env,
           timeout=30)
        require(late_output == "late-bind=EGL,GLES2 symbols-only\n",
                "target EGL/GLES late discovery returned unexpected output")

    match = re.fullmatch(r"sdl=([0-9]+\.[0-9]+\.[0-9]+)\n"
                         r"drivers=([A-Za-z0-9_.-]+(?:,[A-Za-z0-9_.-]+)*)\n",
                         output)
    require(match is not None, "target SDL returned malformed metadata: %r" % output)
    version = match.group(1)
    drivers = match.group(2).split(",")
    require(version == contract["topology"]["graphics"]["sdl"]
            ["version_from_library"], "target SDL version changed")
    require(drivers == contract["topology"]["graphics"]["sdl"]
            ["compiled_video_drivers"],
            "target SDL compiled-driver list changed")
    require(len(drivers) == len(set(item.lower() for item in drivers)),
            "target SDL returned duplicate drivers")
    expected_session = contract["topology"]["graphics"]["session"]
    expected_driver = expected_session["observed_environment"]["SDL_VIDEODRIVER"]
    require(expected_driver.lower() in [item.lower() for item in drivers],
            "firmware session requests an SDL driver absent from its SDL library")
    return version, drivers, late_output.strip()


def run_hint_cases(contract, drivers):
    compiler = (resolve_tool("clang", required=False) or
                resolve_tool("cc", required=False) or
                resolve_tool("gcc", required=False))
    require(compiler is not None, "host C compiler is required for hint cases")
    supported_wanted = contract["topology"]["graphics"]["session"] \
        ["observed_environment"]["SDL_VIDEODRIVER"]
    supported = next((item for item in drivers
                      if item.lower() == supported_wanted.lower()), None)
    require(supported is not None, "measured SDL lacks the supported hint case")
    lower_drivers = {item.lower() for item in drivers}
    unsupported = next((item for item in ("windows", "cocoa", "directfb")
                        if item not in lower_drivers), None)
    require(unsupported is not None, "could not choose an absent SDL hint")

    with tempfile.TemporaryDirectory(prefix="rocknix-sdl-hints.") as temporary:
        work = Path(temporary)
        source = work / "rocknix-sdl-hint-cases.c"
        binary = work / "rocknix-sdl-hint-cases"
        source.write_text(HINT_CASE_SOURCE, encoding="utf-8")
        command = [
            compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
            "-D_POSIX_C_SOURCE=200809L", "-DNXGL_SDL_HINT_TESTING=1",
            "-I", str(NXGL_ROOT / "include"),
            "-I", str(NXGL_ROOT / "src"),
            str(NXGL_ROOT / "src/nxgl_sdl_hint.c"),
            str(NXGL_ROOT / "src/nxgl_arbiter.c"), str(source),
            "-o", str(binary),
        ]
        pkg_config = resolve_tool("pkg-config", required=False)
        if pkg_config:
            flags = subprocess.run(
                [pkg_config, "--cflags", "sdl2"], stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, text=True, check=False,
                env=clean_subprocess_env(),
            )
            if flags.returncode == 0:
                command[1:1] = flags.stdout.split()
        run_command(command, "nxgl SDL hint-case compilation")
        clean_env = clean_subprocess_env()
        output = run_command([
            str(binary), ",".join(drivers), unsupported, supported,
        ], "nxgl measured-driver hint cases", env=clean_env)
        without_wayland = [item for item in drivers
                           if item.lower() != supported.lower()]
        require(without_wayland,
                "cannot exercise fallback without the Wayland driver")
        fallback_supported = next(
            (item for item in without_wayland if item.lower() == "kmsdrm"),
            without_wayland[0],
        )
        fallback_output = run_command([
            str(binary), ",".join(without_wayland), supported,
            fallback_supported,
        ], "nxgl Wayland-unavailable fallback cases", env=clean_env)
    require("case=no-hint action=no-hint removed=0" in output and
            "action=cleared-unsupported removed=1" in output and
            "action=preserved-supported removed=0" in output and
            "video_initialized=0" in output,
            "nxgl hint/fallback receipt is incomplete")
    require("case=no-hint action=no-hint removed=0" in fallback_output and
            "case=unsupported-hint hint=%s action=cleared-unsupported "
            "removed=1" % supported in fallback_output and
            "video_initialized=0" in fallback_output,
            "nxgl Wayland-unavailable fallback selected a replacement")
    return unsupported, supported, fallback_supported


def run_provider_fallback_case():
    compiler = (resolve_tool("clang", required=False) or
                resolve_tool("cc", required=False) or
                resolve_tool("gcc", required=False))
    require(compiler is not None,
            "host C compiler is required for provider fallback case")
    with tempfile.TemporaryDirectory(
            prefix="rocknix-provider-fallback.") as temporary:
        binary = Path(temporary) / "rocknix-provider-fallback"
        command = [
            compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
            "-D_POSIX_C_SOURCE=200809L",
            "-I", str(NXGL_ROOT / "include"),
            "-I", str(NXGL_ROOT / "src"),
            str(NXGL_ROOT / "src/nxgl_provider_recovery.c"),
            str(NXGL_ROOT / "src/nxgl_diagnostics.c"),
            str(NXGL_ROOT / "src/nxgl_arbiter.c"),
            str(PROVIDER_FALLBACK_SOURCE), "-ldl", "-o", str(binary),
        ]
        pkg_config = resolve_tool("pkg-config", required=False)
        if pkg_config:
            flags = subprocess.run(
                [pkg_config, "--cflags", "sdl2"], stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, text=True, check=False,
                env=clean_subprocess_env(),
            )
            if flags.returncode == 0:
                command[1:1] = flags.stdout.split()
        output = run_command(
            command, "nxgl provider-fallback compilation")
        require(output == "", "provider-fallback compiler emitted output")
        clean_env = clean_subprocess_env()
        output = run_command(
            [str(binary)], "nxgl unavailable-provider fallback",
            env=clean_env,
        )
    require(output ==
            "provider-fallback=candidate-unavailable action=no-action "
            "egl_override=absent gles_override=absent "
            "ld_preload=preserved\n",
            "unavailable provider changed the process environment")
    return "candidate-unavailable/no-action"


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Verify one opt-in ROCKNIX RK3566 PC environment"
    )
    parser.add_argument("--environment", required=True, type=Path)
    parser.add_argument("--static-only", action="store_true",
                        help="skip QEMU SDL metadata and host hint cases")
    args = parser.parse_args(argv)
    environment = Path(os.path.abspath(os.fspath(args.environment)))
    try:
        require(environment.is_dir() and not environment.is_symlink(),
                "prepared environment is missing or unsafe")
        require(not inside(environment, REPOSITORY),
                "prepared environment must stay outside the repository")
        contract = read_json(CONTRACT_PATH)
        validate_contract(contract)
        validate_repository_boundary()
        system_image, rootfs, runtime_count = validate_environment(
            contract, environment
        )
        qemu_text = "skipped"
        hint_text = "skipped"
        provider_text = "skipped"
        if not args.static_only:
            version, drivers, late_bind = qemu_sdl_probe(
                contract, system_image, rootfs
            )
            unsupported, supported, fallback = run_hint_cases(
                contract, drivers
            )
            provider_text = run_provider_fallback_case()
            qemu_text = "sdl=%s drivers=%s %s" % (
                version, ",".join(drivers), late_bind,
            )
            hint_text = "unsupported=%s supported=%s no-wayland=%s" % (
                unsupported, supported, fallback,
            )
    except (json.JSONDecodeError, OSError, StopIteration,
            VerificationError) as error:
        print("ROCKNIX environment gate failed: %s" % error, file=sys.stderr)
        return 1

    print(
        "ROCKNIX environment gate passed: runtime_files=%d qemu=[%s] "
        "hints=[%s] provider=[%s] hardware_ran=0 device_access=0 "
        "video_initialized=0 "
        "egl_initialized=0 gles_context_created=0" %
        (runtime_count, qemu_text, hint_text, provider_text)
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
