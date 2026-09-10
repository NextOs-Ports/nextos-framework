#!/usr/bin/env python3
"""Hook de preparo dos dados do Sally Face para o NXExtract.

O jogo NAO abre o asset pack: a Unity espera a arvore de cenas ja desmontada
(613 entradas do `datapack.unity3d`), exatamente como o pacote privado
full-data entregava. O `sf_unbundle.py` faz isso em STREAMING (le o blob em
blocos de 1 MB), que e o unico caminho viavel num aparelho de 916 MB — o
preparo por UnityPy do `sallyface_stage.py` precisa de varios GB e nao roda no
device.

Depois da desmontagem, um patch_selector do NXExtract escolhe o perfil interno
de retag GLES2. A escolha usa apenas `data.unity3d` e `datapack.unity3d`, nunca
nome, assinatura, versao ou hash do APK externo. Perfis conhecidos reproduzem
o resultado provado do `tools/shader_gles2_patch.py`; um payload compativel mas
novo segue o fallback generico sem transformacao cega. A reducao de texturas
oversized continua em RUNTIME no loader. Nada altera a copia em `gamedata/`.
"""
import hashlib
import json
import os
import subprocess
import sys


PREPARE_RECEIPT = ".sallyface-prepare-arm64-4.json"
PREPARE_RECIPE_VERSION = "arm64-4"


def fail(message):
    sys.stderr.write("sallyface prepare: %s\n" % message)
    raise SystemExit(1)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def patch_selection():
    """Return (profile, authenticated hashes) or the generic fallback."""
    raw = os.environ.get("NXEXTRACT_COMPATIBILITY_JSON", "")
    try:
        document = json.loads(raw)
        selections = document["patch_selections"]
    except (KeyError, TypeError, ValueError):
        return None, {}
    selected = {}
    hashes = {}
    for item in selections:
        if not isinstance(item, dict):
            return None, {}
        member = item.get("member")
        if member in ("assets/bin/Data/data.unity3d",
                      "assets/bin/Data/datapack.unity3d"):
            selected[member] = item.get("profile")
            if item.get("state") == "matched":
                hashes[member] = item.get("payload_sha256")
    pairs = {
        ("sf-data-a", "sf-datapack-a"): "sf-assets-a",
        ("sf-data-b", "sf-datapack-b"): "sf-assets-b",
    }
    profile = pairs.get((
        selected.get("assets/bin/Data/data.unity3d"),
        selected.get("assets/bin/Data/datapack.unity3d"),
    ))
    return profile, hashes


def write_prepare_receipt(stage, profile):
    """Publish the persistent arm64-4 migration receipt as the last hook step."""
    value = {
        "schema": "org.nextos.sallyface.prepare-contract",
        "schema_version": 1,
        "recipe_version": PREPARE_RECIPE_VERSION,
        "profile": profile or "generic-pass-through",
    }
    payload = (
        json.dumps(value, sort_keys=True, separators=(",", ":"),
                   ensure_ascii=True) + "\n"
    ).encode("ascii")
    path = os.path.join(stage, "assets", PREPARE_RECEIPT)
    temporary = path + ".nxpart"
    try:
        if os.path.lexists(temporary):
            os.unlink(temporary)
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
        if hasattr(os, "O_NOFOLLOW"):
            flags |= os.O_NOFOLLOW
        descriptor = os.open(temporary, flags, 0o600)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except OSError as error:
        try:
            if os.path.lexists(temporary):
                os.unlink(temporary)
        except OSError:
            pass
        fail("nao foi possivel publicar o recibo arm64-4: %s" % error)
    return path


def main():
    if len(sys.argv) != 3:
        fail("uso: sf_prepare.py <stage> <game_dir>")
    stage, game_dir = sys.argv[1], sys.argv[2]
    data = os.path.join(stage, "assets", "bin", "Data")
    datapack = os.path.join(data, "datapack.unity3d")
    unbundle = os.path.join(game_dir, "nxextract", "sf_unbundle.py")

    if not os.path.isdir(data):
        fail("arvore assets/bin/Data ausente no stage")
    if not os.path.isfile(unbundle):
        fail("sf_unbundle.py ausente no port")
    retag = os.path.join(game_dir, "nxextract", "sf_retag.py")
    spec = os.path.join(game_dir, "nxextract", "sf_retag_spec.json")
    if not os.path.isfile(retag) or not os.path.isfile(spec):
        fail("sf_retag.py/spec ausentes no port")
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    profile, selected_hashes = patch_selection()

    # Revalida os bytes exatos que o NXExtract autenticou no planejamento.
    # A comparacao e dinamica contra o receipt do engine; nenhum hash literal
    # ou identidade do container mora neste helper.
    if profile and os.path.isfile(datapack):
        for member, path in (
                ("assets/bin/Data/data.unity3d",
                 os.path.join(data, "data.unity3d")),
                ("assets/bin/Data/datapack.unity3d", datapack)):
            expected = selected_hashes.get(member)
            if (not expected or not os.path.isfile(path) or
                    sha256_file(path) != expected):
                fail("payload selecionado mudou depois do planejamento: %s" %
                     member)

    if os.path.isfile(datapack):
        before = len(os.listdir(data))
        subprocess.run(
            [sys.executable, "-B", unbundle, datapack, data],
            check=True, env=env,
        )
        after = len(os.listdir(data))
        if after <= before:
            fail("desmontagem nao produziu entradas novas (%d -> %d)"
                 % (before, after))
        # O pack ja foi consumido: mante-lo custaria 533 MB no cartao do
        # jogador e a Unity nunca o abre (o full-data tambem nao o levava).
        os.remove(datapack)
        print("sallyface prepare: datapack desmontado (%d -> %d entradas)"
              % (before, after - 1))
    else:
        # Retomada de execucao interrompida: desmontagem ja feita; o retag
        # abaixo e idempotente (arquivo ja no SHA de saida = pulado).
        print("sallyface prepare: datapack ja desmontado")

    if profile:
        subprocess.run(
            [sys.executable, "-B", retag, spec, profile, data],
            check=True, env=env,
        )
        print("SALLY-PATCH-PROFILE: applied=%s fallback=0" % profile)
    else:
        print("SALLY-PATCH-PROFILE: applied=generic-pass-through fallback=1")
    write_prepare_receipt(stage, profile)
    print("SALLY-PREPARE-RECEIPT: recipe=arm64-4 profile=%s" %
          (profile or "generic-pass-through"))
    print("NXEXTRACT_PREDICATE patch_selection sallyface-retag ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
