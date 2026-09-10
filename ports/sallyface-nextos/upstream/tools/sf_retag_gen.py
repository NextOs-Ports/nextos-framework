#!/usr/bin/env python3
"""Gera o nxextract/sf_retag_spec.json a partir do container pinado.

Roda NO HOST (precisa de UnityPy + lz4, como o preparo full-data). O spec
descreve, por deltas minimos, a transformacao que o tools/shader_gles2_patch.py
faz -- e e' isso que o sf_retag.py reproduz no aparelho sem dependencias.

O spec NAO carrega dado do jogo: somente offsets, SHA-256 e os poucos bytes
trocados pelo retag (ints de plataforma e trechos curtos do blob
recomprimido).

Uso:
  sf_retag_gen.py <assets/bin/Data do APK extraido> <perfil> <saida spec.json>

Cada execucao gera um spec v2 valido com um perfil. Para builds compativeis
adicionais, gere cada perfil de forma independente e una somente os objetos de
``profiles`` depois de validar as respectivas saidas.
"""
import hashlib
import json
import os
import shutil
import struct
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "nxextract"))

import shader_gles2_patch as sp          # noqa: E402
import sallyface_stage as st             # noqa: E402
import sf_retag                          # noqa: E402


def sha(data):
    return hashlib.sha256(data).hexdigest()


def diff_ops(a, b, resync=48):
    """Delta minimo [(off_in, remove_len, insert_hex)] de a -> b."""
    ops = []
    i = j = 0
    while i < len(a) and j < len(b):
        if a[i] == b[j]:
            i += 1
            j += 1
            continue
        # regiao divergente: procura o proximo ponto de ressincronizacao;
        # a janela cresce porque uma tabela de objetos inteira pode mudar
        # (offsets deslocados) antes do conteudo voltar a alinhar
        found = None
        for window in (1 << 16, 1 << 20, 1 << 23, 1 << 26):
            for di in range(0, window):
                if i + di + resync > len(a):
                    break
                probe = a[i + di:i + di + resync]
                # probe de baixa entropia (zeros, padding) casa em qualquer
                # lugar e dessincroniza o delta -- exigir conteudo real
                if len(set(probe)) < 8:
                    continue
                k = b.find(probe, j, j + window + resync)
                if k >= 0:
                    found = (di, k - j)
                    break
            if found is not None:
                break
        if found is None:
            ops.append((i, len(a) - i, _hex_chunks(b[j:])))
            return ops
        di, dj = found
        ops.append((i, di, _hex_chunks(b[j:j + dj])))
        i += di
        j += dj
    if i < len(a) or j < len(b):
        ops.append((i, len(a) - i, _hex_chunks(b[j:])))
    return ops


def _hex_chunks(data, width=96):
    """Hex quebrado em blocos curtos: linha longa engasga auditor de release."""
    h = data.hex()
    if len(h) <= width:
        return h
    return [h[i:i + width] for i in range(0, len(h), width)]


def check_ops(a, b, ops):
    if sf_retag.apply_ops(a, ops) != b:
        raise SystemExit("delta nao reconstruiu a saida")
    def _hexlen(v):
        return len("".join(v)) // 2 if isinstance(v, list) else len(v) // 2
    return sum(_hexlen(x[2]) for x in ops)


def _collect_key(node, key, output):
    if isinstance(node, dict):
        for current, value in node.items():
            if current == key:
                output.append(value)
            _collect_key(value, key, output)
    elif isinstance(node, list):
        for value in node:
            _collect_key(value, key, output)


def _flat_values(value):
    if isinstance(value, (list, tuple)):
        for item in value:
            yield from _flat_values(item)
    else:
        yield value


