#!/usr/bin/env python3
"""Gate da validacao separada por fase: source_validate / output_validate.

Ate' a 1.2.14 a MESMA regra de `validate` era cobrada duas vezes: no payload
recem-extraido e de novo no resultado, depois dos hooks. Um hook legitimo que
muda o tamanho dos arquivos -- recomprimir textura, reduzir audio -- nao tinha
como declarar os dois estados, entao a receita precisava afrouxar para um
intervalo que coubesse ambos.

O custo disso nao e' estetico. Com o intervalo frouxo, um stage interrompido NO
MEIO da transformacao -- parte original, parte convertida -- cai dentro da
faixa e passa. Era justamente o estado que se queria recusar.

Com as fases separadas cada lado declara valor EXATO, e o estado intermediario
nao satisfaz nenhum dos dois.

Este gate roda o extrator de verdade, com um hook que muda o tamanho.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

TESTS = Path(__file__).resolve().parent
EXTRACTOR = TESTS.parent / "nxextract.py"

SOURCE_BYTES = b"origem-" + b"o" * 57          # 64 bytes
OUTPUT_BYTES = b"convertido-" + b"c" * 21      # 32 bytes


def require(condition, message):
    if not condition:
        print("nxextract_phase_validation=FAIL %s" % message, file=sys.stderr)
        raise SystemExit(1)


def sha256_of(data):
    import hashlib
    return hashlib.sha256(data).hexdigest()


def plain_manifest(package):
    """Mesmo manifesto de texto que a suite do extrator ja' usa nos APK falsos."""
    return ('<?xml version="1.0" encoding="utf-8"?>'
            '<manifest package="%s"></manifest>' % package).encode("utf-8")


def build_apk(path):
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("assets/payload.bin", SOURCE_BYTES)
        archive.writestr("AndroidManifest.xml",
                         plain_manifest("org.nextos.phaseprobe"))


def recipe(with_phases):
    rule = {
        "id": "payload",
        "source": {"kind": "entry", "patterns": ["assets/payload.bin"]},
        "destination": "data/payload.bin",
    }
    if with_phases:
        rule["source_validate"] = {"type": "file", "size": len(SOURCE_BYTES),
                                   "sha256": sha256_of(SOURCE_BYTES)}
        rule["output_validate"] = {"type": "file", "size": len(OUTPUT_BYTES),
                                   "sha256": sha256_of(OUTPUT_BYTES)}
    else:
        # o que a receita precisava fazer antes: uma faixa que cabe os dois
        rule["validate"] = {"type": "file", "min_size": len(OUTPUT_BYTES),
                            "max_size": len(SOURCE_BYTES)}
    return {
        "schema": 1, "id": "phaseprobe", "version": "1",
        "title": "PHASE PROBE",
        "abi_order": ["arm64-v8a"],
        "input": {"search_dirs": ["gamedata", "."],
                  "prefer_first_nonempty": True,
                  "sniff_all_in_primary": True},
        "extract": [rule],
        "hooks": [{
            "id": "convert",
            "argv": ["{game_dir}/convert.sh", "{stage}/data/payload.bin"],
            "cwd": "{game_dir}",
        }],
        "commit": ["data"],
        "marker": ".nxextract-phaseprobe.json",
        "space": {"safety_bytes": 1024},
        "log": "nxextract.log",
    }


def run_case(tmp, with_phases, hook_body):
    game = Path(tmp)
    (game / "gamedata").mkdir(parents=True, exist_ok=True)
    build_apk(game / "gamedata" / "probe.apk")
    (game / "extractor.json").write_text(
        json.dumps(recipe(with_phases)), encoding="utf-8")
    hook = game / "convert.sh"
    hook.write_text(hook_body, encoding="utf-8")
    hook.chmod(0o755)
    result = subprocess.run(
        [sys.executable, "-B", str(EXTRACTOR), "install",
         "--recipe", str(game / "extractor.json"),
         "--game-dir", str(game), "--ui", "none"],
        capture_output=True, text=True, timeout=300)
    return result


CONVERT_OK = "#!/bin/sh\nprintf '%s' 'convertido-ccccccccccccccccccccc' > \"$1\"\n"
CONVERT_PARTIAL = "#!/bin/sh\nprintf '%s' 'origem-parcialmente-mexido-xxxxx' > \"$1\"\n"


def main():
    # 1. hook que muda o tamanho, com valores EXATOS dos dois lados: instala.
    with tempfile.TemporaryDirectory() as tmp:
        result = run_case(tmp, True, CONVERT_OK)
        require(result.returncode == 0,
                "fases separadas com valores exatos deveriam instalar:\n%s"
                % (result.stdout + result.stderr)[-500:])
        require("NXE0000" in result.stdout + result.stderr,
                "instalacao nao chegou ao marcador terminal")

    # 2. o mesmo hook deixando um estado INTERMEDIARIO: recusado, porque nao
    #    satisfaz nem a origem nem a saida.
    with tempfile.TemporaryDirectory() as tmp:
        result = run_case(tmp, True, CONVERT_PARTIAL)
        require(result.returncode != 0,
                "estado intermediario passou com as fases separadas")

    # 3. e com a regra unica frouxa -- o que a receita PRECISAVA fazer antes --
    #    o mesmo estado intermediario passa. E' a perda que motivou o item.
    with tempfile.TemporaryDirectory() as tmp:
        result = run_case(tmp, False, CONVERT_PARTIAL)
        require(result.returncode == 0,
                "a faixa frouxa deveria aceitar o intermediario (era o problema)")

    # 4. receita antiga, sem os campos novos e sem hook que muda tamanho:
    #    continua funcionando igual. O contrato e' aditivo.
    with tempfile.TemporaryDirectory() as tmp:
        result = run_case(tmp, False, "#!/bin/sh\nexit 0\n")
        require(result.returncode == 0,
                "receita antiga parou de funcionar:\n%s"
                % (result.stdout + result.stderr)[-400:])

    print("nxextract_phase_validation=PASS 4 cenarios")


if __name__ == "__main__":
    main()
