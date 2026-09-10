#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Streets of Rage 4 -- extrator BYO-DATA: tira do APK (sua copia legal) tudo que o
# port precisa, igual ao modelo Bully/Dysmantle (so que aqui o SOR4.dll vem do
# assembly-store XABA+LZ4 do .NET-Android, nao e unzip simples).
#
# Saida (em GAMEDIR):
#   gameassets/<assets inteiro>   <- TUDO de assets/ do APK (bigfile, .xnb, .wem,
#                                    .bnk, fontes, videos, shader, sem-extensao...).
#                                    O AssetManager do jogo le a raiz = gameassets.
#   host_pkg/libs/libWwise.real.so
#   host_pkg/<fontes .ttf/.otf>   <- copia das fontes tb p/ o host (SharpFont)
#   SOR4.dll                      <- extraido do assembly-store (XALZ/LZ4 puro-python)
#
# uso: sor4_apkextract.py --game-dir <dir> --apk <apk> [--apk <split> ...]
import sys, os, struct, zipfile, shutil, binascii

from sor4_apkset import ApkSet

def log(msg): print(msg, flush=True)
def pct(p):
    n = p // 5
    print("    [%s] %d%%" % ("#"*n + "."*(20-n), p), flush=True)

def copy_entry_atomic(apk_set, record, destination):
    """Stream one ZIP entry to a same-filesystem temporary file, then rename."""
    info = record[1]
    try:
        if os.path.getsize(destination) == info.file_size:
            value = 0
            with open(destination, "rb") as existing:
                while True:
                    block = existing.read(1024 * 1024)
                    if not block:
                        break
                    value = binascii.crc32(block, value)
            if (value & 0xFFFFFFFF) == info.CRC:
                return False
    except OSError:
        pass
    parent = os.path.dirname(destination)
    if parent:
        os.makedirs(parent, exist_ok=True)
    temporary = destination + ".sor4-part"
    try:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        with apk_set.open(record) as source, open(temporary, "xb") as output:
            shutil.copyfileobj(source, output, 1024 * 1024)
            output.flush()
            os.fsync(output.fileno())
        if os.path.getsize(temporary) != info.file_size:
            raise IOError("short extraction for " + info.filename)
        os.replace(temporary, destination)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
    return True

def safe_destination(root, relative):
    root = os.path.realpath(root)
    destination = os.path.realpath(os.path.join(root, *relative.split("/")))
    try:
        inside = os.path.commonpath((root, destination)) == root
    except ValueError:
        inside = False
    if not inside:
        raise ValueError("asset destination escapes output root: " + relative)
    return destination

def build_safe_asset_plan(apk_set, gameassets, host_dir):
    """Reject every ambiguous ZIP path before writing the first payload byte."""
    assets = []; fonts = []
    for source_index, info, relative in apk_set.assets:
        destination = safe_destination(gameassets, relative)
        assets.append(((source_index, info), relative, destination))
    for source_index, info, relative in apk_set.fonts:
        basename = relative.rsplit("/", 1)[-1]
        fonts.append(((source_index, info), safe_destination(host_dir, basename)))
    return assets, fonts

# ---------- LZ4 block decompress (puro python; sem modulo) ----------
def lz4_block_decompress(src, usize):
    out = bytearray(usize); op = 0; ip = 0; n = len(src)
    while ip < n:
        token = src[ip]; ip += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[ip]; ip += 1; lit += b
                if b != 255: break
        out[op:op+lit] = src[ip:ip+lit]; op += lit; ip += lit
        if ip >= n: break
        off = src[ip] | (src[ip+1] << 8); ip += 2
        ml = token & 15
        if ml == 15:
            while True:
                b = src[ip]; ip += 1; ml += b
                if b != 255: break
        ml += 4; mp = op - off
        if off >= ml:
            out[op:op+ml] = out[mp:mp+ml]; op += ml
        else:
            for _ in range(ml):
                out[op] = out[mp]; op += 1; mp += 1
    return bytes(out[:op])

# ---------- acha e descomprime o SOR4.dll no blob (XALZ com usize alvo) ----------
MANAGED_ASSEMBLIES = {
    0: "_Microsoft.Android.Resource.Designer",
    1: "Xamarin.Android.Google.BillingClient",
    2: "Xamarin.AndroidX.Activity",
    3: "Xamarin.AndroidX.Core",
    4: "Xamarin.AndroidX.Fragment",
    5: "Xamarin.AndroidX.Lifecycle.Common.Jvm",
    6: "Xamarin.AndroidX.Lifecycle.LiveData.Core",
    7: "Xamarin.AndroidX.Lifecycle.ViewModel.Android",
    8: "Xamarin.AndroidX.Loader",
    9: "Xamarin.AndroidX.SavedState.SavedState.Android",
    10: "Xamarin.Firebase.Common",
    11: "Xamarin.Firebase.Config",
    12: "Xamarin.Google.Android.Play.Core",
    13: "Xamarin.GooglePlayServices.Auth",
    14: "Xamarin.GooglePlayServices.Base",
    15: "Xamarin.GooglePlayServices.Basement",
    16: "Xamarin.GooglePlayServices.Drive",
    17: "Xamarin.GooglePlayServices.Games",
    18: "Xamarin.GooglePlayServices.Measurement.Api",
    19: "Xamarin.GooglePlayServices.Tasks",
    20: "Xamarin.Kotlin.StdLib",
    21: "Xamarin.KotlinX.Coroutines.Core.Jvm",
    22: "EOSSDK.Android",
    23: "HelpshiftSDKx.Android",
    # 24 is the Android MonoGame build; the port supplies its patched Linux build.
    25: "SharpFont.Core",
    26: "StandaloneTypeModel.Android.Retail",
    27: "SOR4",
}

