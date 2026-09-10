#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Sela uma bateria inteira num manifesto agregado, conferindo o que ela é.

Cada gate do runner canônico deixa o seu próprio diretório de recibo com um
`MANIFEST.sha256` que cobre **aquele** comando. O manifesto do último comando
não é selo da bateria: ele fixa só o checkpoint. Registrar aquele hash como se
fosse o da regressão inteira promete mais do que ele prova.

Esta ferramenta fecha a lacuna e não acredita no diretório de logs:

- lê os campos exatos de cada recibo e exige `command_status=0`, `tee_status=0`
  e `received_signal=none`;
- valida o conteúdo de cada `MANIFEST.sha256` -- formato, nomes contidos, sem
  caminho absoluto, sem `..`, sem duplicata e sem symlink -- e **recalcula** o
  SHA-256 de cada arquivo que ele fixa, exigindo que o manifesto cubra o
  diretório inteiro;
- confere os **comandos canônicos** do runner, um a um, e não só a quantidade;
- exige o mesmo `git_head` em toda a bateria;
- grava um resumo selado (`RUN-SUMMARY.txt`), que entra no próprio agregado.

Uso:

    python3 framework/tools/seal-run-manifest.py --log-root ABSOLUTO \
      [--runner framework/tests/run-safe-gates.sh]
