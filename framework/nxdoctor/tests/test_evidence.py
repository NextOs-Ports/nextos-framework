#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Integration and tamper gate for nx-doctor-recovery-evidence-v1."""

import copy
import builtins
import contextlib
import errno
import io
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OWNER = os.path.abspath(os.path.join(HERE, ".."))
EVIDENCE_TOOL = os.path.join(OWNER, "evidence.py")
DOCTOR_TOOL = os.path.join(OWNER, "nxdoctor.py")
sys.path.insert(0, OWNER)
sys.path.insert(0, HERE)

import evidence as EV
import test_nxdoctor as CORE


CHECKS = {"actions": 0, "refusals": 0, "guards": 0,
          "owner_preserved": 0, "physical_pending": 0}


def fail(message):
    print("FAIL: %s" % message)
    sys.exit(1)


def check(condition, message, bucket="guards"):
    if not condition:
        fail(message)
    CHECKS[bucket] += 1


def expect_error(function, message, contains=None):
    try:
        function()
    except EV.EvidenceError as error:
        if contains is not None and contains not in str(error):
            fail("%s (wrong error: %s)" % (message, error))
        CHECKS["guards"] += 1
        return
    fail(message)


def invoke(function, *args):
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        rc = function(*args)
    lines = [line for line in output.getvalue().splitlines() if line]
    check(len(lines) == 1, "action did not emit exactly one receipt")
    return rc, json.loads(lines[0])


def doctor_hash():
    return EV.sha256_file(DOCTOR_TOOL)


def validate(document, run_id, environment="HOST_FIXTURE",
             expected_action=None, expected_target=None):
    identity = document["payload"]["identity"]
    if expected_action is None:
        expected_action = identity["action"]
    if expected_target is None:
        expected_target = identity["target"]
    return EV.validate_evidence(document, run_id, doctor_hash(), environment,
                                expected_action, expected_target)


def reseal(document):
    document["integrity"]["payload_sha256"] = EV.sha256_bytes(
        EV.canonical_bytes(document["payload"]))
    return document


def make_fixture(root, label):
    base = os.path.join(root, label)
    os.makedirs(base)
    return CORE.make_port(base)


def capture(port, action, target, run_id):
    document = EV.capture_action(port, action, target, run_id)
    result = validate(document, run_id, expected_action=action,
                      expected_target=target)
    serialized = json.dumps(document, sort_keys=True)
    check(os.path.abspath(port) not in serialized,
          "evidence leaked the absolute fixture path")
    execution = document["payload"]["execution"]
    check("stdout" not in execution and "stderr" not in execution,
          "evidence retained raw process output")
    return document, result


