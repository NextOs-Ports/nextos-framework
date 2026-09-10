#!/usr/bin/env python3
"""Gate dos hooks de preparacao transacionais (P6).

O executor sempre publicou o STAGE atomicamente, mas as saidas do proprio hook
nao eram atomicas: um preparador que substitui varios arquivos um a um podia
ser interrompido entre alvos e deixar mistura de original+transformado -- e o
retry nao tinha como adivinhar quanto ja foi aplicado.

Com `"transactional": true` o hook escreve TODAS as saidas no shadow workspace
(`NXEXTRACT_HOOK_SHADOW`), o executor valida o conjunto inteiro (inputs
intactos, checkpoint no overlay, SHA por saida), sela um journal e so entao
publica com uma renomeacao atomica por alvo. Interrupcao em qualquer ponto
deixa o stage ou pristino (antes do selo) ou roll-forward deterministico
(depois do selo).

Este gate roda o extrator DE VERDADE e injeta as falhas do item 5 do P6:
SIGKILL depois de cada alvo, ENOSPC simulado e falha de validacao -- o retry
do mesmo container termina no MESMO fingerprint, sem `.nxpart`, sem alvo
extra e sem alterar o payload live publicado anteriormente.
"""
import hashlib
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

INPUT_A = b"input-a-" + b"a" * 56          # 64 bytes
INPUT_B = b"input-b-" + b"b" * 56          # 64 bytes
OUTPUT_A = b"saida-a-" + b"A" * 24         # 32 bytes
OUTPUT_B = b"saida-b-" + b"B" * 24         # 32 bytes
MARKER_BYTES = b'{"prepared": true}\n'      # 19 bytes


def require(condition, message):
    if not condition:
        print("nxextract_hook_transaction=FAIL %s" % message, file=sys.stderr)
        raise SystemExit(1)


def sha256_of(data):
    return hashlib.sha256(data).hexdigest()


def plain_manifest(package):
    return ('<?xml version="1.0" encoding="utf-8"?>'
            '<manifest package="%s"></manifest>' % package).encode("utf-8")


def build_apk(path):
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("assets/a.bin", INPUT_A)
        archive.writestr("assets/b.bin", INPUT_B)
        archive.writestr("AndroidManifest.xml",
                         plain_manifest("org.nextos.txprobe"))


def recipe():
    def rule(name, source_bytes, output_bytes):
        return {
            "id": name,
            "source": {"kind": "entry", "patterns": ["assets/%s.bin" % name]},
            "destination": "data/%s.bin" % name,
            "source_validate": {"type": "file", "size": len(source_bytes),
                                "sha256": sha256_of(source_bytes)},
            "output_validate": {"type": "file", "size": len(output_bytes),
                                "sha256": sha256_of(output_bytes)},
        }
    return {
        "schema": 1, "id": "txprobe", "version": "1",
        "title": "TX PROBE",
        "abi_order": ["arm64-v8a"],
        "input": {"search_dirs": ["gamedata", "."],
                  "prefer_first_nonempty": True,
                  "sniff_all_in_primary": True},
        "extract": [rule("a", INPUT_A, OUTPUT_A),
                    rule("b", INPUT_B, OUTPUT_B)],
        "hooks": [{
            "id": "prepare",
            "transactional": True,
            "argv": ["{game_dir}/prepare.sh"],
            "cwd": "{game_dir}",
            "checkpoint": [
                {"path": "data/a.bin", "type": "file",
                 "size": len(OUTPUT_A), "sha256": sha256_of(OUTPUT_A)},
                {"path": "data/b.bin", "type": "file",
                 "size": len(OUTPUT_B), "sha256": sha256_of(OUTPUT_B)},
                {"path": "data/.prepared.json", "type": "file",
                 "size": len(MARKER_BYTES)},
            ],
        }],
        "commit": ["data"],
        "marker": ".nxextract-txprobe.json",
        "space": {"safety_bytes": 1024},
        "log": "nxextract.log",
    }


