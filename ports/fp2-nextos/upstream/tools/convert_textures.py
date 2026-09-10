#!/usr/bin/env python3
"""ETC2_RGBA8 -> ETC1 RGB + ETC1 gemea de alfa (DUPLA CAMADA), offline.

POR QUE, medido neste port:
  * O Utgard (Mali-450) nao tem ETC2.  Com o dispositivo em GLES2 a propria
    Unity descomprime cada ETC2_RGBA8 na CPU e sobe **RGBA8888** -- o censo de
    upload do port mediu 54,0 Mpx em `GL_RGBA` so' ate a tela de titulo, e ZERO
    upload de ETC2.  Sao 32 bpp na GPU, mais o custo de descompressao a cada
    carga.
  * ETC1 e' nativo aqui (`GL_OES_compressed_ETC1_RGB8_texture` esta na lista de
    extensoes do aparelho) e sobe direto, sem CPU.  RGB 4 bpp + alfa 4 bpp = 8
    bpp: **4x menos memoria de GPU** que hoje, e nenhuma descompressao.
  * O alfa nao cabe no ETC1, entao vai numa SEGUNDA textura -- e' o esquema de
    dupla camada.  A Unity ja tem o mecanismo pronto: `Sprite.m_RD.alphaTexture`
    + `_AlphaTex`/`_EnableExternalAlpha` na familia Sprites, que os shaders
    GLES2 gerados por este port ja reproduzem.

ATENCAO ao enquadramento: contra o ETC2 ORIGINAL (8 bpp) a troca nao economiza
nada -- ETC1+alfa tambem da 8 bpp.  A economia e' contra os 32 bpp que a Unity
esta subindo HOJE neste aparelho.

Escala real medida (nao a do guia, que contava cada arquivo uma vez por
`.splitN`): **469** Texture2D em ETC2_RGBA8, 437,6 Mpx, NENHUMA em `.resource`
-- todo o dado esta embutido no proprio `.assets`, o que dispensa mexer nos
arquivos de streaming.
"""
import argparse
import ctypes
import hashlib
import os
import re
import subprocess
import sys

import UnityPy
import texture2ddecoder
from PIL import Image
from UnityPy.helpers import TypeTreeHelper
from UnityPy.streams import EndianBinaryWriter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from serialized_patch import SerializedFileLayout   # noqa: E402

ETC2_RGBA8 = 47
ETC_RGB4 = 34

_lib = None


def encoder(path=None):
    global _lib
    if _lib is None:
        src_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "..", "src")
        src = os.path.join(src_dir, "etc1.c")
        header = os.path.join(src_dir, "etc1.h")
        with open(src, "rb") as source, open(header, "rb") as include:
            source_id = hashlib.sha256(source.read() + include.read()).hexdigest()[:16]
        cache_root = os.environ.get("XDG_CACHE_HOME")
        if not cache_root:
            cache_root = os.path.join(os.path.expanduser("~"), ".cache")
        default_so = os.path.join(cache_root, "nextos-ports", "fp2",
                                  "libfp2etc1-%s.so" % source_id)
        so = path or os.environ.get("FP2_ETC1_LIB", default_so)
        if not os.path.exists(so):
            os.makedirs(os.path.dirname(os.path.realpath(so)), exist_ok=True)
            temporary = "%s.tmp.%d" % (so, os.getpid())
            try:
                subprocess.run(["cc", "-O3", "-shared", "-fPIC", "-o",
                                temporary, src], check=True)
                os.replace(temporary, so)
            finally:
                if os.path.exists(temporary):
                    os.unlink(temporary)
        _lib = ctypes.CDLL(so)
        _lib.ss_etc1_size.restype = ctypes.c_size_t
        _lib.ss_etc1_size.argtypes = [ctypes.c_int, ctypes.c_int]
        for name in ("ss_etc1_encode_rgba", "ss_etc1_encode_alpha"):
            getattr(_lib, name).argtypes = [
                ctypes.c_char_p, ctypes.c_int, ctypes.c_int,
                ctypes.c_size_t, ctypes.c_char_p]
    return _lib


