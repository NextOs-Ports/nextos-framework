#!/usr/bin/env python3
"""Static fail-closed audit of every framework test class and signal path."""

import ast
import json
import re
import shlex
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
FRAMEWORK_ROOT = REPO_ROOT / "framework"
MATRIX_PATH = Path(__file__).with_name("test-matrix-v1.json")
RUNNER = FRAMEWORK_ROOT / "nxbootstrap/tests/run-isolated.sh"
GUARD = FRAMEWORK_ROOT / "nxbootstrap/tests/private-pid-namespace.sh"
WATCHDOG = FRAMEWORK_ROOT / "nxbootstrap/tests/namespace-watchdog.py"
INTERRUPTION_TEST = FRAMEWORK_ROOT / "nxbootstrap/tests/test-runner-interruption.sh"
NXANDROID_SIGNAL_TEST = FRAMEWORK_ROOT / "nxandroid/tests/test_signal.c"
NXANDROID_INVENTORY = FRAMEWORK_ROOT / "nxandroid/tools/inventory_m11_guests.py"
RUN_LOGGED = FRAMEWORK_ROOT / "tools/run-logged.sh"
CAPTURE = FRAMEWORK_ROOT / "tools/capture-checkpoint.sh"
SAFE_RUNNER = FRAMEWORK_ROOT / "tests/run-safe-gates.sh"
NXCOMPAT_HOST_RUNNER = FRAMEWORK_ROOT / "nxcompat/tests/run-host.sh"
NXCOMPAT_INSTALL_SMOKE = FRAMEWORK_ROOT / "nxcompat/tests/test_install_smoke.c"
NXGL_M13_HOST_RUNNER = FRAMEWORK_ROOT / "nxgl/tests/run-m13-host.sh"
CHRONO_M21_HOST_RUNNER = REPO_ROOT / "ports/chrono/tests/run-m21-host.sh"
CHRONO_M21_BUILD = REPO_ROOT / "ports/chrono/build_universal.sh"
CHRONO_M21_PACKAGE = REPO_ROOT / "ports/chrono/package/build-package.sh"
PINNED_PORTMASTER_VENDOR = (
    FRAMEWORK_ROOT / "portmaster/vendor/PortMaster-GUI-8f9ddc4"
)


PINNED_MUOS_FIXTURES = FRAMEWORK_ROOT / "tests/fixtures/muos-2601.1"


# V3-HARDENING-01: fuzz corpora are hostile-input fixtures by design (trailing
# whitespace, missing final newline, overlong UTF-8, embedded NUL). Cleaning
# them to satisfy source-hygiene lint would destroy the very cases they exist
# to replay, so the corpus directories are excluded from the text-hygiene scan
# exactly like pinned third-party artifacts. Their replayers still audit them.
FUZZ_CORPUS_DIRS = (
    FRAMEWORK_ROOT / "nxinput/tests/corpus",
    FRAMEWORK_ROOT / "nxcompat/tests/corpus-settings",
    # V4-04B adversarial corpora: broken JSON/Python and invalid UTF-8 are the
    # cases those replayers exist to refuse; parsing them here would be a
    # false alarm against fixtures whose brokenness is load-bearing.
    FRAMEWORK_ROOT / "nxgenerator/tests/fixtures/recipe-authority-corpus",
    FRAMEWORK_ROOT / "nxrelease/tests/corpus-nxscan",
)


def is_pinned_portmaster_vendor(path):
    # muOS 2601.1 fixtures are byte-exact third-party artifacts (control.txt,
    # gamecontrollerdb) pinned by sha in test-muos-device-faithful.sh; editing
    # them to satisfy lint would break their provenance, same as the vendor.
    if path == PINNED_MUOS_FIXTURES or PINNED_MUOS_FIXTURES in path.parents:
        return True
    for corpus in FUZZ_CORPUS_DIRS:
        if path == corpus or corpus in path.parents:
            return True
    return path == PINNED_PORTMASTER_VENDOR or PINNED_PORTMASTER_VENDOR in path.parents


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def read(path):
    return path.read_text(encoding="utf-8")


def load_matrix():
    with MATRIX_PATH.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def discovered_test_sources():
    result = set()
    for root in (FRAMEWORK_ROOT,
                 REPO_ROOT / "suportando_outros_devices/extrator-universal",
                 REPO_ROOT / "ports/chrono/tests",
                 REPO_ROOT / "tools/tests"):
        if not root.is_dir():
            continue
        for path in root.rglob("test*"):
            if (path.is_file() and not path.is_symlink() and
                    not is_pinned_portmaster_vendor(path) and
                    path.suffix in (".py", ".sh", ".c")):
                result.add(path.relative_to(REPO_ROOT).as_posix())
    return result