def write_bytes_atomic(path, data):
    temporary = path + ".sor4-part"
    try:
        os.unlink(temporary)
    except FileNotFoundError:
        pass
    with open(temporary, "xb") as output:
        output.write(data); output.flush(); os.fsync(output.fileno())
    os.replace(temporary, path)

def extract_managed_assemblies(blob, game_dir, host_dir):
    pos = 0; entries = []
    while True:
        i = blob.find(b"XALZ", pos)
        if i < 0: break
        idx, usize = struct.unpack_from("<II", blob, i + 4)
        entries.append((i, idx, usize)); pos = i + 4
    extracted = set()
    for number, (off, idx, usize) in enumerate(entries):
        name = MANAGED_ASSEMBLIES.get(idx)
        if name is None:
            continue
        ds = off + 12
        nxt = entries[number + 1][0] if number + 1 < len(entries) else len(blob)
        comp = blob[ds:nxt]
        try:
            dec = lz4_block_decompress(comp, usize)
        except Exception:
            return False
        if len(dec) != usize or dec[:2] != b"MZ":
            return False
        destination = (os.path.join(game_dir, "SOR4.dll") if name == "SOR4"
                       else os.path.join(host_dir, name + ".dll"))
        write_bytes_atomic(destination, dec)
        extracted.add(idx)
    return extracted == set(MANAGED_ASSEMBLIES)

def parse_args(argv):
    # Keep the original two-positional-argument interface for old local recipes.
    if len(argv) >= 3 and not argv[1].startswith("-"):
        return [argv[1]], argv[2], "--libs-only" in argv[3:]
    apks = []; game_dir = None; libs_only = False; index = 1
    while index < len(argv):
        argument = argv[index]
        if argument == "--apk" and index + 1 < len(argv):
            index += 1; apks.append(argv[index])
        elif argument == "--game-dir" and index + 1 < len(argv):
            index += 1; game_dir = argv[index]
        elif argument == "--libs-only":
            libs_only = True
        else:
            raise ValueError("unknown or incomplete argument: " + argument)
        index += 1
    if not apks or not game_dir:
        raise ValueError("at least one --apk and --game-dir are required")
    return apks, game_dir, libs_only


def main():
    try:
        apks, gd, libs_only = parse_args(sys.argv)
    except ValueError as error:
        log("ERRO: %s" % error)
        log("uso: sor4_apkextract.py --game-dir DIR --apk GAME.apk [--apk SPLIT.apk ...]")
        return 2
    libs = os.path.join(gd, "host_pkg", "libs")
    ga   = os.path.join(gd, "gameassets")
    hp   = os.path.join(gd, "host_pkg")
    try:
        apk_set = ApkSet(apks)
        assets, fonts = build_safe_asset_plan(apk_set, ga, hp)
    except (OSError, zipfile.BadZipFile, ValueError, RuntimeError, NotImplementedError) as error:
        log("ERRO: " + str(error)); return 4
    try:
        for d in (libs, ga, hp): os.makedirs(d, exist_ok=True)

        # 1) libWwise real
        log(">> Biblioteca de audio (libWwise)..."); pct(1)
        wwise = apk_set.get("lib/arm64-v8a/libWwise.so")
        if wwise is None:
            log("ERRO: libWwise.so nao foi encontrada no conjunto APK"); return 3
        copy_entry_atomic(apk_set, wwise, os.path.join(libs, "libWwise.real.so"))

        # 2) SOR4.dll do assembly-store
        log(">> Codigo do jogo (SOR4.dll do assembly-store)..."); pct(3)
        assemblies = apk_set.get("lib/arm64-v8a/libassemblies.arm64-v8a.blob.so")
        if assemblies is None:
            log("ERRO: assembly store ARM64 nao encontrado no conjunto APK"); return 3
        with apk_set.open(assemblies) as source:
            blob = source.read()
        if not extract_managed_assemblies(blob, gd, hp):
            log("ERRO: nao consegui extrair as assemblies gerenciadas suportadas."); return 3
        log("  SOR4.dll + dependencias gerenciadas OK")
        del blob

        # 3) virtual assets/ tree -> gameassets/ across base/data splits.
        if not libs_only:
            total = len(assets); done = 0
            log(">> Dados do jogo (%d arquivos em %d APK(s))..." % (total, len(apks)))
            for record, rel, dst in assets:
                copied = copy_entry_atomic(apk_set, record, dst)
                done += 1
                p = (done * 100) // max(total, 1)
                suffix = "" if copied else "  (resume)"
                print("%d/%d  %d%%  %s%s" % (done, total, p, rel, suffix), flush=True)

        # Fontes tambem no host_pkg (o host/SharpFont procura ali).
        for record, destination in fonts:
            copy_entry_atomic(apk_set, record, destination)
    finally:
        apk_set.close()

    pct(96)
    log(">> Extracao do conjunto APK concluida.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
