#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""V3-ABI-01 unit tests: execution roles, mixed closures and GNU ld scripts.

Everything runs on the host with fixtures this file synthesizes itself inside
an owned temporary directory.  No inspected file is ever executed.
"""

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

MODULE_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MODULE_DIR))

import nxabi  # noqa: E402


def minimal_elf_bytes():
    """A plausible little-endian ELF64 header (magic + sane e_ident + pad)."""
    e_ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\x00" * 8
    return e_ident + b"\x00" * 48


class FixtureTree(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.mkdtemp(prefix="nxabi-v3-test-")
        self.root = Path(self.work)

    def tearDown(self):
        if self.work.startswith(tempfile.gettempdir()):
            shutil.rmtree(self.work, ignore_errors=True)

    def write(self, relative, data):
        target = self.root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        mode = "wb" if isinstance(data, bytes) else "w"
        with open(str(target), mode) as handle:
            handle.write(data)
        return target


class ExecutionRoleModelTest(unittest.TestCase):
    def test_enum_is_exactly_the_v3_contract(self):
        self.assertEqual(
            nxabi.EXECUTION_ROLES,
            ("guest", "loader", "extractor", "splash", "adapter", "helper"))

    def test_declared_role_is_validated_not_guessed(self):
        self.assertEqual(
            nxabi.classify_execution_role("bin/mygame", "guest"), "guest")
        self.assertEqual(
            nxabi.classify_execution_role("bin/shim", "adapter"), "adapter")
        with self.assertRaises(nxabi.AbiError):
            nxabi.classify_execution_role("bin/mygame", "pilot")

    def test_nxrelease_game_alias_normalizes_to_guest(self):
        self.assertEqual(
            nxabi.classify_execution_role("bin/mygame", "game"), "guest")

    def test_documented_conventions(self):
        self.assertEqual(
            nxabi.classify_execution_role("nxsplash-armhf"), "splash")
        self.assertEqual(
            nxabi.classify_execution_role("dir/nxextract-ui"), "extractor")

    def test_nextos_suffix_needs_a_declaration(self):
        with self.assertRaises(nxabi.AbiError):
            nxabi.classify_execution_role("chrono-nextos")
        self.assertEqual(
            nxabi.classify_execution_role("chrono-nextos", "loader"), "loader")
        self.assertEqual(
            nxabi.classify_execution_role("chrono-nextos", "guest"), "guest")
        with self.assertRaises(nxabi.AbiError):
            nxabi.classify_execution_role("chrono-nextos", "splash")

    def test_declaration_conflicting_with_convention_fails(self):
        with self.assertRaises(nxabi.AbiError):
            nxabi.classify_execution_role("nxsplash-armhf", "guest")

    def test_unconventional_name_without_declaration_fails_closed(self):
        with self.assertRaises(nxabi.AbiError):
            nxabi.classify_execution_role("random-binary")


class ValidateRoleAbiTest(unittest.TestCase):
    def errors(self, findings):
        return [item for item in findings if item["level"] == "error"]

    def test_each_role_is_validated_in_isolation(self):
        for role, architecture in (
                ("guest", "armv7"), ("loader", "aarch64"),
                ("extractor", "aarch64"), ("splash", "armv7"),
                ("adapter", "armv7"), ("helper", "aarch64")):
            spec = nxabi.ROLE_ARCH_SPECS[architecture]
            findings = nxabi.validate_role_abi(role, {
                "architecture": architecture,
                "class": spec["elf_class"],
                "interpreter": spec["interpreter"],
            }, host_abi="aarch64")
            self.assertEqual(self.errors(findings), [], role)

    def test_helper_never_inherits_the_game_abi(self):
        findings = nxabi.validate_role_abi(
            "helper", {"class": "ELF64"}, host_abi="aarch64")
        errors = self.errors(findings)
        self.assertEqual([item["check"] for item in errors], ["role-abi"])
        self.assertIn("never inherits", errors[0]["message"])

    def test_role_abi_may_differ_from_the_host_abi(self):
        spec = nxabi.ROLE_ARCH_SPECS["armv7"]
        findings = nxabi.validate_role_abi("guest", {
            "architecture": "armv7",
            "class": spec["elf_class"],
            "interpreter": spec["interpreter"],
        }, host_abi="aarch64")
        self.assertEqual(self.errors(findings), [])

    def test_class_and_interpreter_mismatch_fail(self):
        findings = nxabi.validate_role_abi("splash", {
            "architecture": "armv7",
            "class": "ELF64",
            "interpreter": "/lib/ld-linux-aarch64.so.1",
        }, host_abi="aarch64")
        checks = {item["check"] for item in self.errors(findings)}
        self.assertEqual(checks, {"role-elf-class", "role-interpreter"})

    def test_third_abi_needs_explicit_declaration(self):
        incomplete = nxabi.validate_role_abi(
            "splash", {"architecture": "riscv64"}, host_abi="aarch64")
        self.assertEqual(
            [item["check"] for item in self.errors(incomplete)],
            ["role-abi-declaration"])
        complete = nxabi.validate_role_abi("splash", {
            "architecture": "riscv64",
            "class": "ELF64",
            "interpreter": "/lib/ld-linux-riscv64-lp64d.so.1",
        }, host_abi="aarch64")
        self.assertEqual(self.errors(complete), [])


class MixedClosureAuditTest(unittest.TestCase):
    def mixed_roles(self):
        return {
            "loader": {
                "abi": "aarch64",
                "interpreter": "/lib/ld-linux-aarch64.so.1",
                "needed": ["libc.so.6", "libnxboot.so"],
            },
            "guest": {
                "abi": "armv7",
                "interpreter": "/lib/ld-linux-armhf.so.3",
                "needed": ["libc.so.6", "libSDL2-2.0.so.0"],
            },
            "splash": {
                "abi": "riscv64",
                "interpreter": "/lib/ld-linux-riscv64-lp64d.so.1",
                "needed": ["libc.so.6", "libpng16.so.16"],
            },
        }

    def test_mixed_three_abi_closure_passes_when_self_consistent(self):
        findings = nxabi.audit_mixed_closure(
            self.mixed_roles(),
            physical_libs={"libnxboot.so", "libSDL2-2.0.so.0",
                           "libpng16.so.16"},
            firmware_contract={"aarch64": {"libc.so.6"},
                               "armv7": {"libc.so.6"},
                               "riscv64": {"libc.so.6"}})
        self.assertEqual(findings, [])

    def test_missing_interpreter_is_a_named_failure(self):
        roles = self.mixed_roles()
        del roles["splash"]["interpreter"]
        findings = nxabi.audit_mixed_closure(
            roles,
            physical_libs={"libnxboot.so", "libSDL2-2.0.so.0",
                           "libpng16.so.16"},
            firmware_contract={"libc.so.6"})
        named = [item for item in findings
                 if item["check"] == "role-interpreter"]
        self.assertEqual(len(named), 1)
        self.assertEqual(named[0]["role"], "splash")
        self.assertIn("splash", named[0]["message"])

    def test_nothing_is_assumed_by_custom(self):
        # libudev is a classic "everyone has it" assumption; without a caller
        # declaration it must NOT resolve.
        roles = {"guest": {
            "abi": "armv7",
            "interpreter": "/lib/ld-linux-armhf.so.3",
            "needed": ["libudev.so.1"],
        }}
        findings = nxabi.audit_mixed_closure(roles, physical_libs=set())
        self.assertEqual(
            [item["check"] for item in findings], ["closure-unresolved"])
        declared = nxabi.audit_mixed_closure(
            roles, physical_libs=set(), firmware_contract={"libudev.so.1"})
        self.assertEqual(declared, [])

    def test_unknown_role_name_is_rejected(self):
        findings = nxabi.audit_mixed_closure(
            {"pilot": {"abi": "armv7", "interpreter": "none", "needed": []}},
            physical_libs=set())
        self.assertEqual([item["check"] for item in findings], ["role-name"])

    def test_static_role_declares_interpreter_none(self):
        findings = nxabi.audit_mixed_closure(
            {"extractor": {"abi": "aarch64", "interpreter": "none",
                           "needed": []}},
            physical_libs=set())
        self.assertEqual(findings, [])


class ClassifyLinkerInputTest(FixtureTree):
    def test_real_elf_header_is_elf(self):
        target = self.write("bin/game", minimal_elf_bytes())
        self.assertEqual(nxabi.classify_linker_input(target), "elf")

    def test_ld_script_is_linker_script_not_invalid_elf(self):
        target = self.write(
            "lib/libc.so",
            "/* GNU ld script */\n"
            "GROUP ( libc.so.6 AS_NEEDED ( ld-linux-armhf.so.3 ) )\n")
        self.assertEqual(nxabi.classify_linker_input(target), "linker-script")

    def test_output_format_alone_is_recognized(self):
        target = self.write(
            "lib/fmt.so", 'OUTPUT_FORMAT(elf32-littlearm)\n')
        self.assertEqual(nxabi.classify_linker_input(target), "linker-script")

    def test_text_symlink_truncated_and_junk(self):
        text = self.write("notes.txt", "just some notes\n")
        self.assertEqual(nxabi.classify_linker_input(text), "text")
        truncated = self.write("bin/chopped", b"\x7fELF")
        self.assertEqual(nxabi.classify_linker_input(truncated), "invalid")
        junk = self.write("bin/junk", b"\x00\x01\x02\x03garbage")
        self.assertEqual(nxabi.classify_linker_input(junk), "invalid")
        link = self.root / "lib" / "liblink.so"
        link.parent.mkdir(parents=True, exist_ok=True)
        os.symlink(str(text), str(link))
        self.assertEqual(nxabi.classify_linker_input(link), "symlink")

    def test_require_elf_never_calls_a_script_invalid_elf(self):
        script = self.write(
            "lib/libc.so",
            "GROUP ( libc.so.6 AS_NEEDED ( ld-linux-armhf.so.3 ) )\n")
        with self.assertRaises(nxabi.AbiError) as context:
            nxabi.require_elf(script)
        message = str(context.exception)
        self.assertIn("linker script", message)
        self.assertNotIn("invalid ELF", message)
        # A genuinely wrong ELF still fails, on its own grounds.
        truncated = self.write("bin/chopped", b"\x7fELF")
        with self.assertRaises(nxabi.AbiError) as context:
            nxabi.require_elf(truncated)
        self.assertIn("truncated", str(context.exception))


class ResolveLinkerScriptTest(FixtureTree):
    def make_sysroot(self):
        sysroot = self.root / "sysroot"
        self.write("sysroot/lib/libc.so.6", minimal_elf_bytes())
        self.write("sysroot/lib/ld-linux-armhf.so.3", minimal_elf_bytes())
        return sysroot

    def test_group_with_as_needed_resolves_in_order(self):
        sysroot = self.make_sysroot()
        script = self.write(
            "sysroot/lib/libc.so",
            "/* GNU ld script */\n"
            "OUTPUT_FORMAT(elf32-littlearm)\n"
            "GROUP ( libc.so.6 AS_NEEDED ( ld-linux-armhf.so.3 ) )\n")
        closure = nxabi.resolve_linker_script(script, [sysroot / "lib"])
        self.assertEqual(
            [entry["member"] for entry in closure],
            ["libc.so.6", "ld-linux-armhf.so.3"])
        self.assertEqual(
            [entry["as_needed"] for entry in closure], [False, True])
        for entry in closure:
            self.assertEqual(entry["origin"], str(sysroot / "lib"))
            self.assertTrue(entry["path"].startswith(str(sysroot)))
            self.assertEqual(entry["via"], str(script))

    def test_absolute_member_is_rerooted_inside_the_sysroot(self):
        sysroot = self.make_sysroot()
        script = self.write(
            "sysroot/lib/libabs.so", "GROUP ( /lib/libc.so.6 )\n")
        closure = nxabi.resolve_linker_script(script, [sysroot])
        self.assertEqual(closure[0]["member"], "/lib/libc.so.6")
        self.assertTrue(closure[0]["path"].startswith(str(sysroot)))

    def test_unresolvable_member_fails_closed(self):
        sysroot = self.make_sysroot()
        script = self.write(
            "sysroot/lib/libmiss.so", "GROUP ( libnothere.so.9 )\n")
        with self.assertRaises(nxabi.AbiError) as context:
            nxabi.resolve_linker_script(script, [sysroot / "lib"])
        self.assertIn("libnothere.so.9", str(context.exception))

    def test_traversal_outside_the_search_roots_fails(self):
        sysroot = self.make_sysroot()
        self.write("outside/libevil.so.1", minimal_elf_bytes())
        script = self.write(
            "sysroot/lib/libtrav.so", "GROUP ( ../../outside/libevil.so.1 )\n")
        with self.assertRaises(nxabi.AbiError) as context:
            nxabi.resolve_linker_script(script, [sysroot / "lib"])
        self.assertIn("escapes", str(context.exception))

    def test_backticks_and_shell_metacharacters_are_rejected(self):
        sysroot = self.make_sysroot()
        for payload in ("GROUP ( `rm -rf /` )\n",
                        "GROUP ( libc.so.6 ) ; echo pwned\n",
                        "GROUP ( $(rm-x) )\n"):
            script = self.write("sysroot/lib/libbad.so", payload)
            with self.assertRaises(nxabi.AbiError) as context:
                nxabi.resolve_linker_script(script, [sysroot / "lib"])
            self.assertIn("forbidden shell metacharacter",
                          str(context.exception))

    def test_recursion_bomb_is_rejected(self):
        sysroot = self.root / "sysroot"
        self.write("sysroot/lib/liba.so", "GROUP ( libb.so )\n")
        self.write("sysroot/lib/libb.so", "GROUP ( liba.so )\n")
        with self.assertRaises(nxabi.AbiError) as context:
            nxabi.resolve_linker_script(
                sysroot / "lib" / "liba.so", [sysroot / "lib"])
        self.assertIn("recursion", str(context.exception))

    def test_nested_script_within_depth_resolves(self):
        sysroot = self.root / "sysroot"
        self.write("sysroot/lib/libreal.so.6", minimal_elf_bytes())
        self.write("sysroot/lib/libinner.so", "GROUP ( libreal.so.6 )\n")
        self.write("sysroot/lib/libouter.so",
                   "GROUP ( AS_NEEDED ( libinner.so ) )\n")
        closure = nxabi.resolve_linker_script(
            sysroot / "lib" / "libouter.so", [sysroot / "lib"])
        self.assertEqual(len(closure), 1)
        self.assertEqual(closure[0]["member"], "libreal.so.6")
        self.assertTrue(closure[0]["as_needed"])


if __name__ == "__main__":
    result = unittest.main(verbosity=2, exit=False).result
    if not result.wasSuccessful():
        raise SystemExit(1)
    print("test_v3_roles: PASS V3-ABI-01 "
          "(execution roles + mixed closures + GNU ld script recognition)")