def check_matrix(matrix):
    require(matrix.get("schema_version") == 1,
            "test matrix schema must be 1")
    expected_classes = {"pure", "filesystem", "process", "hardware"}
    require(set(matrix.get("classes", {})) == expected_classes,
            "test matrix must define exactly pure/filesystem/process/hardware")
    policy = matrix.get("policy", {})
    require(policy.get("logged_runner") == "framework/tools/run-logged.sh",
            "canonical logged runner changed")
    require(policy.get("process_runner") ==
            "framework/nxbootstrap/tests/run-isolated.sh",
            "canonical process runner changed")
    require(policy.get("hardware_automatic") is False and
            policy.get("host_signal_authority") == "none",
            "hardware or host signal policy was weakened")

    gates = matrix.get("gates")
    require(isinstance(gates, list) and gates, "test matrix has no gates")
    ids = [gate.get("id") for gate in gates]
    require(all(ids) and len(ids) == len(set(ids)),
            "test gate ids are missing or duplicated")
    covered = set()
    private_ip = re.compile(
        r"(?:10\.|127\.|169\.254\.|192\.168\.|172\.(?:1[6-9]|2[0-9]|3[01])\.)"
    )
    forbidden_commands = {
        "ssh", "scp", "smbclient", "systemctl", "loginctl", "qdbus",
        "dbus-send", "shutdown", "reboot", "poweroff", "setsid", "pkill",
        "killall",
    }
    for gate in gates:
        gate_id = gate["id"]
        gate_class = gate.get("class")
        require(gate_class in expected_classes,
                "gate %s has an unknown class" % gate_id)
        require(gate.get("logged") is True,
                "gate %s bypasses durable logging" % gate_id)
        command = gate.get("command")
        automatic = gate.get("automatic")
        if automatic:
            require(isinstance(command, list) and command,
                    "automatic gate %s has no argv" % gate_id)
            command_text = " ".join(command)
            require(not private_ip.search(command_text),
                    "automatic gate %s contains an IP" % gate_id)
            require(not (set(command) & forbidden_commands),
                    "automatic gate %s contains an external/system command" %
                    gate_id)
        if gate_class == "pure":
            require(gate.get("namespace_required") is False and
                    gate.get("signals") == [],
                    "pure gate %s has process effects" % gate_id)
        elif gate_class == "filesystem":
            require(gate.get("namespace_required") is False and
                    gate.get("signals") == [],
                    "filesystem gate %s can signal a process" % gate_id)
        elif gate_class == "process":
            require(gate.get("namespace_required") is True,
                    "process gate %s lacks namespace requirement" % gate_id)
            require(command == ["bash", "framework/nxbootstrap/tests/run-isolated.sh"],
                    "process gate %s bypasses the sealed runner" % gate_id)
            require(gate.get("signals"),
                    "process gate %s does not declare its signals" % gate_id)
        elif gate_class == "hardware":
            require(automatic is False and command is None,
                    "hardware gate %s became automatic" % gate_id)
            require(gate.get("signals") == [],
                    "hardware gate %s pre-authorizes signals" % gate_id)
        for source in gate.get("sources", []):
            source_path = REPO_ROOT / source
            require(source_path.is_file() and not source_path.is_symlink(),
                    "gate %s source is missing/unsafe: %s" % (gate_id, source))
            covered.add(source)
        for support_file in gate.get("support_files", []):
            support_path = REPO_ROOT / support_file
            require(support_path.is_file() and not support_path.is_symlink(),
                    "gate %s support file is missing/unsafe: %s" %
                    (gate_id, support_file))

    discovered = discovered_test_sources()
    missing = sorted(discovered - covered)
    extra = sorted(covered - discovered)
    require(not missing, "unclassified test source(s): %s" % ", ".join(missing))
    require(not extra, "matrix source is not a discovered test: %s" %
            ", ".join(extra))
    hardware_ids = {gate["id"] for gate in gates if gate["class"] == "hardware"}
    require("chrono-device-pilot" in hardware_ids,
            "real-device integration is not separated from host gates")
    bootstrap = next(gate for gate in gates
                     if gate["id"] == "bootstrap-isolated")
    nxsplash = next((gate for gate in gates
                     if gate["id"] == "nxsplash-host"), None)
    require(nxsplash is not None and
            nxsplash.get("class") == "filesystem" and
            nxsplash.get("automatic") is True and
            nxsplash.get("command") ==
            ["bash", "framework/nxsplash/tests/run.sh"] and
            nxsplash.get("sources") ==
            ["framework/nxsplash/tests/test-fbdev.c"] and
            nxsplash.get("namespace_required") is False and
            nxsplash.get("signals") == [],
            "nxsplash release verification escaped its automatic host gate")
    inventory_path = "framework/nxandroid/tools/inventory_m11_guests.py"
    require(inventory_path in bootstrap.get("support_files", []) and
            all(inventory_path not in (gate.get("command") or [])
                for gate in gates if gate["class"] != "process"),
            "bounded inventory supervisor escaped the sealed process gate")


def logical_shell_lines(source):
    pending = ""
    for raw_line in source.splitlines():
        stripped = raw_line.strip()
        if not pending and (not stripped or stripped.startswith("#")):
            continue
        if raw_line.rstrip().endswith("\\"):
            pending += raw_line.rstrip()[:-1] + " "
            continue
        yield pending + raw_line
        pending = ""
    require(not pending, "safe runner ends with an incomplete continuation")


def check_safe_runner(matrix):
    source = read(SAFE_RUNNER)
    actual = []
    for line in logical_shell_lines(source):
        stripped = line.strip()
        # run_gate_external is the same invocation shape; it only differs in
        # tolerating exit 77 for a gate that needs an owner-supplied artifact,
        # and it must therefore be inventoried exactly like run_gate.
        if (stripped.startswith("run_gate ") or
                stripped.startswith("run_gate_external ")):
            try:
                words = shlex.split(stripped, comments=True, posix=True)
            except ValueError as error:
                raise AssertionError("safe runner cannot be parsed: %s" % error)
            require(len(words) >= 3,
                    "safe runner has an incomplete gate invocation")
            actual.append((words[1], words[2:]))

    expected = [(gate["id"], gate["command"])
                for gate in matrix["gates"] if gate.get("automatic")]
    actual_ids = [gate_id for gate_id, _ in actual]
    expected_ids = [gate_id for gate_id, _ in expected]
    require(len(actual_ids) == len(set(actual_ids)),
            "safe runner contains a duplicated gate")
    require(set(actual_ids) == set(expected_ids),
            "safe runner gates differ from the automatic matrix: actual=%s expected=%s" %
            (actual_ids, expected_ids))
    expected_commands = dict(expected)
    for gate_id, command in actual:
        require(command == expected_commands[gate_id],
                "safe runner command diverges for %s: %r != %r" %
                (gate_id, command, expected_commands[gate_id]))
    require(actual_ids[:2] == ["test-infrastructure", "toolchain-preflight"],
            "infrastructure and pinned toolchain preflight must be the first "
            "two safe gates")
    toolchain_preflight = next(
        (gate for gate in matrix["gates"]
         if gate.get("id") == "toolchain-preflight"), None)
    require(toolchain_preflight is not None and
            toolchain_preflight.get("class") == "filesystem" and
            toolchain_preflight.get("automatic") is True and
            toolchain_preflight.get("command") ==
            ["python3", "-B", "framework/nxabi/nxabi.py", "toolchain"] and
            toolchain_preflight.get("support_files") == [
                "framework/nxabi/nxabi.py",
                "framework/nxabi/TOOLCHAIN-PIN.json",
            ] and
            toolchain_preflight.get("namespace_required") is False and
            toolchain_preflight.get("signals") == [],
            "toolchain preflight no longer checks the pinned local inputs")
    require(actual_ids.index("bootstrap-static-safety") <
            actual_ids.index("bootstrap-isolated") and
            actual_ids.index("tooling-filesystem") <
            actual_ids.index("bootstrap-isolated"),
            "process isolation runs before its static/filesystem prerequisites")
    require("hardware_ran=0 device_access=0" in source,
            "safe runner does not explicitly report zero hardware access")


