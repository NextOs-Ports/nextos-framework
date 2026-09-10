#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for the M17 ABI gate.

Everything here runs on the host, builds only its own fixtures inside an owned
temporary directory and never executes an inspected ELF.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

MODULE_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MODULE_DIR))

import nxabi  # noqa: E402

POLICY_PATH = MODULE_DIR / "policy-v1.json"
PIN_PATH = MODULE_DIR / "TOOLCHAIN-PIN.json"


def base_record(**overrides):
    record = {
        "path": "fixture",
        "sha256": "0" * 64,
        "size": 1024,
        "build_id": "deadbeef",
        "class": "ELF32",
        "data": "2's complement, little endian",
        "elf_type": "EXEC (Executable file)",
        "machine": "ARM",
        "flags": "0x5000400, Version5 EABI, hard-float ABI",
        "os_abi": "UNIX - System V",
        "architecture": "armv7",
        "namespace": "linux",
        "pt_load_count": 2,
        "pt_interp": "/lib/ld-linux-armhf.so.3",
        "pt_interp_count": 1,
        "pt_gnu_stack": "RW",
        "needed": ["libc.so.6"],
        "needed_raw_count": 1,
        "soname": None,
        "soname_count": 0,
        "rpath": [],
        "runpath": [],
        "glibc_versions": ["2.4"],
        "glibc_max": "2.4",
        "glibc_floor_symbols": [],
        "glibcxx_max": None,
        "cxxabi_max": None,
        "forbidden_version_tokens": [],
        "undefined_symbols": [],
        "undefined_sdl": [],
        "toolchain_note": ["gcc"],
    }
    record.update(overrides)
    return record