def encode_pair(rgba, width, height):
    lib = encoder()
    size = lib.ss_etc1_size(width, height)
    rgb_out = ctypes.create_string_buffer(size)
    alpha_out = ctypes.create_string_buffer(size)
    lib.ss_etc1_encode_rgba(rgba, width, height, width * 4, rgb_out)
    lib.ss_etc1_encode_alpha(rgba, width, height, width * 4, alpha_out)
    return rgb_out.raw[:size], alpha_out.raw[:size]


def decode_mip_chain(tree):
    """Decode every ETC2 mip in Unity's stored (bottom-up) row order.

    ``Texture2D.image`` is intentionally presentation-oriented: UnityPy flips
    the rows before returning the PIL image.  Feeding those bytes directly
    back to a serialized Texture2D flips every atlas in game.  Decoding the
    raw mip payload ourselves preserves the on-disk row order and also avoids
    throwing away the eight source textures which carry mipmaps.
    """
    raw = bytes(tree["image data"])
    width = int(tree["m_Width"])
    height = int(tree["m_Height"])
    mip_count = max(1, int(tree.get("m_MipCount", 1)))
    cursor = 0
    levels = []
    for level in range(mip_count):
        size = ((width + 3) // 4) * ((height + 3) // 4) * 16
        payload = raw[cursor:cursor + size]
        if len(payload) != size:
            raise ValueError("ETC2 mip %d is truncated (%d != %d)" %
                             (level, len(payload), size))
        decoded_bgra = texture2ddecoder.decode_etc2a8(payload, width, height)
        image = Image.frombytes("RGBA", (width, height), decoded_bgra,
                                "raw", "BGRA")
        levels.append((image.tobytes(), width, height))
        cursor += size
        width = max(1, width // 2)
        height = max(1, height // 2)
    if cursor != len(raw):
        raise ValueError("ETC2 mip chain has %d unexplained byte(s)" %
                         (len(raw) - cursor))
    return levels


def encode_mip_chain(levels):
    rgb_chain = []
    alpha_chain = []
    for rgba, width, height in levels:
        rgb, alpha = encode_pair(rgba, width, height)
        rgb_chain.append(rgb)
        alpha_chain.append(alpha)
    return b"".join(rgb_chain), b"".join(alpha_chain)


def serialize(obj, tree):
    """Bytes de um typetree usando o NO de tipo de um objeto existente."""
    writer = EndianBinaryWriter(endian=obj.reader.endian)
    node = obj._get_typetree_node(None)
    TypeTreeHelper.write_typetree(tree, node, writer, obj.assets_file)
    return writer.bytes


def split_parts(path):
    parts = []
    index = 0
    while os.path.exists("%s.split%d" % (path, index)):
        parts.append("%s.split%d" % (path, index))
        index += 1
    return parts


def write_back(path, blob):
    parts = split_parts(path)
    if not parts:
        with open(path, "wb") as fh:
            fh.write(blob)
        return "single"
    chunk = os.path.getsize(parts[0])
    written = index = 0
    while written < len(blob):
        with open("%s.split%d" % (path, index), "wb") as fh:
            fh.write(blob[written:written + chunk])
        written += chunk
        index += 1
    for stale in parts[index:]:
        os.remove(stale)
    return "split(%d)" % index


def process(path, dry_run=False):
    load_path = path if os.path.exists(path) else path + ".split0"
    env = UnityPy.load(load_path)
    environment_objects = list(env.objects)
    if not environment_objects:
        return None
    assets_files = {id(obj.assets_file): obj.assets_file
                    for obj in environment_objects}
    if len(assets_files) != 1:
        raise ValueError("base contains %d serialized files; refuse" %
                         len(assets_files))
    assets_file = next(iter(assets_files.values()))
    objects = list(assets_file.objects.values())
    next_path_id = max(o.path_id for o in objects) + 1

    replacements = {}
    additions = []
    alpha_of = {}
    pixels = 0
    for obj in objects:
        if obj.type.name != "Texture2D":
            continue
        tree = obj.read_typetree()
        if tree["m_TextureFormat"] != ETC2_RGBA8:
            continue
        if (tree.get("m_StreamData") or {}).get("path"):
            print("  PULADO (streamed): %s" % tree["m_Name"])
            continue
        width, height = tree["m_Width"], tree["m_Height"]
        levels = decode_mip_chain(tree)
        rgb, alpha = encode_mip_chain(levels)
        pixels += width * height

        alpha_tree = dict(tree)
        alpha_tree["m_Name"] = (tree["m_Name"] or "tex") + "_alpha"
        alpha_tree["m_TextureFormat"] = ETC_RGB4
        alpha_tree["image data"] = alpha
        alpha_tree["m_CompleteImageSize"] = len(alpha)
        alpha_id = next_path_id
        next_path_id += 1
        additions.append((alpha_id, obj.type_id, serialize(obj, alpha_tree)))
        alpha_of[obj.path_id] = alpha_id

        tree["m_TextureFormat"] = ETC_RGB4
        tree["image data"] = rgb
        tree["m_CompleteImageSize"] = len(rgb)
        replacements[obj.path_id] = serialize(obj, tree)

    if not replacements:
        return None

    sprites = 0
    for obj in objects:
        if obj.type.name != "Sprite":
            continue
        tree = obj.read_typetree()
        target = (tree.get("m_RD", {}).get("texture") or {})
        if target.get("m_FileID") != 0:
            continue
        alpha_id = alpha_of.get(target.get("m_PathID"))
        if alpha_id is None:
            continue
        tree["m_RD"]["alphaTexture"] = {"m_FileID": 0, "m_PathID": alpha_id}
        replacements[obj.path_id] = serialize(obj, tree)
        sprites += 1

    parts = split_parts(path)
    blob = b"".join(open(p, "rb").read() for p in parts) if parts \
        else open(path, "rb").read()
    layout = SerializedFileLayout(
        blob, [(o.path_id, o.byte_start, o.byte_size) for o in objects])
    if layout.rebuild({}) != layout.blob:
        raise SystemExit("%s: no-op nao e' identico; recuse" % path)
    out = layout.rebuild(replacements, additions)
    how = "dry-run" if dry_run else write_back(path, out)
    return len(additions), sprites, pixels, len(blob), len(out), how


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data_dir")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--only")
    args = ap.parse_args()

    bases = set()
    for root, _dirs, files in os.walk(args.data_dir):
        if os.sep + "Managed" in root:
            continue
        for name in files:
            if name.endswith((".resource", ".config")):
                continue
            bases.add(re.sub(r"\.split\d+$", "", os.path.join(root, name)))

    textures = sprites = 0
    pixels = 0
    for base in sorted(bases):
        if args.only and os.path.basename(base) != args.only:
            continue
        try:
            result = process(base, args.dry_run)
        except Exception as exc:            # noqa: BLE001
            if "SerializedFile" not in str(exc):
                print("ERRO %s: %s" % (os.path.basename(base), exc))
            continue
        if not result:
            continue
        count, sprite_count, px, before, after, how = result
        textures += count
        sprites += sprite_count
        pixels += px
        print("%-46s %4d textura(s) %5d sprite(s) %10d -> %10d  %s"
              % (os.path.relpath(base, args.data_dir), count, sprite_count,
                 before, after, how))
    print("TOTAL: %d textura(s) convertida(s), %d sprite(s) religado(s), "
          "%.1f Mpx" % (textures, sprites, pixels / 1e6))


if __name__ == "__main__":
    main()
