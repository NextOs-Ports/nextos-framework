#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Offline adversarial gate for immutable framework build snapshots."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import zlib


REPOSITORY = Path(__file__).resolve().parents[3]
ROOT = REPOSITORY / "framework" / "nxgenerator"
TOOL = ROOT / "framework_pin.py"
SCHEMA = ROOT / "schema" / "framework-build-pin-v1.schema.json"


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def load_tool_module():
    specification = importlib.util.spec_from_file_location(
        "nxgenerator_framework_pin_under_test", TOOL
    )
    require(specification is not None and specification.loader is not None,
            "cannot load framework pin helper")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


PIN_TOOL = load_tool_module()


def run(command, cwd=None, environment=None, expected=0, pattern=None,
        umask=None):
    selected_environment = os.environ.copy()
    selected_environment["PYTHONDONTWRITEBYTECODE"] = "1"
    if environment:
        selected_environment.update(environment)

    def set_umask():
        if umask is not None:
            os.umask(umask)

    result = subprocess.run(
        [str(item) for item in command],
        cwd=None if cwd is None else str(cwd),
        env=selected_environment,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        preexec_fn=set_umask if umask is not None else None,
    )
    require(
        result.returncode == expected,
        "command status %d != %d: %s\nstdout=%s\nstderr=%s" %
        (result.returncode, expected, " ".join(str(item) for item in command),
         result.stdout.strip(), result.stderr.strip()),
    )
    if pattern is not None:
        combined = result.stdout + result.stderr
        require(pattern.lower() in combined.lower(),
                "command did not report %r: %s" % (pattern, combined.strip()))
    return result


def git(repository, *arguments):
    return run(["git", "-C", repository] + list(arguments)).stdout.strip()


def initialize_repository(path):
    path.mkdir()
    git(path, "init", "-q")
    git(path, "config", "user.name", "NXGenerator Test")
    git(path, "config", "user.email", "nxgenerator@example.invalid")
    component = path / "framework" / "nxloader"
    (component / "src").mkdir(parents=True)
    (component / "VERSION").write_text("0.7.1\n", encoding="ascii")
    (component / "src" / "core.c").write_text(
        "int pinned_source(void) { return 71; }\n", encoding="ascii"
    )
    tool = component / "tool.sh"
    tool.write_text("#!/bin/sh\nexit 0\n", encoding="ascii")
    tool.chmod(0o755)
    git(path, "add", "framework/nxloader")
    git(path, "commit", "-q", "-m", "fixture v0.7.1")
    return git(path, "rev-parse", "HEAD")


def tool(*arguments, cwd=None, expected=0, pattern=None, environment=None,
         umask=None):
    return run(
        [sys.executable, "-B", TOOL] + list(arguments),
        cwd=cwd,
        environment=environment,
        expected=expected,
        pattern=pattern,
        umask=umask,
    )


def canonical_json(value):
    return (json.dumps(
        value, ensure_ascii=False, indent=2, sort_keys=True
    ) + "\n").encode("utf-8")


def read_pin(path):
    payload = path.read_bytes()
    document = json.loads(payload.decode("utf-8"))
    require(payload == canonical_json(document), "created pin is not canonical")
    return document


def tree_snapshot(root):
    result = {}
    paths = [root] + sorted(root.rglob("*"))
    for path in paths:
        relative = "." if path == root else path.relative_to(root).as_posix()
        information = path.lstat()
        require(not stat.S_ISLNK(information.st_mode),
                "materialized snapshot contains a symlink")
        if stat.S_ISDIR(information.st_mode):
            result[relative] = (
                "dir", stat.S_IMODE(information.st_mode),
                information.st_mtime_ns, b"",
            )
        else:
            require(stat.S_ISREG(information.st_mode),
                    "materialized snapshot contains a special file")
            result[relative] = (
                "file", stat.S_IMODE(information.st_mode),
                information.st_mtime_ns, path.read_bytes(),
            )
    return result


def write_pin(path, document, canonical=True):
    payload = canonical_json(document)
    if not canonical:
        payload = json.dumps(document, separators=(",", ":")).encode("utf-8")
    path.write_bytes(payload)


def altered_pin(original, path, mutate, canonical=True):
    document = json.loads(json.dumps(original))
    mutate(document)
    write_pin(path, document, canonical=canonical)
    return path


