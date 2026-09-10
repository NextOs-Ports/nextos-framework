#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""nxrelease side of the V4 declarative opt-ins.

nxgenerator 0.3.0 always writes `display`, `egl_binding` and
`input_sdl3_portmaster` into adapter-contract.json, already normalized.
nxrelease checks their shape and the one package-level consequence it can
actually observe: an enabled EGL binding exists precisely BECAUSE no packaged
ELF may declare DT_NEEDED libEGL. Binding EGL at runtime and linking it
statically are contradictory claims, and shipping both would silently restore
the global dependency the front removes.
"""

import importlib.util
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[3]
TOOL = REPOSITORY / "framework" / "nxrelease" / "nxrelease.py"


class GateError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def load_tool():
    specification = importlib.util.spec_from_file_location(
        "nxrelease_v4_optins_under_test", TOOL)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


EMITTER = """
#include <stdio.h>
#include <string.h>
#include "nxgl_display.h"

static void emit(nxgl_display_policy policy, int iw, int ih, int dw, int dh) {
  nxgl_display_request request;
  nxgl_display_plan plan;
  char receipt[512];
  memset(&request, 0, sizeof(request));
  request.struct_size = sizeof(request);
  request.api_version = NXGL_DISPLAY_API_VERSION;
  request.policy = policy;
  request.internal_width = iw;
  request.internal_height = ih;
  if (nxgl_display_plan_build(&request, dw, dh, &plan) != NXGL_DISPLAY_OK) {
    return;
  }
  if (nxgl_display_receipt(&plan, receipt, sizeof(receipt)) == 0) {
    return;
  }
  printf("%s\\n", receipt);
}

