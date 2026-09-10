#!/usr/bin/env python3
"""Prepare the two Sally Face textures that exceed Mali-450's 4096px limit.

This is deliberately a source-to-output transform.  It accepts only the exact
pristine ``sharedassets106.assets``/``.resS`` pair from Sally Face 1.5.51,
changes only ``os3_BG`` and ``os3_BG2`` once, and writes a new serialized file
without mutating or backing up either source file.  Re-running it from the same
source produces the same bytes and leaves an identical output untouched.

No other texture is resized here.  Runtime ETC1 RGB+alpha conversion remains a
separate compatibility step in the native adapter.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import UnityPy
from PIL import Image


SCHEMA = "nextos.sallyface.oversize-fix.v1"
SOURCE_ASSETS_NAME = "sharedassets106.assets"
SOURCE_RESS_NAME = "sharedassets106.assets.resS"
SOURCE_ASSETS_SIZE = 140_656
SOURCE_RESS_SIZE = 60_239_840
SOURCE_ASSETS_SHA256 = (
    "85311fffda355bb3dfc19c4cdb2ada4d33995907a30bdd86fe5e0215a940d539"
)
SOURCE_RESS_SHA256 = (
    "0245854605aff30f070ce70b56dffbbd3a66716c5e7a51acf60eaa4dfcd3a876"
)
OUTPUT_ASSETS_SIZE = 15_188_296
OUTPUT_ASSETS_SHA256 = (
    "22145289e107499203e3bbd4ea8b1de2787acb2733f45bfbe25b8ca7b2c5de90"
)


class ContractError(RuntimeError):
    """The source or generated output does not match the closed contract."""


@dataclass(frozen=True)
class TextureContract:
    name: str
    source_width: int
    source_height: int
    output_width: int
    output_height: int
    texture_format: int
    stream_offset: int
    stream_size: int


TEXTURES = (
    TextureContract(
        name="os3_BG2",
        source_width=6018,
        source_height=833,
        output_width=3009,
        output_height=416,
        texture_format=4,  # Unity TextureFormat.RGBA32
        stream_offset=0,
        stream_size=20_051_976,
    ),
    TextureContract(
        name="os3_BG",
        source_width=6252,
        source_height=1607,
        output_width=3126,
        output_height=803,
        texture_format=4,
        stream_offset=20_051_984,
        stream_size=40_187_856,
    ),
)
TEXTURES_BY_NAME = {item.name: item for item in TEXTURES}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def canonical_json(value: Any) -> bytes:
    return (
        json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
        + "\n"
    ).encode("utf-8")


def _regular_file(path: Path, label: str) -> os.stat_result:
    try:
        info = path.lstat()
    except FileNotFoundError as exc:
        raise ContractError(f"{label} ausente: {path}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise ContractError(f"{label} precisa ser arquivo regular, sem symlink: {path}")
    return info


def _require_distinct(source: Path, output: Path) -> None:
    source_real = source.resolve(strict=True)
    output_real = output.parent.resolve(strict=True) / output.name
    if source_real == output_real:
        raise ContractError("source e output precisam ser caminhos distintos")


def _check_identity(path: Path, name: str, size: int, digest: str) -> None:
    info = _regular_file(path, name)
    if path.name != name:
        raise ContractError(f"nome inesperado para {name}: {path.name!r}")
    if info.st_size != size:
        raise ContractError(
            f"tamanho inesperado para {name}: {info.st_size}, esperado {size}"
        )
    found = sha256_file(path)
    if found != digest:
        raise ContractError(
            f"SHA-256 inesperado para {name}: {found}, esperado {digest}"
        )


def inspect_source(source_assets: Path, source_ress: Path) -> dict[str, Any]:
    """Validate the exact pristine pair without changing it."""

    source_assets = Path(source_assets)
    source_ress = Path(source_ress)
    if source_assets.parent.resolve() != source_ress.parent.resolve():
        raise ContractError(".assets e .resS de origem precisam estar no mesmo diretorio")
    _check_identity(
        source_assets,
        SOURCE_ASSETS_NAME,
        SOURCE_ASSETS_SIZE,
        SOURCE_ASSETS_SHA256,
    )
    _check_identity(
        source_ress,
        SOURCE_RESS_NAME,
        SOURCE_RESS_SIZE,
        SOURCE_RESS_SHA256,
    )

    environment = UnityPy.load(str(source_assets))
    seen: dict[str, dict[str, Any]] = {}
    for obj in environment.objects:
        if obj.type.name != "Texture2D":
            continue
        data = obj.read()
        name = str(data.m_Name)
        if name not in TEXTURES_BY_NAME:
            raise ContractError(f"Texture2D inesperada em {SOURCE_ASSETS_NAME}: {name!r}")
        if name in seen:
            raise ContractError(f"Texture2D duplicada: {name}")
        expected = TEXTURES_BY_NAME[name]
        stream = getattr(data, "m_StreamData", None)
        actual = {
            "path_id": int(obj.path_id),
            "width": int(data.m_Width),
            "height": int(data.m_Height),
            "format": int(data.m_TextureFormat),
            "stream_path": str(getattr(stream, "path", "")),
            "stream_offset": int(getattr(stream, "offset", -1)),
            "stream_size": int(getattr(stream, "size", -1)),
        }
        wanted = {
            "width": expected.source_width,
            "height": expected.source_height,
            "format": expected.texture_format,
            "stream_path": SOURCE_RESS_NAME,
            "stream_offset": expected.stream_offset,
            "stream_size": expected.stream_size,
        }
        for key, value in wanted.items():
            if actual[key] != value:
                raise ContractError(
                    f"{name}.{key}={actual[key]!r}, esperado {value!r}"
                )
        if expected.stream_offset + expected.stream_size > SOURCE_RESS_SIZE:
            raise ContractError(f"faixa de {name} ultrapassa o .resS")
        seen[name] = actual

    if set(seen) != set(TEXTURES_BY_NAME):
        missing = sorted(set(TEXTURES_BY_NAME) - set(seen))
        raise ContractError(f"texturas obrigatorias ausentes: {missing}")

    return {
        "schema": SCHEMA,
        "source": {
            SOURCE_ASSETS_NAME: {
                "bytes": SOURCE_ASSETS_SIZE,
                "sha256": SOURCE_ASSETS_SHA256,
            },
            SOURCE_RESS_NAME: {
                "bytes": SOURCE_RESS_SIZE,
                "sha256": SOURCE_RESS_SHA256,
            },
        },
        "textures": [
            {
                "name": item.name,
                "source": [item.source_width, item.source_height],
                "output": [item.output_width, item.output_height],
                "format": item.texture_format,
                "stream_offset": item.stream_offset,
                "stream_size": item.stream_size,
            }
            for item in TEXTURES
        ],
    }


def _atomic_write_if_changed(path: Path, payload: bytes, mode: int = 0o644) -> bool:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_symlink():
        raise ContractError(f"output nao pode ser symlink: {path}")
    if path.exists() and path.read_bytes() == payload:
        return False
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()
    return True


def inspect_output(output_assets: Path) -> dict[str, Any]:
    """Validate the generated serialized file and its two inline textures."""

    output_assets = Path(output_assets)
    info = _regular_file(output_assets, "output assets")
    if info.st_size != OUTPUT_ASSETS_SIZE:
        raise ContractError(
            f"output tem {info.st_size} bytes; esperado {OUTPUT_ASSETS_SIZE}"
        )
    output_hash = sha256_file(output_assets)
    if output_hash != OUTPUT_ASSETS_SHA256:
        raise ContractError(
            f"output SHA-256 {output_hash}; esperado {OUTPUT_ASSETS_SHA256}"
        )
    output_ress = output_assets.with_name(SOURCE_RESS_NAME)
    if output_ress.exists() or output_ress.is_symlink():
        raise ContractError(f"output nao pode conter {SOURCE_RESS_NAME}")

    environment = UnityPy.load(str(output_assets))
    seen: dict[str, dict[str, Any]] = {}
    for obj in environment.objects:
        if obj.type.name != "Texture2D":
            continue
        data = obj.read()
        name = str(data.m_Name)
        expected = TEXTURES_BY_NAME.get(name)
        if expected is None:
            raise ContractError(f"Texture2D inesperada no output: {name!r}")
        stream = getattr(data, "m_StreamData", None)
        actual = {
            "path_id": int(obj.path_id),
            "width": int(data.m_Width),
            "height": int(data.m_Height),
            "format": int(data.m_TextureFormat),
            "stream_path": str(getattr(stream, "path", "")),
            "stream_offset": int(getattr(stream, "offset", 0)),
            "stream_size": int(getattr(stream, "size", 0)),
        }
        wanted = {
            "width": expected.output_width,
            "height": expected.output_height,
            "format": expected.texture_format,
            "stream_path": "",
            "stream_offset": 0,
            "stream_size": 0,
        }
        for key, value in wanted.items():
            if actual[key] != value:
                raise ContractError(
                    f"output {name}.{key}={actual[key]!r}, esperado {value!r}"
                )
        try:
            image = data.image
        except Exception as exc:  # noqa: BLE001 - report UnityPy decoder failures
            raise ContractError(f"nao decodifiquei o output de {name}: {exc}") from exc
        if image.size != (expected.output_width, expected.output_height):
            raise ContractError(f"imagem decodificada de {name} tem tamanho errado")
        seen[name] = actual

    if set(seen) != set(TEXTURES_BY_NAME):
        raise ContractError("output nao contem exatamente as duas texturas esperadas")
    return {
        "bytes": info.st_size,
        "sha256": output_hash,
        "textures": [seen[item.name] | {"name": item.name} for item in TEXTURES],
    }


def transform(
    source_assets: Path,
    source_ress: Path,
    output_directory: Path,
    *,
    write_receipt: bool = True,
) -> dict[str, Any]:
    """Generate the one allowed output from pristine inputs."""

    source_assets = Path(source_assets)
    source_ress = Path(source_ress)
    output_directory = Path(output_directory)
    output_directory.mkdir(parents=True, exist_ok=True)
    if output_directory.is_symlink() or not output_directory.is_dir():
        raise ContractError(f"output-dir invalido: {output_directory}")
    output_assets = output_directory / SOURCE_ASSETS_NAME
    _require_distinct(source_assets, output_assets)
    source_report = inspect_source(source_assets, source_ress)
    stale_ress = output_directory / SOURCE_RESS_NAME
    if stale_ress.exists() or stale_ress.is_symlink():
        raise ContractError(
            f"output-dir contem .resS obsoleto; use um diretorio limpo: {stale_ress}"
        )

    environment = UnityPy.load(str(source_assets))
    changed: list[dict[str, Any]] = []
    resampling = getattr(Image, "Resampling", Image).LANCZOS
    for obj in environment.objects:
        if obj.type.name != "Texture2D":
            continue
        data = obj.read()
        expected = TEXTURES_BY_NAME.get(str(data.m_Name))
        if expected is None:
            raise ContractError(f"Texture2D inesperada durante conversao: {data.m_Name!r}")
        image = data.image.convert("RGBA")
        resized = image.resize(
            (expected.output_width, expected.output_height), resampling
        )
        data.set_image(resized, target_format=expected.texture_format)
        stream = getattr(data, "m_StreamData", None)
        if stream is None:
            raise ContractError(f"{expected.name} nao possui m_StreamData")
        stream.path = ""
        stream.offset = 0
        stream.size = 0
        data.save()
        changed.append(
            {
                "name": expected.name,
                "source": [expected.source_width, expected.source_height],
                "output": [expected.output_width, expected.output_height],
                "format": expected.texture_format,
            }
        )

    if len(changed) != len(TEXTURES):
        raise ContractError("conversao nao tocou exatamente duas texturas")
    payload = environment.file.save()
    source_mode = stat.S_IMODE(source_assets.stat().st_mode)
    output_changed = _atomic_write_if_changed(output_assets, payload, source_mode)
    output_report = inspect_output(output_assets)
    receipt = {
        "schema": SCHEMA,
        "source": source_report["source"],
        "output": {
            SOURCE_ASSETS_NAME: {
                "bytes": output_report["bytes"],
                "sha256": output_report["sha256"],
            },
            SOURCE_RESS_NAME: {"present": False},
        },
        "transforms": changed,
        "output_changed": output_changed,
    }
    if write_receipt:
        receipt_path = output_directory / "sallyface-oversize-receipt.json"
        # output_changed describes the asset operation, not receipt timestamp/state.
        stable_receipt = dict(receipt)
        stable_receipt.pop("output_changed", None)
        _atomic_write_if_changed(receipt_path, canonical_json(stable_receipt))
    return receipt


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    check = subparsers.add_parser("check-source", help="validate pristine inputs")
    check.add_argument("--source-assets", type=Path, required=True)
    check.add_argument("--source-ress", type=Path, required=True)
    convert = subparsers.add_parser("convert", help="write the one allowed output")
    convert.add_argument("--source-assets", type=Path, required=True)
    convert.add_argument("--source-ress", type=Path, required=True)
    convert.add_argument("--output-dir", type=Path, required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "check-source":
            report = inspect_source(args.source_assets, args.source_ress)
        else:
            report = transform(args.source_assets, args.source_ress, args.output_dir)
    except ContractError as exc:
        raise SystemExit(f"sallyface oversize gate: {exc}") from exc
    print(canonical_json(report).decode("utf-8"), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
