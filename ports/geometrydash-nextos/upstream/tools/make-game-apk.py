#!/usr/bin/env python3
"""Normaliza o `game.apk` que o so-loader le' com minizip.

Por que existir: o jogo pede os proprios recursos por caminho `assets/...`
dentro de um zip, e `src/apk_asset.c` serve alguns audios COPIANDO OS BYTES
CRUS -- ou seja, so' enxerga entrada STORED. O APK que o usuario traz mistura
STORED e DEFLATE. Este passo reescreve o pacote ja' copiado pelo NXExtract
mantendo so' o que o jogo abre (`assets/`), tudo STORED e com os caminhos
originais. Sai menor do que o APK de entrada e nao deixa copia dobrada.

Roda como hook do NXExtract, dentro do stage e antes do commit:

    tools/make-game-apk.py --stage {stage} --input game.apk --output game.apk \\
                           --keep assets/ --require assets/GJ_GameSheet.plist

O arquivo do usuario em gamedata/ nunca e' tocado.
"""

import argparse
import os
import sys
import zipfile


# Conteudo fixo: o checkpoint do NXExtract confere o sha256 deste texto, entao
# ele nao pode variar por build, por jogo nem por data.
STAMP_TEXT = "nxextract game.apk: assets only, all STORED, v1\n"


def emit_progress(done, total, message=""):
    """Formato NXEXTRACT_PROGRESS: a UI do extrator acompanha o hook."""
    path = os.environ.get("NXEXTRACT_PROGRESS_FILE")
    line = "NXEXTRACT_PROGRESS %d %d %s\n" % (done, total, message)
    if path:
        try:
            with open(path, "a", encoding="utf-8") as handle:
                handle.write(line)
            return
        except OSError:
            pass
    sys.stdout.write(line)
    sys.stdout.flush()


def selected_entries(archive, keep):
    """Membros a manter, em ordem estavel -- o zip sai reproduzivel."""
    chosen = []
    for info in archive.infolist():
        name = info.filename
        if info.is_dir() or name.endswith("/"):
            continue
        if keep and not any(name.startswith(prefix) for prefix in keep):
            continue
        chosen.append(info)
    chosen.sort(key=lambda info: info.filename)
    return chosen


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", required=True)
    parser.add_argument("--input", default="game.apk", help="relativo ao stage")
    parser.add_argument("--output", default="game.apk", help="relativo ao stage")
    parser.add_argument("--keep", action="append", default=[],
                        help="prefixo de caminho a manter (repetivel)")
    parser.add_argument("--require", action="append", default=[],
                        help="caminho que o pacote precisa ter (repetivel)")
    parser.add_argument("--stamp", default="",
                        help="arquivo de conclusao, relativo ao stage")
    args = parser.parse_args()

    source = os.path.join(args.stage, args.input)
    output = os.path.join(args.stage, args.output)
    if not os.path.isfile(source):
        sys.stderr.write("make-game-apk: pacote ausente: %s\n" % source)
        return 1

    try:
        archive = zipfile.ZipFile(source, "r")
    except (OSError, zipfile.BadZipFile) as error:
        sys.stderr.write("make-game-apk: %s nao e' um zip legivel (%s)\n"
                         % (source, error))
        return 1

    with archive:
        entries = selected_entries(archive, args.keep)
        if not entries:
            sys.stderr.write("make-game-apk: nenhum membro casou com --keep\n")
            return 1

        # Faltar conteudo obrigatorio e' recusa COM MOTIVO, nunca um zip mudo
        # que so' aparece como tela preta depois.
        names = {info.filename for info in entries}
        for required in args.require:
            prefix = required.rstrip("/") + "/"
            if required not in names and not any(n.startswith(prefix) for n in names):
                sys.stderr.write(
                    "make-game-apk: o pacote nao tem %s -- nao e' o jogo desta "
                    "receita, ou veio incompleto\n" % required)
                return 1

        tmp = output + ".part"
        total = len(entries)
        emit_progress(0, total, "preparando game.apk")
        # Data fixa: o pacote nao pode mudar de hash so' porque foi remontado
        # noutro dia -- a conferencia do usuario depende disso.
        stamp = (1980, 1, 1, 0, 0, 0)
        try:
            with zipfile.ZipFile(tmp, "w", zipfile.ZIP_STORED,
                                 allowZip64=True) as out:
                for index, info in enumerate(entries, 1):
                    item = zipfile.ZipInfo(info.filename, date_time=stamp)
                    item.compress_type = zipfile.ZIP_STORED
                    item.external_attr = 0o644 << 16
                    with archive.open(info, "r") as handle:
                        out.writestr(item, handle.read())
                    if index % 250 == 0 or index == total:
                        emit_progress(index, total, "preparando game.apk")
        except (OSError, zipfile.BadZipFile) as error:
            try:
                os.unlink(tmp)
            except OSError:
                pass
            sys.stderr.write("make-game-apk: %s\n" % error)
            return 1

    try:
        os.replace(tmp, output)
    except OSError as error:
        sys.stderr.write("make-game-apk: %s\n" % error)
        return 1

    # O carimbo e' o checkpoint do hook, e existe por um motivo medido: o
    # arquivo de saida tem o MESMO nome do de entrada, entao "game.apk existe e
    # tem tamanho plausivel" ja' era verdade antes de normalizar. Com isso, um
    # reinstalar (--force-source) pulava o hook e publicava o APK CRU -- que
    # roda, mas com metade das entradas em DEFLATE, e o leitor de audio do
    # loader so' enxerga STORED. Falha silenciosa, som faltando depois.
    if args.stamp:
        stamp_path = os.path.join(args.stage, args.stamp)
        try:
            with open(stamp_path, "w", encoding="utf-8") as handle:
                handle.write(STAMP_TEXT)
        except OSError as error:
            sys.stderr.write("make-game-apk: %s\n" % error)
            return 1

    size = os.path.getsize(output)
    sys.stdout.write("make-game-apk: %s (%d entradas, %d bytes, STORED)\n"
                     % (output, total, size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
