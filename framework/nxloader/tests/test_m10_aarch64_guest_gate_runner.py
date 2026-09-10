#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure self-tests for the M10 local guest-gate runner.

These tests create only temporary text files and test-owned Python children.
They never load a guest ELF, run nxloader_inspect, access a device, or use the
network.
"""

import hashlib
import importlib.util
import json
import os
import signal
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


RUNNER = Path(__file__).with_name("run_m10_aarch64_guest_gate.py")
SPEC = importlib.util.spec_from_file_location("m10_guest_gate", RUNNER)
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


class ChildSupervisorTests(unittest.TestCase):
    def test_normal_child_is_bounded_and_collected(self):
        result = gate.run_child(
            [sys.executable, "-c", "print('bounded-ok')"],
            timeout_seconds=2.0, output_limit_bytes=4096,
            term_grace_seconds=0.05)
        self.assertEqual(result["returncode"], 0)
        self.assertEqual(result["stdout"], "bounded-ok\n")
        self.assertFalse(result["timed_out"])
        self.assertFalse(result["output_limit_exceeded"])
        self.assertIsNone(result["termination_signal"])

    def test_timeout_uses_kill_fallback_and_leaves_sibling_alive(self):
        sibling = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(10)"],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, close_fds=True)
        try:
            target = (
                "import signal,time; "
                "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                "time.sleep(10)")
            result = gate.run_child(
                [sys.executable, "-c", target], timeout_seconds=0.25,
                output_limit_bytes=4096, term_grace_seconds=0.05)
            self.assertTrue(result["timed_out"])
            self.assertEqual(result["termination_signal"], "SIGKILL")
            self.assertEqual(result["returncode"], -signal.SIGKILL)
            os.kill(sibling.pid, 0)
        finally:
            sibling.terminate()
            try:
                sibling.wait(timeout=2)
            except subprocess.TimeoutExpired:
                sibling.kill()
                sibling.wait()

    def test_output_limit_terminates_and_caps_capture(self):
        producer = (
            "import os\n"
            "chunk=b'x'*4096\n"
            "while True:\n"
            " os.write(1,chunk)\n")
        limit = 8192
        result = gate.run_child(
            [sys.executable, "-c", producer], timeout_seconds=2.0,
            output_limit_bytes=limit, term_grace_seconds=0.05)
        self.assertFalse(result["timed_out"])
        self.assertTrue(result["output_limit_exceeded"])
        self.assertIn("stdout", result["output_limit_streams"])
        self.assertGreater(result["stdout_bytes"], limit)
        self.assertEqual(len(result["stdout"].encode("utf-8")), limit)
        self.assertTrue(result["stdout_truncated"])
        self.assertIn(result["termination_signal"], ("SIGTERM", "SIGKILL"))


class PureDataTests(unittest.TestCase):
    def test_registry_export_inventory_matches_provider_rules(self):
        inventory = gate.inventory_registry_exports({
            "added": [{"address": 0x1000, "weak": False}],
            "equivalent": [
                {"address": 0x2000, "weak": False},
                {"address": 0x2000, "weak": False},
            ],
            "replace": [
                {"address": 0x3000, "weak": True},
                {"address": 0x4000, "weak": False},
            ],
            "ignore": [
                {"address": 0x5000, "weak": False},
                {"address": 0x6000, "weak": True},
            ],
            "collision": [
                {"address": 0x7000, "weak": False},
                {"address": 0x8000, "weak": False},
            ],
        })
        self.assertEqual(inventory, {
            "eligible": 9,
            "added": 5,
            "equivalent": 1,
            "replaced_lower_priority": 1,
            "ignored_lower_priority": 1,
            "collisions": 1,
        })

    def test_export_report_parser_accepts_exact_success(self):
        self.assertEqual(
            gate.parse_export_report(
                "relocate=success\nexports=success added=42 equivalent=3\n",
                "fixture"),
            {"added": 42, "equivalent": 3})

    def test_export_report_parser_rejects_missing_malformed_or_duplicate(self):
        invalid_outputs = (
            "relocate=success\n",
            "exports=success added=x equivalent=0\n",
            "exports=failed added=0 equivalent=0\n",
            "exports=success added=1 equivalent=0 trailing=1\n",
            "exports=success added=1 equivalent=0\n"
            "exports=success added=1 equivalent=0\n",
        )
        for output in invalid_outputs:
            with self.subTest(output=output):
                with self.assertRaises(gate.GateError):
                    gate.parse_export_report(output, "fixture")

    def test_exports_subprocess_is_limited_and_lifecycle_stays_zero(self):
        with tempfile.TemporaryDirectory(prefix="m10-exports-test.") as root:
            fake_inspector = Path(root) / "fake_inspector.py"
            fake_inspector.write_text(
                "import sys\n"
                "if sys.argv[1:] != ['--exports']:\n"
                " raise SystemExit(7)\n"
                "print('arch=AArch64/ELF64 flags=0x0 '"
                "'arm_float_abi=not-applicable image=4096 segments=3 '"
                "'symbols=7 relocations=11 needed=2')\n"
                "print('relocate=success')\n"
                "print('exports=success added=1 equivalent=0')\n",
                encoding="utf-8")
            static_report = {
                "id": "synthetic-guest",
                "pt_load_count": 3,
                "dynamic_symbol_count": 7,
                "relocation_count": 11,
                "needed": ["libc.so", "libm.so"],
                "registry_exports": {
                    "eligible": 1,
                    "added": 1,
                    "equivalent": 0,
                    "replaced_lower_priority": 0,
                    "ignored_lower_priority": 0,
                    "collisions": 0,
                },
            }
            report = gate.inspect_exports(
                sys.executable, str(fake_inspector), static_report,
                [str(fake_inspector)], timeout_seconds=2.0,
                output_limit_bytes=4096)
            self.assertEqual(report["mode"], "--exports")
            self.assertEqual(report["status"], "PASS")
            self.assertEqual(report["added"], 1)
            self.assertEqual(report["equivalent"], 0)
            self.assertEqual(report["registry_create_executed"], 1)
            self.assertEqual(report["registry_add_module_executed"], 1)
            self.assertEqual(report["relocate_executed"], 1)
            for field in (
                    "resolve_executed", "finalize_executed",
                    "guest_initializers_executed",
                    "guest_jni_onload_executed", "device_access",
                    "guest_files_copied"):
                self.assertEqual(report[field], 0)
            self.assertFalse(report["timed_out"])
            self.assertFalse(report["output_limit_exceeded"])
            self.assertFalse(report["stdout_truncated"])
            self.assertFalse(report["stderr_truncated"])

    def test_sanitization_removes_explicit_and_unknown_absolute_paths(self):
        private = "/private/reference/guest.so"
        value = gate.sanitize_text(
            "failed at %s via /another/private/location token\x01" % private,
            [private])
        self.assertNotIn(private, value)
        self.assertNotIn("/another/private/location", value)
        self.assertIn("<redacted-path>", value)
        self.assertNotIn("\x01", value)

    def test_source_snapshot_is_deterministic_ordered_and_path_free(self):
        with tempfile.TemporaryDirectory(prefix="m10-snapshot-test.") as root:
            root_path = Path(root)
            contents = {}
            for index, relative_name in enumerate(gate.SOURCE_SNAPSHOT_PATHS):
                content = ("fixture-%02d:%s\n" %
                           (index, relative_name)).encode("utf-8")
                path = root_path / relative_name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
                contents[relative_name] = content

            first = gate.build_source_snapshot(root_path)
            second = gate.build_source_snapshot(root_path)
            self.assertEqual(first, second)
            self.assertEqual(first["file_count"],
                             len(gate.SOURCE_SNAPSHOT_PATHS))
            self.assertEqual(
                [item["path"] for item in first["files"]],
                list(gate.SOURCE_SNAPSHOT_PATHS))

            expected = hashlib.sha256()
            expected.update(gate.SOURCE_SNAPSHOT_DOMAIN)
            for relative_name in gate.SOURCE_SNAPSHOT_PATHS:
                name = relative_name.encode("utf-8")
                content = contents[relative_name]
                expected.update(struct.pack(">I", len(name)))
                expected.update(name)
                expected.update(struct.pack(">Q", len(content)))
                expected.update(content)
            self.assertEqual(first["sha256"], expected.hexdigest())
            self.assertNotIn(root, json.dumps(first, sort_keys=True))

            changed_path = root_path / gate.SOURCE_SNAPSHOT_PATHS[0]
            changed_path.write_bytes(contents[gate.SOURCE_SNAPSHOT_PATHS[0]] +
                                     b"changed\n")
            changed = gate.build_source_snapshot(root_path)
            self.assertNotEqual(first["sha256"], changed["sha256"])


if __name__ == "__main__":
    unittest.main()
