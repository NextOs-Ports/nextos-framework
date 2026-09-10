#!/usr/bin/env python3
"""Funde a arvore de dados do Suzy Cube dentro do stage do NXExtract.

O jogo entrega `assets/bin/Data` em duas fontes que so' juntas formam a arvore
completa, e AMBAS vem fatiadas em `.splitN`:

  * o proprio APK, com 33 entradas (globalgamemanagers.assets e
    sharedassets0.assets em 7 pedacos cada);
  * um OBB embutido em `assets/sc.txt` (zip -> .obb -> zip), com 529 entradas,
    das quais 399 sao pedacos.

Regras que este script existe para garantir:

  * a remontagem e' em ORDEM NUMERICA.  Ordem lexicografica poe `split10` antes
    de `split9` e corrompe o arquivo em silencio -- o jogo abre e quebra depois.
  * um indice faltando e' ERRO, nunca um arquivo truncado.
  * o APK e' canonico: o OBB nunca sobrepoe um arquivo que o APK ja' trouxe.
  * cada pedaco e' apagado assim que entra no arquivo final, e o payload
    intermediario some no fim.  Sem isso o pico de espaco dobra, o que importa
    no ArkOS, onde o cartao costuma estar apertado.

O NXExtract chama este hook entre a extracao e a validacao final: se ele falhar,
o stage inteiro e' descartado e nada e' commitado no diretorio do jogador.
"""

import io
import os
import re
import shutil
import sys
import zipfile

SPLIT = re.compile(r"^(.*)\.split(\d+)$")
DATA = os.path.join("assets", "bin", "Data")
# Vive sob `assets` porque todo destino precisa cair numa raiz de
# commit do NXExtract; este hook o apaga antes da validacao final.
PAYLOAD = os.path.join("assets", ".nxpayload")
CHUNK = 1 << 20


def progress(done, total):
    """Protocolo que a UI do NXExtract entende; sem isto a barra parece travada
    durante os ~400 MB de remontagem."""
    print("NXEXTRACT_PROGRESS %d %d" % (done, total), flush=True)


def fail(message):
    print("merge-suzycube-data: %s" % message, file=sys.stderr, flush=True)
    raise SystemExit(1)


def unpack_obb(stage):
    """Extrai o OBB (embutido em payload/sc.zip ou avulso em payload/main.obb)
    por cima da arvore de dados, sem sobrepor o que o APK ja' colocou."""
    embedded = os.path.join(stage, PAYLOAD, "sc.zip")
    standalone = os.path.join(stage, PAYLOAD, "main.obb")
    blob = None

    if os.path.isfile(embedded):
        with zipfile.ZipFile(embedded) as outer:
            names = [n for n in outer.namelist() if n.lower().endswith(".obb")]
            if not names:
                fail("sc.zip nao contem nenhum .obb")
            blob = io.BytesIO(outer.read(names[0]))
    elif os.path.isfile(standalone):
        blob = open(standalone, "rb")
    else:
        # Nem toda distribuicao do jogo traz OBB; se a arvore do APK ja' estiver
        # completa a validacao final decide, nao este hook.
        print("merge-suzycube-data: sem OBB; usando apenas a arvore do APK",
              flush=True)
        return 0

    written = 0
    with zipfile.ZipFile(blob) as obb:
        members = [m for m in obb.infolist()
                   if not m.is_dir() and m.filename.startswith(DATA + "/")]
        total = len(members)
        for index, member in enumerate(members, 1):
            relative = member.filename[len(DATA) + 1:]
            target = os.path.join(stage, DATA, relative)
            if os.path.exists(target):
                continue                      # o APK e' canonico
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with obb.open(member) as source, open(target, "wb") as sink:
                shutil.copyfileobj(source, sink, CHUNK)
            written += 1
            if index % 25 == 0 or index == total:
                progress(index, total)
    if hasattr(blob, "close"):
        blob.close()
    return written


def join_splits(stage):
    root = os.path.join(stage, DATA)
    if not os.path.isdir(root):
        fail("arvore %s ausente no stage" % DATA)

    groups = {}
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            match = SPLIT.match(name)
            if match:
                base = os.path.join(dirpath, match.group(1))
                groups.setdefault(base, []).append(
                    (int(match.group(2)), os.path.join(dirpath, name)))

    total = len(groups)
    for done, base in enumerate(sorted(groups), 1):
        parts = sorted(groups[base], key=lambda item: item[0])
        indices = [item[0] for item in parts]
        if indices != list(range(len(indices))):
            fail("pedaco faltando em %s: %s"
                 % (os.path.relpath(base, stage), indices))
        with open(base, "wb") as sink:
            for _, path in parts:
                with open(path, "rb") as source:
                    shutil.copyfileobj(source, sink, CHUNK)
                # Apagar aqui, e nao no fim, mantem o pico de espaco baixo.
                os.remove(path)
        if done % 10 == 0 or done == total:
            progress(done, total)
    return total


def main():
    if len(sys.argv) != 2:
        fail("uso: merge-suzycube-data.py <stage>")
    stage = sys.argv[1]
    if not os.path.isdir(stage):
        fail("stage inexistente: %s" % stage)

    copied = unpack_obb(stage)
    joined = join_splits(stage)
    shutil.rmtree(os.path.join(stage, PAYLOAD), ignore_errors=True)

    # Mesma regua do checkpoint do NXExtract, mas com um erro que ensina o
    # caminho: a distribuicao da Play traz o OBB SEPARADO do APK, e quem copia
    # so o APK cai aqui -- os outros ports sao APK-unico e instalam, este nao.
    for name in ("globalgamemanagers.assets", "sharedassets0.assets"):
        path = os.path.join(stage, DATA, name)
        if os.path.isfile(path) and os.path.getsize(path) >= 6000000:
            continue
        fail(
            "the APK alone does not contain the full game data (%s is "
            "missing). Copy the matching main.<n>.com.noodlecake.suzycube.obb "
            "from your device (Android/obb/com.noodlecake.suzycube/) into "
            "suzycube/gamedata next to the APK and launch the game again."
            % name)

    print("merge-suzycube-data: %d arquivos do OBB, %d remontados"
          % (copied, joined), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