def test_synthetic_repository(work):
    repository = work / "repository"
    commit_one = initialize_repository(repository)
    pin = work / "framework-pin.json"
    tool(
        "create", "--repository", repository,
        "--commit", "HEAD", "--component", "nxloader",
        "--output", pin,
        cwd=work,
    )
    document = read_pin(pin)
    require(document == {
        "schema": PIN_TOOL.PIN_SCHEMA,
        "schema_version": 1,
        "tree_digest": PIN_TOOL.TREE_DIGEST,
        "components": {
            "nxloader": {
                "version": "0.7.1",
                "commit": commit_one,
                "tree_sha256": document["components"]["nxloader"]["tree_sha256"],
            },
        },
    }, "created framework pin fields differ")
    require(len(document["components"]["nxloader"]["tree_sha256"]) == 64,
            "component tree digest is not SHA-256")

    component = repository / "framework" / "nxloader"
    (component / "VERSION").write_text("9.9.9\n", encoding="ascii")
    (component / "src" / "core.c").write_text(
        "int dirty_checkout(void) { return 999; }\n", encoding="ascii"
    )
    (component / "untracked.txt").write_text("must not leak\n", encoding="ascii")

    first = work / "snapshot-a"
    tool(
        "materialize", "--repository", repository,
        "--pin", pin, "--destination", first,
        cwd=work, environment={"LC_ALL": "C"}, umask=0o077,
    )
    require((first / "framework/nxloader/VERSION").read_text() == "0.7.1\n",
            "materializer read dirty VERSION from the working tree")
    require(b"pinned_source" in
            (first / "framework/nxloader/src/core.c").read_bytes(),
            "materializer read dirty source from the working tree")
    require(not (first / "framework/nxloader/untracked.txt").exists(),
            "materializer copied an untracked working-tree file")
    tool("verify", "--pin", pin, "--snapshot", first, cwd=work)

    git(repository, "add", "framework/nxloader")
    git(repository, "commit", "-q", "-m", "fixture changed checkout")
    commit_two = git(repository, "rev-parse", "HEAD")
    require(commit_two != commit_one, "fixture commits did not diverge")
    git(repository, "replace", commit_one, commit_two)

    redirected_repository = work / "redirected-repository"
    initialize_repository(redirected_repository)
    hostile_path = work / "hostile-path"
    hostile_path.mkdir()
    hostile_git_marker = work / "hostile-git-executed"
    hostile_git = hostile_path / "git"
    hostile_git.write_text(
        "#!/bin/sh\n: > '%s'\nexit 91\n" % hostile_git_marker,
        encoding="utf-8",
    )
    hostile_git.chmod(0o755)

    second = work / "díferent parent" / "snapshot-b"
    second.parent.mkdir()
    tool(
        "materialize", "--repository", repository,
        "--pin", pin, "--destination", second,
        cwd=repository, environment={
            "LC_ALL": "C.UTF-8",
            "GIT_DIR": str(redirected_repository / ".git"),
            "GIT_WORK_TREE": str(redirected_repository),
            "GIT_OBJECT_DIRECTORY": str(
                redirected_repository / ".git" / "objects"
            ),
            "PATH": str(hostile_path),
        }, umask=0o022,
    )
    require(not hostile_git_marker.exists(),
            "framework pin helper executed Git from inherited PATH")
    require(tree_snapshot(first) == tree_snapshot(second),
            "snapshot changed with path, umask, locale, checkout or replace ref")
    require(all(record[2] == PIN_TOOL.NORMALIZED_MTIME
                * 1_000_000_000
                for record in tree_snapshot(second).values()),
            "snapshot mtimes are not normalized")
    tool("verify", "--pin", pin, "--snapshot", second, cwd=repository)

    existing = work / "existing"
    existing.mkdir()
    marker = existing / "marker"
    marker.write_text("owned\n", encoding="ascii")
    tool(
        "materialize", "--repository", repository,
        "--pin", pin, "--destination", existing,
        expected=1, pattern="refusing to overwrite",
    )
    require(marker.read_text() == "owned\n",
            "existing destination was modified")
    output_link = work / "output-link"
    output_link.symlink_to(existing, target_is_directory=True)
    tool(
        "materialize", "--repository", repository,
        "--pin", pin, "--destination", output_link,
        expected=1, pattern="refusing to overwrite",
    )

    malformed = []
    malformed.append(altered_pin(
        document, work / "wrong-version.json",
        lambda value: value["components"]["nxloader"].update(
            {"version": "0.7.0"}
        ),
    ))
    malformed.append(altered_pin(
        document, work / "wrong-digest.json",
        lambda value: value["components"]["nxloader"].update(
            {"tree_sha256": "0" * 64}
        ),
    ))
    malformed.append(altered_pin(
        document, work / "short-commit.json",
        lambda value: value["components"]["nxloader"].update(
            {"commit": commit_one[:12]}
        ),
    ))
    malformed.append(altered_pin(
        document, work / "missing-object.json",
        lambda value: value["components"]["nxloader"].update(
            {"commit": "f" * 40}
        ),
    ))
    malformed.append(altered_pin(
        document, work / "extra-field.json",
        lambda value: value.update({"source_branch": "moving"}),
    ))
    malformed.append(altered_pin(
        document, work / "unknown-component.json",
        lambda value: value["components"].update({
            "nxextract": value["components"]["nxloader"]
        }),
    ))
    for index, bad_pin in enumerate(malformed):
        destination = work / ("rejected-%d" % index)
        tool(
            "materialize", "--repository", repository,
            "--pin", bad_pin, "--destination", destination,
            expected=1,
        )
        require(not destination.exists(),
                "invalid pin published a partial snapshot")

    noncanonical = altered_pin(
        document, work / "noncanonical.json", lambda value: None,
        canonical=False,
    )
    tool(
        "materialize", "--repository", repository,
        "--pin", noncanonical, "--destination", work / "noncanonical-output",
        expected=1, pattern="canonical JSON",
    )
    pin_link = work / "pin-link.json"
    pin_link.symlink_to(pin)
    tool(
        "materialize", "--repository", repository,
        "--pin", pin_link, "--destination", work / "pin-link-output",
        expected=1, pattern="symlink",
    )

    oversized_json = work / "oversized-pin.json"
    oversized_json.write_bytes(b"{" + b" " * PIN_TOOL.MAX_JSON_BYTES + b"}")
    tool(
        "materialize", "--repository", repository,
        "--pin", oversized_json,
        "--destination", work / "oversized-json-output",
        expected=1, pattern="size limit",
    )

    existing_pin = work / "existing-pin.json"
    existing_pin.write_text("owned\n", encoding="ascii")
    tool(
        "create", "--repository", repository,
        "--commit", commit_one, "--component", "nxloader",
        "--output", existing_pin,
        expected=1, pattern="refusing to overwrite",
    )
    require(existing_pin.read_text(encoding="ascii") == "owned\n",
            "pin creation modified an existing output")

    ancestor_link = work / "ancestor-link"
    ancestor_link.symlink_to(work, target_is_directory=True)
    tool(
        "materialize", "--repository", repository,
        "--pin", pin, "--destination", ancestor_link / "snapshot",
        expected=1, pattern="traverses a symlink",
    )

    tampered_source = work / "tampered-source"
    shutil.copytree(first, tampered_source)
    source = tampered_source / "framework/nxloader/src/core.c"
    source.write_text("tampered\n", encoding="ascii")
    os.utime(source, (PIN_TOOL.NORMALIZED_MTIME, PIN_TOOL.NORMALIZED_MTIME))
    tool(
        "verify", "--pin", pin, "--snapshot", tampered_source,
        expected=1, pattern="tree SHA-256 mismatch",
    )

    tampered_receipt = work / "tampered-receipt"
    shutil.copytree(first, tampered_receipt)
    receipt_path = tampered_receipt / PIN_TOOL.RECEIPT_NAME
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    receipt["claims"]["component_working_trees_ignored"] = False
    receipt_path.write_bytes(canonical_json(receipt))
    os.utime(receipt_path,
             (PIN_TOOL.NORMALIZED_MTIME, PIN_TOOL.NORMALIZED_MTIME))
    tool(
        "verify", "--pin", pin, "--snapshot", tampered_receipt,
        expected=1, pattern="receipt differs",
    )

    tampered_generator = work / "tampered-generator"
    shutil.copytree(first, tampered_generator)
    receipt_path = tampered_generator / PIN_TOOL.RECEIPT_NAME
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    receipt["generator"]["receipt_contract"] = "self-asserted-v9"
    receipt_path.write_bytes(canonical_json(receipt))
    os.utime(
        receipt_path,
        ns=(PIN_TOOL.NORMALIZED_MTIME_NS, PIN_TOOL.NORMALIZED_MTIME_NS),
    )
    tool(
        "verify", "--pin", pin, "--snapshot", tampered_generator,
        expected=1, pattern="generator is invalid",
    )

    tampered_mode = work / "tampered-mode"
    shutil.copytree(first, tampered_mode)
    target = tampered_mode / "framework/nxloader/src/core.c"
    target.chmod(0o600)
    tool(
        "verify", "--pin", pin, "--snapshot", tampered_mode,
        expected=1, pattern="type/mode differs",
    )

    tampered_mtime = work / "tampered-mtime"
    shutil.copytree(first, tampered_mtime)
    target = tampered_mtime / "framework/nxloader/VERSION"
    os.utime(target, (PIN_TOOL.NORMALIZED_MTIME + 1,
                      PIN_TOOL.NORMALIZED_MTIME + 1))
    tool(
        "verify", "--pin", pin, "--snapshot", tampered_mtime,
        expected=1, pattern="mtime differs",
    )

    tampered_mtime_ns = work / "tampered-mtime-ns"
    shutil.copytree(first, tampered_mtime_ns)
    target = tampered_mtime_ns / "framework/nxloader/VERSION"
    changed_ns = PIN_TOOL.NORMALIZED_MTIME_NS + 123456789
    os.utime(target, ns=(changed_ns, changed_ns))
    tool(
        "verify", "--pin", pin, "--snapshot", tampered_mtime_ns,
        expected=1, pattern="mtime differs",
    )

    tampered_hardlink = work / "tampered-hardlink"
    shutil.copytree(first, tampered_hardlink)
    target = tampered_hardlink / "framework/nxloader/src/core.c"
    backing = work / "hardlink-backing.c"
    backing.write_bytes(target.read_bytes())
    backing.chmod(0o644)
    os.utime(
        backing,
        ns=(PIN_TOOL.NORMALIZED_MTIME_NS, PIN_TOOL.NORMALIZED_MTIME_NS),
    )
    target.unlink()
    os.link(backing, target)
    os.utime(
        target.parent,
        ns=(PIN_TOOL.NORMALIZED_MTIME_NS, PIN_TOOL.NORMALIZED_MTIME_NS),
    )
    tool(
        "verify", "--pin", pin, "--snapshot", tampered_hardlink,
        expected=1, pattern="hard link",
    )

    tampered_link = work / "tampered-link"
    shutil.copytree(first, tampered_link)
    target = tampered_link / "framework/nxloader/src/core.c"
    target.unlink()
    target.symlink_to("../VERSION")
    os.utime(
        target.parent,
        ns=(PIN_TOOL.NORMALIZED_MTIME_NS, PIN_TOOL.NORMALIZED_MTIME_NS),
    )
    tool(
        "verify", "--pin", pin, "--snapshot", tampered_link,
        expected=1, pattern="symlink",
    )

    alternate_tool = work / "framework_pin_alternate_version.py"
    shutil.copy2(TOOL, alternate_tool)
    (work / "VERSION").write_text("9.9.9\n", encoding="ascii")
    third = work / "snapshot-alternate-tool-version"
    run([
        sys.executable, "-B", alternate_tool,
        "materialize", "--repository", repository,
        "--pin", pin, "--destination", third,
    ], cwd=work)
    require(
        tree_snapshot(first) == tree_snapshot(third),
        "snapshot bytes depend on the helper checkout VERSION",
    )
    return commit_one


