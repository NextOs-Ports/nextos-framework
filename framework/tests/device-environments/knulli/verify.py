#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Gate de VIEWPORT do Knulli, contra o que a imagem oficial declara.

Escopo estreito de propósito: ordem de botões, mapeamento e chord SELECT+START
já têm gate próprio e não são repetidos aqui.

O gate lê do ambiente preparado o que a firmware declara sobre o painel --
placa, autoridade de resolução, geometrias internas, modos HDMI e a
configuração de rotação -- e depois submete esses fatos MEDIDOS ao seletor
puro de resolução do nxgl. Nada inicializa vídeo, define modo ou toca em DRM,
Mali, framebuffer, áudio ou input.
"""

import argparse
import importlib.util
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parents[3]
CONTRACT = HERE / "contract-v1.json"
PROBE = REPOSITORY / "framework/tests/probes/viewport_cases.c"
NXGL_ROOT = REPOSITORY / "framework/nxgl"
FBSET_RE = re.compile(r"fbset\s+-g\s+(\d+)\s+(\d+)")
BOARD_TEST_RE = re.compile(r'\[\s*"\$BOARD"\s*=\s*"([^"]+)"\s*\]')
VIDEO_TEST_RE = re.compile(r'\[\s*"\$VIDEO"\s*=\s*"([^"]*)"\s*\]')


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def load_imagefs():
    spec = importlib.util.spec_from_file_location(
        "imagefs", HERE.parent / "imagefs.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def shell_function(text, name):
    """Corpo de uma função shell, por casamento de chave em coluna zero."""
    opening = "%s() {" % name
    start = text.index(opening) + len(opening)
    end = text.index("\n}", start)
    return text[start:end]


def case_branch(text, label):
    """Corpo de um ramo `label)` de um `case`, até o `;;` que o fecha."""
    pattern = re.compile(r"^\s*%s\)\s*$" % re.escape(label), re.MULTILINE)
    match = pattern.search(text)
    require(match, "case branch %s not found" % label)
    rest = text[match.end():]
    stop = rest.index(";;")
    return rest[:stop]


def branch_geometries(body, test_regex, default_key=None):
    """Liga cada geometria ao ramo if/elif/else que realmente a define.

    Uma varredura solta por `fbset -g` diria que a placa existe sem provar em
    qual ramo ela cai. Aqui a chave vem do teste do ramo, e um `fbset` fora de
    qualquer ramo é falha, não um dado a mais.
    """
    mapping = {}
    keys = None
    for line in body.splitlines():
        stripped = line.strip()
        if stripped.startswith(("if ", "elif ")):
            keys = test_regex.findall(stripped)
            require(keys, "conditional branch without a recognizable test: %s"
                    % stripped)
            continue
        if stripped.startswith("else"):
            require(default_key is not None,
                    "unexpected else branch in this case body")
            keys = [default_key]
            continue
        if stripped.startswith("fi"):
            keys = None
            continue
        found = FBSET_RE.search(stripped)
        if not found:
            continue
        require(keys, "geometry outside any branch: %s" % stripped)
        geometry = (int(found.group(1)), int(found.group(2)))
        for key in keys:
            previous = mapping.get(key)
            require(previous in (None, geometry),
                    "branch %r declares two geometries: %s and %s"
                    % (key, previous, geometry))
            mapping[key] = geometry
    require(mapping, "no geometry was bound to a branch")
    return mapping


def parse_authority(text):
    """Extrai do script da firmware o que ele realmente decide, por ramo."""
    body = shell_function(text, "set_output")
    internal = branch_geometries(case_branch(body, "internal"),
                                 BOARD_TEST_RE, default_key="default")
    hdmi_raw = branch_geometries(case_branch(body, "hdmi"), VIDEO_TEST_RE)
    hdmi = {key: value for key, value in hdmi_raw.items() if key}

    current = case_branch(text, '"currentResolution"')
    require("fbset" in current,
            "currentResolution stopped querying the framebuffer at runtime")
    swapped = None
    straight = None
    keys = None
    for line in current.splitlines():
        stripped = line.strip()
        if stripped.startswith(("if ", "elif ")):
            keys = BOARD_TEST_RE.findall(stripped)
            continue
        if stripped.startswith("else"):
            keys = ["default"]
            continue
        if "awk" not in stripped:
            continue
        require(keys, "report order printed outside any branch: %s" % stripped)
        if '{print $3"x"$2}' in stripped:
            swapped = tuple(keys)
        elif '{print $2"x"$3}' in stripped:
            straight = tuple(keys)
        else:
            raise GateError("unrecognized report order: %s" % stripped)
    require(swapped and straight,
            "currentResolution no longer shows both report orders, so the "
            "swapped case cannot be trusted as documented")
    return internal, hdmi, swapped, straight


def check_provenance(environment, contract, imagefs):
    receipt_path = environment / "environment-receipt.json"
    require(receipt_path.is_file() and not receipt_path.is_symlink(),
            "prepared environment receipt is missing")
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    require(receipt.get("environment_id") == contract["id"],
            "receipt belongs to another environment")
    require(receipt["contract_sha256"] == imagefs.sha256_file(CONTRACT),
            "the contract changed after preparation; prepare again")
    source = receipt["source_image"]
    firmware = contract["firmware"]
    require(source["sha256"] == firmware["image_sha256"]
            and source["size"] == firmware["image_size"],
            "receipt does not describe the official image of the contract")
    require(not any(receipt["evidence"][key] for key in
                    ("hardware_ran", "device_access", "physical_runtime_claim")),
            "the receipt claims hardware evidence it cannot have")
    require(not firmware["stored_in_repository"]
            and not firmware["redistributed_by_framework"],
            "the contract must keep the image out of the repository")
    require("files" in receipt,
            "prepared receipt predates per-file pins; prepare it again")
    return receipt


def check_declared_viewport(declared, contract, parsed):
    """Confere o que o contrato promete contra o que o script realmente liga."""
    viewport = contract["viewport"]
    board_file = declared / viewport["board_file"].lstrip("/")
    require(board_file.is_file() and not board_file.is_symlink(),
            "the board file was not prepared")
    board = board_file.read_text(encoding="utf-8", errors="replace").strip()
    require(board == viewport["board"],
            "image board %r differs from the contract %r"
            % (board, viewport["board"]))

    internal, hdmi, swapped, straight = parsed
    declared_internal = {key: tuple(value) for key, value
                         in viewport["internal_panels"].items()}
    require(internal == declared_internal,
            "internal panel branches in the image are %s, the contract says %s"
            % (internal, declared_internal))
    declared_hdmi = {key: tuple(value) for key, value
                     in viewport["hdmi_modes"].items()}
    for name, geometry in declared_hdmi.items():
        require(hdmi.get(name) == geometry,
                "HDMI branch %s in the image is %s, the contract says %s"
                % (name, hdmi.get(name), geometry))

    exception = viewport["reported_order_exception"]
    require(swapped == (exception["board"],),
            "the swapped report order is bound to %s, not to %s"
            % (list(swapped), exception["board"]))
    require(straight == ("default",),
            "the straight report order is no longer the default branch: %s"
            % list(straight))
    require(board in internal,
            "the board of this image has no branch of its own: %s" % board)

    configuration = declared / "usr/share/batocera/datainit/system/batocera.conf"
    require(configuration.is_file() and not configuration.is_symlink(),
            "the firmware configuration was not prepared")
    conf = configuration.read_text(encoding="utf-8", errors="replace")
    setting = viewport["rotation_setting"]
    require(setting in conf, "the rotation setting vanished from the image")
    for value in viewport["rotation_values"]:
        require(re.search(r"^##\s*%d ->" % value, conf, re.MULTILINE),
                "rotation value %d is no longer documented by the image"
                % value)
    require(re.search(r"^#%s=" % re.escape(setting), conf, re.MULTILINE),
            "the image no longer ships the rotation setting commented")
    active = [line for line in conf.splitlines()
              if re.match(r"^\s*%s\s*=" % re.escape(setting), line)]
    require(not active,
            "the image forces a rotation: %s" % active)
    return board, internal, hdmi


def build_probe(workdir, imagefs):
    compiler = (imagefs.resolve_tool("clang", required=False) or
                imagefs.resolve_tool("cc", required=False) or
                imagefs.resolve_tool("gcc", required=False))
    require(compiler, "host C compiler not found; the gate cannot run blind")
    pkg_config = imagefs.resolve_tool("pkg-config", required=False)
    binary = workdir / "viewport-cases"
    command = [compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
               "-D_POSIX_C_SOURCE=200809L",
               "-I", str(NXGL_ROOT / "include"), "-I", str(NXGL_ROOT / "src"),
               str(NXGL_ROOT / "src/nxgl_logic.c"), str(PROBE),
               "-o", str(binary)]
    if pkg_config:
        flags = imagefs.run_guarded([pkg_config, "--cflags", "sdl2"],
                                    "pkg-config")
        if flags.returncode == 0:
            command[1:1] = flags.stdout.decode().split()
    result = imagefs.run_guarded(command, "clang")
    require(result.returncode == 0,
            "viewport probe failed to build:\n%s"
            % result.stdout.decode("utf-8", "replace"))
    return binary


def run_probe(binary, sources, imagefs):
    result = imagefs.run_guarded(
        [str(binary)] + [str(value) for value in sources], "viewport probe")
    require(result.returncode == 0,
            "viewport probe failed: %s" % result.stdout.decode())
    return result.stdout.decode("utf-8", "replace").strip()


def check_policy(internal, hdmi, imagefs):
    """Alimenta o seletor puro com o que foi lido da imagem, não do contrato."""
    cases = []
    with tempfile.TemporaryDirectory(prefix="knulli-viewport.") as tmp:
        binary = build_probe(Path(tmp), imagefs)
        for name, (width, height) in sorted(internal.items()):
            # O painel interno só é conhecido pelo framebuffer: é o caminho
            # que esta firmware usa ao definir o modo com fbset.
            output = run_probe(binary, (0, 0, 0, 0, width, height), imagefs)
            require(output == "chosen=%dx%d source=fbdev-fact" % (width, height),
                    "%s panel was not taken verbatim: %s" % (name, output))
            cases.append("%s=%dx%d" % (name, width, height))
        for name, (width, height) in sorted(hdmi.items()):
            output = run_probe(binary, (width, height, 0, 0, 640, 480), imagefs)
            require(output == "chosen=%dx%d source=sdl-desktop" % (width, height),
                    "%s was not taken verbatim: %s" % (name, output))
            cases.append("%s=%dx%d" % (name, width, height))
        # Sem nenhuma fonte, a resposta é um erro explícito -- nunca uma
        # resolução inventada.
        output = run_probe(binary, (0, 0, 0, 0, 0, 0), imagefs)
        require(output.startswith("chosen=none"),
                "a resolution was invented with no source: %s" % output)
        cases.append("no-source=refused")
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--environment", required=True)
    arguments = parser.parse_args()
    imagefs = load_imagefs()
    environment = imagefs.environment_argument(arguments.environment)

    contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
    receipt = check_provenance(environment, contract, imagefs)
    declared = environment / imagefs.fixed_layout(receipt, "declared", "declared")
    pinned, aliases = imagefs.verify_records(declared, receipt["files"])

    authority = declared / contract["viewport"]["authority"].lstrip("/")
    require(authority.is_file() and not authority.is_symlink(),
            "the resolution authority was not prepared")
    parsed = parse_authority(
        authority.read_text(encoding="utf-8", errors="replace"))
    board, internal, hdmi = check_declared_viewport(declared, contract, parsed)
    cases = check_policy(internal, hdmi, imagefs)

    print("knulli viewport gate passed: pinned_files=%d board=%s "
          "internal_branches=%d hdmi_branches=%d rotation=not-forced "
          "policy=%s scope=viewport hardware_ran=0 device_access=0"
          % (pinned, board, len(internal), len(hdmi), ",".join(cases)))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except GateError as error:
        print("knulli viewport gate failed: %s" % error, file=sys.stderr)
        sys.exit(1)
    except Exception as error:  # noqa: BLE001 - fronteira do gate
        # Falha de contencao do imagefs (byte trocado, sobra, symlink)
        # chega como excecao propria dele; o gate reporta igual.
        print("knulli viewport gate failed: %s" % error, file=sys.stderr)
        sys.exit(1)
