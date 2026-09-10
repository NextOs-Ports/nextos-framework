#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fail-closed validator for the v2 synthetic firmware matrix and receipts."""

import argparse
import hashlib
import json
import os
import re
import struct
import sys
from pathlib import Path, PurePosixPath


REPOSITORY = Path(__file__).resolve().parents[2]
TEST_ROOT = Path(__file__).resolve().parent
PROFILES_PATH = TEST_ROOT / "firmware-profiles-v2.json"
RECEIPT_SCHEMA_PATH = TEST_ROOT / "firmware-matrix-receipt-v2.schema.json"
MATRIX_PATH = TEST_ROOT / "test-matrix-v1.json"
SAFE_RUNNER_PATH = TEST_ROOT / "run-safe-gates.sh"
RUNTIME_RUNNER_PATH = TEST_ROOT / "run-firmware-matrix-v2.sh"
PORTMASTER_CONTRACT_PATH = REPOSITORY / "framework/portmaster/contract-v1.json"
LAUNCHER_TEMPLATE_PATH = (
    REPOSITORY / "framework/nxbootstrap/templates/launcher.sh.in"
)
NXEXTRACT_ROOT = REPOSITORY / "suportando_outros_devices/extrator-universal"
NXEXTRACT_MANIFEST_PATH = NXEXTRACT_ROOT / "ui/release/manifest-v1.json"
NXSPLASH_ROOT = REPOSITORY / "framework/nxsplash"
NXSPLASH_MANIFEST_PATH = NXSPLASH_ROOT / "release/manifest-v1.json"
NXINPUT_CONTRACT_PATH = (
    REPOSITORY / "framework/nxinput/references/m15-input-contract-v1.json"
)
SPRUCE_ENVIRONMENT_CONTRACT_PATH = (
    TEST_ROOT / "device-environments/spruce/contract-v1.json"
)
ROCKNIX_ENVIRONMENT_CONTRACT_PATH = (
    TEST_ROOT / "device-environments/rocknix/contract-v1.json"
)

PROFILE_IDS = (
    "muos",
    "rocknix-panfrost",
    "amberelec",
    "knulli-batocera",
    "arkos",
    "darkosre",
    "nextos-mali450",
    "trimui",
    "spruce",
)
EVIDENCE_LEVELS = {
    "muos": "physical-subsystem",
    "rocknix-panfrost": "physical-runtime-ui-unverified",
    "amberelec": "design-only",
    "knulli-batocera": "release-fix",
    "arkos": "physical-full",
    "darkosre": "field-observation",
    "nextos-mali450": "physical-full",
    "trimui": "design-only",
    "spruce": "field-observation",
}
EXPECTED_ARCHES = {
    "muos": ("aarch64", "armv7"),
    "rocknix-panfrost": ("aarch64", "armv7"),
    "amberelec": ("aarch64", "armv7"),
    "knulli-batocera": ("aarch64", "armv7"),
    "arkos": ("aarch64", "armv7"),
    "darkosre": ("aarch64", "armv7"),
    "nextos-mali450": ("aarch64", "armv7"),
    "trimui": ("aarch64",),
    "spruce": ("aarch64", "armv7"),
}
ARCH_CONTRACT = {
    "aarch64": (False, "libs.aarch64", 2, 183),
    "armv7": (True, "libs.armhf", 1, 40),
}
PROFILE_KEYS = {
    "id", "label", "firmware_names", "evidence", "portmaster",
    "detection", "arch_cases",
    # P10: forma do provedor GLES1 por perfil. Sem isso a matriz nao consegue
    # reprovar um port GLES1 antes do campo. Conteudo validado pelo gate
    # gles1-profile-matrix.
    "graphics_provider",
}
# E5: fixtures de runtime fieis ao device + casos de bug de campo (opt-in).
OPTIONAL_PROFILE_KEYS = {"runtime_fixtures", "field_cases",
    # 22/08/2026 (campo dArkOSRE): o unit do frontend exporta hint de
    # provedor grafico; o fato fica pinado no perfil para o caso de classe
    # "hint desprovado por renderer morto" nunca mais viver so' na memoria.
    "frontend_environment"}
