#!/usr/bin/env python3
"""Injeta variantes GLES2 (plataforma 5) nos Shader de assets/bin/Data.

Motivo: o pacote e' VULKAN-ONLY e o Mali-450 nao tem Vulkan.  Com
`m_GraphicsAPIs` trocado para GLES2 a Unity roda o backend GL, mas os dados nao
trazem subprograma de plataforma 5 -- ela mesma reclama
"Desired shader compiler platform 5 is not available in shader blob".

O que entra e' o formato MEDIDO nas variantes GLES20 que a propria Unity enviou
neste pacote (`unity default resources`):

  entrada do blob = u32 versao 201802150 | u32 tipo 5 | 4x u32 de stats |
                    u32 n de keywords + keywords | u32 tamanho + fonte +
                    padding de 4 | cauda de 32 bytes cujo 1o u32 e' a mascara
                    de canais de vertice
  a fonte inteira (VERTEX+FRAGMENT num texto so') fica na entrada de VERTICE;
  a de FRAGMENTO fica vazia, com mascara 0.

A escrita e' CIRURGICA (serialized_patch.py): `UnityPy.save()` nao e' usado em
lugar nenhum -- um save de no-op ja muda 68 mil bytes neste pacote.
"""
import argparse
import os
import re
import struct
import sys

import lz4.block
import UnityPy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gles2_gen import generate                      # noqa: E402
from serialized_patch import SerializedFileLayout   # noqa: E402

SUBPROGRAM_VERSION = 201802150
GPU_GLES2 = 5
GPU_SPIRV = 25
PLATFORM_GLES20 = 5
STAGES = ("progVertex", "progFragment")


def align4(buf):
    while len(buf) % 4:
        buf.append(0)


def build_entry(program_type, source, mask, keywords=()):
    e = bytearray()
    e += struct.pack("<II", SUBPROGRAM_VERSION, program_type)
    e += struct.pack("<IIII", 0, 0, 0, 0)
    e += struct.pack("<I", len(keywords))
    for kw in keywords:
        raw = kw.encode()
        e += struct.pack("<I", len(raw)) + raw
        align4(e)
    raw = source.encode()
    e += struct.pack("<I", len(raw)) + raw
    align4(e)
    e += struct.pack("<8I", mask, 0, 1, 0, 0, 0, 0, 0)
    return bytes(e)


def build_blob(entries):
    header = bytearray(struct.pack("<I", len(entries)))
    header += b"\0" * (8 * len(entries))
    body = bytearray()
    for index, payload in enumerate(entries):
        struct.pack_into("<II", header, 4 + 8 * index,
                         len(header) + len(body), len(payload))
        body += payload
    return bytes(header) + bytes(body)


def subprogram(blob_index, gpu_type, tier, keyword_indices, source_map,
               requirements):
    return {
        "m_BlobIndex": blob_index,
        "m_Channels": {"m_Channels": [], "m_SourceMap": source_map},
        "m_KeywordIndices": list(keyword_indices),
        "m_ShaderHardwareTier": tier,
        "m_GpuProgramType": gpu_type,
        "m_VectorParams": [], "m_MatrixParams": [], "m_TextureParams": [],
        "m_BufferParams": [], "m_ConstantBuffers": [],
        "m_ConstantBufferBindings": [], "m_UAVParams": [], "m_Samplers": [],
        "m_ShaderRequirements": requirements,
    }


def convert_shader(tree):
    """Devolve (mudou, n_passes).  Altera `tree` no lugar."""
    if PLATFORM_GLES20 in tree["platforms"]:
        return False, 0

    parsed = tree["m_ParsedForm"]
    props = {}
    prop_order = []
    for p in parsed.get("m_PropInfo", {}).get("m_Props", []):
        props[p["m_Name"]] = p["m_Type"]
        prop_order.append(p["m_Name"])

    entries = []
    passes = 0
    for subshader_index, subshader in enumerate(parsed["m_SubShaders"]):
        for pass_index, a_pass in enumerate(subshader["m_Passes"]):
            vertex = a_pass.get("progVertex", {}).get("m_SubPrograms", [])
            fragment = a_pass.get("progFragment", {}).get("m_SubPrograms", [])
            if not vertex and not fragment:
                continue

            source_map = 0
            for sp in vertex:
                if sp["m_GpuProgramType"] in (GPU_SPIRV, 4):
                    source_map = sp["m_Channels"]["m_SourceMap"]
                    break
            if not source_map and vertex:
                source_map = vertex[0]["m_Channels"]["m_SourceMap"]
            if not source_map:
                source_map = 25   # posicao + cor + uv0

            glsl = generate(source_map, props, prop_order,
                            "%s|sub%d|pass%d" % (parsed["m_Name"],
                                                  subshader_index,
                                                  pass_index))
            vertex_index = len(entries)
            entries.append(build_entry(GPU_GLES2, glsl, source_map))
            fragment_index = len(entries)
            entries.append(build_entry(GPU_GLES2, "", 0))

            # Espelhar TODO conjunto de keywords que ja existe: assim a busca
            # por variante da Unity acha correspondencia exata, seja qual for o
            # estado de keyword na hora do desenho.  As entradas do blob sao
            # compartilhadas, entao isso custa so' os registros pequenos.
            for stage, blob_index in ((STAGES[0], vertex_index),
                                      (STAGES[1], fragment_index)):
                existing = a_pass.get(stage, {}).get("m_SubPrograms", [])
                if not existing:
                    continue
                seen = set()
                added = []
                for sp in existing:
                    key = (tuple(sp["m_KeywordIndices"]),
                           sp["m_ShaderHardwareTier"])
                    if key in seen:
                        continue
                    seen.add(key)
                    added.append(subprogram(blob_index, GPU_GLES2, key[1],
                                            key[0], source_map if stage ==
                                            STAGES[0] else 0,
                                            sp["m_ShaderRequirements"]))
                if not any(not k[0] for k in seen):
                    # Nenhuma variante de keyword vazia: acrescentar uma, que e'
                    # o alvo do fallback da Unity quando nada casa.
                    for tier in (0, 1, 2):
                        added.append(subprogram(blob_index, GPU_GLES2, tier, (),
                                                source_map if stage ==
                                                STAGES[0] else 0, 1))
                a_pass[stage]["m_SubPrograms"] = added + existing
            passes += 1

    if not entries:
        return False, 0

    segment = build_blob(entries)
    compressed = lz4.block.compress(segment, mode="high_compression",
                                    store_size=False)
    blob = bytes(tree["compressedBlob"])
    tree["platforms"] = [PLATFORM_GLES20] + list(tree["platforms"])
    tree["offsets"] = [len(blob)] + list(tree["offsets"])
    tree["compressedLengths"] = [len(compressed)] + list(tree["compressedLengths"])
    tree["decompressedLengths"] = [len(segment)] + list(tree["decompressedLengths"])
    tree["compressedBlob"] = blob + compressed
    return True, passes