def test_git_object_and_filter_boundaries(work):
    corrupt_repository = work / "corrupt-repository"
    initialize_repository(corrupt_repository)
    object_id = git(
        corrupt_repository, "rev-parse", "HEAD:framework/nxloader/src/core.c"
    )
    loose_object = (
        corrupt_repository / ".git" / "objects" /
        object_id[:2] / object_id[2:]
    )
    raw_object = zlib.decompress(loose_object.read_bytes())
    require(b"return 71" in raw_object, "corrupt-object fixture differs")
    loose_object.chmod(0o644)
    loose_object.write_bytes(zlib.compress(raw_object.replace(
        b"return 71", b"return 72"
    )))
    tool(
        "create", "--repository", corrupt_repository,
        "--commit", "HEAD", "--component", "nxloader",
        "--output", work / "corrupt-pin.json",
        expected=1, pattern="object hash mismatch",
    )

    filter_repository = work / "filter-repository"
    initialize_repository(filter_repository)
    attributes = filter_repository / ".gitattributes"
    attributes.write_text(
        "framework/nxloader/** filter=nxgenerator-evil\n", encoding="ascii"
    )
    git(filter_repository, "add", ".gitattributes")
    git(filter_repository, "commit", "-q", "-m", "declare inert filter")
    marker = work / "filter-executed"
    filter_script = work / "filter-helper.sh"
    filter_script.write_text(
        "#!/bin/sh\n: > '%s'\ncat\n" % marker, encoding="utf-8"
    )
    filter_script.chmod(0o755)
    git(
        filter_repository, "config", "filter.nxgenerator-evil.clean",
        str(filter_script),
    )
    git(
        filter_repository, "config", "filter.nxgenerator-evil.smudge",
        str(filter_script),
    )
    tool(
        "create", "--repository", filter_repository,
        "--commit", "HEAD", "--component", "nxloader",
        "--output", work / "filter-pin.json",
    )
    require(not marker.exists(), "Git clean/smudge filter executed")

    lazy_repository = work / "lazy-repository"
    initialize_repository(lazy_repository)
    missing_object = git(
        lazy_repository, "rev-parse", "HEAD:framework/nxloader/src/core.c"
    )
    missing_path = (
        lazy_repository / ".git" / "objects" /
        missing_object[:2] / missing_object[2:]
    )
    missing_path.unlink()
    lazy_marker = work / "lazy-fetch-executed"
    lazy_helper = work / "lazy-fetch-helper.sh"
    lazy_helper.write_text(
        "#!/bin/sh\n: > '%s'\nexit 1\n" % lazy_marker, encoding="utf-8"
    )
    lazy_helper.chmod(0o755)
    git(lazy_repository, "config", "core.repositoryformatversion", "1")
    included_config = work / "included-promisor.config"
    included_config.write_text(
        "[extensions]\n"
        "\tpartialClone = origin\n"
        "[remote \"origin\"]\n"
        "\tpromisor = true\n"
        "\turl = ext::%s\n"
        "[protocol \"ext\"]\n"
        "\tallow = always\n" % lazy_helper,
        encoding="utf-8",
    )
    git(lazy_repository, "config", "include.path", str(included_config))
    tool(
        "create", "--repository", lazy_repository,
        "--commit", "HEAD", "--component", "nxloader",
        "--output", work / "lazy-pin.json",
        expected=1,
    )
    require(not lazy_marker.exists(), "Git attempted a lazy fetch")

    oversized_repository = work / "oversized-repository"
    initialize_repository(oversized_repository)
    oversized = oversized_repository / "framework/nxloader/src/oversized.bin"
    with oversized.open("wb") as output:
        output.seek(PIN_TOOL.MAX_BLOB_BYTES)
        output.write(b"x")
    git(oversized_repository, "add", "framework/nxloader/src/oversized.bin")
    git(oversized_repository, "commit", "-q", "-m", "add oversized blob")
    tool(
        "create", "--repository", oversized_repository,
        "--commit", "HEAD", "--component", "nxloader",
        "--output", work / "oversized-blob-pin.json",
        expected=1, pattern="size limit",
    )


