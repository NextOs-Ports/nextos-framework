#!/usr/bin/env python3
"""Reetiqueta os shaders do Sally Face NO APARELHO -- stdlib apenas.

O pacote full-data provado no Mali-450 levava a arvore ja' preparada pelo
tools/shader_gles2_patch.py (UnityPy + lz4), que nao existem no device. Este
modulo reproduz o MESMO resultado byte-a-byte por delta-spec. O NXExtract
autentica antes da extracao os payloads Unity internos que selecionam o perfil;
nome, assinatura, tamanho e hash do container externo nunca participam dessa
decisao. Nenhum dado do jogo viaja no ZIP: o spec so' carrega posicoes, bytes
novos e hashes das saidas transformadas.

Duas operacoes:
  1. patch dos .assets soltos do datapack (contrato de shaders de cena);
  2. reconstrucao do data.unity3d: os nos internos sao recortados do
     container original (parser do sf_unbundle), recebem seus deltas e o
     UnityFS e' regravado com blocos SEM compressao (rapido e deterministico;
     o formato aceita comp=0 -- o proprio full-data ja' provou que a Unity
     abre um data.unity3d regravado).

Cada saida e' verificada por SHA-256. A entrada completa ja foi autenticada
pelo patch_selector do NXExtract e volta a ser ligada ao plano em sf_prepare;
uma divergencia de delta ou saida falha explicitamente, nunca produz textura
silenciosamente corrompida.
"""
import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sf_unbundle import lz4_decompress  # noqa: E402


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def apply_ops(data, ops):
    """Aplica deltas [(offset_in, remove_len, insert_hex)] em ordem.

    insert_hex pode ser uma string ou uma lista de blocos curtos (o spec
    quebra hex longo em pedacos para nenhuma linha do JSON ficar quilometrica
    -- auditor de release faz varredura por linha)."""
    out = bytearray()
    pos = 0
    for off, rem, ins in ops:
        if off < pos or off + rem > len(data):
            raise ValueError("delta fora dos limites")
        out += data[pos:off]
        if isinstance(ins, list):
            ins = "".join(ins)
        out += bytes.fromhex(ins)
        pos = off + rem
    out += data[pos:]
    return bytes(out)


def patch_file_with_ops(path, entry):
    data = open(path, "rb").read()
    if _sha(data) == entry["sha_out"]:
        return 0        # ja reetiquetado (retomada de execucao interrompida)
    out = apply_ops(data, entry["ops"])
    if _sha(out) != entry["sha_out"]:
        raise SystemExit("%s: perfil interno nao produziu a saida esperada" % path)
    tmp = path + ".retag.tmp"
    with open(tmp, "wb") as fh:
        fh.write(out)
    os.replace(tmp, path)
    return len(entry["ops"])


# ---- leitura do UnityFS original (mesmo formato do sf_unbundle) ----
def _read_bundle(path):
    fh = open(path, "rb")
    head = fh.read(64)
    if head[:8] != b"UnityFS\0":
        raise SystemExit("%s: nao e' UnityFS" % path)
    pos = 8
    (version,) = struct.unpack_from(">I", head, pos); pos += 4
    end = head.index(0, pos); uv = head[pos:end]; pos = end + 1
    end = head.index(0, pos); ur = head[pos:end]; pos = end + 1
    (_size,) = struct.unpack_from(">q", head, pos); pos += 8
    (ci_size,) = struct.unpack_from(">I", head, pos); pos += 4
    (ui_size,) = struct.unpack_from(">I", head, pos); pos += 4
    (flags,) = struct.unpack_from(">I", head, pos); pos += 4
    file_size = os.fstat(fh.fileno()).st_size
    if flags & 0x80:
        bi_off = file_size - ci_size
    else:
        if flags & 0x200:
            pos = (pos + 15) & ~15
        bi_off = pos
    fh.seek(bi_off)
    ci = fh.read(ci_size)
    comp = flags & 0x3F
    if comp in (2, 3):
        info = lz4_decompress(ci, ui_size)
    elif comp == 0:
        info = ci
    else:
        raise SystemExit("blocksInfo com compressao desconhecida")
    if flags & 0x80:
        data_off = pos
    else:
        data_off = bi_off + ci_size
    if flags & 0x200:
        data_off = (data_off + 15) & ~15
    bp = 16
    (bc,) = struct.unpack_from(">i", info, bp); bp += 4
    blocks = []
    for _ in range(bc):
        u, c, bf = struct.unpack_from(">IIH", info, bp); bp += 10
        blocks.append((u, c, bf))
    (nc,) = struct.unpack_from(">i", info, bp); bp += 4
    nodes = []
    for _ in range(nc):
        off, nsize, nf = struct.unpack_from(">qqI", info, bp); bp += 20
        end = info.index(0, bp)
        name = info[bp:end].decode(); bp = end + 1
        nodes.append((off, nsize, nf, name))
    blob = bytearray()
    fh.seek(data_off)
    for (u, c, bflags) in blocks:
        chunk = fh.read(c)
        k = bflags & 0x3F
        if k in (2, 3):
            blob += lz4_decompress(chunk, u)
        elif k == 0:
            blob += chunk
        else:
            raise SystemExit("bloco com compressao desconhecida")
    fh.close()
    return (version, bytes(uv), bytes(ur)), nodes, bytes(blob)