def test_real_actions(root):
    owner_hashes = []

    port = make_fixture(root, "restore")
    before_owner = CORE.owner_hash(port)
    restore, result = capture(
        port, "restore-previous", {"generation": CORE.GEN_B}, "restore-ok")
    check(result == "ok", "restore evidence did not validate", "actions")
    check(CORE.owner_hash(port) == before_owner,
          "restore changed owner data", "owner_preserved")
    owner_hashes.append(before_owner)
    repeated, result = capture(
        port, "restore-previous", {"generation": CORE.GEN_B},
        "restore-already")
    check(result == "already-clear", "restore idempotency was not captured",
          "actions")

    port = make_fixture(root, "restore-v2-chmodless")
    long_a = "a1" * 32
    long_b = "b2" * 32
    components = {
        "files/launcher/Fixture.sh": ("#!/bin/bash\nexit 0\n", "0755"),
        "files/nxport.json": ('{"id":"fixture"}\n', "0644"),
        "files/runtime/fixture-loader": ("#!/bin/bash\nexit 0\n", "0755"),
    }
    CORE.make_generation_v2(port, long_a, components)
    CORE.make_generation_v2(port, long_b, components)
    CORE.write_state_v2(port, long_a, "null", long_b)
    for generation in (long_a, long_b):
        root_dir = os.path.join(port, ".nxruntime", "generations", generation)
        for dirpath, _dirnames, filenames in os.walk(root_dir):
            for name in filenames:
                os.chmod(os.path.join(dirpath, name), 0o777)
    before_owner = CORE.owner_hash(port)
    modern, result = capture(
        port, "restore-previous", {"generation": long_b},
        "restore-v2-chmodless")
    check(result == "ok", "v2/64/chmodless restore evidence failed",
          "actions")
    check(CORE.owner_hash(port) == before_owner,
          "v2/64/chmodless restore changed owner data", "owner_preserved")

    port = make_fixture(root, "discard")
    before_owner = CORE.owner_hash(port)
    target = ".nxruntime/staging/evidence-target"
    CORE.write(os.path.join(port, target, "half.bin"), b"partial")
    discard, result = capture(
        port, "discard-staging", {"path": target}, "discard-ok")
    check(result == "ok" and not os.path.exists(os.path.join(port, target)),
          "discard evidence did not prove exact removal", "actions")
    check(CORE.owner_hash(port) == before_owner,
          "discard changed owner data", "owner_preserved")
    owner_hashes.append(before_owner)
    repeated, result = capture(
        port, "discard-staging", {"path": target}, "discard-already")
    check(result == "already-clear", "discard idempotency was not captured",
          "actions")

    port = make_fixture(root, "gc")
    before_owner = CORE.owner_hash(port)
    CORE.make_generation(port, CORE.GEN_C,
                         {"bin/game": ("GAME-C", 0o755),
                          "lib/libx.so": ("LIBX-C", 0o644)})
    collected, result = capture(
        port, "complete-gc", {"generation": CORE.GEN_C}, "gc-ok")
    check(result == "ok"
          and not os.path.exists(os.path.join(
              port, ".nxruntime", "generations", CORE.GEN_C)),
          "gc evidence did not prove exact collection", "actions")
    check(CORE.owner_hash(port) == before_owner,
          "gc changed owner data", "owner_preserved")
    owner_hashes.append(before_owner)
    repeated, result = capture(
        port, "complete-gc", {"generation": CORE.GEN_C}, "gc-already")
    check(result == "already-clear", "gc idempotency was not captured",
          "actions")

    port = make_fixture(root, "gc-resume")
    CORE.make_generation(port, CORE.GEN_C,
                         {"bin/game": ("GAME-C", 0o755)})
    generation = os.path.join(port, ".nxruntime", "generations", CORE.GEN_C)
    CORE.write(os.path.join(generation, EV.doctor.GC_CLAIM_NAME),
               (CORE.GEN_C + "\n").encode("ascii"), 0o600)
    os.unlink(os.path.join(generation, "commit"))
    resumed, result = capture(
        port, "complete-gc", {"generation": CORE.GEN_C}, "gc-resume")
    before_claim = resumed["payload"]["snapshot_before"]["generations"] \
        [CORE.GEN_C]["claim"]
    check(result == "ok" and isinstance(before_claim, dict)
          and before_claim["links"] == 1,
          "authenticated interrupted-gc claim was not bound", "actions")

    port = make_fixture(root, "clear")
    before_owner = CORE.owner_hash(port)
    dead_pid = 4194000
    while os.path.exists("/proc/%d" % dead_pid):
        dead_pid -= 1
    _flock, lock_dir, _name = EV.doctor.lock_paths(port)
    CORE.write(os.path.join(lock_dir, "owner"), "%d 1\n" % dead_pid)
    cleared, result = capture(
        port, "clear-lock", {"pid": dead_pid}, "clear-ok")
    check(result == "ok" and not os.path.exists(lock_dir),
          "clear-lock evidence did not prove dead-owner removal", "actions")
    check(CORE.owner_hash(port) == before_owner,
          "clear-lock changed owner data", "owner_preserved")
    owner_hashes.append(before_owner)
    repeated, result = capture(
        port, "clear-lock", {"pid": dead_pid}, "clear-already")
    check(result == "already-clear", "clear-lock idempotency was not captured",
          "actions")

    port = make_fixture(root, "clear-launcher-token")
    before_owner = CORE.owner_hash(port)
    _flock, lock_dir, _name = EV.doctor.lock_paths(port)
    CORE.write(os.path.join(lock_dir, "owner"),
               "pid=%d token=launcher-run-1\n" % dead_pid, 0o600)
    token_clear, result = capture(
        port, "clear-lock", {"pid": dead_pid}, "clear-token-ok")
    check(result == "ok" and not os.path.exists(lock_dir),
          "current launcher token owner was not understood", "actions")
    check(CORE.owner_hash(port) == before_owner,
          "token clear-lock changed owner data", "owner_preserved")

    return {"restore": restore, "modern": modern, "discard": discard,
            "gc": collected, "gc_resume": resumed,
            "clear": cleared, "clear_token": token_clear,
            "owner_hashes": owner_hashes}


