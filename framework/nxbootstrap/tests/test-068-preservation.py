#!/usr/bin/env python3
"""Preserve the 0.6.8 safety floor and immutable visual component pins."""

import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT.parents[1]
GENERATOR = ROOT / "tools" / "generate-port.py"
TEMPLATE = ROOT / "templates" / "launcher.sh.in"
LEGACY_RUNTIME = ROOT / "nxbootstrap.sh"
BEHAVIOR_TEST = ROOT / "tests" / "test-launcher-behavior.sh"
NXSPLASH_ROOT = REPO_ROOT / "framework" / "nxsplash"
NXEXTRACT_ROOT = REPO_ROOT / "suportando_outros_devices" / "extrator-universal"
NXINPUT_README = REPO_ROOT / "framework" / "nxinput" / "README.md"
# Reselado: a capability graphics.gles1 entrou no registry e o gate
# nxport-contract exige que a allowlist de shell deste runtime
# aposentado acompanhe o registry. Os dois pins andam juntos.
LEGACY_RUNTIME_SHA256 = (
    "8caa540767594a2d1d58b3d2bffe22807f86cb5fa2cad792fd16ec72b7400ee4"
)
EXAMPLES = (
    ("nxport.example.json", "Meu Port.sh"),
    ("nxport-armv7.example.json", "Meu Port ARMHF.sh"),
)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def check_source_contract():
    require(LEGACY_RUNTIME.is_file() and not LEGACY_RUNTIME.is_symlink(),
            "retired runtime evidence is missing or unsafe")
    require(sha256(LEGACY_RUNTIME.read_bytes()) == LEGACY_RUNTIME_SHA256,
            "retired 0.6.8-compatible runtime evidence changed")
    generator = GENERATOR.read_text(encoding="utf-8")
    require(generator.count('NXEXTRACT_VERSION = "1.3.0"') == 1,
            "generator lacks the single NXExtract 1.2.13 pin")
    template = TEMPLATE.read_text(encoding="utf-8")
    source_contract = generator + "\n" + template
    for token in (
        "nxbootstrap_install_exit_trap",
        "NXBOOTSTRAP_LAUNCHER_DIR=\"\"",
        "NXBOOTSTRAP_PHASE_FILE=\"$GAMEDIR/nxphase-result.json\"",
        "NXOBS_PHASE_PROTOCOL=nxbootstrap-phase-v1",
        "nxbootstrap_phase_event runtime START lifecycle 6207",
        "nxbootstrap_phase_event runtime EXIT lifecycle 6208",
        # 0.6.19: ARMHF interpreter found off the default path (spruce/Flip) is
        # launched through an explicit loader; the prefix is empty (byte-identical
        # run line) for every normal port.
        "NXBOOTSTRAP_INTERP_PREFIX=\"\"",
        "$NXBOOTSTRAP_INTERP_PREFIX \"$BIN\"@RUN_ARGS@",
        "--library-path $NXBOOTSTRAP_INTERP_LIBS",
        # 0.6.20: post-game RUNTIME DIAGNOSIS classifies the exit (undefined
        # symbol, missing library, crash, black screen) into one readable line.
        "echo \"RUNTIME DIAGNOSIS: $NXBOOTSTRAP_DIAG\"",
        "black-screen — the process ran but no non-black frame",
        # 0.6.21: capability-gated guest audio shield (embedded OpenAL) + the
        # inherited-audio-env receipt line.
        "[ \"$NXBOOTSTRAP_CAP\" = audio.embedded-openal ]",
        "export ALSOFT_DRIVERS=opensl",
        "ENV RECEIPT: sdl_audiodriver=",
    ):
        require(token in source_contract,
                "0.6.21 source contract token is missing: %s" % token)
    trap_position = template.index("nxbootstrap_install_exit_trap\n")
    discovery_position = template.index("NXBOOTSTRAP_LAUNCHER_DIR=$(", trap_position)
    portmaster_position = template.index("NXBOOTSTRAP_PM_ROOTS=(", discovery_position)
    require(trap_position < discovery_position < portmaster_position,
            "EXIT trap is not installed before launcher/PortMaster discovery")