KNOWN_FIELD_CASES = {
    "embedded-openal-shield",
    "armhf-interp-preflight",
    "python-probe",
    "spruce-mixed-abi-runtime",
}
ARCH_KEYS = {
    "architecture", "port_32bit", "library_suffix",
    "nxextract_ui_arch", "nxsplash_arch",
}
CHECK_KEYS = {
    "root_and_control",
    "cfw_detection",
    "path_without_stat",
    "normal_status",
    "early_log_0600",
    "early_log_fields",
    "early_status",
    "pm_finish_once_normal",
    "pm_finish_once_early",
    "architecture_route",
    "ui_route",
    "mapping_and_library_precedence",
    "synthetic_receipt_boundary",
}
SAFE_CFW = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
SAFE_ID = re.compile(r"^[a-z0-9][a-z0-9-]{0,63}$")
PRIVATE_IP = re.compile(
    r"(?:10\.|127\.|169\.254\.|192\.168\.|"
    r"172\.(?:1[6-9]|2[0-9]|3[01])\.)"
)
EXTERNAL_STAT = re.compile(
    r"(?:^|[;&|()\s])(?:command\s+|builtin\s+)?stat\s+-",
    re.MULTILINE,
)


class MatrixError(Exception):
    """Malformed profile, route, artifact or evidence boundary."""


def require(condition, message):
    if not condition:
        raise MatrixError(message)


def read_json(path):
    def no_duplicates(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate JSON key in %s: %s" % (path, key))
            result[key] = value
        return result

    require(path.is_file() and not path.is_symlink(),
            "missing or unsafe JSON: %s" % path)
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream, object_pairs_hook=no_duplicates)


def regular_repository_file(relative):
    require(isinstance(relative, str) and relative,
            "empty repository evidence reference")
    logical = PurePosixPath(relative)
    require(not logical.is_absolute() and ".." not in logical.parts and
            "\\" not in relative,
            "unsafe repository evidence reference: %s" % relative)
    path = REPOSITORY.joinpath(*logical.parts)
    require(path.is_file() and not path.is_symlink(),
            "missing or unsafe evidence reference: %s" % relative)
    require(os.path.commonpath((str(REPOSITORY), str(path.resolve()))) ==
            str(REPOSITORY),
            "evidence reference escaped repository: %s" % relative)


def validate_absolute_path(value, context):
    require(isinstance(value, str) and value.startswith("/") and
            "//" not in value and ".." not in PurePosixPath(value).parts,
            "%s is not a normalized absolute path" % context)
    require("/home/" not in value and "/Users/" not in value,
            "%s contains a personal path" % context)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def elf_identity(path):
    payload = path.read_bytes()[:64]
    require(payload.startswith(b"\x7fELF") and len(payload) >= 20,
            "artifact is not an ELF: %s" % path)
    elf_class = payload[4]
    require(payload[5] in (1, 2), "unsupported ELF endianness: %s" % path)
    endian = "<" if payload[5] == 1 else ">"
    machine = struct.unpack(endian + "H", payload[18:20])[0]
    return elf_class, machine


def validate_artifact(manifest, component_root, architecture,
                      expected_class, expected_machine, component):
    artifact = manifest.get("artifacts", {}).get(architecture)
    require(isinstance(artifact, dict),
            "%s lacks %s artifact" % (component, architecture))
    require(artifact.get("mode") == "0755",
            "%s %s mode is not 0755" % (component, architecture))
    relative = artifact.get("path")
    require(isinstance(relative, str),
            "%s %s has no artifact path" % (component, architecture))
    path = component_root.joinpath(*PurePosixPath(relative).parts)
    require(path.is_file() and not path.is_symlink(),
            "%s %s artifact is missing or unsafe" % (component, architecture))
    require(path.stat().st_size == artifact.get("size") and
            sha256_file(path) == artifact.get("sha256"),
            "%s %s artifact differs from its release manifest" %
            (component, architecture))
    actual_class, actual_machine = elf_identity(path)
    require((actual_class, actual_machine) ==
            (expected_class, expected_machine),
            "%s %s is cross-ABI: class=%s machine=%s" %
            (component, architecture, actual_class, actual_machine))