# O preparador transacional: le os inputs do stage, escreve as saidas no
# shadow. `KILL_AFTER=<n>` mata o EXTRATOR INTEIRO (SIGKILL, como uma queda de
# energia) depois de n alvos prontos. `ENOSPC_AFTER=<n>` simula disco cheio.
# `BAD_OUTPUT=1` produz um alvo com tamanho errado. `TOUCH_INPUT=1` viola o
# contrato e mexe no stage.
PREPARE_SH = r"""#!/bin/sh
set -eu
shadow="$NXEXTRACT_HOOK_SHADOW"
stage="$NXEXTRACT_STAGE"
control="$NXEXTRACT_GAME_DIR"
mkdir -p "$shadow/data"
done_targets=0
step() {
    done_targets=$((done_targets + 1))
    if [ -f "$control/kill_after" ] && \
       [ "$(cat "$control/kill_after")" = "$done_targets" ]; then
        kill -KILL "$PPID"
        sleep 5
    fi
    if [ -f "$control/enospc_after" ] && \
       [ "$(cat "$control/enospc_after")" = "$done_targets" ]; then
        printf 'half' > "$shadow/data/half.bin.tmp"
        mv "$shadow/data/half.bin.tmp" "$shadow/data/half.bin"
        echo "write $stage/data/half.bin: No space left on device" >&2
        echo "write $stage/data/half.bin: No space left on device"
        exit 1
    fi
}
if [ -f "$control/touch_input" ]; then
    printf 'violado' >> "$stage/data/a.bin"
fi
grep -q 'input-a-' "$stage/data/a.bin"
printf '%s' 'saida-a-AAAAAAAAAAAAAAAAAAAAAAAA' > "$shadow/data/a.bin"
echo "target a done"
step
grep -q 'input-b-' "$stage/data/b.bin"
if [ -f "$control/bad_output" ]; then
    printf '%s' 'curto' > "$shadow/data/b.bin"
else
    printf '%s' 'saida-b-BBBBBBBBBBBBBBBBBBBBBBBB' > "$shadow/data/b.bin"
fi
echo "target b done"
step
printf '%s\n' '{"prepared": true}' > "$shadow/data/.prepared.json"
echo "marker done"
step
exit 0
"""


def make_game(tmp):
    game = Path(tmp)
    (game / "gamedata").mkdir(parents=True, exist_ok=True)
    build_apk(game / "gamedata" / "probe.apk")
    (game / "extractor.json").write_text(
        json.dumps(recipe()), encoding="utf-8")
    hook = game / "prepare.sh"
    hook.write_text(PREPARE_SH, encoding="utf-8")
    hook.chmod(0o755)
    return game


def run_install(game, force_source=False):
    argv = [sys.executable, "-B", str(EXTRACTOR), "install",
            "--recipe", str(game / "extractor.json"),
            "--game-dir", str(game), "--ui", "none"]
    if force_source:
        argv.append("--force-source")
    return subprocess.run(argv, capture_output=True, text=True, timeout=300)


def clear_controls(game):
    for name in ("kill_after", "enospc_after", "bad_output", "touch_input"):
        try:
            (game / name).unlink()
        except FileNotFoundError:
            pass


def workspace_of(game):
    return game / ".nxextract" / "txprobe"


def stage_of(game):
    return workspace_of(game) / "stage"


def hook_fingerprint(game):
    marker = workspace_of(game) / "hooks" / "prepare.json"
    require(marker.is_file(), "marcador do hook transacional nao existe")
    state = json.loads(marker.read_text(encoding="utf-8"))
    require(state.get("transactional") is True,
            "marcador do hook nao registra a transacao")
    value = state.get("fingerprint", "")
    require(isinstance(value, str) and len(value) == 64,
            "marcador do hook sem fingerprint")
    return value


def no_leftovers(root):
    for current, _directories, files in os.walk(root):
        for name in files:
            require(not name.endswith((".nxpart", ".part")),
                    "sobrou temporario %s em %s" % (name, current))


def stage_pristine(game):
    """Depois de falha ANTES do selo, os inputs do stage seguem intactos."""
    stage = stage_of(game)
    a = stage / "data" / "a.bin"
    b = stage / "data" / "b.bin"
    require(a.read_bytes() == INPUT_A and b.read_bytes() == INPUT_B,
            "stage nao ficou pristino depois da interrupcao")
    require(not (stage / "data" / ".prepared.json").exists(),
            "marcador do preparador apareceu num stage nao publicado")
    require(not (stage / "data" / "half.bin").exists(),
            "saida parcial vazou do shadow para o stage")


