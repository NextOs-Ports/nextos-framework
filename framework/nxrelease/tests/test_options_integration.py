#!/usr/bin/env python3
"""Gate de integracao das opcoes declarativas entre NXRelease e nxbootstrap.

O defeito que este gate impede: o NXRelease recebe o nxport como JSON PUBLICO e
pedia o launcher canonico direto ao nxbootstrap. Campos internos das `options`
-- `shell_variable` e `shell_suffix` -- so' nascem na normalizacao do proprio
nxbootstrap, entao renderizar sobre o JSON cru estourava com
`KeyError: 'shell_variable'` ANTES de qualquer auditoria do ZIP. E estourava
por fora do `except` da funcao, virando traceback cru em vez de erro fechado.

Um port com `options` era, portanto, impossivel de empacotar -- e o operador
recebia um rastro de pilha em vez de uma frase dizendo o que estava errado.

Os tres casos:

  1. opcao VALIDA -- o launcher canonico e' renderizado e traz a variavel de
     ambiente derivada na normalizacao;
  2. opcao INVALIDA -- vira ReleaseError com mensagem, nunca KeyError,
     TypeError ou traceback;
  3. manifesto que passa na validacao mas quebra o render -- tambem vira
     ReleaseError nomeando a excecao, para o operador saber onde olhar.

O gate nao empacota nada e nao toca em port publicado.
"""
import copy
import importlib.util
import json
import sys
from pathlib import Path

TESTS = Path(__file__).resolve().parent
REPOSITORY = TESTS.parent.parent.parent
NXRELEASE = TESTS.parent / "nxrelease.py"

VALID_OPTION = {
    "id": "quality",
    "label": "Quality",
    "values": ["medium", "high"],
    "default": "medium",
    "environment": "QUALITY",
}


def require(condition, message):
    if not condition:
        print("nxrelease_options=FAIL %s" % message, file=sys.stderr)
        raise SystemExit(1)


def load_nxrelease():
    spec = importlib.util.spec_from_file_location("nxrelease_under_test",
                                                  NXRELEASE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def base_manifest(generator):
    """Manifesto minimo valido, montado aqui de proposito.

    Depender de um nxport.json de port real deixaria o gate refem de qual port
    existe e de qual versao de NXExtract ele pina: um port com pin antigo faria
    este teste reprovar por um motivo que nao e' o dele.
    """
    return {
        "schema_version": 2,
        "id": "gateprobe",
        "title": "Gate Probe",
        "launcher_name": "Gate Probe.sh",
        "architecture": "aarch64",
        "executable": "gateprobe",
        "argument_mode": "none",
        "home_mode": "port",
        "nxextract": {"mode": "yes",
                      "version": generator.NXEXTRACT_VERSION},
        "prepare_script": "",
        "private_library_paths": [],
        "required_capabilities": ["host.portmaster", "graphics.window"],
        "required_files": ["gateprobe", "nxsplash-nextos"],
        "enabled_quirks": [],
        "runtime_report": "log",
    }


def main():
    nxrelease = load_nxrelease()
    version = (TESTS.parent.parent / "nxbootstrap" / "VERSION").read_text(
        encoding="utf-8").strip()
    generator = nxrelease.load_canonical_nxbootstrap_generator(version)

    # 1. opcao valida: renderiza e a variavel derivada aparece no launcher
    manifest = copy.deepcopy(base_manifest(generator))
    manifest["options"] = [copy.deepcopy(VALID_OPTION)]
    launcher = nxrelease.canonical_nxbootstrap_launcher(manifest, version)
    require(isinstance(launcher, str) and launcher,
            "opcao valida nao produziu launcher")
    require("GAME_OPTION_QUALITY" in launcher,
            "a variavel derivada na normalizacao nao chegou ao launcher")

    # o manifesto do chamador nao pode ter sido mutado pela normalizacao
    require("shell_variable" not in manifest["options"][0],
            "a normalizacao vazou campos internos para o manifesto do chamador")

    # 2. opcao invalida: erro fechado, com mensagem, nunca traceback
    broken = copy.deepcopy(base_manifest(generator))
    broken["options"] = [{"id": "quality"}]        # sem values/default/environment
    try:
        nxrelease.canonical_nxbootstrap_launcher(broken, version)
    except nxrelease.ReleaseError as error:
        require("options[0]" in str(error) or "rejected" in str(error),
                "a mensagem nao diz qual opcao esta' errada: %s" % error)
    except Exception as error:  # noqa: BLE001
        require(False, "opcao invalida escapou como %s: %s"
                % (type(error).__name__, error))
    else:
        require(False, "opcao invalida foi aceita")

    # 3. manifesto malformado de outra forma: tambem fechado
    other = copy.deepcopy(base_manifest(generator))
    other["options"] = "nao sou uma lista"
    try:
        nxrelease.canonical_nxbootstrap_launcher(other, version)
    except nxrelease.ReleaseError:
        pass
    except Exception as error:  # noqa: BLE001
        require(False, "options malformado escapou como %s"
                % type(error).__name__)
    else:
        require(False, "options malformado foi aceito")

    # 4. sem options o launcher continua identico ao de antes: o contrato e'
    #    aditivo, e um port publicado nao pode mudar por causa desta correcao.
    plain = copy.deepcopy(base_manifest(generator))
    first = nxrelease.canonical_nxbootstrap_launcher(plain, version)
    second = nxrelease.canonical_nxbootstrap_launcher(
        copy.deepcopy(base_manifest(generator)), version)
    require(first == second, "render sem options nao e' deterministico")
    require("GAME_OPTION_" not in first,
            "um manifesto sem options ganhou variavel de opcao")

    print("nxrelease_options=PASS 4 cenarios (nxbootstrap %s)" % version)


if __name__ == "__main__":
    main()
