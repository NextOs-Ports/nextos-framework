#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Os dois casos SDL do Spruce, contra a lista REAL da SDL 32-bit da firmware.

A lista de drivers nao vem do JSON: ela e medida no ambiente preparado pelo
mesmo probe ARMHF que o verificador usa, executado sob qemu com o loader
alternativo da firmware. Em cima dessa lista rodam os dois casos que faltavam:
sem hint e com hint incompativel injetado. Nada inicializa video nem toca
DRM, Mali, framebuffer, audio ou input.
"""

import argparse
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parents[3]
CONTRACT = HERE / "contract-v1.json"
CASES_SOURCE = REPOSITORY / "framework/tests/probes/sdl_hint_cases.c"
NXGL_ROOT = REPOSITORY / "framework/nxgl"
INCOMPATIBLE_HINT = "x11"
TRUSTED_TOOL_PATH = os.defpath


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def resolve_tool(name, required=True):
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


def clean_subprocess_env():
    return {"PATH": TRUSTED_TOOL_PATH, "LANG": "C", "LC_ALL": "C"}


def load_verifier():
    spec = importlib.util.spec_from_file_location(
        "spruce_verify", HERE / "verify.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def measure_firmware_drivers(environment):
    """Ask the firmware's own 32-bit SDL which video drivers it compiled in."""
    verifier = load_verifier()
    contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
    receipt_path = environment / "environment-receipt.json"
    require(receipt_path.is_file() and not receipt_path.is_symlink(),
            "prepared environment receipt is missing: %s" % receipt_path)
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    layout = receipt["layout"]
    alternate = environment / layout["alternate_loader"]
    roots = {
        "armhf-chroot": environment / layout["armhf_chroot"],
        "muos-reduced": environment / layout["muos_reduced"],
    }
    for path in (alternate, roots["armhf-chroot"], roots["muos-reduced"]):
        require(path.exists(), "prepared environment is incomplete: %s" % path)
    measured = verifier.qemu_sdl_probe(contract, alternate, roots)
    require(isinstance(measured, str) and measured.startswith("sdl="),
            "ARMHF SDL probe did not report a version")
    drivers = contract["topology"]["sdl"]["compiled_video_drivers"]
    require(drivers, "the firmware contract declares no compiled video driver")
    require(INCOMPATIBLE_HINT not in [d.lower() for d in drivers],
            "the injected hint must be absent from the firmware's SDL")
    return drivers, measured


def build_cases(workdir):
    compiler = (resolve_tool("clang", required=False) or
                resolve_tool("cc", required=False) or
                resolve_tool("gcc", required=False))
    require(compiler, "host C compiler not found; the gate cannot run blind")
    pkg_config = resolve_tool("pkg-config", required=False)
    binary = workdir / "sdl-hint-cases"
    command = [
        compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
        "-D_POSIX_C_SOURCE=200809L", "-DNXGL_SDL_HINT_TESTING=1",
        "-I", str(NXGL_ROOT / "include"), "-I", str(NXGL_ROOT / "src"),
        str(NXGL_ROOT / "src/nxgl_sdl_hint.c"),
        str(NXGL_ROOT / "src/nxgl_arbiter.c"), str(CASES_SOURCE),
        "-o", str(binary),
    ]
    if pkg_config:
        sdl_flags = subprocess.run(
            [pkg_config, "--cflags", "sdl2"], stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, env=clean_subprocess_env())
        if sdl_flags.returncode == 0:
            command[1:1] = sdl_flags.stdout.decode().split()
    result = subprocess.run(command, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT,
                            env=clean_subprocess_env())
    require(result.returncode == 0,
            "SDL hint cases failed to build:\n%s"
            % result.stdout.decode("utf-8", "replace"))
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--environment", required=True,
                        help="prepared Spruce environment directory")
    arguments = parser.parse_args()
    environment = Path(arguments.environment).resolve()
    require(environment.is_dir(), "environment directory not found")

    drivers, measured = measure_firmware_drivers(environment)
    with tempfile.TemporaryDirectory(prefix="spruce-sdl-hint.") as tmp:
        binary = build_cases(Path(tmp))
        result = subprocess.run(
            [str(binary), ",".join(drivers), INCOMPATIBLE_HINT],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            env=clean_subprocess_env())
        output = result.stdout.decode("utf-8", "replace")
        require(result.returncode == 0, "SDL hint cases failed:\n%s" % output)
    print(output.strip())
    print("spruce SDL hint gate passed: %s drivers=%s injected=%s "
          "hardware_ran=0 device_access=0"
          % (measured.split()[0].strip(), ",".join(drivers),
             INCOMPATIBLE_HINT))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except GateError as error:
        print("spruce SDL hint gate failed: %s" % error, file=sys.stderr)
        sys.exit(1)
