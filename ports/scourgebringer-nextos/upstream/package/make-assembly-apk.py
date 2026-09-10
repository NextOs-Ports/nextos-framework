#!/usr/bin/env python3
"""Gera um APK reduzido contendo apenas o que o monodroid precisa ler por mmap.

O ScourgeBringer guarda o assembly store dentro do proprio APK
Em APKs 1.61.16 o store usa ``assemblies/``; no APK 1.61 b19 ele usa a DSO
``libassemblies.arm64-v8a.blob.so``. O monodroid abre os APKs de
``runtimeApks``, varre o diretorio central e exige entradas STORED/alinhadas.
Copiar o APK inteiro seria desperdicio, entao reescrevemos somente o manifesto
e o formato de assembly encontrado, com dados alinhados a 16 KiB.
"""
import os
import struct
import sys
import zipfile

ALIGN = 16384
KEEP_PREFIXES = ("assemblies/",)
KEEP_NAMES = ("AndroidManifest.xml",)


def is_assembly_blob(name):
    base = name.rsplit("/", 1)[-1]
    return (name.startswith("lib/") and
            base.startswith("libassemblies.") and
            base.endswith(".blob.so"))


def build(src_apk, dst_apk):
    entries = []
    layout = None
    with zipfile.ZipFile(src_apk) as z:
        names = set(z.namelist())
        classic = any(name.startswith(KEEP_PREFIXES) for name in names)
        blobs = {name for name in names if is_assembly_blob(name)}
        if classic:
            layout = "assemblies-directory"
        elif blobs:
            layout = "assembly-blob-dso"
        else:
            raise SystemExit("nenhum assembly store compativel em %s" % src_apk)
        for info in z.infolist():
            keep_classic = classic and info.filename.startswith(KEEP_PREFIXES)
            keep_blob = not classic and info.filename in blobs
            if keep_classic or keep_blob or info.filename in KEEP_NAMES:
                entries.append((info.filename, z.read(info.filename)))
    if not entries:
        raise SystemExit("nenhuma entrada de runtime encontrada em %s" % src_apk)

    out = open(dst_apk, "wb")
    central = []
    for name, data in entries:
        nb = name.encode("utf-8")
        local_header_len = 30 + len(nb)
        offset = out.tell()
        pad = (-(offset + local_header_len)) % ALIGN
        extra = b"\0" * pad
        crc = zipfile.crc32(data) & 0xFFFFFFFF
        out.write(struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, 0, 0, 0,
                              crc, len(data), len(data), len(nb), len(extra)))
        out.write(nb)
        out.write(extra)
        data_off = out.tell()
        assert data_off % ALIGN == 0, (name, data_off)
        out.write(data)
        central.append((nb, crc, len(data), offset))

    cd_start = out.tell()
    for nb, crc, size, offset in central:
        out.write(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0, 0, 0, 0,
                              crc, size, size, len(nb), 0, 0, 0, 0, 0, offset))
        out.write(nb)
    cd_size = out.tell() - cd_start
    out.write(struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(central), len(central),
                          cd_size, cd_start, 0))
    out.close()

    with zipfile.ZipFile(dst_apk) as z:
        bad = z.testzip()
        if bad:
            raise SystemExit("zip invalido: %s" % bad)
        for info in z.infolist():
            print("  %-40s %9d bytes  compress=%d" %
                  (info.filename, info.file_size, info.compress_type))
    print("OK: layout=%s %s (%d bytes)" %
          (layout, dst_apk, os.path.getsize(dst_apk)))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("uso: make_assembly_apk.py <apk-original> <apk-saida>")
    build(sys.argv[1], sys.argv[2])