class PolicyTest(unittest.TestCase):
    def setUp(self):
        self.policy = nxabi.load_policy(POLICY_PATH)
        self.sdl = nxabi.load_sdl_table(self.policy, POLICY_PATH)
        self.options = {"profile": "universal-low-glibc"}

    def audit(self, record):
        return nxabi.audit_record(record, self.policy, self.sdl, self.options)

    def levels(self, findings, check):
        return [item["level"] for item in findings if item["check"] == check]

    def test_policy_ceilings_are_the_agreed_ones(self):
        ceilings = self.policy["ceilings"]
        self.assertEqual(ceilings["glibc_max"], "2.30")
        self.assertEqual(ceilings["glibc_preferred"], "2.17")
        self.assertEqual(ceilings["glibcxx_max"], "3.4.25")
        self.assertEqual(ceilings["cxxabi_max"], "1.3.11")

    def test_clean_armhf_record_has_no_error(self):
        findings = self.audit(base_record())
        self.assertEqual([f for f in findings if f["level"] == "error"], [])

    def test_glibc_above_ceiling_fails(self):
        findings = self.audit(base_record(
            glibc_max="2.43", glibc_versions=["2.4", "2.43"]))
        self.assertIn("error", self.levels(findings, "glibc-ceiling"))

    def test_glibcxx_above_ceiling_fails(self):
        findings = self.audit(base_record(glibcxx_max="3.4.29"))
        self.assertIn("error", self.levels(findings, "glibcxx-ceiling"))

    def test_cxxabi_above_ceiling_fails(self):
        findings = self.audit(base_record(cxxabi_max="1.3.13"))
        self.assertIn("error", self.levels(findings, "cxxabi-ceiling"))

    def test_glibc_private_fails(self):
        findings = self.audit(base_record(
            forbidden_version_tokens=["GLIBC_PRIVATE"]))
        self.assertIn("error", self.levels(findings, "version-token"))

    def test_soft_float_armhf_fails(self):
        findings = self.audit(base_record(flags="0x5000200, Version5 EABI"))
        self.assertIn("error", self.levels(findings, "float-abi"))

    def test_wrong_interpreter_fails(self):
        findings = self.audit(base_record(
            pt_interp="/lib/ld-linux-aarch64.so.1"))
        self.assertIn("error", self.levels(findings, "pt-interp"))

    def test_android_interpreter_on_linux_elf_fails(self):
        # Namespace inference sends this one down the Android branch, where a
        # GLIBC reference is itself the error (M17-006).
        findings = self.audit(base_record(
            namespace="android", pt_interp="/system/bin/linker",
            glibc_versions=["2.4"]))
        self.assertIn("error", self.levels(findings, "android-namespace"))

    def test_android_upstream_elf_is_not_a_project_build(self):
        findings = self.audit(base_record(
            namespace="android", pt_interp="/system/bin/linker",
            glibc_versions=[], glibc_max=None))
        self.assertEqual([f for f in findings if f["level"] == "error"], [])
        self.assertIn("android-upstream", [f["check"] for f in findings])

    def test_runpath_fails(self):
        findings = self.audit(base_record(runpath=["$ORIGIN"]))
        self.assertIn("error", self.levels(findings, "search-path"))

    def test_executable_stack_fails(self):
        findings = self.audit(base_record(pt_gnu_stack="RWE"))
        self.assertIn("error", self.levels(findings, "pt-gnu-stack"))

    def test_development_symlink_soname_fails(self):
        findings = self.audit(base_record(
            needed=["libc.so.6", "libEGL.so"], needed_raw_count=2))
        self.assertIn("error", self.levels(findings, "soname-forbidden"))

    def test_new_libc_wrapper_within_public_ceiling_warns(self):
        findings = self.audit(base_record(undefined_symbols=["memfd_create"]))
        self.assertEqual(self.levels(findings, "new-libc-api"), ["warn"])

    def test_new_libc_wrapper_above_public_ceiling_fails(self):
        findings = self.audit(base_record(undefined_symbols=["close_range"]))
        self.assertEqual(self.levels(findings, "new-libc-api"), ["error"])

    def test_sdl_symbol_above_floor_fails(self):
        findings = self.audit(base_record(
            undefined_sdl=["SDL_GameControllerRumble"]))
        self.assertIn("error", self.levels(findings, "sdl-floor"))

    def test_sdl_symbol_at_floor_passes(self):
        findings = self.audit(base_record(undefined_sdl=["SDL_Init"]))
        self.assertEqual(self.levels(findings, "sdl-floor"), [])

    def test_preferred_floor_names_the_guilty_symbol(self):
        findings = self.audit(base_record(
            glibc_max="2.27", glibc_versions=["2.4", "2.27"],
            glibc_floor_symbols=["powf"]))
        message = [f["message"] for f in findings
                   if f["check"] == "glibc-preferred"]
        self.assertTrue(message and "powf" in message[0])

    def test_waiver_downgrades_but_never_hides(self):
        record = base_record(sha256="a" * 64,
                             needed=["libc.so.6", "libEGL.so"],
                             needed_raw_count=2)
        policy = json.loads(json.dumps(self.policy))
        policy["exceptions"] = {
            "a" * 64: {"checks": ["soname-forbidden"], "reason": "test"}
        }
        findings = nxabi.apply_exceptions(
            nxabi.audit_record(record, policy, self.sdl, self.options),
            record, policy)
        waived = [f for f in findings if f["check"] == "soname-forbidden"]
        self.assertTrue(waived)
        self.assertEqual(waived[0]["level"], "warn")
        self.assertTrue(waived[0]["waived"])

    def test_waiver_is_keyed_by_hash_so_a_rebuild_loses_it(self):
        record = base_record(sha256="b" * 64,
                             needed=["libc.so.6", "libEGL.so"],
                             needed_raw_count=2)
        policy = json.loads(json.dumps(self.policy))
        policy["exceptions"] = {
            "a" * 64: {"checks": ["soname-forbidden"], "reason": "test"}
        }
        findings = nxabi.apply_exceptions(
            nxabi.audit_record(record, policy, self.sdl, self.options),
            record, policy)
        self.assertIn("error", self.levels(findings, "soname-forbidden"))

    def test_horizonchase_runpath_is_deliberately_not_waived(self):
        waivers = self.policy.get("exceptions", {})
        open_question = self.policy["open_policy_questions"][
            "horizonchase-runpath-origin"]
        self.assertNotIn(open_question["sha256"], waivers)


class SdlTableTest(unittest.TestCase):
    def test_table_is_present_and_marks_assumptions(self):
        policy = nxabi.load_policy(POLICY_PATH)
        table = nxabi.load_sdl_table(policy, POLICY_PATH)
        self.assertGreater(len(table), 500)
        self.assertEqual(table["SDL_GameControllerRumble"][0], "2.0.9")
        self.assertEqual(table["SDL_GL_GetDrawableSize"][0], "2.0.1")
        sources = {value[1] for value in table.values()}
        self.assertIn("sdl2-headers", sources)


