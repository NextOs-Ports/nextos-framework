#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""V3-HARDENING-01: deterministic adversarial corpus replay for nxextract.

Not a fuzzer.  make_axml_corpus.py regenerates the byte-exact corpus into a
tempdir; every file is replayed through the REAL parsers:

  manifest/* -> parse_android_manifest
      ok-*  must return a non-empty package string;
      bad-* must fail with a CONTROLLED error (the module's NXError family
      or the documented ValueError) -- never a raw struct.error,
      UnicodeDecodeError, IndexError or MemoryError, and never hang.

  zip/*      -> zip_classification (+ the safe-member validation path for
      hostile members)
      ok-*  must classify "apk";
      bad-* must never classify "apk", never raise, and finish quickly
      (the engine caps bound nested bombs and huge member counts).
"""
import importlib.util
import os
import struct
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


NX = _load("nxextract_corpus_target", ROOT / "nxextract.py")
GEN = _load("make_axml_corpus", ROOT / "tests" / "make_axml_corpus.py")

# The contract: rejection is allowed ONLY through these controlled types.
CONTROLLED_ERRORS = (NX.NXError, ValueError)
# ...and never through these leaks, even though some subclass ValueError.
FORBIDDEN_ERRORS = (struct.error, UnicodeDecodeError, IndexError, MemoryError)

REPLAY_BUDGET_SECONDS = 30.0  # generous bound: "finished" vs "hung"


class CorpusReplayTest(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls):
        cls._tempdir = tempfile.TemporaryDirectory(prefix="nxextract-corpus-")
        cls.corpus = Path(cls._tempdir.name)
        GEN.write_corpus(str(cls.corpus))

    @classmethod
    def tearDownClass(cls):
        cls._tempdir.cleanup()

    # ------------------------------------------------------------ helpers

    def _files(self, subdir, prefix):
        directory = self.corpus / subdir
        found = sorted(
            path for path in directory.iterdir() if path.name.startswith(prefix)
        )
        self.assertTrue(found, "no %s/%s* corpus files generated" % (subdir, prefix))
        return found

    def _assert_controlled(self, path, error):
        self.assertIsInstance(
            error,
            CONTROLLED_ERRORS,
            "%s leaked an uncontrolled %r" % (path.name, error),
        )
        self.assertNotIsInstance(
            error,
            FORBIDDEN_ERRORS,
            "%s leaked a raw parser internal: %r" % (path.name, error),
        )

    # ------------------------------------------------------- determinism

    def test_generator_is_deterministic(self):
        with tempfile.TemporaryDirectory(prefix="nxextract-corpus-b-") as other:
            GEN.write_corpus(other)
            for path in sorted(self.corpus.rglob("*")):
                if not path.is_file():
                    continue
                relative = path.relative_to(self.corpus)
                twin = Path(other) / relative
                self.assertEqual(
                    path.read_bytes(),
                    twin.read_bytes(),
                    "generator is not byte-stable for %s" % relative,
                )

    def test_corpus_is_complete(self):
        total = sum(1 for path in self.corpus.rglob("*") if path.is_file())
        self.assertGreaterEqual(total, 12)

    # -------------------------------------------------------- manifests

    def test_ok_manifests_return_package(self):
        for path in self._files("manifest", "ok-"):
            package, split = NX.parse_android_manifest(path.read_bytes())
            self.assertIsInstance(package, str, path.name)
            self.assertTrue(package, "%s accepted with empty package" % path.name)
            self.assertIsInstance(split, str, path.name)

    def test_bad_manifests_fail_closed(self):
        for path in self._files("manifest", "bad-"):
            started = time.monotonic()
            with self.assertRaises(
                CONTROLLED_ERRORS,
                msg="%s must be rejected, not accepted" % path.name,
            ) as caught:
                NX.parse_android_manifest(path.read_bytes())
            self._assert_controlled(path, caught.exception)
            self.assertLess(
                time.monotonic() - started,
                REPLAY_BUDGET_SECONDS,
                "%s blew the replay time budget" % path.name,
            )

    # ------------------------------------------------------------- zips

    def test_ok_zips_classify_as_apk(self):
        for path in self._files("zip", "ok-"):
            self.assertEqual(NX.zip_classification(str(path)), "apk", path.name)

    def test_bad_zips_never_classify_apk_and_never_hang(self):
        for path in self._files("zip", "bad-"):
            started = time.monotonic()
            verdict = NX.zip_classification(str(path))  # must not raise
            elapsed = time.monotonic() - started
            self.assertIn(
                verdict,
                ("bundle", "archive", None),
                "%s classified as %r" % (path.name, verdict),
            )
            self.assertNotEqual(verdict, "apk", path.name)
            self.assertLess(
                elapsed,
                REPLAY_BUDGET_SECONDS,
                "%s blew the replay time budget" % path.name,
            )

    def test_traversal_member_is_refused_by_safe_member_path(self):
        # The hostile member itself trips the existing safe-member
        # validation, both directly and through the Archive front door.
        with self.assertRaises(NX.SourceError):
            NX.safe_zip_name("../../../evil.txt")
        bomb = self.corpus / "zip" / "bad-member-traversal.zip"
        with self.assertRaises(NX.SourceError):
            NX.Archive(str(bomb), "zip")

    def test_truncated_eocd_is_a_controlled_none(self):
        broken = self.corpus / "zip" / "bad-zip-eocd-truncated.zip"
        self.assertIsNone(NX.zip_classification(str(broken)))


if __name__ == "__main__":
    unittest.main()
