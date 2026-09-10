#!/usr/bin/env python3
"""Marca os shaders do Sally Face como GLES2 para o renderer Utgard aceita-los.

O jogo foi compilado so' com variantes GLES3 (ShaderCompilerPlatform 9) e Vulkan
(18). Rodando num Mali-450, a Unity pede a plataforma 5 (GLES20), nao acha nada
no blob e registra "Desired shader compiler platform 5 is not available in
shader blob" -- e o material cai no error shader.

O GLSL dentro do blob e' TEXTO. Nenhum dos 101 shaders declara uniform block de
verdade (0 UBOs reais; os 98 "std140" sao macro boilerplate nunca usada), entao
o codigo roda em ESSL 100 depois de traduzido. A traducao acontece em tempo de
execucao no glShaderSource (src/egl.c); este script so' faz a Unity ENTREGAR o
shader e escolher seu renderer GLES2 nativo, corrigindo:

  1. BuildSettings.m_GraphicsAPIs [11] (GLES3) -> [8] (GLES2)
  2. PlayerSettings.playerMinOpenGLESVersion 2 (GLES3.0) -> 1 (GLES2.0)
  3. Shader.platforms            9 (GLES3x) -> 5 (GLES20)
  4. m_ParsedForm .. m_Platforms 9          -> 5
  5. m_GpuProgramType            4 (GLES3)  -> 5 (GLES)
  6. dentro do blob comprimido: o int logo depois do stamp de versao, 4 -> 5

🚨 SO' RODAR EM CONTAINER UnityFS (`.unity3d`).  Medido em 25/08/2026: aplicado
no `unity default resources` -- que e' um SerializedFile CRU, nao um container --
o arquivo sai corrompido e a Unity morre no boot tentando alocar 100 GB
(`Could not allocate memory: System out of memory! Trying to allocate:
107803723968B`).  Os 6 shaders internos que moram la (`Hidden/InternalClear`,
`Hidden/Internal-Colored`, `GUI/Text Shader`...) NAO precisam do patch: com o
`data.unity3d` corrigido sobra UMA unica mensagem de `platform 5` na corrida
inteira, e ela e' inofensiva.

Uso recomendado (source -> output, sem tocar na origem):
  shader_gles2_patch.py --output data-gles2.unity3d data-gles3.unity3d

O modo legado com um ou mais posicionais continua editando no lugar e criando
``.gles3.bak``. O orquestrador do Sally Face usa sempre o modo source -> output.

Os shaders que vieram no ``datapack.unity3d`` acabam como SerializedFiles crus
(``sharedassets*.assets``). Eles NAO podem passar por ``patch_file`` porque esse
caminho salva um UnityFS com ``packer=\"lz4\"``. Use o caminho dedicado
``patch_serialized_*``: ele salva o SerializedFile cru, reabre o resultado e
prova que todos os objetos que nao sao Shader continuam byte a byte identicos.
"""
import argparse
import os
import shutil
import struct
import sys
import tempfile
from pathlib import Path

import UnityPy
import lz4.block

PLAT_GLES3X, PLAT_GLES20 = 9, 5
GRAPHICS_API_GLES3, GRAPHICS_API_GLES2 = 11, 8
OPEN_GLES_VERSION_30, OPEN_GLES_VERSION_20 = 2, 1
TYPE_GLES3, TYPE_GLES = 4, 5
VERSION_STAMPS = (202012090,)


def new_counters():
    return {
        "shaders": 0,
        "shaders_changed": 0,
        "platforms": 0,
        "gpu_type": 0,
        "blob": 0,
        "segs": 0,
        "graphics_apis": 0,
        "minimum_gles": 0,
    }


def flat_shape(x):
    """Devolve (lista_plana, forma) para remontar listas aninhadas."""
    if isinstance(x, (list, tuple)):
        parts, shape = [], []
        for i in x:
            p, s = flat_shape(i)
            parts += p
            shape.append(s)
        return parts, shape
    return [x], None


def rebuild(flat, shape, it=None):
    if it is None:
        it = iter(flat)
    if shape is None:
        return next(it)
    return [rebuild(flat, s, it) for s in shape]


def patch_segment(seg: bytes) -> tuple[bytes, int]:
    """Troca o programType 4->5 logo apos cada stamp de versao. Tamanho intacto."""
    buf = bytearray(seg)
    hits = 0
    for stamp in VERSION_STAMPS:
        needle = struct.pack("<i", stamp)
        pos = 0
        while True:
            pos = buf.find(needle, pos)
            if pos < 0:
                break
            tpos = pos + 4
            if tpos + 4 <= len(buf):
                (t,) = struct.unpack_from("<i", buf, tpos)
                if t == TYPE_GLES3:
                    struct.pack_into("<i", buf, tpos, TYPE_GLES)
                    hits += 1
            pos += 4
    return bytes(buf), hits


