#!/usr/bin/env python3
"""Gate do registro de identidades de motor do NXExtract.

Havia exatamente UMA versao aceita, cravada em constante, e era isso que tornava
impossivel subir o NXExtract "por opt-in": no instante do bump todo port que
pinava a versao anterior passava a reprovar, entao promover uma versao nova
significava migrar todos de uma vez -- o oposto da regra.

Agora as identidades moram em nxextract-engines-v1.json, com a canonica
marcada. O que este gate protege:

  1. o registro casa com a arvore: a entrada canonica tem os hashes REAIS do
     extrator que esta' no repositorio. Sem isso o registro vira decoracao;
  2. a versao declarada por um pacote precisa ESTAR no registro;
  3. os hashes cobrados sao os DAQUELA versao, nao os da canonica -- aceitar a
     versao antiga com o motor da canonica deixaria passar um motor trocado;
  4. o nxbootstrap e o nxrelease concordam sobre qual e' a canonica; duas
     ferramentas com canonicas diferentes gerariam pacote que a outra recusa.

A entrada nova entra NO MOMENTO do bump, registrando a identidade que sai --
nunca reconstruida depois a partir do que um port por acaso carrega, porque
isso transformaria um motor nao auditado em suportado.
"""
import hashlib
import importlib.util
import json
import sys
from pathlib import Path

TESTS = Path(__file__).resolve().parent
FRAMEWORK = TESTS.parent.parent
REPOSITORY = FRAMEWORK.parent
REGISTRY = TESTS.parent / "nxextract-engines-v1.json"
EXTRACTOR = REPOSITORY / "suportando_outros_devices" / "extrator-universal"

FILES = {
    "engine_sha256": "nxextract.py",
    "runner_sha256": "run-extractor.sh",
    "runtime_env_sha256": "nxextract-runtime-env.sh",
}


def require(condition, message):
    if not condition:
        print("nxextract_engines=FAIL %s" % message, file=sys.stderr)
        raise SystemExit(1)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    require(REGISTRY.is_file() and not REGISTRY.is_symlink(),
            "registro ausente ou inseguro")
    document = json.loads(REGISTRY.read_text(encoding="utf-8"))
    require(document.get("schema") == "org.nextos.nxextract.engine-identities"
            and document.get("schema_version") == 1,
            "cabecalho do registro mudou")
    engines = document.get("engines")
    canonical = document.get("canonical")
    require(isinstance(engines, dict) and engines, "registro sem entradas")
    require(canonical in engines, "canonica ausente do proprio registro")

    canonical_marked = [version for version, entry in engines.items()
                        if entry.get("status") == "canonical"]
    require(canonical_marked == [canonical],
            "exatamente uma entrada deve estar marcada como canonical: %s"
            % canonical_marked)

    for version, entry in sorted(engines.items()):
        require(set(FILES) <= set(entry),
                "entrada %s sem os tres hashes" % version)
        for field in FILES:
            digest = entry[field]
            require(isinstance(digest, str) and len(digest) == 64,
                    "entrada %s tem %s malformado" % (version, field))

    # 1. a entrada canonica casa com a arvore do repositorio
    tree_version = (EXTRACTOR / "VERSION").read_text(encoding="utf-8").strip()
    require(tree_version == canonical,
            "a arvore traz o NXExtract %s, mas a canonica do registro e' %s"
            % (tree_version, canonical))
    for field, filename in sorted(FILES.items()):
        actual = sha256(EXTRACTOR / filename)
        require(engines[canonical][field] == actual,
                "a entrada canonica discorda da arvore em %s (%s)"
                % (field, filename))

    # 2. as duas ferramentas concordam sobre a canonica
    nxrelease = load(TESTS.parent / "nxrelease.py", "nxrelease_engines_probe")
    require(nxrelease.NXEXTRACT_CANONICAL_VERSION == canonical,
            "nxrelease aponta outra canonica: %s"
            % nxrelease.NXEXTRACT_CANONICAL_VERSION)
    require(set(nxrelease.NXEXTRACT_SUPPORTED_VERSIONS) == set(engines),
            "nxrelease e o registro discordam sobre o conjunto suportado")

    generator = load(
        FRAMEWORK / "nxbootstrap" / "tools" / "generate-port.py",
        "nxbootstrap_engines_probe")
    require(generator.NXEXTRACT_VERSION == canonical,
            "nxbootstrap gera pinando %s, mas a canonica e' %s"
            % (generator.NXEXTRACT_VERSION, canonical))
    require(set(generator.NXEXTRACT_SUPPORTED_VERSIONS) <= set(engines),
            "nxbootstrap aceita versao que o registro nao conhece")

    # 3. os hashes cobrados sao os da versao declarada
    require(nxrelease.NXEXTRACT_ENGINE_SHA256 ==
            engines[canonical]["engine_sha256"],
            "nxrelease nao usa o hash da entrada canonica")

    print("nxextract_engines=PASS canonica=%s suportadas=%d" %
          (canonical, len(engines)))


if __name__ == "__main__":
    main()
