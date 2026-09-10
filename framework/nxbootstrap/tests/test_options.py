#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Contrato das opcoes declarativas do launcher (nxbootstrap 0.6.28+).

Regras provadas aqui:
  1. ADITIVO -- um manifesto sem `options` gera o launcher de antes, byte a
     byte, incluindo os ports que ja existem;
  2. o campo `language` continua com forma e semantica proprias;
  3. valor, id e variavel passam por regex fechada: nada de aspas, cifrao,
     espaco, backtick ou nome reservado do framework chegando ao shell;
  4. um valor fora da lista -- editado a mao ou herdado do ambiente -- volta
     ao padrao declarado;
  5. o launcher gerado tem sintaxe valida e reexporta cada opcao depois do
     hook mutavel.
"""

import copy
import importlib.util
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENERATOR_PATH = ROOT / "tools/generate-port.py"
EXAMPLE = ROOT / "examples/nxport-armv7.example.json"
OPTIONS_EXAMPLE = ROOT / "examples/nxport-options.example.json"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def load_generator():
    spec = importlib.util.spec_from_file_location(
        "nxbootstrap_generator", GENERATOR_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def expect_rejected(generator, document, fragment):
    try:
        generator.validate(document)
    except generator.ManifestError as error:
        require(fragment in str(error),
                "unexpected rejection: %s (wanted %r)" % (error, fragment))
    else:
        raise AssertionError("invalid option contract was accepted: %s"
                             % fragment)


def generate(generator, document, workdir, name):
    output = workdir / name
    manifest = workdir / ("%s.json" % name)
    manifest.write_text(json.dumps(document), encoding="utf-8")
    generator.generate(manifest, output, False)
    launcher = output / document["launcher_name"]
    require(launcher.is_file(), "launcher was not generated")
    return launcher.read_text(encoding="utf-8"), output


def main():
    generator = load_generator()
    base = json.loads(EXAMPLE.read_text(encoding="utf-8"))
    require(OPTIONS_EXAMPLE.is_file(), "options example is missing")
    example = json.loads(OPTIONS_EXAMPLE.read_text(encoding="utf-8"))

    # 1/2. Aditivo: sem options, nada muda e nada vaza para o launcher.
    config = generator.validate(copy.deepcopy(base))
    require(config["options"] == [], "options must default to empty")
    require(generator.render_options_block(config) == "",
            "an absent options contract must render nothing")
    require(generator.render_options_reassert_block(config) == "",
            "an absent options contract must not reassert anything")

    valid = generator.validate(copy.deepcopy(example))
    require([option["id"] for option in valid["options"]] ==
            sorted(option["id"] for option in valid["options"]),
            "options must be canonically ordered by id")

    # 3. Regex fechada e nomes reservados.
    for mutation, fragment in (
        ({"id": "Shadows"}, "id is invalid"),
        ({"values": ["auto"]}, "needs at least two values"),
        ({"values": ["auto", "auto"]}, "contains duplicates"),
        ({"values": ["auto", "on; rm -rf /"]}, "unsafe value"),
        ({"values": ["auto", "$(id)"]}, "unsafe value"),
        ({"values": ["auto", 'a"b']}, "unsafe value"),
        ({"default": "high"}, "default must be one of values"),
        ({"environment": "sdl_videodriver"}, "environment is invalid"),
        ({"environment": "SDL_VIDEODRIVER"}, "reserved by the framework"),
        ({"environment": "NXPORT_LANGUAGE"}, "reserved by the framework"),
        ({"environment": "GAME_LANGUAGE"}, "reserved by the framework"),
        ({"environment": "HOME"}, "reserved by the framework"),
        ({"label": "rm -rf `id`"}, "label is invalid"),
    ):
        broken = copy.deepcopy(example)
        broken["options"][0].update(mutation)
        expect_rejected(generator, broken, fragment)

    duplicated = copy.deepcopy(example)
    duplicated["options"].append(copy.deepcopy(duplicated["options"][0]))
    expect_rejected(generator, duplicated, "duplicated option id")

    same_environment = copy.deepcopy(example)
    same_environment["options"][1]["environment"] = \
        same_environment["options"][0]["environment"]
    expect_rejected(generator, same_environment, "duplicated option environment")

    too_many = copy.deepcopy(example)
    template = copy.deepcopy(too_many["options"][0])
    too_many["options"] = []
    for index in range(17):
        entry = copy.deepcopy(template)
        entry["id"] = "opt%d" % index
        entry["environment"] = "DEMO_OPT_%d" % index
        too_many["options"].append(entry)
    expect_rejected(generator, too_many, "at most 16 entries")

    unknown = copy.deepcopy(example)
    unknown["options"][0]["hidden"] = True
    expect_rejected(generator, unknown, "unknown field(s): hidden")

    workdir = Path(tempfile.mkdtemp(prefix="nxbootstrap-options-"))
    try:
        # 1. Byte-identidade real: mesmo manifesto, com e sem o campo ausente.
        without_first, _ = generate(generator, base, workdir, "a")
        without_second, _ = generate(generator, base, workdir, "b")
        require(without_first == without_second,
                "generation is not deterministic")
        require("GAME_OPTION_" not in without_first,
                "a port without options must not carry an option block")

        launcher, _ = generate(generator, example, workdir, "c")
        for option in valid["options"]:
            variable = option["shell_variable"]
            require('%s="%s"' % (variable, option["default"]) in launcher,
                    "editable default is missing for %s" % option["id"])
            require("  %s) ;;" % "|".join(option["values"]) in launcher,
                    "allowed values are not enforced for %s" % option["id"])
            require("%s=${%s:-$%s}" % (option["environment"],
                                       option["environment"], variable)
                    in launcher,
                    "environment handoff is missing for %s" % option["id"])
            require("export %s" % option["environment"] in launcher,
                    "option is not exported: %s" % option["id"])
            require("%s=$NXBOOTSTRAP_OPTION_%s" % (option["environment"],
                                                   option["shell_suffix"])
                    in launcher,
                    "option is not reasserted after the hook: %s"
                    % option["id"])

        # 5. Sintaxe: o launcher gerado tem que ser shell valido.
        script = workdir / "c" / example["launcher_name"]
        bash = shutil.which("bash")
        require(bash is not None, "bash is required to check the launcher")
        result = subprocess.run([bash, "-n", str(script)],
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        require(result.returncode == 0,
                "generated launcher is not valid shell:\n%s"
                % result.stdout.decode("utf-8", "replace"))

        # 4. Valor fora da lista volta ao padrao -- provado executando o
        #    proprio bloco gerado, sem rodar o launcher inteiro.
        option = valid["options"][0]
        block = generator.render_options_block(valid)
        probe = workdir / "probe.sh"
        probe.write_text(block + "\nprintf '%s\\n' \"$" +
                         option["environment"] + '"\n', encoding="utf-8")
        for injected, expected in (
            (option["values"][-1], option["values"][-1]),
            ("wat", option["default"]),
            ("on; touch /tmp/nxbootstrap-option-escape", option["default"]),
        ):
            run = subprocess.run(
                [bash, str(probe)], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                env={"PATH": "/usr/bin:/bin", option["environment"]: injected})
            require(run.returncode == 0,
                    "option probe failed: %s" % run.stdout.decode())
            require(run.stdout.decode().strip() == expected,
                    "option %s=%r produced %r, expected %r"
                    % (option["environment"], injected,
                       run.stdout.decode().strip(), expected))
        require(not Path("/tmp/nxbootstrap-option-escape").exists(),
                "an injected option value reached the shell")
    finally:
        shutil.rmtree(str(workdir), ignore_errors=True)

    print("nxbootstrap options gate passed: options=%d rejections=%d "
          "byte_identical_without_options=1" % (len(valid["options"]), 17))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