def validate_rebuilt_bundle(path):
    """Prova a semantica que evita o fallback magenta, nao apenas o parse."""
    import UnityPy
    import lz4.block

    environment = UnityPy.load(str(path))
    minimum_gles = []
    graphics_apis = []
    shaders = 0
    platforms_gles3 = 0
    gpu_type_gles3 = 0
    blob_type_gles3 = 0

    for obj in environment.objects:
        if obj.type.name == "PlayerSettings":
            tree = obj.read_typetree()
            minimum_gles.append(tree.get("playerMinOpenGLESVersion"))
            continue
        if obj.type.name == "BuildSettings":
            tree = obj.read_typetree()
            graphics_apis.append(list(tree.get("m_GraphicsAPIs") or []))
            continue
        if obj.type.name != "Shader":
            continue

        shaders += 1
        tree = obj.read_typetree()
        for key in ("platforms", "m_Platforms"):
            found = []
            _collect_key(tree, key, found)
            platforms_gles3 += sum(
                value == sp.PLAT_GLES3X
                for value in _flat_values(found)
                if isinstance(value, int)
            )
        found = []
        _collect_key(tree, "m_GpuProgramType", found)
        gpu_type_gles3 += sum(
            value == sp.TYPE_GLES3
            for value in _flat_values(found)
            if isinstance(value, int)
        )

        blob = bytes(tree.get("compressedBlob") or b"")
        if not blob:
            continue
        offsets, _ = sp.flat_shape(tree["offsets"])
        compressed, _ = sp.flat_shape(tree["compressedLengths"])
        decompressed, _ = sp.flat_shape(tree["decompressedLengths"])
        for offset, packed_size, raw_size in zip(
                offsets, compressed, decompressed):
            segment = lz4.block.decompress(
                blob[offset:offset + packed_size], uncompressed_size=raw_size)
            _unchanged, hits = sp.patch_segment(segment)
            blob_type_gles3 += hits

    if minimum_gles != [sp.OPEN_GLES_VERSION_20]:
        raise SystemExit(
            "data.unity3d: PlayerSettings nao ficou GLES2: %r" % minimum_gles)
    if graphics_apis != [[sp.GRAPHICS_API_GLES2]]:
        raise SystemExit(
            "data.unity3d: BuildSettings nao ficou GLES2: %r" % graphics_apis)
    if shaders == 0:
        raise SystemExit("reconstrucao ilegivel: zero Shader no bundle")
    if platforms_gles3 or gpu_type_gles3 or blob_type_gles3:
        raise SystemExit(
            "data.unity3d: retag incompleto: platform9=%d gpu4=%d blob4=%d" %
            (platforms_gles3, gpu_type_gles3, blob_type_gles3))
    return shaders


def main(argv):
    if len(argv) != 4:
        sys.stderr.write(__doc__)
        return 2
    data_dir = Path(argv[1])
    profile_id = argv[2]
    out_path = Path(argv[3])

    if not profile_id or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789-"
                             for c in profile_id):
        raise SystemExit("perfil invalido: %r" % profile_id)

    profile = {
        "id": profile_id,
        "loose_assets": [],
        "data_bundle": None,
    }
    spec = {
        "schema": "org.nextos.sallyface.retag-spec",
        "schema_version": 2,
        "fallback": "generic-pass-through",
        "profiles": [profile],
    }

    # 1. entradas do datapack (contrato de shaders de cena)
    _env, entries = st.load_bundle(data_dir / "datapack.unity3d")
    total_bytes = 0
    for name in sorted(st.SCENE_SHADER_CONTRACT):
        e = entries[name]
        raw = bytes(getattr(e, "reader", e).bytes)
        out, counters = sp.patch_serialized_bytes(raw, name)
        ops = diff_ops(raw, out)
        total_bytes += check_ops(raw, out, ops)
        profile["loose_assets"].append({
            "name": name,
            "sha_out": sha(out),
            "ops": ops,
            "shaders": counters["shaders"],
        })
        print("loose %-24s %d ops" % (name, len(ops)))

    # 2. nos internos do data.unity3d
    bundle_path = data_dir / "data.unity3d"
    header, nodes, blob = sf_retag._read_bundle(str(bundle_path))
    node_specs = []
    for off, nsize, nf, name in nodes:
        raw = bytes(blob[off:off + nsize])
        if name == "unity default resources":
            continue      # SerializedFile cru especial: nunca tocar (25/08)
        try:
            out, counters = sp.patch_serialized_bytes(raw, name)
        except Exception:
            continue
        if out == raw:
            continue
        ops = diff_ops(raw, out)
        total_bytes += check_ops(raw, out, ops)
        node_specs.append({
            "name": name,
            "sha_out": sha(out),
            "ops": ops,
        })
        print("node  %-24s %d ops  (%s)" % (name, len(ops), {
            k: v for k, v in counters.items() if v}))
    profile["data_bundle"] = {
        "nodes": node_specs,
        "bundle_sha_out": "",
    }

    # 3. reconstroi no host com o MESMO codigo do device e pina a saida
    with tempfile.TemporaryDirectory() as td:
        work = Path(td) / "data.unity3d"
        shutil.copyfile(bundle_path, work)
        # primeiro reconstroi sem pino, depois pina a saida
        sf_retag.rebuild_data_bundle(str(work), {
            **profile["data_bundle"], "bundle_sha_out": None})
        h = hashlib.sha256()
        with open(work, "rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b""):
                h.update(chunk)
        profile["data_bundle"]["bundle_sha_out"] = h.hexdigest()

        # Valida a configuracao global e todos os campos Shader que fizeram o
        # 1.1.2 aceitar o InternalErrorShader magenta como quadro valido.
        shaders = validate_rebuilt_bundle(work)
        print("rebuilt data.unity3d: %d bytes, %d Shader legiveis"
              % (work.stat().st_size, shaders))

    out_path.write_text(json.dumps(spec, indent=1))
    print("spec: %s (%d bytes de conteudo em deltas)"
          % (out_path, total_bytes))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
