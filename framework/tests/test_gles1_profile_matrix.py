#!/usr/bin/env python3
"""Gate gles1-profile-matrix.

Antes deste gate, um port GLES1 so' descobria que nao rodava numa familia de
aparelho quando alguem tentava no aparelho. A matriz nao tinha como reprovar
antes do campo porque nenhum perfil dizia QUAL E' A FORMA do provedor GLES1 do
firmware -- e essa forma e' justamente o que decide se o port abre.

As formas, medidas ou nao:

  unified-blob             um objeto so' traz EGL e GLES1, e os SONAMEs
                           apontam todos para ele
  crossed-soname           o nome VERSIONADO resolve para uma biblioteca sem
                           driver e o blob real fica atras do nome sem versao;
                           quem se liga ao versionado ganha um contexto que
                           aceita tudo e nao desenha nada
  mesa-frontend-dependent  quem responde e' a Mesa, cujo front-end GLES1 e'
                           opcao de compilacao: a imagem pode nao expor nada
  absent                   nao ha GLES1 nenhum
  unknown                  ninguem mediu

E o nivel de evidencia e' declarado separado da forma, porque "achamos que e'
um blob" e "medimos que e' um blob" nao valem a mesma coisa.

O que este gate cobra:

  1. todo perfil declara a forma e a evidencia, dentro de um vocabulario
     fechado;
  2. onde a forma e' `crossed-soname`, o reparo de provedor TEM de existir no
     framework -- sem ele nenhum port GLES1 roda ali, e declarar a forma sem
     ter a resposta seria so' documentar a derrota;
  3. um port que declara `graphics.gles1` so' pode listar em
     `documentation.proven_support` perfis que existem, cuja forma nao seja
     `absent`, e cuja evidencia seja `measured-on-device`. Nao se afirma prova
     sobre aparelho que ninguem mediu.

O item 3 e' o que impede a regressao mais provavel daqui pra frente: alguem
copiar `proven_support` de um port para outro e a documentacao publica passar a
prometer aparelho onde ninguem entrou.
"""
import json
import sys
from pathlib import Path

TEST_ROOT = Path(__file__).resolve().parent
REPOSITORY = TEST_ROOT.parent.parent
PROFILES_PATH = TEST_ROOT / "firmware-profiles-v2.json"
REPAIR_ADAPTER = (REPOSITORY / "framework" / "nxgl" / "adapters" /
                  "nxgl_provider_discovery_adapter.c")

SHAPES = {
    "unified-blob",
    "crossed-soname",
    "mesa-frontend-dependent",
    "absent",
    "unknown",
}
EVIDENCE = {"measured-on-device", "inferred-from-family", "not-measured"}
GLES1_CAPABILITY = "graphics.gles1"


def require(condition, message):
    if not condition:
        print("gles1_profile_matrix=FAIL %s" % message, file=sys.stderr)
        raise SystemExit(1)


def read_json(path):
    require(path.is_file() and not path.is_symlink(),
            "arquivo ausente ou inseguro: %s" % path)
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def check_profiles(document):
    profiles = document.get("profiles")
    require(isinstance(profiles, list) and profiles, "perfis ausentes")
    shapes = {}
    crossed = []
    for profile in profiles:
        profile_id = profile.get("id")
        block = profile.get("graphics_provider")
        require(isinstance(block, dict) and set(block) ==
                {"gles1_shape", "gles1_evidence", "note"},
                "graphics_provider malformado no perfil %r" % profile_id)
        shape = block["gles1_shape"]
        evidence = block["gles1_evidence"]
        require(shape in SHAPES,
                "forma desconhecida no perfil %r: %r" % (profile_id, shape))
        require(evidence in EVIDENCE,
                "evidencia desconhecida no perfil %r: %r"
                % (profile_id, evidence))
        require(isinstance(block["note"], str) and block["note"].strip(),
                "o perfil %r declara a forma sem dizer por que" % profile_id)
        # Uma forma medida sem medicao, ou uma medicao sem forma, seria
        # contradicao: unknown existe justamente para o caso nao medido.
        require((shape == "unknown") == (evidence == "not-measured"),
                "perfil %r: forma e evidencia se contradizem (%s / %s)"
                % (profile_id, shape, evidence))
        shapes[profile_id] = (shape, evidence)
        if shape == "crossed-soname":
            crossed.append(profile_id)
    return shapes, crossed


def check_repair_exists(crossed):
    if not crossed:
        return
    require(REPAIR_ADAPTER.is_file() and not REPAIR_ADAPTER.is_symlink(),
            "perfis com SONAME cruzado (%s) mas o reparo de provedor nao esta' "
            "no framework" % ", ".join(sorted(crossed)))


def check_port_claims(shapes):
    claims = 0
    for manifest in sorted((REPOSITORY / "ports").glob("*/nxproject.json")):
        port = manifest.parent.name
        document = json.loads(manifest.read_text(encoding="utf-8"))
        nxport = document.get("nxport") or {}
        capabilities = nxport.get("required_capabilities") or []
        if GLES1_CAPABILITY not in capabilities:
            continue
        proven = (document.get("documentation") or {}).get("proven_support")
        require(isinstance(proven, list),
                "%s: documentation.proven_support ausente" % port)
        for profile_id in proven:
            require(profile_id in shapes,
                    "%s afirma prova no perfil inexistente %r"
                    % (port, profile_id))
            shape, evidence = shapes[profile_id]
            require(shape != "absent",
                    "%s afirma prova em %r, onde nao ha GLES1"
                    % (port, profile_id))
            require(evidence == "measured-on-device",
                    "%s afirma prova em %r, mas a forma do provedor la' nunca "
                    "foi medida (%s)" % (port, profile_id, evidence))
            claims += 1
    return claims


def main():
    document = read_json(PROFILES_PATH)
    shapes, crossed = check_profiles(document)
    check_repair_exists(crossed)
    claims = check_port_claims(shapes)
    measured = sum(1 for _, evidence in shapes.values()
                   if evidence == "measured-on-device")
    print("gles1_profile_matrix=PASS perfis=%d medidos=%d cruzados=%d "
          "afirmacoes_de_port=%d"
          % (len(shapes), measured, len(crossed), claims))


if __name__ == "__main__":
    main()