def test_real_refusals(root):
    cases = []

    port = make_fixture(root, "refuse-restore")
    cases.append(capture(port, "restore-previous",
                         {"generation": CORE.GEN_C}, "refuse-restore"))

    port = make_fixture(root, "refuse-discard")
    target = ".nxruntime/staging/save.dat"
    CORE.write(os.path.join(port, target), b"owner-like")
    cases.append(capture(port, "discard-staging", {"path": target},
                         "refuse-discard"))

    port = make_fixture(root, "refuse-gc")
    cases.append(capture(port, "complete-gc", {"generation": CORE.GEN_A},
                         "refuse-gc"))

    port = make_fixture(root, "refuse-gc-no-state")
    CORE.make_generation(port, CORE.GEN_C,
                         {"bin/game": ("GAME-C", 0o755)})
    os.unlink(os.path.join(port, ".nxruntime", "state.json"))
    cases.append(capture(port, "complete-gc", {"generation": CORE.GEN_C},
                         "refuse-gc-no-state"))
    check(os.path.isdir(os.path.join(port, ".nxruntime", "generations",
                                    CORE.GEN_C)),
          "gc without state mutated the generation")

    port = make_fixture(root, "refuse-clear")
    _flock, lock_dir, _name = EV.doctor.lock_paths(port)
    pid = os.getpid()
    starttime = EV.doctor.proc_starttime(pid)
    CORE.write(os.path.join(lock_dir, "owner"), "%d %d\n" % (pid, starttime))
    cases.append(capture(port, "clear-lock", {"pid": pid}, "refuse-clear"))

    port = make_fixture(root, "refuse-clear-live-token")
    _flock, lock_dir, _name = EV.doctor.lock_paths(port)
    CORE.write(os.path.join(lock_dir, "owner"),
               "pid=%d token=live-launcher-run\n" % pid, 0o600)
    cases.append(capture(port, "clear-lock", {"pid": pid},
                         "refuse-clear-live-token"))

    port = make_fixture(root, "refuse-clear-no-owner")
    _flock, lock_dir, _name = EV.doctor.lock_paths(port)
    CORE.write(os.path.join(lock_dir, "sentinel"), "preserve\n")
    dead_pid = 4194000
    while os.path.exists("/proc/%d" % dead_pid):
        dead_pid -= 1
    cases.append(capture(port, "clear-lock", {"pid": dead_pid},
                         "refuse-clear-no-owner"))
    check(os.path.isfile(os.path.join(lock_dir, "sentinel")),
          "clear-lock without owner mutated the lock")

    for _document, result in cases:
        check(result == "refused", "refusal did not remain fail-closed",
              "refusals")


