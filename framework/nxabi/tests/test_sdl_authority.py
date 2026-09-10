#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""V4-03B: single fail-closed SDL symbol->version authority.

nxabi and nxrelease must take the same floor decision from exactly the same
bytes.  This gate proves the strict parser fails closed on every corruption
class, that the real table identifies itself and covers the field case
(SDL_JoystickGetVendor/Product born in 2.0.6, above the universal 2.0.4
floor), and that nxrelease's loader opens the very same authority (same id,
same sha256, same full mapping)."""

import importlib.util
import hashlib
import pathlib
import sys
import tempfile
import unittest

TESTS_DIR = pathlib.Path(__file__).resolve().parent
NXABI_DIR = TESTS_DIR.parent
sys.path.insert(0, str(NXABI_DIR))

import nxabi  # noqa: E402

POLICY_PATH = NXABI_DIR / "policy-v1.json"
NXRELEASE_PATH = NXABI_DIR.parent / "nxrelease" / "nxrelease.py"

GOOD_HEADER = "#% authority: nx-sdl-symbol-floor/1\n"


def load_nxrelease():
    spec = importlib.util.spec_from_file_location(
        "nxrelease_authority_gate", str(NXRELEASE_PATH))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class AuthorityTableTest(unittest.TestCase):
    def setUp(self):
        self.authority = nxabi.load_symbol_authority()

    def test_identity_and_bytes(self):
        self.assertEqual(self.authority["authority"], "nx-sdl-symbol-floor/1")
        raw = nxabi.SDL_AUTHORITY_TABLE.read_bytes()
        self.assertEqual(self.authority["sha256"],
                         hashlib.sha256(raw).hexdigest())
        self.assertGreater(self.authority["symbol_count"], 500)

    def test_field_case_versions(self):
        table = self.authority["table"]
        self.assertEqual(table["SDL_JoystickGetVendor"][0], "2.0.6")
        self.assertEqual(table["SDL_JoystickGetProduct"][0], "2.0.6")
        self.assertEqual(table["SDL_Init"][0], "2.0.0")

    def test_policy_route_matches_direct_route(self):
        policy = nxabi.load_policy(POLICY_PATH)
        by_policy = nxabi.load_sdl_authority_for_policy(policy, POLICY_PATH)
        self.assertEqual(by_policy["sha256"], self.authority["sha256"])
        self.assertEqual(policy["sdl"]["floor"], "2.0.4")


class StrictParserTest(unittest.TestCase):
    def refuse(self, content=None, link_to=None):
        with tempfile.TemporaryDirectory() as raw:
            candidate = pathlib.Path(raw) / "table.tsv"
            if link_to is not None:
                candidate.symlink_to(link_to)
            else:
                candidate.write_text(content, encoding="utf-8")
            with self.assertRaises(nxabi.AbiError):
                nxabi.load_symbol_authority(candidate)

    def test_missing_table(self):
        with tempfile.TemporaryDirectory() as raw:
            with self.assertRaises(nxabi.AbiError):
                nxabi.load_symbol_authority(pathlib.Path(raw) / "missing.tsv")

    def test_symlink(self):
        self.refuse(link_to=nxabi.SDL_AUTHORITY_TABLE)

    def test_missing_authority_directive(self):
        self.refuse(content="SDL_Init\t2.0.0\tsdl2-headers\n")

    def test_wrong_authority_id(self):
        self.refuse(content="#% authority: someone-else/9\n"
                            "SDL_Init\t2.0.0\tsdl2-headers\n")

    def test_duplicate_directive(self):
        self.refuse(content=GOOD_HEADER + GOOD_HEADER +
                            "SDL_Init\t2.0.0\tsdl2-headers\n")

    def test_malformed_row(self):
        self.refuse(content=GOOD_HEADER + "SDL_Init 2.0.0 sdl2-headers\n")

    def test_extra_column(self):
        self.refuse(content=GOOD_HEADER +
                            "SDL_Init\t2.0.0\tsdl2-headers\textra\n")

    def test_invalid_symbol_name(self):
        self.refuse(content=GOOD_HEADER + "NotSDL\t2.0.0\tsdl2-headers\n")

    def test_invalid_version(self):
        self.refuse(content=GOOD_HEADER + "SDL_Init\tnew\tsdl2-headers\n")

    def test_ambiguous_duplicate_symbol(self):
        self.refuse(content=GOOD_HEADER +
                            "SDL_Init\t2.0.0\tsdl2-headers\n"
                            "SDL_Init\t2.0.6\tsdl2-headers\n")

    def test_empty_table(self):
        self.refuse(content=GOOD_HEADER + "# only comments\n")

    def test_not_utf8(self):
        with tempfile.TemporaryDirectory() as raw:
            candidate = pathlib.Path(raw) / "table.tsv"
            candidate.write_bytes(GOOD_HEADER.encode() + b"SDL_\xff\t2.0.0\tx\n")
            with self.assertRaises(nxabi.AbiError):
                nxabi.load_symbol_authority(candidate)

    def test_missing_policy_table_fails(self):
        with self.assertRaises(nxabi.AbiError):
            nxabi.load_sdl_table({"sdl": {}}, POLICY_PATH)


class ConsumerConsistencyTest(unittest.TestCase):
    def test_nxrelease_opens_the_same_authority(self):
        nxrelease = load_nxrelease()
        theirs = nxrelease.load_sdl_symbol_authority()
        ours = nxabi.load_symbol_authority()
        self.assertEqual(theirs["authority"], ours["authority"])
        self.assertEqual(theirs["sha256"], ours["sha256"])
        self.assertEqual(theirs["table"], ours["table"])
        self.assertEqual(nxrelease.SDL_PUBLIC_FLOOR, "2.0.4")
        self.assertEqual(nxrelease.assert_abi_policy_agrees(), "agrees")

    def test_both_consumers_refuse_the_field_case(self):
        """Vendor/Product (SDL 2.0.6) must fail in nxabi AND nxrelease."""
        policy = nxabi.load_policy(POLICY_PATH)
        table = nxabi.load_sdl_table(policy, POLICY_PATH)
        record = {
            "path": "fixture/loader", "sha256": "0" * 64, "size": 1,
            "build_id": "ab", "class": "ELF64", "data": "2's complement, little endian",
            "elf_type": "DYN (Shared object file)", "machine": "AArch64",
            "flags": "0x0", "os_abi": "UNIX - System V",
            "architecture": "aarch64", "namespace": "linux",
            "pt_load_count": 1, "pt_interp": None, "pt_interp_count": 0,
            "pt_gnu_stack": "RW", "needed": ["libSDL2-2.0.so.0"],
            "needed_raw_count": 1, "soname": None, "soname_count": 0,
            "rpath": [], "runpath": [],
            "glibc_versions": [], "glibc_max": None,
            "glibc_floor_symbols": [], "glibcxx_max": None,
            "cxxabi_max": None, "forbidden_version_tokens": [],
            "undefined_symbols": ["SDL_JoystickGetProduct",
                                  "SDL_JoystickGetVendor"],
            "undefined_sdl": ["SDL_JoystickGetProduct",
                              "SDL_JoystickGetVendor"],
            "toolchain_note": ["gcc"],
        }
        findings = nxabi.audit_record(
            record, policy, table, {"profile": "universal-low-glibc"})
        floor_errors = [item["message"] for item in findings
                        if item["check"] == "sdl-floor"
                        and item["level"] == "error"]
        self.assertEqual(len(floor_errors), 2)
        for message in floor_errors:
            self.assertIn("2.0.6", message)
            self.assertIn("2.0.4", message)

        nxrelease = load_nxrelease()
        nxrelease._DYNAMIC_SYMBOLS.clear()
        nxrelease._DYNAMIC_SYMBOLS["fixture/loader"] = (
            {"SDL_JoystickGetVendor"}, set())
        with self.assertRaises(nxrelease.ReleaseError) as context:
            nxrelease.validate_symbol_floor(
                [{"path": "fixture/loader",
                  "needed": ("libSDL2-2.0.so.0",), "soname": None}], {})
        self.assertIn("SDL_JoystickGetVendor", str(context.exception))
        self.assertIn("2.0.6", str(context.exception))
        self.assertIn("2.0.4", str(context.exception))
        nxrelease._DYNAMIC_SYMBOLS.clear()


if __name__ == "__main__":
    unittest.main(verbosity=2)