def patch_tree(node, counters):
    """Desce o typetree trocando m_GpuProgramType e m_Platforms."""
    if isinstance(node, dict):
        for k, v in node.items():
            if k == "m_GpuProgramType" and v == TYPE_GLES3:
                node[k] = TYPE_GLES
                counters["gpu_type"] += 1
            elif k in ("m_Platforms", "platforms") and isinstance(v, list):
                for i, p in enumerate(v):
                    if p == PLAT_GLES3X:
                        v[i] = PLAT_GLES20
                        counters["platforms"] += 1
            else:
                patch_tree(v, counters)
    elif isinstance(node, list):
        for v in node:
            patch_tree(v, counters)


def patch_shader_tree(tree, counters):
    """Converte uma arvore Shader e devolve se algum campo realmente mudou."""
    counters["shaders"] += 1
    before = (counters["platforms"], counters["gpu_type"], counters["blob"])
    patch_tree(tree, counters)

    blob = bytes(tree.get("compressedBlob") or b"")
    if blob:
        offs, shape_o = flat_shape(tree["offsets"])
        clen, _ = flat_shape(tree["compressedLengths"])
        dlen, _ = flat_shape(tree["decompressedLengths"])
        out = bytearray()
        new_offs, new_clen = [], []
        for off, cl, dl in zip(offs, clen, dlen):
            seg = lz4.block.decompress(blob[off:off + cl], uncompressed_size=dl)
            seg, hits = patch_segment(seg)
            counters["blob"] += hits
            counters["segs"] += 1
            packed = lz4.block.compress(
                seg, mode="high_compression", store_size=False
            )
            new_offs.append(len(out))
            new_clen.append(len(packed))
            out += packed
        tree["compressedBlob"] = bytes(out)
        tree["offsets"] = rebuild(new_offs, shape_o)
        tree["compressedLengths"] = rebuild(new_clen, shape_o)

    after = (counters["platforms"], counters["gpu_type"], counters["blob"])
    changed = after != before
    if changed:
        counters["shaders_changed"] += 1
    return changed


def patch_object(obj, counters, label):
    """Aplica a conversao ao objeto Unity correspondente, quando necessario."""
    if obj.type.name == "PlayerSettings":
        tree = obj.read_typetree()
        minimum = tree.get("playerMinOpenGLESVersion")
        if minimum == OPEN_GLES_VERSION_20:
            return False
        if minimum != OPEN_GLES_VERSION_30:
            raise ValueError(
                f"{label}: playerMinOpenGLESVersion inesperado: {minimum!r}"
            )
        tree["playerMinOpenGLESVersion"] = OPEN_GLES_VERSION_20
        obj.save_typetree(tree)
        counters["minimum_gles"] += 1
        return True

    if obj.type.name == "BuildSettings":
        tree = obj.read_typetree()
        graphics_apis = list(tree.get("m_GraphicsAPIs") or [])
        if graphics_apis == [GRAPHICS_API_GLES2]:
            return False
        # O Sally Face declara [Vulkan, GLES3]. O Utgard nao tem Vulkan e o
        # blob nao tem ES2, entao a lista vira SO' GLES2 -- deixar o Vulkan
        # na frente faria a Unity tentar libvulkan.so antes de qualquer GL.
        if GRAPHICS_API_GLES3 not in graphics_apis:
            raise ValueError(
                f"{label}: m_GraphicsAPIs inesperado: {graphics_apis!r}"
            )
        tree["m_GraphicsAPIs"] = [GRAPHICS_API_GLES2]
        obj.save_typetree(tree)
        counters["graphics_apis"] += 1
        return True

    if obj.type.name != "Shader":
        return False
    tree = obj.read_typetree()
    if not patch_shader_tree(tree, counters):
        return False
    obj.save_typetree(tree)
    return True


def patch_file(path: Path, *, make_backup: bool = True) -> dict:
    with open(path, "rb") as fh:
        if fh.read(7) != b"UnityFS":
            raise SystemExit(
                f"{path}: nao e' um container UnityFS -- ver o aviso no topo "
                f"deste arquivo; rodar aqui CORROMPE o arquivo"
            )
    counters = new_counters()
    env = UnityPy.load(str(path))
    touched = False

    for obj in env.objects:
        if patch_object(obj, counters, path):
            touched = True

    if touched:
        if make_backup:
            backup = path.with_suffix(path.suffix + ".gles3.bak")
            if not backup.exists():
                shutil.copy2(path, backup)
        data = env.file.save(packer="lz4")
        temporary = path.with_name(path.name + ".new")
        temporary.write_bytes(data)
        shutil.copymode(path, temporary)
        temporary.replace(path)
    return counters