def test_tamper_guards(documents):
    source = documents["restore"]

    bad = copy.deepcopy(source)
    bad["integrity"]["payload_sha256"] = "0" * 64
    expect_error(lambda: validate(bad, "restore-ok"),
                 "divergent payload hash passed", "hash diverges")

    expect_error(lambda: validate(source, "another-run"),
                 "stale run id passed", "stale")

    bad = copy.deepcopy(source)
    bad["payload"]["identity"]["action"] = "complete-gc"
    reseal(bad)
    expect_error(lambda: validate(bad, "restore-ok"),
                 "swapped action passed")

    bad = copy.deepcopy(source)
    bad["payload"]["identity"]["target"] = {"generation": CORE.GEN_A}
    reseal(bad)
    expect_error(
        lambda: validate(bad, "restore-ok",
                         expected_action="restore-previous",
                         expected_target={"generation": CORE.GEN_B}),
        "valid but unexpected target passed", "action or target")

    bad = copy.deepcopy(source)
    bad["payload"]["identity"]["action"] = "unknown"
    reseal(bad)
    expect_error(lambda: validate(bad, "restore-ok"),
                 "unknown action passed", "unknown action")

    bad = copy.deepcopy(source)
    bad["payload"]["execution"]["receipt"]["action"] = "complete-gc"
    reseal(bad)
    expect_error(lambda: validate(bad, "restore-ok"),
                 "receipt action swap passed", "receipt identity")

    bad = copy.deepcopy(source)
    bad["payload"]["execution"]["returncode"] = 1
    reseal(bad)
    expect_error(lambda: validate(bad, "restore-ok"),
                 "return-code mismatch passed", "process status")

    bad = copy.deepcopy(source)
    bad["payload"]["execution"]["stdout_nonempty_lines"] = 2
    reseal(bad)
    expect_error(lambda: validate(bad, "restore-ok"),
                 "duplicate receipts passed", "one clean receipt")

    bad = copy.deepcopy(source)
    bad["payload"]["identity"]["environment"] = "PHYSICAL"
    reseal(bad)
    expect_error(lambda: validate(bad, "restore-ok", "PHYSICAL"),
                 "physical claim without context passed", "PENDING")

    bad = copy.deepcopy(source)
    identity = bad["payload"]["identity"]
    identity["environment"] = "PHYSICAL"
    started = bad["payload"]["timing"]["started_unix"]
    identity["physical_context"] = {
        "context_sha256": "0" * 64,
        "created_unix": started - 1,
        "expires_unix": started + 60,
        "machine": "aarch64",
        "nonce_sha256": "1" * 64,
        "boot_id_sha256": "2" * 64,
        "os_release_sha256": "3" * 64,
        "port_identity_sha256": "4" * 64,
        "doctor_sha256": identity["doctor_sha256"],
        "evidence_tool_sha256": identity["evidence_tool_sha256"],
    }
    reseal(bad)
    expect_error(lambda: validate(bad, "restore-ok", "PHYSICAL"),
                 "synthetic PHYSICAL block passed without pinned context",
                 "PENDING")

    bad = copy.deepcopy(source)
    bad["payload"]["identity"]["physical_context"] = {
        "context_sha256": "0" * 64}
    reseal(bad)
    expect_error(lambda: validate(bad, "restore-ok"),
                 "host evidence accepted physical context", "host evidence")

    bad = copy.deepcopy(source)
    after = bad["payload"]["snapshot_after"]
    protected = after["protected"]["entries"]
    owner = after["owner_data"]["entries"]
    protected_file = next(item for item in protected
                          if item["type"] == "file"
                          and EV.is_owner_record(item))
    protected_file["sha256"] = "1" * 64
    owner_file = next(item for item in owner
                      if item["path"] == protected_file["path"])
    owner_file["sha256"] = "1" * 64
    after["protected"]["sha256"] = EV.sha256_bytes(
        EV.canonical_bytes(protected))
    after["owner_data"]["sha256"] = EV.sha256_bytes(
        EV.canonical_bytes(owner))
    reseal(bad)
    expect_error(lambda: validate(bad, "restore-ok"),
                 "owner mutation passed", "protected owner data")

    bad = copy.deepcopy(source)
    bad["payload"]["snapshot_after"]["state"]["document"]["active"] = \
        CORE.GEN_A
    reseal(bad)
    expect_error(lambda: validate(bad, "restore-ok"),
                 "state document detached from raw bytes passed",
                 "not bound")

    bad = copy.deepcopy(documents["clear"])
    bad["payload"]["snapshot_before"]["lock"]["owner"]["pid"] += 1
    reseal(bad)
    expect_error(lambda: validate(bad, "clear-ok"),
                 "lock owner detached from owner bytes passed",
                 "parsed lock owner")

    bad = copy.deepcopy(documents["clear"])
    bad["payload"]["snapshot_before"]["lock"]["owner"]["liveness"] = \
        "alive"
    reseal(bad)
    expect_error(lambda: validate(bad, "clear-ok"),
                 "forged lock liveness passed", "inconsistent")

    bad = copy.deepcopy(documents["discard"])
    bad["payload"]["identity"]["target"]["path"] = \
        ".nxruntime/staging/../../gamedata"
    reseal(bad)
    expect_error(lambda: validate(bad, "discard-ok"),
                 "hostile path passed")


