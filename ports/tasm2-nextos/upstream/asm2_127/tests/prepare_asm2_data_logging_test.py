#!/usr/bin/env python3
"""Regression tests for privacy-safe ASM2 owner-source diagnostics."""

from contextlib import redirect_stdout
import hashlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "package" / "sources" / "tools"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location(
    "prepare_asm2_data", TOOLS / "prepare_asm2_data.py"
)
assert SPEC is not None and SPEC.loader is not None
PREPARE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = PREPARE
SPEC.loader.exec_module(PREPARE)


def profile_for(payload: bytes, *, digest: str | None = None):
    return PREPARE.SourceProfile(
        identifier="test-profile",
        description="test profile",
        source_size=len(payload),
        source_sha256=digest or hashlib.sha256(payload).hexdigest(),
        runtime_mode="copy",
        runtime_size=len(payload),
        runtime_sha256=hashlib.sha256(payload).hexdigest(),
    )


class SourceDiagnosticsTest(unittest.TestCase):
    def run_scan(self, payload: bytes, profile, source_name: str):
        with tempfile.TemporaryDirectory() as temporary:
            game_dir = Path(temporary)
            gamedata = game_dir / "gamedata"
            gamedata.mkdir()
            source = gamedata / source_name
            source.write_bytes(payload)
            output = io.StringIO()
            with mock.patch.object(PREPARE, "SOURCE_PROFILES", (profile,)):
                with redirect_stdout(output):
                    result = PREPARE.find_source_package(
                        game_dir, "armeabi-v7a"
                    )
            return result[0], result[1].name, output.getvalue()

    def test_accepted_source_logs_identity_without_filename(self):
        payload = b"supported-owner-source"
        private_name = "private-origin-name.apk"
        selected, selected_name, output = self.run_scan(
            payload, profile_for(payload), private_name
        )

        self.assertEqual(selected.identifier, "test-profile")
        self.assertEqual(selected_name, private_name)
        self.assertIn(f"size={len(payload)}", output)
        self.assertIn(f"sha256={hashlib.sha256(payload).hexdigest()}", output)
        self.assertIn("status=accepted profiles=test-profile", output)
        self.assertIn("accepted=1", output)
        self.assertNotIn(private_name, output)

    def test_same_size_wrong_hash_reports_rejection_and_expectation(self):
        payload = b"same-size-wrong-content"
        profile = profile_for(payload, digest="0" * 64)
        private_name = "unrecognized-source.apk"

        with tempfile.TemporaryDirectory() as temporary:
            game_dir = Path(temporary)
            gamedata = game_dir / "gamedata"
            gamedata.mkdir()
            (gamedata / private_name).write_bytes(payload)
            output = io.StringIO()
            with mock.patch.object(PREPARE, "SOURCE_PROFILES", (profile,)):
                with redirect_stdout(output):
                    with self.assertRaisesRegex(
                        RuntimeError, r"inspected 1 candidate\(s\), accepted 0"
                    ):
                        PREPARE.find_source_package(
                            game_dir, "armeabi-v7a"
                        )

        text = output.getvalue()
        self.assertIn("reason=sha256-mismatch", text)
        self.assertIn("size_matched_profiles=test-profile", text)
        self.assertIn(f"sha256={hashlib.sha256(payload).hexdigest()}", text)
        self.assertIn(f"sha256={'0' * 64}", text)
        self.assertNotIn(private_name, text)
        self.assertNotIn(temporary, text)

    def test_unknown_apk_size_is_hashed_and_reported(self):
        payload = b"unknown-apk-size"
        profile = profile_for(b"different-supported-size")
        private_name = "another-private-name.apk"

        with tempfile.TemporaryDirectory() as temporary:
            game_dir = Path(temporary)
            gamedata = game_dir / "gamedata"
            gamedata.mkdir()
            (gamedata / private_name).write_bytes(payload)
            output = io.StringIO()
            with mock.patch.object(PREPARE, "SOURCE_PROFILES", (profile,)):
                with redirect_stdout(output):
                    with self.assertRaises(RuntimeError):
                        PREPARE.find_source_package(
                            game_dir, "armeabi-v7a"
                        )

        text = output.getvalue()
        self.assertIn("reason=unsupported-size", text)
        self.assertIn(f"size={len(payload)}", text)
        self.assertIn(f"sha256={hashlib.sha256(payload).hexdigest()}", text)
        self.assertIn(f"supported_sizes={profile.source_size}", text)
        self.assertNotIn(private_name, text)
        self.assertNotIn(temporary, text)


if __name__ == "__main__":
    unittest.main()
