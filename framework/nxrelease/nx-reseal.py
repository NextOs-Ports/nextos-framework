#!/usr/bin/env python3
"""nx-reseal -- recarimba TODOS os pins sha256 dos manifestos do framework.

Todo bump de componente (nxbootstrap/nxrelease/nxextract/...) muda arquivos que
os manifestos de evidencia pinam por sha256 (contracts/declarative-v1.json,
nxcompat/m12-audit, nxandroid/m11, nxgl/m13, nxaudio/m14, tests/test-matrix,
tests/run-safe-gates, cross-pins entre manifestos). Este utilitario recomputa e
recarimba esses pins EM CASCATA (ate fixpoint), com um guard que nunca mascara
drift real de conteudo.

Formatos de pin reconhecidos (os dois usados na arvore):
  A) objeto {"path"|"target": "<repo-rel>", "sha256": "<64hex>", ...}
  B) dicionario {"<repo-rel-path>": "<64hex>", ...}   (ex.: evidence_pins do m14)

Guard de seguranca -- um pin so e recarimbado se o ARQUIVO-ALVO for confiavel:
  (a) esta na lista explicita `--changed a,b,c` (mudancas intencionais), OU
  (b) `git diff HEAD -- <alvo>` esta vazio (conteudo committed; pin stale), OU
  (c) o diff-vs-HEAD do alvo contem APENAS linhas com hash de 64 hex
      (o alvo e ele mesmo um manifesto que acabou de ser resselado).
Alvo com mudanca de CONTEUDO fora disso = SUSPEITO: e reportado, NAO tocado, e o
processo termina com erro. Isso garante que o reseal nunca "aprova" um drift.

Uso:
  nx-reseal.py --check                    # gate: falha se existir pin stale
  nx-reseal.py --changed framework/a,framework/b   # ressela apos um bump
  nx-reseal.py --root <repo> ...          # raiz alternativa (testes)

Sai com 0 = tudo carimbado/limpo; 1 = pins stale (--check) ou alvo SUSPEITO.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

HEX64 = re.compile(r"^[0-9a-f]{64}$")
ANY_HEX64 = re.compile(r"[0-9a-f]{64}")
MAX_FIXPOINT_ITERATIONS = 16


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 16), b""):
            digest.update(block)
    return digest.hexdigest()


def git_diff(root, relative):
    return subprocess.run(
        ["git", "-C", root, "diff", "--unified=0", "HEAD", "--", relative],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
    ).stdout


class Resealer:
    def __init__(self, root, changed, check_only):
        self.root = root
        self.changed = set(changed)
        self.check_only = check_only
        self.resealed = set()      # (manifest, alvo)
        self.suspicious = set()    # alvos nao confiaveis com pin stale
        self.stale_seen = set()    # alvos com pin stale (p/ --check)
        self._trust_cache = {}

    # -- confianca no arquivo-alvo -------------------------------------------
    def target_is_trusted(self, relative):
        if relative in self.changed:
            return True
        if relative not in self._trust_cache:
            diff = git_diff(self.root, relative)
            if not diff.strip():
                trusted = True          # unchanged vs HEAD: pin stale legitimo
            else:
                trusted = True
                for line in diff.splitlines():
                    if line[:1] in "+-" and line[:2] not in ("++", "--"):
                        if not ANY_HEX64.search(line[1:]):
                            trusted = False   # mudanca de CONTEUDO real
                            break
            self._trust_cache[relative] = trusted
        return self._trust_cache[relative]

    # -- um passe sobre um documento JSON ------------------------------------
    def walk(self, node):
        changed = False
        if isinstance(node, dict):
            # Formato A cobre {"path"|"target": ..., "sha256": ...} -- o
            # nxrelease.json de port usa "target" para o mesmo contrato.
            path = node.get("path")
            if not isinstance(path, str):
                path = node.get("target")
            digest = node.get("sha256")
            if (isinstance(path, str) and isinstance(digest, str)
                    and HEX64.match(digest)):
                if self.consider(node, "sha256", path, digest):
                    changed = True
            for key, value in list(node.items()):
                if (isinstance(key, str) and isinstance(value, str)
                        and HEX64.match(value) and "/" in key
                        and not key.startswith("http")):
                    if self.consider(node, key, key, value):
                        changed = True
            for value in node.values():
                if self.walk(value):
                    changed = True
        elif isinstance(node, list):
            for item in node:
                if self.walk(item):
                    changed = True
        return changed

    def consider(self, container, slot, relative, pinned):
        absolute = os.path.join(self.root, relative)
        if not os.path.isfile(absolute):
            return False
        actual = sha256_file(absolute)
        if actual == pinned:
            return False
        self.stale_seen.add(relative)
        if not self.target_is_trusted(relative):
            self.suspicious.add(relative)
            return False
        if self.check_only:
            return False
        container[slot] = actual
        self.resealed.add(relative)
        return True

    # -- laco ate fixpoint ----------------------------------------------------
    def run(self):
        framework = os.path.join(self.root, "framework")
        scan_root = framework if os.path.isdir(framework) else self.root
        for _ in range(MAX_FIXPOINT_ITERATIONS):
            self.stale_seen.clear()
            touched = []
            for directory, _, files in os.walk(scan_root):
                if os.sep + ".git" in directory:
                    continue
                for name in files:
                    if not name.endswith(".json"):
                        continue
                    manifest = os.path.join(directory, name)
                    try:
                        with open(manifest, encoding="utf-8") as handle:
                            document = json.load(handle)
                    except (OSError, ValueError):
                        continue
                    if self.walk(document):
                        with open(manifest, "w", encoding="utf-8") as handle:
                            json.dump(document, handle, indent=2,
                                      ensure_ascii=False)
                            handle.write("\n")
                        touched.append(os.path.relpath(manifest, self.root))
            if self.check_only or not touched:
                break
        return self.report()

    def report(self):
        for target in sorted(self.resealed):
            print("resealed: %s" % target)
        for target in sorted(self.suspicious):
            print("SUSPICIOUS (content changed, pin left stale): %s" % target)
        if self.check_only:
            clean = sorted(self.stale_seen - self.suspicious)
            for target in clean:
                print("stale pin: %s" % target)
            status = 0 if not self.stale_seen else 1
            print("nx-reseal --check: %s (stale=%d suspicious=%d)"
                  % ("PASS" if status == 0 else "FAIL",
                     len(self.stale_seen), len(self.suspicious)))
            return status
        status = 0 if not self.suspicious else 1
        print("nx-reseal: %s (resealed=%d suspicious=%d)"
              % ("PASS" if status == 0 else "FAIL",
                 len(self.resealed), len(self.suspicious)))
        return status


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=None,
                        help="raiz do repositorio (default: toplevel do git)")
    parser.add_argument("--changed", default="",
                        help="lista repo-relativa, separada por virgula, dos "
                             "arquivos mudados DE PROPOSITO neste bump")
    parser.add_argument("--check", action="store_true",
                        help="nao escreve nada; falha se houver pin stale")
    arguments = parser.parse_args()

    root = arguments.root
    if root is None:
        root = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            stdout=subprocess.PIPE, text=True, check=True,
        ).stdout.strip()
    root = os.path.abspath(root)

    changed = [item.strip() for item in arguments.changed.split(",")
               if item.strip()]
    resealer = Resealer(root, changed, arguments.check)
    sys.exit(resealer.run())


if __name__ == "__main__":
    main()
