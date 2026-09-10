#!/usr/bin/env python3
"""Gate input-action-contract (opt-in).

Ver framework/nxinput/input-contract-v1.md para a forma do arquivo e para o
defeito de campo que motivou isto.

Em uma frase: quando um port traduz controle normalizado em slot interno da
engine, o codigo costuma estar coerente CONSIGO MESMO enquanto discorda da
documentacao e da intencao. Foi assim no Huntdown -- controle chegava como
TRIGGERRIGHT, a documentacao dizia Dash, o adapter mandava para Fire. Ninguem
comparava os tres.

Este gate compara. Um port sem `input-contract.json` nao e' conferido.
"""
import json
import re
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parent.parent.parent
PORTS = REPOSITORY / "ports"

# Vocabulario fechado de controles NORMALIZADOS. Nomes da SDL, nunca rotulo
# fisico impresso na carcaça e nunca nome de aparelho: e' o que impede o
# contrato de virar mapeamento por marca.
CONTROLS = {
    "A", "B", "X", "Y",
    "BACK", "GUIDE", "START",
    "LEFTSTICK", "RIGHTSTICK",
    "LEFTSHOULDER", "RIGHTSHOULDER",
    "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT",
    "TRIGGERLEFT", "TRIGGERRIGHT",
}
GROUPS = {"gameplay", "menu"}
ACTION_RE = re.compile(r"[a-z][a-z0-9-]{0,30}\Z")


def require(condition, message):
    if not condition:
        print("input_action_contract=FAIL %s" % message, file=sys.stderr)
        raise SystemExit(1)


def check_contract(path):
    port = path.parent.name
    document = json.loads(path.read_text(encoding="utf-8"))
    require(document.get("schema") == 1, "%s: schema deve ser 1" % port)
    require(document.get("id") == port,
            "%s: id do contrato nao bate com a pasta" % port)

    adapter_relative = document.get("adapter")
    require(isinstance(adapter_relative, str) and adapter_relative,
            "%s: adapter ausente" % port)
    adapter = path.parent / adapter_relative
    require(adapter.is_file() and not adapter.is_symlink(),
            "%s: adapter ausente ou inseguro: %s" % (port, adapter_relative))

    extraction = document.get("extraction") or {}
    pattern = extraction.get("pattern")
    minimum = extraction.get("minimum_pairs")
    require(isinstance(pattern, str) and pattern,
            "%s: extraction.pattern ausente" % port)
    require(isinstance(minimum, int) and minimum > 0,
            "%s: extraction.minimum_pairs ausente ou invalido" % port)
    compiled = re.compile(pattern)
    require(set(compiled.groupindex) == {"control", "slot"},
            "%s: o padrao precisa dos grupos nomeados control e slot" % port)

    source = adapter.read_text(encoding="utf-8", errors="replace")
    observed = set()
    for match in compiled.finditer(source):
        observed.add((match.group("control"), match.group("slot")))
    # Um padrao que nao casa com nada passaria vazio e o gate viraria enfeite.
    require(len(observed) >= minimum,
            "%s: o padrao extraiu %d pares do adapter, minimo %d"
            % (port, len(observed), minimum))

    groups = document.get("groups")
    require(isinstance(groups, dict) and groups, "%s: groups ausente" % port)
    declared_pairs = 0
    for group_name, actions in sorted(groups.items()):
        context = "%s/%s" % (port, group_name)
        require(group_name in GROUPS,
                "%s: grupo desconhecido (validos: %s)"
                % (context, ", ".join(sorted(GROUPS))))
        require(isinstance(actions, dict) and actions,
                "%s: grupo vazio" % context)
        control_owner = {}
        slot_owner = {}
        for action, binding in sorted(actions.items()):
            require(ACTION_RE.match(action),
                    "%s: nome de acao invalido: %r" % (context, action))
            require(isinstance(binding, dict) and set(binding) ==
                    {"control", "slot"},
                    "%s.%s: binding malformado" % (context, action))
            control = binding["control"]
            slot = binding["slot"]
            require(control in CONTROLS,
                    "%s.%s: controle fora do vocabulario normalizado: %r"
                    % (context, action, control))

            # O caso Huntdown: o MESMO controle servindo a duas acoes dentro do
            # mesmo grupo. Entre grupos e' legitimo -- um botao pode significar
            # coisas diferentes no menu e no gameplay --, dentro nao e'.
            previous = control_owner.get(control)
            require(previous is None,
                    "%s: o controle %s serve a duas acoes no mesmo grupo: "
                    "%s e %s" % (context, control, previous, action))
            control_owner[control] = action

            previous_slot = slot_owner.get(slot)
            require(previous_slot is None,
                    "%s: o slot %s recebe duas acoes no mesmo grupo: %s e %s"
                    % (context, slot, previous_slot, action))
            slot_owner[slot] = action

            # E o passo que impede a documentacao de derivar do codigo.
            require((control, slot) in observed,
                    "%s.%s declara %s -> %s, mas o adapter nao liga esse par"
                    % (context, action, control, slot))
            declared_pairs += 1
    return declared_pairs


def main():
    contracts = sorted(PORTS.glob("*/input-contract.json"))
    total = 0
    for path in contracts:
        require(not path.is_symlink(), "%s e' symlink" % path)
        total += check_contract(path)
    print("input_action_contract=PASS ports=%d bindings=%d"
          % (len(contracts), total))


if __name__ == "__main__":
    main()
