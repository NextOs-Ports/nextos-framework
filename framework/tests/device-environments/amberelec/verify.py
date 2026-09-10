#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Gate do ambiente AmberELEC/P4ELEC preparado a partir da imagem oficial.

Confere, sem abrir nenhum dispositivo de áudio:

- proveniência: recibo, contrato e SHA-256 da imagem;
- **OpenAL**: a biblioteca que a firmware entrega e a configuração que ela
  impõe a qualquer OpenAL que leia o arquivo do sistema -- inclusive um
  embutido no port;
- **ALSA/Pulse**: quais back-ends realmente existem na imagem;
- **ambiente de login**: os fragmentos de perfil que mudam o ambiente antes de
  o port rodar;
- a regra do framework: o back-end é decidido por **capability declarada pelo
  port**, nunca por nome de firmware ou de aparelho.
"""

import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path, PurePosixPath

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parents[3]
CONTRACT = HERE / "contract-v1.json"
GENERATOR = REPOSITORY / "framework/nxbootstrap/tools/generate-port.py"
TEMPLATE = REPOSITORY / "framework/nxbootstrap/templates/launcher.sh.in"
ARM32 = (1, 40)
ARM64 = (2, 183)


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


def check_audio(system, contract, imagefs):
    """OpenAL, ALSA e Pulse: ELF regular, ABI conferida e config medida."""
    audio = contract["audio"]
    real = system / audio["openal_real_file"].lstrip("/")
    require(real.is_file() and not real.is_symlink(),
            "the firmware OpenAL library was not prepared")
    identity = imagefs.elf_identity(real)
    require(identity in (ARM32, ARM64),
            "the firmware OpenAL is not an ARM ELF: %s" % (identity,))

    configuration = system / audio["openal_configuration"].lstrip("/")
    require(configuration.is_file() and not configuration.is_symlink(),
            "the firmware ships no OpenAL configuration; this environment "
            "would be asserting a mute source that does not exist")
    body = configuration.read_text(encoding="utf-8", errors="replace")
    setting = audio["forced_driver_setting"]
    require(re.search(r"^%s\s*$" % re.escape(setting), body, re.MULTILINE),
            "the firmware OpenAL configuration no longer forces %r" % setting)

    # ALSA e Pulse entram como ELF REGULAR da mesma ABI do OpenAL. Antes
    # bastava o caminho existir -- um link pendente contava como back-end.
    present = []
    for key in ("alsa_real_file", "pulse_real_file"):
        path = system / audio[key].lstrip("/")
        require(path.is_file() and not path.is_symlink(),
                "declared backend is not a regular file: %s" % audio[key])
        backend_identity = imagefs.elf_identity(path)
        require(backend_identity == identity,
                "%s is %s while OpenAL is %s"
                % (audio[key], backend_identity, identity))
        present.append(Path(audio[key]).name)
    require(present, "neither ALSA nor Pulse is shipped by this image")
    return identity, setting, present


def check_login_environment(system, contract):
    """Prova que o perfil de login CARREGA o fragmento, não só que ele existe."""
    login = contract["login_environment"]
    profile = system / login["profile"].lstrip("/")
    require(profile.is_file() and not profile.is_symlink(),
            "the login profile was not prepared")
    profile_text = profile.read_text(encoding="utf-8", errors="replace")
    pattern = login["loader_pattern"]
    require(pattern in profile_text,
            "the login profile no longer reads %s" % pattern)
    require(re.search(r"^\s*\.\s+\$config", profile_text, re.MULTILINE)
            or re.search(r"^\s*source\s+\$config", profile_text, re.MULTILINE),
            "the login profile no longer sources what it lists")

    variable = login["inherited_variable"]
    touched = []
    for fragment in login["fragments"]:
        path = system / fragment.lstrip("/")
        require(path.is_file() and not path.is_symlink(),
                "login fragment was not prepared: %s" % fragment)
        directory = str(PurePosixPath(fragment).parent)
        require(pattern.startswith(directory),
                "fragment %s is outside the directory the profile loads (%s)"
                % (fragment, pattern))
        text = path.read_text(encoding="utf-8", errors="replace")
        if variable in text:
            touched.append(PurePosixPath(fragment).name)
    require(touched,
            "no login fragment touches %s; the inherited environment this "
            "environment describes would not exist" % variable)
    return touched


def check_framework_rule(contract):
    """A escolha de back-end vem de capability, nunca de nome de aparelho.

    A varredura é GLOBAL: qualquer linha do gerador ou do template que decida
    algo a partir de `CFW_NAME`/`DEVICE_NAME` reprova, e não só as linhas que
    por acaso mencionam áudio.
    """
    rule = contract["framework_rule"]
    registry = json.loads(
        (REPOSITORY / rule["registry"]).read_text(encoding="utf-8"))
    entries = {item["id"]: item for item in registry["capabilities"]}
    capability = rule["capability"]
    require(capability in entries,
            "the capability %s disappeared from the registry" % capability)
    entry = entries[capability]
    require(entry.get("role") == "port-declared",
            "%s stopped being declared by the port" % capability)
    require(entry.get("source") == "probe",
            "%s stopped being decided by a probe" % capability)

    selectors = "|".join(re.escape(name) for name in rule["forbidden_selectors"])
    branch = re.compile(r"(?:^|\s)(?:if|elif|case|test|\[\[?)\b[^\n]*"
                        r"\$\{?(%s)\b" % selectors)
    # Atribuição AUTÔNOMA. `IFS= read ...` é prefixo de comando, não decisão.
    assignment = re.compile(r"""^\s*(?:export\s+|declare\s+-x\s+)?
                                ([A-Za-z_][A-Za-z0-9_]*)=
                                ("[^"]*"|'[^']*'|\S*)\s*(?:\#.*)?$""",
                            re.VERBOSE)
    terminator = re.compile(r"(?:;;|^\s*(?:fi|esac|done)\b)")
    scanned = 0
    for name, path in (("generator", GENERATOR), ("template", TEMPLATE)):
        lines = path.read_text(encoding="utf-8").splitlines()
        scanned += len(lines)
        for index, line in enumerate(lines):
            match = branch.search(line)
            if not match:
                continue
            selector = match.group(1)
            # Um ramo que só SANEIA o valor (unset, validação, log) é
            # legítimo. O que não pode é um ramo por aparelho DECIDIR
            # ambiente de execução: isso aparece como atribuição dentro dele.
            for offset, follow in enumerate(lines[index:index + 20]):
                if offset and terminator.search(follow):
                    break
                if offset == 0 and terminator.search(follow):
                    # Ramo de uma linha só: não há corpo a inspecionar.
                    break
                assigned = assignment.match(follow)
                if not assigned:
                    continue
                if assigned.group(1) in rule["forbidden_selectors"]:
                    continue
                raise GateError(
                    "the %s decides %s from %s at line %d: %s"
                    % (name, assigned.group(1), selector, index + 1,
                       follow.strip()))
    require(scanned > 0, "nothing was scanned for device-keyed selection")
    return capability, scanned


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--environment", required=True)
    arguments = parser.parse_args()
    imagefs = load_imagefs()
    environment = imagefs.environment_argument(arguments.environment)

    contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
    receipt = check_provenance(environment, contract, imagefs)
    system = environment / imagefs.fixed_layout(receipt, "system", "system")
    require(system.is_dir(), "prepared system tree is missing")

    pinned, alias_count = imagefs.verify_records(
        system, receipt["files"], receipt.get("soname_aliases", []))
    identity, setting, backends = check_audio(system, contract, imagefs)
    fragments = check_login_environment(system, contract)
    capability, scanned = check_framework_rule(contract)

    print("amberelec environment gate passed: pinned_files=%d aliases=%d "
          "openal=%s forced=%s "
          "backends=%s login_fragments=%s capability=%s scanned_lines=%d "
          "firmware=AmberELEC-only backend_chosen_by_device=no "
          "hardware_ran=0 device_access=0"
          % (pinned, alias_count,
             "armv7" if identity == ARM32 else "aarch64", setting,
             ",".join(sorted(backends)), ",".join(sorted(fragments)),
             capability, scanned))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except GateError as error:
        print("amberelec environment gate failed: %s" % error, file=sys.stderr)
        sys.exit(1)
    except Exception as error:  # noqa: BLE001 - fronteira do gate
        # Falha de contencao do imagefs (byte trocado, sobra, symlink)
        # chega como excecao propria dele; o gate reporta igual.
        print("amberelec environment gate failed: %s" % error, file=sys.stderr)
        sys.exit(1)
