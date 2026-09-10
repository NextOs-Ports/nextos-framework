#!/usr/bin/env python3
"""Troca BuildSettings.m_GraphicsAPIs de Vulkan(21) para GLES2(8) IN-PLACE.

Um int por um int: o arquivo nao muda de tamanho, nenhum objeto se move e a
tabela de objetos do SerializedFile continua valida.  E' o oposto de
re-serializar com UnityPy -- a armadilha que ja matou a Unity em outro port.

Uso: patch_graphics_api.py <globalgamemanagers> [--api N] [--check]
"""
import sys, struct, shutil, hashlib
import UnityPy

VULKAN, GLES2, GLES3 = 21, 8, 11


def find_object(path):
    env = UnityPy.load(path)
    for o in env.objects:
        if o.type.name == "BuildSettings":
            return o
    raise SystemExit("BuildSettings nao encontrado em %s" % path)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    check = "--check" in sys.argv
    want = GLES2
    if "--api" in sys.argv:
        want = int(sys.argv[sys.argv.index("--api") + 1])
    if not args:
        raise SystemExit(__doc__)
    path = args[0]

    obj = find_object(path)
    tree = obj.read_typetree()
    apis = tree["m_GraphicsAPIs"]
    print("m_GraphicsAPIs atual: %s" % apis)
    if check:
        return 0 if want in apis else 1
    if want in apis:
        print("ja contem %d; nada a fazer" % want)
        return 0
    if len(apis) != 1:
        raise SystemExit("esperava UMA api; achei %s -- patch in-place so vale "
                         "para tamanho identico" % apis)

    raw = obj.get_raw_data()
    # A lista e' serializada como int32 tamanho seguido dos int32 dos ids.
    needle = struct.pack("<ii", 1, apis[0])
    hits = [i for i in range(0, len(raw) - 7)
            if raw[i:i + 8] == needle]
    if len(hits) != 1:
        raise SystemExit("padrao [1, %d] aparece %d vez(es) no objeto; "
                         "recuse o patch cego" % (apis[0], len(hits)))

    with open(path, "rb") as fh:
        blob = fh.read()
    absolute = obj.byte_start + hits[0] + 4
    if struct.unpack_from("<i", blob, absolute)[0] != apis[0]:
        raise SystemExit("offset absoluto %d nao contem %d" % (absolute, apis[0]))

    shutil.copy2(path, path + ".orig")
    patched = bytearray(blob)
    struct.pack_into("<i", patched, absolute, want)
    with open(path, "wb") as fh:
        fh.write(patched)
    print("offset %d: %d -> %d  (backup em %s.orig)" %
          (absolute, apis[0], want, path))
    print("sha256 novo: %s" % hashlib.sha256(patched).hexdigest())

    conferido = find_object(path).read_typetree()["m_GraphicsAPIs"]
    print("relido: %s" % conferido)
    if want not in conferido:
        raise SystemExit("relido nao contem %d" % want)
    return 0


if __name__ == "__main__":
    sys.exit(main())
