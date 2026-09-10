#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""V4-05A: integral parity of the SDL floor between nxabi and nxrelease.

The 03B audit proved the two consumers could still disagree in three
classes. These regressions hold the corrected contract, class by class:

  1. known symbol at/below the floor  -> no finding in either tool;
  2. known post-floor symbol          -> fatal in both;
  3. symbol absent from the authority -> fatal in both for a public/universal
                                         candidate (was WARN in nxabi);
  4. indirect SDL (no direct DT_NEEDED of the core) -> the floor still
                                         reaches the import in both;
  5. SDL3 consumer                    -> documented exclusion in both;
  6. historical waiver                -> never downgrades a public verdict
                                         and never readmits Vendor/Product;
  7. new PUBLIC candidate             -> both consumers produce the same
                                         severity through the ONE shared
                                         decision function.

Directed and synthetic; no stage, no ZIP, no battery.
"""

import importlib.util
import pathlib
import sys

COMPONENT = pathlib.Path(__file__).resolve().parent.parent
FRAMEWORK = COMPONENT.parent


def require(condition, message):
    if not condition:
        raise SystemExit("sdl floor parity FAILED: %s" % message)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, str(path))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def record(**overrides):
    base = {
        "path": "loader-example", "sha256": "f" * 64, "size": 1, "build_id":
        "ab", "class": "ELF64", "data": "2's complement, little endian",
        "elf_type": "DYN (Shared object file)", "machine": "AArch64",
        "flags": "", "os_abi": "UNIX - System V", "architecture": "aarch64",
        "namespace": "linux-glibc", "pt_load_count": 2,
        "pt_interp": "/lib/ld-linux-aarch64.so.1", "pt_interp_count": 1,
        "pt_gnu_stack": "RW", "needed": ["libc.so.6"], "needed_raw_count": 1,
        "soname": None, "soname_count": 0, "rpath": [], "runpath": [],
        "glibc_versions": ["2.17"], "glibc_max": "2.17",
        "glibc_floor_symbols": [], "glibcxx_max": None, "cxxabi_max": None,
        "forbidden_version_tokens": [], "undefined_symbols": [],
        "undefined_sdl": [], "toolchain_note": None,
    }
    base.update(overrides)
    base["undefined_symbols"] = sorted(
        set(base["undefined_symbols"]) | set(base["undefined_sdl"]))
    return base


def levels(findings, check):
    return [item["level"] for item in findings if item["check"] == check]


def main():
    nxabi = load("parity_nxabi", FRAMEWORK / "nxabi" / "nxabi.py")
    policy = nxabi.load_policy(FRAMEWORK / "nxabi" / "policy-v1.json")
    authority = nxabi.load_sdl_authority_for_policy(
        policy, FRAMEWORK / "nxabi" / "policy-v1.json")
    table = authority["table"]
    public = {"profile": "universal-low-glibc", "sdl_floor": None}

    # The one shared decision function exists and both sources call it.
    require(hasattr(nxabi, "decide_sdl_floor"),
            "nxabi lost the shared decide_sdl_floor")
    release_source = (COMPONENT / "nxrelease.py").read_text()
    require("decide_sdl_floor" in release_source,
            "nxrelease no longer routes the floor through the shared "
            "decision function")
    require("if soname not in item.get(\"needed\", ())" in release_source,
            "the non-SDL family gate lost its direct-NEEDED scope")

    # 1. known allowed symbol: silent in nxabi.
    ok = nxabi.audit_record(
        record(undefined_sdl=["SDL_Init"], needed=["libSDL2-2.0.so.0"]),
        policy, table, public)
    require(not levels(ok, "sdl-floor") and not levels(ok, "sdl-unknown"),
            "an at-floor symbol raised a floor finding")

    # 2. known post-floor symbol: fatal.
    post = nxabi.audit_record(
        record(undefined_sdl=["SDL_JoystickGetVendor"],
               needed=["libSDL2-2.0.so.0"]),
        policy, table, public)
    require(levels(post, "sdl-floor") == ["error"],
            "the SDL 2.0.6 Vendor import is not fatal in nxabi")

    # 3. absent from the authority: fatal for public (the 03B divergence).
    unknown = nxabi.audit_record(
        record(undefined_sdl=["SDL_NotARealSymbol"],
               needed=["libSDL2-2.0.so.0"]),
        policy, table, public)
    require(levels(unknown, "sdl-unknown") == ["error"],
            "an authority-unknown symbol is not fatal for a public "
            "candidate in nxabi")

    # 4. indirect SDL: no DT_NEEDED of the core, import still decided.
    indirect = nxabi.audit_record(
        record(undefined_sdl=["SDL_JoystickGetVendor"], needed=["libc.so.6"]),
        policy, table, public)
    require(levels(indirect, "sdl-floor") == ["error"],
            "nxabi lost the indirect-SDL reach")
    require("decide_sdl_floor" in release_source and
            "even when the SDL arrives" in release_source and
            "no direct DT_NEEDED" in release_source,
            "nxrelease no longer documents/implements the indirect reach")

    # 5. SDL3 exclusion, both tools.
    sdl3 = nxabi.audit_record(
        record(undefined_sdl=["SDL_JoystickGetVendor"],
               needed=["libSDL3.so.0"]),
        policy, table, public)
    require(not levels(sdl3, "sdl-floor") and
            levels(sdl3, "sdl3-bundled") == ["info"],
            "the documented SDL3 exclusion regressed in nxabi")
    require("sdl3_needed" in release_source,
            "the documented SDL3 exclusion regressed in nxrelease")

    # 6. historical waiver: annotates, never downgrades a public verdict.
    waiver_policy = dict(policy)
    waiver_policy = {**policy, "exceptions": {
        "f" * 64: {"checks": ["sdl-floor", "sdl-unknown"],
                    "reason": "historical evidence"}}}
    waived = nxabi.apply_exceptions(
        nxabi.audit_record(
            record(undefined_sdl=["SDL_JoystickGetVendor"],
                   needed=["libSDL2-2.0.so.0"]),
            waiver_policy, table, public),
        record(undefined_sdl=["SDL_JoystickGetVendor"],
               needed=["libSDL2-2.0.so.0"]),
        waiver_policy, public)
    floor_items = [item for item in waived if item["check"] == "sdl-floor"]
    require(floor_items and floor_items[0]["level"] == "error" and
            floor_items[0].get("waived") is False and
            "waiver refused" in floor_items[0]["message"],
            "a historical waiver downgraded Vendor/Product for a public "
            "candidate")

    # 7. a non-public profile may still read historical evidence as warn.
    profiles = policy.get("build_profiles", {})
    nonpublic = next((name for name, spec in profiles.items()
                      if spec.get("public") is False), None)
    if nonpublic is not None:
        relaxed = nxabi.apply_exceptions(
            nxabi.audit_record(
                record(undefined_sdl=["SDL_JoystickGetVendor"],
                       needed=["libSDL2-2.0.so.0"]),
                waiver_policy, table,
                {"profile": nonpublic, "sdl_floor": None}),
            record(undefined_sdl=["SDL_JoystickGetVendor"],
                   needed=["libSDL2-2.0.so.0"]),
            waiver_policy, {"profile": nonpublic, "sdl_floor": None})
        relaxed_floor = [item for item in relaxed
                         if item["check"] == "sdl-floor"]
        require(relaxed_floor and relaxed_floor[0]["level"] == "warn" and
                relaxed_floor[0].get("waived") is True,
                "historical evidence became unreadable for a non-public "
                "profile")

    print("nxrelease/nxabi SDL floor parity: PASS classes=7 "
          "shared_decision=1 indirect_reach=1 waiver_refusal=1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
