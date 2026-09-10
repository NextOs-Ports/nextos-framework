#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Gate for nxdoctor: synthetic port trees, read-only proof, action guards,
clock independence and owner-data preservation."""

import contextlib
import hashlib
import importlib.util
import errno
import io
import json
import shutil
import os
import pathlib
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "nxdoctor.py")

GEN_A = "a" * 32   # active
GEN_B = "b" * 32   # previous_healthy
GEN_C = "c" * 32   # unreferenced complete (gc candidate)
GEN_D = "d" * 32   # corrupt variations
GEN_E = "e" * 32   # incomplete (missing commit)
GEN_F = "f" * 32   # symlinked dir
GEN_9 = "9" * 32   # owner-data planted
GEN_7 = "7" * 32   # delete denied half way through

CHECKS = {"guards": 0}


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]


def load_module():
    spec = importlib.util.spec_from_file_location("nxdoctor", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ND = load_module()


def fail(message):
    print("FAIL: %s" % message)
    sys.exit(1)


def check(condition, message):
    if not condition:
        fail(message)


def guard(condition, message):
    check(condition, message)
    CHECKS["guards"] += 1


def sha256_bytes(value):
    return hashlib.sha256(value).hexdigest()


def tree_hash(root):
    """Deterministic hash of names, types, modes and contents (no mtimes)."""
    digest = hashlib.sha256()
    entries = []
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames.sort()
        for name in sorted(dirnames + filenames):
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, root)
            info = os.lstat(path)
            kind = ("l" if stat.S_ISLNK(info.st_mode)
                    else "d" if stat.S_ISDIR(info.st_mode) else "f")
            row = [rel, kind, oct(stat.S_IMODE(info.st_mode))]
            if kind == "f":
                with open(path, "rb") as stream:
                    row.append(sha256_bytes(stream.read()))
            elif kind == "l":
                row.append(os.readlink(path))
            entries.append("\x00".join(row))
    for entry in sorted(entries):
        digest.update(entry.encode("utf-8") + b"\n")
    return digest.hexdigest()


def write(path, data, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if isinstance(data, str):
        data = data.encode("utf-8")
    with open(path, "wb") as stream:
        stream.write(data)
    os.chmod(path, mode)


def make_generation(port, gid, files, commit=True, break_hash=None,
                    manifest_bytes=None):
    gen = os.path.join(port, ".nxruntime", "generations", gid)
    components = {}
    for rel, (data, mode) in files.items():
        write(os.path.join(gen, rel), data, mode)
        components[rel] = {"sha256": sha256_bytes(data.encode("utf-8")
                                                  if isinstance(data, str)
                                                  else data),
                           "mode": mode}
    if break_hash:
        components[break_hash]["sha256"] = "0" * 64
    manifest = {"schema": "nxruntime-generation-v1", "schema_version": 1,
                "generation_id": gid, "components": components,
                "created_note": "synthetic"}
    if manifest_bytes is None:
        manifest_bytes = json.dumps(manifest).encode("utf-8")
    write(os.path.join(gen, "manifest.json"), manifest_bytes)
    if commit:
        write(os.path.join(gen, "commit"), gid + "\n")
    return gen


def write_state(port, active, pending, previous, seq=5, failures=0):
    state = {"schema": "nxruntime-state-v1", "schema_version": 1,
             "active": active, "pending": pending,
             "previous_healthy": previous, "activation_seq": seq,
             "prehealth_failures": failures}
    write(os.path.join(port, ".nxruntime", "state.json"),
          json.dumps(state) + "\n")


GPTK_OK = ("# synthetic controllers\n"
           "format = NEXTOS_CONTROLLERS/1\n"
           "[menu]\nA = enter\nB = back\n"
           "[gameplay]\nLEFT_ANALOG = mouse_movement\n"
           "[cursor]\nA = mouse_left\n"
           "[camera]\nRIGHT_ANALOG = arrow_keys\n")
SETTINGS_OK = ("# NEXTOS_SETTINGS/1\n"
               "language=auto\nquality=high\n")


def make_generation_v2(port, gid, files):
    """Build the shape nxbootstrap 0.6.36+ actually writes."""
    gen = os.path.join(port, ".nxruntime", "generations", gid)
    records = []
    for rel, (data, mode) in sorted(files.items()):
        write(os.path.join(gen, rel), data, int(mode, 8))
        payload = data.encode("utf-8") if isinstance(data, str) else data
        if rel.startswith("files/launcher/"):
            role, path = "launcher", rel[len("files/launcher/"):]
        elif rel == "files/nxport.json":
            role, path = "nxport", "nxport.json"
        else:
            role, path = "executable", rel[len("files/runtime/"):]
        records.append({"role": role, "path": path, "mode": mode,
                        "sha256": sha256_bytes(payload)})
    manifest = {"schema": "nxruntime-generation-v2", "schema_version": 2,
                "generation_id": gid, "components": records}
    write(os.path.join(gen, "manifest.json"),
          json.dumps(manifest).encode("utf-8"))
    write(os.path.join(gen, "commit"), gid + "\n")
    return gen


def write_state_v2(port, active, pending, previous, seq=7, failures=0,
                   failure_generation="null", last_health_run_id="null"):
    state = {"schema": "nxruntime-state-v2", "schema_version": 2,
             "active": active, "pending": pending,
             "previous_healthy": previous, "activation_seq": seq,
             "prehealth_failures": failures,
             "failure_generation": failure_generation,
             "last_health_run_id": last_health_run_id}
    write(os.path.join(port, ".nxruntime", "state.json"),
          json.dumps(state) + "\n")


def make_port(base):
    port = os.path.join(base, "demo-port")
    write(os.path.join(port, "gamedata", "README.txt"), "put game data here\n")
    write(os.path.join(port, "gamedata", "level1.bin"), b"\x01\x02payload")
    write(os.path.join(port, "NEXTOSCONTROLLERS.gptk"), GPTK_OK)
    write(os.path.join(port, "NEXTOSSETTINGS.txt"), SETTINGS_OK)
    write(os.path.join(port, "log.txt"), "hello\n")
    write(os.path.join(port, "events.jsonl"), "{}\n")
    write(os.path.join(port, "demo-port-launcher-error.1.log"), "boom\n")
    make_generation(port, GEN_A,
                    {"bin/game": ("GAME-A", 0o755),
                     "lib/libx.so": ("LIBX-A", 0o644)})
    make_generation(port, GEN_B,
                    {"bin/game": ("GAME-B", 0o755),
                     "lib/libx.so": ("LIBX-B", 0o644)})
    write_state(port, GEN_A, None, GEN_B)
    return port


def run_cli(port, extra, env_lock):
    env = dict(os.environ)
    env["XDG_RUNTIME_DIR"] = env_lock
    proc = subprocess.run([sys.executable, TOOL, port] + extra,
                          capture_output=True, text=True, env=env)
    return proc


def call(port, argv, env_lock):
    """Call main() in-process, capturing stdout; returns (rc, out)."""
    old = os.environ.get("XDG_RUNTIME_DIR")
    os.environ["XDG_RUNTIME_DIR"] = env_lock
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            rc = ND.main([port] + argv)
    finally:
        if old is None:
            os.environ.pop("XDG_RUNTIME_DIR", None)
        else:
            os.environ["XDG_RUNTIME_DIR"] = old
    return rc, buf.getvalue()


def last_receipt(out):
    lines = [line for line in out.strip().splitlines() if line]
    return json.loads(lines[-1])


def owner_hash(port):
    pieces = [tree_hash(os.path.join(port, "gamedata"))]
    for name in ("NEXTOSCONTROLLERS.gptk", "NEXTOSSETTINGS.txt"):
        with open(os.path.join(port, name), "rb") as stream:
            pieces.append(sha256_bytes(stream.read()))
    return "|".join(pieces)


def main():
    base = tempfile.mkdtemp(prefix="nxdoctor-gate-")
    lockbase = os.path.join(base, "runtime")
    os.makedirs(lockbase)
    try:
        run_gate(base, lockbase)
    finally:
        shutil.rmtree(base, ignore_errors=True)


def run_gate(base, lockbase):
    # ------------------------------------------------------------------
    # 1. healthy two-generation tree: report + read-only proof
    # ------------------------------------------------------------------
    port = make_port(base)
    before = tree_hash(port)
    proc = run_cli(port, ["--json"], lockbase)
    check(proc.returncode == 0, "healthy report rc=%d stderr=%s"
          % (proc.returncode, proc.stderr))
    report = json.loads(proc.stdout)
    check(report["schema"] == "nx-doctor-report-v1", "report schema")
    state = report["state"]
    check(state["active"] == GEN_A and state["pending"] is None
          and state["previous_healthy"] == GEN_B
          and state["activation_seq"] == 5
          and state["prehealth_failures"] == 0, "state fields")
    status = {g["generation_id"]: g["status"] for g in report["generations"]}
    check(status == {GEN_A: "complete", GEN_B: "complete"},
          "healthy generation statuses: %r" % status)
    check(report["gamedata"]["gamedata_present"]
          and report["gamedata"]["readme_present"], "gamedata section")
    check(report["controllers_gptk"]["status"] == "ok", "gptk ok")
    check(report["settings"]["status"] == "ok", "settings ok")
    check(any(l["name"] == "log.txt" for l in report["logs"])
          and any(l["name"] == "demo-port-launcher-error.1.log"
                  for l in report["logs"]), "log inventory")
    check(report["space"]["free_bytes"] > 0, "free bytes")
    check(os.path.abspath(port) not in proc.stdout, "no absolute paths")
    # human mode too
    proc2 = run_cli(port, [], lockbase)
    check(proc2.returncode == 0 and "active=%s" % GEN_A in proc2.stdout,
          "human report")
    check(tree_hash(port) == before, "read-only run modified the tree")
    readonly_ok = 1

    # legacy port (no state.json)
    legacy = os.path.join(base, "legacy-port")
    write(os.path.join(legacy, "gamedata", "README.txt"), "x\n")
    rc, out = call(legacy, ["--json"], lockbase)
    check(rc == 0 and json.loads(out)["state"]["mode"] == "legacy",
          "legacy report")

    # ------------------------------------------------------------------
    # 2. corrupt / incomplete detection with INVERTED mtimes
    # ------------------------------------------------------------------
    make_generation(port, GEN_D,
                    {"bin/game": ("GAME-D", 0o755)}, break_hash="bin/game")
    make_generation(port, GEN_E,
                    {"bin/game": ("GAME-E", 0o755)}, commit=False)
    badman = os.path.join(base, "badman-port")
    write(os.path.join(badman, "gamedata", "README.txt"), "x\n")
    make_generation(badman, GEN_D, {"bin/game": ("X", 0o755)},
                    manifest_bytes=b"{not json")
    write_state(badman, GEN_D, None, None)
    # invert mtimes: the OLDER generation (A, active) gets the NEWEST stamp,
    # broken ones get the oldest -- statuses must not change.
    gens = os.path.join(port, ".nxruntime", "generations")
    os.utime(os.path.join(gens, GEN_A), (9999999999, 9999999999))
    os.utime(os.path.join(gens, GEN_A, "commit"), (9999999999, 9999999999))
    os.utime(os.path.join(gens, GEN_D), (1000, 1000))
    os.utime(os.path.join(gens, GEN_E), (2000, 2000))
    os.utime(os.path.join(gens, GEN_B), (1500, 1500))
    rc, out = call(port, ["--json"], lockbase)
    report = json.loads(out)
    status = {g["generation_id"]: g["status"] for g in report["generations"]}
    check(status[GEN_A] == "complete" and status[GEN_B] == "complete",
          "clock independence: healthy generations")
    check(status[GEN_D] == "corrupt", "hash mismatch -> corrupt")
    check(status[GEN_E] == "incomplete", "missing commit -> incomplete")
    check(report["state"]["previous_healthy"] == GEN_B,
          "previous still reported")
    rc, out = call(badman, ["--json"], lockbase)
    report = json.loads(out)
    check(report["generations"][0]["status"] == "corrupt",
          "unparseable manifest -> corrupt")
    clock_ok = 1

    # unreferenced generations listed, none deleted
    make_generation(port, GEN_C,
                    {"bin/game": ("GAME-C", 0o755)})
    rc, out = call(port, ["--json"], lockbase)
    report = json.loads(out)
    unref = {u["generation_id"]
             for u in report["space"]["unreferenced_generations"]}
    check(unref == {GEN_C, GEN_D, GEN_E}, "unreferenced list: %r" % unref)
    check(os.path.isdir(os.path.join(gens, GEN_C)), "report deleted nothing")

    # ---- chmodless media: /roms is exFAT and reads back 0777 ------------
    # Measured on the authorized device: every member of a HEALTHY generation
    # was reported "component mode mismatch" and the whole generation came
    # back "corrupt", on a port the launcher had just run successfully.
    for gid in (GEN_A, GEN_B):
        for dirpath, _dirnames, filenames in os.walk(
                os.path.join(gens, gid)):
            for name in filenames:
                os.chmod(os.path.join(dirpath, name), 0o777)
    rc, out = call(port, ["--json"], lockbase)
    chmodless = {g["generation_id"]: g
                 for g in json.loads(out)["generations"]}
    for gid in (GEN_A, GEN_B):
        entry = chmodless[gid]
        check(entry["status"] != "corrupt",
              "a healthy generation on chmodless media was called %s"
              % entry["status"])
        check(entry["components_verified"] == entry["components_total"]
              and entry["components_total"] > 0,
              "chmodless media stopped the hash verification")
        check(any("does not enforce modes" in note
                  for note in entry.get("notes", [])),
              "the chmodless property was not reported at all")
    # What still bites: a member pinned 0755 that is NOT executable in the
    # mounted view, and any mode the filesystem clearly DOES enforce.
    check(ND.classify_component_mode(0o755, 0o644) == "mismatch",
          "an enforced-mode mismatch was excused")
    # 0644 pinned, 0755 on disk: only the "the filesystem clearly DOES
    # enforce modes" rule catches this one, and it is a real mismatch.
    check(ND.classify_component_mode(0o644, 0o755) == "mismatch",
          "a mode change on a filesystem that enforces modes was excused")
    check(ND.classify_component_mode(0o755, 0o666) == "mismatch",
          "a pinned executable that is not executable was excused")
    check(ND.classify_component_mode(0o644, 0o777) == "not-enforced",
          "chmodless 0644 was not recognized")
    check(ND.classify_component_mode(0o755, 0o777) == "not-enforced",
          "chmodless 0755 was not recognized")
    # And a real tamper is still caught by the hash, which is the check that
    # matters: mode is not what a tamper breaks.
    victim = os.path.join(gens, GEN_A, "files", "runtime", "bin", "game")
    if not os.path.isfile(victim):
        for dirpath, _d, filenames in os.walk(os.path.join(gens, GEN_A)):
            if filenames:
                victim = os.path.join(dirpath, sorted(filenames)[0])
                break
    original = open(victim, "rb").read()
    with open(victim, "wb") as handle:
        handle.write(original + b"X")
    os.chmod(victim, 0o777)
    rc, out = call(port, ["--json"], lockbase)
    tampered = {g["generation_id"]: g
                for g in json.loads(out)["generations"]}[GEN_A]
    check(tampered["status"] == "corrupt",
          "a tampered member on chmodless media was not caught by the hash")
    with open(victim, "wb") as handle:
        handle.write(original)

    # ---- the rotations a port really carries ----------------------------
    # Listing only some of them understates what is on the card, which is
    # exactly what misleads whoever is diagnosing one that filled up.
    for rotation in ("events.prev.jsonl", "nxextract-detail.log",
                     "nxextract-detail.log.prev"):
        write(os.path.join(port, rotation), b"x" * 32)
    rc, out = call(port, ["--json"], lockbase)
    listed = {entry["name"] for entry in json.loads(out)["logs"]}
    check({"events.prev.jsonl", "nxextract-detail.log",
           "nxextract-detail.log.prev"} <= listed,
          "the doctor does not list the rotations a port really carries: %r"
          % sorted(listed))

    # ---- the last phase the launcher published --------------------------
    # The doctor used to list this file's SIZE and nothing else, while the
    # single most useful fact for a port that will not start sat inside it.
    # The record below is built from the LAUNCHER TEMPLATE's own printf, so a
    # format change on the writing side lands here instead of going unnoticed.
    template = (pathlib.Path(REPOSITORY) / "framework/nxbootstrap/templates"
                / "launcher.sh.in").read_text(encoding="utf-8")
    marker = '{\\"schema\\":\\"'
    check(marker in template,
          "the launcher no longer publishes a phase result the way this "
          "test reads it")
    literal = template[template.index(marker):]
    literal = literal[:literal.index('\\"pid\\":$$}') + len('\\"pid\\":$$}')]
    published = (literal.replace('\\"', '"')
                 .replace("$NXBOOTSTRAP_PHASE_SEQUENCE", "7")
                 .replace("@NXBOOTSTRAP_VERSION@", "0.7.0")
                 .replace("$NXOBS_RUN_ID", "demo-run")
                 .replace("$source", "lifecycle")
                 .replace("$phase", "runtime")
                 .replace("$boundary", "EXIT")
                 .replace("$event_status", "failed")
                 .replace("$reason_code", "6208")
                 .replace("$child_json", "42")
                 .replace("$$", "9931"))
    write(os.path.join(port, "nxphase-result.json"),
          published.encode("utf-8"))
    rc, out = call(port, ["--json"], lockbase)
    phase = json.loads(out)["phase"]
    check(phase["read"] == "ok",
          "the doctor could not read the launcher's own phase result: %r"
          % phase)
    check(phase["phase"] == "runtime" and phase["boundary"] == "EXIT"
          and phase["status"] == "failed" and phase["child_status"] == 42
          and phase["sequence"] == 7,
          "the doctor lost the published phase fields: %r" % phase)
    # `read` and `status` must stay apart: an unreadable file is not a phase
    # that failed.
    write(os.path.join(port, "nxphase-result.json"), b"{ not json")
    rc, out = call(port, ["--json"], lockbase)
    check(json.loads(out)["phase"] == {"read": "unreadable"},
          "a corrupt phase result was not reported as unreadable")
    write(os.path.join(port, "nxphase-result.json"),
          b'{"schema":"someone-elses-phase","phase":"x"}')
    rc, out = call(port, ["--json"], lockbase)
    check(json.loads(out)["phase"]["read"] == "unknown-schema",
          "a foreign phase schema was accepted")
    os.unlink(os.path.join(port, "nxphase-result.json"))
    rc, out = call(port, ["--json"], lockbase)
    check(json.loads(out)["phase"] == {"read": "absent"},
          "an absent phase result was not reported as absent")

    owner_before = owner_hash(port)
    actions_done = 0

    # ------------------------------------------------------------------
    # 3. restore-previous
    # ------------------------------------------------------------------
    rc, out = call(port, ["--restore-previous", "--generation", GEN_C],
                   lockbase)
    guard(rc == 1 and last_receipt(out)["result"] == "refused",
          "restore refused for non-previous id")
    # incomplete previous refused
    commit_b = os.path.join(gens, GEN_B, "commit")
    os.rename(commit_b, commit_b + ".away")
    rc, out = call(port, ["--restore-previous", "--generation", GEN_B],
                   lockbase)
    guard(rc == 1 and "not complete" in last_receipt(out)["reason"],
          "restore refused for incomplete previous")
    os.rename(commit_b + ".away", commit_b)
    # happy path
    rc, out = call(port, ["--restore-previous", "--generation", GEN_B],
                   lockbase)
    receipt = last_receipt(out)
    check(rc == 0 and receipt["result"] == "ok"
          and receipt["activation_seq"] == 6, "restore happy path")
    with open(os.path.join(port, ".nxruntime", "state.json"), "rb") as f:
        new_state = json.loads(f.read().decode("utf-8"))
    check(new_state["active"] == GEN_B and new_state["pending"] is None
          and new_state["activation_seq"] == 6
          and new_state["previous_healthy"] == GEN_B,
          "restored state contents")
    # idempotent rerun
    rc, out = call(port, ["--restore-previous", "--generation", GEN_B],
                   lockbase)
    check(rc == 0 and last_receipt(out)["result"] == "already-clear",
          "restore idempotent rerun")
    # A rollback that cannot persist must SAY it did not happen: dying half
    # way leaves the caller guessing whether state moved.
    write_state(port, GEN_A, None, GEN_B, seq=6)
    state_before = open(os.path.join(port, ".nxruntime", "state.json"),
                        "rb").read()
    real_replace = ND.os.replace

    def denying_replace(src, dst, *args, **kwargs):
        raise OSError(errno.EROFS, os.strerror(errno.EROFS), dst)

    ND.os.replace = denying_replace
    try:
        rc, out = call(port, ["--restore-previous", "--generation", GEN_B],
                       lockbase)
    finally:
        ND.os.replace = real_replace
    check(rc == 1 and last_receipt(out)["result"] == "refused"
          and "state could not be written" in last_receipt(out)["reason"],
          "a rollback that cannot persist is a refusal")
    check(open(os.path.join(port, ".nxruntime", "state.json"), "rb").read()
          == state_before, "the refused rollback left state untouched")
    actions_done += 1

    # ------------------------------------------------------------------
    # 4. complete-gc
    # ------------------------------------------------------------------
    rc, out = call(port, ["--complete-gc", "--generation", GEN_B], lockbase)
    guard(rc == 1 and "referenced" in last_receipt(out)["reason"],
          "gc refuses referenced generation (active/previous)")
    write_state(port, GEN_B, GEN_A, GEN_B, seq=6)
    rc, out = call(port, ["--complete-gc", "--generation", GEN_A], lockbase)
    guard(rc == 1, "gc refuses pending generation")
    rc, out = call(port, ["--complete-gc", "--generation", "../../evil"],
                   lockbase)
    guard(rc == 1 and "32 or 64 lowercase hex" in last_receipt(out)["reason"],
          "gc refuses traversal id")
    # symlinked generation dir
    real = os.path.join(base, "elsewhere")
    os.makedirs(real, exist_ok=True)
    os.symlink(real, os.path.join(gens, GEN_F))
    rc, out = call(port, ["--complete-gc", "--generation", GEN_F], lockbase)
    guard(rc == 1 and "plain directory" in last_receipt(out)["reason"],
          "gc refuses symlinked generation dir")
    os.unlink(os.path.join(gens, GEN_F))
    check(os.path.isdir(real), "symlink target untouched")
    # planted owner data inside a generation
    make_generation(port, GEN_9, {"bin/game": ("GAME-9", 0o755)})
    write(os.path.join(gens, GEN_9, "bin", "game.apk"), b"APK")
    rc, out = call(port, ["--complete-gc", "--generation", GEN_9], lockbase)
    guard(rc == 1 and "owner data" in last_receipt(out)["reason"],
          "gc refuses generation holding a planted game.apk")
    check(os.path.isfile(os.path.join(gens, GEN_9, "bin", "game.apk")),
          "planted apk survived")
    os.unlink(os.path.join(gens, GEN_9, "bin", "game.apk"))
    # incomplete generation refused (might be a mid-install pending)
    rc, out = call(port, ["--complete-gc", "--generation", GEN_E], lockbase)
    guard(rc == 1 and "incomplete" in last_receipt(out)["reason"],
          "gc refuses incomplete generation")
    # happy path: exactly the unreferenced complete generation goes away
    rc, out = call(port, ["--complete-gc", "--generation", GEN_C], lockbase)
    check(rc == 0 and last_receipt(out)["result"] == "ok",
          "gc happy path")
    check(not os.path.exists(os.path.join(gens, GEN_C)), "GEN_C removed")
    for keep in (GEN_A, GEN_B, GEN_D, GEN_E, GEN_9):
        check(os.path.isdir(os.path.join(gens, keep)),
              "gc kept generation %s" % keep)
    # corrupt-but-unreferenced also collectable
    rc, out = call(port, ["--complete-gc", "--generation", GEN_D], lockbase)
    check(rc == 0 and last_receipt(out)["status_before"] == "corrupt",
          "gc collects corrupt unreferenced generation")
    # idempotent rerun
    rc, out = call(port, ["--complete-gc", "--generation", GEN_C], lockbase)
    check(rc == 0 and last_receipt(out)["result"] == "already-clear",
          "gc idempotent rerun")

    # ---- a delete the filesystem denies half way through -----------------
    # Injected in-process, because the suite may run root-mapped and root
    # walks straight through a chmod.  The tool itself carries no fault hook.
    make_generation(port, GEN_7, {"bin/game": ("GAME-7", 0o755),
                                  "lib/a.so": ("LIB-7", 0o644)})
    target7 = os.path.join(gens, GEN_7)
    check(ND.verify_generation(port, GEN_7)["status"] == "complete",
          "GEN_7 starts complete")
    # Deny a member deep in the tree: the post-order walk reaches it before
    # the generation root, which is the only ordering that can leave `commit`
    # standing over a gutted tree.
    blocked = os.path.join(target7, "lib", "a.so")
    real_unlink = ND.os.unlink

    def denying_unlink(path):
        if os.path.realpath(path) == os.path.realpath(blocked):
            raise OSError(errno.EROFS, os.strerror(errno.EROFS), path)
        return real_unlink(path)

    ND.os.unlink = denying_unlink
    try:
        rc, out = call(port, ["--complete-gc", "--generation", GEN_7],
                       lockbase)
    finally:
        ND.os.unlink = real_unlink
    receipt7 = last_receipt(out)
    check(rc == 1 and receipt7["result"] == "refused"
          and "could not be removed" in receipt7["reason"]
          and "read-only" in receipt7["reason"].lower()
          and receipt7["path"] == "a.so",
          "gc reports a denied delete as a refusal naming the entry")
    check(os.path.isdir(target7), "the denied generation still exists")
    # The load-bearing half: creation writes `commit` last, so the aborted
    # deletion must have removed it first.  A gutted tree still carrying its
    # completion marker is exactly what the launcher would run.
    check(not os.path.exists(os.path.join(target7, "commit")),
          "the aborted delete dropped commit first")
    check(ND.verify_generation(port, GEN_7)["status"] != "complete",
          "a half-deleted generation never reads as complete")
    check(os.path.isfile(os.path.join(target7, ND.GC_CLAIM_NAME)),
          "the aborted delete left its claim behind")
    # ... and the next pass finishes it, which only works because the claim
    # survives: an incomplete generation is otherwise never collected.
    rc, out = call(port, ["--complete-gc", "--generation", GEN_7], lockbase)
    receipt7 = last_receipt(out)
    check(rc == 0 and receipt7["result"] == "ok"
          and receipt7["status_before"] == "gc-interrupted",
          "the next pass finishes the interrupted collection")
    check(not os.path.exists(target7), "GEN_7 finally removed")
    check(owner_hash(port) == owner_before, "owner data survived the denied gc")

    # ---- and the denial that lands on the generation root itself ---------
    # Here the walk has already emptied the root, so the claim is the last
    # thing standing between a resumable collection and permanently
    # uncollectable garbage: `commit` is gone, so nothing would ever agree to
    # finish the job.
    make_generation(port, GEN_7, {"bin/game": ("GAME-7", 0o755)})
    real_rmdir = ND.os.rmdir

    def denying_rmdir(path):
        if os.path.realpath(path) == os.path.realpath(target7):
            raise OSError(errno.EBUSY, os.strerror(errno.EBUSY), path)
        return real_rmdir(path)

    ND.os.rmdir = denying_rmdir
    try:
        rc, out = call(port, ["--complete-gc", "--generation", GEN_7],
                       lockbase)
    finally:
        ND.os.rmdir = real_rmdir
    receipt7 = last_receipt(out)
    check(rc == 1 and receipt7["result"] == "refused"
          and receipt7["path"] == GEN_7,
          "gc reports a denied root removal naming the generation")
    check(os.path.isfile(os.path.join(target7, ND.GC_CLAIM_NAME)),
          "the claim outlives every other member of the tree")
    check(sorted(os.listdir(target7)) == [ND.GC_CLAIM_NAME],
          "nothing but the claim survived the emptied root")
    rc, out = call(port, ["--complete-gc", "--generation", GEN_7], lockbase)
    check(rc == 0 and last_receipt(out)["status_before"] == "gc-interrupted",
          "an emptied generation is still finishable")
    check(not os.path.exists(target7), "the emptied generation was removed")
    actions_done += 1

    # ------------------------------------------------------------------
    # 5. discard-staging
    # ------------------------------------------------------------------
    staging = os.path.join(port, ".nxruntime", "staging")
    write(os.path.join(staging, "pkg", "half.bin"), b"junk")
    rc, out = call(port, ["--discard-staging", "--path", "gamedata"],
                   lockbase)
    guard(rc == 1 and "outside" in last_receipt(out)["reason"],
          "discard refuses a path outside staging")
    rc, out = call(port, ["--discard-staging", "--path",
                          ".nxruntime/staging/../generations"], lockbase)
    guard(rc == 1 and "unsafe" in last_receipt(out)["reason"],
          "discard refuses traversal")
    os.symlink(os.path.join(port, "gamedata"),
               os.path.join(staging, "sneaky"))
    rc, out = call(port, ["--discard-staging", "--path",
                          ".nxruntime/staging/sneaky"], lockbase)
    guard(rc == 1 and "symlink" in last_receipt(out)["reason"],
          "discard refuses a symlink target")
    os.unlink(os.path.join(staging, "sneaky"))
    # staged dir that looks like a committed generation
    fake_gen = os.path.join(staging, "gen")
    write(os.path.join(fake_gen, "manifest.json"), b"{}")
    write(os.path.join(fake_gen, "commit"), b"x\n")
    rc, out = call(port, ["--discard-staging", "--path",
                          ".nxruntime/staging/gen"], lockbase)
    guard(rc == 1 and "committed generation" in last_receipt(out)["reason"],
          "discard refuses a committed generation")
    # owner-data pattern inside staging
    write(os.path.join(staging, "pkg2", "save.dat"), b"S")
    rc, out = call(port, ["--discard-staging", "--path",
                          ".nxruntime/staging/pkg2"], lockbase)
    guard(rc == 1 and "owner data" in last_receipt(out)["reason"],
          "discard refuses owner-data patterns")
    # hardlinked file
    write(os.path.join(staging, "linked.bin"), b"L")
    os.link(os.path.join(staging, "linked.bin"),
            os.path.join(port, "gamedata", "hardlink-peer"))
    rc, out = call(port, ["--discard-staging", "--path",
                          ".nxruntime/staging/linked.bin"], lockbase)
    guard(rc == 1 and "multiply-linked" in last_receipt(out)["reason"],
          "discard refuses hardlink>1")
    os.unlink(os.path.join(port, "gamedata", "hardlink-peer"))
    rc, out = call(port, ["--discard-staging", "--path",
                          ".nxruntime/staging/linked.bin"], lockbase)
    check(rc == 0, "discard file happy path")
    # happy path dir + idempotent rerun
    rc, out = call(port, ["--discard-staging", "--path",
                          ".nxruntime/staging/pkg"], lockbase)
    check(rc == 0 and last_receipt(out)["result"] == "ok"
          and not os.path.exists(os.path.join(staging, "pkg")),
          "discard dir happy path")
    rc, out = call(port, ["--discard-staging", "--path",
                          ".nxruntime/staging/pkg"], lockbase)
    check(rc == 0 and last_receipt(out)["result"] == "already-clear",
          "discard idempotent rerun")
    # A denied delete here has the same shape as in complete-gc, and used to
    # have the same defect: an OSError straight out of the tool.
    write(os.path.join(staging, "denied", "half.bin"), b"junk")
    blocked_dir = os.path.join(staging, "denied", "half.bin")
    real_unlink = ND.os.unlink

    def denying_unlink(path, *args, **kwargs):
        if ".nxdoctor-discard-" in str(path) \
                and os.path.basename(path) == "half.bin":
            raise OSError(errno.EROFS, os.strerror(errno.EROFS), path)
        return real_unlink(path, *args, **kwargs)

    ND.os.unlink = denying_unlink
    try:
        rc, out = call(port, ["--discard-staging", "--path",
                              ".nxruntime/staging/denied"], lockbase)
    finally:
        ND.os.unlink = real_unlink
    receipt_denied = last_receipt(out)
    check(rc == 0 and receipt_denied["result"] == "ok"
          and receipt_denied.get("cleanup_pending") is True,
          "discard did not report its atomically detached quarantine")
    check(not os.path.exists(os.path.dirname(blocked_dir)),
          "the detached staging target remained reachable")
    rc, out = call(port, ["--discard-staging", "--path",
                          ".nxruntime/staging/denied"], lockbase)
    check(rc == 0 and last_receipt(out)["result"] == "already-clear",
          "detached discard remains idempotently clear")
    # A single regular staging file takes the same path.
    lone = os.path.join(staging, "lone.bin")
    write(lone, b"junk")
    def deny_lone_cleanup(path, *args, **kwargs):
        if ".nxdoctor-discard-" in str(path):
            raise OSError(errno.EACCES, os.strerror(errno.EACCES), path)
        return real_unlink(path, *args, **kwargs)

    ND.os.unlink = deny_lone_cleanup
    try:
        rc, out = call(port, ["--discard-staging", "--path",
                              ".nxruntime/staging/lone.bin"], lockbase)
    finally:
        ND.os.unlink = real_unlink
    check(rc == 0 and last_receipt(out)["result"] == "ok"
          and last_receipt(out).get("cleanup_pending") is True
          and not os.path.exists(lone),
          "a denied single-file cleanup was not quarantined")
    actions_done += 1

    # ------------------------------------------------------------------
    # 6. clear-lock
    # ------------------------------------------------------------------
    lock_dir = os.path.join(lockbase, ".nxbootstrap-%d" % os.getuid(),
                            "nxport-%s.flock.d" % os.path.basename(port))
    dead_pid = 4194000
    while os.path.exists("/proc/%d" % dead_pid):
        dead_pid -= 1
    # alive owner refused
    my_pid = os.getpid()
    my_start = ND.proc_starttime(my_pid)
    write(os.path.join(lock_dir, "owner"), "%d %d\n" % (my_pid, my_start))
    rc, out = call(port, ["--clear-lock", "--pid", str(my_pid)], lockbase)
    guard(rc == 1 and "alive" in last_receipt(out)["reason"],
          "clear-lock refuses a live owner")
    # mismatched pid refused
    rc, out = call(port, ["--clear-lock", "--pid", str(dead_pid)], lockbase)
    guard(rc == 1 and "does not match" in last_receipt(out)["reason"],
          "clear-lock refuses a pid mismatch")
    # starttime differs -> pid considered reused -> clearable
    write(os.path.join(lock_dir, "owner"),
          "%d %d\n" % (my_pid, my_start + 777))
    rc, out = call(port, ["--clear-lock", "--pid", str(my_pid)], lockbase)
    check(rc == 0 and last_receipt(out)["result"] == "ok",
          "clear-lock on a reused pid (starttime differs)")
    # dead pid happy path
    write(os.path.join(lock_dir, "owner"), "%d 1\n" % dead_pid)
    rc, out = call(port, ["--clear-lock", "--pid", str(dead_pid)], lockbase)
    check(rc == 0 and last_receipt(out)["result"] == "ok"
          and not os.path.exists(lock_dir), "clear-lock dead-pid happy path")
    rc, out = call(port, ["--clear-lock", "--pid", str(dead_pid)], lockbase)
    check(rc == 0 and last_receipt(out)["result"] == "already-clear",
          "clear-lock idempotent rerun")
    # The base launcher actually writes pid=<pid> token=<run-id>, not a
    # starttime record. A dead owner is provable and clearable; a live PID is
    # refused because token-only ownership cannot prove PID reuse.
    write(os.path.join(lock_dir, "owner"),
          "pid=%d token=dead-launcher-run\n" % dead_pid)
    rc, out = call(port, ["--clear-lock", "--pid", str(dead_pid)], lockbase)
    check(rc == 0 and last_receipt(out)["result"] == "ok",
          "clear-lock understands the launcher's token owner")
    write(os.path.join(lock_dir, "owner"),
          "pid=%d token=live-launcher-run\n" % my_pid)
    rc, out = call(port, ["--clear-lock", "--pid", str(my_pid)], lockbase)
    guard(rc == 1 and "cannot prove starttime reuse" in
          last_receipt(out)["reason"],
          "clear-lock refuses a live token-only owner")
    shutil.rmtree(lock_dir)
    # Third instance of the same shape. A lock that cannot be cleared is a
    # refusal naming the entry, never a traceback -- the caller is usually a
    # player whose port refuses to launch.
    write(os.path.join(lock_dir, "owner"), "%d 1\n" % dead_pid)
    real_rmdir = ND.os.rmdir

    def denying_rmdir(path, *args, **kwargs):
        if ".nxdoctor-cleared-lock-" in os.path.basename(path):
            raise OSError(errno.EBUSY, os.strerror(errno.EBUSY), path)
        return real_rmdir(path, *args, **kwargs)

    ND.os.rmdir = denying_rmdir
    try:
        rc, out = call(port, ["--clear-lock", "--pid", str(dead_pid)],
                       lockbase)
    finally:
        ND.os.rmdir = real_rmdir
    check(rc == 0 and last_receipt(out)["result"] == "ok"
          and last_receipt(out).get("cleanup_pending") is True
          and not os.path.exists(lock_dir),
          "a denied lock cleanup was not safely detached")
    rc, out = call(port, ["--clear-lock", "--pid", str(dead_pid)], lockbase)
    check(rc == 0 and last_receipt(out)["result"] in ("ok", "already-clear"),
          "the lock clears once the filesystem allows it")
    actions_done += 1

    # ------------------------------------------------------------------
    # 7. gptk / settings validation negatives (never rewritten)
    # ------------------------------------------------------------------
    neg = os.path.join(base, "neg-port")
    os.makedirs(neg, exist_ok=True)
    cases = [
        ("bad magic", "format = OTHER/9\n[menu]\nA = x\n", GPTK_OK),
        ("NUL byte", "format = NEXTOS_CONTROLLERS/1\n[menu]\nA = \x00\n",
         GPTK_OK),
        ("oversize", "format = NEXTOS_CONTROLLERS/1\n" + "#x\n" * 40000,
         GPTK_OK),
        ("unknown section",
         "format = NEXTOS_CONTROLLERS/1\n[weapons]\nA = x\n", GPTK_OK),
    ]
    for label, bad, _good in cases:
        write(os.path.join(neg, "NEXTOSCONTROLLERS.gptk"), bad)
        payload_before = open(os.path.join(neg, "NEXTOSCONTROLLERS.gptk"),
                              "rb").read()
        rc, out = call(neg, ["--json"], lockbase)
        report = json.loads(out)
        guard(report["controllers_gptk"]["status"] == "invalid",
              "gptk negative not caught: %s" % label)
        check(open(os.path.join(neg, "NEXTOSCONTROLLERS.gptk"),
                   "rb").read() == payload_before,
              "gptk was rewritten (%s)" % label)
    scases = [
        ("bad magic", "resolution=auto\n"),
        # espelho do runtime nxcompat_settings.c (auditoria V3, ponto 8):
        ("unknown key", "# NEXTOS_SETTINGS/1\nresolution=auto\n"),
        ("quality off allowlist", "# NEXTOS_SETTINGS/1\nquality=ultra\n"),
        ("value bad char", "# NEXTOS_SETTINGS/1\nlanguage=pt BR\n"),
        ("duplicate known key", "# NEXTOS_SETTINGS/1\nlanguage=auto\nlanguage=en\n"),
        ("oversize", "# NEXTOS_SETTINGS/1\n" + "quality=high\n" * 4000),
        ("NUL byte", "# NEXTOS_SETTINGS/1\nlanguage=\x00\n"),
    ]
    for label, bad in scases:
        write(os.path.join(neg, "NEXTOSSETTINGS.txt"), bad)
        rc, out = call(neg, ["--json"], lockbase)
        report = json.loads(out)
        guard(report["settings"]["status"] == "invalid",
              "settings negative not caught: %s" % label)

    # ------------------------------------------------------------------
    # 8. owner data byte-for-byte across every action
    # ------------------------------------------------------------------
    check(owner_hash(port) == owner_before,
          "owner data changed across the action battery")
    owner_ok = 1

    # ------------------------------------------------------------------
    # 9. interruption safety of the atomic state write
    # ------------------------------------------------------------------
    state_path = os.path.join(port, ".nxruntime", "state.json")
    with open(state_path, "rb") as stream:
        state_before = stream.read()
    real_replace = ND.os.replace

    def broken_replace(src, dst, *args, **kwargs):
        raise OSError("simulated power loss")

    ND.os.replace = broken_replace
    try:
        failed = False
        try:
            ND.write_state_atomic(port, {"probe": True}, state_before)
        except OSError:
            failed = True
        check(failed, "broken replace did not raise")
    finally:
        ND.os.replace = real_replace
    with open(state_path, "rb") as stream:
        check(stream.read() == state_before,
              "state.json changed on interrupted write")
    leftovers = [n for n in os.listdir(os.path.join(port, ".nxruntime"))
                 if "nxdoctor-tmp" in n]
    check(leftovers == [], "temp file leaked: %r" % leftovers)
    # idempotent rerun after the simulated interruption
    rc, out = call(port, ["--restore-previous", "--generation", GEN_B],
                   lockbase)
    check(rc == 0, "rerun after interruption")

    # --- the shapes a REAL port actually writes ------------------------
    # nxbootstrap has written state-v2 since 0.6.35 and generation-v2 since
    # 0.6.36, with the complete 64-hex SHA-256 as the generation id. The doctor
    # was pinned to v1 and to 32-hex ids, so on every port it exists to
    # diagnose it read state as "unknown schema", every generation manifest as
    # corrupt, and refused to collect any real generation id. These cases fix
    # that in place: a modern port must be fully readable.
    modern = tempfile.mkdtemp(prefix="nxdoctor-modern.")
    try:
        port2 = make_port(modern)
        long_a = "a1" * 32
        long_b = "b2" * 32
        require_v2 = {
            "files/launcher/Fixture.sh": ("#!/bin/bash\nexit 0\n", "0755"),
            "files/nxport.json": ('{"id":"fixture"}\n', "0644"),
            "files/runtime/fixture-loader": ("#!/bin/bash\nexit 0\n", "0755"),
        }
        long_c = "c3" * 32
        make_generation_v2(port2, long_a, require_v2)
        make_generation_v2(port2, long_b, require_v2)
        make_generation_v2(port2, long_c, require_v2)
        write_state_v2(port2, long_a, "null", long_b)

        lock2 = os.path.join(modern, "runtime")
        os.makedirs(lock2, exist_ok=True)
        rc, out = call(port2, ["--json"], lock2)
        check(rc == 0, "the doctor refused a modern v2 port: rc=%d out=%r"
              % (rc, out[:400]))
        check(out.strip().startswith("{"),
              "the doctor produced no report for a v2 port: %r" % out[:400])
        report = json.loads(out)
        state_report = report["state"]
        check(state_report.get("problem") is None,
              "the doctor could not read state-v2: %r"
              % state_report.get("problem"))
        check(state_report.get("active") == long_a,
              "the doctor lost the active generation of a v2 port: %r"
              % state_report)
        # The launcher writes the literal string "null" for an unset slot.
        check(state_report.get("pending") is None,
              "the doctor did not normalize the literal null the launcher "
              "really writes: %r" % state_report.get("pending"))
        check(state_report.get("previous_healthy") == long_b,
              "the doctor lost previous_healthy on a v2 port")
        check(state_report.get("failure_generation") is None
              and state_report.get("last_health_run_id") == "null",
              "the doctor lost the real state-v2 health fields")
        by_id = {item["generation_id"]: item
                 for item in report["generations"]}
        # make_port() also seeds two legacy v1 generations; the v2 pair must be
        # present alongside them, and both shapes must read correctly.
        check({long_a, long_b} <= set(by_id),
              "the doctor did not enumerate both v2 generations: %r"
              % sorted(by_id))
        for gid in (long_a, long_b):
            item = by_id[gid]
            check(item["manifest_ok"] is True,
                  "the doctor read a valid generation-v2 manifest as corrupt "
                  "(%s): %r" % (gid, item["problems"]))
            check(item["status"] == "complete",
                  "a complete generation-v2 was reported as %r"
                  % item["status"])
            check(item["components_total"] == len(require_v2),
                  "the doctor verified the wrong component count")

        # A corrupted byte inside a v2 generation must still be caught: the
        # normalization must not turn verification into a no-op.
        broken = os.path.join(port2, ".nxruntime", "generations", long_b,
                              "files", "runtime", "fixture-loader")
        write(broken, "#!/bin/bash\nexit 1\n", 0o755)
        rc, out = call(port2, ["--json"], lock2)
        check(rc == 0, "the doctor refused a port with a corrupt generation")
        by_id = {item["generation_id"]: item
                 for item in json.loads(out)["generations"]}
        check(by_id[long_b]["status"] == "corrupt",
              "a tampered generation-v2 member was not detected")
        check(by_id[long_a]["status"] == "complete",
              "the healthy generation was collaterally condemned")

        # complete-gc must accept a real 64-hex id. It previously refused every
        # modern id as "not 32 lowercase hex", which made the action dead.
        write_state_v2(port2, long_a, "null", long_b,
                       failure_generation=long_c,
                       last_health_run_id="health-run-1")
        rc, out = call(port2,
                       ["--json", "--complete-gc", "--generation", long_c],
                       lock2)
        check(rc == 1 and last_receipt(out).get("result") == "refused"
              and os.path.isdir(os.path.join(
                  port2, ".nxruntime", "generations", long_c)),
              "complete-gc ignored state-v2 failure_generation")
        write_state_v2(port2, long_a, "null", long_b)
        rc, out = call(port2,
                       ["--json", "--complete-gc", "--generation", long_c],
                       lock2)
        receipt_line = last_receipt(out)
        check(rc == 0,
              "complete-gc refused a real 64-hex generation id: rc=%d %r"
              % (rc, receipt_line))
        check(receipt_line.get("result") == "ok",
              "complete-gc did not collect an unreferenced generation: %r"
              % receipt_line)
        check(not os.path.exists(os.path.join(
            port2, ".nxruntime", "generations", long_c)),
            "complete-gc reported ok without collecting")
        # Referenced generations are still refused: active and previous.
        for referenced in (long_a, long_b):
            rc, out = call(
                port2,
                ["--json", "--complete-gc", "--generation", referenced], lock2)
            check(last_receipt(out).get("result") == "refused",
                  "complete-gc collected a REFERENCED generation (%s)"
                  % referenced)
            check(os.path.isdir(os.path.join(
                port2, ".nxruntime", "generations", referenced)),
                "a referenced generation was removed")
        # The visible seed is normally the largest file in a V4 port; a space
        # report that omits it misleads whoever is diagnosing a full card.
        seed_name = "nxruntime-" + long_a + ".nxb"
        write(os.path.join(port2, seed_name), b"S" * 4096)
        rc, out = call(port2, ["--json"], lock2)
        check(rc == 0, "the doctor refused a port carrying a seed")
        seeds = json.loads(out)["space"].get("seeds")
        check(isinstance(seeds, list) and len(seeds) == 1,
              "the doctor did not report the visible seed: %r" % seeds)
        check(seeds[0]["name"] == seed_name and seeds[0]["bytes"] == 4096
              and seeds[0]["regular"] is True
              and seeds[0]["generation_id"] == long_a,
              "the seed report lost its identity or size: %r" % seeds[0])
        # A non-regular seed is named as such instead of being sized.
        os.unlink(os.path.join(port2, seed_name))
        os.symlink("/etc/hostname", os.path.join(port2, seed_name))
        rc, out = call(port2, ["--json"], lock2)
        seeds = json.loads(out)["space"]["seeds"]
        check(seeds[0]["regular"] is False,
              "a symlinked seed was reported as a regular file")
        os.unlink(os.path.join(port2, seed_name))

        check(owner_hash(port2) is not None,
              "owner data did not survive the modern-port cases")
    finally:
        shutil.rmtree(modern, ignore_errors=True)

    print("nxdoctor gate passed: readonly=%d actions=%d guards=%d "
          "clock_independent=%d owner_preserved=%d"
          % (readonly_ok, actions_done, CHECKS["guards"], clock_ok,
             owner_ok))


if __name__ == "__main__":
    main()
