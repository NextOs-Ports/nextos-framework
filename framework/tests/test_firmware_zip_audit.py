#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Owned synthetic fixtures for the fast PortMaster ZIP preflight."""

import subprocess
import sys
import tempfile
import unittest
import zipfile
import json
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
AUDITOR = REPOSITORY / "framework/tests/audit-portmaster-zip.py"
REFERENCE_ELF = REPOSITORY / "ports/kotor/kotor-universal"


class FirmwareZipAuditTests(unittest.TestCase):
    def run_audit(self, archive):
        return subprocess.run(
            [sys.executable, "-B", str(AUDITOR), str(archive)],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, check=False,
        )

    @staticmethod
    def write_zip(path, members):
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as archive:
            for name, payload in members:
                archive.writestr(name, payload)

    @staticmethod
    def valid_metadata():
        return (json.dumps({
            "attr": {
                "arch": ["aarch64"],
                "min_glibc": "2.17",
                "runtime": None,
                "title": "Fixture",
            },
            "items": ["Fixture.sh", "fixture/"],
            "items_opt": [],
            "name": "fixture.zip",
            "version": 1,
        }, sort_keys=True) + "\n").encode("utf-8")

    def valid_contract_members(self):
        return [
            ("fixture/INSTALLATION.md", b"# Installation / Instalacao\n"),
            ("fixture/port.json", self.valid_metadata()),
        ]

    def test_safe_shell_and_low_glibc_elf_pass(self):
        self.assertTrue(REFERENCE_ELF.is_file())
        with tempfile.TemporaryDirectory(prefix="firmware-zip-test.") as root:
            archive = Path(root) / "safe.zip"
            self.write_zip(archive, [
                ("Fixture.sh", b"#!/bin/bash\necho fixture\n"),
                ("fixture/helper.sh", b"#!/bin/sh\nprintf '%s\\n' ok\n"),
                ("fixture/fixture-nextos", REFERENCE_ELF.read_bytes()),
            ] + self.valid_contract_members())
            result = self.run_audit(archive)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("result=profile-contract-pass", result.stdout)
            self.assertIn("hardware_ran=0", result.stdout)
            self.assertIn("device_access=0", result.stdout)

    def test_external_stat_fails(self):
        with tempfile.TemporaryDirectory(prefix="firmware-zip-test.") as root:
            cases = {
                "flag": b"#!/bin/bash\nif stat -c '%a' file; then true; fi\n",
                "argument": b"#!/bin/sh\nstat file\n",
                "bare": b"#!/bin/dash\nstat\n",
                "env": b"#!/bin/ash\nenv stat file\n",
                "command": b"#!/bin/mksh\ncommand stat file\n",
                "builtin": b"#!/bin/ksh\nbuiltin stat file\n",
                "busybox": b"#!/bin/hush\nbusybox stat file\n",
                "exec": b"#!/bin/sh\nexec stat file\n",
                "nohup": b"#!/bin/sh\nnohup stat file\n",
                "sudo": b"#!/bin/sh\nsudo -u root stat file\n",
                "nice": b"#!/bin/sh\nnice -n 5 stat file\n",
                "xargs": b"#!/bin/sh\nprintf x | xargs stat\n",
                "find-exec": b"#!/bin/sh\nfind . -exec stat {} \\;\n",
                "shell-c": b"#!/bin/sh\nsh -c 'stat file'\n",
            }
            for label, payload in cases.items():
                with self.subTest(label=label):
                    archive = Path(root) / ("stat-" + label + ".zip")
                    self.write_zip(archive, [("Fixture.sh", payload)])
                    result = self.run_audit(archive)
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("external stat", result.stderr)

            extensionless_cases = {
                "sh": b"#!/bin/sh\ncommand stat file\n",
                "ash": b"#!/bin/ash\ncommand stat file\n",
                "mksh": b"#!/bin/mksh\ncommand stat file\n",
                "hush": b"#!/bin/hush\ncommand stat file\n",
                "env-busybox-sh": (
                    b"#!/usr/bin/env busybox sh\ncommand stat file\n"
                ),
            }
            for label, payload in extensionless_cases.items():
                with self.subTest(extensionless=label):
                    archive = Path(root) / (
                        "extensionless-stat-" + label + ".zip"
                    )
                    self.write_zip(archive, [
                        ("Fixture.sh", b"#!/bin/bash\ntrue\n"),
                        ("fixture/helper", payload),
                    ])
                    result = self.run_audit(archive)
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("external stat", result.stderr)

    def test_legacy_bootstrap_fails(self):
        with tempfile.TemporaryDirectory(prefix="firmware-zip-test.") as root:
            archive = Path(root) / "legacy-bootstrap.zip"
            self.write_zip(archive, [
                ("Fixture.sh", b"#!/bin/bash\ntrue\n"),
                ("fixture/nxbootstrap.sh", b"#!/bin/bash\ntrue\n"),
            ])
            result = self.run_audit(archive)
            self.assertEqual(result.returncode, 1)
            self.assertIn("retired legacy nxbootstrap.sh", result.stderr)

    def test_bad_shell_syntax_fails(self):
        with tempfile.TemporaryDirectory(prefix="firmware-zip-test.") as root:
            archive = Path(root) / "syntax.zip"
            self.write_zip(archive, [
                ("Fixture.sh", b"#!/bin/bash\nif true; then\n"),
            ])
            result = self.run_audit(archive)
            self.assertEqual(result.returncode, 1)
            self.assertIn("shell syntax error", result.stderr)

    def test_owner_data_source_identity_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="firmware-zip-test.") as root:
            cases = {
                "member": (
                    "fixture/APKPure/NOTICE.md",
                    b"Owner data stays outside this ZIP.\n",
                ),
                "site-text": (
                    "fixture/INSTALLATION.md",
                    b"Download source: APK Mirror\n",
                ),
                "mod-filename": (
                    "fixture/INSTALLATION.md",
                    b"Use hitman-go-mod_1.18.1.apk\n",
                ),
            }
            for label, public_member in cases.items():
                with self.subTest(label=label):
                    archive = Path(root) / ("owner-source-" + label + ".zip")
                    self.write_zip(archive, [
                        ("Fixture.sh", b"#!/bin/bash\ntrue\n"),
                        public_member,
                    ])
                    result = self.run_audit(archive)
                    self.assertEqual(result.returncode, 1)
                    self.assertIn("owner-data source", result.stderr)

            safe = Path(root) / "technical-mod-context.zip"
            self.write_zip(safe, [
                ("Fixture.sh", b"#!/bin/bash\ntrue\n"),
                ("fixture/README.md",
                 b"The adapter modifies only its private input state.\n"),
            ] + self.valid_contract_members())
            result = self.run_audit(safe)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_traversal_and_multiple_launchers_fail(self):
        with tempfile.TemporaryDirectory(prefix="firmware-zip-test.") as root:
            traversal = Path(root) / "traversal.zip"
            self.write_zip(traversal, [
                ("../Fixture.sh", b"#!/bin/bash\ntrue\n"),
            ])
            result = self.run_audit(traversal)
            self.assertEqual(result.returncode, 1)
            self.assertIn("normalized relative path", result.stderr)

            multiple = Path(root) / "multiple.zip"
            self.write_zip(multiple, [
                ("One.sh", b"#!/bin/bash\ntrue\n"),
                ("Two.sh", b"#!/bin/bash\ntrue\n"),
            ])
            result = self.run_audit(multiple)
            self.assertEqual(result.returncode, 1)
            self.assertIn("exactly one top-level launcher", result.stderr)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(
        FirmwareZipAuditTests
    )
    outcome = unittest.TextTestRunner(verbosity=2).run(suite)
    if outcome.wasSuccessful():
        print("firmware ZIP fixture gate passed: cases=6 "
              "hardware_ran=0 device_access=0 firmware_images_used=0")
    raise SystemExit(not outcome.wasSuccessful())