def check_m12_host_gate(matrix):
    by_id = {gate["id"]: gate for gate in matrix["gates"]}
    host = by_id.get("nxcompat-host")
    require(host is not None and host.get("class") == "filesystem" and
            host.get("automatic") is True and
            host.get("command") ==
            ["bash", "framework/nxcompat/tests/run-host.sh"] and
            host.get("signals") == [],
            "M12 host gate lost its automatic filesystem-only contract")
    require(set(host.get("sources", [])) == {
                "framework/nxcompat/tests/test_nxcompat.c",
                "framework/nxcompat/tests/test_sdl2_adapter.c",
                "framework/nxcompat/tests/test_m12_probe.c",
                "framework/nxcompat/tests/test_nxcompat_registry.c",
                "framework/nxcompat/tests/test_install_smoke.c",
                "framework/nxgl/tests/test_nxgl_environment.c",
                "framework/nxgl/tests/test_nxgl_nxcompat.c",
                "framework/nxinput/tests/test_nxinput_nxcompat.c",
                "framework/nxinput/tests/test_evdev_chord_map.c",
                "framework/nxcompat/tests/test_sdl_optional.c",
                "framework/nxcompat/tests/test_sdl_optional_fixture.c",
                "framework/nxcompat/tests/test_sdl_optional_provider.c",
            }, "M12 host source classification changed")
    nxgl_native = by_id.get("nxgl-native")
    require(nxgl_native is not None and
            nxgl_native.get("class") == "filesystem" and
            nxgl_native.get("automatic") is False and
            nxgl_native.get("command") is None and
            nxgl_native.get("sources") ==
            ["framework/nxgl/tests/test_nxgl.c"],
            "native NXGL test escaped its manual filesystem class")
    nxinput_native = by_id.get("nxinput-native")
    require(nxinput_native is not None and
            nxinput_native.get("class") == "hardware" and
            nxinput_native.get("automatic") is False and
            nxinput_native.get("command") is None and
            nxinput_native.get("sources") ==
            ["framework/nxinput/tests/test_nxinput.c"],
            "native NXInput test escaped its manual hardware class")

    runner = read(NXCOMPAT_HOST_RUNNER)
    for token in (
            "export SDL_AUDIODRIVER=dummy",
            "export SDL_VIDEODRIVER=dummy",
            "-DNXGL_BUILD_NATIVE_TESTS=OFF",
            "-DNXINPUT_BUILD_NATIVE_TESTS=OFF",
            'cd -- "$work_root/analyzer-clang"',
            "test_install_smoke.c",
            "bin/nxcompat-probe",
            "cmp -s",
            "guest_code_executed=0 hardware_ran=0 device_access=0 network_access=0"):
        require(token in runner, "M12 host runner lacks: %s" % token)
    require(runner.index('if nm -u "$build/test-nxcompat-sdl2"') <
            runner.index('ctest --test-dir "$build" --output-on-failure'),
            "SDL symbol barrier runs after nxcompat CTest")
    nxgl_ctest = runner.index("-R '^nxgl-(environment|nxcompat)$'")
    require(runner.index('if nm -u "$build/test-nxgl-nxcompat"') <
            nxgl_ctest,
            "graphics symbol barrier runs after NXGL CTest")
    require(runner.index('if nm -u "$build/test-nxgl-environment"') <
            nxgl_ctest,
            "video environment symbol barrier runs after NXGL CTest")
    require('readelf -d "$build/test-nxgl-environment"' in runner,
            "video environment fixture lost its provider dependency barrier")
    require(runner.index('if nm -u "$build/test-nxinput-nxcompat"') <
            runner.index("-R '^(nxinput-static-gate|nxinput-nxcompat|nxinput-evdev-chord)$'"),
            "input symbol barrier runs after NXInput CTest")
    require(runner.count('"$work_root/install-smoke-$compiler"') == 1,
            "installed consumer smoke must be link-only")
    for archive in (
            "libnxcompat.a", "libnxcompat-sdl2.a", "libnxgl.a",
            "libnxgl-nxcompat.a", "libnxinput.a",
            "libnxinput-nxcompat.a"):
        require(archive in runner,
                "installed archive is not checked: %s" % archive)
    require("find \"$work_root\" -depth -delete" in runner,
            "M12 host gate no longer cleans its exact work root")

    smoke = read(NXCOMPAT_INSTALL_SMOKE)
    for token in (
            "nxcompat_reason_name", "nxcompat_sdl2_negotiate_audio_v2",
            "nxgl_open_options_init", "nxgl_nxcompat_publish_context",
            "nxinput_config_init", "nxinput_nxcompat_publish_context"):
        require(token in smoke, "install smoke lost public symbol: %s" % token)

    nxgl_cmake = read(FRAMEWORK_ROOT / "nxgl/CMakeLists.txt")
    nxinput_cmake = read(FRAMEWORK_ROOT / "nxinput/CMakeLists.txt")
    require(re.search(r'option\(NXGL_BUILD_NATIVE_TESTS\s+"[^"]*"\s+OFF\)',
                      nxgl_cmake),
            "NXGL native tests are not opt-in")
    require(re.search(
                r'option\(NXINPUT_BUILD_NATIVE_TESTS\s+"[^"]*"\s+OFF\)',
                nxinput_cmake),
            "NXInput hardware-facing tests are not opt-in")
    require(" CACHE BOOL \"\" FORCE)" not in nxgl_cmake + nxinput_cmake,
            "embedded components overwrite caller cache policy")


