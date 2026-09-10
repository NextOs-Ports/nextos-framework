#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Prepara no PC o material de VIEWPORT do Knulli, a partir da imagem oficial.

Mesmo molde estrutural dos outros ambientes locais -- contrato versionado,
preparação transacional fora do repositório e recibo de proveniência -- e
escopo deliberadamente estreito: só o que a firmware declara sobre o painel.
Ordem de botões, mapeamento e chord já têm gate próprio e não são duplicados.

A imagem nunca entra no Git e nunca é redistribuída. A leitura é somente
leitura: `mcopy` no deslocamento da partição e `unsquashfs` dos caminhos
declarados. Nada é montado, nada usa loop device e o arquivo de origem não
recebe um byte.

Uso:

    python3 framework/tests/device-environments/knulli/prepare.py \
      --image /caminho/para/knulli-h700-...img \
      --output /caminho/para/firmware-environments/knulli-20250813
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
CONTRACT = HERE / "contract-v1.json"


def load_imagefs():
    spec = importlib.util.spec_from_file_location(
        "imagefs", HERE.parent / "imagefs.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True)
    parser.add_argument("--output", required=True)
    arguments = parser.parse_args()

    imagefs = load_imagefs()
    require = imagefs.require
    contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
    firmware = contract["firmware"]
    layout = contract["image_layout"]

    raw_image = imagefs.safe_argument(arguments.image, "--image")
    require(raw_image.is_file() and not raw_image.is_symlink(),
            "official image not found or is a symlink: %s" % raw_image)
    image = imagefs.outside_repository(raw_image.resolve(strict=True),
                                       "official image")
    require(not image.name.endswith(".gz"),
            "decompress the official archive first; this preparer reads the "
            "raw image and never writes next to the source")
    require(image.stat().st_size == firmware["image_size"],
            "image size differs from the contract")
    print("knulli prepare: hashing the official image (%d bytes)"
          % firmware["image_size"])
    require(imagefs.sha256_file(image) == firmware["image_sha256"],
            "image SHA-256 differs from the contract")

    partition = imagefs.partition_named(image, layout["boot_partition_name"])
    require(partition["offset"] == layout["boot_offset"],
            "boot partition offset differs from the contract")
    require(imagefs.filesystem_kind(image, partition["offset"])
            == layout["filesystem"],
            "boot partition filesystem differs from the contract")

    raw_output = imagefs.safe_argument(arguments.output, "--output")
    require(not raw_output.exists() and not raw_output.is_symlink(),
            "output already exists or is a symlink: %s" % raw_output)
    require(raw_output.parent.is_dir() and not raw_output.parent.is_symlink(),
            "output parent is missing or is a symlink: %s" % raw_output.parent)
    output_parent = imagefs.outside_repository(
        raw_output.parent.resolve(strict=True), "output parent")
    output = imagefs.outside_repository(output_parent / raw_output.name,
                                        "output")
    staging = Path(tempfile.mkdtemp(prefix="knulli-environment.",
                                    dir=str(output_parent)))
    try:
        reader = imagefs.reader_for(image, partition["offset"])
        root = staging / "declared"
        records = imagefs.extract_files(reader, contract["seeds"]["boot"], root)

        # A imagem de sistema é um squashfs dentro do FAT. Ele sai para um
        # arquivo temporário, cede os caminhos declarados e é apagado: o
        # ambiente guarda só o material de viewport.
        unsquashfs = imagefs.resolve_tool("unsquashfs")
        with tempfile.TemporaryDirectory(prefix="knulli-system.",
                                         dir=str(output_parent)) as scratch:
            system = Path(scratch) / "system.squashfs"
            print("knulli prepare: extracting the system image (read-only)")
            reader.read(layout["system_image"], system)
            result = imagefs.run_guarded(
                [unsquashfs, "-d", str(root), "-f", "-n", str(system)]
                + contract["seeds"]["system"], "unsquashfs",
                timeout=15 * 60)
            require(result.returncode == 0,
                    "unsquashfs failed:\n%s"
                    % result.stdout.decode("utf-8", "replace"))
        for path in contract["seeds"]["system"]:
            local = imagefs.safe_logical_path(root, path)
            require(local.is_file() and not local.is_symlink()
                    and local.stat().st_size > 0,
                    "declared system file is missing: %s" % path)
            records.append(imagefs.file_record(local, path))
        records.sort(key=lambda record: record["path"])
        imagefs.verify_records(root, records)

        receipt = {
            "schema": "nxframework-prepared-device-environment-v1",
            "schema_version": 1,
            "environment_id": contract["id"],
            "contract_sha256": imagefs.sha256_file(CONTRACT),
            "source_image": {
                "name": firmware["image_name"],
                "size": firmware["image_size"],
                "sha256": firmware["image_sha256"],
                "boot_offset": partition["offset"],
                "read_method": layout["read_method"],
            },
            "evidence": {
                "kind": "prepared-device-environment",
                "hardware_ran": False,
                "device_access": False,
                "physical_runtime_claim": False,
            },
            "scope": contract["scope"],
            "layout": {"declared": "declared",
                       "receipt": "environment-receipt.json"},
            "inventory": [record["path"] for record in records],
            "files": records,
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

    print("knulli prepare: ready environment=%s files=%d scope=viewport "
          "hardware_ran=0 device_access=0" % (output, len(records)))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:  # noqa: BLE001 - fronteira do utilitário
        print("knulli prepare failed: %s" % error, file=sys.stderr)
        sys.exit(1)