def validate_shared_contracts(document, matrix, safe_runner):
    contracts = document.get("shared_contracts")
    require(isinstance(contracts, dict) and set(contracts) == {
                "generated_launcher_gate", "input_gate", "graphics_gate",
                "audio_gate", "nxextract_ui_manifest", "nxsplash_manifest",
                "launcher_expectations", "input_expectations",
                # P10: gate que confronta a forma do provedor GLES1 declarada
                # em cada perfil com o que os ports afirmam ter provado.
                "gles1_provider_gate",
            }, "shared contract fields changed")
    gate_ids = {gate.get("id") for gate in matrix.get("gates", [])}
    for field in ("generated_launcher_gate", "input_gate", "graphics_gate",
                  "audio_gate", "gles1_provider_gate"):
        require(contracts[field] in gate_ids,
                "shared gate is absent from test matrix: %s" % contracts[field])
    for gate in (contracts["input_gate"], contracts["graphics_gate"],
                 contracts["audio_gate"]):
        require(safe_runner.count("run_gate %s " % gate) == 1,
                "shared gate does not run exactly once: %s" % gate)

    launcher = contracts.get("launcher_expectations")
    require(launcher == {
                "external_stat": "absent-from-path-and-not-invoked",
                "early_log_mode": "0600",
                "early_log_fields": ["version", "status", "pid", "launcher",
                                     "game_dir", "cfw"],
                "exit_status_preserved": True,
                "pm_finish_count": 1,
                "backend_selected_by_firmware_name": False,
            }, "launcher expectations were weakened")
    input_contract = contracts.get("input_expectations")
    require(input_contract == {
                "mapping_owner": "active-portmaster-control",
                "focus_loss_releases_state": True,
                "hotplug_uses_instance_id_and_rescan": True,
                "disconnect_releases_state": True,
                "profile_simulation_is_physical_input": False,
            }, "input lifecycle expectations were weakened")

    nxinput_text = NXINPUT_CONTRACT_PATH.read_text(encoding="utf-8")
    for token in ("focus loss and disconnect release every logical state",
                  "bounded periodic rescan", "instance ID",
                  "virtual_input_claimed_as_physical"):
        require(token in nxinput_text,
                "approved input contract lacks lifecycle token: %s" % token)


def validate_spruce_environment_contract(contract):
    require(isinstance(contract, dict) and
            contract.get("schema") == "nxframework-device-environment-v1" and
            contract.get("schema_version") == 1 and
            contract.get("id") == "spruce-miyoo-flip-4.3.4",
            "Spruce PC environment contract identity changed")
    encoded = json.dumps(contract, ensure_ascii=False)
    require(PRIVATE_IP.search(encoded) is None and "/home/" not in encoded and
            "/Users/" not in encoded,
            "Spruce environment contract contains private host data")
    firmware = contract.get("firmware")
    require(isinstance(firmware, dict) and
            firmware.get("name") == "spruce" and
            firmware.get("version") == "4.3.4" and
            firmware.get("stored_in_repository") is False and
            firmware.get("redistributed_by_framework") is False and
            isinstance(firmware.get("archive_size"), int) and
            re.fullmatch(r"[0-9a-f]{64}",
                         firmware.get("archive_sha256", "")),
            "Spruce firmware source contract changed")
    topology = contract.get("topology")
    require(isinstance(topology, dict) and
            topology.get("host_architecture") == "aarch64" and
            topology.get("roles") == {
                "nxextract": "aarch64",
                "nxsplash": "armv7",
                "game": "armv7",
            } and
            topology.get("default_armhf_interpreter") == {
                "path": "/lib/ld-linux-armhf.so.3",
                "state": "absent",
            } and
            topology.get("alternate_armhf_interpreter") ==
            "/mnt/SDCARD/spruce/flip/ld-linux-armhf.so.3",
            "Spruce mixed-ABI topology changed")
    require(topology.get("sdl") == {
                "version": "2.0.22",
                "compiled_video_drivers": ["KMSDRM", "dummy"],
                "physical_selected_video_driver": "KMSDRM",
            }, "Spruce target SDL evidence changed")
    armhf_paths = topology.get("armhf_library_path_order")
    require(isinstance(armhf_paths, list) and armhf_paths and
            "/mnt/SDCARD/spruce/flip/muOS/usr/lib" not in armhf_paths,
            "Spruce ARMHF closure admits the AArch64 usr/lib")
    for path in armhf_paths:
        validate_absolute_path(path, "spruce.armhf_library_path_order")
    runtime_files = contract.get("runtime_files")
    require(isinstance(runtime_files, list) and len(runtime_files) >= 8,
            "Spruce runtime file inventory is incomplete")
    for item in runtime_files:
        require(isinstance(item, dict) and
                item.get("root") in ("armhf-chroot", "muos-reduced") and
                (item.get("elf_class"), item.get("elf_machine")) in
                ((1, 40), (2, 183)) and
                isinstance(item.get("size"), int) and item["size"] > 0 and
                re.fullmatch(r"[0-9a-f]{64}", item.get("sha256", "")),
                "Spruce runtime artifact is malformed")


