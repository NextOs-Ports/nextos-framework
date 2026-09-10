#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V4-04B/0.3.25: structural scanner gate, now WIRED into nxrelease.

Proves the ast/JSON scanner over the adversarial corpus: annotation with
None passes, comments/docstrings/innocent strings pass, real
Assign/AnnAssign/dict-nested credentials fail, invalid Python/JSON is never
declared clean, an unsupported type is explicit UNSUPPORTED (so the caller's
existing regex scanner keeps owning it), and no finding ever echoes the
secret value.  Also proves nxscan's sensitive-name pattern is byte-identical
to the live nxrelease SECRET_NAME_PATTERN, so foundation and live scanner
can never disagree about what a sensitive name is.

0.3.25 replaces the old ``test_unwired`` negative with positive wiring
regressions: nxrelease loads nxscan, the structural verdict decides the
languages nxscan understands, UNSUPPORTED/STRUCTURAL_ERROR still fall back to
the historical fail-closed regex authority, and the field false positive that
motivated the adapter (a Python ``name: Type`` annotation) no longer trips the
live scanner while a real assignment still does."""

import importlib.util
import pathlib
import sys
import unittest

TESTS_DIR = pathlib.Path(__file__).resolve().parent
NXRELEASE_ROOT = TESTS_DIR.parent
CORPUS = TESTS_DIR / "corpus-nxscan"

SECRET_VALUES = ("hunter2secret", "ABCDEF123456", "0123456789abcdef")


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, str(path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


nxscan = load_module(NXRELEASE_ROOT / "nxscan.py", "nxscan_gate")

EXPECTED = {
    "py-annotation-none.py": ("python", nxscan.PASS),
    "py-comment.py": ("python", nxscan.PASS),
    "py-docstring.py": ("python", nxscan.PASS),
    "py-innocent-string.py": ("python", nxscan.PASS),
    "py-assign-real.py": ("python", nxscan.FAIL),
    "py-annassign-real.py": ("python", nxscan.FAIL),
    "py-bytes-real.py": ("python", nxscan.FAIL),
    "py-dict-nested.py": ("python", nxscan.FAIL),
    "py-invalid-syntax.py": ("python", nxscan.STRUCTURAL_ERROR),
    "json-secret-nested.json": ("json", nxscan.FAIL),
    "json-innocent-value.json": ("json", nxscan.PASS),
    "json-similar-key.json": ("json", nxscan.PASS),
    "json-duplicate.json": ("json", nxscan.STRUCTURAL_ERROR),
    "json-invalid.json": ("json", nxscan.STRUCTURAL_ERROR),
    "unsupported.txt": ("unknown", nxscan.UNSUPPORTED),
}


class CorpusTest(unittest.TestCase):
    def test_every_corpus_case(self):
        for name, (language, status) in sorted(EXPECTED.items()):
            case = CORPUS / name
            self.assertTrue(case.is_file(), "missing corpus case " + name)
            result = nxscan.scan_bytes(name, case.read_bytes())
            self.assertEqual(result["schema"], nxscan.SCAN_SCHEMA)
            self.assertEqual(result["language"], language, name)
            self.assertEqual(result["status"], status,
                             (name, result["findings"]))
            for finding in result["findings"]:
                text = finding["message"] + finding["code"]
                for secret in SECRET_VALUES:
                    self.assertNotIn(secret, text,
                                     "secret value echoed for " + name)

    def test_corpus_is_complete(self):
        on_disk = {item.name for item in CORPUS.iterdir()}
        self.assertEqual(on_disk, set(EXPECTED),
                         "corpus and expectations drifted")


class ContractTest(unittest.TestCase):
    def test_sensitive_name_pattern_matches_live_nxrelease(self):
        text = (NXRELEASE_ROOT / "nxrelease.py").read_text(encoding="utf-8")
        needle = 'rb"' + nxscan.SENSITIVE_NAME_PATTERN + '"'
        self.assertIn(needle.replace('rb"', ''), text.replace('rb"', ''))
        self.assertIn(nxscan.SENSITIVE_NAME_PATTERN, text,
                      "nxscan sensitive-name pattern is no longer "
                      "byte-identical to nxrelease SECRET_NAME_PATTERN")

    def test_result_identity_and_read_only(self):
        result = nxscan.scan_bytes("x.json", b"{}")
        self.assertEqual(result["status"], nxscan.PASS)
        self.assertEqual(len(result["scanner_sha256"]), 64)

    def test_misuse_is_structural(self):
        self.assertEqual(nxscan.scan_bytes("", b"{}")["status"],
                         nxscan.STRUCTURAL_ERROR)
        self.assertEqual(nxscan.scan_bytes("x.json", "not-bytes")["status"],
                         nxscan.STRUCTURAL_ERROR)
        oversized = b"x" * (nxscan.MAX_SCAN_BYTES + 1)
        self.assertEqual(nxscan.scan_bytes("x.json", oversized)["status"],
                         nxscan.STRUCTURAL_ERROR)

    def test_deep_json_is_structural_not_crash(self):
        deep = ("[" * 3000 + "]" * 3000).encode()
        result = nxscan.scan_bytes("deep.json", deep)
        self.assertEqual(result["status"], nxscan.STRUCTURAL_ERROR)

    def test_bom_is_explicit(self):
        result = nxscan.scan_bytes("bom.json", b"\xef\xbb\xbf{}")
        self.assertEqual(result["status"], nxscan.STRUCTURAL_ERROR)
        self.assertEqual(result["findings"][0]["code"], "JSON-BOM")


nxrelease = load_module(NXRELEASE_ROOT / "nxrelease.py", "nxrelease_scan_gate")


class WiringTest(unittest.TestCase):
    """0.3.25: the adapter is live and the fallback never got weaker."""

    def test_nxrelease_loaded_the_structural_scanner(self):
        self.assertIs(nxrelease.NXSCAN.SCAN_SCHEMA.__class__, str)
        self.assertEqual(nxrelease.NXSCAN.SCAN_SCHEMA, nxscan.SCAN_SCHEMA)
        self.assertEqual(nxrelease.NXSCAN_PATH, NXRELEASE_ROOT / "nxscan.py")

    def test_structural_fail_rejects_through_the_live_scanner(self):
        """Every corpus FAIL is proven by the grammar and reaches nxrelease."""
        proven = 0
        for name, (_language, status) in sorted(EXPECTED.items()):
            data = (CORPUS / name).read_bytes()
            verdict = nxrelease._structural_secret_verdict(data, name)
            if status == nxscan.FAIL:
                self.assertIs(verdict, True, "not rejected: " + name)
                self.assertTrue(
                    nxrelease._contains_secret_literal(data, name),
                    "live scanner did not reject " + name)
                proven += 1
            else:
                self.assertIsNone(verdict,
                                  "structural PASS must never acquit " + name)
        self.assertGreaterEqual(proven, 4)

    def test_adapter_is_strictly_additive(self):
        """The regex authority is never weakened: everything it rejected
        before the adapter is still rejected after it."""
        for payload, path in (
                (b"password=abcdefgh\n", "settings.py"),
                (b"api_key = ABCDEF123456\n", "notes.txt"),
                (b"secret: 0123456789abcdef\n", "conf.yaml"),
                (b"private_key=abcdefgh\n", "x.cfg")):
            self.assertTrue(nxrelease._contains_secret_literal(payload, path),
                            "regex authority regressed for " + path)

    def test_structural_catches_what_the_regex_cannot(self):
        """A quoted Python credential: invisible to the regex, proven by ast."""
        quoted = b'password = "hunter2secret"\n'
        self.assertIsNone(nxrelease.SECRET_LITERAL_RE.search(quoted))
        self.assertIs(
            nxrelease._structural_secret_verdict(quoted, "gen.py"), True)
        self.assertTrue(nxrelease._contains_secret_literal(quoted, "gen.py"))

    def test_unsupported_falls_back_to_the_regex_authority(self):
        # nxscan does not parse .txt: it must NOT decide, and the historical
        # fail-closed regex must still catch the credential.
        payload = b"password = hunter2secret\n"
        self.assertIsNone(
            nxrelease._structural_secret_verdict(payload, "notes.txt"))
        self.assertTrue(
            nxrelease._contains_secret_literal(payload, "notes.txt"))

    def test_structural_error_falls_back_and_stays_fail_closed(self):
        broken = (CORPUS / "py-invalid-syntax.py").read_bytes()
        self.assertIsNone(
            nxrelease._structural_secret_verdict(broken, "broken.py"))
        # Unparseable Python is never declared clean by intuition: the regex
        # authority still owns the verdict for it.
        self.assertEqual(
            nxrelease._contains_secret_literal(broken, "broken.py"),
            nxrelease.SECRET_LITERAL_RE.search(broken) is not None or
            nxrelease.PYTHON_ANNOTATED_SECRET_LITERAL_RE.search(broken)
            is not None)

    def test_bom_duplicate_and_nan_json_fall_back_not_pass(self):
        for payload in (b"\xef\xbb\xbf{}",
                        b'{"a": 1, "a": 2}',
                        b'{"x": NaN}'):
            self.assertIsNone(
                nxrelease._structural_secret_verdict(payload, "x.json"),
                "a structurally broken JSON must not be decided structurally")

    def test_python_annotation_false_positive_stays_gone(self):
        # The field case: generated bindings declare a type, not a secret.
        # The adapter must not reintroduce it.
        annotation = b"class C:\n    m_VCPassword: Optional[str] = None\n"
        self.assertIsNone(
            nxrelease._structural_secret_verdict(annotation, "gen.py"))
        self.assertFalse(
            nxrelease._contains_secret_literal(annotation, "gen.py"))

    def test_json_credential_still_fails_through_the_adapter(self):
        real = b'{"api_key": "ABCDEF123456"}'
        self.assertTrue(nxrelease._contains_secret_literal(real, "c.json"))

    def test_no_finding_echoes_a_secret_value(self):
        real = b'{"api_key": "ABCDEF123456"}'
        blob = repr(nxscan.scan_bytes("c.json", real))
        for value in SECRET_VALUES:
            self.assertNotIn(value, blob)

    def test_scanner_crash_degrades_to_regex_not_to_pass(self):
        class Boom(object):
            SCAN_SCHEMA = nxscan.SCAN_SCHEMA
            PASS = nxscan.PASS
            FAIL = nxscan.FAIL

            @staticmethod
            def scan_bytes(*_args, **_kwargs):
                raise RuntimeError("scanner exploded")

        original = nxrelease.NXSCAN
        nxrelease.NXSCAN = Boom
        try:
            # Unquoted on purpose: this is exactly the shape the historical
            # regex authority catches, so a scanner crash must still fail.
            payload = b"password = hunter2secret\n"
            self.assertIsNone(
                nxrelease._structural_secret_verdict(payload, "x.py"))
            self.assertTrue(
                nxrelease._contains_secret_literal(payload, "x.py"))
        finally:
            nxrelease.NXSCAN = original


if __name__ == "__main__":
    unittest.main(verbosity=1)