def rebuild_data_bundle(path, spec):
    """Regrava o data.unity3d com os nos reetiquetados, blocos comp=0."""
    expected = spec.get("bundle_sha_out")
    if expected:
        h = hashlib.sha256()
        with open(path, "rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b""):
                h.update(chunk)
        if h.hexdigest() == expected:
            return      # ja reconstruido (retomada)
    header, nodes, blob = _read_bundle(path)
    per_node = {e["name"]: e for e in spec["nodes"]}
    seen_nodes = set()
    out_nodes = []
    for off, nsize, nf, name in nodes:
        raw = blob[off:off + nsize]
        entry = per_node.get(name)
        if entry is not None:
            raw = apply_ops(raw, entry["ops"])
            if _sha(raw) != entry["sha_out"]:
                raise SystemExit("%s: retag interno divergiu" % name)
            seen_nodes.add(name)
        out_nodes.append((name, nf, raw))
    missing = sorted(set(per_node) - seen_nodes)
    if missing:
        raise SystemExit("data.unity3d: nos internos ausentes: %s" %
                         ", ".join(missing))

    new_blob = bytearray()
    node_rows = []
    for name, nf, raw in out_nodes:
        node_rows.append((len(new_blob), len(raw), nf, name))
        new_blob += raw

    # blocos de 1 MiB sem compressao
    block_rows = []
    step = 1 << 20
    for i in range(0, len(new_blob), step):
        n = min(step, len(new_blob) - i)
        block_rows.append((n, n, 0x40))   # 0x40 = streamed, comp none

    info = bytearray(b"\0" * 16)
    info += struct.pack(">i", len(block_rows))
    for u, c, bf in block_rows:
        info += struct.pack(">IIH", u, c, bf)
    info += struct.pack(">i", len(node_rows))
    for off, nsize, nf, name in node_rows:
        info += struct.pack(">qqI", off, nsize, nf)
        info += name.encode() + b"\0"
    info = bytes(info)

    # Cabecalho no MESMO formato do original (UnityFS v8, flags 0x240:
    # blocksInfo sem compressao + directory info + alinhamento de 16).
    version, uv, ur = header
    head = bytearray()
    head += b"UnityFS\0"
    head += struct.pack(">I", version)
    head += uv + b"\0"
    head += ur + b"\0"
    flags = 0x240
    header_len = len(head) + 8 + 4 + 4 + 4
    info_off = (header_len + 15) & ~15
    data_off = (info_off + len(info) + 15) & ~15
    total = data_off + len(new_blob)
    head += struct.pack(">q", total)
    head += struct.pack(">I", len(info))
    head += struct.pack(">I", len(info))
    head += struct.pack(">I", flags)

    tmp = path + ".retag.tmp"
    with open(tmp, "wb") as fh:
        fh.write(bytes(head))
        fh.write(b"\0" * (info_off - header_len))
        fh.write(info)
        fh.write(b"\0" * (data_off - info_off - len(info)))
        fh.write(bytes(new_blob))
    expected = spec.get("bundle_sha_out")
    if expected:      # o gerador roda uma vez sem pino para PINAR a saida
        out_sha = hashlib.sha256()
        with open(tmp, "rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b""):
                out_sha.update(chunk)
        if out_sha.hexdigest() != expected:
            os.remove(tmp)
            raise SystemExit("data.unity3d: reconstrucao divergiu do pinado")
    os.replace(tmp, path)


def main(argv):
    if len(argv) != 4:
        sys.stderr.write(
            "uso: sf_retag.py <spec.json> <perfil> <dir assets/bin/Data>\n")
        return 2
    spec = json.load(open(argv[1]))
    if (spec.get("schema") != "org.nextos.sallyface.retag-spec" or
            spec.get("schema_version") != 2 or
            not isinstance(spec.get("profiles"), list)):
        raise SystemExit("spec de retag invalido")
    matches = [item for item in spec["profiles"]
               if item.get("id") == argv[2]]
    if len(matches) != 1:
        raise SystemExit("perfil de retag ausente ou ambiguo: %s" % argv[2])
    profile = matches[0]
    data_dir = argv[3]
    n_ops = 0
    for entry in profile["loose_assets"]:
        n_ops += patch_file_with_ops(
            os.path.join(data_dir, entry["name"]), entry)
    rebuild_data_bundle(os.path.join(data_dir, "data.unity3d"),
                        profile["data_bundle"])
    print("sf_retag: perfil=%s %d arquivos soltos (%d deltas) + "
          "data.unity3d GLES2" %
          (profile["id"], len(profile["loose_assets"]), n_ops))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