def validate_rocknix_environment_contract(contract):
    require(isinstance(contract, dict) and
            contract.get("schema") == "nxframework-device-environment-v1" and
            contract.get("schema_version") == 1 and
            contract.get("id") == "rocknix-rk3566-specific-20260801",
            "ROCKNIX PC environment contract identity changed")
    encoded = json.dumps(contract, ensure_ascii=False)
    require(PRIVATE_IP.search(encoded) is None and "/home/" not in encoded and
            "/Users/" not in encoded,
            "ROCKNIX environment contract contains private host data")

    firmware = contract.get("firmware")
    require(isinstance(firmware, dict) and
            firmware.get("name") == "ROCKNIX" and
            firmware.get("version") == "20260801" and
            firmware.get("image_variant") == "RK3566-Specific" and
            firmware.get("archive_size") == 1372826408 and
            firmware.get("archive_sha256") ==
            "2a2eb9371c63c4463601b9511172ac5d551f600b18d08c3b4fa9080a584ecdef" and
            firmware.get("uncompressed_size") == 2198863872 and
            firmware.get("stored_in_repository") is False and
            firmware.get("redistributed_by_framework") is False,
            "ROCKNIX firmware source contract changed")

    disk = contract.get("disk")
    require(isinstance(disk, dict) and disk.get("partition_table") == "gpt" and
            disk.get("logical_sector_size") == 512 and
            disk.get("total_logical_sectors") == 4294656 and
            disk.get("crc_state") == "valid" and
            isinstance(disk.get("partitions"), list) and
            len(disk["partitions"]) == 2,
            "ROCKNIX disk contract changed")
    for partition in disk["partitions"]:
        require(partition.get("offset") ==
                partition.get("start_lba") * disk["logical_sector_size"] and
                partition.get("size") ==
                (partition.get("end_lba") - partition.get("start_lba") + 1) *
                disk["logical_sector_size"],
                "ROCKNIX partition arithmetic changed")

    system = contract.get("system_payload")
    require(isinstance(system, dict) and
            system.get("partition_number") == 1 and
            system.get("path") == "SYSTEM" and
            system.get("size") == 1357410304 and
            system.get("sha256") ==
            "bfc4d7fae0f77f81633c988ad2ff00645b4e24a3bc8d7ac3cc9ace24b1541eee" and
            system.get("raw_offset") ==
            system.get("raw_start_sector") * disk["logical_sector_size"] and
            system.get("size") ==
            system.get("raw_sector_count") * disk["logical_sector_size"] and
            system.get("contiguous") is True,
            "ROCKNIX SYSTEM payload contract changed")

    graphics = contract.get("topology", {}).get("graphics")
    require(isinstance(graphics, dict) and
            graphics.get("evidence_scope") ==
            "image-backed-userspace-inventory-only" and
            graphics.get("session", {}).get("physical_selected_backend") ==
            "not-measured" and
            graphics.get("mesa", {}).get("physical_driver_bound") ==
            "not-measured" and
            graphics.get("egl", {}).get("initialized_during_pc_verification")
            is False and
            graphics.get("gles", {}).get("context_created_during_pc_verification")
            is False,
            "ROCKNIX host evidence overclaims physical graphics")
    require(graphics.get("sdl") == {
                "version_from_library": "2.32.10",
                "library": "/usr/lib/libSDL2-2.0.so.0.3200.10",
                "compiled_video_drivers_state":
                "measured-under-qemu-without-video-init",
                "compiled_video_drivers": [
                    "x11", "wayland", "KMSDRM", "offscreen", "dummy",
                    "evdev",
                ],
                "physical_selected_video_driver": "not-measured",
            }, "ROCKNIX target SDL inventory changed")
    policy = graphics.get("framework_policy")
    require(policy == {
                "backend_selected_by_firmware_or_device_name": False,
                "absent_sdl_video_hint": "leave-absent-for-autodetection",
                "supported_inherited_sdl_video_hint": "preserve",
                "unsupported_inherited_sdl_video_hint":
                "remove-without-replacement",
                "egl_gles_provider_override": False,
                "ld_preload_override": False,
            }, "ROCKNIX capability-based graphics policy changed")

    runtime_files = contract.get("runtime_files")
    require(isinstance(runtime_files, list),
            "ROCKNIX runtime file inventory is malformed")
    required_ids = {
        "sway-profile", "sway-service", "os-release", "aarch64-loader",
        "libc", "sdl2", "egl-dispatch",
        "gles2-dispatch", "egl-mesa", "gbm", "drm", "wayland-client",
        "wayland-egl", "panfrost-dri-link", "dril-dri", "gallium",
        "panfrost-kernel-module", "vulkan-panfrost", "sway",
    }
    require({item.get("id") for item in runtime_files} == required_ids and
            len(runtime_files) == len(required_ids),
            "ROCKNIX graphics userspace inventory changed")
    for item in runtime_files:
        require(isinstance(item, dict) and isinstance(item.get("path"), str) and
                not item["path"].startswith("/") and
                ".." not in PurePosixPath(item["path"]).parts,
                "ROCKNIX runtime artifact path is malformed")
        if item.get("kind") == "symlink":
            require(item.get("target") == "libdril_dri.so",
                    "ROCKNIX Panfrost symlink changed")
        else:
            require(isinstance(item.get("size"), int) and item["size"] > 0 and
                    re.fullmatch(r"[0-9a-f]{64}", item.get("sha256", "")),
                    "ROCKNIX runtime artifact identity is malformed")
        if item.get("kind") in ("elf", "kernel-module"):
            require((item.get("elf_class"), item.get("elf_machine")) ==
                    (2, 183), "ROCKNIX runtime artifact is not AArch64")


