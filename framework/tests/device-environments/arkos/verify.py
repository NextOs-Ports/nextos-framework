#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Gate do ambiente ArkOS/dArkOSRE preparado a partir da imagem oficial.

Confere, sem inicializar vídeo e sem tocar em DRM, Mali, framebuffer, áudio ou
input:

- proveniência: recibo, contrato e SHA-256 da imagem;
- as duas raízes **multiarch** reais e o isolamento entre elas;
- o **script de linker** `libc.so` que o Debian multiarch entrega ao lado da
  biblioteca de runtime, e a regra que o framework aplica a ele;
- as cadeias de **soname** de EGL/GLES/GBM/DRM presentes em cada ABI;
- a SDL da própria imagem e o driver **KMSDRM** que ela compilou;
- a rota ARMHF declarada pelos perfis ArkOS e dArkOSRE contra esse layout.

A aceitação física já registrada do port publicado é citada, nunca repetida.
"""

import argparse
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parents[3]
CONTRACT = HERE / "contract-v1.json"
PROFILES = REPOSITORY / "framework/tests/firmware-profiles-v2.json"
PROBE = REPOSITORY / "framework/tests/probes/sdl-drivers.c"
ARCH_ELF = {"aarch64": (2, 183), "armv7": (1, 40)}
CLANG_TARGET = {
    "aarch64": ["--target=aarch64-linux-gnu"],
    "armv7": ["--target=armv7a-linux-gnueabihf", "-march=armv7-a",
              "-mfloat-abi=hard"],
}
QEMU = {"aarch64": "qemu-aarch64-static", "armv7": "qemu-arm-static"}


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def load_imagefs():
    spec = importlib.util.spec_from_file_location(
        "imagefs", HERE.parent / "imagefs.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_provenance(environment, contract, imagefs):
    receipt_path = environment / "environment-receipt.json"
    require(receipt_path.is_file() and not receipt_path.is_symlink(),
            "prepared environment receipt is missing")
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    require(receipt.get("environment_id") == contract["id"],
            "receipt belongs to another environment")
    require(receipt["contract_sha256"] == imagefs.sha256_file(CONTRACT),
            "the contract changed after preparation; prepare again")
    source = receipt["source_image"]
    firmware = contract["firmware"]
    require(source["sha256"] == firmware["image_sha256"]
            and source["size"] == firmware["image_size"],
            "receipt does not describe the official image of the contract")
    require(not any(receipt["evidence"][key] for key in
                    ("hardware_ran", "device_access", "physical_runtime_claim")),
            "the receipt claims hardware evidence it cannot have")
    require(not firmware["stored_in_repository"]
            and not firmware["redistributed_by_framework"],
            "the contract must keep the image out of the repository")
    require("files" in receipt,
            "prepared receipt predates per-file pins; prepare it again")
    return receipt


def check_prepared_bytes(root, receipt, contract, imagefs):
    """Recusa arquivo alterado, ausente, extra ou virado symlink."""
    records = [record for group in receipt["files"].values()
               for record in group]
    aliases = sorted(alias for group in receipt["soname_aliases"].values()
                     for alias in group)
    return imagefs.verify_records(
        root, records, aliases,
        complete_closures=contract["topology"]["roots"])


def check_multiarch_roots(root, contract, imagefs):
    counted = {}
    for abi, directories in contract["topology"]["roots"].items():
        total = 0
        for directory in directories:
            local = root / directory.lstrip("/")
            if not local.is_dir():
                continue
            for path in sorted(local.iterdir()):
                if path.is_symlink() or not path.is_file():
                    continue
                identity = imagefs.elf_identity(path)
                if identity is None:
                    continue
                require(identity == ARCH_ELF[abi],
                        "multiarch root %s holds a %s ELF: %s"
                        % (directory, identity, path.name))
                total += 1
        require(total > 0, "no %s runtime library was prepared" % abi)
        counted[abi] = total
    interpreters = contract["topology"]["interpreters"]
    for abi, interpreter in interpreters.items():
        local = root / interpreter.lstrip("/")
        require(local.is_file(), "missing %s interpreter: %s" % (abi, interpreter))
        require(imagefs.elf_identity(local) == ARCH_ELF[abi],
                "%s interpreter has the wrong ABI" % abi)
    return counted


def check_linker_scripts(root, contract, imagefs):
    """O script do GNU ld é artefato de build: reconhecido e ignorado."""
    scripts = []
    for path in contract["linker_scripts"]["expected_paths"]:
        local = root / path.lstrip("/")
        require(local.is_file(),
                "the image no longer ships the linker script %s" % path)
        require(imagefs.elf_identity(local) is None,
                "%s is an ELF; the fixture would prove nothing" % path)
        require(imagefs.is_gnu_ld_script(local),
                "%s is not recognized as a GNU ld script" % path)
        scripts.append(path)
    # A regra vale para a raiz inteira: nenhum arquivo não-ELF pode ser
    # confundido com biblioteca, e nenhum ELF de ABI errada pode passar.
    for abi, directories in contract["topology"]["roots"].items():
        for directory in directories:
            local = root / directory.lstrip("/")
            if not local.is_dir():
                continue
            for path in sorted(local.iterdir()):
                if path.is_symlink() or not path.is_file():
                    continue
                if imagefs.elf_identity(path) is not None:
                    continue
                require(imagefs.is_gnu_ld_script(path),
                        "unclassified non-ELF file in a runtime root: %s"
                        % path)
    return scripts


def check_sonames(root, contract, imagefs):
    """Cada soname declarado tem de ser provado, um por um.

    Antes, um soname ausente era simplesmente pulado e a contagem final ainda
    anunciava a cadeia inteira -- o número dizia mais do que a leitura provava.
    Agora a ausência é falha, e um ELF sem `DT_SONAME` também: sem o campo, não
    há o que conferir contra o nome pelo qual a biblioteca foi encontrada.
    """
    found = {}
    for key, abi in (("aarch64_sonames", "aarch64"),
                     ("armv7_sonames", "armv7")):
        chains = []
        for soname in contract["renderer"][key]:
            located = None
            for directory in contract["topology"]["roots"][abi]:
                candidate = root / directory.lstrip("/") / soname
                if candidate.exists() or candidate.is_symlink():
                    located = candidate
                    break
            require(located is not None,
                    "declared soname %s was not prepared for %s"
                    % (soname, abi))
            target = located.resolve()
            require(target.is_file() and not target.is_symlink(),
                    "soname %s does not resolve to a regular file" % soname)
            require(imagefs.elf_identity(target) == ARCH_ELF[abi],
                    "soname %s resolves to the wrong ABI" % soname)
            _, declared = imagefs.elf_dynamic(target)
            require(declared is not None,
                    "%s has no DT_SONAME, so the chain cannot be proven"
                    % soname)
            require(declared == soname,
                    "soname mismatch for %s: library declares %s"
                    % (soname, declared))
            chains.append("%s->%s" % (soname, target.name))
        require(len(chains) == len(contract["renderer"][key]),
                "not every declared %s soname was proven" % abi)
        found[abi] = chains

    # Provedores que a imagem entrega SEM DT_SONAME: presentes e com a ABI
    # conferida, mas sem cadeia a provar. Ficam contados à parte, porque somar
    # os dois anunciaria uma prova que não existe.
    providers = {}
    for key, abi in (("aarch64_unsonamed_providers", "aarch64"),
                     ("armv7_unsonamed_providers", "armv7")):
        names = []
        for provider in contract["renderer"].get(key, []):
            located = None
            for directory in contract["topology"]["roots"][abi]:
                candidate = root / directory.lstrip("/") / provider
                if candidate.exists() or candidate.is_symlink():
                    located = candidate
                    break
            require(located is not None,
                    "declared provider %s was not prepared for %s"
                    % (provider, abi))
            target = located.resolve()
            require(target.is_file() and not target.is_symlink(),
                    "provider %s does not resolve to a regular file" % provider)
            require(imagefs.elf_identity(target) == ARCH_ELF[abi],
                    "provider %s has the wrong ABI" % provider)
            _, declared = imagefs.elf_dynamic(target)
            require(declared is None,
                    "%s now declares DT_SONAME %s: move it to the proven "
                    "soname list" % (provider, declared))
            names.append(provider)
        providers[abi] = names
    return found, providers


def query_sdl_drivers(root, contract, abi, imagefs):
    clang = imagefs.resolve_tool("clang", required=False)
    qemu = imagefs.resolve_tool(QEMU[abi], required=False)
    require(clang and qemu,
            "clang and %s are required to measure the %s SDL" % (QEMU[abi], abi))
    library = root / contract["renderer"]["sdl"][abi].lstrip("/")
    require(library.exists(), "no SDL prepared for %s" % abi)
    interpreter = contract["topology"]["interpreters"][abi]
    search = [str(root / d.lstrip("/")) for d in
              contract["topology"]["roots"][abi]]
    with tempfile.TemporaryDirectory(prefix="arkos-sdl-probe.") as temporary:
        probe = Path(temporary) / ("sdl-drivers-%s" % abi)
        build = imagefs.run_guarded(
            [clang] + CLANG_TARGET[abi] + [
                "-O2", "-fuse-ld=lld", "-nostdlib", "-fno-builtin",
                "-fno-stack-protector",
                "-Wl,--dynamic-linker=%s" % interpreter,
                "-Wl,-e,_start", "-Wl,--no-as-needed",
                "-Wl,--allow-shlib-undefined",
                str(PROBE), "-L%s" % library.parent,
                "-l:%s" % library.name, "-o", str(probe),
            ], "clang")
        require(build.returncode == 0,
                "%s SDL probe failed to build:\n%s"
                % (abi, build.stdout.decode("utf-8", "replace")))
        run = imagefs.run_guarded(
            [qemu, str(root / interpreter.lstrip("/")),
             "--library-path", ":".join(search), str(probe)], QEMU[abi])
        output = run.stdout.decode("utf-8", "replace")
        require(run.returncode == 0,
                "%s SDL probe failed (%d):\n%s" % (abi, run.returncode, output))
    lines = dict(line.split("=", 1) for line in output.strip().splitlines()
                 if "=" in line)
    drivers = [item for item in lines.get("drivers", "").split(",") if item]
    require(contract["renderer"]["sdl"]["required_video_driver"] in drivers,
            "the %s SDL of the image does not compile %s: %s"
            % (abi, contract["renderer"]["sdl"]["required_video_driver"],
               drivers))
    return lines.get("sdl"), drivers


def check_profiles(contract):
    """Confere as rotas declaradas pelos perfis contra este layout multiarch.

    Inclui a rota mixed-ABI do dArkOS: instalador na ABI nativa do host,
    splash e jogo em ARMHF -- exatamente o que o recibo físico do port
    publicado registrou naquele aparelho.
    """
    document = json.loads(PROFILES.read_text(encoding="utf-8"))
    by_id = {profile["id"]: profile for profile in document["profiles"]}
    checked = []
    for profile_id, expectation in contract["profile_cross_check"].items():
        if profile_id == "note":
            continue
        require(profile_id in by_id, "profile %s disappeared" % profile_id)
        cases = by_id[profile_id]["arch_cases"]
        routes = [case["architecture"] for case in cases]
        require(routes == list(expectation["architectures"]),
                "profile %s declares %s, but this multiarch image supports %s"
                % (profile_id, routes, expectation["architectures"]))
        armv7 = next((case for case in cases
                      if case["architecture"] == "armv7"), None)
        require(armv7 is not None, "profile %s lost its ARMHF route"
                % profile_id)
        expected_ui = expectation["armv7_nxextract_ui_arch"]
        require(armv7["nxextract_ui_arch"] == expected_ui,
                "profile %s routes the ARMHF installer to %s, expected %s"
                % (profile_id, armv7["nxextract_ui_arch"], expected_ui))
        require(armv7["nxsplash_arch"] == "armv7",
                "profile %s no longer keeps the ARMHF splash" % profile_id)
        require(armv7["port_32bit"] is True and
                armv7["library_suffix"] == "libs.armhf",
                "profile %s ARMHF route lost its 32-bit contract" % profile_id)
        checked.append("%s:%s" % (profile_id, expected_ui))
    return sorted(checked)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--environment", required=True)
    parser.add_argument("--skip-sdl-query", action="store_true")
    arguments = parser.parse_args()

    imagefs = load_imagefs()
    environment = imagefs.environment_argument(arguments.environment)
    contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
    receipt = check_provenance(environment, contract, imagefs)
    root = environment / imagefs.fixed_layout(receipt, "root", "root")
    require(root.is_dir(), "prepared root is missing")

    pinned, alias_count = check_prepared_bytes(
        root, receipt, contract, imagefs)
    counted = check_multiarch_roots(root, contract, imagefs)
    scripts = check_linker_scripts(root, contract, imagefs)
    sonames, providers = check_sonames(root, contract, imagefs)
    profiles = check_profiles(contract)

    drivers = {}
    if not arguments.skip_sdl_query:
        for abi in ("aarch64", "armv7"):
            version, found = query_sdl_drivers(root, contract, abi, imagefs)
            drivers[abi] = (version, found)

    print("arkos environment gate passed: pinned_files=%d aliases=%d "
          "aarch64_elfs=%d armv7_elfs=%d "
          "ld_scripts=%d proven_sonames=%s unsonamed_providers=%s "
          "profiles=%s sdl=%s "
          "hardware_ran=0 device_access=0"
          % (pinned, alias_count, counted["aarch64"], counted["armv7"],
             len(scripts),
             "+".join("%s:%d" % (abi, len(chains))
                      for abi, chains in sorted(sonames.items())),
             "+".join("%s:%d" % (abi, len(names))
                      for abi, names in sorted(providers.items())),
             ",".join(profiles),
             ";".join("%s:%s:%s" % (abi, version, ",".join(found))
                      for abi, (version, found) in sorted(drivers.items()))
             or "skipped"))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except GateError as error:
        print("arkos environment gate failed: %s" % error, file=sys.stderr)
        sys.exit(1)
    except Exception as error:  # noqa: BLE001 - fronteira do gate
        # Falha de contencao do imagefs (byte trocado, sobra, symlink)
        # chega como excecao propria dele; o gate reporta igual.
        print("arkos environment gate failed: %s" % error, file=sys.stderr)
        sys.exit(1)