def check_m13_host_gate(matrix):
    by_id = {gate["id"]: gate for gate in matrix["gates"]}
    host = by_id.get("nxgl-m13-host")
    require(host is not None and host.get("class") == "filesystem" and
            host.get("automatic") is True and
            host.get("command") ==
            ["bash", "framework/nxgl/tests/run-m13-host.sh"] and
            host.get("namespace_required") is False and
            host.get("signals") == [],
            "M13 host gate lost its automatic filesystem-only contract")
    require(host.get("sources") == [
                "framework/nxgl/tests/test_nxgl_open_v2.c",
                "framework/nxgl/tests/test_nxgl_present_v2.c",
                "framework/nxgl/tests/test_nxgl_metrics.c",
                "framework/nxgl/tests/test_nxgl_diagnostics.c",
                "framework/nxgl/tests/test_nxgl_sdl_hint.c",
                "framework/nxgl/tests/test_nxgl_provider_recovery.c",
            ], "M13 host source classification changed")
    require(host.get("support_files") == [
                "framework/nxgl/tests/run-m13-host.sh",
                "framework/nxgl/CMakeLists.txt",
                "framework/nxgl/include/nxgl_provider_recovery.h",
                "framework/nxgl/src/nxgl_arbiter.c",
                "framework/nxgl/src/nxgl_sdl_hint.c",
                "framework/nxgl/src/nxgl_provider_recovery.c",
                "framework/nxgl/tests/fake_provider_good.c",
                "framework/nxgl/tests/fake_provider_dependency.c",
                "framework/nxgl/tests/fake_provider_mixed.c",
            ], "M13 host support boundary changed")

    audit = by_id.get("nxgl-m13-audit")
    require(audit is not None and audit.get("class") == "pure" and
            audit.get("automatic") is True and
            audit.get("command") ==
            ["python3", "-B", "framework/nxgl/tests/test_m13_audit.py"] and
            audit.get("sources") ==
            ["framework/nxgl/tests/test_m13_audit.py"] and
            audit.get("signals") == [],
            "M13 audit lost its process-free automatic contract")

    runner = read(NXGL_M13_HOST_RUNNER)
    for token in (
            "mktemp -d /tmp/nxgl-m13-host.XXXXXX",
            "-DNXGL_BUILD_NATIVE_TESTS=OFF",
            "test-nxgl-open-v2", "test-nxgl-present-v2",
            "test-nxgl-metrics", "test-nxgl-diagnostics",
            "test-nxgl-sdl-hint",
            "test-nxgl-provider-recovery", "check_provider_recovery_archive",
            "seal_fake_executable", "nm -u", "readelf -d",
            "physical_device_evidence=0", "device_access=0",
            "network_access=0", "session_access=0",
            'find "$work_root" -depth -delete'):
        require(token in runner, "M13 host runner lacks: %s" % token)
    ctest_anchor = ("-R '^nxgl-m13-(open-v2|present-v2|metrics|diagnostics|"
                    "sdl-hint|provider-recovery)$'")
    require(runner.count("ctest --test-dir") == 1 and
            ctest_anchor in runner,
            "M13 host runner can execute tests outside the sealed set")
    require(runner.index('for executable in "${m13_executables[@]}"; do') <
            runner.index(ctest_anchor),
            "M13 symbol/dependency seals run after CTest")
    require('cmake --build "$build" --target nxgl ' in runner and
            '"${m13_executables[@]}"' in runner,
            "M13 runner builds an unbounded/native target set")
    require("NXGL_M13_TESTING=1" in runner and
            "NXGL_PRESENT_V2_TESTING=1" in runner,
            "M13 analyzers lost their private provider boundaries")

    cmake = read(FRAMEWORK_ROOT / "nxgl/CMakeLists.txt")
    for token in (
            "src/nxgl_diagnostics.c", "src/nxgl_metrics.c",
            "NXGL_M13_TESTING=1", "NXGL_PRESENT_V2_TESTING=1",
            "add_test(NAME nxgl-m13-open-v2",
            "add_test(NAME nxgl-m13-present-v2",
            "add_test(NAME nxgl-m13-metrics",
            "add_test(NAME nxgl-m13-diagnostics",
            "add_test(NAME nxgl-m13-sdl-hint",
            "add_test(NAME nxgl-m13-provider-recovery"):
        require(token in cmake, "M13 CMake boundary lacks: %s" % token)
    require("install(TARGETS test-nxgl" not in cmake,
            "M13 test-only target escaped into installation")


def check_m21_pilot_gates(matrix):
    by_id = {gate["id"]: gate for gate in matrix["gates"]}
    audit = by_id.get("chrono-m21-audit")
    require(audit is not None and audit.get("class") == "pure" and
            audit.get("automatic") is True and audit.get("command") ==
            ["python3", "-B", "ports/chrono/tests/test_m21_pilot.py"] and
            audit.get("sources") ==
            ["ports/chrono/tests/test_m21_pilot.py"] and
            audit.get("support_files") == [
                "ports/chrono/references/m21-pilot-v1.json",
                "ports/chrono/references/m21-host-receipt-v1.json"] and
            audit.get("signals") == [],
            "M21 audit lost its process-free prephysical contract")
    host = by_id.get("chrono-m21-host")
    require(host is not None and host.get("class") == "filesystem" and
            host.get("automatic") is False and
            host.get("scope") == "legacy-port-specific" and
            host.get("command") ==
            ["bash", "ports/chrono/tests/run-m21-host.sh"] and
            host.get("namespace_required") is False and
            host.get("signals") == [],
            "M21 host gate escaped its manual legacy-port scope")
    m22 = by_id.get("chrono-m22-audit")
    require(m22 is not None and m22.get("class") == "pure" and
            m22.get("automatic") is True and m22.get("command") ==
            ["python3", "-B", "ports/chrono/tests/test_m22_physical.py"] and
            m22.get("sources") ==
            ["ports/chrono/tests/test_m22_physical.py"] and
            m22.get("support_files") ==
            ["ports/chrono/references/m22-physical-receipt-v1.json"] and
            m22.get("namespace_required") is False and
            m22.get("signals") == [],
            "M22 physical receipt escaped its process-free audit")
    m23 = by_id.get("chrono-m23-audit")
    require(m23 is not None and m23.get("class") == "pure" and
            m23.get("automatic") is False and
            m23.get("scope") == "legacy-port-specific" and
            m23.get("command") ==
            ["python3", "-B", "ports/chrono/tests/test_m23_promotion.py"] and
            m23.get("sources") ==
            ["ports/chrono/tests/test_m23_promotion.py"] and
            m23.get("support_files") == [
                "ports/chrono/references/m23-promotion-v1.json",
                "framework/catalog/ports-v1.json",
                "framework/catalog/port-checks-v1.tsv"] and
            m23.get("namespace_required") is False and
            m23.get("signals") == [],
            "M23 promotion escaped its manual legacy-port scope")
    m24 = by_id.get("chrono-m24-closure")
    require(m24 is not None and m24.get("class") == "pure" and
            m24.get("automatic") is False and
            m24.get("scope") == "legacy-port-specific" and
            m24.get("command") ==
            ["python3", "-B", "ports/chrono/tests/test_m24_closure.py"] and
            m24.get("sources") ==
            ["ports/chrono/tests/test_m24_closure.py"] and
            m24.get("support_files") ==
            ["ports/chrono/references/m24-closure-v1.json"] and
            m24.get("namespace_required") is False and
            m24.get("signals") == [],
            "M24 closure escaped its manual legacy-port scope")
    hardware = by_id.get("chrono-device-pilot")
    require(hardware is not None and hardware.get("class") == "hardware" and
            hardware.get("automatic") is False and
            hardware.get("command") is None and
            hardware.get("signals") == [],
            "Chrono physical acceptance escaped its manual hardware gate")

    runner = read(CHRONO_M21_HOST_RUNNER)
    for token in (
            "test_m21_pilot.py\"", "clean builds differ",
            "deterministic packages differ", "nm -g", "readelf -h",
            "PACKAGE-MANIFEST.sha256", "packaged_elfs=2",
            "external_guest_code_executed=0",
            "external_guest_initializers_executed=0",
            "external_guest_jni_onload_executed=0", "hardware_ran=0",
            "physical_device_evidence=0", "device_access=0",
            "network_access=0", "session_access=0",
            'find "$WORK_ROOT" -depth -delete'):
        require(token in runner, "M21 host runner lacks: %s" % token)
    for forbidden in ("ssh ", "scp ", "smbclient", "systemctl", "setsid"):
        require(forbidden not in runner,
                "M21 host runner gained external/hardware action: %s" %
                forbidden)
    require(runner.index("public_symbols=$(nm -g") <
            runner.index("CT_PACKAGE_BINARY="),
            "M21 loader symbol seal runs after packaging")