def validate_profiles(document, portmaster_contract, launcher_template,
                      nxextract_manifest, nxsplash_manifest):
    profiles = document.get("profiles")
    require(isinstance(profiles, list) and
            tuple(item.get("id") for item in profiles) == PROFILE_IDS,
            "firmware profile set or ordering changed")
    known_roots = set(
        portmaster_contract["portmaster_discovery"]["known_roots"]
    )
    case_count = 0
    for profile in profiles:
        profile_id = profile.get("id")
        require(set(profile) - OPTIONAL_PROFILE_KEYS == PROFILE_KEYS and
                SAFE_ID.fullmatch(profile_id),
                "malformed profile: %r" % profile_id)
        require(isinstance(profile.get("label"), str) and
                profile["label"].endswith("synthetic contract fixture"),
                "profile is not labelled synthetic: %s" % profile_id)

        names = profile.get("firmware_names")
        require(isinstance(names, list) and names and
                len(names) == len(set(names)) and
                all(SAFE_CFW.fullmatch(name) for name in names),
                "profile has unsafe firmware names: %s" % profile_id)
        evidence = profile.get("evidence")
        require(isinstance(evidence, dict) and
                set(evidence) == {"level", "scope", "refs"} and
                evidence.get("level") == EVIDENCE_LEVELS[profile_id] and
                isinstance(evidence.get("scope"), str) and
                len(evidence.get("scope", "")) > 8,
                "profile evidence is malformed: %s" % profile_id)
        refs = evidence.get("refs")
        require(isinstance(refs, list) and refs and
                len(refs) == len(set(refs)),
                "profile evidence refs are malformed: %s" % profile_id)
        for reference in refs:
            regular_repository_file(reference)

        portmaster = profile.get("portmaster")
        require(isinstance(portmaster, dict) and set(portmaster) == {
                    "candidate_roots", "primary_root", "control_relative_path",
                    "launcher_root", "data_root",
                }, "profile PortMaster fields changed: %s" % profile_id)
        roots = portmaster.get("candidate_roots")
        require(isinstance(roots, list) and roots and
                len(roots) == len(set(roots)) and roots[0] ==
                portmaster.get("primary_root") and set(roots) <= known_roots,
                "profile PortMaster roots are not contracted: %s" % profile_id)
        for root in roots:
            validate_absolute_path(root, profile_id + ".candidate_roots")
            require(('"%s"' % root) in launcher_template,
                    "generated launcher does not discover %s" % root)
        require(portmaster.get("control_relative_path") == "control.txt",
                "profile control path changed: %s" % profile_id)
        validate_absolute_path(portmaster.get("launcher_root"),
                               profile_id + ".launcher_root")
        validate_absolute_path(portmaster.get("data_root"),
                               profile_id + ".data_root")

        detection = profile.get("detection")
        require(isinstance(detection, dict) and set(detection) == {
                    "method", "accepted_sources", "unsafe_values_rejected",
                } and detection.get("unsafe_values_rejected") is True and
                detection.get("method") in
                ("control-cfw-name", "dual-regular-marker-fallback"),
                "profile CFW detection is unsafe: %s" % profile_id)
        if profile_id == "darkosre":
            require(detection.get("method") == "dual-regular-marker-fallback" and
                    "two-independent-regular-markers" in
                    detection.get("accepted_sources", []),
                    "dArkOSRE lost its two-marker detection boundary")

        for case_id in profile.get("field_cases", []):
            require(case_id in KNOWN_FIELD_CASES,
                    "unknown field case in %s: %s" % (profile_id, case_id))
        fixtures = profile.get("runtime_fixtures", {})
        require(isinstance(fixtures, dict),
                "runtime_fixtures must be an object: %s" % profile_id)
        for name, fixture in fixtures.items():
            require(isinstance(fixture, dict) and SAFE_ID.fullmatch(name) and
                    {"mount", "provenance", "field_bug"} <= set(fixture),
                    "runtime fixture is underspecified: %s.%s" %
                    (profile_id, name))
            validate_absolute_path(fixture.get("mount"),
                                   "%s.%s.mount" % (profile_id, name))
            source = fixture.get("source")
            contract_reference = fixture.get("contract")
            require(not (source is not None and contract_reference is not None),
                    "runtime fixture has two source authorities: %s.%s" %
                    (profile_id, name))
            if source is not None:
                require((REPOSITORY / source).is_file(),
                        "runtime fixture source is missing: %s" % source)
            elif contract_reference is not None:
                require(fixture.get("state") == "external-firmware",
                        "external runtime fixture has wrong state: %s.%s" %
                        (profile_id, name))
                regular_repository_file(contract_reference)
            else:
                require(fixture.get("state") in ("absent", "stubbed-per-case"),
                        "sourceless fixture must declare state=absent: %s.%s" %
                        (profile_id, name))

        cases = profile.get("arch_cases")
        require(isinstance(cases, list) and
                tuple(case.get("architecture") for case in cases) ==
                EXPECTED_ARCHES[profile_id],
                "architecture routes drifted for %s" % profile_id)
        for case in cases:
            require(set(case) == ARCH_KEYS,
                    "architecture case fields changed: %s" % profile_id)
            architecture = case.get("architecture")
            port32, suffix, _, _ = ARCH_CONTRACT[architecture]
            expected_ui_arch = architecture
            expected_splash_arch = architecture
            # Rota mixed-ABI comprovada em campo: o instalador roda na ABI
            # nativa do host enquanto o jogo e o splash seguem ARMHF.
            if profile_id in ("spruce", "darkosre") and architecture == "armv7":
                expected_ui_arch = "aarch64"
            require(case.get("port_32bit") is port32 and
                    case.get("library_suffix") == suffix and
                    case.get("nxextract_ui_arch") == expected_ui_arch and
                    case.get("nxsplash_arch") == expected_splash_arch,
                    "role ABI route changed in profile %s/%s" %
                    (profile_id, architecture))
            ui_class, ui_machine = ARCH_CONTRACT[expected_ui_arch][2:]
            splash_class, splash_machine = ARCH_CONTRACT[
                expected_splash_arch
            ][2:]
            validate_artifact(
                nxextract_manifest, NXEXTRACT_ROOT, expected_ui_arch,
                ui_class, ui_machine, "NXExtract UI"
            )
            validate_artifact(
                nxsplash_manifest, NXSPLASH_ROOT, expected_splash_arch,
                splash_class, splash_machine, "NXSplash"
            )
            case_count += 1
    require(case_count == 17, "firmware architecture case count changed")
    return case_count