def split_parts(path):
    parts = []
    index = 0
    while True:
        candidate = "%s.split%d" % (path, index)
        if not os.path.exists(candidate):
            break
        parts.append(candidate)
        index += 1
    return parts


def write_back(path, blob):
    """Preserva o layout em `.splitN` quando o arquivo veio assim."""
    parts = split_parts(path)
    if not parts:
        with open(path, "wb") as fh:
            fh.write(blob)
        return "single"
    chunk = os.path.getsize(parts[0])
    written = 0
    index = 0
    while written < len(blob):
        piece = blob[written:written + chunk]
        name = "%s.split%d" % (path, index)
        with open(name, "wb") as fh:
            fh.write(piece)
        written += len(piece)
        index += 1
    for stale in parts[index:]:
        os.remove(stale)
    return "split(%d)" % index


def process(path, dry_run=False):
    load_path = path if os.path.exists(path) else path + ".split0"
    env = UnityPy.load(load_path)
    replacements = {}
    names = []
    for obj in env.file.objects.values():
        if obj.type.name != "Shader":
            continue
        tree = obj.read_typetree()
        changed, passes = convert_shader(tree)
        if not changed:
            continue
        # save_typetree DEVOLVE os bytes; get_raw_data() rele o original e
        # ignora a alteracao -- ja custou uma rodada aqui.
        replacements[obj.path_id] = bytes(obj.save_typetree(tree))
        names.append((tree["m_ParsedForm"]["m_Name"], passes))
    if not replacements:
        return None

    parts = split_parts(path)
    if parts:
        blob = b"".join(open(p, "rb").read() for p in parts)
    else:
        blob = open(path, "rb").read()
    layout = SerializedFileLayout(
        blob, [(o.path_id, o.byte_start, o.byte_size)
               for o in env.file.objects.values()])
    if layout.rebuild({}) != layout.blob:
        raise SystemExit("%s: reescrita de no-op nao e' identica; recuse" % path)
    out = layout.rebuild(replacements)
    if not dry_run:
        how = write_back(path, out)
    else:
        how = "dry-run"
    return names, len(blob), len(out), how


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data_dir")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--only", default=None,
                    help="processar so' este basename")
    args = ap.parse_args()

    # `Resources/unity_builtin_extra` guarda 35 dos shaders e mora num
    # SUBDIRETORIO -- listar so' o topo o deixava de fora.
    bases = set()
    for root, _dirs, files in os.walk(args.data_dir):
        for name in files:
            if name.endswith((".resource", ".config", ".dll", ".xml")):
                continue
            full = os.path.join(root, name)
            bases.add(re.sub(r"\.split\d+$", "", full))

    total_shaders = total_passes = total_files = 0
    for base in sorted(bases):
        if args.only and os.path.basename(base) != args.only:
            continue
        path = base
        try:
            result = process(path, args.dry_run)
        except Exception as exc:            # noqa: BLE001
            print("ERRO %s: %s" % (base, exc))
            continue
        if not result:
            continue
        names, before, after, how = result
        total_files += 1
        total_shaders += len(names)
        total_passes += sum(n for _, n in names)
        print("%-46s %2d shader(s) %5d pass(es) %9d -> %9d  %s"
              % (os.path.relpath(base, args.data_dir), len(names),
                 sum(n for _, n in names), before, after, how))
    print("TOTAL: %d arquivo(s), %d shader(s), %d pass(es)"
          % (total_files, total_shaders, total_passes))


if __name__ == "__main__":
    main()