def test_unsupported_git_entries(work):
    symlink_repository = work / "symlink-repository"
    initialize_repository(symlink_repository)
    link = symlink_repository / "framework/nxloader/source-link"
    link.symlink_to("VERSION")
    git(symlink_repository, "add", "framework/nxloader/source-link")
    git(symlink_repository, "commit", "-q", "-m", "add symlink")
    tool(
        "create", "--repository", symlink_repository,
        "--commit", "HEAD", "--component", "nxloader",
        "--output", work / "symlink-pin.json",
        expected=1, pattern="unsupported mode/type",
    )

    gitlink_repository = work / "gitlink-repository"
    referenced_commit = initialize_repository(gitlink_repository)
    git(
        gitlink_repository, "update-index", "--add", "--cacheinfo",
        "160000,%s,framework/nxloader/vendor" % referenced_commit,
    )
    git(gitlink_repository, "commit", "-q", "-m", "add gitlink")
    tool(
        "create", "--repository", gitlink_repository,
        "--commit", "HEAD", "--component", "nxloader",
        "--output", work / "gitlink-pin.json",
        expected=1, pattern="unsupported mode/type",
    )

    version_repository = work / "version-repository"
    initialize_repository(version_repository)
    (version_repository / "framework/nxloader/VERSION").write_text(
        "00.7.1\n", encoding="ascii"
    )
    git(version_repository, "add", "framework/nxloader/VERSION")
    git(version_repository, "commit", "-q", "-m", "noncanonical version")
    tool(
        "create", "--repository", version_repository,
        "--commit", "HEAD", "--component", "nxloader",
        "--output", work / "version-pin.json",
        expected=1, pattern="VERSION is invalid",
    )

    empty_tree_repository = work / "empty-tree-repository"
    initialize_repository(empty_tree_repository)
    empty_tree = git(empty_tree_repository, "mktree")
    base_tree = git(
        empty_tree_repository, "rev-parse", "HEAD:framework/nxloader"
    )
    base_listing = run([
        "git", "-C", empty_tree_repository, "ls-tree", base_tree,
    ]).stdout
    # Feed a valid component listing plus one representable-but-unmaterializable
    # empty subtree through mktree.
    component_tree = subprocess.run(
        ["git", "-C", str(empty_tree_repository), "mktree"],
        input=base_listing + "040000 tree %s\tempty-proof\n" % empty_tree,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=True,
    ).stdout.strip()
    framework_tree = subprocess.run(
        ["git", "-C", str(empty_tree_repository), "mktree"],
        input="040000 tree %s\tnxloader\n" % component_tree,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=True,
    ).stdout.strip()
    root_tree = subprocess.run(
        ["git", "-C", str(empty_tree_repository), "mktree"],
        input="040000 tree %s\tframework\n" % framework_tree,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=True,
    ).stdout.strip()
    empty_commit = run(
        [
            "git", "-C", empty_tree_repository, "commit-tree", root_tree,
            "-m", "fixture with empty tree",
        ],
        environment={
            "GIT_AUTHOR_NAME": "NXGenerator Test",
            "GIT_AUTHOR_EMAIL": "nxgenerator@example.invalid",
            "GIT_COMMITTER_NAME": "NXGenerator Test",
            "GIT_COMMITTER_EMAIL": "nxgenerator@example.invalid",
        },
    ).stdout.strip()
    tool(
        "create", "--repository", empty_tree_repository,
        "--commit", empty_commit, "--component", "nxloader",
        "--output", work / "empty-tree-pin.json",
        expected=1, pattern="empty Git directory",
    )


