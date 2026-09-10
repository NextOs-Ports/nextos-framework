#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Directed cases for nxledger 0.2.3 over temporary Git fixture repos.

Registered in ``framework/tests/test-matrix-v1.json`` (gate ``nxledger-host``).
Covers the complete authority inventory (Part A), the one-shot exact-identity
enforcement (Part B) and the DEV/PUBLIC-FINAL profiles (Part C).
"""

import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL_DIR = os.path.dirname(HERE)
TOOL = os.path.join(TOOL_DIR, "nxledger.py")

sys.path.insert(0, TOOL_DIR)
import nxledger  # noqa: E402


def clean_env():
    env = dict(os.environ)
    env.pop("SOURCE_DATE_EPOCH", None)
    env["GIT_CONFIG_GLOBAL"] = os.devnull
    env["GIT_CONFIG_SYSTEM"] = os.devnull
    env["GIT_AUTHOR_NAME"] = "Fixture"
    env["GIT_AUTHOR_EMAIL"] = "fixture@invalid"
    env["GIT_COMMITTER_NAME"] = "Fixture"
    env["GIT_COMMITTER_EMAIL"] = "fixture@invalid"
    env["GIT_AUTHOR_DATE"] = "2026-08-31T12:00:00 +0000"
    env["GIT_COMMITTER_DATE"] = "2026-08-31T12:00:00 +0000"
    return env


def git(repo, *args, env=None):
    subprocess.run(("git", "-C", repo) + args, check=True,
                   env=env or clean_env(),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def run_tool(*args, env=None, tool=TOOL):
    return subprocess.run(
        (sys.executable, "-B", tool) + args, env=env or clean_env(),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def read_bytes(path):
    with open(path, "rb") as fh:
        return fh.read()


def write_json(path, payload):
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, sort_keys=True)
        fh.write("\n")
    return path


class Fixture:
    """A minimal repository carrying every allowlisted identity source."""

    def __init__(self, base):
        self.root = os.path.join(base, "repo")
        os.makedirs(self.root)
        git(self.root, "init", "-q", "-b", "fixture/main")
        for name, rel in nxledger.AUTHORITY_VERSION_FILES:
            self.write(rel, "0.1.%d\n" % len(name))
        self.write(nxledger.DECLARATIVE_CONTRACT_PATH, json.dumps({
            "schema_version": 1,
            "contract_version": "9.9.9",
        }) + "\n")
        self.write(".gitignore", "/ignored-out/\n")
        os.makedirs(os.path.join(self.root, "ignored-out"), exist_ok=True)
        self.commit("fixture: base")

    def write(self, rel, content):
        path = os.path.join(self.root, rel)
        os.makedirs(os.path.dirname(path) or self.root, exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(content)

    def commit(self, message):
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", message)


class Base(unittest.TestCase):
    def setUp(self):
        self.base = os.path.realpath(tempfile.mkdtemp(prefix="nxledger-fx."))
        self.addCleanup(shutil.rmtree, self.base, ignore_errors=True)
        self.fx = Fixture(self.base)

    def derive_json(self, *args, env=None):
        proc = run_tool("--repo", self.fx.root, *args, env=env)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertNotIn(b"Traceback", proc.stderr)
        return json.loads(proc.stdout.decode("ascii")), proc.stdout


class DerivationCases(Base):
    def test_clean_branch_head(self):
        ledger, _ = self.derive_json()
        self.assertEqual(ledger["schema"], "org.nextos.v4.ledger")
        self.assertEqual(ledger["schema_version"], 2)
        self.assertEqual(ledger["repo"]["branch"], "fixture/main")
        self.assertFalse(ledger["repo"]["detached"])
        self.assertFalse(ledger["repo"]["dirty"])
        self.assertEqual(ledger["tags_at_head"], [])
        self.assertNotIn("generated_epoch", ledger)

    def test_authority_count_derived_from_allowlist(self):
        ledger, _ = self.derive_json()
        expected = {name for name, _ in nxledger.AUTHORITY_VERSION_FILES}
        self.assertEqual(set(ledger["components"]), expected)
        self.assertEqual(len(ledger["components"]),
                         nxledger.EXPECTED_AUTHORITY_COUNT)
        for name in ("portmaster_contract", "apkcompat", "framework_tests",
                     "nxextract"):
            self.assertIn(name, ledger["components"])

    def test_detached_head(self):
        git(self.fx.root, "checkout", "-q", "--detach")
        ledger, _ = self.derive_json()
        self.assertIsNone(ledger["repo"]["branch"])
        self.assertTrue(ledger["repo"]["detached"])

    def test_dirty_tree(self):
        self.fx.write("untracked-note.txt", "dirty\n")
        ledger, _ = self.derive_json()
        self.assertTrue(ledger["repo"]["dirty"])

    def test_deterministic_two_runs(self):
        _, first = self.derive_json()
        _, second = self.derive_json()
        self.assertEqual(first, second)

    def test_source_date_epoch_only_when_explicit(self):
        env = clean_env()
        env["SOURCE_DATE_EPOCH"] = "1756600000"
        ledger, _ = self.derive_json(env=env)
        self.assertEqual(ledger["generated_epoch"], 1756600000)

    def test_tags_at_head_sorted(self):
        git(self.fx.root, "tag", "b-tag")
        git(self.fx.root, "tag", "a-tag")
        ledger, _ = self.derive_json()
        self.assertEqual(ledger["tags_at_head"], ["a-tag", "b-tag"])


class AuthorityFailClosed(Base):
    """Missing, symlink, invalid, unknown-extra and stale per new domain."""

    def expect_error(self, fragment):
        proc = run_tool("--repo", self.fx.root)
        self.assertEqual(proc.returncode, 2, proc.stderr)
        self.assertNotIn(b"Traceback", proc.stderr)
        self.assertIn(fragment, proc.stderr.decode("utf-8"))

    def all_domains(self):
        return (("portmaster_contract", "framework/portmaster/VERSION"),
                ("apkcompat", "framework/contracts/apkcompat/VERSION"),
                ("framework_tests", "framework/tests/VERSION"),
                ("nxgl", "framework/nxgl/VERSION"))

    def test_missing_per_domain(self):
        for name, rel in self.all_domains():
            with self.subTest(name):
                fx = Fixture(tempfile.mkdtemp(prefix="d.", dir=self.base))
                os.unlink(os.path.join(fx.root, rel))
                proc = run_tool("--repo", fx.root)
                self.assertEqual(proc.returncode, 2)
                self.assertIn("VERSION missing",
                              proc.stderr.decode("utf-8"))

    def test_symlink_per_domain(self):
        for name, rel in self.all_domains():
            with self.subTest(name):
                fx = Fixture(tempfile.mkdtemp(prefix="s.", dir=self.base))
                path = os.path.join(fx.root, rel)
                os.unlink(path)
                os.symlink("../../nxinput/VERSION", path)
                proc = run_tool("--repo", fx.root)
                self.assertEqual(proc.returncode, 2)
                self.assertIn("symlink", proc.stderr.decode("utf-8"))

    def test_invalid_per_domain(self):
        for name, rel in self.all_domains():
            with self.subTest(name):
                fx = Fixture(tempfile.mkdtemp(prefix="i.", dir=self.base))
                fx.write(rel, "banana\n")
                proc = run_tool("--repo", fx.root)
                self.assertEqual(proc.returncode, 2)
                self.assertIn("not a version", proc.stderr.decode("utf-8"))

    def test_unknown_extra_domain_anywhere(self):
        self.fx.write("framework/newdomain/VERSION", "1.0.0\n")
        self.expect_error("unlisted versioned authority")

    def test_unknown_extra_nested_domain(self):
        self.fx.write("framework/contracts/otherpolicy/VERSION", "1.0.0\n")
        self.expect_error("unlisted versioned authority")

    def test_stale_change_detected_per_domain(self):
        for name, rel in self.all_domains():
            with self.subTest(name):
                fx = Fixture(tempfile.mkdtemp(prefix="t.", dir=self.base))
                proc = run_tool("--repo", fx.root)
                before = json.loads(proc.stdout)["components"][name]
                fx.write(rel, "9.9.9\n")
                fx.commit("bump %s" % name)
                proc = run_tool("--repo", fx.root)
                after = json.loads(proc.stdout)["components"][name]
                self.assertNotEqual(before, after)

    def test_contract_missing_field(self):
        self.fx.write(nxledger.DECLARATIVE_CONTRACT_PATH,
                      json.dumps({"schema_version": 1}) + "\n")
        self.expect_error("refusing to invent")

    def test_regression_011_missed_portmaster_bump(self):
        """Negative control: 0.1.1 could not see a portmaster-only bump."""
        old_dir = os.path.realpath(tempfile.mkdtemp(prefix="old.",
                                                    dir=self.base))
        source = subprocess.run(
            ("git", "-C", TOOL_DIR, "show",
             "59703d778e23597225ac724a9a7bfef4cb0bf3a6:"
             "framework/nxledger/nxledger.py"),
            stdout=subprocess.PIPE, check=True).stdout
        old_tool = os.path.join(old_dir, "nxledger.py")
        with open(old_tool, "wb") as fh:
            fh.write(source)
        with open(os.path.join(old_dir, "VERSION"), "w") as fh:
            fh.write("0.1.1\n")

        def components(tool):
            proc = run_tool("--repo", self.fx.root, tool=tool)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            return json.loads(proc.stdout)["components"]

        old_before = components(old_tool)
        new_before = components(TOOL)
        self.fx.write("framework/portmaster/VERSION", "9.9.9\n")
        self.fx.commit("fixture: bump portmaster only")
        old_after = components(old_tool)
        new_after = components(TOOL)
        self.assertEqual(old_before, old_after,
                         "0.1.1 was expected to MISS the portmaster bump")
        self.assertNotEqual(new_before, new_after,
                            "0.2.0 must SEE the portmaster bump")


class CheckCases(Base):
    def record(self):
        _, raw = self.derive_json()
        path = os.path.join(self.base, "recorded-ledger.json")
        with open(path, "wb") as fh:
            fh.write(raw)
        return path

    def check(self, path):
        return run_tool("--repo", self.fx.root, "--check", path)

    def divergences(self, proc):
        self.assertEqual(proc.returncode, 1, proc.stderr)
        return [line.split()[2].rstrip(":") for line in
                proc.stderr.decode("utf-8").splitlines()
                if line.startswith("nxledger: DIVERGENT")]

    def test_check_match(self):
        proc = self.check(self.record())
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn(b"nxledger: MATCH", proc.stdout)

    def test_check_stale_commit_and_tree(self):
        path = self.record()
        self.fx.write("note.txt", "advance\n")
        self.fx.commit("fixture: advance")
        labels = self.divergences(self.check(path))
        self.assertIn("repo.head_commit", labels)
        self.assertIn("repo.tree", labels)

    def test_check_stale_new_domains(self):
        for rel in ("framework/portmaster/VERSION",
                    "framework/contracts/apkcompat/VERSION",
                    "framework/tests/VERSION"):
            with self.subTest(rel):
                fx = Fixture(tempfile.mkdtemp(prefix="c.", dir=self.base))
                proc = run_tool("--repo", fx.root)
                path = os.path.join(self.base, "l-%s.json" % rel.count("/"))
                if os.path.exists(path):
                    os.unlink(path)
                with open(path, "wb") as fh:
                    fh.write(proc.stdout)
                fx.write(rel, "9.9.8\n")
                fx.commit("bump")
                proc = run_tool("--repo", fx.root, "--check", path)
                self.assertEqual(proc.returncode, 1)
                self.assertIn(b"components", proc.stderr)

    def test_check_stale_branch_and_dirty(self):
        path = self.record()
        git(self.fx.root, "checkout", "-q", "--detach")
        labels = self.divergences(self.check(path))
        self.assertIn("repo.branch", labels)
        self.assertIn("repo.detached", labels)

    def test_check_stale_tag_only(self):
        git(self.fx.root, "tag", "v-fixture")
        path = self.record()
        git(self.fx.root, "tag", "-d", "v-fixture")
        self.assertEqual(self.divergences(self.check(path)),
                         ["tags_at_head"])

    def test_check_refuses_old_schema(self):
        path = write_json(os.path.join(self.base, "old.json"),
                          {"schema": "org.nextos.v4.ledger",
                           "schema_version": 1})
        proc = self.check(path)
        self.assertEqual(proc.returncode, 2)
        self.assertIn(b"ledger schema", proc.stderr)

    def test_check_io_failure_is_exit_4(self):
        path = self.record()
        os.chmod(path, 0)
        try:
            proc = self.check(path)
        finally:
            os.chmod(path, 0o600)
        self.assertEqual(proc.returncode, 4, proc.stderr)
        self.assertNotIn(b"Traceback", proc.stderr)


class OutCases(Base):
    def test_out_outside_repo_passes_check_same_state(self):
        target = os.path.join(self.base, "ledger-out.json")
        _, raw = self.derive_json("--out", target)
        self.assertEqual(read_bytes(target), raw)
        proc = run_tool("--repo", self.fx.root, "--check", target)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_out_ignored_inside_repo_passes_check_same_state(self):
        target = os.path.join(self.fx.root, "ignored-out", "ledger.json")
        _, raw = self.derive_json("--out", target)
        proc = run_tool("--repo", self.fx.root, "--check", target)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_out_refuses_tracked_location_inside_repo(self):
        target = os.path.join(self.fx.root, "ledger.json")
        proc = run_tool("--repo", self.fx.root, "--out", target)
        self.assertEqual(proc.returncode, 2)
        self.assertIn(b"git-ignored", proc.stderr)

    def test_out_refuses_existing_and_symlink(self):
        target = os.path.join(self.base, "existing.json")
        write_json(target, {})
        proc = run_tool("--repo", self.fx.root, "--out", target)
        self.assertEqual(proc.returncode, 2)
        self.assertIn(b"existing path", proc.stderr)
        link = os.path.join(self.base, "link.json")
        os.symlink("somewhere.json", link)
        proc = run_tool("--repo", self.fx.root, "--out", link)
        self.assertEqual(proc.returncode, 2)

    def test_interrupted_write_leaves_nothing(self):
        target = os.path.join(self.base, "never.json")

        def broken(fd):
            raise OSError("simulated interruption")

        with mock.patch.object(nxledger, "_fsync", broken):
            with self.assertRaises(nxledger.IOFailure):
                nxledger.write_atomic(b"{}\n", target, self.fx.root)
        self.assertFalse(os.path.lexists(target))
        leftovers = [n for n in os.listdir(self.base)
                     if n.startswith(".nxledger.")]
        self.assertEqual(leftovers, [])


class OneShotBase(Base):
    def setUp(self):
        super().setUp()
        self.state = os.path.realpath(
            tempfile.mkdtemp(prefix="nxledger-state."))
        self.addCleanup(shutil.rmtree, self.state, ignore_errors=True)
        os.chmod(self.state, 0o700)

    def manifest(self, name="inputs.json", **extra):
        payload = {"schema": nxledger.INPUTS_SCHEMA,
                   "schema_version": nxledger.INPUTS_SCHEMA_VERSION}
        payload.update(extra)
        return write_json(os.path.join(self.base, name), payload)

    def evidence(self, attempt_id, name="evidence.json", result="PASS"):
        return write_json(os.path.join(self.base, name), {
            "schema": nxledger.EVIDENCE_SCHEMA,
            "schema_version": nxledger.EVIDENCE_SCHEMA_VERSION,
            "attempt_id": attempt_id,
            "result": result,
        })

    def cli(self, *args):
        return run_tool("oneshot", *args)

    def common(self, command, *extra, profile="DEV/device-matrix",
               attempt_type="host-battery", manifest=None):
        return self.cli(
            command, "--repo", self.fx.root, "--state-root", self.state,
            "--profile", profile, "--attempt-type", attempt_type,
            "--inputs-manifest", manifest or self.manifest(), *extra)

    def reserve_ok(self, *extra, **kwargs):
        proc = self.common("reserve", *extra, **kwargs)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        for line in proc.stdout.decode("utf-8").splitlines():
            if line.startswith("nxledger oneshot: id="):
                return line.split("id=")[1].strip()
        raise AssertionError("no id printed")

    def base_id_for(self, manifest):
        _, _, base_id = nxledger.build_attempt_tuple(
            self.fx.root, "PUBLIC-FINAL", "host-battery", manifest)
        return base_id

    def finish_pass(self, attempt_id, name):
        proc = self.cli("finish", "--state-root", self.state,
                        "--id", attempt_id, "--result", "PASS",
                        "--evidence-manifest",
                        self.evidence(attempt_id, name))
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def result_id_of(self, attempt_id):
        path = os.path.join(self.state, "attempts", attempt_id,
                            "result.json")
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)["result_id"]

    def host_authority(self, attempt_id, name="host-auth.json"):
        return write_json(os.path.join(self.base, name), {
            "schema": nxledger.HOST_AUTHORITY_SCHEMA,
            "schema_version": nxledger.HOST_AUTHORITY_SCHEMA_VERSION,
            "attempt_id": attempt_id,
            "result_id": self.result_id_of(attempt_id),
        })

    def physical_proof_pass(self, manifest, tag="pp", family="mali450"):
        proof_id = self.reserve_ok("--family", family,
                                   profile="PUBLIC-FINAL",
                                   attempt_type="physical-proof",
                                   manifest=manifest)
        self.finish_pass(proof_id, "e-%s.json" % tag)
        return proof_id



class OneShotIdentity(OneShotBase):
    def test_deterministic_id_and_report_json(self):
        m = self.manifest()
        a = self.common("eligible", "--json", manifest=m)
        b = self.common("eligible", "--json", manifest=m)
        self.assertEqual(a.stdout, b.stdout)
        report = json.loads(a.stdout)
        self.assertEqual(report["profile"], "DEV/device-matrix")
        self.assertFalse(report["blocked"])
        self.assertEqual(len(report["attempt_id"]), 64)

    def test_report_printed_before_reserve(self):
        proc = self.common("reserve")
        out = proc.stdout.decode("utf-8")
        self.assertLess(out.index("profile="), out.index("RESERVED"))
        self.assertIn("effect:", out)

    def test_dirty_tree_blocks(self):
        self.fx.write("untracked.txt", "x\n")
        proc = self.common("reserve")
        self.assertEqual(proc.returncode, 3)
        self.assertIn(b"dirty", proc.stdout)
        self.assertEqual(store_states(self.state), [])

    def test_detached_needs_declaration(self):
        git(self.fx.root, "checkout", "-q", "--detach")
        proc = self.common("eligible")
        self.assertEqual(proc.returncode, 3)
        self.assertIn(b"detached", proc.stdout)
        proc = self.common("eligible", "--detached-ok")
        self.assertEqual(proc.returncode, 0, proc.stdout)

    def test_manifest_stale_commit_fails(self):
        m = self.manifest(commit="0" * 40)
        proc = self.common("eligible", manifest=m)
        self.assertEqual(proc.returncode, 2)
        self.assertIn(b"stale", proc.stderr)

    def test_missing_profile_and_type_fail(self):
        proc = self.cli("eligible", "--repo", self.fx.root,
                        "--state-root", self.state,
                        "--attempt-type", "host-battery",
                        "--inputs-manifest", self.manifest())
        self.assertEqual(proc.returncode, 2)
        self.assertIn(b"explicit --profile", proc.stderr)


class OneShotElves(OneShotBase):
    def build_elf(self):
        src = os.path.join(self.base, "probe.c")
        out = os.path.join(self.base, "probe-elf")
        with open(src, "w") as fh:
            fh.write("int main(void){return 0;}\n")
        subprocess.run(("cc", "-Wl,--build-id=sha1", "-o", out, src),
                       check=True)
        return out

    def test_elf_record_and_divergences(self):
        elf = self.build_elf()
        record = nxledger.inspect_elf(elf)
        self.assertIn(record["elf_class"], (32, 64))
        self.assertTrue(record["build_id"])
        self.assertTrue(record["pt_interp"])
        self.assertTrue(record["max_glibc"].startswith("GLIBC_"))
        good = self.manifest("m1.json", elves=[
            {"logical_path": "bin/probe", "file": elf,
             "sha256": record["sha256"], "build_id": record["build_id"]}])
        proc = self.common("eligible", manifest=good)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        for key, bad in (("sha256", "0" * 64), ("build_id", "ff"),
                         ("size", 1)):
            with self.subTest(key):
                m = self.manifest("m-%s.json" % key, elves=[
                    {"logical_path": "bin/probe", "file": elf, key: bad}])
                proc = self.common("eligible", manifest=m)
                self.assertEqual(proc.returncode, 2)
                self.assertIn(b"divergence", proc.stderr)

    def test_manifest_negatives(self):
        elf = self.build_elf()
        cases = {
            "duplicate": {"elves": [
                {"logical_path": "a", "file": elf},
                {"logical_path": "a", "file": elf}]},
            "unsafe": {"elves": [{"logical_path": "../x", "file": elf}]},
            "missing-field": {"elves": [{"logical_path": "a"}]},
        }
        for name, extra in cases.items():
            with self.subTest(name):
                proc = self.common(
                    "eligible", manifest=self.manifest("n-%s.json" % name,
                                                       **extra))
                self.assertEqual(proc.returncode, 2, proc.stderr)
        link = os.path.join(self.base, "elf-link")
        os.symlink(elf, link)
        proc = self.common("eligible", manifest=self.manifest(
            "n-link.json", elves=[{"logical_path": "a", "file": link}]))
        self.assertEqual(proc.returncode, 2)


def store_states(state_root):
    attempts = os.path.join(state_root, "attempts")
    if not os.path.isdir(attempts):
        return []
    return sorted(os.listdir(attempts))


class OneShotStateRoot(OneShotBase):
    def refuse(self, root, fragment):
        proc = self.cli("eligible", "--repo", self.fx.root,
                        "--state-root", root,
                        "--profile", "DEV/device-matrix",
                        "--attempt-type", "host-battery",
                        "--inputs-manifest", self.manifest())
        self.assertEqual(proc.returncode, 2, proc.stderr)
        self.assertIn(fragment, proc.stderr.decode("utf-8"))

    def test_relative_public_symlink_owner(self):
        self.refuse("relative/path", "absolute")
        public = os.path.realpath(tempfile.mkdtemp(prefix="pub."))
        self.addCleanup(shutil.rmtree, public, ignore_errors=True)
        os.chmod(public, 0o755)
        self.refuse(public, "private")
        link = os.path.join(self.base, "state-link")
        os.symlink(self.state, link)
        self.refuse(link, "symlink")
        nested = os.path.join(link, "sub")
        self.refuse(nested, "symlink")
        with mock.patch.object(nxledger.os, "geteuid",
                               return_value=os.geteuid() + 1):
            with self.assertRaises(nxledger.LedgerError) as ctx:
                nxledger.validate_state_root(self.state)
            self.assertIn("owned", str(ctx.exception))


class OneShotLifecycle(OneShotBase):
    def test_concurrent_reserves_exactly_one_wins(self):
        m = self.manifest()
        args = ("oneshot", "reserve", "--repo", self.fx.root,
                "--state-root", self.state, "--profile", "DEV/device-matrix",
                "--attempt-type", "host-battery", "--inputs-manifest", m)
        procs = [subprocess.Popen((sys.executable, "-B", TOOL) + args,
                                  env=clean_env(), stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE) for _ in range(2)]
        codes = sorted(p.wait() for p in procs)
        for p in procs:
            p.stdout.close()
            p.stderr.close()
        self.assertEqual(codes, [0, 3])
        self.assertEqual(len(store_states(self.state)), 1)

    def test_second_reserve_always_refused_after_any_outcome(self):
        for index, result in enumerate(("PASS", "FAIL", "INCONCLUSIVE")):
            with self.subTest(result):
                m = self.manifest("r-%d.json" % index,
                                  toolchain={"case": result})
                attempt_id = self.reserve_ok(manifest=m)
                proc = self.cli("finish", "--state-root", self.state,
                                "--id", attempt_id, "--result", result,
                                "--evidence-manifest",
                                self.evidence(attempt_id,
                                              "e-%d.json" % index,
                                              result=result))
                self.assertEqual(proc.returncode, 0, proc.stderr)
                proc = self.common("reserve", manifest=m)
                self.assertEqual(proc.returncode, 3)
                self.assertIn(b"consumed", proc.stdout)

    def test_crash_between_mkdir_and_reservation_blocks_retry(self):
        m = self.manifest("crash.json")
        attempt, attempt_id, base_id = nxledger.build_attempt_tuple(
            self.fx.root, "DEV/device-matrix", "host-battery", m)

        def broken(src, dst):
            raise OSError("simulated crash")

        with mock.patch.object(nxledger, "_link", broken):
            with self.assertRaises(nxledger.IOFailure):
                nxledger.oneshot_reserve(self.state, attempt, attempt_id,
                                         base_id, [])
        directory = nxledger._attempt_dir(self.state, attempt_id)
        self.assertFalse(os.path.exists(
            os.path.join(directory, "reservation.json")))
        self.assertEqual([n for n in os.listdir(directory)
                          if n.startswith(".nxledger.")], [])
        proc = self.common("reserve", manifest=m)
        self.assertEqual(proc.returncode, 3)
        self.assertIn(b"inconclusive", proc.stdout)

    def test_finish_negatives(self):
        attempt_id = self.reserve_ok()
        ghost = "0" * 64
        proc = self.cli("finish", "--state-root", self.state, "--id", ghost,
                        "--result", "PASS",
                        "--evidence-manifest", self.evidence(ghost))
        self.assertEqual(proc.returncode, 3)
        self.assertIn(b"no reservation", proc.stderr)
        proc = self.cli("finish", "--state-root", self.state,
                        "--id", attempt_id, "--result", "MAYBE",
                        "--evidence-manifest", self.evidence(attempt_id))
        self.assertEqual(proc.returncode, 2)
        stale = self.evidence("1" * 64, "stale.json")
        proc = self.cli("finish", "--state-root", self.state,
                        "--id", attempt_id, "--result", "PASS",
                        "--evidence-manifest", stale)
        self.assertEqual(proc.returncode, 2)
        self.assertIn(b"stale", proc.stderr)
        proc = self.cli("finish", "--state-root", self.state,
                        "--id", attempt_id, "--result", "PASS",
                        "--evidence-manifest", self.evidence(attempt_id))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        proc = self.cli("finish", "--state-root", self.state,
                        "--id", attempt_id, "--result", "FAIL",
                        "--evidence-manifest", self.evidence(attempt_id))
        self.assertEqual(proc.returncode, 3)
        self.assertIn(b"append-only", proc.stderr)

    def test_finish_crash_leaves_no_partial_result(self):
        attempt_id = self.reserve_ok()

        def broken(fd):
            raise OSError("simulated crash")

        with mock.patch.object(nxledger, "_fsync", broken):
            with self.assertRaises(nxledger.IOFailure):
                nxledger.oneshot_finish(self.state, attempt_id, "PASS",
                                        self.evidence(attempt_id))
        directory = nxledger._attempt_dir(self.state, attempt_id)
        self.assertFalse(os.path.exists(
            os.path.join(directory, "result.json")))
        self.assertEqual([n for n in os.listdir(directory)
                          if n.startswith(".nxledger.")], [])


class Profiles(OneShotBase):
    def preflight(self, result="PASS", commit=None, tree=None):
        head, tree_id, _, _, _, _ = nxledger.read_repo_identity(self.fx.root)
        return write_json(os.path.join(self.base, "preflight.json"), {
            "schema": nxledger.PREFLIGHT_SCHEMA,
            "schema_version": nxledger.PREFLIGHT_SCHEMA_VERSION,
            "result": result,
            "commit": commit or head,
            "tree": tree or tree_id,
        })

    def physical(self, family, result="PASS", commit=None, tree=None,
                 base_id=None, proof_attempt_id=None, proof_result_id=None,
                 name=None):
        head, tree_id, _, _, _, _ = nxledger.read_repo_identity(self.fx.root)
        if proof_result_id is None and proof_attempt_id is not None and \
                os.path.exists(os.path.join(
                    self.state, "attempts", proof_attempt_id, "result.json")):
            proof_result_id = self.result_id_of(proof_attempt_id)
        return write_json(
            os.path.join(self.base, name or ("phys-%s.json" % family)), {
                "schema": nxledger.PHYSICAL_SCHEMA,
                "schema_version": nxledger.PHYSICAL_SCHEMA_VERSION,
                "family": family,
                "result": result,
                "commit": commit or head,
                "tree": tree or tree_id,
                "base_id": base_id,
                "attempt_id": proof_attempt_id,
                "result_id": proof_result_id,
            })

    def test_dev_never_authorizes_public_candidate(self):
        proc = self.common("eligible", profile="DEV/device-matrix",
                           attempt_type="package-candidate")
        self.assertEqual(proc.returncode, 3)
        self.assertIn(b"DEV/device-matrix never authorizes", proc.stdout)

    def test_dev_receipt_records_unproven_claims_false(self):
        attempt_id = self.reserve_ok()
        directory = nxledger._attempt_dir(self.state, attempt_id)
        with open(os.path.join(directory, "reservation.json")) as fh:
            reservation = json.load(fh)
        self.assertIs(reservation["physical_support_proven"], False)
        self.assertIs(reservation["public_candidate_authorized"], False)

    def test_public_final_blocked_with_complete_list(self):
        proc = self.common("eligible", profile="PUBLIC-FINAL",
                           attempt_type="package-candidate")
        self.assertEqual(proc.returncode, 3)
        out = proc.stdout.decode("utf-8")
        self.assertIn("host authority missing", out)
        self.assertIn("preflight receipt missing", out)
        self.assertIn("physical families", out)

    def test_public_final_missing_families_listed_individually(self):
        proc = self.common(
            "eligible", "--require-physical", "mali450",
            "--require-physical", "muos-h700",
            profile="PUBLIC-FINAL", attempt_type="package-candidate")
        out = proc.stdout.decode("utf-8")
        self.assertIn("physical authority missing: mali450", out)
        self.assertIn("physical authority missing: muos-h700", out)

    def test_package_flow_pass_and_profile_isolation(self):
        base_manifest = self.manifest("flow.json")
        host_id = self.reserve_ok(profile="PUBLIC-FINAL",
                                  manifest=base_manifest)
        self.finish_pass(host_id, "e-host.json")
        base_id = self.base_id_for(base_manifest)
        proof_id = self.physical_proof_pass(base_manifest)
        args = ("--preflight-receipt", self.preflight(),
                "--host-authority", self.host_authority(host_id),
                "--require-physical", "mali450",
                "--physical-authority",
                self.physical("mali450", base_id=base_id,
                              proof_attempt_id=proof_id))
        proc = self.common("reserve", *args, profile="PUBLIC-FINAL",
                           attempt_type="package-candidate",
                           manifest=base_manifest)
        self.assertEqual(proc.returncode, 0,
                         proc.stdout + proc.stderr)
        # A DEV host PASS must not serve a PUBLIC-FINAL candidate: in a
        # fresh store holding ONLY a DEV PASS, the PUBLIC-FINAL candidate
        # stays blocked because the base identity differs by profile.
        self.state = os.path.realpath(
            tempfile.mkdtemp(prefix="nxledger-state2."))
        self.addCleanup(shutil.rmtree, self.state, ignore_errors=True)
        os.chmod(self.state, 0o700)
        dev_id = self.reserve_ok(profile="DEV/device-matrix",
                                 manifest=base_manifest)
        proc = self.cli("finish", "--state-root", self.state,
                        "--id", dev_id, "--result", "PASS",
                        "--evidence-manifest",
                        self.evidence(dev_id, "e-dev.json"))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        proc = self.common("eligible", *args, profile="PUBLIC-FINAL",
                           attempt_type="package-candidate",
                           manifest=base_manifest)
        self.assertEqual(proc.returncode, 3)
        self.assertIn(b"host authority does not name a finished attempt",
                      proc.stdout)

    def test_package_blocked_before_host_pass_or_bad_preflight(self):
        manifest = self.manifest("pkg.json")
        base_id = self.base_id_for(manifest)
        args_base = dict(profile="PUBLIC-FINAL",
                         attempt_type="package-candidate", manifest=manifest)

        def phys(name=None):
            return self.physical("mali450", base_id=base_id,
                                 proof_attempt_id="1" * 64, name=name)

        proc = self.common("eligible", "--preflight-receipt",
                           self.preflight(), "--require-physical", "mali450",
                           "--physical-authority", phys(),
                           **args_base)
        self.assertEqual(proc.returncode, 3)
        self.assertIn(b"host authority missing", proc.stdout)
        host_id = self.reserve_ok(profile="PUBLIC-FINAL", manifest=manifest)
        self.finish_pass(host_id, "e-h.json")
        for name, receipt, code in (
                ("skip", self.preflight(result="SKIP"), 3),
                ("stale", self.preflight(commit="0" * 40), 3),
                ("malformed", write_json(
                    os.path.join(self.base, "bad.json"), {"schema": "x"}),
                 2)):
            with self.subTest(name):
                proc = self.common(
                    "eligible", "--preflight-receipt", receipt,
                    "--require-physical", "mali450",
                    "--physical-authority", phys("p-%s.json" % name),
                    **args_base)
                self.assertEqual(proc.returncode, code,
                                 proc.stdout + proc.stderr)


def build_probe_elf(base, tag="a"):
    src = os.path.join(base, "probe-%s.c" % tag)
    out = os.path.join(base, "probe-elf-%s" % tag)
    with open(src, "w") as fh:
        fh.write("int marker_%s = %d; int main(void){return marker_%s;}\n"
                 % (tag, len(tag), tag))
    subprocess.run(("cc", "-Wl,--build-id=sha1", "-o", out, src), check=True)
    return out


class ArtifactBinding(OneShotBase):
    """Fix 1: host PASS and package share the complete input identity."""

    def test_host_pass_elf_a_never_serves_package_elf_b(self):
        elf_a = build_probe_elf(self.base, "aa")
        elf_b = build_probe_elf(self.base, "bb")
        m_a = self.manifest("m-a.json", elves=[
            {"logical_path": "bin/game", "file": elf_a}])
        m_b = self.manifest("m-b.json", elves=[
            {"logical_path": "bin/game", "file": elf_b}])
        self.assertNotEqual(self.base_id_for(m_a), self.base_id_for(m_b))
        host_id = self.reserve_ok(profile="PUBLIC-FINAL", manifest=m_a)
        self.finish_pass(host_id, "e-a.json")
        proc = self.common("eligible", "--host-authority",
                           self.host_authority(host_id),
                           profile="PUBLIC-FINAL",
                           attempt_type="package-candidate", manifest=m_b)
        self.assertEqual(proc.returncode, 3)
        self.assertIn(b"binds another artifact identity", proc.stdout)

    def test_toolchain_and_manifest_change_new_identity(self):
        m1 = self.manifest("t1.json", toolchain={"cc": "gcc-13"})
        m2 = self.manifest("t2.json", toolchain={"cc": "gcc-14"})
        self.assertNotEqual(self.base_id_for(m1), self.base_id_for(m2))

    def test_receipts_enter_attempt_id_not_base(self):
        m = self.manifest("r.json")
        a1, id1, b1 = nxledger.build_attempt_tuple(
            self.fx.root, "PUBLIC-FINAL", "package-candidate", m,
            {"require_physical": ["mali450"]})
        a2, id2, b2 = nxledger.build_attempt_tuple(
            self.fx.root, "PUBLIC-FINAL", "package-candidate", m,
            {"require_physical": ["mali450", "muos-h700"]})
        self.assertEqual(b1, b2)
        self.assertNotEqual(id1, id2)

    def test_duplicate_required_family_is_error(self):
        proc = self.common("eligible", "--require-physical", "mali450",
                           "--require-physical", "mali450",
                           profile="PUBLIC-FINAL",
                           attempt_type="package-candidate")
        self.assertEqual(proc.returncode, 2)
        self.assertIn(b"duplicate required physical family", proc.stderr)


class ReceiptBinding(Profiles):
    """Fix 3: a receipt of another artifact/family/attempt never authorizes."""

    def prepared(self):
        manifest = self.manifest("rb.json")
        base_id = self.base_id_for(manifest)
        host_id = self.reserve_ok(profile="PUBLIC-FINAL", manifest=manifest)
        self.finish_pass(host_id, "e-rb.json")
        self.host_auth = self.host_authority(host_id, "ha-rb.json")
        proof_id = self.physical_proof_pass(manifest, tag="rb")
        return manifest, base_id, proof_id

    def eligible(self, manifest, receipt):
        return self.common(
            "eligible", "--preflight-receipt", self.preflight(),
            "--host-authority", self.host_auth,
            "--require-physical", "mali450",
            "--physical-authority", receipt,
            profile="PUBLIC-FINAL", attempt_type="package-candidate",
            manifest=manifest)

    def test_receipt_of_other_artifact_family_or_attempt_refused(self):
        manifest, base_id, proof_id = self.prepared()
        good = self.physical("mali450", base_id=base_id,
                             proof_attempt_id=proof_id, name="ok.json")
        self.assertEqual(self.eligible(manifest, good).returncode, 0)
        cases = {
            "other-artifact": self.physical(
                "mali450", base_id="0" * 64, proof_attempt_id=proof_id,
                name="oa.json"),
            "other-attempt": self.physical(
                "mali450", base_id=base_id, proof_attempt_id="2" * 64,
                name="ot.json"),
            "other-family-receipt": self.physical(
                "muos-h700", base_id=base_id, proof_attempt_id=proof_id,
                name="of.json"),
        }
        for name, receipt in cases.items():
            with self.subTest(name):
                proc = self.eligible(manifest, receipt)
                self.assertEqual(proc.returncode, 3, proc.stdout)

    def test_duplicate_provided_family_is_error_not_lastwins(self):
        manifest, base_id, proof_id = self.prepared()
        r1 = self.physical("mali450", base_id=base_id,
                           proof_attempt_id=proof_id, name="d1.json")
        r2 = self.physical("mali450", base_id="0" * 64,
                           proof_attempt_id=proof_id, name="d2.json")
        proc = self.common(
            "eligible", "--preflight-receipt", self.preflight(),
            "--require-physical", "mali450",
            "--physical-authority", r1, "--physical-authority", r2,
            profile="PUBLIC-FINAL", attempt_type="package-candidate",
            manifest=manifest)
        self.assertEqual(proc.returncode, 2)
        self.assertIn(b"duplicate physical authority", proc.stderr)

    def test_dev_physical_proof_never_serves_public_package(self):
        manifest = self.manifest("dev-pp.json")
        base_id = self.base_id_for(manifest)
        host_id = self.reserve_ok(profile="PUBLIC-FINAL", manifest=manifest)
        self.finish_pass(host_id, "e-dp.json")
        self.host_auth = self.host_authority(host_id, "ha-dp.json")
        dev_proof = self.reserve_ok("--family", "mali450",
                                    profile="DEV/device-matrix",
                                    attempt_type="physical-proof",
                                    manifest=manifest)
        self.finish_pass(dev_proof, "e-dp2.json")
        receipt = self.physical("mali450", base_id=base_id,
                                proof_attempt_id=dev_proof, name="dp.json")
        proc = self.eligible(manifest, receipt)
        self.assertEqual(proc.returncode, 3)
        self.assertIn(b"physical-proof PASS of this artifact and profile",
                      proc.stdout)


class StateRootHardening(OneShotBase):
    """Fix 2: external, private, symlink-free at every level, no late swap."""

    def refuse(self, root, fragment):
        proc = self.cli("eligible", "--repo", self.fx.root,
                        "--state-root", root,
                        "--profile", "DEV/device-matrix",
                        "--attempt-type", "host-battery",
                        "--inputs-manifest", self.manifest())
        self.assertEqual(proc.returncode, 2, proc.stderr)
        self.assertIn(fragment, proc.stderr.decode("utf-8"))

    def test_root_inside_git_tree_refused(self):
        inside = os.path.join(self.fx.root, "state-inside")
        os.mkdir(inside, 0o700)
        self.refuse(inside, "OUTSIDE any Git work tree")

    def test_symlink_at_each_level_refused(self):
        real = os.path.join(self.base, "lvl1", "lvl2", "root")
        os.makedirs(real, mode=0o700)
        os.chmod(os.path.join(self.base, "lvl1"), 0o700)
        os.chmod(os.path.join(self.base, "lvl1", "lvl2"), 0o700)
        link_leaf = os.path.join(self.base, "leaf-link")
        os.symlink(real, link_leaf)
        self.refuse(link_leaf, "symlink")
        link_mid = os.path.join(self.base, "mid-link")
        os.symlink(os.path.join(self.base, "lvl1"), link_mid)
        self.refuse(os.path.join(link_mid, "lvl2", "root"), "symlink")

    def test_public_attempts_dir_refused(self):
        attempt_id = self.reserve_ok()
        os.chmod(os.path.join(self.state, "attempts"), 0o755)
        proc = self.cli("finish", "--state-root", self.state,
                        "--id", attempt_id, "--result", "PASS",
                        "--evidence-manifest", self.evidence(attempt_id))
        self.assertEqual(proc.returncode, 2)
        self.assertIn(b"private", proc.stderr)

    def test_late_symlink_swap_of_attempt_dir_refused(self):
        attempt_id = self.reserve_ok()
        directory = nxledger._attempt_dir(self.state, attempt_id)
        moved = directory + ".moved"
        os.rename(directory, moved)
        os.symlink(moved, directory)
        proc = self.cli("finish", "--state-root", self.state,
                        "--id", attempt_id, "--result", "PASS",
                        "--evidence-manifest", self.evidence(attempt_id))
        self.assertEqual(proc.returncode, 2)
        self.assertIn(b"symlink", proc.stderr)


class HostileStore(OneShotBase):
    """Fix 4: a byte changed in the store blocks with an explicit finding."""

    def reserve_and_doc(self):
        attempt_id = self.reserve_ok()
        path = os.path.join(nxledger._attempt_dir(self.state, attempt_id),
                            "reservation.json")
        with open(path, "r", encoding="utf-8") as fh:
            return attempt_id, path, json.load(fh)

    def rewrite(self, path, document):
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(document, fh, sort_keys=True)
        os.chmod(path, 0o600)

    def expect_tampered(self, attempt_id):
        proc = self.cli("finish", "--state-root", self.state,
                        "--id", attempt_id, "--result", "PASS",
                        "--evidence-manifest", self.evidence(attempt_id))
        self.assertEqual(proc.returncode, 3, proc.stderr)
        self.assertIn(b"invalid", proc.stderr)

    def test_tampered_fields_block_field_by_field(self):
        mutations = (
            ("profile", "PUBLIC-FINAL"),
            ("attempt_type", "package-candidate"),
            ("base_id", "0" * 64),
            ("inputs_manifest_sha256", "1" * 64),
            ("schema", "org.nextos.other"),
        )
        for index, (key, value) in enumerate(mutations):
            with self.subTest(key):
                m = self.manifest("h-%d.json" % index,
                                  toolchain={"case": key})
                attempt_id = self.reserve_ok(manifest=m)
                path = os.path.join(
                    nxledger._attempt_dir(self.state, attempt_id),
                    "reservation.json")
                with open(path, "r", encoding="utf-8") as fh:
                    document = json.load(fh)
                document[key] = value
                self.rewrite(path, document)
                self.expect_tampered(attempt_id)

    def test_removed_field_and_duplicate_key_block(self):
        attempt_id, path, document = self.reserve_and_doc()
        document.pop("authorities")
        self.rewrite(path, document)
        self.expect_tampered(attempt_id)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write('{"schema": "a", "schema": "b"}')
        os.chmod(path, 0o600)
        self.expect_tampered(attempt_id)

    def test_public_mode_document_blocks(self):
        attempt_id, path, _ = self.reserve_and_doc()
        os.chmod(path, 0o644)
        self.expect_tampered(attempt_id)

    def test_result_without_reservation_blocks(self):
        attempt_id = self.reserve_ok()
        directory = nxledger._attempt_dir(self.state, attempt_id)
        os.unlink(os.path.join(directory, "reservation.json"))
        write_json(os.path.join(directory, "result.json"), {
            "schema": "org.nextos.v4.oneshot-result", "schema_version": 1,
            "attempt_id": attempt_id, "result": "PASS",
            "evidence_sha256": "0" * 64})
        os.chmod(os.path.join(directory, "result.json"), 0o600)
        view = nxledger.store_lookup(self.state, attempt_id)
        self.assertEqual(view["state"], "tampered")

    def test_tampered_host_pass_never_serves_package(self):
        manifest = self.manifest("tp.json")
        host_id = self.reserve_ok(profile="PUBLIC-FINAL", manifest=manifest)
        proc = self.cli("finish", "--state-root", self.state,
                        "--id", host_id, "--result", "PASS",
                        "--evidence-manifest",
                        self.evidence(host_id, "e-tp.json"))
        self.assertEqual(proc.returncode, 0)
        authority = self.host_authority(host_id, "ha-tp.json")
        path = os.path.join(nxledger._attempt_dir(self.state, host_id),
                            "reservation.json")
        with open(path, "r", encoding="utf-8") as fh:
            document = json.load(fh)
        document["repo"]["dirty"] = True
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(document, fh, sort_keys=True)
        os.chmod(path, 0o600)
        proc = self.common("eligible", "--host-authority", authority,
                           profile="PUBLIC-FINAL",
                           attempt_type="package-candidate",
                           manifest=manifest)
        self.assertEqual(proc.returncode, 3)
        self.assertIn(b"host authority does not name a finished attempt",
                      proc.stdout)


class IOInjection(OneShotBase):
    """Fix 5: every I/O and parser failure is controlled exit 4."""

    def test_truncated_elf_is_io_failure(self):
        whole = build_probe_elf(self.base, "tr")
        path = os.path.join(self.base, "trunc-elf")
        with open(whole, "rb") as fh:
            head = fh.read(100)
        with open(path, "wb") as fh:
            fh.write(head)
        with self.assertRaises(nxledger.IOFailure):
            nxledger.inspect_elf(path)
        m = self.manifest("trunc.json", elves=[
            {"logical_path": "bin/x", "file": path}])
        proc = self.common("eligible", manifest=m)
        self.assertEqual(proc.returncode, 4, proc.stderr)
        self.assertNotIn(b"Traceback", proc.stderr)

    def test_git_launch_failure_is_io_failure(self):
        def broken(*args, **kwargs):
            raise OSError("simulated exec failure")
        with mock.patch.object(nxledger.subprocess, "run", broken):
            with self.assertRaises(nxledger.IOFailure):
                nxledger.repo_root(self.fx.root)

    def test_cleanup_failure_is_io_failure(self):
        target = os.path.join(self.base, "cleanup.json")
        original = os.unlink

        def broken(path, *a, **k):
            if ".nxledger." in str(path):
                raise OSError("simulated cleanup failure")
            return original(path, *a, **k)

        with mock.patch.object(nxledger.os, "unlink", broken):
            with self.assertRaises(nxledger.IOFailure):
                nxledger.write_atomic(b"{}\n", target, self.fx.root)

    def test_listdir_failure_is_io_failure(self):
        self.reserve_ok()

        def broken(path):
            raise OSError("simulated listdir failure")

        with mock.patch.object(nxledger.os, "listdir", broken):
            with self.assertRaises(nxledger.IOFailure):
                list(nxledger._iter_finished(self.state))

    def test_fsync_dir_failure_during_reserve_consumes_tuple(self):
        m = self.manifest("fsd.json")
        attempt, attempt_id, base_id = nxledger.build_attempt_tuple(
            self.fx.root, "DEV/device-matrix", "host-battery", m)
        real = nxledger._fsync
        calls = {"n": 0}

        def broken(fd):
            calls["n"] += 1
            if calls["n"] > 1:
                raise OSError("simulated fsync failure")
            return real(fd)

        with mock.patch.object(nxledger, "_fsync", broken):
            with self.assertRaises(nxledger.IOFailure):
                nxledger.oneshot_reserve(self.state, attempt, attempt_id,
                                         base_id, [])
        proc = self.common("reserve", manifest=m)
        self.assertEqual(proc.returncode, 3)


class SealedResult(OneShotBase):
    """0.2.2 regressions 1-3: the outcome is sealed, claims are derived."""

    def finished(self, result="PASS", profile="DEV/device-matrix", tag="sr"):
        m = self.manifest("m-%s.json" % tag, toolchain={"case": tag})
        attempt_id = self.reserve_ok(profile=profile, manifest=m)
        proc = self.cli("finish", "--state-root", self.state,
                        "--id", attempt_id, "--result", result,
                        "--evidence-manifest",
                        self.evidence(attempt_id, "e-%s.json" % tag,
                                      result=result))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn(b"result-id=", proc.stdout)
        return m, attempt_id

    def result_path(self, attempt_id):
        return os.path.join(nxledger._attempt_dir(self.state, attempt_id),
                            "result.json")

    def rewrite_result(self, attempt_id, document):
        path = self.result_path(attempt_id)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(document, fh, sort_keys=True)
        os.chmod(path, 0o600)

    def test_fail_flipped_to_pass_is_tampered_and_never_authorizes(self):
        m, host_id = self.finished(result="FAIL", profile="PUBLIC-FINAL",
                                   tag="flip")
        authority = self.host_authority(host_id, "ha-flip.json")
        with open(self.result_path(host_id), encoding="utf-8") as fh:
            document = json.load(fh)
        document["result"] = "PASS"
        self.rewrite_result(host_id, document)
        view = nxledger.store_lookup(self.state, host_id)
        self.assertEqual(view["state"], "tampered")
        proc = self.common("eligible", "--host-authority", authority,
                           profile="PUBLIC-FINAL",
                           attempt_type="package-candidate", manifest=m)
        self.assertEqual(proc.returncode, 3)
        self.assertIn(b"host authority does not name a finished attempt",
                      proc.stdout)

    def test_recomputed_flip_still_never_authorizes_package(self):
        m, host_id = self.finished(result="FAIL", profile="PUBLIC-FINAL",
                                   tag="flip2")
        authority = self.host_authority(host_id, "ha-flip2.json")
        with open(self.result_path(host_id), encoding="utf-8") as fh:
            document = json.load(fh)
        document["result"] = "PASS"
        document["result_id"] = nxledger._result_id(document)
        self.rewrite_result(host_id, document)
        proc = self.common("eligible", "--host-authority", authority,
                           profile="PUBLIC-FINAL",
                           attempt_type="package-candidate", manifest=m)
        self.assertEqual(proc.returncode, 3)
        self.assertIn(b"result id does not match", proc.stdout)

    def test_every_result_field_mutation_is_tampered(self):
        mutations = (
            ("result", "FAIL"),
            ("result", "INCONCLUSIVE"),
            ("evidence_sha256", "0" * 64),
            ("result_id", "1" * 64),
            ("reservation_sha256", "2" * 64),
            ("base_id", "3" * 64),
            ("profile", "PUBLIC-FINAL"),
            ("attempt_type", "physical-proof"),
            ("family", "mali450"),
        )
        for index, (key, value) in enumerate(mutations):
            with self.subTest(key + "=" + str(value)):
                _, attempt_id = self.finished(tag="mut%d" % index)
                with open(self.result_path(attempt_id),
                          encoding="utf-8") as fh:
                    document = json.load(fh)
                document[key] = value
                self.rewrite_result(attempt_id, document)
                view = nxledger.store_lookup(self.state, attempt_id)
                self.assertEqual(view["state"], "tampered", view)
        _, attempt_id = self.finished(tag="extra")
        with open(self.result_path(attempt_id), encoding="utf-8") as fh:
            document = json.load(fh)
        document["note"] = "decorative"
        self.rewrite_result(attempt_id, document)
        self.assertEqual(
            nxledger.store_lookup(self.state, attempt_id)["state"],
            "tampered")
        _, attempt_id = self.finished(tag="missing")
        with open(self.result_path(attempt_id), encoding="utf-8") as fh:
            document = json.load(fh)
        del document["evidence_sha256"]
        self.rewrite_result(attempt_id, document)
        self.assertEqual(
            nxledger.store_lookup(self.state, attempt_id)["state"],
            "tampered")

    def test_claim_and_effects_mutations_are_tampered(self):
        attempt_id = self.reserve_ok()
        path = os.path.join(nxledger._attempt_dir(self.state, attempt_id),
                            "reservation.json")
        with open(path, "rb") as fh:
            original = fh.read()
        document = json.loads(original)
        effects_edit = list(document["allowed_effects"])
        effects_edit[0] = effects_edit[0] + " and anything else"
        for change in ({"physical_support_proven": True},
                       {"public_candidate_authorized": True},
                       {"allowed_effects": effects_edit},
                       {"allowed_effects": []},
                       {"extra_claim": True}):
            with self.subTest(str(sorted(change))):
                mutated = dict(json.loads(original))
                mutated.update(change)
                with open(path, "w", encoding="utf-8") as fh:
                    json.dump(mutated, fh, sort_keys=True)
                os.chmod(path, 0o600)
                view = nxledger.store_lookup(self.state, attempt_id)
                self.assertEqual(view["state"], "tampered", view)
        with open(path, "wb") as fh:
            fh.write(original)
        os.chmod(path, 0o600)
        self.assertEqual(
            nxledger.store_lookup(self.state, attempt_id)["state"],
            "reserved")

    def test_bool_never_counts_as_schema_version(self):
        attempt_id = self.reserve_ok()
        path = os.path.join(nxledger._attempt_dir(self.state, attempt_id),
                            "reservation.json")
        with open(path, encoding="utf-8") as fh:
            document = json.load(fh)
        document["schema_version"] = True
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(document, fh, sort_keys=True)
        os.chmod(path, 0o600)
        self.assertEqual(
            nxledger.store_lookup(self.state, attempt_id)["state"],
            "tampered")


class HostAuthorityBinding(Profiles):
    """0.2.2 regression 4: the exact host authority is pinned and rechecked."""

    def test_wrong_result_id_or_type_is_refused(self):
        manifest = self.manifest("hab.json")
        host_id = self.reserve_ok(profile="PUBLIC-FINAL", manifest=manifest)
        self.finish_pass(host_id, "e-hab.json")
        wrong = write_json(os.path.join(self.base, "ha-wrong.json"), {
            "schema": nxledger.HOST_AUTHORITY_SCHEMA,
            "schema_version": nxledger.HOST_AUTHORITY_SCHEMA_VERSION,
            "attempt_id": host_id,
            "result_id": "4" * 64,
        })
        proc = self.common("eligible", "--host-authority", wrong,
                           profile="PUBLIC-FINAL",
                           attempt_type="package-candidate",
                           manifest=manifest)
        self.assertEqual(proc.returncode, 3)
        self.assertIn(b"result id does not match", proc.stdout)
        proof_id = self.physical_proof_pass(manifest, tag="hab")
        not_host = self.host_authority(proof_id, "ha-proof.json")
        proc = self.common("eligible", "--host-authority", not_host,
                           profile="PUBLIC-FINAL",
                           attempt_type="package-candidate",
                           manifest=manifest)
        self.assertEqual(proc.returncode, 3)
        self.assertIn(b"not a host-battery attempt", proc.stdout)
        proc = self.common("eligible", "--host-authority",
                           self.host_authority(host_id, "ha-dev.json"),
                           profile="DEV/device-matrix",
                           attempt_type="host-battery", manifest=manifest)
        self.assertEqual(proc.returncode, 2)
        self.assertIn(b"only valid for package-candidate", proc.stderr)

    def test_authority_tampered_after_package_reserve_blocks_finish(self):
        manifest = self.manifest("habf.json")
        host_id = self.reserve_ok(profile="PUBLIC-FINAL", manifest=manifest)
        self.finish_pass(host_id, "e-habf.json")
        base_id = self.base_id_for(manifest)
        proof_id = self.physical_proof_pass(manifest, tag="habf")
        package_id = self.reserve_ok(
            "--preflight-receipt", self.preflight(),
            "--host-authority", self.host_authority(host_id, "ha-habf.json"),
            "--require-physical", "mali450",
            "--physical-authority",
            self.physical("mali450", base_id=base_id,
                          proof_attempt_id=proof_id),
            profile="PUBLIC-FINAL", attempt_type="package-candidate",
            manifest=manifest)
        host_result = os.path.join(
            nxledger._attempt_dir(self.state, host_id), "result.json")
        with open(host_result, encoding="utf-8") as fh:
            document = json.load(fh)
        document["result"] = "FAIL"
        document["result_id"] = nxledger._result_id(document)
        with open(host_result, "w", encoding="utf-8") as fh:
            json.dump(document, fh, sort_keys=True)
        os.chmod(host_result, 0o600)
        proc = self.cli("finish", "--state-root", self.state,
                        "--id", package_id, "--result", "PASS",
                        "--evidence-manifest",
                        self.evidence(package_id, "e-pkgf.json"))
        self.assertEqual(proc.returncode, 3, proc.stderr)
        self.assertIn(b"no longer valid at finish time", proc.stderr)
        self.assertFalse(os.path.exists(os.path.join(
            nxledger._attempt_dir(self.state, package_id), "result.json")))


class FamilyBinding(Profiles):
    """0.2.2 regression 5: one proof, one family."""

    def test_family_argument_rules(self):
        proc = self.common("eligible", profile="PUBLIC-FINAL",
                           attempt_type="physical-proof")
        self.assertEqual(proc.returncode, 2)
        self.assertIn(b"requires exactly one canonical --family", proc.stderr)
        proc = self.common("eligible", "--family", "mali450",
                           profile="DEV/device-matrix",
                           attempt_type="host-battery")
        self.assertEqual(proc.returncode, 2)
        self.assertIn(b"only valid for physical-proof", proc.stderr)

    def test_one_proof_never_serves_two_families(self):
        manifest = self.manifest("fam.json")
        base_id = self.base_id_for(manifest)
        host_id = self.reserve_ok(profile="PUBLIC-FINAL", manifest=manifest)
        self.finish_pass(host_id, "e-fam.json")
        authority = self.host_authority(host_id, "ha-fam.json")
        proof_a = self.physical_proof_pass(manifest, tag="fam-a",
                                           family="mali450")
        proof_b = self.physical_proof_pass(manifest, tag="fam-b",
                                           family="muos-h700")

        def eligible(receipt_b):
            return self.common(
                "eligible", "--preflight-receipt", self.preflight(),
                "--host-authority", authority,
                "--require-physical", "mali450",
                "--require-physical", "muos-h700",
                "--physical-authority",
                self.physical("mali450", base_id=base_id,
                              proof_attempt_id=proof_a, name="fam-ra.json"),
                "--physical-authority", receipt_b,
                profile="PUBLIC-FINAL", attempt_type="package-candidate",
                manifest=manifest)

        reused = self.physical(
            "muos-h700", base_id=base_id, proof_attempt_id=proof_a,
            proof_result_id=self.result_id_of(proof_a), name="fam-rb1.json")
        proc = eligible(reused)
        self.assertEqual(proc.returncode, 3)
        self.assertIn(b"one proof never serves two families", proc.stdout)
        correct = self.physical("muos-h700", base_id=base_id,
                                proof_attempt_id=proof_b,
                                name="fam-rb2.json")
        proc = eligible(correct)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)


class StrictInputDocuments(OneShotBase):
    """0.2.2 regression 6: one strict JSON loader for every input."""

    def raw(self, name, text):
        path = os.path.join(self.base, name)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
        return path

    def test_duplicate_key_and_nan_are_refused_everywhere(self):
        dup = '{"schema": "x", "schema": "y"}'
        nan = '{"schema": "x", "value": NaN}'
        for flag, extra in (
                ("--inputs-manifest", ()),
                ("--preflight-receipt", ()),
                ("--physical-authority", ())):
            for tag, text in (("dup", dup), ("nan", nan)):
                with self.subTest(flag + "-" + tag):
                    hostile = self.raw("bad-%s.json" % tag, text)
                    if flag == "--inputs-manifest":
                        proc = self.cli(
                            "eligible", "--repo", self.fx.root,
                            "--state-root", self.state,
                            "--profile", "DEV/device-matrix",
                            "--attempt-type", "host-battery",
                            "--inputs-manifest", hostile)
                    else:
                        proc = self.common(
                            "eligible", flag, hostile, *extra,
                            profile="PUBLIC-FINAL",
                            attempt_type="package-candidate")
                    self.assertEqual(proc.returncode, 2, proc.stderr)
                    self.assertNotIn(b"Traceback", proc.stderr)
        attempt_id = self.reserve_ok()
        for tag, text in (("dup", dup), ("nan", nan)):
            with self.subTest("evidence-" + tag):
                proc = self.cli(
                    "finish", "--state-root", self.state,
                    "--id", attempt_id, "--result", "PASS",
                    "--evidence-manifest",
                    self.raw("bad-ev-%s.json" % tag, text))
                self.assertEqual(proc.returncode, 2, proc.stderr)


class HybridDerivation(OneShotBase):
    """0.2.2 regression 7: sources that move mid-derivation are refused."""

    def test_elf_changing_during_measurement_is_refused(self):
        elf = build_probe_elf(self.base, "hy")
        m = self.manifest("hy.json", elves=[
            {"logical_path": "bin/game", "file": elf}])
        original = nxledger.inspect_elf

        def sneaky(path):
            record = original(path)
            with open(path, "ab") as fh:
                fh.write(b"x")
            return record

        head, tree, *_ = nxledger.read_repo_identity(self.fx.root)
        repo = {"head_commit": head, "tree": tree}
        with mock.patch.object(nxledger, "inspect_elf", sneaky):
            with self.assertRaisesRegex(nxledger.LedgerError,
                                        "changed while it was being "
                                        "measured"):
                nxledger.load_inputs_manifest(m, self.fx.root, repo)

    def test_repo_moving_during_derivation_is_refused(self):
        m = self.manifest("hy2.json")
        original = nxledger.load_inputs_manifest
        fixture = self.fx

        def sneaky(path, root, repo):
            result = original(path, root, repo)
            fixture.write("moved.txt", "x\n")
            fixture.commit("fixture: moved mid-derivation")
            return result

        with mock.patch.object(nxledger, "load_inputs_manifest", sneaky):
            with self.assertRaisesRegex(nxledger.LedgerError,
                                        "changed during derivation"):
                nxledger.build_attempt_tuple(
                    self.fx.root, "DEV/device-matrix", "host-battery", m)


class GitShapedStateRoots(OneShotBase):
    """0.2.2 regression 8: bare repos and .git directories are refused."""

    def test_bare_repo_state_root_refused(self):
        bare = os.path.join(self.base, "bare.git")
        subprocess.run(("git", "init", "-q", "--bare", bare), check=True,
                       env=clean_env())
        inside = os.path.join(bare, "priv")
        os.makedirs(inside, mode=0o700)
        with self.assertRaisesRegex(nxledger.LedgerError,
                                    "bare Git|Git directory"):
            nxledger.validate_state_root(inside)

    def test_git_dir_state_root_refused(self):
        inside = os.path.join(self.fx.root, ".git", "priv")
        os.makedirs(inside, mode=0o700)
        with self.assertRaisesRegex(nxledger.LedgerError,
                                    "Git directory|Git work tree"):
            nxledger.validate_state_root(inside)


class NegativeControl021(OneShotBase):
    """0.2.2 regression 10: 0.2.1 accepted what 0.2.2 refuses."""

    OLD_COMMIT = "a4f1ac584d54a17c05616547b49b2af15375534a"

    def load_old_module(self):
        proc = subprocess.run(
            ("git", "-C", TOOL_DIR, "show",
             self.OLD_COMMIT + ":framework/nxledger/nxledger.py"),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        old_dir = os.path.join(self.base, "old021")
        os.makedirs(old_dir)
        old_path = os.path.join(old_dir, "nxledger_021.py")
        with open(old_path, "wb") as fh:
            fh.write(proc.stdout)
        import importlib.util
        spec = importlib.util.spec_from_file_location("nxledger_021",
                                                      old_path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_021_accepted_result_flip_and_cross_family_proof(self):
        old = self.load_old_module()
        m = self.manifest("old.json")
        attempt, attempt_id, base_id = old.build_attempt_tuple(
            self.fx.root, "PUBLIC-FINAL", "host-battery", m)
        effects, reasons = old.evaluate_attempt(
            self.state, attempt, attempt_id, base_id, {})
        self.assertEqual(reasons, [])
        old.oneshot_reserve(self.state, attempt, attempt_id, base_id,
                            effects)
        old_evidence = write_json(os.path.join(self.base, "old-ev.json"), {
            "schema": old.EVIDENCE_SCHEMA,
            "schema_version": old.EVIDENCE_SCHEMA_VERSION,
            "attempt_id": attempt_id})
        import contextlib
        import io
        with contextlib.redirect_stdout(io.StringIO()):
            old.oneshot_finish(self.state, attempt_id, "FAIL", old_evidence)
        result_path = os.path.join(
            old._attempt_dir(self.state, attempt_id), "result.json")
        with open(result_path, encoding="utf-8") as fh:
            document = json.load(fh)
        document["result"] = "PASS"
        with open(result_path, "w", encoding="utf-8") as fh:
            json.dump(document, fh, sort_keys=True)
        os.chmod(result_path, 0o600)
        old_view = old.store_lookup(self.state, attempt_id)
        self.assertEqual(old_view["state"], "finished")
        self.assertEqual(old_view["result"].get("result"), "PASS")
        new_view = nxledger.store_lookup(self.state, attempt_id)
        self.assertEqual(new_view["state"], "tampered")
        # Cross-family: an 0.2.1 physical proof carried no family, so ONE
        # proof could back a receipt of ANY family.
        proof, proof_id, _ = old.build_attempt_tuple(
            self.fx.root, "PUBLIC-FINAL", "physical-proof", m)
        proof_effects, proof_reasons = old.evaluate_attempt(
            self.state, proof, proof_id, base_id, {})
        self.assertEqual(proof_reasons, [])
        old.oneshot_reserve(self.state, proof, proof_id, base_id,
                            proof_effects)
        with contextlib.redirect_stdout(io.StringIO()):
            old.oneshot_finish(self.state, proof_id, "PASS", write_json(
                os.path.join(self.base, "old-pev.json"), {
                    "schema": old.EVIDENCE_SCHEMA,
                    "schema_version": old.EVIDENCE_SCHEMA_VERSION,
                    "attempt_id": proof_id}))
        head, tree, *_ = old.read_repo_identity(self.fx.root)
        receipt = write_json(os.path.join(self.base, "old-phys.json"), {
            "schema": old.PHYSICAL_SCHEMA,
            "schema_version": old.PHYSICAL_SCHEMA_VERSION,
            "family": "muos-h700",
            "result": "PASS",
            "commit": head,
            "tree": tree,
            "base_id": base_id,
            "attempt_id": proof_id})
        preflight = write_json(os.path.join(self.base, "old-pf.json"), {
            "schema": old.PREFLIGHT_SCHEMA,
            "schema_version": old.PREFLIGHT_SCHEMA_VERSION,
            "result": "PASS", "commit": head, "tree": tree})
        package, package_id, package_base = old.build_attempt_tuple(
            self.fx.root, "PUBLIC-FINAL", "package-candidate", m, {
                "require_physical": ["muos-h700"],
                "preflight_receipt": preflight,
                "physical_authorities": [receipt]})
        _, package_reasons = old.evaluate_attempt(
            self.state, package, package_id, package_base, {})
        self.assertEqual(package_reasons, [],
                         "0.2.1 must accept the cross-family proof for the "
                         "negative control to hold")

class ResultSchemaV2(OneShotBase):
    """V4-05B5: oneshot-result is genuinely /2; a /1 document fails closed."""

    def test_emitted_result_is_schema_version_2(self):
        self.assertEqual(nxledger.RESULT_SCHEMA_VERSION, 2)
        aid = self.reserve_ok(profile="PUBLIC-FINAL", manifest=self.manifest())
        self.finish_pass(aid, "e.json")
        path = os.path.join(self.state, "attempts", aid, "result.json")
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
        self.assertEqual(doc["schema"], "org.nextos.v4.oneshot-result")
        self.assertEqual(doc["schema_version"], 2)
        # sealed and internally consistent
        self.assertEqual(nxledger.store_lookup(self.state, aid)["state"],
                         "finished")

    def test_result_v1_fails_closed(self):
        """A result document presenting schema_version 1 is tampered."""
        aid = self.reserve_ok(profile="PUBLIC-FINAL", manifest=self.manifest())
        self.finish_pass(aid, "e2.json")
        path = os.path.join(self.state, "attempts", aid, "result.json")
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
        doc["schema_version"] = 1
        # even recompute the result_id so ONLY the version is "wrong"
        core = {k: doc[k] for k in doc if k != "result_id"}
        doc["result_id"] = hashlib.sha256(
            nxledger.canonical_bytes(core)).hexdigest()
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(doc, fh, sort_keys=True)
        os.chmod(path, 0o600)
        self.assertEqual(nxledger.store_lookup(self.state, aid)["state"],
                         "tampered")

    def test_real_021_result_v1_refused(self):
        """A real result produced by nxledger 0.2.1 (schema_version 1, no
        sealed key set) is refused by 0.2.3 -- read back as tampered."""
        proc = subprocess.run(
            ("git", "-C", os.path.dirname(nxledger.__file__), "show",
             "a4f1ac584d54a17c05616547b49b2af15375534a:"
             "framework/nxledger/nxledger.py"),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        old_dir = os.path.join(self.base, "old021rv2")
        os.makedirs(old_dir)
        old_path = os.path.join(old_dir, "nxledger_021.py")
        with open(old_path, "wb") as fh:
            fh.write(proc.stdout)
        import importlib.util
        spec = importlib.util.spec_from_file_location("nxledger_021_rv2",
                                                      old_path)
        old = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(old)
        m = self.manifest("r021.json")
        attempt, aid, bid = old.build_attempt_tuple(
            self.fx.root, "PUBLIC-FINAL", "host-battery", m)
        effects, reasons = old.evaluate_attempt(self.state, attempt, aid,
                                                bid, {})
        self.assertEqual(reasons, [])
        old.oneshot_reserve(self.state, attempt, aid, bid, effects)
        ev = write_json(os.path.join(self.base, "r021-ev.json"), {
            "schema": old.EVIDENCE_SCHEMA,
            "schema_version": old.EVIDENCE_SCHEMA_VERSION,
            "attempt_id": aid})
        import contextlib
        import io
        with contextlib.redirect_stdout(io.StringIO()):
            old.oneshot_finish(self.state, aid, "PASS", ev)
        result_path = os.path.join(
            old._attempt_dir(self.state, aid), "result.json")
        with open(result_path, encoding="utf-8") as fh:
            produced = json.load(fh)
        # the real 0.2.1 result IS schema_version 1
        self.assertEqual(produced["schema_version"], 1)
        # 0.2.1 itself reads its own result as a finished PASS...
        self.assertEqual(old.store_lookup(self.state, aid)["state"],
                         "finished")
        # ...but 0.2.3 refuses that /1 document as tampered.
        self.assertEqual(nxledger.store_lookup(self.state, aid)["state"],
                         "tampered")

    def test_result_v2_accepted_in_host_authority_package_flow(self):
        """result/2 is produced, reread, sealed by result_id and authorizes
        a PUBLIC-FINAL package through the host-authority path."""
        manifest = self.manifest("v2flow.json")
        host_id = self.reserve_ok(profile="PUBLIC-FINAL", manifest=manifest)
        self.finish_pass(host_id, "e-hv2.json")
        # sealed result reread and its result_id used as authority
        result_id = self.result_id_of(host_id)
        self.assertTrue(len(result_id) == 64 and all(c in "0123456789abcdef" for c in result_id))
        host_auth = self.host_authority(host_id, "ha-v2.json")
        proof_id = self.physical_proof_pass(manifest, tag="v2")
        head, tree_id, _, _, _, _ = nxledger.read_repo_identity(self.fx.root)
        phys = write_json(os.path.join(self.base, "phys-v2.json"), {
            "schema": nxledger.PHYSICAL_SCHEMA,
            "schema_version": nxledger.PHYSICAL_SCHEMA_VERSION,
            "family": "mali450", "result": "PASS",
            "commit": head, "tree": tree_id,
            "base_id": self.base_id_for(manifest),
            "attempt_id": proof_id,
            "result_id": self.result_id_of(proof_id)})
        preflight = write_json(os.path.join(self.base, "pf-v2.json"), {
            "schema": nxledger.PREFLIGHT_SCHEMA,
            "schema_version": nxledger.PREFLIGHT_SCHEMA_VERSION,
            "result": "PASS", "commit": head, "tree": tree_id})
        proc = self.common(
            "eligible", "--host-authority", host_auth,
            "--preflight-receipt", preflight,
            "--require-physical", "mali450",
            "--physical-authority", phys,
            profile="PUBLIC-FINAL", attempt_type="package-candidate",
            manifest=manifest)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)


class RunnerHygiene(Base):
    def test_cli_never_tracebacks_on_controlled_errors(self):
        for args, code in ((("--repo", os.path.join(self.base, "nowhere")),
                            2),):
            proc = run_tool(*args)
            self.assertEqual(proc.returncode, code)
            self.assertNotIn(b"Traceback", proc.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
