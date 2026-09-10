#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Gate do ambiente TrimUI/CrossMix preparado a partir da distribuição oficial.

Confere, sem tocar em aparelho:

- proveniência: recibo, contrato e SHA-256 do arquivo oficial;
- as **raízes do cartão** que o `control.txt` da firmware declara;
- a ABI das **bibliotecas** que ela entrega e o sufixo de runtime dos ports;
- as declarações de **SDL/runtime** (banco de controles, GL4ES, helper de
  plataforma);
- a **descoberta do controle**: a variável que a firmware exporta, o banco que
  ela entrega e o que esse banco tem — ou não tem — sobre este aparelho.

A tabela de botões **não** é re-derivada aqui: ela é o caso já validado
`trimui-smartpro-crossmix` em `fixtures/controls`. Este gate só prova que a
firmware oficial continua coerente com a conclusão daquele caso.
"""

import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parents[3]
CONTRACT = HERE / "contract-v1.json"
CONTROLS = REPOSITORY / "framework/tests/fixtures/controls/controls-v1.json"
PROFILES = REPOSITORY / "framework/tests/firmware-profiles-v2.json"
AARCH64 = (2, 183)


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
    source = receipt["source_archive"]
    firmware = contract["firmware"]
    require(source["sha256"] == firmware["archive_sha256"]
            and source["size"] == firmware["archive_size"],
            "receipt does not describe the official distribution")
    require(not any(receipt["evidence"][key] for key in
                    ("hardware_ran", "device_access", "physical_runtime_claim")),
            "the receipt claims hardware evidence it cannot have")
    require(not firmware["stored_in_repository"]
            and not firmware["redistributed_by_framework"],
            "the contract must keep the distribution out of the repository")
    require("files" in receipt,
            "prepared receipt predates per-file pins; prepare it again")
    return receipt


def check_roots_and_runtime(card, contract):
    """Lê o control.txt REALMENTE executado: o da raiz do PortMaster.

    A variante em `trimui/` existe na imagem, mas quem o PortMaster carrega é
    `$controlfolder/control.txt`. Validar a variante provaria outra coisa.
    """
    runtime = contract["runtime"]
    control = card / runtime["control"].lstrip("/")
    require(control.is_file() and not control.is_symlink(),
            "the firmware control.txt was not prepared")
    text = control.read_text(encoding="utf-8", errors="replace")

    declared = runtime["declared_controlfolder"]
    require(re.search(r'^controlfolder="%s"' % re.escape(declared), text,
                      re.MULTILINE),
            "the firmware no longer declares the PortMaster root %s" % declared)
    data = runtime["declared_data_directory"]
    require(re.search(r'^directory="%s"' % re.escape(data), text, re.MULTILINE),
            "the firmware no longer declares the data root %s" % data)
    card_root = contract["topology"]["card_root"]
    require(declared.startswith(card_root),
            "the declared PortMaster root left the card root")
    ports_root = contract["topology"]["ports_root"]
    require(ports_root == "/%s/ports" % data.strip("/"),
            "the contract ports root %s does not follow the firmware's "
            "declared data directory %s" % (ports_root, data))

    # A variante por aparelho é registrada no ambiente, mas quem prova o
    # contrato é a da raiz: se um dia a variante deixar de existir, o gate
    # continua válido; se ela passar a ser a executada, o contrato muda.
    variant = card / runtime["device_variant_control"].lstrip("/")
    require(variant.is_file() and not variant.is_symlink(),
            "the device variant control.txt was not prepared")

    modular = card / runtime["modular"].lstrip("/")
    require(modular.is_file() and not modular.is_symlink(),
            "the modular firmware file was not prepared")
    modular_text = modular.read_text(encoding="utf-8", errors="replace")
    require("pm_platform_helper" in modular_text,
            "the firmware no longer provides the PortMaster platform helper")
    require("DISPLAY_WIDTH" in modular_text and "DISPLAY_HEIGHT" in modular_text,
            "the firmware stopped deriving geometry from the runtime")

    gl_defaults = card / runtime["gl_defaults"].lstrip("/")
    require(gl_defaults.is_file() and not gl_defaults.is_symlink(),
            "the GL defaults file was not prepared")
    gl_text = gl_defaults.read_text(encoding="utf-8", errors="replace")
    for token in runtime["gl4es_defaults"]:
        require(token in gl_text,
                "the firmware GL default changed: %s missing" % token)

    device_info = card / runtime["device_info"].lstrip("/")
    require(device_info.is_file() and not device_info.is_symlink(),
            "device_info.txt was not prepared")
    require("source $controlfolder/device_info.txt" in text,
            "control.txt no longer sources device_info.txt")

    document = json.loads(PROFILES.read_text(encoding="utf-8"))
    profile = next((item for item in document["profiles"]
                    if item["id"] == "trimui"), None)
    require(profile is not None, "the trimui profile disappeared")
    expectation = contract["profile_cross_check"]["trimui"]
    for key, value in expectation.items():
        require(profile["portmaster"][key] == value,
                "the trimui profile %s is %r, this image declares %r"
                % (key, profile["portmaster"][key], value))
    return declared, ports_root


def check_libraries(card, contract, imagefs, receipt):
    """Fixa por bytes o subconjunto usado e audita o diretório inteiro.

    Antes o gate anunciava "N bibliotecas" tendo conferido só as que extraiu.
    Agora as duas contas aparecem separadas: as fixadas por byte e as
    auditadas por ABI na origem.
    """
    checked = []
    for path in contract["libraries"]["aarch64"]:
        local = card / path.lstrip("/")
        require(local.is_file() and not local.is_symlink(),
                "library was not prepared: %s" % path)
        identity = imagefs.elf_identity(local)
        require(identity == AARCH64,
                "%s is not an AArch64 ELF: %s" % (path, identity))
        checked.append(Path(path).name)
    probe = card / contract["libraries"]["runtime_probe"].lstrip("/")
    require(probe.is_file() and not probe.is_symlink(),
            "the runtime library probe was not prepared")
    require(imagefs.elf_identity(probe) == AARCH64,
            "the PortMaster runtime library is not AArch64")
    suffix = contract["topology"]["runtime_library_suffix"]
    require(suffix in contract["libraries"]["runtime_probe"],
            "the runtime library suffix declared by the contract is not the "
            "one the firmware ships")

    audit = receipt.get("library_audit")
    require(audit, "prepared receipt has no library audit; prepare it again")
    require(audit["directory"] == contract["libraries"]["audited_directory"],
            "the audit covers %s, the contract declares %s"
            % (audit["directory"], contract["libraries"]["audited_directory"]))
    require(audit["expected_abi"] == contract["libraries"]["audit_abi"],
            "the audit expected another ABI")
    require(audit["elves"] > 0, "the audit found no ELF to classify")
    require(not audit["foreign"],
            "the firmware library directory mixes ABIs: %s"
            % ", ".join(audit["foreign"]))
    return checked, audit


def check_controller_discovery(card, contract):
    """Prova a cadeia inteira: variável, caminho exato, banco e o caso validado."""
    discovery = contract["controller_discovery"]
    control = card / contract["runtime"]["control"].lstrip("/")
    text = control.read_text(encoding="utf-8", errors="replace")
    variable = discovery["environment_variable"]
    match = re.search(r'^export %s="([^"]+)"' % re.escape(variable), text,
                      re.MULTILINE)
    require(match, "the firmware no longer exports %s" % variable)
    exported = match.group(1)
    require(exported == discovery["exported_value"],
            "the exported controller database is %r, the contract says %r"
            % (exported, discovery["exported_value"]))
    controlfolder = contract["runtime"]["declared_controlfolder"]
    resolved = exported.replace("$controlfolder", controlfolder)
    require(resolved == discovery["resolved_database"],
            "the exported path resolves to %r, the contract says %r"
            % (resolved, discovery["resolved_database"]))
    require(resolved.rstrip("/").endswith(
            discovery["database"].lstrip("/").split("/")[-1]),
            "the exported database is not the file shipped by the firmware")

    database = card / discovery["database"].lstrip("/")
    require(database.is_file() and not database.is_symlink(),
            "the controller database was not prepared")
    body = database.read_text(encoding="utf-8", errors="replace")
    require(len(body.splitlines()) > 100,
            "the shipped controller database looks truncated")

    # Reuso -- não re-derivação -- do caso já validado, campo a campo.
    cases = json.loads(CONTROLS.read_text(encoding="utf-8"))["cases"]
    case = next((item for item in cases
                 if item["id"] == discovery["validated_case"]), None)
    require(case is not None,
            "the validated controls case %s disappeared"
            % discovery["validated_case"])
    require(case["sdl_mapping_suffix"] == discovery["expected_mapping_suffix"],
            "the validated case now carries an SDL mapping: %r"
            % case["sdl_mapping_suffix"])
    require(case["expect_authority"] == discovery["expected_authority"],
            "the validated case authority changed to %r"
            % case["expect_authority"])
    require(case["expect_source"] == discovery["expected_source"],
            "the validated case source changed to %r" % case["expect_source"])
    require(case["expect_select"] == discovery["expected_select"] and
            case["expect_start"] == discovery["expected_start"],
            "the validated case SELECT/START changed to %s/%s"
            % (case["expect_select"], case["expect_start"]))
    for code in (case["expect_select"], case["expect_start"]):
        require(code in case["evdev_keys"],
                "code %s is not in the evdev table of the validated case"
                % code)
    for identity in discovery["expected_absent_identities"]:
        require(identity.lower() not in body.lower(),
                "the shipped database now names %r: the validated case "
                "concluded the opposite and must be revisited" % identity)
    return case["id"], case["expect_select"], case["expect_start"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--environment", required=True)
    arguments = parser.parse_args()
    imagefs = load_imagefs()
    environment = imagefs.environment_argument(arguments.environment)

    contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
    receipt = check_provenance(environment, contract, imagefs)
    card = environment / imagefs.fixed_layout(receipt, "card", "card")
    require(card.is_dir(), "prepared card tree is missing")

    pinned, _ = imagefs.verify_records(card, receipt["files"])
    portmaster, ports_root = check_roots_and_runtime(card, contract)
    libraries, audit = check_libraries(card, contract, imagefs, receipt)
    case, select, start = check_controller_discovery(card, contract)

    print("crossmix environment gate passed: pinned_files=%d "
          "portmaster_root=%s ports_root=%s pinned_libraries=%d "
          "audited_libraries=%d runtime_suffix=%s controls_case=%s "
          "authority=evdev-fallback select=%s start=%s "
          "hardware_ran=0 device_access=0"
          % (pinned, portmaster, ports_root, len(libraries), audit["elves"],
             contract["topology"]["runtime_library_suffix"], case, select,
             start))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except GateError as error:
        print("crossmix environment gate failed: %s" % error, file=sys.stderr)
        sys.exit(1)
    except Exception as error:  # noqa: BLE001 - fronteira do gate
        # Falha de contencao do imagefs (byte trocado, sobra, symlink)
        # chega como excecao propria dele; o gate reporta igual.
        print("crossmix environment gate failed: %s" % error, file=sys.stderr)
        sys.exit(1)
