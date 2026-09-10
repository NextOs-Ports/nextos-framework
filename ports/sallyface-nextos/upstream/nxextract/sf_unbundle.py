#!/usr/bin/env python3
"""Desmonta um UnityFS (.unity3d) no layout SOLTO -- SEM dependencias externas.

Reimplementacao stdlib-only do tools/unbundle_datapack.py (que usava UnityPy),
para rodar no APARELHO durante o nxextract (o device tem python3 mas nao tem
UnityPy/lz4/Pillow). Extrai cada sub-arquivo do container byte-a-byte, exatamente
como esta' dentro do UnityFS -- sem reserializacao. O Sally Face procura sozinho
`assets/bin/Data/resources.assets` e variantes; entregar os arquivos soltos
dispensa emular o Play Core/Play Asset Delivery.

Formato UnityFS (BE): assinatura "UnityFS\\0", versao u32, unityVersion cstr,
unityRevision cstr, size i64, compressedBlocksInfoSize u32,
uncompressedBlocksInfoSize u32, flags u32. flags&0x3F = compressao do blocksInfo
(0 none, 2/3 lz4/lz4hc); flags&0x80 = blocksInfo no fim; flags&0x200 = padding de
alinhamento (16) antes do blocksInfo. blocksInfo (descomprimido): hash[16],
blockCount i32, blocks[{uncompressedSize u32, compressedSize u32, flags u16}],
nodeCount i32, nodes[{offset i64, size i64, flags u32, path cstr}].
"""
import os
import struct
import sys


# ---- lz4 block decompress (pure python; serve lz4 e lz4hc) ----
def lz4_decompress(src, dst_size):
    dst = bytearray(dst_size)
    s = 0
    d = 0
    n = len(src)
    while s < n:
        token = src[s]; s += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[s]; s += 1
                lit += b
                if b != 255:
                    break
        dst[d:d + lit] = src[s:s + lit]
        s += lit
        d += lit
        if s >= n:
            break
        offset = src[s] | (src[s + 1] << 8); s += 2
        match = token & 15
        if match == 15:
            while True:
                b = src[s]; s += 1
                match += b
                if b != 255:
                    break
        match += 4
        p = d - offset
        if p < 0:
            raise ValueError("lz4: offset invalido")
        if offset >= match:
            # sem sobreposicao: slice de uma vez (rapido)
            dst[d:d + match] = dst[p:p + match]
            d += match
        else:
            # sobreposicao: byte a byte
            for _ in range(match):
                dst[d] = dst[p]
                d += 1; p += 1
    if d != dst_size:
        raise ValueError(f"lz4: descomprimiu {d}, esperado {dst_size}")
    return bytes(dst)


def _cstr(buf, pos):
    end = buf.index(0, pos)
    return buf[pos:end].decode("utf-8", "replace"), end + 1


def _read_at(fh, off, n):
    fh.seek(off)
    return fh.read(n)


def unbundle(bundle_path, out_dir):
    # STREAMING: nunca segura o blob de dados inteiro na RAM (o datapack do Sally
    # Face descomprime para ~1,6 GB e o aparelho tem pouca memoria). Descomprime
    # bloco a bloco para um arquivo temporario em disco e recorta os nos de la'.
    fh = open(bundle_path, "rb")
    try:
        head = fh.read(64)
        if head[:8] != b"UnityFS\0":
            raise SystemExit(f"{bundle_path}: nao e' UnityFS")
        pos = 8
        (version,) = struct.unpack_from(">I", head, pos); pos += 4
        _uv, pos = _cstr(head, pos)   # unityVersion
        _ur, pos = _cstr(head, pos)   # unityRevision
        (size,) = struct.unpack_from(">q", head, pos); pos += 8
        (ci_size,) = struct.unpack_from(">I", head, pos); pos += 4  # compressed blocksInfo
        (ui_size,) = struct.unpack_from(">I", head, pos); pos += 4  # uncompressed blocksInfo
        (flags,) = struct.unpack_from(">I", head, pos); pos += 4

        file_size = os.fstat(fh.fileno()).st_size
        if flags & 0x80:            # blocksInfo no fim do arquivo
            bi_off = file_size - ci_size
        else:
            if flags & 0x200:       # padding de alinhamento a 16 bytes
                pos = (pos + 15) & ~15
            bi_off = pos
        ci = _read_at(fh, bi_off, ci_size)
        comp = flags & 0x3F
        if comp in (2, 3):          # lz4 / lz4hc
            blocks_info = lz4_decompress(ci, ui_size)
        elif comp == 0:
            blocks_info = ci
        else:
            raise SystemExit(f"compressao de blocksInfo nao suportada: {comp}")

        # onde comecam os blocos de dados
        if flags & 0x80:
            data_off = pos
        else:
            data_off = bi_off + ci_size
        if flags & 0x200:           # BlockInfoNeedPaddingAtStart: dados alinhados a 16
            data_off = (data_off + 15) & ~15

        bp = 16                     # pula uncompressedDataHash[16]
        (block_count,) = struct.unpack_from(">i", blocks_info, bp); bp += 4
        blocks = []
        for _ in range(block_count):
            u, c, bf = struct.unpack_from(">IIH", blocks_info, bp); bp += 10
            blocks.append((u, c, bf))
        (node_count,) = struct.unpack_from(">i", blocks_info, bp); bp += 4
        nodes = []
        for _ in range(node_count):
            off, nsize, nf = struct.unpack_from(">qqI", blocks_info, bp); bp += 20
            path, bp = _cstr(blocks_info, bp)
            nodes.append((off, nsize, path))

        # descomprime cada bloco e grava sequencialmente no temporario
        os.makedirs(out_dir, exist_ok=True)
        blob_path = os.path.join(out_dir, ".sf_unblob.tmp")
        fh.seek(data_off)
        with open(blob_path, "wb") as bf_out:
            for (u, c, bflags) in blocks:
                chunk = fh.read(c)
                bc = bflags & 0x3F
                if bc in (2, 3):
                    bf_out.write(lz4_decompress(chunk, u))
                elif bc == 0:
                    bf_out.write(chunk)
                else:
                    raise SystemExit(f"compressao de bloco nao suportada: {bc}")
    finally:
        fh.close()

    # recorta os nos do temporario (seek+read, memoria baixa)
    total = 0
    try:
        with open(blob_path, "rb") as blob:
            for (off, nsize, path) in nodes:
                target = os.path.join(out_dir, path)
                d = os.path.dirname(target)
                if d:
                    os.makedirs(d, exist_ok=True)
                blob.seek(off)
                remaining = nsize
                with open(target, "wb") as out:
                    while remaining > 0:
                        buf = blob.read(min(remaining, 1 << 20))
                        if not buf:
                            break
                        out.write(buf)
                        remaining -= len(buf)
                total += nsize
    finally:
        try:
            os.remove(blob_path)
        except OSError:
            pass
    return len(nodes), total


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("uso: sf_unbundle.py <arquivo.unity3d> <diretorio-de-saida>\n")
        return 2
    n, total = unbundle(argv[1], argv[2])
    print(f"{os.path.basename(argv[1])}: {n} arquivos, {total} B")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
