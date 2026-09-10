#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Prepara no PC o ambiente ArkOS/dArkOSRE a partir da imagem OFICIAL.

Mesmo molde estrutural dos outros ambientes locais, com o escopo deste item:
raízes multiarch, o script de linker `libc.so` do Debian multiarch, as cadeias
de soname de EGL/GLES e a SDL com KMSDRM que a imagem entrega.

A imagem nunca entra no Git e nunca é redistribuída. A leitura é somente
leitura (`debugfs` com deslocamento da partição): sem montar, sem loop device,
sem root e sem escrever um byte no arquivo de origem.

Uso:

    python3 framework/tests/device-environments/arkos/prepare.py \
      --image /caminho/para/arkos-r36s-v2.0.img \
      --output /caminho/para/firmware-environments/arkos-r36s-v2.0
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
    require(not image.name.endswith((".xz", ".gz")),
            "decompress the official archive first; this preparer reads the "
            "raw image and never writes next to the source")
    require(image.stat().st_size == firmware["image_size"],
            "image size differs from the contract")
    print("arkos prepare: hashing the official image (%d bytes)"
          % firmware["image_size"])
    require(imagefs.sha256_file(image) == firmware["image_sha256"],
            "image SHA-256 differs from the contract")

    partition = imagefs.partition_at(image, layout["rootfs_offset"])
    require(imagefs.filesystem_kind(image, partition["offset"])
            == layout["filesystem"],
            "rootfs filesystem differs from the contract")

    raw_output = imagefs.safe_argument(arguments.output, "--output")
    require(not raw_output.exists() and not raw_output.is_symlink(),
            "output already exists or is a symlink: %s" % raw_output)
    require(raw_output.parent.is_dir() and not raw_output.parent.is_symlink(),
            "output parent is missing or is a symlink: %s" % raw_output.parent)
    output_parent = imagefs.outside_repository(
        raw_output.parent.resolve(strict=True), "output parent")
    output = imagefs.outside_repository(output_parent / raw_output.name,
                                        "output")
    staging = Path(tempfile.mkdtemp(prefix="arkos-environment.",
                                    dir=str(output_parent)))
    try:
        reader = imagefs.reader_for(image, partition["offset"])
        root = staging / "root"
        files = {}
        aliases = {}
        for abi in ("aarch64", "armv7"):
            files[abi], aliases[abi] = imagefs.extract_closure(
                reader, contract["seeds"][abi],
                contract["topology"]["roots"][abi], root, abi)

        # Arquivos que entram exatamente como estão, sem seguir link nem
        # fechamento: é assim que o script de linker chega ao ambiente.
        files["verbatim"] = imagefs.extract_files(
            reader, contract["seeds"]["verbatim"], root)
        aliases["verbatim"] = []
        all_records = [record for records in files.values()
                       for record in records]
        all_aliases = sorted(alias for group in aliases.values()
                             for alias in group)
        imagefs.verify_records(
            root, all_records, all_aliases,
            complete_closures=contract["topology"]["roots"])

        receipt = {
            "schema": "nxframework-prepared-device-environment-v1",
            "schema_version": 1,
            "environment_id": contract["id"],
            "contract_sha256": imagefs.sha256_file(CONTRACT),
            "source_image": {
                "name": firmware["image_name"],
                "size": firmware["image_size"],
                "sha256": firmware["image_sha256"],
                "rootfs_offset": partition["offset"],
                "read_method": layout["read_method"],
            },
            "evidence": {
                "kind": "prepared-device-environment",
                "hardware_ran": False,
                "device_access": False,
                "physical_runtime_claim": False,
            },
            "scope": contract["scope"],
            "layout": {"root": "root", "receipt": "environment-receipt.json"},
            # `inventory` mantém o formato legível; `files` é o recibo forte,
            # com tamanho, SHA-256, ABI e dinâmica de cada arquivo.
            "inventory": {group: [record["path"] for record in records]
                          for group, records in files.items()},
            "files": files,
            "soname_aliases": {group: list(group_aliases)
                               for group, group_aliases in aliases.items()},
            "counts": {group: len(records)
                       for group, records in files.items()},
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

    print("arkos prepare: ready environment=%s aarch64=%d armv7=%d "
          "verbatim=%d aliases=%d hardware_ran=0 device_access=0"
          % (output, receipt["counts"]["aarch64"], receipt["counts"]["armv7"],
             receipt["counts"]["verbatim"], len(all_aliases)))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:  # noqa: BLE001 - fronteira do utilitário
        print("arkos prepare failed: %s" % error, file=sys.stderr)
        sys.exit(1)