def tree_bytes(root):
    snapshot = {}
    for current, _directories, files in os.walk(root):
        for name in files:
            path = Path(current) / name
            snapshot[str(path.relative_to(root))] = path.read_bytes()
    return snapshot


def main():
    fingerprints = set()

    # 1. instalacao limpa transacional
    with tempfile.TemporaryDirectory() as tmp:
        game = make_game(tmp)
        result = run_install(game)
        require(result.returncode == 0 and "NXE0000" in
                result.stdout + result.stderr,
                "instalacao transacional limpa falhou:\n%s"
                % (result.stdout + result.stderr)[-600:])
        fingerprints.add(hook_fingerprint(game))
        no_leftovers(game)

    # 2/3. SIGKILL no extrator inteiro depois de CADA alvo; o retry do mesmo
    # container termina no mesmo fingerprint, sem alvo extra.
    for window in ("1", "2"):
        with tempfile.TemporaryDirectory() as tmp:
            game = make_game(tmp)
            (game / "kill_after").write_text(window)
            result = run_install(game)
            require(result.returncode != 0,
                    "SIGKILL injetado depois do alvo %s nao derrubou" % window)
            stage_pristine(game)
            no_leftovers(game)
            clear_controls(game)
            result = run_install(game)
            require(result.returncode == 0 and "NXE0000" in
                    result.stdout + result.stderr,
                    "retry depois de SIGKILL no alvo %s falhou:\n%s"
                    % (window, (result.stdout + result.stderr)[-600:]))
            fingerprints.add(hook_fingerprint(game))
            no_leftovers(game)

    # 4. ENOSPC simulado: hook falha com detalhe; a mensagem chega SANITIZADA
    # (sem caminho absoluto) e o retry fecha no mesmo fingerprint.
    with tempfile.TemporaryDirectory() as tmp:
        game = make_game(tmp)
        (game / "enospc_after").write_text("1")
        result = run_install(game)
        combined = result.stdout + result.stderr
        require(result.returncode != 0, "ENOSPC simulado passou")
        require("last detail" in combined,
                "falha de hook nao levou a ultima linha de detalhe ao resumo")
        for line in combined.splitlines():
            if "last detail" in line:
                require(str(game) not in line,
                        "detalhe do hook vazou caminho absoluto: %s" % line)
        stage_pristine(game)
        clear_controls(game)
        result = run_install(game)
        require(result.returncode == 0,
                "retry depois de ENOSPC falhou:\n%s"
                % (result.stdout + result.stderr)[-600:])
        fingerprints.add(hook_fingerprint(game))

    # 5. falha de validacao: saida com tamanho errado e' recusada ANTES de
    # publicar; o stage segue pristino e o retry correto instala.
    with tempfile.TemporaryDirectory() as tmp:
        game = make_game(tmp)
        (game / "bad_output").write_text("1")
        result = run_install(game)
        require(result.returncode != 0, "saida invalida foi aceita")
        stage_pristine(game)
        clear_controls(game)
        result = run_install(game)
        require(result.returncode == 0,
                "retry depois de validacao reprovada falhou:\n%s"
                % (result.stdout + result.stderr)[-600:])
        fingerprints.add(hook_fingerprint(game))

    # 6. interrupcao DEPOIS do selo do journal. 6a: nada publicado ainda --
    # a proxima execucao publica direto do journal, sem reexecutar o hook.
    # 6b: metade publicada e depois restaurada pela re-extracao -- o journal
    # fica stale, o hook reexecuta sobre inputs pristinos. Nos dois casos o
    # resultado termina no MESMO fingerprint.
    def rebuild_sealed_state(game, publish_half):
        workspace = workspace_of(game)
        stage = stage_of(game)
        (stage / "data").mkdir(parents=True)
        (stage / "data" / "a.bin").write_bytes(INPUT_A)
        (stage / "data" / "b.bin").write_bytes(
            OUTPUT_B if publish_half else INPUT_B)
        shadow = workspace / "hooks" / "prepare.shadow"
        (shadow / "data").mkdir(parents=True)
        (shadow / "data" / "a.bin").write_bytes(OUTPUT_A)
        if not publish_half:
            (shadow / "data" / "b.bin").write_bytes(OUTPUT_B)
        (shadow / "data" / ".prepared.json").write_bytes(MARKER_BYTES)
        hook_marker = workspace / "hooks" / "prepare.json"
        state = json.loads(hook_marker.read_text(encoding="utf-8"))
        hook_marker.unlink()
        outputs = [
            {"path": "data/.prepared.json", "size": len(MARKER_BYTES),
             "sha256": sha256_of(MARKER_BYTES)},
            {"path": "data/a.bin", "size": len(OUTPUT_A),
             "sha256": sha256_of(OUTPUT_A),
             "replaces": {"size": len(INPUT_A), "sha256": sha256_of(INPUT_A)}},
            {"path": "data/b.bin", "size": len(OUTPUT_B),
             "sha256": sha256_of(OUTPUT_B),
             "replaces": {"size": len(INPUT_B), "sha256": sha256_of(INPUT_B)}},
        ]
        journal = {
            "format": 1, "hook": "prepare",
            "recipe_digest": state["recipe_digest"],
            "plan_fingerprint": json.loads(
                (workspace / "state.json").read_text(encoding="utf-8")
            )["plan_fingerprint"],
            "state": "prepared",
            "counters": {"outputs": 3, "replaced": 2,
                         "bytes": sum(o["size"] for o in outputs)},
            "outputs": outputs,
            "fingerprint": state["fingerprint"],
        }
        journal_file = workspace / "hooks" / "prepare.journal.json"
        journal_file.write_text(json.dumps(journal), encoding="utf-8")
        os.chmod(journal_file, 0o600)
        return state["fingerprint"]

    for publish_half, expected_line in (
        (False, "resumed from output journal"),
        (True, "journal is stale"),
    ):
        with tempfile.TemporaryDirectory() as tmp:
            game = make_game(tmp)
            result = run_install(game)
            require(result.returncode == 0,
                    "instalacao base do cenario 6 falhou")
            reference = rebuild_sealed_state(game, publish_half)
            result = run_install(game, force_source=True)
            combined = result.stdout + result.stderr
            require(result.returncode == 0 and "NXE0000" in combined,
                    "retomada da transacao selada falhou (half=%s):\n%s"
                    % (publish_half, combined[-600:]))
            log_text = (game / "nxextract.log").read_text(encoding="utf-8")
            require(expected_line in log_text,
                    "retomada nao passou pelo caminho esperado (%s)"
                    % expected_line)
            require(hook_fingerprint(game) == reference,
                    "retomada terminou em fingerprint diferente")
            fingerprints.add(hook_fingerprint(game))
            no_leftovers(game)

    # 7. o payload LIVE publicado anteriormente nao muda quando um retry
    # posterior morre no meio do hook.
    with tempfile.TemporaryDirectory() as tmp:
        game = make_game(tmp)
        result = run_install(game)
        require(result.returncode == 0, "instalacao base do cenario 7 falhou")
        live = tree_bytes(game / "data")
        (game / "kill_after").write_text("1")
        result = run_install(game, force_source=True)
        require(result.returncode != 0,
                "reinstalacao com SIGKILL deveria falhar")
        require(tree_bytes(game / "data") == live,
                "payload live anterior foi alterado por um retry interrompido")
        clear_controls(game)
        result = run_install(game, force_source=True)
        require(result.returncode == 0,
                "retry final do cenario 7 falhou:\n%s"
                % (result.stdout + result.stderr)[-600:])
        require(tree_bytes(game / "data") == live,
                "payload republicado divergiu do live anterior")
        fingerprints.add(hook_fingerprint(game))

    # 8. hook transacional que mexe nos inputs validados do stage: recusado.
    with tempfile.TemporaryDirectory() as tmp:
        game = make_game(tmp)
        (game / "touch_input").write_text("1")
        result = run_install(game)
        combined = result.stdout + result.stderr
        require(result.returncode != 0 and
                "modified validated stage input" in combined,
                "hook que altera input do stage nao foi recusado:\n%s"
                % combined[-600:])

    require(len(fingerprints) == 1,
            "fingerprints divergiram entre cenarios: %s" % sorted(fingerprints))
    print("nxextract_hook_transaction=PASS 8 cenarios, fingerprint unico")


if __name__ == "__main__":
    main()