def _object_payloads(environment):
    return {
        (int(obj.path_id), str(obj.type.name)): bytes(obj.get_raw_data())
        for obj in environment.objects
    }


def patch_serialized_bytes(payload: bytes, label: str = "SerializedFile"):
    """Converte settings/shaders de um SerializedFile cru com prova por objeto."""
    payload = bytes(payload)
    if payload.startswith(b"UnityFS"):
        raise ValueError(f"{label}: use patch_source_to_output para UnityFS")

    environment = UnityPy.load(payload)
    before = _object_payloads(environment)
    counters = new_counters()
    touched = False
    for obj in environment.objects:
        if patch_object(obj, counters, label):
            touched = True
    if not touched:
        raise ValueError(f"{label}: nenhum settings/shader GLES3 para converter")

    result = bytes(environment.file.save())
    checked = UnityPy.load(result)
    after = _object_payloads(checked)
    if set(before) != set(after):
        raise ValueError(f"{label}: inventario de objetos mudou ao salvar")
    allowed = {"Shader", "PlayerSettings", "BuildSettings"}
    changed = {name: 0 for name in allowed}
    for key, original in before.items():
        if after[key] == original:
            continue
        if key[1] not in allowed:
            raise ValueError(
                f"{label}: objeto fora do contrato mudou: "
                f"path_id={key[0]} type={key[1]}"
            )
        changed[key[1]] += 1
    expected = {
        "Shader": counters["shaders_changed"],
        "PlayerSettings": counters["minimum_gles"],
        "BuildSettings": counters["graphics_apis"],
    }
    if changed != expected:
        raise ValueError(
            f"{label}: objetos alterados={changed}, esperado={expected}"
        )
    return result, counters


def patch_serialized_source_to_output(source: Path, output: Path) -> dict:
    """Patch atomico source -> output para ``*.assets`` crus."""
    source = Path(source)
    output = Path(output)
    if not source.is_file() or source.is_symlink():
        raise ValueError(f"source precisa ser arquivo regular: {source}")
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.is_symlink():
        raise ValueError(f"output nao pode ser symlink: {output}")
    if source.resolve() == (output.parent.resolve() / output.name):
        raise ValueError("source e output precisam ser caminhos distintos")

    result, counters = patch_serialized_bytes(source.read_bytes(), source.name)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.", suffix=".tmp", dir=output.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(result)
            handle.flush()
            os.fsync(handle.fileno())
        shutil.copymode(source, temporary)
        os.replace(temporary, output)
        return counters
    finally:
        if temporary.exists():
            temporary.unlink()


def patch_source_to_output(source: Path, output: Path) -> dict:
    """Patch a pristine source into a distinct output path atomically."""

    source = Path(source)
    output = Path(output)
    if not source.is_file() or source.is_symlink():
        raise ValueError(f"source precisa ser arquivo regular: {source}")
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.is_symlink():
        raise ValueError(f"output nao pode ser symlink: {output}")
    if source.resolve() == (output.parent.resolve() / output.name):
        raise ValueError("source e output precisam ser caminhos distintos")

    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.", suffix=".tmp", dir=output.parent
    )
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        shutil.copy2(source, temporary)
        counters = patch_file(temporary, make_backup=False)
        payload = temporary.read_bytes()
        if output.exists() and output.read_bytes() == payload:
            temporary.unlink()
        else:
            os.replace(temporary, output)
        return counters
    finally:
        if temporary.exists():
            temporary.unlink()


def _format_result(path: Path, counters: dict) -> str:
    return (
        f"{path.name}: {counters['shaders']} shaders, "
        f"{counters['segs']} segmentos, platforms {counters['platforms']}, "
        f"m_GpuProgramType {counters['gpu_type']}, "
        f"programType no blob {counters['blob']}, "
        f"BuildSettings GLES2 {counters['graphics_apis']}, "
        f"minimo GLES2 {counters['minimum_gles']}"
    )


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument(
        "--output",
        type=Path,
        help="grava um unico source em outro caminho, sem alterar a origem",
    )
    args = parser.parse_args(argv)
    if args.output:
        if len(args.files) != 1:
            parser.error("--output exige exatamente um source")
        counters = patch_source_to_output(args.files[0], args.output)
        print(_format_result(args.output, counters))
        return 0
    for path in args.files:
        counters = patch_file(path)
        print(_format_result(path, counters))
    return 0


if __name__ == "__main__":
    sys.exit(main())
