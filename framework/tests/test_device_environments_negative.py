#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Gate negativo permanente do imagefs e do selador de bateria.

As proteções destes dois componentes só valem se alguém provar que elas
REPROVAM. Os casos abaixo já foram verificados à mão uma vez; aqui eles viram
gate reproduzível, sem imagem oficial, sem rede e sem aparelho: tudo é montado
em diretório temporário próprio.
"""

import ast
import hashlib
import importlib.util
import json
import os
import shlex
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import warnings
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
IMAGEFS = ROOT / "framework/tests/device-environments/imagefs.py"
SEALER = ROOT / "framework/tools/seal-run-manifest.py"
ENVIRONMENTS = ROOT / "framework/tests/device-environments"
EXTERNAL_TOOLS = {
    "7z", "cc", "clang", "debugfs", "gcc", "mcopy", "mdir", "mtype",
    "pkg-config", "qemu-aarch64", "qemu-aarch64-static",
    "qemu-arm-static", "unsquashfs",
}


def load_imagefs():
    spec = importlib.util.spec_from_file_location("imagefs", IMAGEFS)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


imagefs = load_imagefs()
failures = []


def check(label, condition):
    if not condition:
        failures.append(label)


def rejects(label, action, fragment=None):
    """A ação TEM de falhar; opcionalmente com a mensagem esperada."""
    try:
        action()
    except imagefs.ImageError as error:
        if fragment and fragment not in str(error):
            failures.append("%s: wrong message %r" % (label, error))
        return
    except Exception as error:  # noqa: BLE001 - o gate mede a recusa
        failures.append("%s: unexpected error %r" % (label, error))
        return
    failures.append("%s: was accepted" % label)


def elf(path, elf_class=2, machine=183):
    header = bytearray(64)
    header[:7] = b"\x7fELF" + bytes((elf_class, 1, 1))
    struct.pack_into("<H", header, 18, machine)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(header))


def zip_with(members, path):
    with zipfile.ZipFile(str(path), "w") as archive:
        for name, data, mode in members:
            info = zipfile.ZipInfo(name)
            info.create_system = 3
            info.external_attr = mode << 16
            archive.writestr(info, data)


def imagefs_cases(workdir):
    # 1. Contenção: caminho lógico não pode escapar do ambiente preparado.
    root = workdir / "prepared"
    root.mkdir()
    rejects("logical path with ..",
            lambda: imagefs.safe_logical_path(root, "/usr/../../etc/passwd"),
            "non-canonical")
    rejects("relative logical path",
            lambda: imagefs.safe_logical_path(root, "usr/lib"), "unsafe")
    rejects("logical path with newline",
            lambda: imagefs.safe_logical_path(root, "/usr/li\nb"), "unsafe")

    # 2. Repositório: imagem e saída ficam fora dele.
    rejects("output inside the repository",
            lambda: imagefs.outside_repository(ROOT / "framework", "output"),
            "outside the repository")
    check("outside path accepted",
          imagefs.outside_repository(workdir, "output") == workdir.resolve())

    # 3. --environment: symlink é recusado ANTES de resolver.
    link = workdir / "environment-link"
    link.symlink_to(root)
    rejects("environment symlink", lambda: imagefs.environment_argument(link),
            "symlink")
    rejects("environment inside the repository",
            lambda: imagefs.environment_argument(ROOT / "framework"),
            "outside the repository")
    rejects("relative environment",
            lambda: imagefs.environment_argument("framework"), "absolute")

    # 4. Layout fixo: o recibo não escolhe o diretório lido.
    rejects("layout from the receipt",
            lambda: imagefs.fixed_layout({"layout": {"root": "../elsewhere"}},
                                         "root", "root"),
            "this environment only reads")

    # 5. ZIP hostil.
    hostile = workdir / "hostile"
    hostile.mkdir()
    absolute = hostile / "absolute.zip"
    zip_with([("/etc/passwd", b"x", 0o644)], absolute)
    rejects("zip with absolute member",
            lambda: imagefs.ZipReader(absolute), "absolute member")
    escaping = hostile / "escaping.zip"
    zip_with([("../outside.txt", b"x", 0o644)], escaping)
    rejects("zip escaping the root",
            lambda: imagefs.ZipReader(escaping), "escapes the archive root")
    symlinked = hostile / "symlink.zip"
    zip_with([("lib/libc.so.6", b"/etc/passwd", stat.S_IFLNK | 0o777)],
             symlinked)
    rejects("zip with symlink member",
            lambda: imagefs.ZipReader(symlinked), "symlink member")
    duplicated = hostile / "duplicated.zip"
    with warnings.catch_warnings():
        # O ZIP duplicado é o ponto do teste; o aviso do zipfile é ruído.
        warnings.simplefilter("ignore", UserWarning)
        with zipfile.ZipFile(str(duplicated), "w") as archive:
            archive.writestr("lib/a.so", b"one")
            archive.writestr("lib/a.so", b"two")
    rejects("zip with duplicated member",
            lambda: imagefs.ZipReader(duplicated), "duplicated member")

    # 6. Recibo forte: alterado, ausente, extra e symlink.
    tree = workdir / "tree"
    elf(tree / "usr/lib/libgood.so.1")
    record = imagefs.file_record(tree / "usr/lib/libgood.so.1",
                                 "/usr/lib/libgood.so.1", "aarch64")
    check("record pins bytes",
          record["sha256"] == hashlib.sha256(
              (tree / "usr/lib/libgood.so.1").read_bytes()).hexdigest())
    check("clean tree verifies",
          imagefs.verify_records(tree, [record])[0] == 1)

    (tree / "usr/lib/libgood.so.1").write_bytes(b"\x7fELF" + b"\x00" * 60 + b"x")
    rejects("changed bytes", lambda: imagefs.verify_records(tree, [record]),
            "changed size")
    elf(tree / "usr/lib/libgood.so.1")

    extra = tree / "usr/lib/extra.so"
    elf(extra)
    rejects("undeclared file", lambda: imagefs.verify_records(tree, [record]),
            "undeclared file")
    extra.unlink()

    (tree / "usr/lib/libgood.so.1").unlink()
    rejects("missing file", lambda: imagefs.verify_records(tree, [record]),
            "missing or became a symlink")
    # Symlink para fora: a contenção pega antes mesmo de olhar os bytes.
    (tree / "usr/lib/libgood.so.1").symlink_to("/etc/hostname")
    rejects("file replaced by a symlink out of the tree",
            lambda: imagefs.verify_records(tree, [record]),
            "escaped the prepared root")
    # Symlink para dentro: continua sendo recusado, agora pelo registro.
    (tree / "usr/lib/libgood.so.1").unlink()
    elf(tree / "usr/lib/libtarget.so.1")
    (tree / "usr/lib/libgood.so.1").symlink_to("libtarget.so.1")
    rejects("file replaced by a symlink inside the tree",
            lambda: imagefs.verify_records(tree, [record]),
            "missing or became a symlink")

    # 7. ABI errada no registro.
    other = workdir / "other"
    elf(other / "usr/lib/libarm.so.1", elf_class=1, machine=40)
    rejects("wrong ABI record",
            lambda: imagefs.file_record(other / "usr/lib/libarm.so.1",
                                        "/usr/lib/libarm.so.1", "aarch64"),
            "wrong ELF identity")


class FakeReader:
    """Leitor mínimo para exercitar o fechamento sem imagem."""

    def __init__(self, files):
        self.files = files

    def exists(self, path):
        return path in self.files

    def resolve(self, path):
        return path

    def read(self, path, destination):
        imagefs.require(path in self.files, "missing: %s" % path)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(self.files[path])


def dynamic_elf(needed=(), soname=None, machine=183):
    """ELF64 mínimo com PT_LOAD + PT_DYNAMIC e uma strtab real."""
    strings = b"\x00"
    offsets = {}
    for name in list(needed) + ([soname] if soname else []):
        offsets[name] = len(strings)
        strings += name.encode() + b"\x00"
    entries = [(1, offsets[name]) for name in needed]
    if soname:
        entries.append((14, offsets[soname]))
    entries.append((5, 0x1000))  # DT_STRTAB (endereço virtual)
    entries.append((0, 0))
    dynamic = b"".join(struct.pack("<qQ", tag, value) for tag, value in entries)

    header = bytearray(64)
    header[:7] = b"\x7fELF" + bytes((2, 1, 1))
    struct.pack_into("<H", header, 16, 3)
    struct.pack_into("<H", header, 18, machine)
    struct.pack_into("<Q", header, 32, 64)      # e_phoff
    struct.pack_into("<H", header, 54, 56)      # e_phentsize
    struct.pack_into("<H", header, 56, 2)       # e_phnum
    program = bytearray(112)
    struct.pack_into("<I", program, 0, 1)       # PT_LOAD
    struct.pack_into("<Q", program, 8, 0x200)   # p_offset
    struct.pack_into("<Q", program, 16, 0x1000)  # p_vaddr
    struct.pack_into("<Q", program, 32, len(strings))
    struct.pack_into("<I", program, 56, 2)      # PT_DYNAMIC
    struct.pack_into("<Q", program, 56 + 8, 0x400)
    struct.pack_into("<Q", program, 56 + 32, len(dynamic))
    body = bytearray(0x600)
    body[0:64] = header
    body[64:64 + 112] = program
    body[0x200:0x200 + len(strings)] = strings
    body[0x400:0x400 + len(dynamic)] = dynamic
    return bytes(body)


def closure_cases(workdir):
    root = workdir / "closure"
    reader = FakeReader({
        "/usr/lib/libmain.so": dynamic_elf(needed=("libmissing.so.1",)),
    })
    rejects("unresolved DT_NEEDED",
            lambda: imagefs.extract_closure(
                reader, ["/usr/lib/libmain.so"], ["/usr/lib"], root,
                "aarch64"),
            "unresolved DT_NEEDED")

    complete = workdir / "closure-ok"
    reader = FakeReader({
        "/usr/lib/libmain.so": dynamic_elf(needed=("libdep.so.1",),
                                           soname="libmain.so.9"),
        "/usr/lib/libdep.so.1": dynamic_elf(soname="libdep.so.1"),
    })
    records, aliases = imagefs.extract_closure(
        reader, ["/usr/lib/libmain.so"], ["/usr/lib"], complete, "aarch64")
    check("closure pulled the dependency", len(records) == 2)
    check("alias published for the soname",
          aliases == ["/usr/lib/libmain.so.9"])
    check("alias verifies",
          imagefs.verify_records(
              complete, records, aliases,
              complete_closures={"aarch64": ["/usr/lib"]}) == (2, 1))

    alias = complete / "usr/lib/libmain.so.9"
    alias.unlink()
    alias.symlink_to("libdep.so.1")
    rejects("alias redirected to another pinned library",
            lambda: imagefs.verify_records(
                complete, records, aliases,
                complete_closures={"aarch64": ["/usr/lib"]}),
            "declared owner")
    alias.unlink()
    alias.symlink_to("libmain.so")

    main_record = next(record for record in records
                       if record["path"] == "/usr/lib/libmain.so")
    (complete / "usr/lib/libdep.so.1").unlink()
    check("selective inventory may omit a DT_NEEDED provider",
          imagefs.verify_records(
              complete, [main_record], aliases) == (1, 1))
    rejects("dependency removed from a complete closure",
            lambda: imagefs.verify_records(
                complete, [main_record], aliases,
                complete_closures={"aarch64": ["/usr/lib"]}),
            "unresolved DT_NEEDED")

    wrong_abi = workdir / "closure-wrong-abi"
    (wrong_abi / "usr/bin").mkdir(parents=True)
    (wrong_abi / "usr/lib").mkdir(parents=True)
    (wrong_abi / "usr/bin/main").write_bytes(
        dynamic_elf(needed=("libdep.so.1",)))
    (wrong_abi / "usr/lib/libdep.so.1").write_bytes(
        dynamic_elf(machine=62))
    main = imagefs.file_record(
        wrong_abi / "usr/bin/main", "/usr/bin/main", "aarch64")
    foreign = imagefs.file_record(
        wrong_abi / "usr/lib/libdep.so.1", "/usr/lib/libdep.so.1",
        "x86_64")
    rejects("DT_NEEDED provider from another ABI",
            lambda: imagefs.verify_records(
                wrong_abi, [main, foreign], complete_closures={
                    "aarch64": ["/usr/lib"]}),
            "unresolved DT_NEEDED")


def tool_path_cases(workdir):
    """PATH herdado não escolhe ferramentas e argv externo nunca é relativo."""
    marker = workdir / "contaminated-tool-ran"
    wrapper = workdir / "true"
    wrapper.write_text(
        "#!/bin/sh\nprintf contaminated > '%s'\nexit 91\n" % marker,
        encoding="utf-8")
    wrapper.chmod(wrapper.stat().st_mode | stat.S_IXUSR)
    previous = os.environ.get("PATH")
    try:
        os.environ["PATH"] = "%s:%s" % (workdir, previous or "")
        trusted = shutil.which("true", path=os.defpath)
        check("trusted PATH provides true", trusted is not None)
        if trusted is not None:
            expected = str(Path(trusted).resolve(strict=True))
            check("resolve_tool ignores inherited PATH",
                  imagefs.resolve_tool("true") == expected)
            check("clean subprocess PATH is fixed",
                  imagefs.clean_subprocess_env().get("PATH") == os.defpath)
            check("trusted absolute executable runs",
                  imagefs.run_guarded([expected], "true").returncode == 0)
        rejects("bare executable", lambda: imagefs.run_guarded(["true"],
                                                                 "true"),
                "absolute path")
        check("contaminated wrapper was not executed", not marker.exists())
    finally:
        if previous is None:
            os.environ.pop("PATH", None)
        else:
            os.environ["PATH"] = previous

    # Protege os módulos standalone: novos which() precisam declarar o path,
    # e nenhum subprocess.run() pode reintroduzir ferramenta conhecida nua.
    for path in sorted(ENVIRONMENTS.rglob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            if (isinstance(node.func, ast.Attribute) and
                    node.func.attr == "which"):
                check("%s:%d which has a trusted path" %
                      (path.relative_to(ROOT), node.lineno),
                      any(keyword.arg == "path" for keyword in node.keywords))
            if not (isinstance(node.func, ast.Attribute) and
                    node.func.attr == "run" and node.args):
                continue
            argv = node.args[0]
            if not isinstance(argv, (ast.List, ast.Tuple)) or not argv.elts:
                continue
            executable = argv.elts[0]
            check("%s:%d external executable is not bare" %
                  (path.relative_to(ROOT), node.lineno),
                  not (isinstance(executable, ast.Constant) and
                       executable.value in EXTERNAL_TOOLS))


RUN_RECEIPT_FILES = (
    "metadata.txt", "console.log", "result.txt", "input-files.sha256",
    "command-status.txt",
)
CHECKPOINT_FILES = (
    "metadata.txt", "git-status.txt", "tracked-worktree.patch",
    "tracked-index.patch", "source-files.sha256", "source-snapshot.tar.gz",
)


def write_manifest(directory, names):
    lines = []
    for name in names:
        digest = hashlib.sha256((directory / name).read_bytes()).hexdigest()
        lines.append("%s  %s" % (digest, name))
    (directory / "MANIFEST.sha256").write_text("\n".join(lines) + "\n",
                                               encoding="utf-8")


def write_receipt(directory, command, git_head, status="0", tee="0",
                  signal="none"):
    directory.mkdir(parents=True)
    argv = shlex.split(command)
    argv_digest = hashlib.sha256(
        b"".join(argument.encode("utf-8") + b"\0" for argument in argv)
    ).hexdigest()
    (directory / "metadata.txt").write_text(
        "format=nxframework-run-log-v1\nrun_id=%s\n"
        "started_utc=2026-01-01T00:00:00.000000000Z\ncwd=%s\n"
        "runner_pid=100\nrunner_ppid=99\nuid=1000\ngid=1000\n"
        "argument_count=%d\ncommand_argv_sha256=%s\n"
        "command_redacted= %s\ngit_root=%s\ngit_head=%s\n"
        "git_branch=framework/tests-1.1.3\n"
        % (directory.name, ROOT, len(argv), argv_digest, command, ROOT,
           git_head), encoding="utf-8")
    (directory / "result.txt").write_text(
        "format=nxframework-run-result-v1\nrun_id=%s\n"
        "ended_utc=2026-01-01T00:00:01.000000000Z\ncommand_status=%s\n"
        "tee_status=%s\nreceived_signal=%s\n"
        % (directory.name, status, tee, signal), encoding="utf-8")
    (directory / "console.log").write_text("fixture\n", encoding="utf-8")
    (directory / "input-files.sha256").write_text("", encoding="utf-8")
    (directory / "command-status.txt").write_text(status + "\n",
                                                   encoding="utf-8")
    write_manifest(directory, RUN_RECEIPT_FILES)


def write_checkpoint(directory, git_head):
    directory.mkdir(parents=True)
    included = ("framework", "suportando_outros_devices",
                "publicando_ports")
    metadata = [
        "format=nxframework-checkpoint-v1",
        "checkpoint_id=%s" % directory.name,
        "created_utc=2026-01-01T00:00:01.000000000Z",
        "repo=%s" % ROOT,
        "git_head=%s" % git_head,
        "git_branch=framework/tests-1.1.3",
        "included_path_count=%d" % len(included),
    ]
    metadata += ["included_path=%s" % path for path in included]
    (directory / "metadata.txt").write_text("\n".join(metadata) + "\n",
                                             encoding="utf-8")
    for name in CHECKPOINT_FILES[1:]:
        (directory / name).write_bytes(b"fixture\n")
    write_manifest(directory, CHECKPOINT_FILES)


def seal(log_root, runner):
    return subprocess.run(
        [sys.executable, "-B", str(SEALER), "--log-root", str(log_root),
         "--runner", str(runner)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
        timeout=120)


def sealer_cases(workdir):
    runner = workdir / "runner.sh"
    runner.write_text(
        "#!/usr/bin/env bash\n"
        "run_gate alpha \\\n  python3 -B alpha.py\n"
        "run_gate beta \\\n  bash beta.sh\n"
        "run_gate m20-green-checkpoint \\\n  bash checkpoint.sh \\\n"
        "  --checkpoint-root \"$LOG_ROOT/checkpoints\"\n",
        encoding="utf-8")
    head = "a" * 40

    def build(name):
        log_root = workdir / name
        log_root.mkdir()
        write_receipt(log_root / "run-alpha", "python3 -B alpha.py", head)
        write_receipt(log_root / "run-beta", "bash beta.sh", head)
        write_receipt(
            log_root / "run-checkpoint",
            "bash checkpoint.sh --checkpoint-root %s"
            % (log_root / "checkpoints"), head)
        checkpoint = log_root / "checkpoints" / "cp"
        write_checkpoint(checkpoint, head)
        return log_root

    good = build("good")
    result = seal(good, runner)
    check("sealer accepts a complete run", result.returncode == 0)
    check("sealer states the result",
          b"ALL PASS count=3" in result.stdout)
    check("sealer wrote the summary",
          (good / "RUN-SUMMARY.txt").is_file())
    check("summary is sealed by the manifest",
          "RUN-SUMMARY.txt" in
          (good / "RUN-MANIFEST.sha256").read_text(encoding="utf-8"))

    failed = build("failed-status")
    write_receipt_over(failed / "run-beta", "bash beta.sh", head, status="1")
    check("sealer rejects command_status!=0",
          b"command_status=0" in seal(failed, runner).stdout)

    failed_checkpoint = build("failed-checkpoint-status")
    write_receipt_over(
        failed_checkpoint / "run-checkpoint",
        "bash checkpoint.sh --checkpoint-root %s"
        % (failed_checkpoint / "checkpoints"), head, status="7")
    check("sealer rejects checkpoint gate command_status!=0",
          b"command_status=0" in seal(failed_checkpoint, runner).stdout)

    failed_checkpoint_tee = build("failed-checkpoint-tee")
    write_receipt_over(
        failed_checkpoint_tee / "run-checkpoint",
        "bash checkpoint.sh --checkpoint-root %s"
        % (failed_checkpoint_tee / "checkpoints"), head, tee="1")
    check("sealer rejects checkpoint gate tee_status!=0",
          b"tee_status=0" in seal(failed_checkpoint_tee, runner).stdout)

    signalled = build("signalled")
    write_receipt_over(
        signalled / "run-checkpoint",
        "bash checkpoint.sh --checkpoint-root %s"
        % (signalled / "checkpoints"), head, signal="TERM")
    check("sealer rejects a signalled checkpoint gate receipt",
          b"recorded a signal" in seal(signalled, runner).stdout)

    mixed = build("mixed-head")
    write_receipt_over(mixed / "run-beta", "bash beta.sh", "b" * 40)
    check("sealer rejects mixed git_head",
          b"different commits" in seal(mixed, runner).stdout)

    foreign_checkpoint = build("foreign-checkpoint-head")
    shutil.rmtree(foreign_checkpoint / "checkpoints" / "cp")
    write_checkpoint(foreign_checkpoint / "checkpoints" / "cp", "b" * 40)
    check("sealer rejects a checkpoint from another git_head",
          b"git_head does not match" in seal(foreign_checkpoint,
                                               runner).stdout)

    wrong_checkpoint_suffix = build("wrong-checkpoint-suffix")
    write_receipt_over(
        wrong_checkpoint_suffix / "run-checkpoint",
        "bash checkpoint.sh --checkpoint-root %s"
        % (wrong_checkpoint_suffix / "not-checkpoints"), head)
    check("sealer preserves the LOG_ROOT command suffix",
          b"not canonical gates" in seal(wrong_checkpoint_suffix,
                                          runner).stdout)

    missing = build("missing-gate")
    shutil.rmtree(missing / "run-beta")
    check("sealer rejects a missing canonical gate",
          b"canonical gates without a receipt" in seal(missing, runner).stdout)

    foreign = build("foreign-command")
    write_receipt(foreign / "run-extra", "python3 -B intruder.py", head)
    check("sealer rejects a non-canonical command",
          b"not canonical gates" in seal(foreign, runner).stdout)

    tampered = build("tampered")
    tampered_result = tampered / "run-beta" / "result.txt"
    tampered_result.write_text(
        tampered_result.read_text(encoding="utf-8").replace(
            "ended_utc=2026-01-01T00:00:01.000000000Z",
            "ended_utc=2026-01-01T00:00:02.000000000Z"),
        encoding="utf-8")
    check("sealer recomputes receipt manifests",
          b"does not match its bytes" in seal(tampered, runner).stdout)

    uncovered = build("uncovered")
    (uncovered / "run-beta" / "uncovered.txt").write_text("x",
                                                           encoding="utf-8")
    check("sealer rejects files outside the receipt manifest",
          b"not covered by" in seal(uncovered, runner).stdout)


def write_receipt_over(directory, command, git_head, status="0", tee="0",
                       signal="none"):
    shutil.rmtree(directory)
    write_receipt(directory, command, git_head, status, tee, signal)


def main():
    workdir = Path(tempfile.mkdtemp(prefix="device-environments-negative."))
    try:
        imagefs_cases(workdir)
        closure_cases(workdir)
        tool_path_cases(workdir)
        sealer_cases(workdir)
    finally:
        shutil.rmtree(str(workdir), ignore_errors=True)

    if failures:
        print("device environment negative gate failed:", file=sys.stderr)
        for failure in failures:
            print("  - %s" % failure, file=sys.stderr)
        return 1
    print("device environment negative gate passed: containment, receipts, "
          "closure, trusted tool paths and run seal all fail closed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