def check_m12a_observability_gate(matrix):
    by_id = {gate["id"]: gate for gate in matrix["gates"]}
    gate = by_id.get("nxobs-m12a-host")
    require(gate is not None and gate.get("class") == "filesystem" and
            gate.get("automatic") is True and gate.get("command") ==
            ["python3", "-B",
             "framework/nxobs/tests/test_m12a_observability.py"] and
            gate.get("sources") ==
            ["framework/nxobs/tests/test_m12a_observability.py"] and
            gate.get("support_files") == [
                "framework/nxobs/nx-support-bundle.py",
                "framework/nxobs/schema-v1.json",
                "framework/nxobs/m12a-observability-v1.json",
                "framework/nxobs/references/m12a-real-examples-v1.json"] and
            gate.get("namespace_required") is False and
            gate.get("signals") == [],
            "M12A support-bundle gate lost its hermetic filesystem contract")
    support_flow = by_id.get("nxobs-support-flow")
    require(
        support_flow is not None and
        support_flow.get("class") == "filesystem" and
        support_flow.get("automatic") is True and
        support_flow.get("command") ==
            ["bash", "tools/tests/test-nx-logs-privacy.sh"] and
        support_flow.get("sources") ==
            ["tools/tests/test-nx-logs-privacy.sh"] and
        support_flow.get("support_files") == [
            "tools/nx-logs", "framework/nxobs/nx-support-bundle.py"] and
        support_flow.get("namespace_required") is False and
        support_flow.get("signals") == [],
        "nx-logs public support flow lost its hermetic privacy gate")
    tool = read(FRAMEWORK_ROOT / "nxobs/nx-support-bundle.py")
    for token in ("MAX_INPUT_BYTES = 8 * 1024 * 1024",
                  "MAX_LINE_BYTES = 65536", "MAX_EVENTS = 2048",
                  '"raw_logs_included": False', "os.replace",
                  "MANIFEST.sha256", "input must be a regular non-symlink"):
        require(token in tool, "M12A tool boundary lacks: %s" % token)
    for forbidden in ("socket.", "requests.", "subprocess.", "os.system("):
        require(forbidden not in tool,
                "M12A filesystem gate gained external/process access: %s" %
                forbidden)

    build = read(CHRONO_M21_BUILD)
    require("--network none" in build and
            "sha256:036c7910ea53bc78cc213452afa92fa83d55de1c51ae54f315af58b5a41a45cf" in build and
            "apt-get" not in build and "debian:buster" not in build,
            "M21 public build is not the pinned offline M17 route")
    package = read(CHRONO_M21_PACKAGE)
    require("umask 022" in package and
            "chrono/nxextract/nxextract-ui" in package and
            "glibc_at_most \"$candidate\" 30" in package,
            "M21 package lost deterministic/low-glibc boundaries")


def check_p06_crash_gate(matrix):
    by_id = {gate["id"]: gate for gate in matrix["gates"]}
    gate = by_id.get("bootstrap-isolated")
    source = "framework/nxobs/tests/test_p06_observability.py"
    required_support = {
        "framework/nxobs/VERSION",
        "framework/nxobs/crash-schema-v1.json",
        "framework/nxobs/p06-crash-contract-v1.json",
        "framework/nxobs/include/nxobs_crash.h",
        "framework/nxobs/src/nxobs_crash.c",
        "framework/nxobs/tests/crash-fixture.c",
        "framework/nxobs/nx-support-bundle.py",
        "framework/nxobs/nx-symbolize-crash.py",
        "framework/nxobs/nx-archive-symbols.py",
    }
    require(gate is not None and source in gate.get("sources", []) and
            required_support <= set(gate.get("support_files", [])) and
            any("SIGSEGV exact nxobs crash-fixture child" in item
                for item in gate.get("signals", [])) and
            any("SIGABRT exact nxobs crash-fixture child" in item
                for item in gate.get("signals", [])),
            "P06 native crash gate escaped the sealed process contract")
    isolated = read(FRAMEWORK_ROOT / "nxbootstrap/tests/isolated-suite.sh")
    require("../../nxobs/tests/test_p06_observability.py" in isolated,
            "P06 native crash gate is declared but not executed")
    implementation = read(FRAMEWORK_ROOT / "nxobs/src/nxobs_crash.c")
    for token in ("SA_RESETHAND", "kill(getpid(), signal_number)",
                  "fsync(g_receipt_fd)", "dl_iterate_phdr"):
        require(token in implementation,
                "P06 native crash boundary lacks: %s" % token)


# Ferramentas de repositorio sem extensao que falam com device: entram na
# MESMA auditoria (sintaxe, tokens de sistema/remoto, whitespace), com a
# linguagem lida do shebang. Antes de 2026-08-23 tools/ ficava fora de tudo --
# nx-verify/nx-kill-port usavam ssh sem nunca passar pelo scanner.
AUDITED_EXTENSIONLESS_TOOLS = frozenset((
    "tools/nx-verify",
    "tools/nx-kill-port",
    "tools/nx-logs",
))