int main(void) {
  emit(NXGL_DISPLAY_POLICY_PRESERVE, 640, 480, 1280, 720);
  emit(NXGL_DISPLAY_POLICY_PRESERVE, 640, 480, 2560, 720);
  emit(NXGL_DISPLAY_POLICY_STRETCH, 640, 480, 1280, 720);
  emit(NXGL_DISPLAY_POLICY_FILL, 640, 480, 1280, 720);
  emit(NXGL_DISPLAY_POLICY_GAME, 640, 480, 1280, 720);
  return 0;
}
"""


def check_parser_against_the_real_emitter(tool):
    """Parse receipts the C emitter actually printed, not strings we typed.

    nxrelease refuses a hand-written `display_proofs` entry precisely because
    only the runtime can say what the adapter installed -- and every other
    case in this gate feeds the parser a string written here. If the emitter's
    format drifts (a field renamed, reordered, added), the nxgl gate still
    passes on its own prefix checks and this parser starts returning None for
    every real receipt: a release front that silently refuses everything, or
    accepts a receipt that no longer means what it says.
    """
    compiler = shutil.which(os.environ.get("CC", "")) or shutil.which("cc") \
        or shutil.which("gcc")
    require(compiler is not None,
            "no C compiler to run the real display emitter")
    nxgl = REPOSITORY / "framework" / "nxgl"
    with tempfile.TemporaryDirectory() as work:
        source = Path(work) / "emit_receipts.c"
        source.write_text(EMITTER, encoding="utf-8")
        binary = Path(work) / "emit_receipts"
        build = subprocess.run(
            [compiler, "-std=c99", "-Wall", "-Werror",
             "-I", str(nxgl / "include"), "-o", str(binary), str(source),
             str(nxgl / "src" / "nxgl_display.c")],
            capture_output=True, text=True)
        require(build.returncode == 0,
                "the real display emitter did not build: %s"
                % build.stderr.strip().splitlines()[-1:])
        run = subprocess.run([str(binary)], capture_output=True, text=True)
        require(run.returncode == 0, "the real display emitter failed to run")
    lines = [line for line in run.stdout.splitlines() if line.strip()]
    require(len(lines) == 5,
            "the real emitter produced %d receipts, expected 5" % len(lines))
    for line in lines:
        parsed = tool.parse_display_receipt(line)
        require(parsed is not None,
                "the parser rejected a receipt the emitter really printed: %s"
                % line)
        require(parsed["policy"] in ("game", "preserve", "adaptive", "fill",
                                     "stretch"),
                "the parser read a policy the emitter cannot emit: %s" % line)
        require(parsed["content"][2] > 0 and parsed["content"][3] > 0,
                "the parser read an empty content rect from a real receipt")
    # And the values must be the ones the emitter computed, not merely
    # well-formed: 640x480 preserved on 1280x720 is a 960x720 pillarboxed rect.
    preserved = tool.parse_display_receipt(lines[0])
    require(preserved["policy"] == "preserve" and
            preserved["internal"] == (640, 480) and
            preserved["drawable"] == (1280, 720) and
            preserved["content"] == (160, 0, 960, 720),
            "the parser did not recover the emitter's own numbers")
    return len(lines)


# Mission 114A: the summary below must be DERIVED from what actually ran.
# The previous version printed hardcoded counts, which is how a report ends
# up claiming 8 negatives while 9 executed.
COUNTS = {"accepts": 0, "refuses": 0, "bundle_accepts": 0,
          "bundle_refuses": 0}
_SECTION = ["general"]


def accepts(tool, contract, config, message):
    try:
        tool._validate_v4_optins(contract, config)
    except tool.ReleaseError as error:
        raise GateError("%s: %s" % (message, error))
    COUNTS["accepts"] += 1
    if _SECTION[0] == "bundle":
        COUNTS["bundle_accepts"] += 1


def refuses(tool, contract, config, fragment, message):
    try:
        tool._validate_v4_optins(contract, config)
    except tool.ReleaseError as error:
        require(fragment in str(error),
                "%s: refusal did not mention %r (%s)"
                % (message, fragment, error))
        COUNTS["refuses"] += 1
        if _SECTION[0] == "bundle":
            COUNTS["bundle_refuses"] += 1
        return
    raise GateError(message)


IMPORTS = sorted(["eglGetCurrentContext", "eglGetCurrentDisplay",
                  "eglMakeCurrent", "eglSwapBuffers"])


def contract(**overrides):
    value = {
        "display": {"policy": "game", "remap_input": False},
        "egl_binding": {"enabled": False, "imports": []},
        "input_sdl3_portmaster": {"enabled": False, "private_sdl3_sha256": ""},
    }
    value.update(overrides)
    return value


def config(needed=()):
    return {"records": [{"target": "port/game-nextos",
                         "needed": list(needed)}]}


def main():
    tool = load_tool()

    # The all-default contract a regenerated V3 port produces passes untouched.
    accepts(tool, contract(), config(), "the all-default contract")
    # An old contract with none of the blocks is still valid: the fields are
    # additive and their absence is the no-op.
    accepts(tool, {}, config(), "a contract with none of the V4 blocks")

    # A declared remap is a claim about the player's finger, so it needs the
    # receipt the runtime actually printed.
    remap = {"policy": "preserve", "internal_width": 640,
             "internal_height": 480, "remap_input": True}
    good_proofs = [{
        "device": "darkosre-rk3326-mali-g31",
        "evidence": ("DISPLAY: policy=preserve internal=640x480 "
                     "drawable=1280x720 content=160,0+960x720 "
                     "letterbox=0/0 pillarbox=160/160 cropped=0 noop=0"),
    }]
    value = contract(display=remap)
    value["display_proofs"] = good_proofs
    accepts(tool, value, config(), "a declared remap with its receipt")
    # remap_input false needs no receipt at all.
    accepts(tool,
            contract(display={"policy": "preserve", "internal_width": 640,
                              "internal_height": 480, "remap_input": False}),
            config(), "remap_input false")
    accepts(tool, contract(egl_binding={"enabled": True, "imports": IMPORTS}),
            config(("libc.so.6", "libSDL2-2.0.so.0")),
            "an enabled EGL binding without DT_NEEDED libEGL")
    accepts(tool,
            contract(input_sdl3_portmaster={"enabled": True,
                                            "private_sdl3_sha256": "a" * 64}),
            config(), "an enabled SDL3 opt-in with its pin")

    # The package-level consequence: runtime binding and DT_NEEDED libEGL are
    # contradictory claims.
    refuses(tool,
            contract(egl_binding={"enabled": True, "imports": IMPORTS}),
            config(("libc.so.6", "libEGL.so.1")),
            "DT_NEEDED libEGL",
            "an enabled egl_binding shipped an ELF linking EGL")

    for bad, fragment, why in (
        (contract(display={"policy": "mali"}), "display.policy",
         "a policy named after a GPU"),
        (contract(display={"policy": "game", "internal_width": 640}),
         "presentation fields", "policy game with a presentation field"),
        (contract(display={"policy": "preserve", "internal_width": 0,
                           "internal_height": 480}), "internal_width",
         "a zero internal width"),
        (contract(display={"policy": "preserve", "internal_width": True,
                           "internal_height": 480}), "internal_width",
         "a boolean internal width"),
        (contract(egl_binding={"enabled": True, "imports": []}),
         "unique, ordered inventory", "an empty enabled inventory"),
        (contract(egl_binding={"enabled": True,
                               "imports": ["eglSwapBuffers",
                                           "eglGetCurrentContext"]}),
         "unique, ordered inventory", "an unordered inventory"),
        (contract(egl_binding={"enabled": True,
                               "imports": ["eglSwapBuffers"]}),
         "eglGetCurrentContext", "an inventory without the ownership probe"),
        (contract(egl_binding={"enabled": True,
                               "imports": ["eglGetCurrentContext",
                                           "glClear"]}),
         "EGL symbol names", "a non-EGL symbol"),
        (contract(egl_binding={"enabled": False,
                               "imports": ["eglGetCurrentContext"]}),
         "empty inventory", "a disabled binding pinning an inventory"),
        (contract(input_sdl3_portmaster={"enabled": True,
                                         "private_sdl3_sha256": ""}),
         "pin the private SDL3", "an enabled opt-in with no pinned SDL3"),
        (contract(input_sdl3_portmaster={"enabled": False,
                                         "private_sdl3_sha256": "a" * 64}),
         "must not pin", "a disabled opt-in pinning an SDL3"),
        (contract(input_sdl3_portmaster={"enabled": True}),
         "malformed", "an opt-in missing its digest field"),
    ):
        refuses(tool, bad, config(), fragment,
                "nxrelease accepted %s" % why)


    # --- V4-CONTROLLERS-03/C3: the NXCONTROLLER_PROFILES/1 bundle -------
    # Mission 114A: the POSITIVE consumes the REAL sealed C3 bundle, not a
    # synthetic three-line stand-in. Every negative is a mutation of those
    # same real bytes, so what the gate accepts and what it refuses are the
    # same artifact the port would actually ship.
    import hashlib
    import pathlib
    import tempfile

    C3_BUNDLE_SHA256 = (
        "a578a7d82d47e681ad7a1cbe48bb49327dad6dbe1c808e46ca3485dbab0dae43")
    bundle_path = pathlib.Path(
        os.environ.get("NX_CONTROLLERS_NXB")
        or (Path(__file__).resolve().parent / "fixtures" / "controllers.nxb"))
    require(bundle_path.is_file(),
            "the sealed C3 bundle is missing: %s" % bundle_path)
    REAL_BUNDLE_BYTES = bundle_path.read_bytes()
    require(hashlib.sha256(REAL_BUNDLE_BYTES).hexdigest() == C3_BUNDLE_SHA256,
            "the C3 bundle under test is not the sealed one (%s)"
            % hashlib.sha256(REAL_BUNDLE_BYTES).hexdigest())
    REAL_BUNDLE = REAL_BUNDLE_BYTES.decode("utf-8")
    require(REAL_BUNDLE.startswith("NXCONTROLLER_PROFILES/1\n") and
            "\n# license=" in REAL_BUNDLE and
            len([line for line in REAL_BUNDLE.splitlines()
                 if not line.startswith("#") and line]) > 500,
            "the sealed C3 bundle does not look like the real 526-GUID one")

    def bundle_config(payload, digest=None, present=True):
        with tempfile.NamedTemporaryFile("w", suffix=".nxb", delete=False,
                                         encoding="utf-8") as stream:
            stream.write(payload)
            path = stream.name
        real = hashlib.sha256(payload.encode("utf-8")).hexdigest()
        records = []
        if present:
            records.append({"target": "port/controllers.nxb",
                            "actual_path": path,
                            "sha256": digest if digest is not None else real})
        return {"records": records, "port_dir": "port"}, real

    def profiles_contract(**fields):
        base = {"enabled": True, "bundle": "controllers.nxb",
                "sha256": "0" * 64}
        base.update(fields)
        return contract(input_controller_profiles=base)

    # POSITIVE: the real sealed bundle, pinned by its real SHA-256.
    _SECTION[0] = "bundle"
    cfg, real_digest = bundle_config(REAL_BUNDLE)
    require(real_digest == C3_BUNDLE_SHA256,
            "the positive is not pinned to the sealed bundle digest")
    accepts(tool, profiles_contract(sha256=real_digest), cfg,
            "the real sealed C3 bundle was refused")
    # And the disabled declaration, which pins nothing at all.
    accepts(tool,
            contract(input_controller_profiles={"enabled": False,
                                                "bundle": "", "sha256": ""}),
            config(), "a disabled declaration was refused")

    # NEGATIVES, every one of them a mutation of the real bundle.
    cfg_absent, _ = bundle_config(REAL_BUNDLE, present=False)
    refuses(tool, profiles_contract(sha256=real_digest), cfg_absent,
            "does not carry it", "a pinned bundle missing from the package")
    cfg_drift, _ = bundle_config(REAL_BUNDLE, digest="b" * 64)
    refuses(tool, profiles_contract(sha256="c" * 64), cfg_drift,
            "does not match the pinned", "a bundle drifting from its pin")
    # The record may claim the right digest while the BYTES are someone
    # else's: the pin is verified against the file, not against the claim.
    cfg_lie, _ = bundle_config(REAL_BUNDLE.replace("a:b1", "a:b0", 1),
                               digest=real_digest)
    refuses(tool, profiles_contract(sha256=real_digest), cfg_lie,
            "does not match the pinned",
            "a package record lying about the bundle bytes")
    cfg_header, header_digest = bundle_config("junk\n" + REAL_BUNDLE)
    refuses(tool, profiles_contract(sha256=header_digest), cfg_header,
            "NXCONTROLLER_PROFILES/1 header", "a bundle without the header")
    cfg_lic, lic_digest = bundle_config(
        "\n".join(line for line in REAL_BUNDLE.split("\n")
                  if not line.startswith("# license=")))
    refuses(tool, profiles_contract(sha256=lic_digest), cfg_lic,
            "license header", "a bundle without its license")
    cfg_ip, ip_digest = bundle_config(REAL_BUNDLE + "# note=192.168.31.99\n")
    refuses(tool, profiles_contract(sha256=ip_digest), cfg_ip,
            "forbidden content", "a bundle carrying an address")
    cfg_latest, latest_digest = bundle_config(REAL_BUNDLE + "# source=latest\n")
    refuses(tool, profiles_contract(sha256=latest_digest), cfg_latest,
            "forbidden content", "a bundle pointing at latest")
    cfg_home, home_digest = bundle_config(
        REAL_BUNDLE + "# note=/home/owner/roms\n")
    refuses(tool, profiles_contract(sha256=home_digest), cfg_home,
            "forbidden content", "a bundle carrying an owner path")
    refuses(tool, profiles_contract(bundle="../evil.nxb",
                                    sha256=real_digest), cfg,
            "plain file name", "a bundle path with traversal")
    refuses(tool, profiles_contract(bundle="renamed-profiles.nxb",
                                    sha256=real_digest), cfg,
            "exactly controllers.nxb",
            "a bundle name the runtime authority cannot declare")
    refuses(tool, profiles_contract(sha256=""), cfg,
            "pin the bundle SHA-256", "an enabled opt-in with no pin")
    refuses(tool,
            contract(input_controller_profiles={
                "enabled": False, "bundle": "controllers.nxb",
                "sha256": "a" * 64}), config(),
            "must not pin", "a disabled opt-in pinning a bundle")
    _SECTION[0] = "general"

    # --- V4-DISPLAY-01 receipt negatives --------------------------------
    def with_proofs(proofs):
        value = contract(display=dict(remap))
        if proofs is not None:
            value["display_proofs"] = proofs
        return value

    for proofs, fragment, why in (
        (None, "must carry display_proofs", "a remap with no receipt at all"),
        ([], "must carry display_proofs", "a remap with an empty proof list"),
        ([{"device": "d", "evidence": "content rect looked fine"}],
         "well-formed DISPLAY receipt", "a hand-written verdict"),
        ([{"device": "d",
           "evidence": ("DISPLAY: policy=stretch internal=640x480 "
                        "drawable=1280x720 content=0,0+1280x720")}],
         "reports policy", "a receipt for a different policy"),
        ([{"device": "d",
           "evidence": ("DISPLAY: policy=preserve internal=320x240 "
                        "drawable=1280x720 content=160,0+960x720")}],
         "different internal resolution",
         "a receipt for a different internal size"),
        ([{"device": "d",
           "evidence": ("DISPLAY: policy=preserve internal=640x480 "
                        "drawable=1280x720 content=160,0+0x720")}],
         "empty content rect", "a receipt with an empty content rect"),
        ([{"evidence": good_proofs[0]["evidence"]}],
         "lacks a device identity", "a proof with no device"),
        ([good_proofs[0], dict(good_proofs[0])],
         "duplicate proof", "two proofs for the same device"),
    ):
        refuses(tool, with_proofs(proofs), config(), fragment,
                "nxrelease accepted %s" % why)

    # The parser itself refuses malformed receipt lines.
    for bad in ("", "DISPLAY policy=preserve", "not a receipt",
                "DISPLAY: policy=preserve internal=640 drawable=1280x720 "
                "content=0,0+1x1",
                "DISPLAY: policy=preserve internal=640x480 drawable=1280x720",
                "DISPLAY: policy=preserve policy=fill internal=640x480 "
                "drawable=1280x720 content=0,0+1x1"):
        if tool.parse_display_receipt(bad) is not None:
            raise GateError("the DISPLAY parser accepted %r" % bad)
    require(tool.parse_display_receipt(good_proofs[0]["evidence"])["content"]
            == (160, 0, 960, 720),
            "the DISPLAY parser lost the content rect")
    real = check_parser_against_the_real_emitter(tool)
    print("nxrelease V4 opt-in gate passed: accepts=%d refuses=%d "
          "c3_bundle=%s c3_bundle_accepts=%d c3_bundle_refuses=%d "
          "real_emitter_receipts=%d"
          % (COUNTS["accepts"], COUNTS["refuses"], C3_BUNDLE_SHA256[:16],
             COUNTS["bundle_accepts"], COUNTS["bundle_refuses"], real))


if __name__ == "__main__":
    main()
