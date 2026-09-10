#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Prepara no PC o ambiente TrimUI/CrossMix a partir da distribuição OFICIAL.

Esta CFW não é distribuída como imagem de disco, e sim como a árvore do cartão
num ZIP. O molde do ambiente é o mesmo dos outros: contrato versionado com
tamanho e SHA-256 do arquivo oficial, preparação transacional fora do
repositório e recibo de proveniência. Nada é montado, nada é redistribuído e o
arquivo de origem não recebe um byte.

Uso:

    python3 framework/tests/device-environments/crossmix/prepare.py \
      --archive /caminho/para/CrossMix-OS_v1.3.0.zip \
      --output /caminho/para/firmware-environments/crossmix-v1.3.0
"""

import argparse
import importlib.util
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONTRACT = HERE / "contract-v1.json"


def load_imagefs():
    spec = importlib.util.spec_from_file_location(
        "imagefs", HERE.parent / "imagefs.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True)
    parser.add_argument("--output", required=True)
    arguments = parser.parse_args()

    imagefs = load_imagefs()
    require = imagefs.require
    contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
    firmware = contract["firmware"]

    raw_archive = imagefs.safe_argument(arguments.archive, "--archive")
    require(raw_archive.is_file() and not raw_archive.is_symlink(),
            "official distribution not found or is a symlink: %s" % raw_archive)
    archive = imagefs.outside_repository(raw_archive.resolve(strict=True),
                                         "official distribution")
    require(archive.stat().st_size == firmware["archive_size"],
            "archive size differs from the contract")
    print("crossmix prepare: hashing the official distribution (%d bytes)"
          % firmware["archive_size"])
    require(imagefs.sha256_file(archive) == firmware["archive_sha256"],
            "archive SHA-256 differs from the contract")

    raw_output = imagefs.safe_argument(arguments.output, "--output")
    require(not raw_output.exists() and not raw_output.is_symlink(),
            "output already exists or is a symlink: %s" % raw_output)
    require(raw_output.parent.is_dir() and not raw_output.parent.is_symlink(),
            "output parent is missing or is a symlink: %s" % raw_output.parent)
    output_parent = imagefs.outside_repository(
        raw_output.parent.resolve(strict=True), "output parent")
    output = imagefs.outside_repository(output_parent / raw_output.name,
                                        "output")
    staging = Path(tempfile.mkdtemp(prefix="crossmix-environment.",
                                    dir=str(output_parent)))
    try:
        reader = imagefs.ZipReader(archive)
        root = staging / "card"
        inventory = []
        # Só os caminhos de arquivo do bloco runtime entram; as declarações
        # (pastas que o control.txt anuncia) são conferidas pelo verificador.
        wanted = [contract["runtime"][key] for key in
                  ("control", "device_variant_control", "modular",
                   "device_info", "gl_defaults")]
        wanted += contract["libraries"]["aarch64"]
        wanted.append(contract["libraries"]["runtime_probe"])
        wanted.append(contract["controller_discovery"]["database"])
        for path in wanted:
            require(reader.exists(path),
                    "the official distribution lacks %s" % path)
        records = imagefs.extract_files(reader, wanted, root)
        imagefs.verify_records(root, records)
        # Auditoria de ABI do diretório INTEIRO de bibliotecas da firmware:
        # a alegação deixa de valer só para os arquivos escolhidos.
        audit = imagefs.audit_directory_abi(
            reader, contract["libraries"]["audited_directory"],
            contract["libraries"]["audit_abi"])

        receipt = {
            "schema": "nxframework-prepared-device-environment-v1",
            "schema_version": 1,
            "environment_id": contract["id"],
            "contract_sha256": imagefs.sha256_file(CONTRACT),
            "source_archive": {
                "name": firmware["archive_name"],
                "size": firmware["archive_size"],
                "sha256": firmware["archive_sha256"],
                "distribution": firmware["distribution"],
                "read_method": "read-only zip member extraction; nothing "
                               "mounted, nothing written next to the source",
            },
            "evidence": {
                "kind": "prepared-device-environment",
                "hardware_ran": False,
                "device_access": False,
                "physical_runtime_claim": False,
            },
            "scope": contract["scope"],
            "layout": {"card": "card", "receipt": "environment-receipt.json"},
            "inventory": [record["path"] for record in records],
            "files": records,
            "library_audit": audit,
        }
        (staging / "environment-receipt.json").write_text(
            json.dumps(receipt, indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        # Confere de novo, agora colado no rename: entre a validação inicial
        # e este ponto passaram minutos de leitura da imagem.
        require(not output.exists() and not output.is_symlink(),
                "output appeared during preparation: %s" % output)
        os.rename(str(staging), str(output))
    except BaseException:
        shutil.rmtree(str(staging), ignore_errors=True)
        raise

    print("crossmix prepare: ready environment=%s files=%d audited=%d "
          "hardware_ran=0 device_access=0"
          % (output, len(records), audit["elves"]))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:  # noqa: BLE001 - fronteira do utilitário
        print("crossmix prepare failed: %s" % error, file=sys.stderr)
        sys.exit(1)
