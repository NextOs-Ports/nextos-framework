#!/usr/bin/env python3
"""nx-add-capability -- acrescenta uma capability ao registry, inteira.

Por que existe: uma capability nova toca NOVE arquivos, e o gate reclama de um
por vez. Fazer isso a mao custa uma sequencia longa de tentativa e erro, e um
dos passos falha do pior jeito possivel -- esquecer a tabela de nomes de
`test_nxcompat_registry.c` faz o teste SEGFALTAR, porque o laco passa a ler um
elemento alem do fim de um array fixo, em vez de dar mensagem.

Os nove lugares:

  1. framework/nxcompat/capabilities-v1.json
  2. framework/nxcompat/include/nxcompat.h        enum de id + COUNT
  3. framework/nxcompat/src/nxcompat_registry.c   a tabela C
  4. framework/nxcompat/tests/test_nxcompat_registry.c   tabela de nomes
  5. framework/nxcompat/tests/test_m12_audit.py   lista, contagem,
                                                  CAPABILITY_ENUMS e range(N)
  6. framework/nxbootstrap/tests/test-manifest-contract.py  total e contagem
                                                  por namespace
  7. framework/nxbootstrap/schema/nxport-v2.schema.json     o enum, NA ORDEM
  8. framework/nxbootstrap/nxbootstrap.sh         allowlist do shell, NA ORDEM
  9. framework/nxbootstrap/tests/test-068-preservation.py   o pin sha256 do
                                                  arquivo do passo 8

E, no fim, `nx-reseal.py --changed` com a lista COMPLETA -- ele marca como
SUSPICIOUS todo arquivo que mudou e nao foi declarado, entao a lista pela
metade da FAIL sem reselar nada.

DUAS REGRAS que a ferramenta embute, e que so' se aprendem apanhando:

  * a capability nova entra no FIM da lista, nunca perto das parecidas. O
    registry C usa ids numericos que ja' viajaram em recibos gravados, e o gate
    exige espelho posicional entre o JSON e o C: inserir no meio renumeraria
    todas as entradas seguintes e quebraria a leitura daqueles recibos;
  * a allowlist do shell e o enum do schema comparam por ORDEM, nao por
    conjunto -- entao os dois recebem o nome novo no fim, como o registry.

Uso:

  framework/tools/nx-add-capability.py --id graphics.gles1 \\
      --phase graphics --source nxgl --evidence opened \\
      --role baseline-graphics

  --dry-run mostra o que mudaria sem escrever nada.
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

TOUCHED = [
    "framework/nxcompat/capabilities-v1.json",
    "framework/nxcompat/include/nxcompat.h",
    "framework/nxcompat/src/nxcompat_registry.c",
    "framework/nxcompat/tests/test_nxcompat_registry.c",
    "framework/nxcompat/tests/test_m12_audit.py",
    "framework/nxbootstrap/tests/test-manifest-contract.py",
    "framework/nxbootstrap/schema/nxport-v2.schema.json",
    "framework/nxbootstrap/nxbootstrap.sh",
    "framework/nxbootstrap/tests/test-068-preservation.py",
]


def die(message):
    print("nx-add-capability: %s" % message, file=sys.stderr)
    raise SystemExit(1)


def enum_suffix(identifier):
    return identifier.replace(".", "_").replace("-", "_").upper()


def read(root, relative):
    return (root / relative).read_text(encoding="utf-8")


def write(root, relative, text, dry_run, changed):
    if dry_run:
        print("  [dry-run] escreveria %s" % relative)
    else:
        (root / relative).write_text(text, encoding="utf-8")
        print("  atualizado %s" % relative)
    changed.append(relative)


def main():
    parser = argparse.ArgumentParser(prog="nx-add-capability")
    parser.add_argument("--root", default=None,
                        help="raiz do repositorio (default: toplevel do git)")
    parser.add_argument("--id", required=True)
    parser.add_argument("--phase", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--evidence", required=True)
    parser.add_argument("--role", required=True)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--no-reseal", action="store_true",
                        help="nao chamar nx-reseal no fim")
    args = parser.parse_args()

    if args.root:
        root = Path(args.root).resolve()
    else:
        root = Path(subprocess.run(["git", "rev-parse", "--show-toplevel"],
                                   capture_output=True, text=True,
                                   check=True).stdout.strip())

    registry_path = "framework/nxcompat/capabilities-v1.json"
    registry = json.loads(read(root, registry_path))
    identifiers = [item["id"] for item in registry["capabilities"]]

    if args.id in identifiers:
        die("a capability %s ja' existe" % args.id)
    namespace = args.id.split(".")[0]
    if namespace not in ("host", "graphics", "audio", "input"):
        die("namespace desconhecido: %s" % namespace)
    for field, vocabulary in (("phase", "phases"), ("source", "sources"),
                              ("evidence", "states"), ("role", "roles")):
        value = getattr(args, field)
        if value not in registry[vocabulary]:
            die("%s invalido: %s (validos: %s)" %
                (field, value, ", ".join(registry[vocabulary])))
    if args.evidence in ("absent", "lost"):
        die("minimum_evidence nao pode ser absent nem lost")

    changed = []
    count = len(identifiers) + 1
    new_ids = identifiers + [args.id]
    namespace_counts = {prefix: sum(i.startswith(prefix + ".") for i in new_ids)
                        for prefix in ("host", "graphics", "audio", "input")}
    suffix = enum_suffix(args.id)

    print("nx-add-capability: %s (id numerico %d, no FIM da lista)" %
          (args.id, len(identifiers)))

    # 1. registry JSON -------------------------------------------------------
    registry["capabilities"].append({
        "id": args.id, "phase": args.phase, "source": args.source,
        "minimum_evidence": args.evidence, "role": args.role})
    write(root, registry_path,
          json.dumps(registry, indent=2, ensure_ascii=False) + "\n",
          args.dry_run, changed)

    # 2. nxcompat.h: enum de id e COUNT --------------------------------------
    relative = "framework/nxcompat/include/nxcompat.h"
    text = read(root, relative)
    last_enum = re.search(
        r"  NXCOMPAT_CAPABILITY_([A-Z0-9_]+) = (\d+)\n\} nxcompat_capability_id_v1;",
        text)
    if last_enum is None:
        die("nao achei o fim do enum nxcompat_capability_id_v1")
    text = text.replace(
        last_enum.group(0),
        "  NXCOMPAT_CAPABILITY_%s = %s,\n"
        "  /* Anexada no fim de proposito: os ids acima ja' viajaram em recibos\n"
        "     gravados, e renumerar quebraria a leitura deles. */\n"
        "  NXCOMPAT_CAPABILITY_%s = %d\n} nxcompat_capability_id_v1;"
        % (last_enum.group(1), last_enum.group(2), suffix, len(identifiers)), 1)
    text = re.sub(r"#define NXCOMPAT_CAPABILITY_COUNT \d+u",
                  "#define NXCOMPAT_CAPABILITY_COUNT %du" % count, text, count=1)
    write(root, relative, text, args.dry_run, changed)

    # 3. registry C ----------------------------------------------------------
    relative = "framework/nxcompat/src/nxcompat_registry.c"
    text = read(root, relative)
    marker = "}};"
    index = text.rindex(marker, 0, text.index("typedef char"))
    text = (text[:index] + "},\n"
            "    {%du, \"%s\", NXCOMPAT_PHASE_%s, NXCOMPAT_SOURCE_%s,\n"
            "     NXCOMPAT_EVIDENCE_%s, NXCOMPAT_ROLE_%s}};"
            % (len(identifiers), args.id, args.phase.upper(),
               args.source.replace("-", "_").upper(), args.evidence.upper(),
               args.role.replace("-", "_").upper())
            + text[index + len(marker):])
    write(root, relative, text, args.dry_run, changed)

    # 4. tabela de nomes do teste do registry (esquecer aqui = SEGFAULT) -----
    relative = "framework/nxcompat/tests/test_nxcompat_registry.c"
    text = read(root, relative)
    last_name = '      "%s"};' % identifiers[-1]
    if last_name not in text:
        die("nao achei o fim da tabela de nomes em %s" % relative)
    text = text.replace(last_name,
                        '      "%s",\n      "%s"};' % (identifiers[-1], args.id),
                        1)
    write(root, relative, text, args.dry_run, changed)

    # 5. auditoria m12 -------------------------------------------------------
    relative = "framework/nxcompat/tests/test_m12_audit.py"
    text = read(root, relative)
    tuples = re.findall(r'    \("%s".*?\)\,\n' % re.escape(identifiers[-1]), text)
    if not tuples:
        die("nao achei a tupla da ultima capability em %s" % relative)
    text = text.replace(
        tuples[-1],
        tuples[-1] + '    ("%s", "%s", "%s", "%s", "%s"),\n'
        % (args.id, args.phase, args.source, args.evidence, args.role), 1)
    text = re.sub(r"len\(capabilities\) == \d+",
                  "len(capabilities) == %d" % count, text, count=1)
    text = re.sub(r"tuple\(range\(\d+\)\)", "tuple(range(%d))" % count, text,
                  count=1)
    text = re.sub(r'#define NXCOMPAT_CAPABILITY_COUNT \d+u" in header',
                  '#define NXCOMPAT_CAPABILITY_COUNT %du" in header' % count,
                  text, count=1)
    enums = re.search(r"CAPABILITY_ENUMS = \((.*?)\)\n", text, re.S)
    if enums is None:
        die("nao achei CAPABILITY_ENUMS em %s" % relative)
    body = enums.group(1).rstrip()
    if not body.endswith(","):
        body += ","
    text = text[:enums.start(1)] + body + '\n    "%s",\n' % suffix + \
        text[enums.end(1):]
    write(root, relative, text, args.dry_run, changed)

    # 6. contrato do manifesto: total e contagem por namespace ---------------
    relative = "framework/nxbootstrap/tests/test-manifest-contract.py"
    text = read(root, relative)
    text = re.sub(r"len\(identifiers\) == \d+ == len\(set\(identifiers\)\)",
                  "len(identifiers) == %d == len(set(identifiers))" % count,
                  text, count=1)
    text = re.sub(r"must contain \d+ unique identifiers",
                  "must contain %d unique identifiers" % count, text, count=1)
    for prefix, total in namespace_counts.items():
        text = re.sub(r'"%s": \d+' % prefix, '"%s": %d' % (prefix, total),
                      text, count=1)
    write(root, relative, text, args.dry_run, changed)

    # 7. enum do schema, NA ORDEM do registry --------------------------------
    relative = "framework/nxbootstrap/schema/nxport-v2.schema.json"
    schema = json.loads(read(root, relative))
    schema["properties"]["required_capabilities"]["items"]["enum"] = new_ids
    write(root, relative,
          json.dumps(schema, indent=2, ensure_ascii=False) + "\n",
          args.dry_run, changed)

    # 8. allowlist do shell, NA ORDEM ---------------------------------------
    relative = "framework/nxbootstrap/nxbootstrap.sh"
    text = read(root, relative)
    tail = "%s)" % identifiers[-1]
    if tail not in text:
        die("nao achei o fim da allowlist em %s" % relative)
    text = text.replace(tail, "%s|\\\n    %s)" % (identifiers[-1], args.id), 1)
    write(root, relative, text, args.dry_run, changed)

    # 9. pin sha256 do arquivo do passo 8 ------------------------------------
    relative = "framework/nxbootstrap/tests/test-068-preservation.py"
    if not args.dry_run:
        digest = hashlib.sha256(
            (root / "framework/nxbootstrap/nxbootstrap.sh").read_bytes()
        ).hexdigest()
        text = read(root, relative)
        current = re.search(r'"([0-9a-f]{64})"', text)
        if current is None:
            die("nao achei o pin em %s" % relative)
        write(root, relative, text.replace(current.group(1), digest, 1),
              False, changed)
    else:
        print("  [dry-run] reselaria o pin em %s" % relative)
        changed.append(relative)

    if args.dry_run:
        print("nx-add-capability: dry-run, nada foi escrito")
        return

    if args.no_reseal:
        print("nx-add-capability: reseal NAO executado (--no-reseal). "
              "Rode nx-reseal.py --changed com a lista completa.")
        return

    print("nx-add-capability: reselando pins em cascata")
    result = subprocess.run(
        [sys.executable, str(root / "framework/nxrelease/nx-reseal.py"),
         "--root", str(root), "--changed", ",".join(changed)],
        capture_output=True, text=True)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode != 0:
        die("nx-reseal falhou")
    print("nx-add-capability: OK -- %s adicionada em %d arquivos"
          % (args.id, len(changed)))


if __name__ == "__main__":
    main()