class ToolchainPinTest(unittest.TestCase):
    def test_pin_lists_both_abis_and_disqualifies_the_host_aarch64(self):
        with open(str(PIN_PATH), "r", encoding="utf-8") as stream:
            pin = json.load(stream)
        architectures = {
            entry.get("architecture")
            for entry in pin["toolchains"].values()
        }
        self.assertEqual(architectures, {"armv7", "aarch64"})
        self.assertIn("host-aarch64-gcc", pin["disqualified"])

    def test_pin_verification_runs(self):
        results = []
        with open(str(PIN_PATH), "r", encoding="utf-8") as stream:
            pin = json.load(stream)
        for name, entry in sorted(pin["toolchains"].items()):
            status, detail = nxabi.verify_toolchain(name, entry)
            results.append((name, status, detail))
        # "absent" is allowed (a container may not be pulled on this host);
        # "fail" means real drift and must never happen silently.
        for name, status, detail in results:
            self.assertIn(status, ("ok", "absent"),
                          "{}: {}".format(name, detail))


class RealElfTest(unittest.TestCase):
    """End-to-end inspection of an ELF this test builds itself."""

    @classmethod
    def setUpClass(cls):
        cls.work = None
        compiler = "/opt/prebuilt/bin/arm-linux-gnueabihf-gcc"
        if not os.path.exists(compiler) or shutil.which("readelf") is None:
            raise unittest.SkipTest("pinned ARMHF toolchain not available")
        cls.work = tempfile.mkdtemp(prefix="nxabi-test-")
        source = Path(cls.work) / "fixture.c"
        source.write_text(
            '#include "nx_symver.h"\n'
            "#include <stdio.h>\n"
            "int main(int argc, char **argv){ (void)argv;\n"
            "  volatile float x = (float)argc;\n"
            '  printf("%f\\n", powf(x, 3.f) + expf(x) + exp2f(x)'
            " + logf(x) + log2f(x));\n"
            "  return 0; }\n",
            encoding="utf-8",
        )
        no_builtin = [
            "-fno-builtin-" + name
            for name in ("powf", "expf", "exp2f", "logf", "log2f")
        ]
        common = [
            compiler, "-O2", "-march=armv7-a", "-mfloat-abi=hard",
            "-I", str(MODULE_DIR / "include"),
        ] + no_builtin + [str(source)]
        cls.plain = str(Path(cls.work) / "plain")
        cls.symver = str(Path(cls.work) / "symver")
        subprocess.run(
            common + ["-DNX_SYMVER_DISABLE", "-o", cls.plain, "-lm"],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        subprocess.run(
            common + ["-o", cls.symver, "-lm"],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    @classmethod
    def tearDownClass(cls):
        if cls.work and cls.work.startswith(tempfile.gettempdir()):
            shutil.rmtree(cls.work, ignore_errors=True)

    def test_inspection_reports_the_armhf_contract(self):
        record = nxabi.inspect_elf(self.plain)
        self.assertEqual(record["architecture"], "armv7")
        self.assertEqual(record["class"], "ELF32")
        self.assertEqual(record["pt_interp"], "/lib/ld-linux-armhf.so.3")
        self.assertIn("hard-float ABI", record["flags"])
        self.assertEqual(record["namespace"], "linux")
        self.assertGreaterEqual(record["pt_load_count"], 1)
        self.assertEqual(record["rpath"], [])
        self.assertEqual(record["runpath"], [])

    def test_symver_header_lowers_the_floor(self):
        plain = nxabi.inspect_elf(self.plain)
        symver = nxabi.inspect_elf(self.symver)
        self.assertEqual(plain["glibc_max"], "2.27")
        self.assertEqual(symver["glibc_max"], "2.4")
        self.assertEqual(
            sorted(plain["glibc_floor_symbols"]),
            ["exp2f", "expf", "log2f", "logf", "powf"],
        )

    def test_floor_symbols_are_named_for_the_operator(self):
        policy = nxabi.load_policy(POLICY_PATH)
        table = nxabi.load_sdl_table(policy, POLICY_PATH)
        findings = nxabi.audit_record(
            nxabi.inspect_elf(self.plain), policy, table,
            {"profile": "universal-low-glibc"})
        preferred = [f for f in findings if f["check"] == "glibc-preferred"]
        self.assertTrue(preferred)
        self.assertIn("powf", preferred[0]["message"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