def validate_receipt_schema(schema):
    require(schema.get("$id") ==
            "https://nextos.local/schemas/firmware-matrix-receipt-v2.schema.json",
            "wrong firmware receipt schema identity")
    evidence = schema.get("properties", {}).get("evidence", {})
    alternatives = evidence.get("oneOf")
    require(isinstance(alternatives, list) and len(alternatives) == 2,
            "receipt schema does not separate synthetic and physical evidence")
    by_kind = {}
    for alternative in alternatives:
        properties = alternative.get("properties", {})
        kind = properties.get("kind", {}).get("const")
        by_kind[kind] = properties
    require(set(by_kind) == {"synthetic", "physical"} and
            by_kind["synthetic"]["hardware_ran"].get("const") is False and
            by_kind["synthetic"]["device_access"].get("const") is False and
            by_kind["physical"]["hardware_ran"].get("const") is True and
            by_kind["physical"]["device_access"].get("const") is True,
            "receipt schema evidence kinds can be confused")


def validate_runtime_receipt(receipt, document):
    require(set(receipt) == {"schema", "schema_version", "matrix_contract",
                             "evidence", "profiles", "summary"},
            "runtime receipt root fields changed")
    require(receipt.get("schema") ==
            "nxframework-firmware-matrix-receipt-v2" and
            receipt.get("schema_version") == 2 and
            receipt.get("matrix_contract") == document.get("contract_id"),
            "runtime receipt identity changed")
    require(receipt.get("evidence") == {
                "kind": "synthetic",
                "hardware_ran": False,
                "device_access": False,
                "firmware_images_used": False,
                "universal_evidence": False,
            }, "runtime receipt crossed the synthetic boundary")
    expected = []
    by_id = {profile["id"]: profile for profile in document["profiles"]}
    for profile in document["profiles"]:
        for case in profile["arch_cases"]:
            expected.append((profile["id"], case["architecture"]))
    records = receipt.get("profiles")
    require(isinstance(records, list) and
            [(record.get("id"), record.get("architecture"))
             for record in records] == expected,
            "runtime receipt profile/case set changed")
    for record in records:
        require(set(record) == {"id", "architecture", "cfw_name",
                                "portmaster_root", "data_root", "result",
                                "checks"},
                "runtime profile receipt fields changed")
        profile = by_id[record["id"]]
        require(record.get("cfw_name") == profile["firmware_names"][0] and
                record.get("portmaster_root") ==
                profile["portmaster"]["primary_root"] and
                record.get("data_root") == profile["portmaster"]["data_root"] and
                record.get("result") == "synthetic-contract-pass",
                "runtime profile receipt facts drifted: %s" % record["id"])
        checks = record.get("checks")
        require(isinstance(checks, dict) and set(checks) == CHECK_KEYS and
                all(value is True for value in checks.values()),
                "runtime profile receipt is incomplete: %s/%s" %
                (record["id"], record["architecture"]))
    summary = receipt.get("summary")
    require(summary == {
                "profile_count": len(PROFILE_IDS),
                "case_count": len(expected),
                "passed": len(expected),
                "failed": 0,
            }, "runtime receipt summary changed")


