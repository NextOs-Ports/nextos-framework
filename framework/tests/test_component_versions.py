#!/usr/bin/env python3
"""Gate component-versions: cada componente do framework fala UMA versão só.

Motivo do defeito de campo (auditoria V3, ponto 5): o `nxinput` chegou a
carregar TRÊS versões ao mesmo tempo — CHANGELOG `0.5.0`, `project(nxinput
VERSION 0.4.4)` no CMake e `#define NXINPUT_VERSION "0.3.1"` no header — o
`nxaudio` ficou com header `0.2.0` enquanto o VERSION/CMake/changelog já eram
`0.3.0`, e o `nxbootstrap` teve o VERSION bumpado para `0.6.31` sem a entrada
correspondente no CHANGELOG (topo parou em `0.6.30`). Ninguém comparava as
fontes, então header, pacote e changelog discordavam em silêncio.

Este gate compara, para cada `framework/nx*/`, as fontes de verdade que
existirem:

  * VERSION   — arquivo `VERSION` (âncora quando existe)
  * CMake     — `project(<nome> VERSION x.y.z ...)`
  * Header    — `#define <NOME>_VERSION "x.y.z"` (NÃO o `_API_VERSION`, que é a
                ABI numérica e vive separada de propósito)
  * CHANGELOG — a primeira linha `# x.y.z` OU `## x.y.z`

Regra: um componente é conferido quando tem VERSION file OU CMake (uma âncora
citável). As fontes que existirem têm de ser IDÊNTICAS. Um componente com só
CHANGELOG (ex.: nxextract, versionado por outro esquema) não tem âncora
cruzável e fica de fora. Falha fechada.
"""
import re
import sys
from pathlib import Path

FRAMEWORK_ROOT = Path(__file__).resolve().parents[1]
_SEMVER = r"[0-9]+(?:\.[0-9]+)+"


def require(condition, message):
    if not condition:
        print("component_versions=FAIL " + message)
        sys.exit(1)


def version_file(component_dir):
    path = component_dir / "VERSION"
    if not path.is_file():
        return None
    text = path.read_text(encoding="utf-8", errors="replace").strip()
    m = re.match(r"^(%s)$" % _SEMVER, text)
    return m.group(1) if m else None


def cmake_version(component_dir, name):
    path = component_dir / "CMakeLists.txt"
    if not path.is_file():
        return None
    m = re.search(r"project\(\s*%s\s+VERSION\s+(%s)" % (re.escape(name), _SEMVER),
                  path.read_text(encoding="utf-8", errors="replace"))
    return m.group(1) if m else None


def header_version(component_dir, name):
    macro = name.upper() + "_VERSION"
    pattern = re.compile(r'#\s*define\s+%s\s+"(%s)"' % (re.escape(macro), _SEMVER))
    include_dir = component_dir / "include"
    if not include_dir.is_dir():
        return None
    for header in sorted(include_dir.glob("*.h")):
        m = pattern.search(header.read_text(encoding="utf-8", errors="replace"))
        if m:
            return m.group(1)
    return None


def changelog_version(component_dir):
    changelog = component_dir / "CHANGELOG.md"
    if not changelog.is_file():
        return None
    for line in changelog.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"#{1,2}\s+(%s)" % _SEMVER, line.strip())
        if m:
            return m.group(1)
    return None


def main():
    components = sorted(p for p in FRAMEWORK_ROOT.glob("nx*") if p.is_dir())
    require(components, "nenhum componente framework/nx* encontrado")
    checked = 0
    skipped = []
    for component in components:
        name = component.name
        require(not component.is_symlink(), "%s é symlink" % name)
        sources = {}
        for key, value in (
            ("VERSION", version_file(component)),
            ("cmake", cmake_version(component, name)),
            ("header", header_version(component, name)),
            ("changelog", changelog_version(component)),
        ):
            if value is not None:
                sources[key] = value
        # Sem âncora cruzável (VERSION ou CMake): fora do gate.
        if "VERSION" not in sources and "cmake" not in sources:
            skipped.append(name)
            continue
        distinct = set(sources.values())
        require(len(distinct) == 1,
                "%s versões divergentes: %s"
                % (name, ", ".join("%s=%s" % kv for kv in sorted(sources.items()))))
        checked += 1
    print("component_versions=PASS componentes=%d fora=%d (%s)"
          % (checked, len(skipped), ",".join(skipped) or "-"))


if __name__ == "__main__":
    main()