"""

import argparse
import hashlib
import re
import shlex
import sys
from pathlib import Path, PurePosixPath

MANIFEST_NAME = "RUN-MANIFEST.sha256"
SUMMARY_NAME = "RUN-SUMMARY.txt"
RECEIPT_MANIFEST = "MANIFEST.sha256"
CHUNK = 1 << 20
LINE_RE = re.compile(r"^([0-9a-f]{64})  (.+)$")
REPOSITORY = Path(__file__).resolve().parents[2]
DEFAULT_RUNNER = REPOSITORY / "framework/tests/run-safe-gates.sh"
RUN_METADATA_FIELDS = frozenset({
    "format", "run_id", "started_utc", "cwd", "runner_pid",
    "runner_ppid", "uid", "gid", "argument_count",
    "command_argv_sha256", "command_redacted", "git_root", "git_head",
    "git_branch",
})
RUN_RESULT_FIELDS = frozenset({
    "format", "run_id", "ended_utc", "command_status", "tee_status",
    "received_signal",
})
CHECKPOINT_METADATA_FIELDS = frozenset({
    "format", "checkpoint_id", "created_utc", "repo", "git_head",
    "git_branch", "included_path_count",
})
CHECKPOINT_MANIFEST_ENTRIES = frozenset({
    "metadata.txt", "git-status.txt", "tracked-worktree.patch",
    "tracked-index.patch", "source-files.sha256", "source-snapshot.tar.gz",
})


class SealError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise SealError(message)


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_fields(path, expected_format, expected_fields):
    """Campos `chave=valor` de um recibo com schema fechado."""
    require(path.is_file() and not path.is_symlink(),
            "receipt file is missing or is a symlink: %s" % path)
    fields = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        require(line and "=" in line,
                "unreadable field in %s: %r" % (path, line))
        key, _, value = line.partition("=")
        require(key not in fields, "duplicated field %r in %s" % (key, path))
        fields[key] = value
    require(fields.get("format") == expected_format,
            "%s is not %s" % (path, expected_format))
    missing = expected_fields - fields.keys()
    unexpected = fields.keys() - expected_fields
    require(not missing and not unexpected,
            "%s has the wrong schema (missing=%s unexpected=%s)"
            % (path, ",".join(sorted(missing)) or "none",
               ",".join(sorted(unexpected)) or "none"))
    return fields


def read_checkpoint_metadata(path):
    """Metadata do snapshot: schema fechado com `included_path` repetível."""
    require(path.is_file() and not path.is_symlink(),
            "checkpoint metadata is missing or is a symlink: %s" % path)
    fields = {}
    included_paths = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        require(line and "=" in line,
                "unreadable field in %s: %r" % (path, line))
        key, _, value = line.partition("=")
        if key == "included_path":
            included_paths.append(value)
            continue
        require(key in CHECKPOINT_METADATA_FIELDS,
                "unexpected checkpoint field %r in %s" % (key, path))
        require(key not in fields, "duplicated field %r in %s" % (key, path))
        fields[key] = value

    missing = CHECKPOINT_METADATA_FIELDS - fields.keys()
    require(not missing,
            "%s has the wrong checkpoint schema (missing=%s)"
            % (path, ",".join(sorted(missing))))
    require(fields.get("format") == "nxframework-checkpoint-v1",
            "%s is not nxframework-checkpoint-v1" % path)
    count = fields.get("included_path_count", "")
    require(count.isdigit() and int(count) == len(included_paths) and
            included_paths,
            "%s has an invalid included_path_count" % path)
    require(all(value and not value.startswith("/") and
                ".." not in PurePosixPath(value).parts
                for value in included_paths),
            "%s has an unsafe included_path" % path)
    return fields


def canonical_commands(runner):
    """Comandos que o runner canônico executa, na ordem em que aparecem."""
    require(runner.is_file() and not runner.is_symlink(),
            "runner is missing or is a symlink: %s" % runner)
    text = runner.read_text(encoding="utf-8")
    # Junta continuações de linha antes de separar as invocações.
    joined = text.replace("\\\n", " ")
    commands = {}
    for line in joined.splitlines():
        stripped = line.strip()
        # run_gate_external has the same invocation shape and only differs in
        # tolerating exit 77 for a gate that needs an owner-supplied artifact.
        # A sealer blind to it would treat that gate's receipt as unexpected.
        if not (stripped.startswith("run_gate ") or
                stripped.startswith("run_gate_external ")):
            continue
        tokens = shlex.split(stripped)
        require(len(tokens) >= 3, "unreadable run_gate line: %s" % stripped)
        gate_id = tokens[1]
        argv = tokens[2:]
        require(gate_id not in commands, "duplicated gate id: %s" % gate_id)
        commands[gate_id] = argv
    require(commands, "the runner declares no gate")
    return commands


def normalized_command(argv, log_root):
    """Compara comandos por argv, neutralizando só o caminho do log root.

    O runner escreve o destino como `$LOG_ROOT/...`; o recibo guarda o caminho
    absoluto já expandido. Os dois viram o mesmo marcador para que a
    comparação continue sendo entre comandos, não entre diretórios.
    """
    root = str(log_root)
    normalized = []
    for token in argv:
        if token == "$LOG_ROOT" or token == root:
            token = "<LOG-ROOT>"
        elif token.startswith("$LOG_ROOT/"):
            token = "<LOG-ROOT>" + token[len("$LOG_ROOT"):]
        elif token.startswith(root + "/"):
            token = "<LOG-ROOT>" + token[len(root):]
        normalized.append(token)
    return tuple(normalized)


def verify_receipt_manifest(directory, expected_entries=None):
    """Valida e recalcula o manifesto de um recibo, cobrindo o diretório."""
    manifest = directory / RECEIPT_MANIFEST
    require(manifest.is_file() and not manifest.is_symlink(),
            "receipt without a manifest: %s" % directory.name)
    covered = set()
    for line in manifest.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        match = LINE_RE.match(line)
        require(match, "unreadable manifest line in %s: %r"
                % (directory.name, line))
        digest, name = match.group(1), match.group(2)
        require(not name.startswith("/"),
                "absolute path in %s manifest: %r" % (directory.name, name))
        parts = PurePosixPath(name).parts
        require(".." not in parts and name not in (".", ""),
                "escaping path in %s manifest: %r" % (directory.name, name))
        require(name not in covered,
                "duplicated entry in %s manifest: %r" % (directory.name, name))
        covered.add(name)
        target = directory / name
        require(target.is_file() and not target.is_symlink(),
                "manifest entry is missing or is a symlink: %s/%s"
                % (directory.name, name))
        require(sha256_file(target) == digest,
                "manifest entry does not match its bytes: %s/%s"
                % (directory.name, name))

    present = set()
    for path in directory.rglob("*"):
        if path.is_dir() and not path.is_symlink():
            continue
        require(not path.is_symlink(),
                "symlink inside a receipt: %s" % path)
        present.add(path.relative_to(directory).as_posix())
    missing = present - covered - {RECEIPT_MANIFEST}
    require(not missing,
            "files not covered by the %s manifest: %s"
            % (directory.name, ", ".join(sorted(missing))))
    if expected_entries is not None:
        missing = expected_entries - covered
        unexpected = covered - expected_entries
        require(not missing and not unexpected,
                "%s manifest has the wrong schema (missing=%s unexpected=%s)"
                % (directory.name, ",".join(sorted(missing)) or "none",
                   ",".join(sorted(unexpected)) or "none"))
    return manifest


def collect(log_root, commands):
    receipts = {}
    heads = set()
    for entry in sorted(log_root.iterdir()):
        if entry.name in (MANIFEST_NAME, SUMMARY_NAME):
            continue
        require(entry.is_dir() and not entry.is_symlink(),
                "unexpected entry in the log root: %s" % entry.name)
        if entry.name == "checkpoints":
            continue
        result = read_fields(entry / "result.txt",
                             "nxframework-run-result-v1",
                             RUN_RESULT_FIELDS)
        require(result.get("command_status") == "0",
                "receipt %s did not finish with command_status=0" % entry.name)
        require(result.get("tee_status") == "0",
                "receipt %s did not finish with tee_status=0" % entry.name)
        require(result.get("received_signal") == "none",
                "receipt %s recorded a signal: %s"
                % (entry.name, result.get("received_signal")))
        metadata = read_fields(entry / "metadata.txt",
                               "nxframework-run-log-v1",
                               RUN_METADATA_FIELDS)
        require(metadata.get("run_id") == entry.name and
                result.get("run_id") == entry.name,
                "receipt %s has inconsistent run_id fields" % entry.name)
        head = metadata.get("git_head")
        require(head, "receipt %s has no git_head" % entry.name)
        heads.add(head)
        argv = shlex.split(metadata.get("command_redacted", ""))
        require(argv, "receipt %s has no command" % entry.name)
        verify_receipt_manifest(entry)
        receipts[entry.name] = normalized_command(argv, log_root)

    require(len(heads) == 1,
            "the run mixes different commits: %s" % ", ".join(sorted(heads)))

    expected = {gate: normalized_command(argv, log_root)
                for gate, argv in commands.items()}
    matched = {}
    unexpected = []
    for name, argv in receipts.items():
        found = [gate for gate, candidate in expected.items()
                 if candidate == argv]
        if not found:
            unexpected.append(" ".join(argv))
            continue
        gate = found[0]
        require(gate not in matched,
                "gate %s ran more than once in this log root" % gate)
        matched[gate] = name
    require(not unexpected,
            "commands that are not canonical gates: %s"
            % "; ".join(sorted(unexpected)))
    missing = sorted(set(expected) - set(matched))
    require(not missing,
            "canonical gates without a receipt: %s" % ", ".join(missing))

    run_head = next(iter(heads))
    checkpoints = []
    checkpoint_root = log_root / "checkpoints"
    require(checkpoint_root.is_dir() and not checkpoint_root.is_symlink(),
            "no green checkpoint directory; a sealed run must include it")
    for entry in sorted(checkpoint_root.iterdir()):
        require(entry.is_dir() and not entry.is_symlink(),
                "unexpected entry under checkpoints: %s" % entry.name)
        manifest = verify_receipt_manifest(
            entry, expected_entries=CHECKPOINT_MANIFEST_ENTRIES)
        metadata = read_checkpoint_metadata(entry / "metadata.txt")
        require(metadata.get("checkpoint_id") == entry.name,
                "checkpoint %s has an inconsistent checkpoint_id" % entry.name)
        require(metadata.get("git_head") == run_head,
                "checkpoint %s git_head does not match the gate receipts"
                % entry.name)
        checkpoints.append(manifest)
    require(checkpoints, "no green checkpoint found")
    return matched, run_head, checkpoints


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log-root", required=True,
                        help="absolute log root of one regression run")
    parser.add_argument("--runner", default=str(DEFAULT_RUNNER),
                        help="canonical runner whose gates must be present")
    arguments = parser.parse_args()

    log_root = Path(arguments.log_root)
    require(log_root.is_absolute(), "--log-root must be an absolute path")
    require(not log_root.is_symlink(), "--log-root is a symlink")
    log_root = log_root.resolve(strict=True)
    require(log_root.is_dir(), "log root is missing: %s" % log_root)
    output = log_root / MANIFEST_NAME
    summary_path = log_root / SUMMARY_NAME
    for path in (output, summary_path):
        require(not path.exists() and not path.is_symlink(),
                "seal artifact already exists: %s" % path)

    commands = canonical_commands(Path(arguments.runner).resolve(strict=True))
    matched, head, checkpoints = collect(log_root, commands)

    summary = [
        "format=nxframework-run-seal-v1",
        "git_head=%s" % head,
        "gate_count=%d" % len(matched),
        "checkpoint_count=%d" % len(checkpoints),
        "result=ALL PASS count=%d" % len(matched),
        "hardware_ran=0",
        "device_access=0",
    ]
    summary += ["gate=%s receipt=%s" % (gate, matched[gate])
                for gate in sorted(matched)]
    summary_path.write_text("\n".join(summary) + "\n", encoding="utf-8")

    lines = []
    for gate in sorted(matched):
        manifest = log_root / matched[gate] / RECEIPT_MANIFEST
        lines.append("%s  %s" % (sha256_file(manifest),
                                 manifest.relative_to(log_root).as_posix()))
    for manifest in checkpoints:
        lines.append("%s  %s" % (sha256_file(manifest),
                                 manifest.relative_to(log_root).as_posix()))
    lines.append("%s  %s" % (sha256_file(summary_path), SUMMARY_NAME))
    output.write_text("\n".join(sorted(lines)) + "\n", encoding="utf-8")
    digest = sha256_file(output)

    print("run manifest sealed: result=ALL PASS count=%d checkpoints=%d "
          "git_head=%s file=%s sha256=%s"
          % (len(matched), len(checkpoints), head[:12], output, digest))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SealError as error:
        print("run manifest seal failed: %s" % error, file=sys.stderr)
        sys.exit(1)
