#!/usr/bin/env python3
"""Build or validate a reproducible Sally Face 1.5.51 Data directory.

The builder is source-to-output and refuses an existing destination.  It keeps
the owner's pristine source untouched, expands the 613-file asset pack, patches
``data.unity3d`` and every scene-owned Shader for GLES2, applies the closed
two-texture Mali limit fix, and validates the complete result before an atomic
rename.

The validator proves every asset-pack entry against a closed policy: nine files
may change only by the deterministic scene-Shader patch, one may change only by
the two-texture transform, and one superseded ``.resS`` must be absent. It also
checks all 227 serialized files, 1,002 textures, stream references, GLES2
settings and a deterministic SHA-256 manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import stat
import sys
import tempfile
from collections import Counter
from pathlib import Path, PurePosixPath
from typing import Any

import UnityPy

if __package__:
    from . import shader_gles2_patch, unbundle_datapack
    from .sallyface_oversize_fix import (
        SOURCE_ASSETS_NAME,
        SOURCE_RESS_NAME,
        TEXTURES_BY_NAME,
        canonical_json,
        inspect_output,
        sha256_file,
        transform,
    )
else:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import shader_gles2_patch  # type: ignore[no-redef]
    import unbundle_datapack  # type: ignore[no-redef]
    from sallyface_oversize_fix import (  # type: ignore[no-redef]
        SOURCE_ASSETS_NAME,
        SOURCE_RESS_NAME,
        TEXTURES_BY_NAME,
        canonical_json,
        inspect_output,
        sha256_file,
        transform,
    )


SCHEMA = "nextos.sallyface.stage-manifest.v1"
GAME_VERSION = "1.5.51"
DATAPACK_NAME = "datapack.unity3d"
DATA_NAME = "data.unity3d"
DATAPACK_BYTES = 558_995_040
DATAPACK_SHA256 = (
    "086baf35d492267a26225ef842b767b2f2204ea7b0225bbfdb4747977bd7b1ac"
)
DATA_BYTES = 27_506_399
DATA_SHA256 = "707e92d5ef2c47f7e31d25411c685d49f333e201e8f3cabbc28006013385d383"
PATCHED_DATA_BYTES = 23_887_846
PATCHED_DATA_SHA256 = (
    "154199377b342d29a2f0587eea7bc60437494d7a4d8e72912cdf6d626c359664"
)
EXPECTED_BUNDLE_ENTRIES = 613
EXPECTED_SERIALIZED_FILES = 227
EXPECTED_SCENES = 227
EXPECTED_SHADERS = 44
SCENE_SHADER_CONTRACT = {
    "resources.assets": ((525, "Standard"),),
    "sharedassets19.assets": ((5, "Skybox/Procedural"),),
    "sharedassets207.assets": ((8, "Mobile/Particles/Multiply"),),
    "sharedassets209.assets": (
        (41, "Legacy Shaders/Self-Illumin/Diffuse"),
        (42, "Legacy Shaders/Self-Illumin/VertexLit"),
        (43, "Hidden/GlobalFog"),
    ),
    "sharedassets222.assets": (
        (7, "Legacy Shaders/Transparent/Diffuse"),
        (8, "Legacy Shaders/Transparent/VertexLit"),
    ),
    "sharedassets27.assets": (
        (5, "Legacy Shaders/Particles/Alpha Blended Premultiply"),
        (6, "Hidden/FisheyeShader"),
    ),
    # Chuva do cemiterio: sem este shader a Unity cobre a cena com magenta.
    "sharedassets4.assets": ((14, "Legacy Shaders/Particles/Alpha Blended"),),
    "sharedassets95.assets": (
        (9, "Hidden/ChromaticAberration"),
        (10, "Hidden/Vignetting"),
        (11, "Hidden/SeparableBlur"),
    ),
    "sharedassets96.assets": (
        (5, "Mobile/Particles/Additive"),
        (6, "Hidden/Tonemapper"),
        (7, "Hidden/Twist Effect"),
        (8, "Hidden/NoiseAndGrainDX11"),
        (9, "Hidden/NoiseAndGrain"),
    ),
}
EXPECTED_SCENE_SHADERS = sum(len(items) for items in SCENE_SHADER_CONTRACT.values())
EXPECTED_TEXTURES = 1002
EXPECTED_FORMATS = {3: 112, 4: 311, 45: 76, 47: 503}
EXPECTED_GRAPHICS_APIS = [8]
EXPECTED_MIN_GLES = 1


class StageError(RuntimeError):
    """The source or prepared stage violates the Sally Face contract."""


def _require_regular(path: Path, label: str) -> os.stat_result:
    try:
        info = path.lstat()
    except FileNotFoundError as exc:
        raise StageError(f"{label} ausente: {path}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise StageError(f"{label} precisa ser arquivo regular, sem symlink: {path}")
    return info


def _check_exact_file(
    path: Path, label: str, expected_size: int, expected_hash: str
) -> None:
    info = _require_regular(path, label)
    if info.st_size != expected_size:
        raise StageError(
            f"{label} tem {info.st_size} bytes; esperado {expected_size}"
        )
    found = sha256_file(path)
    if found != expected_hash:
        raise StageError(f"{label} SHA-256 {found}; esperado {expected_hash}")


def validate_source(source_data: Path) -> dict[str, Any]:
    source_data = Path(source_data)
    if source_data.is_symlink() or not source_data.is_dir():
        raise StageError(f"source Data invalido: {source_data}")
    datapack = source_data / DATAPACK_NAME
    data = source_data / DATA_NAME
    _check_exact_file(datapack, DATAPACK_NAME, DATAPACK_BYTES, DATAPACK_SHA256)
    _check_exact_file(data, DATA_NAME, DATA_BYTES, DATA_SHA256)
    return {
        DATAPACK_NAME: {"bytes": DATAPACK_BYTES, "sha256": DATAPACK_SHA256},
        DATA_NAME: {"bytes": DATA_BYTES, "sha256": DATA_SHA256},
    }


def _safe_entry_name(name: str) -> Path:
    pure = PurePosixPath(name)
    if (
        not name
        or pure.is_absolute()
        or ".." in pure.parts
        or "\\" in name
        or pure.as_posix() != name
    ):
        raise StageError(f"entrada insegura no datapack: {name!r}")
    return Path(*pure.parts)


def load_bundle(datapack: Path) -> tuple[Any, Any]:
    environment = UnityPy.load(str(datapack))
    if len(environment.files) != 1:
        raise StageError("datapack precisa conter exatamente um UnityFS")
    bundle = next(iter(environment.files.values()))
    entries = getattr(bundle, "files", None)
    if not entries or len(entries) != EXPECTED_BUNDLE_ENTRIES:
        raise StageError(
            f"datapack tem {len(entries) if entries else 0} entradas; "
            f"esperado {EXPECTED_BUNDLE_ENTRIES}"
        )
    for name in entries:
        _safe_entry_name(name)
    return environment, entries


def _texture_metadata(environment: Any) -> dict[tuple[str, int], dict[str, Any]]:
    result: dict[tuple[str, int], dict[str, Any]] = {}
    for obj in environment.objects:
        if obj.type.name != "Texture2D":
            continue
        data = obj.read()
        asset_name = str(obj.assets_file.name)
        stream = getattr(data, "m_StreamData", None)
        key = (asset_name, int(obj.path_id))
        if key in result:
            raise StageError(f"Texture2D duplicada: {key}")
        result[key] = {
            "name": str(data.m_Name),
            "width": int(data.m_Width),
            "height": int(data.m_Height),
            "format": int(data.m_TextureFormat),
            "stream_path": str(getattr(stream, "path", "")),
            "stream_offset": int(getattr(stream, "offset", 0)),
            "stream_size": int(getattr(stream, "size", 0)),
        }
    return result


def _validate_stream(data_dir: Path, texture: dict[str, Any]) -> None:
    stream_path = texture["stream_path"]
    if not stream_path:
        return
    relative = _safe_entry_name(stream_path)
    stream_file = data_dir / relative
    info = _require_regular(stream_file, f"stream de {texture['name']}")
    offset = texture["stream_offset"]
    size = texture["stream_size"]
    if offset < 0 or size < 0 or offset + size > info.st_size:
        raise StageError(
            f"stream fora do arquivo para {texture['name']}: "
            f"offset={offset} size={size} arquivo={info.st_size}"
        )


def _data_settings(path: Path) -> tuple[dict[str, Any], dict[tuple[str, int], dict[str, Any]]]:
    environment = UnityPy.load(str(path))
    counts = Counter(obj.type.name for obj in environment.objects)
    graphics_apis: list[int] | None = None
    minimum_gles: int | None = None
    scenes: list[str] | None = None
    for obj in environment.objects:
        if obj.type.name == "BuildSettings":
            tree = obj.read_typetree()
            graphics_apis = list(tree.get("m_GraphicsAPIs") or [])
            scenes = list(tree.get("scenes") or tree.get("m_Scenes") or [])
        elif obj.type.name == "PlayerSettings":
            minimum_gles = obj.read_typetree().get("playerMinOpenGLESVersion")
    settings = {
        "shaders": counts["Shader"],
        "graphics_apis": graphics_apis,
        "minimum_gles": minimum_gles,
        "scenes": scenes,
    }
    return settings, _texture_metadata(environment)


def _serialized_shader_identity(payload: bytes) -> tuple[tuple[int, str], ...]:
    environment = UnityPy.load(payload)
    result: list[tuple[int, str]] = []
    for obj in environment.objects:
        if obj.type.name != "Shader":
            continue
        tree = obj.read_typetree()
        name = tree.get("m_ParsedForm", {}).get("m_Name") or tree.get("m_Name") or ""
        result.append((int(obj.path_id), str(name)))
    return tuple(result)


def _manifest_files(data_dir: Path) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for path in sorted(data_dir.rglob("*"), key=lambda item: item.as_posix()):
        if path.is_symlink():
            raise StageError(f"stage nao pode conter symlink: {path}")
        if path.is_dir():
            continue
        if not path.is_file():
            raise StageError(f"entrada nao regular no stage: {path}")
        relative = path.relative_to(data_dir).as_posix()
        if relative.endswith((".fullres.bak", ".gles3.bak", ".new")):
            raise StageError(f"backup/temporario proibido no stage: {relative}")
        result.append(
            {"path": relative, "bytes": path.stat().st_size, "sha256": sha256_file(path)}
        )
    return result


def validate_stage(source_data: Path, prepared_data: Path) -> dict[str, Any]:
    """Validate a complete prepared Data directory and return its manifest."""

    source_data = Path(source_data)
    prepared_data = Path(prepared_data)
    source_contract = validate_source(source_data)
    if prepared_data.is_symlink() or not prepared_data.is_dir():
        raise StageError(f"prepared Data invalido: {prepared_data}")
    if (prepared_data / DATAPACK_NAME).exists():
        raise StageError("o stage solto nao pode conservar datapack.unity3d")
    _check_exact_file(
        prepared_data / DATA_NAME,
        "data.unity3d GLES2",
        PATCHED_DATA_BYTES,
        PATCHED_DATA_SHA256,
    )

    original_environment, entries = load_bundle(source_data / DATAPACK_NAME)
    unchanged = 0
    patched_scene_shaders: list[dict[str, Any]] = []
    for name, entry in entries.items():
        expected = bytes(getattr(entry, "reader", entry).bytes)
        target = prepared_data / _safe_entry_name(name)
        if name == SOURCE_RESS_NAME:
            if target.exists() or target.is_symlink():
                raise StageError(f"{SOURCE_RESS_NAME} deveria ter sido eliminado")
            continue
        if name == SOURCE_ASSETS_NAME:
            _require_regular(target, SOURCE_ASSETS_NAME)
            continue
        if name in SCENE_SHADER_CONTRACT:
            _require_regular(target, f"entrada de shader {name}")
            identity = _serialized_shader_identity(expected)
            if identity != SCENE_SHADER_CONTRACT[name]:
                raise StageError(
                    f"contrato de shaders mudou em {name}: {identity!r}"
                )
            wanted, counters = shader_gles2_patch.patch_serialized_bytes(
                expected, name
            )
            if target.read_bytes() != wanted:
                raise StageError(f"patch GLES2 de shaders divergiu em {name}")
            patched_scene_shaders.append(
                {
                    "asset": name,
                    "shaders": counters["shaders"],
                    "platforms": counters["platforms"],
                    "gpu_types": counters["gpu_type"],
                    "blob_program_types": counters["blob"],
                }
            )
            continue
        info = _require_regular(target, f"entrada {name}")
        if info.st_size != len(expected):
            raise StageError(f"entrada {name} mudou de tamanho")
        if hashlib.sha256(target.read_bytes()).digest() != hashlib.sha256(expected).digest():
            raise StageError(f"entrada {name} mudou fora da allowlist")
        unchanged += 1
    expected_unchanged = EXPECTED_BUNDLE_ENTRIES - 2 - len(SCENE_SHADER_CONTRACT)
    if unchanged != expected_unchanged:
        raise StageError(
            f"entradas byte-identicas={unchanged}, esperado {expected_unchanged}"
        )
    if sum(item["shaders"] for item in patched_scene_shaders) != EXPECTED_SCENE_SHADERS:
        raise StageError(
            f"shaders de cena convertidos="
            f"{sum(item['shaders'] for item in patched_scene_shaders)}, "
            f"esperado {EXPECTED_SCENE_SHADERS}"
        )
    oversize_report = inspect_output(prepared_data / SOURCE_ASSETS_NAME)

    original_textures = _texture_metadata(original_environment)
    prepared_textures: dict[tuple[str, int], dict[str, Any]] = {}
    serialized_files = sorted(prepared_data.glob("*.assets"))
    if len(serialized_files) != EXPECTED_SERIALIZED_FILES:
        raise StageError(
            f"arquivos .assets={len(serialized_files)}, esperado {EXPECTED_SERIALIZED_FILES}"
        )
    for path in serialized_files:
        environment = UnityPy.load(str(path))
        for key, value in _texture_metadata(environment).items():
            if key in prepared_textures:
                raise StageError(f"Texture2D duplicada entre arquivos: {key}")
            prepared_textures[key] = value
            _validate_stream(prepared_data, value)
    if set(prepared_textures) != set(original_textures):
        missing = sorted(set(original_textures) - set(prepared_textures))[:10]
        extra = sorted(set(prepared_textures) - set(original_textures))[:10]
        raise StageError(f"inventario Texture2D divergiu; missing={missing} extra={extra}")

    allowed_changes: list[dict[str, Any]] = []
    for key, original in original_textures.items():
        prepared = prepared_textures[key]
        if original["name"] in TEXTURES_BY_NAME and key[0] == SOURCE_ASSETS_NAME:
            contract = TEXTURES_BY_NAME[original["name"]]
            wanted = dict(original)
            wanted.update(
                width=contract.output_width,
                height=contract.output_height,
                stream_path="",
                stream_offset=0,
                stream_size=0,
            )
            if prepared != wanted:
                raise StageError(
                    f"transformacao inesperada em {key}: {prepared}, esperado {wanted}"
                )
            allowed_changes.append(
                {
                    "asset": key[0],
                    "path_id": key[1],
                    "name": original["name"],
                    "source": [original["width"], original["height"]],
                    "output": [prepared["width"], prepared["height"]],
                }
            )
        elif prepared != original:
            raise StageError(f"Texture2D mudou fora da allowlist: {key}")
    if len(allowed_changes) != 2:
        raise StageError("o stage precisa conter exatamente duas mudancas de textura")

    prepared_settings, prepared_data_textures = _data_settings(prepared_data / DATA_NAME)
    original_settings, original_data_textures = _data_settings(source_data / DATA_NAME)
    if prepared_settings["graphics_apis"] != EXPECTED_GRAPHICS_APIS:
        raise StageError(f"m_GraphicsAPIs={prepared_settings['graphics_apis']}, esperado [8]")
    if prepared_settings["minimum_gles"] != EXPECTED_MIN_GLES:
        raise StageError(
            f"playerMinOpenGLESVersion={prepared_settings['minimum_gles']}, esperado 1"
        )
    if prepared_settings["shaders"] != EXPECTED_SHADERS:
        raise StageError(
            f"shaders={prepared_settings['shaders']}, esperado {EXPECTED_SHADERS}"
        )
    if len(prepared_settings["scenes"] or []) != EXPECTED_SCENES:
        raise StageError(
            f"cenas={len(prepared_settings['scenes'] or [])}, esperado {EXPECTED_SCENES}"
        )
    if prepared_settings["scenes"] != original_settings["scenes"]:
        raise StageError("a lista/ordem nativa de cenas mudou")
    if prepared_data_textures != original_data_textures:
        raise StageError("texturas de data.unity3d mudaram durante o patch de shader")

    all_textures = list(prepared_textures.values()) + list(prepared_data_textures.values())
    formats = Counter(item["format"] for item in all_textures)
    if len(all_textures) != EXPECTED_TEXTURES:
        raise StageError(f"texturas={len(all_textures)}, esperado {EXPECTED_TEXTURES}")
    if dict(sorted(formats.items())) != EXPECTED_FORMATS:
        raise StageError(f"formatos={dict(sorted(formats.items()))}, esperado {EXPECTED_FORMATS}")
    oversized = [
        item for item in all_textures if max(item["width"], item["height"]) > 4096
    ]
    if oversized:
        raise StageError(f"texturas ainda excedem 4096: {oversized[:5]}")

    files = _manifest_files(prepared_data)
    return {
        "schema": SCHEMA,
        "port": "sallyface",
        "game_version": GAME_VERSION,
        "source_contract": source_contract,
        "policy": {
            "runtime_half_resolution": False,
            "byte_identical_datapack_entries": unchanged,
            "changed_datapack_entries": sorted(
                [SOURCE_ASSETS_NAME, *SCENE_SHADER_CONTRACT]
            ),
            "removed_datapack_entries": [SOURCE_RESS_NAME],
        },
        "counts": {
            "datapack_entries": len(entries),
            "serialized_files": len(serialized_files),
            "scenes": len(prepared_settings["scenes"] or []),
            "data_shaders": prepared_settings["shaders"],
            "scene_shaders": EXPECTED_SCENE_SHADERS,
            "textures": len(all_textures),
            "texture_formats": {str(key): value for key, value in sorted(formats.items())},
        },
        "allowed_texture_changes": sorted(
            allowed_changes, key=lambda item: (item["asset"], item["path_id"])
        ),
        "scene_shader_patches": sorted(
            patched_scene_shaders, key=lambda item: item["asset"]
        ),
        "oversize_output": oversize_report,
        "files": files,
    }


def _reject_symlinks(root: Path) -> None:
    for path in root.rglob("*"):
        if path.is_symlink():
            raise StageError(f"source Data nao pode conter symlink: {path}")


def _atomic_manifest(path: Path, manifest: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_symlink():
        raise StageError(f"manifesto nao pode ser symlink: {path}")
    payload = canonical_json(manifest)
    if path.exists() and path.read_bytes() == payload:
        return
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def prepare_stage(
    source_data: Path, output_data: Path, manifest_path: Path | None = None
) -> dict[str, Any]:
    """Build into a fresh directory and publish it only after validation."""

    source_data = Path(source_data)
    output_data = Path(output_data)
    validate_source(source_data)
    if output_data.exists() or output_data.is_symlink():
        raise StageError(f"output ja existe; escolha um destino novo: {output_data}")
    output_data.parent.mkdir(parents=True, exist_ok=True)
    source_real = source_data.resolve()
    output_real = output_data.parent.resolve() / output_data.name
    if os.path.commonpath((str(source_real), str(output_real))) == str(source_real):
        raise StageError("output nao pode ficar dentro do source Data")
    _reject_symlinks(source_data)

    bundle_environment, bundle_file_entries = load_bundle(source_data / DATAPACK_NAME)
    entry_names = list(bundle_file_entries)
    del bundle_file_entries
    del bundle_environment
    with tempfile.TemporaryDirectory(
        prefix=f".{output_data.name}.build.", dir=output_data.parent
    ) as temporary_root:
        temporary_root_path = Path(temporary_root)
        build_data = temporary_root_path / "Data"

        def ignore_source(_directory: str, names: list[str]) -> set[str]:
            return {name for name in names if name in {DATAPACK_NAME, DATA_NAME}}

        shutil.copytree(source_data, build_data, ignore=ignore_source)
        for name in entry_names:
            collision = build_data / _safe_entry_name(name)
            if collision.exists() or collision.is_symlink():
                raise StageError(f"datapack colide com arquivo base: {name}")
        unbundle_datapack.unbundle(str(source_data / DATAPACK_NAME), str(build_data))
        shader_gles2_patch.patch_source_to_output(
            source_data / DATA_NAME, build_data / DATA_NAME
        )
        for name in SCENE_SHADER_CONTRACT:
            source_asset = build_data / name
            patched_asset = temporary_root_path / f"{name}.gles2"
            shader_gles2_patch.patch_serialized_source_to_output(
                source_asset, patched_asset
            )
            os.replace(patched_asset, source_asset)

        fixed_directory = temporary_root_path / "oversize-fixed"
        transform(
            build_data / SOURCE_ASSETS_NAME,
            build_data / SOURCE_RESS_NAME,
            fixed_directory,
        )
        os.replace(
            fixed_directory / SOURCE_ASSETS_NAME,
            build_data / SOURCE_ASSETS_NAME,
        )
        (build_data / SOURCE_RESS_NAME).unlink()

        manifest = validate_stage(source_data, build_data)
        os.replace(build_data, output_data)

    if manifest_path is None:
        manifest_path = output_data.with_name(f"{output_data.name}.manifest.json")
    _atomic_manifest(Path(manifest_path), manifest)
    return manifest


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    prepare = subparsers.add_parser("prepare", help="build a fresh Data directory")
    prepare.add_argument("--source-data", type=Path, required=True)
    prepare.add_argument("--output-data", type=Path, required=True)
    prepare.add_argument("--manifest", type=Path)
    validate = subparsers.add_parser("validate", help="validate an existing Data directory")
    validate.add_argument("--source-data", type=Path, required=True)
    validate.add_argument("--prepared-data", type=Path, required=True)
    validate.add_argument("--manifest", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "prepare":
            manifest = prepare_stage(args.source_data, args.output_data, args.manifest)
        else:
            manifest = validate_stage(args.source_data, args.prepared_data)
            if args.manifest:
                _atomic_manifest(args.manifest, manifest)
    except (StageError, ValueError) as exc:
        raise SystemExit(f"sallyface stage gate: {exc}") from exc
    print(canonical_json(manifest).decode("utf-8"), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
