#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Os três casos de hint SDL do muOS, contra a SDL real da imagem oficial.

A lista de drivers não vem do JSON: ela é medida no ambiente preparado pelo
mesmo probe que o verificador usa, executado sob qemu com o carregador da
própria imagem. Em cima dessa lista rodam os três casos:

- **sem hint**: a autodetecção da SDL fica intacta;
- **hint compatível** (um driver que essa SDL compilou): preservado;
- **hint incompatível** (um driver que ela não tem): removido.

A decisão é do sanitizador real do nxgl, compilado com o seam de teste apenas
para receber a lista medida. Nada inicializa vídeo; DRM, Mali, framebuffer,
áudio e input ficam intocados.
"""

import argparse
import importlib.util
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
TRUSTED_TOOL_PATH = os.defpath


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def clean_subprocess_env():
    return {
        "PATH": TRUSTED_TOOL_PATH,
        "LANG": "C",
        "LC_ALL": "C",
    }


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


def load_verifier():
    spec = importlib.util.spec_from_file_location(
        "muos_verify", HERE / "verify.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


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
    clean_env = clean_subprocess_env()
    if pkg_config:
        sdl_flags = subprocess.run(
            [pkg_config, "--cflags", "sdl2"], stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, env=clean_env, timeout=15)
        if sdl_flags.returncode == 0:
            command[1:1] = sdl_flags.stdout.decode().split()
    result = subprocess.run(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        cwd=str(workdir), env=clean_env, timeout=30)
    require(result.returncode == 0,
            "SDL hint cases failed to build:\n%s"
            % result.stdout.decode("utf-8", "replace"))
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--environment", required=True)
    arguments = parser.parse_args()
    verifier = load_verifier()
    try:
        raw_environment = Path(arguments.environment)
        require(raw_environment.is_absolute(), "--environment must be absolute")
        require(raw_environment.is_dir() and not raw_environment.is_symlink(),
                "environment directory not found or is a symlink")
        environment = verifier.outside_repository(
            raw_environment, "environment")
        contract = verifier.load_json(CONTRACT)
        receipt = verifier.check_provenance(environment, contract)
        root = environment / "root"
        verifier.require(root.is_dir() and not root.is_symlink(),
                         "prepared root is missing or is a symlink")
        verifier.check_roots_and_isolation(root, contract, receipt)
        verifier.check_interpreters(root, contract)
        verifier.check_renderer(root, contract)
    except verifier.GateError as error:
        raise GateError(str(error))

    incompatible = contract["sdl"]["incompatible_hint"]
    measured = {}
    try:
        for abi in ("aarch64", "armv7"):
            drivers = verifier.query_sdl_drivers(root, contract, abi)
            require(drivers is not None,
                    "clang and %s are required to measure the %s SDL"
                    % (verifier.QEMU[abi], abi))
            require(incompatible not in [item.lower() for item in drivers],
                    "the injected hint must be absent from the %s SDL" % abi)
            measured[abi] = drivers
    except verifier.GateError as error:
        raise GateError(str(error))

    with tempfile.TemporaryDirectory(prefix="muos-sdl-hint.") as tmp:
        binary = build_cases(Path(tmp))
        for abi, drivers in sorted(measured.items()):
            result = subprocess.run(
                [str(binary), ",".join(drivers), incompatible],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                cwd=tmp, env=clean_subprocess_env(), timeout=15)
            output = result.stdout.decode("utf-8", "replace")
            require(result.returncode == 0,
                    "%s SDL hint cases failed:\n%s" % (abi, output))
            for token in ("case=no-hint action=no-hint",
                          "case=incompatible-hint",
                          "action=cleared-unsupported",
                          "case=firmware-hint",
                          "action=preserved-supported"):
                require(token in output,
                        "%s case missing %r:\n%s" % (abi, token, output))
            print("[%s] %s" % (abi, output.strip().replace("\n", "\n[%s] " % abi)))

    print("muos SDL hint gate passed: %s injected=%s hardware_ran=0 "
          "device_access=0"
          % (";".join("%s:%s" % (abi, ",".join(drivers))
                      for abi, drivers in sorted(measured.items())),
             incompatible))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except GateError as error:
        print("muos SDL hint gate failed: %s" % error, file=sys.stderr)
        sys.exit(1)