def test_file_and_cli_guards(root, source):
    truncated = os.path.join(root, "truncated.json")
    CORE.write(truncated, b'{"schema":"nx-doctor')
    expect_error(lambda: EV.load_evidence(truncated),
                 "truncated evidence passed", "truncated")

    duplicate = os.path.join(root, "duplicate.json")
    CORE.write(duplicate, b'{"schema":"a","schema":"b"}')
    expect_error(lambda: EV.load_evidence(duplicate),
                 "duplicate JSON keys passed", "duplicate key")

    port = make_fixture(root, "output-inside")
    inside = os.path.join(port, "evidence.json")
    expect_error(lambda: EV.safe_write(inside, b"{}\n", port),
                 "evidence was written inside the port", "outside")

    link_target = os.path.join(root, "real-output")
    CORE.write(link_target, b"preserve")
    link = os.path.join(root, "output-link")
    os.symlink(link_target, link)
    expect_error(lambda: EV.safe_write(link, b"replace"),
                 "symlink output was followed", "new regular")
    check(open(link_target, "rb").read() == b"preserve",
          "symlink output target changed")

    special_port = make_fixture(root, "special-snapshot")
    fifo = os.path.join(special_port, "gamedata", "hostile-fifo")
    os.mkfifo(fifo)
    expect_error(lambda: EV.take_snapshot(special_port),
                 "special snapshot entry passed", "special file")

    evidence_file = os.path.join(root, "saved-evidence.json")
    previous_umask = os.umask(0o777)
    try:
        EV.safe_write(evidence_file,
                      json.dumps(source, sort_keys=True, indent=2) + "\n")
    finally:
        os.umask(previous_umask)
    completed = subprocess.run(
        [sys.executable, EVIDENCE_TOOL, "validate", evidence_file,
         "--expected-run-id", "restore-ok",
         "--expected-doctor-sha256", doctor_hash(),
         "--expected-environment", "HOST_FIXTURE",
         "--action", "restore-previous", "--generation", CORE.GEN_B],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        timeout=30, check=False)
    check(completed.returncode == 0 and "result=ok" in completed.stdout
          and completed.stderr == "", "validator CLI failed saved evidence")
    check(stat.S_IMODE(os.stat(evidence_file).st_mode) == 0o600,
          "saved evidence is not private")

    naked = subprocess.run(
        [sys.executable, EVIDENCE_TOOL, "capture", port, "--physical",
         "--action", "complete-gc", "--generation", CORE.GEN_C,
         "--run-id", "naked-physical"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        timeout=30, check=False)
    check(naked.returncode != 0,
          "a naked --physical boolean was accepted")
    CHECKS["physical_pending"] += 1

    physical_out = os.path.join(root, "physical-context.json")
    stdout = io.StringIO()
    stderr = io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        physical_rc = EV.main([
            "physical-context", port, "--run-id", "physical-pending",
            "--out", physical_out])
    check(physical_rc == 2 and "PENDING" in stderr.getvalue()
          and not os.path.exists(physical_out),
          "self-signed physical context was not kept pending")
    CHECKS["physical_pending"] += 1

    invalid = copy.deepcopy(source)
    invalid["payload"]["capture_error"] = "snapshot-after-failed"
    invalid = EV.seal_payload(invalid["payload"])
    real_capture = EV.capture_action
    EV.capture_action = lambda *_args, **_kwargs: invalid
    stdout = io.StringIO()
    stderr = io.StringIO()
    try:
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            rc = EV.main(["capture", port, "--action", "restore-previous",
                          "--generation", CORE.GEN_B,
                          "--run-id", "invalid-capture"])
    finally:
        EV.capture_action = real_capture
    check(rc == 2 and "did not complete" in stderr.getvalue()
          and "captured" not in stdout.getvalue(),
          "invalid capture reported false success")


def test_core_hardening(root):
    # Runtime/generation parents are confinement boundaries, never traversed
    # when an operator presents a symlinked port layout.
    external = make_fixture(root, "external-runtime")
    port = os.path.join(root, "symlink-runtime-port")
    os.makedirs(port)
    os.symlink(os.path.join(external, ".nxruntime"),
               os.path.join(port, ".nxruntime"))
    state_path = os.path.join(external, ".nxruntime", "state.json")
    state_before = open(state_path, "rb").read()
    rc, answer = invoke(EV.doctor.action_restore_previous,
                        port, CORE.GEN_B)
    check(rc == 1 and answer["result"] == "refused"
          and open(state_path, "rb").read() == state_before,
          "restore traversed a symlinked runtime parent")

    port = make_fixture(root, "symlink-generations")
    CORE.make_generation(port, CORE.GEN_C,
                         {"bin/game": ("GAME-C", 0o755)})
    generations = os.path.join(port, ".nxruntime", "generations")
    external_generations = os.path.join(root, "external-generations")
    os.rename(generations, external_generations)
    sentinel = os.path.join(external_generations, "sentinel")
    CORE.write(sentinel, b"preserve")
    os.symlink(external_generations, generations)
    rc, answer = invoke(EV.doctor.action_complete_gc, port, CORE.GEN_C)
    check(rc == 1 and answer["result"] == "refused"
          and open(sentinel, "rb").read() == b"preserve",
          "gc traversed a symlinked generations parent")

    # A canonical fallback lock excludes a concurrent mutator.
    port = make_fixture(root, "guard-contention")
    _flock, lock_dir, _name = EV.doctor.lock_paths(port)
    pid = os.getpid()
    starttime = EV.doctor.proc_starttime(pid)
    CORE.write(os.path.join(lock_dir, "owner"),
               "%d %d\n" % (pid, starttime), 0o600)
    state_path = os.path.join(port, ".nxruntime", "state.json")
    state_before = open(state_path, "rb").read()
    rc, answer = invoke(EV.doctor.action_restore_previous,
                        port, CORE.GEN_B)
    check(rc == 1 and answer["result"] == "refused"
          and open(state_path, "rb").read() == state_before,
          "action ignored a concurrent fallback lock")
    shutil.rmtree(lock_dir)

    # Inaccessible /proc is unknown, not proof that an owner is dead.
    dead_pid = 4194000
    while os.path.exists("/proc/%d" % dead_pid):
        dead_pid -= 1
    CORE.write(os.path.join(lock_dir, "owner"), "%d 1\n" % dead_pid,
               0o600)
    real_open = builtins.open

    def proc_denied(path, *args, **kwargs):
        if str(path).startswith("/proc/"):
            raise OSError(errno.EACCES, os.strerror(errno.EACCES), path)
        return real_open(path, *args, **kwargs)

    EV.doctor.open = proc_denied
    try:
        rc, answer = invoke(EV.doctor.action_clear_lock, port, dead_pid)
    finally:
        del EV.doctor.open
    check(rc == 1 and answer["result"] == "refused"
          and os.path.isdir(lock_dir),
          "clear-lock treated inaccessible proc as a dead owner")
    shutil.rmtree(lock_dir)

    # A short state write followed by ENOSPC never replaces state.json.
    port = make_fixture(root, "short-state-write")
    state_path = os.path.join(port, ".nxruntime", "state.json")
    state_before = open(state_path, "rb").read()
    real_write = EV.doctor.os.write
    writes = {"count": 0}

    def short_then_full(fd, data):
        writes["count"] += 1
        if writes["count"] == 1:
            count = max(1, len(data) // 2)
            return real_write(fd, data[:count])
        raise OSError(errno.ENOSPC, os.strerror(errno.ENOSPC))

    EV.doctor.os.write = short_then_full
    try:
        rc, answer = invoke(
            EV.doctor.action_restore_previous.__wrapped__,
            port, CORE.GEN_B)
    finally:
        EV.doctor.os.write = real_write
    check(rc == 1 and answer["result"] == "refused"
          and open(state_path, "rb").read() == state_before
          and not any(name.startswith(".state.json.nxdoctor-tmp-")
                      for name in os.listdir(os.path.dirname(state_path))),
          "short write installed or leaked a partial state")

    # failure_generation is a state-v2 reference and blocks collection.
    port = make_fixture(root, "failure-generation")
    long_a, long_b, long_c = "a1" * 32, "b2" * 32, "c3" * 32
    components = {"files/runtime/game": ("GAME", "0755")}
    for generation in (long_a, long_b, long_c):
        CORE.make_generation_v2(port, generation, components)
    CORE.write_state_v2(port, long_a, "null", long_b,
                        failure_generation=long_c,
                        last_health_run_id="health-run-1")
    rc, answer = invoke(EV.doctor.action_complete_gc, port, long_c)
    check(rc == 1 and answer["result"] == "refused"
          and os.path.isdir(os.path.join(
              port, ".nxruntime", "generations", long_c)),
          "gc removed state-v2 failure_generation")

    # Only an exact, single-link claim authorizes interrupted GC recovery.
    port = make_fixture(root, "gc-claims")
    CORE.make_generation(port, CORE.GEN_C,
                         {"bin/game": ("GAME-C", 0o755)})
    generation = os.path.join(port, ".nxruntime", "generations", CORE.GEN_C)
    claim = os.path.join(generation, EV.doctor.GC_CLAIM_NAME)
    CORE.write(claim, b"wrong\n", 0o600)
    rc, answer = invoke(EV.doctor.action_complete_gc, port, CORE.GEN_C)
    check(rc == 1 and answer["result"] == "refused"
          and os.path.isdir(generation), "wrong gc claim was trusted")
    os.unlink(claim)
    claim_source = os.path.join(root, "claim-source")
    CORE.write(claim_source, (CORE.GEN_C + "\n").encode("ascii"), 0o600)
    os.link(claim_source, claim)
    rc, answer = invoke(EV.doctor.action_complete_gc, port, CORE.GEN_C)
    check(rc == 1 and answer["result"] == "refused"
          and os.path.isdir(generation), "hardlinked gc claim was trusted")

    # Mid-cleanup failure is a successful atomic detach with an explicit,
    # resumable quarantine; it never emits a refusal after partial mutation.
    port = make_fixture(root, "partial-discard")
    target_rel = ".nxruntime/staging/partial"
    target = os.path.join(port, target_rel)
    CORE.write(os.path.join(target, "a.bin"), b"a")
    CORE.write(os.path.join(target, "z.bin"), b"z")
    real_unlink = EV.doctor.os.unlink

    def fail_late_unlink(path, *args, **kwargs):
        if ".nxdoctor-discard-" in str(path) \
                and os.path.basename(path) == "z.bin":
            raise OSError(errno.EBUSY, os.strerror(errno.EBUSY), path)
        return real_unlink(path, *args, **kwargs)

    EV.doctor.os.unlink = fail_late_unlink
    try:
        rc, answer = invoke(EV.doctor.action_discard_staging,
                            port, target_rel)
    finally:
        EV.doctor.os.unlink = real_unlink
    quarantine = os.path.join(port, ".nxruntime",
                              answer.get("quarantine_path", "missing"))
    check(rc == 0 and answer["result"] == "ok"
          and answer.get("cleanup_pending") is True
          and not os.path.exists(target) and os.path.exists(quarantine),
          "partial discard was not converted into an atomic quarantine")

    # Filesystem inspection errors are refusals; only ENOENT is clear.
    port = make_fixture(root, "inspection-denied")
    target_rel = ".nxruntime/staging/denied"
    target = os.path.join(port, target_rel)
    CORE.write(os.path.join(target, "half.bin"), b"partial")
    real_lstat = EV.doctor.os.lstat

    def deny_target_lstat(path, *args, **kwargs):
        if os.path.abspath(path) == os.path.abspath(target):
            raise OSError(errno.EACCES, os.strerror(errno.EACCES), path)
        return real_lstat(path, *args, **kwargs)

    EV.doctor.os.lstat = deny_target_lstat
    try:
        rc, answer = invoke(EV.doctor.action_discard_staging,
                            port, target_rel)
    finally:
        EV.doctor.os.lstat = real_lstat
    check(rc == 1 and answer["result"] == "refused"
          and os.path.isdir(target),
          "EACCES was reported as already-clear")

    real_json_loads = EV.json.loads
    EV.json.loads = lambda *_args, **_kwargs: (_ for _ in ()).throw(
        RecursionError("synthetic nesting limit"))
    try:
        expect_error(lambda: EV.strict_json_bytes(b"[]", "deep-json"),
                     "deep JSON escaped fail-closed parsing", "malformed")
    finally:
        EV.json.loads = real_json_loads


def main():
    root = tempfile.mkdtemp(prefix="nxdoctor-evidence.")
    runtime = os.path.join(root, "runtime")
    os.makedirs(runtime)
    previous_runtime = os.environ.get("XDG_RUNTIME_DIR")
    os.environ["XDG_RUNTIME_DIR"] = runtime
    try:
        documents = test_real_actions(root)
        test_real_refusals(root)
        test_tamper_guards(documents)
        test_file_and_cli_guards(root, documents["restore"])
        test_core_hardening(root)
    finally:
        if previous_runtime is None:
            os.environ.pop("XDG_RUNTIME_DIR", None)
        else:
            os.environ["XDG_RUNTIME_DIR"] = previous_runtime
        shutil.rmtree(root, ignore_errors=True)
    print("evidence gate passed: actions=%d refusals=%d guards=%d "
          "owner_preserved=%d physical_pending=%d"
          % (CHECKS["actions"], CHECKS["refusals"], CHECKS["guards"],
             CHECKS["owner_preserved"], CHECKS["physical_pending"]))


if __name__ == "__main__":
    main()