def check_behavior_evidence():
    text = BEHAVIOR_TEST.read_text(encoding="utf-8")
    for token in (
        "launcher did not recognize the two-marker dArkOSRE fallback",
        "single firmware marker fabricated a CFW identity or sourced control",
        "CFW_NAME path traversal sourced an attacker-controlled mod file",
        'NXBEHAV_PATH="$TEST_ROOT/no-stat:$PATH"',
        "launcher called stat or did not launch the child",
        "cfw-fallback=2",
        "no-stat=1",
    ):
        require(token in text,
                "0.6.8 executable regression evidence is missing: %s" % token)
    input_documentation = NXINPUT_README.read_text(encoding="utf-8")
    for token in (
        "skip-absr",
        "dArkOSRE",
        "nxinput` não inventa movimento que nunca chegou à SDL",
        "firmware/device tree",
    ):
        require(token in input_documentation,
                "dArkOSRE input-limit documentation is missing: %s" % token)


def check_generated_launcher(path, bootstrap_version, splash_version):
    data = path.read_bytes()
    bootstrap_slot = ("nxbootstrap " + bootstrap_version).encode("ascii")
    splash_slot = ("nxsplash " + splash_version).encode("ascii")
    require(data.count(bootstrap_slot) == 3,
            "generated launcher bootstrap version-slot count changed")
    require(data.count(splash_slot) == 2,
            "generated launcher nxsplash version-slot count changed")
    text = data.decode("utf-8")
    require(text.count("nxbootstrap_observe_cfw_name") == 3,
            "safe-CFW observer definition/calls changed")
    for token in (
        '[ -f "$HOME/.config/.OS" ]',
        '[ ! -L "$HOME/.config/.OS" ]',
        "IFS= read -r -n 65 candidate",
        '[ "${#candidate}" -le 64 ] || candidate=""',
        "/boot/arkos4clone-uboot.dtb",
        "/opt/system/Advanced/Backup dArkOS Settings.sh",
        "candidate=dArkOSRE",
        'mod_${NXBOOTSTRAP_MOD_NAME}.txt',
        "PHASE %s EXIT status=%s",
        '"schema":"nx-event-v1"',
        "nxbootstrap_phase_event nxsplash START bootstrap 6204",
        "nxbootstrap_phase_event nxsplash OK bootstrap 6205",
    ):
        require(token in text,
                "generated launcher token is missing: %s" % token)
    require(re.search(r"(^|[;&|\s])stat\s+-", text, re.MULTILINE) is None,
            "generated launcher regained an external stat command")


def main():
    bootstrap_version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    splash_version = (NXSPLASH_ROOT / "VERSION").read_text(
        encoding="utf-8").strip()
    splash_release = json.loads(
        (NXSPLASH_ROOT / "release" / "manifest-v1.json").read_text(
            encoding="utf-8"))
    nxextract_version = (NXEXTRACT_ROOT / "VERSION").read_text(
        encoding="utf-8").strip()
    ui_release = json.loads(
        (NXEXTRACT_ROOT / "ui" / "release" / "manifest-v1.json").read_text(
            encoding="utf-8"))

    require(bootstrap_version == "0.8.4",
            "preservation gate is attached to the wrong nxbootstrap version")
    require(splash_version == "0.1.2" and
            splash_release.get("version") == splash_version and
            splash_release.get("component") == "nxsplash",
            "nxbootstrap lacks the exact immutable NXSplash 0.1.2 pin")
    require(nxextract_version == "1.3.0",
            "nxbootstrap lacks the exact NXExtract 1.3.0 engine pin")
    require(ui_release.get("version") == "1.2.16" and
            ui_release.get("component") == "nxextract-ui" and
            set(ui_release.get("artifacts", {})) ==
            {"aarch64", "armv7", "i386", "x86_64"},
            "NXExtract presentation bytes no longer use the immutable 1.2.16 UI set")

    check_source_contract()
    check_behavior_evidence()
    with tempfile.TemporaryDirectory(prefix="nxbootstrap-preservation.") as tmp:
        for index, (example_name, launcher_name) in enumerate(EXAMPLES):
            output = Path(tmp) / ("generated-%d" % index)
            subprocess.run(
                [sys.executable, str(GENERATOR),
                 str(ROOT / "examples" / example_name),
                 "--output", str(output)],
                check=True, stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE, text=True,
            )
            check_generated_launcher(
                output / launcher_name, bootstrap_version, splash_version)

    print("nxbootstrap preservation gate passed: "
          "legacy_runtime_bytes=1 visual_pins=2 generated_launchers=2 "
          "safe_cfw=1 no_stat=1 early_trap=1 phase_protocol=1")


if __name__ == "__main__":
    main()