AUDITED_ROOTS = ("framework",
                 "suportando_outros_devices/extrator-universal",
                 "tools")


def audited_language(path):
    if path.suffix == ".sh":
        return "sh"
    if path.suffix == ".py":
        return "py"
    if path.suffix == ".json":
        return "json"
    if path.relative_to(REPO_ROOT).as_posix() in AUDITED_EXTENSIONLESS_TOOLS:
        shebang = read(path).split("\n", 1)[0]
        if "python" in shebang:
            return "py"
        if re.search(r"\b(?:ba)?sh\b", shebang):
            return "sh"
        raise AssertionError(
            "audited extensionless tool has no recognizable shebang: %s"
            % path.relative_to(REPO_ROOT))
    return None


def is_audited_shell(path):
    return audited_language(path) == "sh"


def audited_source_files():
    for root_name in AUDITED_ROOTS:
        root = REPO_ROOT / root_name
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if (path.is_file() and not path.is_symlink() and
                    not is_pinned_portmaster_vendor(path) and
                    (path.suffix in (".py", ".sh", ".json") or
                     path.relative_to(REPO_ROOT).as_posix()
                     in AUDITED_EXTENSIONLESS_TOOLS)):
                yield path


def audited_all_files():
    for root_name in AUDITED_ROOTS:
        root = REPO_ROOT / root_name
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if (path.is_file() and not path.is_symlink() and
                    not is_pinned_portmaster_vendor(path)):
                yield path