def validate_static():
    document = read_json(PROFILES_PATH)
    schema = read_json(RECEIPT_SCHEMA_PATH)
    matrix = read_json(MATRIX_PATH)
    portmaster_contract = read_json(PORTMASTER_CONTRACT_PATH)
    nxextract_manifest = read_json(NXEXTRACT_MANIFEST_PATH)
    nxsplash_manifest = read_json(NXSPLASH_MANIFEST_PATH)
    spruce_environment_contract = read_json(
        SPRUCE_ENVIRONMENT_CONTRACT_PATH
    )
    rocknix_environment_contract = read_json(
        ROCKNIX_ENVIRONMENT_CONTRACT_PATH
    )
    require(set(document) == {"schema_version", "contract_id", "purpose",
                              "evidence_boundary", "approved_sources",
                              "shared_contracts", "profiles"} and
            document.get("schema_version") == 2 and
            document.get("contract_id") ==
            "nxframework-firmware-test-matrix-v2",
            "firmware matrix v2 root changed")
    require(document.get("evidence_boundary") == {
                "result": "synthetic-contract-pass",
                "evidence_kind": "synthetic",
                "universal_evidence": False,
                "hardware_proof": False,
                "hardware_ran": False,
                "device_access": False,
                "firmware_images_used": False,
            }, "firmware matrix evidence boundary was weakened")
    encoded = json.dumps(document, ensure_ascii=False)
    require(PRIVATE_IP.search(encoded) is None and "/home/" not in encoded and
            "/Users/" not in encoded,
            "firmware matrix contains a private address or personal path")

    sources = document.get("approved_sources")
    require(isinstance(sources, list) and len(sources) == 7 and
            len({source.get("id") for source in sources}) == 7,
            "approved source registry changed")
    for source in sources:
        require(set(source) == {"id", "kind", "ref"} and
                source.get("kind") in {
                    "official-contract", "official-and-local-contract",
                    "approved-release-ledger", "sanitized-evidence-catalog",
                    "approved-adapter-contract", "official-firmware-contract",
                }, "unapproved source kind entered the firmware matrix")
        regular_repository_file(source.get("ref"))

    launcher_template = LAUNCHER_TEMPLATE_PATH.read_text(encoding="utf-8")
    require(EXTERNAL_STAT.search(launcher_template) is None,
            "generated launcher template calls external stat")
    for token in ("nxbootstrap_install_exit_trap", "umask 077", "status=$status",
                  "pid=$$", "game_dir=${GAMEDIR:-unresolved}",
                  "cfw=${CFW_NAME:-unknown}", "nxbootstrap_finish"):
        require(token in launcher_template,
                "launcher early-log/finish contract lacks: %s" % token)

    safe_runner = SAFE_RUNNER_PATH.read_text(encoding="utf-8")
    validate_shared_contracts(document, matrix, safe_runner)
    validate_spruce_environment_contract(spruce_environment_contract)
    validate_rocknix_environment_contract(rocknix_environment_contract)
    case_count = validate_profiles(
        document, portmaster_contract, launcher_template,
        nxextract_manifest, nxsplash_manifest,
    )
    validate_receipt_schema(schema)

    runtime_runner = RUNTIME_RUNNER_PATH.read_text(encoding="utf-8")
    for token in ("nxbootstrap_require_private_pid_namespace", "bwrap",
                  "--unshare-net", "no-stat-bin", "launcher-error",
                  "pm-finish", "synthetic-contract-pass", "--receipt"):
        require(token in runtime_runner,
                "dynamic firmware runner lacks: %s" % token)
    for forbidden in ("hardware_ran=1", "device_access=1", "firmware-compatible",
                      "hardware-validated"):
        require(forbidden not in runtime_runner,
                "dynamic firmware runner overclaims: %s" % forbidden)
    return document, case_count


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Validate the v2 firmware matrix or one synthetic receipt"
    )
    parser.add_argument("--receipt", type=Path)
    args = parser.parse_args(argv)
    document, case_count = validate_static()
    if args.receipt is not None:
        receipt = read_json(args.receipt.resolve())
        validate_runtime_receipt(receipt, document)
        print("firmware matrix v2 receipt gate passed: profiles=9 cases=%d "
              "evidence_kind=synthetic hardware_ran=0 device_access=0" %
              case_count)
    else:
        print("firmware matrix v2 static gate passed: profiles=9 cases=%d "
              "evidence_kind=synthetic hardware_ran=0 device_access=0 "
              "firmware_images_used=0" % case_count)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (MatrixError, KeyError, TypeError, ValueError, OSError,
            json.JSONDecodeError) as error:
        print("firmware matrix v2 gate failed: %s" % error, file=sys.stderr)
        raise SystemExit(1)