def test_real_repository_smoke(work):
    pin = work / "real-framework-pin.json"
    tool(
        "create", "--repository", REPOSITORY,
        "--commit", "nxgenerator-v0.2.1",
        "--component", "nxgenerator", "--component", "nxloader",
        "--output", pin,
    )
    document = read_pin(pin)
    require(document["components"]["nxgenerator"]["version"] == "0.2.1",
            "real nxgenerator tag version differs")
    require(document["components"]["nxloader"]["version"] == "0.7.1",
            "real nxloader version differs")
    snapshot = work / "real-framework-snapshot"
    tool(
        "materialize", "--repository", REPOSITORY,
        "--pin", pin, "--destination", snapshot,
    )
    tool("verify", "--pin", pin, "--snapshot", snapshot)


def main():
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    require(schema["properties"]["schema"]["const"] == PIN_TOOL.PIN_SCHEMA,
            "JSON Schema and runtime pin identity differ")
    require(schema["properties"]["tree_digest"]["const"] ==
            PIN_TOOL.TREE_DIGEST,
            "JSON Schema and runtime tree digest differ")
    require(set(schema["properties"]["components"]["properties"]) ==
            set(PIN_TOOL.COMPONENTS),
            "JSON Schema and runtime component registries differ")
    for command in ("git", sys.executable):
        require(shutil.which(command) is not None,
                "required command is unavailable: %s" % command)
    with tempfile.TemporaryDirectory(prefix="nxgenerator-framework-pin-") as raw:
        work = Path(raw)
        test_synthetic_repository(work)
        test_unsupported_git_entries(work)
        test_git_object_and_filter_boundaries(work)
        test_real_repository_smoke(work)
        for value in (b"../escape", b"/absolute", b"dir\\file",
                      b"control\nname", "control\u0085name".encode("utf-8"),
                      b"x" * 256):
            try:
                PIN_TOOL.validate_relative_path(value, "fixture")
            except PIN_TOOL.PinError:
                continue
            raise GateError("unsafe path was accepted: %r" % value)
    print(
        "nxgenerator framework pin tests passed: immutable_git=1 dirty_checkout=1 "
        "replace_refs=blocked git_env_redirect=blocked object_integrity=1 "
        "lazy_fetch=blocked filters=not_executed deterministic_snapshot=1 "
        "canonical_pin=1 symlink_gitlink_hardlink=blocked tamper_fail_closed=1 "
        "no_overwrite=1 real_smoke=1"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (GateError, OSError, ValueError, KeyError) as error:
        print("nxgenerator framework pin tests failed: %s" % error,
              file=sys.stderr)
        raise SystemExit(1)