def check_source_syntax_and_system_surface():
    paths = list(audited_source_files())
    shell_paths = [path for path in paths if is_audited_shell(path)]
    require(shell_paths, "shell syntax audit found no scripts to check")
    # Um unico `bash -n a.sh b.sh ...` so' analisa o PRIMEIRO arquivo (os
    # demais viram parametros posicionais): o audit passou anos validando 1
    # script de ~54. Tem que ser uma invocacao por arquivo.
    broken = []
    for shell_path in shell_paths:
        syntax = subprocess.run(
            ["bash", "-n", str(shell_path)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if syntax.returncode != 0:
            broken.append("%s: %s" % (
                shell_path.relative_to(REPO_ROOT).as_posix(),
                syntax.stderr.strip().splitlines()[-1]
                if syntax.stderr.strip() else "bash -n failed"))
    require(not broken,
            "shell syntax audit failed (%d files): %s" %
            (len(broken), "; ".join(broken)))
    for path in paths:
        language = audited_language(path)
        if language == "py":
            ast.parse(read(path), filename=str(path))
        elif language == "json":
            json.loads(read(path))

    system_pattern = re.compile(
        r"(?<![A-Za-z0-9_-])(?:systemctl|loginctl|qdbus|dbus-send|"
        r"shutdown|reboot|poweroff|setsid|pkill|killall)"
        r"(?![A-Za-z0-9_-])"
    )
    system_mentions = {
        path.relative_to(REPO_ROOT).as_posix()
        for path in paths
        if audited_language(path) in ("py", "sh") and
        system_pattern.search(read(path))
    }
    expected_system_validators = {
        "framework/nxbootstrap/tests/test-manifest-contract.py",
        "framework/nxbootstrap/tests/test-safety-static.sh",
        "framework/nxandroid/tests/test_m11_audit.py",
        "framework/nxandroid/tools/inventory_m11_guests.py",
        "framework/nxrelease/nxrelease.py",
        "framework/nxobs/nx-support-bundle.py",
        # Reviewed 2026-08-16: the device harness stops and restores the
        # frontend unit around a run and ends leftover port processes by
        # executable directory. Both are the harness's whole purpose, and the
        # frontend is restored on every exit path including interrupts.
        "framework/nxobs/nx-device-launch.sh",
        "framework/nxobs/tests/test_m12a_observability.py",
        "framework/portmaster/tests/test_portmaster_contract.py",
        "framework/tests/audit-portmaster-zip.py",
        "framework/tests/test_infrastructure.py",
        # Reviewed 2026-08-23: the token here is the libc socket symbol
        # shutdown(2) inside the template's symbol-allowlist regex, not a
        # power/session command.
        "tools/new-port-arm.sh",
    }
    require(system_mentions == expected_system_validators,
            "session/power/broad-process token escaped its reviewed validators: %s" %
            sorted(system_mentions))

    remote_pattern = re.compile(
        r"(?<![A-Za-z0-9_-])(?:ssh|scp|smbclient)(?![A-Za-z0-9_-])"
    )
    remote_mentions = {
        path.relative_to(REPO_ROOT).as_posix()
        for path in paths
        if audited_language(path) in ("py", "sh") and
        remote_pattern.search(read(path))
    }
    require(remote_mentions == {
                "framework/nxandroid/tests/test_m11_audit.py",
                # Reviewed 2026-08-16: the device harness and the one-command
                # release path talk to a device only through an IP passed at
                # runtime; no address is baked in, and each refuses a
                # non-literal host.
                "framework/nxobs/nx-device-launch.sh",
                # Reviewed 2026-09-02: the on-device automated input proof
                # (nxinput 0.10.2) reaches the device only through the IP given
                # at runtime, refuses a non-literal host, uploads its agent to a
                # private /tmp directory it created and removes it afterwards.
                "framework/nxinput/tools/nx-device-input-proof.py",
                "framework/nxrelease/nx-ship-port.sh",
                # Reviewed 2026-09-03: the /2 prompt-capture proof reaches a
                # device only through the `user@host` given at runtime
                # (`--target`), bakes no address, uses BatchMode ssh/scp to
                # read /dev/fb0 into a private temp file and removes it
                # afterwards. Its `--offline` mode, the one the host gate
                # exercises, performs no network access at all.
                "framework/tools/nx-prompt-capture-proof.py",
                "framework/tests/test_infrastructure.py",
                # Reviewed 2026-08-23: device tools take the IP at runtime and
                # never bake an address; both entered the audited set on this
                # date (they previously escaped every scanner).
                "tools/nx-verify",
                "tools/nx-kill-port",
                "tools/nx-logs",
                # Hermetic gate: the token names a PATH-local fake that emits
                # synthetic TAR fixtures; it performs no network access.
                "tools/tests/test-nx-logs-privacy.sh",
            },
            "automatic infrastructure gained a remote-access token: %s" %
            sorted(remote_mentions))

    for path in audited_all_files():
        payload = path.read_bytes()
        if not payload or b"\0" in payload:
            continue
        try:
            text_payload = payload.decode("utf-8")
        except UnicodeDecodeError:
            continue
        require(text_payload.endswith("\n"),
                "text source lacks a final newline: %s" %
                path.relative_to(REPO_ROOT))
        # Engine seam patches carry literal upstream diff context (Godot/SDL
        # sources whose lines legitimately end in whitespace); their bytes are
        # authenticated by sha256 in the C5B/C6 PROVENANCE ledgers instead.
        upstream_patch = (path.suffix == ".patch" and
                          path.parent in (FRAMEWORK_ROOT / "nxinput" /
                                          "engine-patches",
                                          FRAMEWORK_ROOT / "nxinput" / "tests" /
                                          "providers" / "patches"))
        for line_number, line in enumerate(text_payload.splitlines(), 1):
            require(upstream_patch or not line.endswith((" ", "\t")),
                    "trailing whitespace: %s:%d" %
                    (path.relative_to(REPO_ROOT), line_number))
            require(not re.match(r"^(?:<<<<<<<|=======|>>>>>>>)", line),
                    "merge conflict marker: %s:%d" %
                    (path.relative_to(REPO_ROOT), line_number))


def check_namespace_guard():
    runner = read(RUNNER)
    guard = read(GUARD)
    require("unshare --user --map-root-user --pid --fork --kill-child=KILL" in runner,
            "runner lost user/PID isolation or kill-child containment")
    require("--mount-proc" in runner,
            "runner no longer mounts private procfs")
    for token in (
            "HOST_PID_NS_FD", "HOST_USER_NS_FD", "HOST_MOUNT_NS_FD",
            "exec {host_pid_ns_fd}</proc/self/ns/pid",
            "exec {host_user_ns_fd}</proc/self/ns/user",
            "exec {host_mount_ns_fd}</proc/self/ns/mnt"):
        require(token in runner,
                "runner does not seal namespace identity: %s" % token)
    require("[[ $$ -eq 1 ]]" in runner,
            "inner namespace runner does not require PID 1")
    require("exec python3 -B \"$TEST_DIR/namespace-watchdog.py\"" in runner,
            "isolated suite bypasses namespace watchdog")
    require("--wall-seconds 4096" in runner and
            "--cpu-seconds 240" in runner,
            "isolated suite lost its contention-tolerant finite resource fuse")
    require("private PID namespace could not start" in runner and
            "exit 77" in runner,
            "runner no longer fails closed when unshare fails")

    for token in (
            "sealed namespace descriptor is absent",
            "host PID namespace descriptor mismatch",
            "host user namespace descriptor mismatch",
            "host mount namespace descriptor mismatch",
            'current_user_ns != "$host_user_ns"',
            'current_mount_ns != "$host_mount_ns"',
            'current_ns != "$host_ns"',
            'current_ns == "$init_ns"'):
        require(token in guard,
                "namespace guard is missing: %s" % token)


def function_node(tree, name):
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == name:
            return node
    raise AssertionError("watchdog function is missing: %s" % name)


def check_watchdog():
    source = read(WATCHDOG)
    tree = ast.parse(source, filename=str(WATCHDOG))
    main = function_node(tree, "main")
    send = function_node(tree, "send_if_owned")
    main_source = ast.get_source_segment(source, main) or ""
    send_source = ast.get_source_segment(source, send) or ""
    require(main_source.index("require_private_namespace()") <
            main_source.index("subprocess.Popen"),
            "watchdog starts a child before proving namespace isolation")
    require("process_starttime(process.pid)" in main_source,
            "watchdog does not capture direct-child starttime")
    require("same_process(process.pid, starttime)" in send_source and
            "os.kill(process.pid, selected_signal)" in send_source,
            "watchdog signal path is not PID+starttime constrained")
    kill_calls = [node for node in ast.walk(tree)
                  if isinstance(node, ast.Call) and
                  isinstance(node.func, ast.Attribute) and
                  isinstance(node.func.value, ast.Name) and
                  node.func.value.id == "os" and node.func.attr == "kill"]
    require(len(kill_calls) == 1 and kill_calls[0] in list(ast.walk(send)),
            "watchdog has a signal path outside send_if_owned")
    for token in ("RLIMIT_CORE", "RLIMIT_CPU", "RLIMIT_AS", "RLIMIT_FSIZE",
                  "RLIMIT_NPROC", "TimeoutExpired", "return TIMEOUT"):
        require(token in source, "watchdog limit is missing: %s" % token)


def check_signal_tests():
    shell_tests = [path for path in FRAMEWORK_ROOT.rglob("test*.sh")
                   if path.is_file()]
    signal_lines = []
    pattern = re.compile(
        r"^\s*(?:if\s+!\s+)?(?:builtin\s+)?kill\s+-")
    for path in shell_tests:
        for line in read(path).splitlines():
            if pattern.match(line):
                signal_lines.append((path, line.strip()))
    behavior_test = (FRAMEWORK_ROOT / "nxbootstrap" / "tests" /
                     "test-launcher-behavior.sh")
    # Only two files may signal, and every signal must target a controlled PID
    # variable of a process this test itself spawned inside the private PID
    # namespace — never a name, a sweep, or a host PID. This is robust to
    # editing the behavioral cases without weakening the no-host-signal rule.
    # The real-crash rollback gate runs only inside isolated-suite.sh (private
    # PID namespace) and signals its own spawned launcher child or itself
    # ("$$", the fake guest reproducing exit 134/143).
    rollback_test = (FRAMEWORK_ROOT / "nxbootstrap" / "tests" /
                     "test-v4-rollback-real-crash.sh")
    allowed_files = {INTERRUPTION_TEST, behavior_test, rollback_test}
    # $runner_pid/$launcher_pid/$first/$daemon_pid/$video_sentinel_pid:
    # spawned launchers/children;
    # $pid: the loop parameter of wait_for_process_exit; $signal_name: the
    # signal, not a target.
    target_ok = re.compile(
        r'kill\s+-(?:"\$signal_name"|[A-Z0-9]+)\s+'
        r'"\$(?:runner_pid|launcher_pid|first|daemon_pid|video_sentinel_pid|pid|\$)"')
    for path, line in signal_lines:
        require(path in allowed_files,
                "unexpected file sends a real signal: %s" % path)
        require(target_ok.search(line),
                "signal does not target a controlled spawned PID: %r" % line)
    require(any(path == INTERRUPTION_TEST for path, _ in signal_lines),
            "runner interruption test lost its signal path")
    require(any(path == behavior_test for path, _ in signal_lines),
            "behavioral launcher gate lost its signal coverage")
    interruption = read(INTERRUPTION_TEST)
    require("nxbootstrap_require_private_pid_namespace" in interruption,
            "runner interruption test lacks namespace guard")
    require(interruption.index("runner_starttime=$(process_starttime") <
            interruption.index('builtin kill -TERM "$runner_pid"'),
            "runner interruption test signals before recording starttime")
    require('$(process_starttime "$runner_pid") == "$runner_starttime"' in
            interruption,
            "runner interruption test does not revalidate ownership")
    behavior = read(behavior_test)
    require("nxbootstrap_require_private_pid_namespace" in behavior and
            "for signal_case in HUP:129 INT:130 TERM:143" in behavior,
            "launcher behavior signal test lost isolation or exact statuses")
    require('XDG_RUNTIME_DIR="$RUNTIME_DIR"' in behavior and
            'mv -f "$PORTS_DIR/behav-port/bin/behav-loader.next"' in behavior and
            'flock -n "$LOCK_FILE" -c true' in behavior,
            "launcher behavior test no longer proves a replacement-stable lock")
    sentinel_spawn = behavior.index("video_sentinel_pid=$!")
    sentinel_capture = behavior.index(
        'video_sentinel_start=$(live_process_starttime "$video_sentinel_pid")')
    sentinel_recheck = behavior.index(
        '[[ $(live_process_starttime "$video_sentinel_pid") ==')
    sentinel_signal = behavior.index(
        'kill -TERM "$video_sentinel_pid"')
    require(sentinel_spawn < sentinel_capture < sentinel_recheck <
            sentinel_signal,
            "video sentinel signal is not spawn/starttime/recheck constrained")

    process_sources = [
        FRAMEWORK_ROOT / "nxbootstrap/tests/test-runner-interruption.sh",
        FRAMEWORK_ROOT / "nxbootstrap/tests/test-namespace-watchdog.sh",
    ]
    sleep_pattern = re.compile(r"\bsleep\s+([0-9]+(?:\.[0-9]+)?)")
    for path in process_sources:
        for value in sleep_pattern.findall(read(path)):
            require(float(value) <= 1.0,
                    "process test has a long blocking sleep: %s:%s" %
                    (path, value))

    signal_fixture = read(NXANDROID_SIGNAL_TEST)
    for token in ("SYS_pidfd_open", "SYS_pidfd_send_signal",
                  "pidfd_open_exact", "pidfd_send_exact"):
        require(token in signal_fixture,
                "nxandroid signal fixture lost pidfd authority: %s" % token)
    for forbidden in ("kill(", "raise(", "pthread_kill(", "tgkill(",
                      "tkill("):
        require(forbidden not in signal_fixture,
                "nxandroid signal fixture gained raw signal path: %s" %
                forbidden)

    inventory = read(NXANDROID_INVENTORY)
    require("require_sealed_namespace()" in inventory and
            "NXBOOTSTRAP_TEST_HOST_{environment_name}_NS_FD" in inventory and
            "raise SystemExit(77)" in inventory and
            "process.terminate()" in inventory and
            "process.kill()" in inventory and
            "process.wait(timeout=1)" in inventory,
            "inventory child timeout supervision is incomplete")
    for forbidden in ("os.kill(", "os.killpg(", "start_new_session=True",
                      "preexec_fn="):
        require(forbidden not in inventory,
                "inventory supervisor gained non-child signal authority: %s" %
                forbidden)


def check_durable_tools():
    logged = read(RUN_LOGGED)
    capture = read(CAPTURE)
    for token in ("command_argv_sha256", "command_redacted",
                  "is_secret_option", "input-files.sha256",
                  "command-status.txt", "jobs -pr", "received_signal",
                  "MANIFEST.sha256"):
        require(token in logged, "run-logged contract is missing %s" % token)
    require("printenv" not in logged and "env >" not in logged,
            "run-logged dumps the ambient environment")
    require(not re.search(r"^\s*(?:builtin\s+)?kill\s+", logged, re.MULTILINE),
            "run-logged itself sends a signal")
    metadata_publish = "printf 'format=nxframework-run-log-v1\\n'"
    require(all(logged.index(token) < logged.index(metadata_publish)
                for token in ("trap 'record_signal HUP' HUP",
                              "trap 'record_signal INT' INT",
                              "trap 'record_signal TERM' TERM")),
            "run-logged publishes metadata before arming signal observation")
    cutoff = "trap '' HUP INT TERM"
    require(logged.index("tee_status=$candidate_status") <
            logged.index(cutoff) < logged.index("ended_utc=$(date -u"),
            "run-logged does not freeze signal observation before durable output")
    for token in ("source-snapshot.tar.gz", "source-files.sha256",
                  "tracked-worktree.patch", "tracked-index.patch",
                  "MANIFEST.sha256"):
        require(token in capture,
                "capture-checkpoint contract is missing %s" % token)
    require("checkpoint_dir=$checkpoint_root/$checkpoint_id" in capture and
            "mkdir -- \"$checkpoint_dir\"" in capture,
            "checkpoint is not append-only/no-replace")
    require(not re.search(r"\brm\s+-[A-Za-z]*r", capture),
            "checkpoint capture contains recursive deletion")


def main():
    matrix = load_matrix()
    check_matrix(matrix)
    check_safe_runner(matrix)
    check_m12_host_gate(matrix)
    check_m13_host_gate(matrix)
    check_m21_pilot_gates(matrix)
    check_m12a_observability_gate(matrix)
    check_p06_crash_gate(matrix)
    check_source_syntax_and_system_surface()
    check_namespace_guard()
    check_watchdog()
    check_signal_tests()
    check_durable_tools()
    print("test infrastructure gate passed: %d gates, 4 effect classes, sealed namespaces" %
          len(matrix["gates"]))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, KeyError, TypeError, ValueError, OSError) as error:
        print("test infrastructure gate failed: %s" % error, file=sys.stderr)
        sys.exit(1)
