#!/usr/bin/env python3
"""Testes do nx-reseal (E0): recarimba pins stale SEM mascarar drift real.

Cria um repo git temporario com a mesma anatomia de pins da arvore real
(formato A {"path","sha256"}, formato B {path: hash} e cross-pin entre
manifestos) e prova:
  1. arvore verde -> --check PASS (exit 0), nada escrito;
  2. pin stale de alvo unchanged-vs-HEAD -> resselado;
  3. alvo com mudanca de CONTEUDO fora de --changed -> SUSPEITO, pin intocado,
     exit 1;
  4. mesmo alvo com --changed -> resselado;
  5. cross-pin (manifesto A pina manifesto B) converge no fixpoint;
  6. --check numa arvore com stale -> FAIL (exit 1) sem escrever.
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / "nx-reseal.py"


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run_tool(root, *extra):
    return subprocess.run(
        [sys.executable, "-B", str(TOOL), "--root", str(root), *extra],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )


def require(condition, message, output=""):
    if not condition:
        raise SystemExit("nx-reseal test FAIL: %s\n%s" % (message, output))


def git(root, *arguments):
    subprocess.run(["git", "-C", str(root), *arguments], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def build_fixture(root):
    framework = root / "framework"
    (framework / "component").mkdir(parents=True)
    tests_dir = framework / "tests"
    tests_dir.mkdir()

    payload = framework / "component" / "payload.txt"
    payload.write_text("conteudo original\n")
    evidence = framework / "component" / "evidence.txt"
    evidence.write_text("evidencia\n")

    # manifesto B (sera pinado pelo A -> cross-pin)
    manifest_b = framework / "tests" / "inner-audit.json"
    manifest_b.write_text(json.dumps({
        "pins": [{"path": "framework/component/payload.txt",
                  "sha256": sha(payload), "role": "payload"}],
    }, indent=2) + "\n")

    # manifesto A: formato A + formato B + cross-pin do manifesto B
    manifest_a = framework / "tests" / "outer-audit.json"
    manifest_a.write_text(json.dumps({
        "pins": [{"path": "framework/tests/inner-audit.json",
                  "sha256": sha(manifest_b), "role": "manifest"}],
        "evidence_pins": {
            "framework/component/evidence.txt": sha(evidence),
        },
    }, indent=2) + "\n")

    git(root, "init", "-q")
    git(root, "-c", "user.email=t@t", "-c", "user.name=t", "add", "-A")
    git(root, "-c", "user.email=t@t", "-c", "user.name=t",
        "commit", "-q", "-m", "fixture")
    return payload, evidence, manifest_a, manifest_b


def main():
    scratch = tempfile.mkdtemp(prefix="nx-reseal-test.")
    try:
        root = Path(scratch)
        payload, evidence, manifest_a, manifest_b = build_fixture(root)

        # 1. arvore verde -> --check PASS
        result = run_tool(root, "--check")
        require(result.returncode == 0, "check limpo devia passar",
                result.stdout)

        # 2. pin stale de alvo UNCHANGED: simula manifesto committed com pin
        #    errado (drift historico). Editar o pin e commitar; o alvo continua
        #    unchanged -> confiavel -> ressela.
        data = json.loads(manifest_b.read_text())
        data["pins"][0]["sha256"] = "0" * 64
        manifest_b.write_text(json.dumps(data, indent=2) + "\n")
        git(root, "-c", "user.email=t@t", "-c", "user.name=t",
            "add", "-A")
        git(root, "-c", "user.email=t@t", "-c", "user.name=t",
            "commit", "-q", "-m", "pin stale committed")
        result = run_tool(root)
        require(result.returncode == 0, "reseal do pin stale devia passar",
                result.stdout)
        data = json.loads(manifest_b.read_text())
        require(data["pins"][0]["sha256"] == sha(payload),
                "pin stale nao foi recarimbado", result.stdout)

        # o reseal mudou manifest_b -> o cross-pin do manifest_a foi atualizado
        # no MESMO run (fixpoint)
        outer = json.loads(manifest_a.read_text())
        require(outer["pins"][0]["sha256"] == sha(manifest_b),
                "cross-pin nao convergiu no fixpoint", result.stdout)
        git(root, "-c", "user.email=t@t", "-c", "user.name=t", "add", "-A")
        git(root, "-c", "user.email=t@t", "-c", "user.name=t",
            "commit", "-q", "-m", "resealed")

        # 3. mudanca de CONTEUDO real sem --changed -> SUSPEITO + exit 1
        payload.write_text("conteudo NOVO nao autorizado\n")
        result = run_tool(root)
        require(result.returncode == 1, "conteudo mudado devia falhar",
                result.stdout)
        require("SUSPICIOUS" in result.stdout, "faltou o rotulo SUSPICIOUS",
                result.stdout)
        inner = json.loads(manifest_b.read_text())
        require(inner["pins"][0]["sha256"] != sha(payload),
                "pin de alvo suspeito foi tocado", result.stdout)

        # 6. --check tambem enxerga o stale e falha sem escrever
        before = manifest_b.read_text()
        result = run_tool(root, "--check")
        require(result.returncode == 1, "--check devia falhar com stale",
                result.stdout)
        require(manifest_b.read_text() == before, "--check escreveu no disco",
                result.stdout)

        # 4. mesma mudanca declarada em --changed -> ressela
        result = run_tool(root, "--changed",
                          "framework/component/payload.txt")
        require(result.returncode == 0, "--changed devia autorizar",
                result.stdout)
        inner = json.loads(manifest_b.read_text())
        require(inner["pins"][0]["sha256"] == sha(payload),
                "--changed nao recarimbou", result.stdout)

        # 5. formato B ({path: hash}) tambem ressela
        evidence.write_text("evidencia v2\n")
        result = run_tool(root, "--changed",
                          "framework/component/evidence.txt")
        require(result.returncode == 0, "formato B devia resselar",
                result.stdout)
        outer = json.loads(manifest_a.read_text())
        require(outer["evidence_pins"]["framework/component/evidence.txt"]
                == sha(evidence), "formato B nao recarimbado", result.stdout)

        print("nx-reseal tests: PASS cases=6 (check-limpo, stale-reseal, "
              "suspicious-guard, check-stale, changed-authorize, formato-B, "
              "cross-pin-fixpoint)")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    main()
