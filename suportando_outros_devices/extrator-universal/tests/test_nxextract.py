#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import errno
import hashlib
import importlib.util
import json
import os
import pathlib
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import uuid
import zipfile
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
NXEXTRACT = ROOT / "nxextract.py"
FIXTURES = ROOT / "tests" / "fixtures"


def load_module():
    spec = importlib.util.spec_from_file_location("nxextract_under_test", NXEXTRACT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


NX = load_module()


class SimulatedPowerLoss(BaseException):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fake_elf(machine=183):
    data = bytearray(96)
    data[:4] = b"\x7fELF"
    data[4] = 1 if machine == 40 else 2
    data[5] = 1
    data[6] = 1
    struct.pack_into("<H", data, 16, 3)
    struct.pack_into("<H", data, 18, machine)
    return bytes(data) + b"NX-TEST-LIBRARY"


def plain_manifest(package, split=""):
    split_attribute = ' split="%s"' % split if split else ""
    return (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<manifest package="%s"%s></manifest>' % (package, split_attribute)
    ).encode("utf-8")


def _utf8_string(value):
    encoded = value.encode("utf-8")
    assert len(value) < 128 and len(encoded) < 128
    return bytes((len(value), len(encoded))) + encoded + b"\0"


def binary_manifest(package, split=""):
    strings = ["manifest", "package", "split", package, split]
    payload = b""
    offsets = []
    for value in strings:
        offsets.append(len(payload))
        payload += _utf8_string(value)
    header_size = 28
    strings_start = header_size + len(strings) * 4
    pool_size = strings_start + len(payload)
    pool = struct.pack(
        "<HHIIIIII",
        0x0001,
        header_size,
        pool_size,
        len(strings),
        0,
        0x100,
        strings_start,
        0,
    )
    pool += struct.pack("<%dI" % len(offsets), *offsets) + payload

    attributes = []
    for name_index, value_index in ((1, 3), (2, 4)):
        attributes.append(
            struct.pack(
                "<IIIHBBI",
                0xFFFFFFFF,
                name_index,
                value_index,
                8,
                0,
                0x03,
                value_index,
            )
        )
    node_size = 16 + 20 + sum(len(value) for value in attributes)
    node = struct.pack("<HHIII", 0x0102, 16, node_size, 1, 0xFFFFFFFF)
    node += struct.pack(
        "<IIHHHHHH", 0xFFFFFFFF, 0, 20, 20, len(attributes), 0, 0, 0
    )
    node += b"".join(attributes)
    total = 8 + len(pool) + len(node)
    return struct.pack("<HHI", 0x0003, 8, total) + pool + node


CHANNEL_UI_PRELUDE = """#!/usr/bin/env python3
import os, select, sys, time
def _fd(token):
    assert token.startswith("fd:"), token
    return int(token[3:])
progress_path = sys.argv[1]
stop_fd = _fd(sys.argv[2])
ready_fd = _fd(sys.argv[3])
def publish(proof=b"visible=sdl\\n"):
    os.write(ready_fd, proof)
    os.close(ready_fd)
def wait_stop(seconds=60):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        readable, _, _ = select.select([stop_fd], [], [], 0.02)
        if readable:
            return True
    return False
"""


def write_channel_ui(path, body):
    """UI falsa do contrato P1: recebe fd:N herdados, publica a prova no pipe
    e espera o pedido de parada por poll/select — nenhum pathname de sessao."""
    path.write_text(CHANNEL_UI_PRELUDE + body, encoding="utf-8")
    path.chmod(0o700)


def make_zip(path, entries):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries.items():
            archive.writestr(name, data)


def zip_bytes(entries):
    with tempfile.NamedTemporaryFile(suffix=".zip") as stream:
        make_zip(Path(stream.name), entries)
        stream.seek(0)
        return stream.read()


def base_recipe(lib_data, assets, extra_rules=None, extra_commit=None, hooks=None):
    asset_bytes = sum(len(value) for value in assets.values())
    rules = [
        {
            "id": "native",
            "source": {
                "kind": "entry",
                "patterns": ["lib/{abi}/libgame.so"],
            },
            "destination": "lib/{abi}/libgame.so",
            "validate": {
                "type": "file",
                "size": len(lib_data),
                "sha256": sha256(lib_data),
                "elf_machine": "arm64-v8a",
            },
        },
        {
            "id": "assets",
            "source": {
                "kind": "entries",
                "patterns": ["assets/**"],
                "strip_prefix": "assets/",
            },
            "destination": "assets",
            "validate": {
                "type": "tree",
                "exact_files": len(assets),
                "exact_bytes": asset_bytes,
                "required_paths": sorted(assets),
            },
        },
    ]
    if extra_rules:
        rules.extend(extra_rules)
    commit = ["lib/{abi}/libgame.so", "assets"]
    if extra_commit:
        commit.extend(extra_commit)
    return {
        "schema": 1,
        "id": "synthetic-port",
        "version": "test-1",
        "title": "SYNTHETIC PORT",
        "abi_order": ["arm64-v8a"],
        "input": {
            "search_dirs": ["gamedata", "."],
            "prefer_first_nonempty": True,
            "sniff_all_in_primary": True,
            "max_files": 64,
            "max_bundle_apks": 32,
            "max_member_bytes": 32 * 1024 * 1024,
            "max_bundle_bytes": 64 * 1024 * 1024,
        },
        "extract": rules,
        "validate": [
            {
                "path": "lib/{abi}/libgame.so",
                "type": "file",
                "sha256": sha256(lib_data),
                "elf_machine": "arm64-v8a",
            }
        ],
        "commit": commit,
        "hooks": hooks or [],
        "marker": ".synthetic-data.json",
        "space": {"safety_bytes": 0},
        "log": "test-extract.log",
        "ui_success_seconds": 0,
        "ui_error_seconds": 0,
    }


class NXExtractCase(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="nxextract-test-")
        self.game = Path(self.temporary.name) / "port"
        self.data = self.game / "gamedata"
        self.data.mkdir(parents=True)
        self.recipe_path = self.game / "extractor.json"
        self.lib = fake_elf()
        self.assets = {
            "readme.dat": b"asset-one",
            "levels/level01.bin": b"level-data" * 7,
        }

    def tearDown(self):
        self.temporary.cleanup()

    def write_recipe(self, recipe=None):
        if recipe is None:
            recipe = base_recipe(self.lib, self.assets)
        self.recipe_path.write_text(
            json.dumps(recipe, sort_keys=True, indent=2), encoding="utf-8"
        )
        return recipe

    def run_cli(
        self, command="install", inputs=None, expect=0, extra=None, env=None
    ):
        argv = [
            sys.executable,
            str(NXEXTRACT),
            command,
            "--recipe",
            str(self.recipe_path),
            "--game-dir",
            str(self.game),
        ]
        if command in ("install", "plan"):
            argv.append("--quiet")
        if command == "install":
            argv += ["--ui", "none"]
        for value in inputs or []:
            argv += ["--input", str(value)]
        argv += extra or []
        process_env = os.environ.copy()
        process_env.update(env or {})
        result = subprocess.run(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=30,
            env=process_env,
        )
        if result.returncode != expect:
            self.fail(
                "command returned %d, expected %d\nstdout:\n%s\nstderr:\n%s"
                % (result.returncode, expect, result.stdout, result.stderr)
            )
        return result

    def assert_payload(self):
        self.assertEqual(
            (self.game / "lib/arm64-v8a/libgame.so").read_bytes(), self.lib
        )
        for relative, data in self.assets.items():
            self.assertEqual((self.game / "assets" / relative).read_bytes(), data)

    def merged_entries(self, manifest=None):
        entries = {
            "AndroidManifest.xml": manifest or plain_manifest("org.nextos.synthetic"),
            "lib/arm64-v8a/libgame.so": self.lib,
        }
        entries.update({"assets/" + key: value for key, value in self.assets.items()})
        return entries

    def test_manifest_parser_handles_binary_axml(self):
        package, split = NX.parse_android_manifest(
            binary_manifest("org.nextos.binary", "config.arm64_v8a")
        )
        self.assertEqual(package, "org.nextos.binary")
        self.assertEqual(split, "config.arm64_v8a")

    def test_renamed_merged_apk_is_selected_by_content(self):
        self.write_recipe()
        source = self.data / ("renamed-" + uuid.uuid4().hex)
        make_zip(source, self.merged_entries(binary_manifest("org.nextos.synthetic")))
        self.run_cli()
        self.assert_payload()
        self.assertTrue(source.exists(), "the legal source must be preserved")
        self.run_cli(command="verify")

    def test_templated_elf_machine_selects_and_validates_armv7(self):
        self.lib = fake_elf(machine=40)
        recipe = base_recipe(self.lib, self.assets)
        recipe["abi_order"] = ["arm64-v8a", "armeabi-v7a"]
        recipe["extract"][0]["validate"]["elf_machine"] = "{abi}"
        recipe["validate"][0]["elf_machine"] = "{abi}"
        self.write_recipe(recipe)
        entries = {
            "AndroidManifest.xml": plain_manifest("org.nextos.synthetic"),
            "lib/armeabi-v7a/libgame.so": self.lib,
        }
        entries.update({"assets/" + key: value for key, value in self.assets.items()})
        source = self.data / ("abi-neutral-" + uuid.uuid4().hex + ".apk")
        make_zip(source, entries)

        self.run_cli()
        self.assertEqual(
            (self.game / "lib/armeabi-v7a/libgame.so").read_bytes(), self.lib
        )
        self.run_cli(command="verify")

    def test_loose_splits_are_grouped_by_manifest_package(self):
        self.write_recipe()
        base = self.data / ("one-" + uuid.uuid4().hex + ".apk")
        abi = self.data / ("two-" + uuid.uuid4().hex + ".apk")
        asset_entries = {
            "AndroidManifest.xml": binary_manifest("org.nextos.synthetic"),
        }
        asset_entries.update(
            {"assets/" + key: value for key, value in self.assets.items()}
        )
        make_zip(base, asset_entries)
        make_zip(
            abi,
            {
                "AndroidManifest.xml": binary_manifest(
                    "org.nextos.synthetic", "config.arm64_v8a"
                ),
                "lib/arm64-v8a/libgame.so": self.lib,
            },
        )
        self.run_cli()
        self.assert_payload()
        self.assertTrue(base.exists())
        self.assertTrue(abi.exists())

    def _bundle_case(self, extension):
        self.write_recipe()
        base_bytes = zip_bytes(
            {
                "AndroidManifest.xml": plain_manifest("org.nextos.synthetic"),
                **{"assets/" + key: value for key, value in self.assets.items()},
            }
        )
        split_bytes = zip_bytes(
            {
                "AndroidManifest.xml": plain_manifest(
                    "org.nextos.synthetic", "config.arm64_v8a"
                ),
                "lib/arm64-v8a/libgame.so": self.lib,
            }
        )
        bundle = self.data / ("completely-random-" + uuid.uuid4().hex + extension)
        make_zip(
            bundle,
            {
                "unknown/base-random.apk": base_bytes,
                "splits/no-fixed-name.apk": split_bytes,
                "metadata/info.json": b"{}",
            },
        )
        self.run_cli()
        self.assert_payload()
        self.assertTrue(bundle.exists())

    def test_apkm_bundle(self):
        self._bundle_case(".apkm")

    def test_apks_bundle(self):
        self._bundle_case(".apks")

    def _compatibility_role_case(self, extension):
        """Real install over one APK or one APK bundle with every role active.

        The synthetic shape pins both field regressions: Off The Road requires
        data_001.xpk while AVConfig.json is legitimately absent; Terraria's
        documented .4 container carries a .49 internal payload which selects a
        patch profile by the internal bytes, never by the container identity.
        """
        self.assets = {
            "data_001.xpk": b"OTR-core-payload",
            "bin/Data/data.unity3d": b"UnityFS\0internal 1.4.5.6.49",
        }
        selector = self.assets["bin/Data/data.unity3d"]
        capture = self.game / "capture-compatibility.py"
        capture.write_text(
            "import json, os, pathlib\n"
            "value=json.loads(os.environ['NXEXTRACT_COMPATIBILITY_JSON'])\n"
            "path=pathlib.Path(os.environ['NXEXTRACT_STAGE'])/'compatibility.json'\n"
            "path.write_text(json.dumps(value,sort_keys=True),encoding='utf-8')\n",
            encoding="utf-8",
        )
        hook = {
            "id": "capture-compatibility",
            "argv": [sys.executable, "{game_dir}/capture-compatibility.py"],
            "checkpoint": [
                {"path": "compatibility.json", "type": "file", "min_size": 32}
            ],
            "contract": {
                "schema": "org.nextos.apk-compat.hook-contract",
                "schema_version": 1,
                "hook_id": "capture-compatibility",
                "inputs": ["assets/bin/Data/data.unity3d"],
                "predicates": [
                    {
                        "class": "patch_selection",
                        "checks": "engine-authenticated internal payload profile",
                    }
                ],
                "fallback": "generic-symbolic-path",
                "error_codes": ["NXH0001"],
            },
        }
        recipe = base_recipe(
            self.lib,
            self.assets,
            hooks=[hook],
            extra_commit=["compatibility.json"],
        )
        recipe["input"]["packages"] = ["org.nextos.synthetic"]
        recipe["reference_build"] = {
            "game_version": "1.4.5.6.4",
            "container_size": 123456,
            "container_sha256": sha256(b"documentation-only-reference"),
        }
        recipe["compatibility"] = {
            "package_families": ["org.nextos.synthetic"],
            "abis": ["arm64-v8a"],
            "required_members": [
                "assets/data_001.xpk",
                {"member": "assets/AVConfig.json", "role": "optional"},
                {
                    "member": "lib/arm64-v8a/libgame.so",
                    "role": "variant_required",
                    "variant": "arm64-v8a",
                },
                {
                    "member": "assets/bin/Data/data.unity3d",
                    "role": "patch_selector",
                },
            ],
        }
        recipe["patch_profiles"] = [
            {
                "id": "terraria-internal-49",
                "match_internal_payload": {
                    "path": "assets/bin/Data/data.unity3d",
                    "sha256": sha256(selector),
                    "magic_ascii": "UnityFS",
                },
                "fallback": "generic-symbolic-path",
            }
        ]
        self.write_recipe(recipe)

        if extension == ".apk":
            source = self.data / ("renamed-owner-copy-" + uuid.uuid4().hex)
            make_zip(source, self.merged_entries())
        else:
            base_bytes = zip_bytes(
                {
                    "AndroidManifest.xml": plain_manifest("org.nextos.synthetic"),
                    **{
                        "assets/" + key: value
                        for key, value in self.assets.items()
                    },
                }
            )
            split_bytes = zip_bytes(
                {
                    "AndroidManifest.xml": plain_manifest(
                        "org.nextos.synthetic", "config.arm64_v8a"
                    ),
                    "lib/arm64-v8a/libgame.so": self.lib,
                }
            )
            source = self.data / ("renamed-owner-copy-" + uuid.uuid4().hex + extension)
            make_zip(
                source,
                {
                    "random/base-name.apk": base_bytes,
                    "random/config-name.apk": split_bytes,
                    "META-INF/not-an-identity.txt": b"repackaged",
                },
            )
        self.run_cli(inputs=[source])

        result = json.loads(
            (self.game / "compatibility.json").read_text(encoding="utf-8")
        )
        self.assertEqual(result["schema"], NX.COMPATIBILITY_RESULT_SCHEMA)
        by_member = {item["member"]: item for item in result["members"]}
        self.assertTrue(by_member["assets/data_001.xpk"]["present"])
        self.assertFalse(by_member["assets/AVConfig.json"]["present"])
        self.assertFalse(by_member["assets/AVConfig.json"]["required"])
        self.assertTrue(by_member["lib/arm64-v8a/libgame.so"]["required"])
        selection = result["patch_selections"][0]
        self.assertEqual(selection["profile"], "terraria-internal-49")
        self.assertEqual(selection["state"], "matched")
        self.assertEqual(selection["payload_sha256"], sha256(selector))

        marker = json.loads(
            (self.game / ".synthetic-data.json").read_text(encoding="utf-8")
        )
        self.assertEqual(marker["compatibility"], result)
        self.assertEqual(
            marker["compatibility_fingerprint"],
            sha256(NX.canonical_json(result)),
        )
        self.assertNotIn(source.name, json.dumps(marker, sort_keys=True))
        self.assertTrue(NX.marker_matches_recipe(marker, NX.Recipe(self.recipe_path)))

    def test_compatibility_roles_runtime_apk(self):
        self._compatibility_role_case(".apk")

    def test_compatibility_roles_runtime_apkm(self):
        self._compatibility_role_case(".apkm")

    def test_compatibility_roles_runtime_apks(self):
        self._compatibility_role_case(".apks")

    def test_compatibility_roles_runtime_xapk(self):
        self._compatibility_role_case(".xapk")

    def test_offtheroad_arm64_2_roles_fixture_runtime(self):
        """The named regression uses only deterministic synthetic payloads."""
        recipe = json.loads(
            (FIXTURES / "offtheroad-arm64-2-roles.json").read_text(
                encoding="utf-8"
            )
        )
        self.write_recipe(recipe)
        package = "org.nextos.fixture.offtheroad"
        libgame = fake_elf()
        libcxx = fake_elf() + b"-CXX"
        data_001 = b"OTRDATA-synthetic-core"

        def entries(
            manifest_package=package,
            abi="arm64-v8a",
            game=libgame,
            cxx=libcxx,
            include_data=True,
        ):
            result = {
                "AndroidManifest.xml": plain_manifest(manifest_package),
                "lib/%s/libgame.so" % abi: game,
                "lib/%s/libc++_shared.so" % abi: cxx,
            }
            if include_data:
                result["assets/data_001.xpk"] = data_001
            return result

        # No AVConfig.json is present. A renamed legal container and a second
        # repackaged container with different ZIP metadata must both install.
        first = self.data / ("owner-container-" + uuid.uuid4().hex)
        repacked = self.data / ("repacked-container-" + uuid.uuid4().hex)
        make_zip(first, entries())
        make_zip(
            repacked,
            {
                **entries(),
                "META-INF/repack-note.txt": b"synthetic packaging variation",
            },
        )
        self.assertNotEqual(sha256(first.read_bytes()), sha256(repacked.read_bytes()))
        self.run_cli(inputs=[first])
        self.run_cli(inputs=[repacked], extra=["--force-source"])

        self.assertEqual(
            (self.game / "lib/arm64-v8a/libgame.so").read_bytes(), libgame
        )
        self.assertEqual(
            (self.game / "lib/arm64-v8a/libc++_shared.so").read_bytes(), libcxx
        )
        self.assertEqual(
            (self.game / "assets/data_001.xpk").read_bytes(), data_001
        )
        self.assertFalse((self.game / "assets/AVConfig.json").exists())
        marker_path = self.game / ".offtheroad-roles-fixture.json"
        marker = json.loads(marker_path.read_text(encoding="utf-8"))
        by_member = {
            item["member"]: item for item in marker["compatibility"]["members"]
        }
        self.assertTrue(by_member["lib/arm64-v8a/libgame.so"]["required"])
        self.assertTrue(by_member["lib/arm64-v8a/libc++_shared.so"]["required"])
        self.assertTrue(by_member["assets/data_001.xpk"]["required"])
        self.assertFalse(by_member["assets/AVConfig.json"]["required"])
        self.assertFalse(by_member["assets/AVConfig.json"]["present"])
        installed_fingerprint = sha256(marker_path.read_bytes())

        negative_cases = {
            "wrong-package": entries(manifest_package="org.nextos.fixture.other"),
            "wrong-abi": entries(abi="armeabi-v7a"),
            "wrong-elf": entries(game=fake_elf(machine=40)),
            "missing-data-001": entries(include_data=False),
            "missing-libcxx": {
                key: value
                for key, value in entries().items()
                if not key.endswith("/libc++_shared.so")
            },
        }
        for label, payload in negative_cases.items():
            with self.subTest(label=label):
                candidate = self.data / ("negative-" + label + "-" + uuid.uuid4().hex)
                make_zip(candidate, payload)
                self.run_cli(
                    inputs=[candidate], expect=1, extra=["--force-source"]
                )
                self.assertEqual(
                    sha256(marker_path.read_bytes()), installed_fingerprint,
                    "a rejected source must not replace the installed receipt",
                )

    def test_compatibility_core_required_runtime_missing(self):
        recipe = base_recipe(self.lib, self.assets)
        recipe["input"]["packages"] = ["org.nextos.synthetic"]
        recipe["compatibility"] = {
            "package_families": ["org.nextos.synthetic"],
            "abis": ["arm64-v8a"],
            "required_members": ["assets/data_001.xpk"],
        }
        self.write_recipe(recipe)
        source = self.data / "structurally-incomplete.apk"
        make_zip(source, self.merged_entries())
        self.run_cli(expect=1, inputs=[source])
        detail = (self.game / "nxextract-detail.log").read_text(encoding="utf-8")
        self.assertIn("required compatibility member assets/data_001.xpk", detail)

    def test_compatibility_optional_member_runtime_present_and_absent(self):
        recipe_data = base_recipe(self.lib, self.assets)
        recipe_data["compatibility"] = {
            "required_members": [
                {"member": "assets/AVConfig.json", "role": "optional"}
            ]
        }
        self.write_recipe(recipe_data)
        recipe = NX.Recipe(self.recipe_path)
        outcomes = []
        opened = []
        try:
            for label, optional in (("absent", None), ("present", b"{}")):
                entries = self.merged_entries()
                if optional is not None:
                    entries["assets/AVConfig.json"] = optional
                path = self.data / (label + ".apk")
                make_zip(path, entries)
                archive = NX.Archive(path, "apk", label=label)
                opened.append(archive)
                group = NX.CandidateGroup(
                    label, [archive], [], "org.nextos.synthetic", "apk-set"
                )
                result = NX._resolve_compatibility_members(
                    recipe, group, "arm64-v8a"
                )
                outcomes.append(result["members"][0]["present"])
        finally:
            for archive in opened:
                archive.close()
        self.assertEqual(outcomes, [False, True])

    def test_compatibility_variant_required_uses_resolved_abi(self):
        recipe_data = base_recipe(self.lib, self.assets)
        recipe_data["abi_order"] = ["arm64-v8a", "armeabi-v7a"]
        recipe_data["compatibility"] = {
            "required_members": [
                {
                    "member": "assets/armv7-only.bin",
                    "role": "variant_required",
                    "variant": "armeabi-v7a",
                }
            ]
        }
        self.write_recipe(recipe_data)
        recipe = NX.Recipe(self.recipe_path)
        source = self.data / "arm64-only.apk"
        make_zip(source, self.merged_entries())
        archive = NX.Archive(source, "apk", label="arm64-only")
        group = NX.CandidateGroup(
            "arm64-only", [archive], [], "org.nextos.synthetic", "apk-set"
        )
        try:
            inactive = NX._resolve_compatibility_members(
                recipe, group, "arm64-v8a"
            )["members"][0]
            self.assertFalse(inactive["required"])
            with self.assertRaisesRegex(
                NX.PlanError, "required compatibility member assets/armv7-only.bin"
            ):
                NX._resolve_compatibility_members(
                    recipe, group, "armeabi-v7a"
                )
        finally:
            archive.close()

    def test_patch_selection_fallback_fingerprint_env_and_checkpoint(self):
        known = b"selector-known"
        unknown = b"selector-other"
        script = self.game / "capture-selection.py"
        counter = self.game / "hook-runs.txt"
        script.write_text(
            "import json, os, pathlib\n"
            "game=pathlib.Path(os.environ['NXEXTRACT_GAME_DIR'])\n"
            "count=game/'hook-runs.txt'\n"
            "value=int(count.read_text())+1 if count.exists() else 1\n"
            "count.write_text(str(value),encoding='utf-8')\n"
            "receipt=pathlib.Path(os.environ['NXEXTRACT_STAGE'])/'selection.json'\n"
            "receipt.write_text(os.environ['NXEXTRACT_COMPATIBILITY_JSON'],encoding='utf-8')\n",
            encoding="utf-8",
        )
        recipe_data = {
            "schema": 1,
            "id": "selector-test",
            "version": "1",
            "abi_order": ["arm64-v8a"],
            "input": {"packages": ["org.nextos.synthetic"]},
            "extract": [
                {
                    "id": "native",
                    "source": {
                        "kind": "entry",
                        "patterns": ["lib/{abi}/libgame.so"],
                    },
                    "destination": "lib/{abi}/libgame.so",
                    "validate": {
                        "sha256": sha256(self.lib),
                        "elf_machine": "arm64-v8a",
                    },
                }
            ],
            "compatibility": {
                "package_families": ["org.nextos.synthetic"],
                "abis": ["arm64-v8a"],
                "required_members": [
                    {"member": "assets/selector.bin", "role": "patch_selector"}
                ],
            },
            "patch_profiles": [
                {
                    "id": "known-profile",
                    "match_internal_payload": {
                        "path": "assets/selector.bin",
                        "sha256": sha256(known),
                    },
                    "fallback": "generic-symbolic-path",
                }
            ],
            "hooks": [
                {
                    "id": "capture",
                    "argv": [sys.executable, "{game_dir}/capture-selection.py"],
                    "checkpoint": [
                        {"path": "selection.json", "type": "file", "min_size": 32}
                    ],
                }
            ],
            "validate": [],
            "commit": ["lib/{abi}/libgame.so"],
        }
        self.write_recipe(recipe_data)
        recipe = NX.Recipe(self.recipe_path)
        plans = []
        archives = []
        for label, selector in (
            ("known", known), ("unknown", unknown), ("absent", None)
        ):
            path = self.data / (label + ".apk")
            entries = {
                "AndroidManifest.xml": plain_manifest("org.nextos.synthetic"),
                "lib/arm64-v8a/libgame.so": self.lib,
            }
            if selector is not None:
                entries["assets/selector.bin"] = selector
            make_zip(
                path,
                entries,
            )
            archive = NX.Archive(path, "apk", label=label)
            archives.append(archive)
            plans.append(
                NX.build_plan_for(
                    recipe,
                    NX.CandidateGroup(
                        label, [archive], [], "org.nextos.synthetic", "apk-set"
                    ),
                    "arm64-v8a",
                )
            )
        try:
            self.assertNotEqual(plans[0].fingerprint, plans[1].fingerprint)
            self.assertEqual(
                plans[0].compatibility_result["patch_selections"][0]["profile"],
                "known-profile",
            )
            fallback = plans[1].compatibility_result["patch_selections"][0]
            self.assertEqual(fallback["state"], "unknown")
            self.assertIsNone(fallback["profile"])
            self.assertEqual(fallback["fallback"], "generic-symbolic-path")
            absent = plans[2].compatibility_result["patch_selections"][0]
            self.assertEqual(absent["state"], "absent")
            self.assertIsNone(absent["profile"])
            self.assertNotIn("payload_sha256", absent)

            workspace = self.game / ".selector-workspace"
            stage = workspace / "stage"
            stage.mkdir(parents=True)
            logger = NX.Logger(None, verbose=False)
            NX.run_hooks(
                recipe, plans[0], str(self.game), str(stage), str(workspace),
                NX.Progress(None), logger,
            )
            NX.run_hooks(
                recipe, plans[1], str(self.game), str(stage), str(workspace),
                NX.Progress(None), logger,
            )
            self.assertEqual(counter.read_text(encoding="utf-8"), "2")
            captured = json.loads(
                (stage / "selection.json").read_text(encoding="utf-8")
            )
            self.assertEqual(captured, plans[1].compatibility_result)
            hook_marker = json.loads(
                (workspace / "hooks/capture.json").read_text(encoding="utf-8")
            )
            self.assertEqual(
                hook_marker["plan_fingerprint"], plans[1].fingerprint
            )

            ambiguous_data = json.loads(json.dumps(recipe_data))
            ambiguous_data["patch_profiles"].append(
                {
                    "id": "also-known",
                    "match_internal_payload": {
                        "path": "assets/selector.bin",
                        "sha256": sha256(known),
                        "magic_ascii": "selector",
                    },
                    "fallback": "generic-symbolic-path",
                }
            )
            ambiguous_path = self.game / "ambiguous-selector.json"
            ambiguous_path.write_text(
                json.dumps(ambiguous_data, sort_keys=True), encoding="utf-8"
            )
            ambiguous_recipe = NX.Recipe(ambiguous_path)
            with self.assertRaisesRegex(
                NX.PlanError, "matched multiple authenticated profiles"
            ):
                NX.build_plan_for(
                    ambiguous_recipe,
                    NX.CandidateGroup(
                        "known", [archives[0]], [],
                        "org.nextos.synthetic", "apk-set",
                    ),
                    "arm64-v8a",
                )
        finally:
            for archive in archives:
                archive.close()

    def test_bundle_space_preflight_counts_only_valid_cached_apks(self):
        self.write_recipe()
        base_bytes = zip_bytes(
            {
                "AndroidManifest.xml": plain_manifest("org.nextos.synthetic"),
                **{"assets/" + key: value for key, value in self.assets.items()},
            }
        )
        split_bytes = zip_bytes(
            {
                "AndroidManifest.xml": plain_manifest(
                    "org.nextos.synthetic", "config.arm64_v8a"
                ),
                "lib/arm64-v8a/libgame.so": self.lib,
            }
        )
        bundle = self.data / "cache-space.apkm"
        names = ["base.apk", "split.apk"]
        make_zip(bundle, {names[0]: base_bytes, names[1]: split_bytes})

        recipe = NX.Recipe(str(self.recipe_path))
        workspace = Path(NX.prepare_workspace(str(self.game), recipe.identifier))
        cache = (
            workspace
            / "source-cache"
            / ("bundle-" + NX._bundle_cache_token(str(bundle)))
        )
        cache.mkdir(parents=True)
        with zipfile.ZipFile(bundle, "r") as archive:
            members = [archive.getinfo(name) for name in names]
        destinations = []
        for index, info in enumerate(members):
            token = NX.sha256_bytes(info.filename.encode("utf-8"))[:12]
            destinations.append(cache / ("%03d-%s.apk" % (index, token)))
        destinations[0].write_bytes(base_bytes)
        destinations[1].write_bytes(b"X" * len(split_bytes))
        (cache / "stale-extra.apk").write_bytes(b"S" * (1024 * 1024))

        discovery = NX.discover_inputs(
            recipe,
            str(self.game),
            [str(bundle)],
            NX.Logger(None, verbose=False),
        )
        checks = []

        def capture_space(_path, required, label):
            checks.append((required, label))

        archives = []
        with mock.patch.object(NX, "_check_free_space", side_effect=capture_space):
            groups, archives = NX.build_candidate_groups(
                recipe,
                discovery,
                str(workspace),
                NX.Logger(None, verbose=False),
                NX.Progress(None),
            )
        try:
            self.assertTrue(groups)
            self.assertEqual(checks, [(len(split_bytes), "APK bundle expansion")])
        finally:
            for archive in archives:
                archive.close()

    def test_xapk_bundle_with_direct_obb(self):
        obb = b"LPK\0" + b"xapk-obb-payload" * 11
        obb_rule = {
            "id": "obb",
            "source": {
                "kind": "entry",
                "patterns": ["*.obb", "**/*.obb"],
                "scopes": ["bundle"],
            },
            "destination": "data/game.obb",
            "validate": {
                "type": "file",
                "size": len(obb),
                "sha256": sha256(obb),
                "magic_ascii": "LPK\u0000",
            },
        }
        recipe = base_recipe(
            self.lib,
            self.assets,
            extra_rules=[obb_rule],
            extra_commit=["data/game.obb"],
        )
        self.write_recipe(recipe)
        merged = zip_bytes(self.merged_entries())
        bundle = self.data / ("export-" + uuid.uuid4().hex + ".xapk")
        make_zip(
            bundle,
            {
                "install/" + uuid.uuid4().hex + ".apk": merged,
                "Android/obb/org.nextos.synthetic/main.payload.obb": obb,
                "manifest.json": b"{}",
            },
        )
        self.run_cli()
        self.assert_payload()
        self.assertEqual((self.game / "data/game.obb").read_bytes(), obb)

    def container_rule(self, destination="data/game.apk", **source):
        rule = {
            "id": "container",
            "source": dict({"kind": "container"}, **source),
            "destination": destination,
            "validate": {
                "type": "file",
                "min_size": 64,
                "max_size": 32 * 1024 * 1024,
                "magic_ascii": "PK\u0003\u0004",
            },
        }
        return rule

    def test_container_copies_the_loose_apk_itself(self):
        """O payload de alguns jogos e' o APK inteiro, nao um subconjunto dele."""
        recipe = base_recipe(
            self.lib,
            self.assets,
            extra_rules=[self.container_rule()],
            extra_commit=["data/game.apk"],
        )
        recipe["input"]["packages"] = ["org.nextos.synthetic"]
        self.write_recipe(recipe)
        source = self.data / ("app-" + uuid.uuid4().hex + ".apk")
        make_zip(source, self.merged_entries())
        self.run_cli()
        self.assert_payload()
        self.assertEqual(
            (self.game / "data/game.apk").read_bytes(), source.read_bytes()
        )

    def test_container_picks_the_base_apk_inside_a_bundle(self):
        """Num XAPK, o APK com os assets e' o BASE; o split nao entra por engano."""
        recipe = base_recipe(
            self.lib,
            self.assets,
            extra_rules=[self.container_rule()],
            extra_commit=["data/game.apk"],
        )
        recipe["input"]["packages"] = ["org.nextos.synthetic"]
        self.write_recipe(recipe)
        base = zip_bytes(
            {
                "AndroidManifest.xml": plain_manifest("org.nextos.synthetic"),
                **{"assets/" + key: value for key, value in self.assets.items()},
            }
        )
        split = zip_bytes(
            {
                "AndroidManifest.xml": plain_manifest(
                    "org.nextos.synthetic", "config.arm64_v8a"
                ),
                "lib/arm64-v8a/libgame.so": self.lib,
            }
        )
        bundle = self.data / ("export-" + uuid.uuid4().hex + ".xapk")
        make_zip(
            bundle,
            {
                "org.nextos.synthetic.apk": base,
                "config.arm64_v8a.apk": split,
                "manifest.json": b"{}",
            },
        )
        self.run_cli()
        self.assert_payload()
        self.assertEqual((self.game / "data/game.apk").read_bytes(), base)

    def test_container_can_ask_for_a_named_split(self):
        recipe = base_recipe(
            self.lib,
            self.assets,
            extra_rules=[self.container_rule(split="config.arm64_v8a")],
            extra_commit=["data/game.apk"],
        )
        recipe["input"]["packages"] = ["org.nextos.synthetic"]
        self.write_recipe(recipe)
        base = zip_bytes(
            {
                "AndroidManifest.xml": plain_manifest("org.nextos.synthetic"),
                **{"assets/" + key: value for key, value in self.assets.items()},
            }
        )
        split = zip_bytes(
            {
                "AndroidManifest.xml": plain_manifest(
                    "org.nextos.synthetic", "config.arm64_v8a"
                ),
                "lib/arm64-v8a/libgame.so": self.lib,
            }
        )
        bundle = self.data / ("export-" + uuid.uuid4().hex + ".xapk")
        make_zip(
            bundle,
            {
                "org.nextos.synthetic.apk": base,
                "config.arm64_v8a.apk": split,
                "manifest.json": b"{}",
            },
        )
        self.run_cli()
        self.assertEqual((self.game / "data/game.apk").read_bytes(), split)

    def test_container_recipe_rejects_whole_apk_identity_lock(self):
        rule = self.container_rule()
        rule["validate"]["sha256"] = sha256(b"one external apk")
        recipe = base_recipe(
            self.lib,
            self.assets,
            extra_rules=[rule],
            extra_commit=["data/game.apk"],
        )
        recipe["input"]["packages"] = ["org.nextos.synthetic"]
        self.write_recipe(recipe)

        result = self.run_cli(expect=1)

        self.assertIn("NXA0001", result.stderr)
        self.assertIn("identity, not compatibility", result.stderr)
        self.assertFalse((self.game / "test-extract.log").exists())
        self.assertFalse((self.game / ".nxextract").exists())

        # V3: a whitelist of ANY length is still container identity. Two
        # distinct digests used to be accepted (the 1.2.18 loophole); now
        # every quantity is refused.
        for label, digests in (
            ("duplicate-pair", [sha256(b"official"), sha256(b"official")]),
            ("two-distinct", [sha256(b"official-one"), sha256(b"official-two")]),
            (
                "three-distinct",
                [sha256(b"a"), sha256(b"b"), sha256(b"c")],
            ),
        ):
            listed_rule = self.container_rule()
            listed_rule["validate"]["sha256"] = digests
            listed_recipe = base_recipe(
                self.lib,
                self.assets,
                extra_rules=[listed_rule],
                extra_commit=["data/game.apk"],
            )
            listed_recipe["input"]["packages"] = ["org.nextos.synthetic"]
            listed_path = self.game / ("container-hash-" + label + ".json")
            listed_path.write_text(
                json.dumps(listed_recipe, sort_keys=True, indent=2),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(NX.RecipeError, "NXA0001"):
                NX.Recipe(str(listed_path))

        missing_package_recipe = base_recipe(
            self.lib,
            self.assets,
            extra_rules=[self.container_rule()],
            extra_commit=["data/game.apk"],
        )
        missing_package_path = self.game / "container-without-package.json"
        missing_package_path.write_text(
            json.dumps(missing_package_recipe, sort_keys=True, indent=2),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(NX.RecipeError, "requires input.packages"):
            NX.Recipe(str(missing_package_path))

        for label, locked_validator, expected in (
            ("crc-single", {"crc32": "deadbeef"}, "NXA0002"),
            ("crc-list", {"crc32": ["deadbeef", "cafebabe"]}, "NXA0002"),
            ("size", {"size": 123456}, "NXA0003"),
        ):
            locked_rule = self.container_rule()
            locked_rule["validate"].update(locked_validator)
            locked_recipe = base_recipe(
                self.lib,
                self.assets,
                extra_rules=[locked_rule],
                extra_commit=["data/game.apk"],
            )
            locked_recipe["input"]["packages"] = ["org.nextos.synthetic"]
            locked_path = self.game / ("locked-container-" + label + ".json")
            locked_path.write_text(
                json.dumps(locked_recipe, sort_keys=True, indent=2),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(NX.RecipeError, expected):
                NX.Recipe(str(locked_path))

        for label, mutate, expected in (
            (
                "source-validate-sha",
                lambda rule: rule.update(
                    {"source_validate": {"sha256": sha256(b"owner-copy")}}
                ),
                "NXA0001",
            ),
            (
                "output-validate-sha",
                lambda rule: rule.update(
                    {"output_validate": {"sha256": sha256(b"owner-copy")}}
                ),
                "NXA0001",
            ),
            (
                "literal-filename-pattern",
                lambda rule: rule["source"].update(
                    {"patterns": ["OriginalOwnerCopy.apk"]}
                ),
                "NXA0005",
            ),
            (
                "basename-destination",
                lambda rule: rule.update(
                    {"destination": "data/{basename}"}
                ),
                "NXA0005",
            ),
        ):
            bypass_rule = self.container_rule()
            mutate(bypass_rule)
            bypass_recipe = base_recipe(
                self.lib,
                self.assets,
                extra_rules=[bypass_rule],
                extra_commit=["data/game.apk"],
            )
            bypass_recipe["input"]["packages"] = ["org.nextos.synthetic"]
            bypass_path = self.game / ("container-bypass-" + label + ".json")
            bypass_path.write_text(
                json.dumps(bypass_recipe, sort_keys=True), encoding="utf-8"
            )
            with self.assertRaisesRegex(NX.RecipeError, expected):
                NX.Recipe(bypass_path)

        final_check_rule = self.container_rule()
        final_check_recipe = base_recipe(
            self.lib,
            self.assets,
            extra_rules=[final_check_rule],
            extra_commit=["data/game.apk"],
        )
        final_check_recipe["input"]["packages"] = ["org.nextos.synthetic"]
        final_check_recipe["validate"].append(
            {
                "path": "data/game.apk",
                "sha256": sha256(b"owner-copy"),
            }
        )
        final_check_path = self.game / "container-bypass-final-check.json"
        final_check_path.write_text(
            json.dumps(final_check_recipe, sort_keys=True), encoding="utf-8"
        )
        with self.assertRaisesRegex(NX.RecipeError, "NXA0001"):
            NX.Recipe(final_check_path)

        def assert_recipe_shape_accepted(name, candidate):
            path = self.game / (name + ".json")
            path.write_text(
                json.dumps(candidate, sort_keys=True, indent=2), encoding="utf-8"
            )
            NX.Recipe(str(path))

        angry_shape = {
            "schema": 1,
            "id": "angry-shape",
            "version": "8.0.3",
            "title": "ANGRY SHAPE",
            "abi_order": ["armeabi-v7a"],
            "input": {"packages": ["com.rovio.angrybirds"]},
            "reference_build": {
                "game_version": "8.0.3",
                "container_sha256": sha256(b"official-one"),
                "container_size": 99999999,
            },
            "extract": [
                {
                    "id": "owner-apk",
                    "source": {"kind": "container"},
                    "destination": "game.apk",
                    "validate": {
                        "type": "file",
                        "min_size": 100,
                        "max_size": 200000000,
                        "magic_hex": "504b0304",
                    },
                }
            ],
            "validate": [{"path": "game.apk", "type": "file", "min_size": 100}],
            "commit": ["game.apk"],
        }
        assert_recipe_shape_accepted("angry-shape", angry_shape)

        scourge_shape = {
            "schema": 1,
            "id": "scourge-shape",
            "version": "1",
            "title": "SCOURGE SHAPE",
            "abi_order": ["arm64-v8a"],
            "input": {"packages": ["com.pid.scourgebringer"]},
            "extract": [
                {
                    "id": "content",
                    "source": {
                        "kind": "entries",
                        "patterns": ["assets/Content/*"],
                        "strip_prefix": "assets/Content/",
                    },
                    "destination": "assets/Content",
                    "validate": {
                        "type": "tree",
                        "min_files": 2,
                        "max_files": 100,
                        "required_paths": ["common_0.xnb", "Sounds/Master.bank"],
                    },
                },
                {
                    "id": "assembly-container",
                    "source": {"kind": "container"},
                    "destination": "assemblies.apk",
                    "validate": {"type": "file"},
                },
            ],
            "hooks": [
                {
                    "id": "assembly-repack",
                    "argv": ["python3", "{game_dir}/make-assembly.py"],
                    "checkpoint": [
                        {
                            "path": "assemblies.apk",
                            "type": "file",
                            "min_size": 100,
                            "max_size": 200000000,
                            "magic_ascii": "PK",
                        }
                    ],
                }
            ],
            "validate": [
                {
                    "path": "assets/Content",
                    "type": "tree",
                    "min_files": 2,
                    "required_paths": ["common_0.xnb"],
                },
                {
                    "path": "assemblies.apk",
                    "type": "file",
                    "min_size": 100,
                    "magic_ascii": "PK",
                },
            ],
            "commit": ["assets/Content", "assemblies.apk"],
        }
        assert_recipe_shape_accepted("scourge-shape", scourge_shape)

        retro_shape = base_recipe(
            self.lib,
            self.assets,
            extra_rules=[self.container_rule("runtime/retrohighway.apk")],
            extra_commit=["runtime/retrohighway.apk"],
        )
        retro_shape["input"]["packages"] = ["com.nicolaigd.retrohighway"]
        assert_recipe_shape_accepted("retro-shape", retro_shape)

    def test_container_accepts_repackaging_but_rejects_wrong_identity(self):
        recipe = base_recipe(
            self.lib,
            self.assets,
            extra_rules=[self.container_rule()],
            extra_commit=["data/game.apk"],
        )
        recipe["input"]["packages"] = ["org.nextos.synthetic"]
        self.write_recipe(recipe)

        reference = self.data / "reference.apk"
        make_zip(reference, self.merged_entries())
        repacked = self.data / ("renamed-repacked-" + uuid.uuid4().hex)
        repacked_entries = {
            "META-INF/DIFFERENT.SF": b"different signing container",
            **self.merged_entries(),
        }
        make_zip(repacked, repacked_entries)
        self.assertNotEqual(
            sha256(reference.read_bytes()), sha256(repacked.read_bytes())
        )

        self.run_cli(extra=["--input", str(reference)])
        self.assertEqual((self.game / "data/game.apk").read_bytes(), reference.read_bytes())
        reference.unlink()

        self.run_cli(
            extra=["--force-source", "--input", str(repacked)]
        )
        installed = (self.game / "data/game.apk").read_bytes()
        self.assertEqual(installed, repacked.read_bytes())

        wrong_package = self.data / "wrong-package.apk"
        make_zip(
            wrong_package,
            self.merged_entries(plain_manifest("org.nextos.different")),
        )
        self.run_cli(
            expect=1,
            extra=["--force-source", "--input", str(wrong_package)],
        )
        self.assertEqual((self.game / "data/game.apk").read_bytes(), installed)

        wrong_payload = self.data / "wrong-payload.apk"
        entries = self.merged_entries()
        entries["lib/arm64-v8a/libgame.so"] = fake_elf(machine=40)
        make_zip(wrong_payload, entries)
        self.run_cli(
            expect=1,
            extra=["--force-source", "--input", str(wrong_payload)],
        )
        self.assertEqual((self.game / "data/game.apk").read_bytes(), installed)

    def _apkcompat_canonical_path(self):
        return (
            Path(__file__).resolve().parents[3]
            / "framework"
            / "contracts"
            / "apkcompat"
            / "apkcompat.py"
        )

    def test_apkcompat_embedded_module_is_byte_identical_to_canonical(self):
        """Sync gate: the embedded APKCOMPAT copy equals the canonical module."""
        text = NXEXTRACT.read_text(encoding="utf-8")
        begin = text.index("--- BEGIN APKCOMPAT CANONICAL")
        begin = text.index("\n", begin) + 1
        end = text.index("# --- END APKCOMPAT CANONICAL ---")
        embedded = text[begin:end]
        canonical = self._apkcompat_canonical_path().read_text(encoding="utf-8")
        self.assertEqual(embedded, canonical)

    def test_apkcompat_shared_fixture_cases(self):
        fixtures = json.loads(
            (
                self._apkcompat_canonical_path().parent
                / "fixtures"
                / "apk-compat-cases-v3.json"
            ).read_text(encoding="utf-8")
        )
        self.assertEqual(fixtures["schema_version"], 3)
        for case in fixtures["cases"]:
            failures = []
            NX.validate_recipe_apk_compat(case["recipe"], failures.append)
            if case["expect"] == "accept":
                self.assertEqual(
                    failures, [], "case %s must be accepted" % case["id"]
                )
            else:
                self.assertTrue(
                    any(case["code"] in item for item in failures),
                    "case %s must fail with %s, got %r"
                    % (case["id"], case["code"], failures),
                )

    def test_hook_inline_contract_shapes(self):
        contract = {
            "schema": "org.nextos.apk-compat.hook-contract",
            "schema_version": 1,
            "hook_id": "patch",
            "inputs": ["assets/data.bin"],
            "predicates": [
                {"class": "compatibility", "checks": "UnityFS header present"}
            ],
            "fallback": "generic-symbolic-path",
            "error_codes": ["NXH0001"],
        }
        hook = {
            "id": "patch",
            "argv": [sys.executable, "{game_dir}/noop.py"],
            "contract": contract,
        }
        recipe = base_recipe(self.lib, self.assets, hooks=[hook])
        self.write_recipe(recipe)
        NX.Recipe(str(self.recipe_path))

        broken = dict(contract)
        del broken["fallback"]
        hook_broken = dict(hook, contract=broken)
        bad = base_recipe(self.lib, self.assets, hooks=[hook_broken])
        bad_path = self.game / "bad-hook-contract.json"
        bad_path.write_text(json.dumps(bad, sort_keys=True), encoding="utf-8")
        with self.assertRaisesRegex(NX.RecipeError, "NXA0047"):
            NX.Recipe(str(bad_path))

        identity = dict(
            contract,
            predicates=[
                {"class": "reference_identity", "checks": "container sha"}
            ],
        )
        hook_identity = dict(hook, contract=identity)
        worse = base_recipe(self.lib, self.assets, hooks=[hook_identity])
        worse_path = self.game / "identity-hook-contract.json"
        worse_path.write_text(json.dumps(worse, sort_keys=True), encoding="utf-8")
        with self.assertRaisesRegex(NX.RecipeError, "NXA0046"):
            NX.Recipe(str(worse_path))

    def test_hook_identity_predicate_cannot_reject_at_runtime(self):
        script = self.game / "identity-gate.py"
        script.write_text(
            "print('NXEXTRACT_PREDICATE reference_identity container-sha fail')\n"
            "raise SystemExit(7)\n",
            encoding="utf-8",
        )
        hook = {"id": "identity-gate", "argv": [sys.executable, "{game_dir}/identity-gate.py"]}
        recipe = base_recipe(self.lib, self.assets, hooks=[hook])
        self.write_recipe(recipe)
        make_zip(self.data / "game.apk", self.merged_entries())
        self.run_cli(expect=1)
        terminal = json.loads(
            (self.game / "nxextract-result.json").read_text(encoding="utf-8")
        )
        message = terminal["error"]["message"]
        self.assertIn("NXA0046", message)
        self.assertIn("must never decide compatibility", message)

    def test_terraria_metamorphic_regression(self):
        """Documented .4 identity, internal .49 asset: the install must pass.

        The static defence must still flag a hook that greps the literal
        documented token -- the exact Terraria 2.0.0 failure shape.
        """
        assets = dict(self.assets)
        assets["data.unity3d"] = (
            b"UnityFS\x00internal-version 1.4.5.6.49 payload"
        )
        recipe = base_recipe(self.lib, assets)
        recipe["input"]["packages"] = ["org.nextos.synthetic"]
        recipe["reference_build"] = {
            "game_version": "1.4.5.6.4",
            "container_size": 233218800,
            "container_sha256": sha256(b"reference container"),
        }
        recipe["compatibility"] = {
            "package_families": ["org.nextos.synthetic"],
            "abis": ["arm64-v8a"],
            "payload_contracts": ["UnityFS"],
        }
        self.write_recipe(recipe)
        make_zip(
            self.data / "renamed-arbitrarily.apk",
            {
                "AndroidManifest.xml": plain_manifest("org.nextos.synthetic"),
                "lib/arm64-v8a/libgame.so": self.lib,
                **{"assets/" + key: value for key, value in assets.items()},
            },
        )
        self.run_cli()
        installed = (self.game / "assets" / "data.unity3d").read_bytes()
        self.assertIn(b"1.4.5.6.49", installed)

        fixtures = json.loads(
            (
                self._apkcompat_canonical_path().parent
                / "fixtures"
                / "apk-compat-cases-v3.json"
            ).read_text(encoding="utf-8")
        )
        regression = fixtures["terraria_regression"]
        findings = NX.scan_static_suspects(
            regression["hook_snippet_that_must_be_flagged"], "terraria-hook"
        )
        self.assertTrue(
            any(regression["expected_static_code"] in item for item in findings),
            findings,
        )

    def test_obs_events_emitted_when_launcher_provides_run_id(self):
        recipe = base_recipe(self.lib, self.assets)
        self.write_recipe(recipe)
        make_zip(self.data / "game.apk", self.merged_entries())
        events = self.game / "events.jsonl"
        self.run_cli(
            env={
                "NXOBS_RUN_ID": "synthetic-run-1",
                "NXOBS_EVENTS_FILE": str(events),
            }
        )
        lines = [
            json.loads(line)
            for line in events.read_text(encoding="utf-8").splitlines()
            if line.strip()
        ]
        self.assertGreaterEqual(len(lines), 2)
        self.assertEqual(lines[0]["schema"], "nx-event-v1")
        self.assertEqual(lines[0]["run_id"], "synthetic-run-1")
        self.assertEqual(lines[0]["status"], "begin")
        self.assertEqual(lines[-1]["source"], "extractor")
        self.assertEqual(lines[-1]["status"], "ok")
        sequences = [line["sequence"] for line in lines]
        self.assertEqual(sequences, sorted(sequences))

    def test_hook_resource_fence_kills_infinite_output(self):
        """V3-HARDENING-01: a hook flooding stdout is fenced and killed."""
        script = self.game / "flood.py"
        script.write_text(
            "import sys\n"
            "while True:\n"
            "    sys.stdout.write('x' * 8192 + chr(10))\n",
            encoding="utf-8",
        )
        hook = {
            "id": "flood",
            "argv": [sys.executable, "-u", "{game_dir}/flood.py"],
            "limits": {"output_bytes": 65536, "wall_seconds": 20},
        }
        recipe = base_recipe(self.lib, self.assets, hooks=[hook])
        self.write_recipe(recipe)
        make_zip(self.data / "game.apk", self.merged_entries())
        self.run_cli(expect=1)
        terminal = json.loads(
            (self.game / "nxextract-result.json").read_text(encoding="utf-8")
        )
        message = terminal["error"]["message"]
        self.assertIn("resource fence", message)
        self.assertIn("output_bytes", message)
        # the validated stage survives for a clean retry
        self.assertTrue((self.game / ".nxextract").exists())

    def test_hook_resource_fence_kills_term_ignoring_sleeper(self):
        """A hook that ignores TERM and sleeps past its wall budget dies by
        group KILL; the validated stage survives."""
        script = self.game / "sleeper.py"
        script.write_text(
            "import signal, time\n"
            "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
            "print('alive', flush=True)\n"
            "time.sleep(600)\n",
            encoding="utf-8",
        )
        hook = {
            "id": "sleeper",
            "argv": [sys.executable, "-u", "{game_dir}/sleeper.py"],
            "limits": {"wall_seconds": 2},
        }
        recipe = base_recipe(self.lib, self.assets, hooks=[hook])
        self.write_recipe(recipe)
        make_zip(self.data / "game.apk", self.merged_entries())
        started = time.monotonic()
        self.run_cli(expect=1)
        self.assertLess(time.monotonic() - started, 25)
        terminal = json.loads(
            (self.game / "nxextract-result.json").read_text(encoding="utf-8")
        )
        self.assertIn("wall_seconds", terminal["error"]["message"])
        self.assertTrue((self.game / ".nxextract").exists())

    def test_hook_resource_fence_kills_the_whole_process_group(self):
        """A hook that spawns children and ignores TERM takes them with it.

        V3-HARDENING-01 names this negative explicitly. Killing only the hook
        would leave its children running past the extraction, writing into the
        card long after the engine reported failure -- the exact orphan the
        group kill exists to prevent.
        """
        survivor = self.game / "survivor.marker"
        child = self.game / "orphan.py"
        child.write_text(
            "import pathlib\n"
            "import signal\n"
            "import sys\n"
            "import time\n"
            "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
            "time.sleep(12)\n"
            "pathlib.Path(sys.argv[1]).write_text('orphan')\n",
            encoding="utf-8",
        )
        script = self.game / "spawner.py"
        script.write_text(
            "import signal\n"
            "import subprocess\n"
            "import sys\n"
            "import time\n"
            "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
            "subprocess.Popen([sys.executable, sys.argv[1], sys.argv[2]])\n"
            "print('spawned', flush=True)\n"
            "time.sleep(600)\n",
            encoding="utf-8",
        )
        hook = {
            "id": "spawner",
            "argv": [sys.executable, "-u", "{game_dir}/spawner.py",
                     str(child), str(survivor)],
            "limits": {"wall_seconds": 2},
        }
        recipe = base_recipe(self.lib, self.assets, hooks=[hook])
        self.write_recipe(recipe)
        make_zip(self.data / "game.apk", self.merged_entries())
        started = time.monotonic()
        self.run_cli(expect=1)
        self.assertLess(time.monotonic() - started, 25)
        terminal = json.loads(
            (self.game / "nxextract-result.json").read_text(encoding="utf-8")
        )
        self.assertIn("wall_seconds", terminal["error"]["message"])
        # The child would write its marker at +12s. Wait past that point: if
        # the group kill worked, the marker never appears.
        time.sleep(14)
        self.assertFalse(
            survivor.exists(),
            "a hook child survived the group kill and kept writing")
        self.assertTrue((self.game / ".nxextract").exists())

    def test_hook_resource_fence_failure_is_fail_closed(self):
        """V3-HARDENING-01 closed: a fence that cannot be established REFUSES.

        The 1.2.x fence swallowed every setrlimit error, so a host where the
        limit could not be applied ran the hook with no CPU, address-space or
        file-size ceiling at all -- precisely the state the limits exist to
        prevent. The hook must never start unfenced.
        """
        marker = self.game / "hook-ran.marker"
        script = self.game / "marker.py"
        script.write_text(
            "import pathlib, sys\n"
            "pathlib.Path(sys.argv[1]).write_text('ran')\n",
            encoding="utf-8",
        )
        hook = {
            "id": "fenced",
            "argv": [sys.executable, "-u", "{game_dir}/marker.py",
                     str(marker)],
        }
        recipe = base_recipe(self.lib, self.assets, hooks=[hook])
        self.write_recipe(recipe)
        make_zip(self.data / "game.apk", self.merged_entries())

        # Control: without the injected failure the hook really runs, so the
        # negative below cannot pass vacuously.
        self.run_cli(expect=0)
        self.assertTrue(marker.exists())
        marker.unlink()

        self.run_cli(expect=1, extra=["--force-source"],
                     env={"NXEXTRACT_TEST_FENCE_FAULT": "RLIMIT_AS"})
        self.assertFalse(
            marker.exists(),
            "the hook executed even though its resource fence failed")
        terminal = json.loads(
            (self.game / "nxextract-result.json").read_text(encoding="utf-8")
        )
        message = terminal["error"]["message"]
        self.assertIn("cannot start hook fenced", message)
        self.assertTrue((self.game / ".nxextract").exists())

    def test_hook_limits_schema_is_bounded(self):
        for label, limits, expected in (
            ("unknown", {"disk_bytes": 1}, "limits.disk_bytes is unknown"),
            ("zero", {"wall_seconds": 0}, "within 1"),
            ("above-ceiling", {"wall_seconds": 999999}, "within 1"),
            ("type", {"cpu_seconds": "fast"}, "within 1"),
        ):
            hook = {
                "id": "limited",
                "argv": [sys.executable, "-c", "pass"],
                "limits": limits,
            }
            recipe = base_recipe(self.lib, self.assets, hooks=[hook])
            path = self.game / ("limits-" + label + ".json")
            path.write_text(json.dumps(recipe), encoding="utf-8")
            with self.assertRaisesRegex(NX.RecipeError, expected):
                NX.Recipe(str(path))

    def test_hook_cannot_override_compatibility_receipt_environment(self):
        hook = {
            "id": "spoof-selection",
            "argv": [sys.executable, "-c", "pass"],
            "env": {"NXEXTRACT_COMPATIBILITY_JSON": "{}"},
        }
        recipe = base_recipe(self.lib, self.assets, hooks=[hook])
        path = self.game / "reserved-hook-env.json"
        path.write_text(json.dumps(recipe), encoding="utf-8")
        with self.assertRaisesRegex(NX.RecipeError, "reserved engine variable"):
            NX.Recipe(path)

    def test_recipe_hardening_byte_ceiling(self):
        recipe = base_recipe(self.lib, self.assets)
        recipe["title"] = "PAD " + "x" * (1024 * 1024)
        oversized = self.game / "oversized.json"
        oversized.write_text(json.dumps(recipe), encoding="utf-8")
        with self.assertRaisesRegex(NX.RecipeError, "hardening ceiling"):
            NX.Recipe(str(oversized))

    def test_recipe_refuses_another_application(self):
        """Dois jogos do mesmo estudio tem os mesmos nomes de asset.

        Sem fixar o pacote, a receita de um aceita o APK do outro e instala o
        jogo errado sem uma linha de reclamacao.
        """
        recipe = base_recipe(self.lib, self.assets)
        recipe["input"] = dict(recipe.get("input", {}),
                               packages=["org.nextos.expected"])
        self.write_recipe(recipe)
        source = self.data / ("other-" + uuid.uuid4().hex + ".apk")
        make_zip(source, self.merged_entries())
        self.run_cli(expect=1)
        report = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("org.nextos.synthetic", report)
        self.assertIn("org.nextos.expected", report)
        self.assertFalse((self.game / "assets").exists())

    def test_recipe_accepts_the_declared_application(self):
        recipe = base_recipe(self.lib, self.assets)
        recipe["input"] = dict(recipe.get("input", {}),
                               packages=["org.nextos.synthetic"])
        self.write_recipe(recipe)
        source = self.data / ("app-" + uuid.uuid4().hex + ".apk")
        make_zip(source, self.merged_entries())
        self.run_cli()
        self.assert_payload()

    def test_invalid_input_contract_fails_before_log_or_workspace(self):
        recipe = base_recipe(self.lib, self.assets)
        recipe["input"]["max_files"] = True
        self.write_recipe(recipe)

        self.run_cli(expect=1)

        self.assertFalse((self.game / "test-extract.log").exists())
        self.assertFalse((self.game / ".nxextract").exists())

    def test_invalid_recipe_templates_and_reserved_paths_fail_before_effects(self):
        cases = []

        unknown_template = base_recipe(self.lib, self.assets)
        unknown_template["hooks"] = [
            {"id": "bad-template", "argv": ["{unknown_field}"]}
        ]
        cases.append(("unknown-template", unknown_template))

        workspace_commit = base_recipe(self.lib, self.assets)
        workspace_commit["commit"] = [".nxextract", "assets"]
        cases.append(("workspace-commit", workspace_commit))

        marker_log_collision = base_recipe(self.lib, self.assets)
        marker_log_collision["marker"] = marker_log_collision["log"]
        cases.append(("marker-log-collision", marker_log_collision))

        movable_result = base_recipe(self.lib, self.assets)
        movable_result["result"] = "custom-result.json"
        cases.append(("movable-terminal-result", movable_result))

        detail_result_collision = base_recipe(self.lib, self.assets)
        detail_result_collision["detail_log"] = "nxextract-result.json"
        cases.append(("detail-result-collision", detail_result_collision))

        invalid_fingerprint = base_recipe(self.lib, self.assets)
        invalid_fingerprint["extract"][1]["validate"]["tree_fingerprint"] = "bad"
        cases.append(("invalid-fingerprint", invalid_fingerprint))

        for label, recipe in cases:
            with self.subTest(label=label):
                self.write_recipe(recipe)
                self.run_cli(expect=1)
                self.assertFalse((self.game / "test-extract.log").exists())
                self.assertFalse((self.game / ".nxextract").exists())

    def test_loose_obb_is_chosen_by_hash_not_filename(self):
        obb = b"LPK\0" + os.urandom(128)
        obb_rule = {
            "id": "obb",
            "source": {
                "kind": "entry_or_file",
                "patterns": ["*"],
                "file_extensions": [".obb"],
            },
            "destination": "data/game.obb",
            "validate": {
                "size": len(obb),
                "sha256": sha256(obb),
                "magic_ascii": "LPK\u0000",
            },
        }
        self.write_recipe(
            base_recipe(
                self.lib,
                self.assets,
                extra_rules=[obb_rule],
                extra_commit=["data/game.obb"],
            )
        )
        make_zip(self.data / "base.apk", self.merged_entries())
        loose = self.data / (uuid.uuid4().hex + ".obb")
        loose.write_bytes(obb)
        self.run_cli()
        self.assertEqual((self.game / "data/game.obb").read_bytes(), obb)
        self.assertTrue(loose.exists())

    def test_second_run_uses_marker_without_source(self):
        self.write_recipe()
        source = self.data / "first.apk"
        make_zip(source, self.merged_entries())
        self.run_cli()
        parked = Path(self.temporary.name) / "parked-source"
        source.rename(parked)
        self.run_cli()
        self.assert_payload()

    def test_fast_marker_requires_schema_and_payload_metadata_seal(self):
        self.write_recipe()
        source = self.data / "sealed.apk"
        make_zip(source, self.merged_entries())
        self.run_cli()
        recipe = NX.Recipe(str(self.recipe_path))
        marker_path = self.game / ".synthetic-data.json"
        marker = json.loads(marker_path.read_text(encoding="utf-8"))
        logger = NX.Logger(None, verbose=False)

        self.assertTrue(NX.marker_matches_recipe(marker, recipe))
        self.assertIsNotNone(
            NX.marker_fast_valid(
                str(marker_path), recipe, str(self.game), logger
            )
        )

        payload = self.game / "assets/readme.dat"
        original = payload.read_bytes()
        payload.write_bytes(b"X" * len(original))
        self.assertIsNone(
            NX.marker_fast_valid(
                str(marker_path), recipe, str(self.game), logger
            )
        )
        payload.write_bytes(original)
        marker["commit"] = marker["commit"] + ["unexpected"]
        NX.atomic_write_json(marker_path, marker)
        self.assertFalse(NX.marker_matches_recipe(marker, recipe))
        self.assertIsNone(
            NX.marker_fast_valid(
                str(marker_path), recipe, str(self.game), logger
            )
        )

    def test_metadata_only_drift_reseals_by_content_without_ui_or_extraction(self):
        self.write_recipe()
        source = self.data / "sealed.apk"
        make_zip(source, self.merged_entries())
        self.run_cli()
        marker_path = self.game / ".synthetic-data.json"
        before = json.loads(marker_path.read_text(encoding="utf-8"))
        self.assertRegex(before["payload_content_seal"], r"^[0-9a-f]{64}$")

        payload = self.game / "assets/readme.dat"
        payload_bytes = payload.read_bytes()
        info = payload.stat()
        os.utime(
            payload,
            ns=(info.st_atime_ns, info.st_mtime_ns + 2_000_000_000),
        )
        source.rename(Path(self.temporary.name) / "parked-source")
        ui_started = self.game / "ui-started"
        ui = self.game / "must-not-start.py"
        ui.write_text(
            "#!/usr/bin/env python3\n"
            "from pathlib import Path\n"
            "Path(%r).write_text('started')\n" % str(ui_started),
            encoding="utf-8",
        )
        ui.chmod(0o700)

        self.run_cli(
            extra=["--reuse-only", "--require-ui", "--ui", str(ui)]
        )

        after = json.loads(marker_path.read_text(encoding="utf-8"))
        self.assertEqual(payload.read_bytes(), payload_bytes)
        self.assertEqual(after["transaction_id"], before["transaction_id"])
        self.assertEqual(
            after["payload_content_seal"], before["payload_content_seal"]
        )
        self.assertNotEqual(after["payload_seal"], before["payload_seal"])
        self.assertFalse(ui_started.exists())
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("metadata drift authenticated", log)
        self.assertIn("no source scan needed", log)

    def test_reuse_only_rejects_same_size_corruption_before_ui(self):
        self.write_recipe()
        make_zip(self.data / "sealed.apk", self.merged_entries())
        self.run_cli()
        marker_path = self.game / ".synthetic-data.json"
        before = json.loads(marker_path.read_text(encoding="utf-8"))
        payload = self.game / "assets/readme.dat"
        payload.write_bytes(b"X" * len(payload.read_bytes()))
        ui_started = self.game / "ui-started"
        ui = self.game / "must-not-start.py"
        ui.write_text(
            "#!/usr/bin/env python3\n"
            "from pathlib import Path\n"
            "Path(%r).write_text('started')\n" % str(ui_started),
            encoding="utf-8",
        )
        ui.chmod(0o700)

        self.run_cli(
            expect=1,
            extra=["--reuse-only", "--require-ui", "--ui", str(ui)],
        )

        after = json.loads(marker_path.read_text(encoding="utf-8"))
        self.assertEqual(after, before)
        self.assertEqual(payload.read_bytes(), b"X" * len(self.assets["readme.dat"]))
        self.assertFalse(ui_started.exists())
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("payload content seal mismatch", log)
        self.assertIn("setup UI and extraction were not started", log)

    def test_reuse_only_refuses_pending_transaction_without_recovery(self):
        self.write_recipe()
        make_zip(self.data / "sealed.apk", self.merged_entries())
        self.run_cli()
        recipe = NX.Recipe(str(self.recipe_path))
        workspace = Path(NX.prepare_workspace(str(self.game), recipe.identifier))
        journal = workspace / "transaction.json"
        journal.write_text('{"deliberately":"invalid"}\n', encoding="utf-8")
        before = journal.read_bytes()

        self.run_cli(expect=1, extra=["--reuse-only"])

        self.assertEqual(journal.read_bytes(), before)
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("pending payload transaction requires recovery", log)
        self.assertIn("no recovery, setup UI or extraction was started", log)

    def test_legacy_120_marker_migrates_without_ui_or_extraction(self):
        self.write_recipe()
        source = self.data / "sealed.apk"
        make_zip(source, self.merged_entries())
        self.run_cli()
        marker_path = self.game / ".synthetic-data.json"
        marker = json.loads(marker_path.read_text(encoding="utf-8"))
        transaction_id = marker["transaction_id"]
        expected_content_seal = marker["payload_content_seal"]
        marker["nxextract_version"] = "1.2.20"
        marker.pop("payload_content_seal")
        marker.pop("payload_content_bytes")
        NX.atomic_write_json(marker_path, marker)
        payload = self.game / "assets/readme.dat"
        info = payload.stat()
        os.utime(
            payload,
            ns=(info.st_atime_ns, info.st_mtime_ns + 2_000_000_000),
        )
        source.rename(Path(self.temporary.name) / "parked-source")

        self.run_cli(expect=1, extra=["--reuse-only"])
        rejected = json.loads(marker_path.read_text(encoding="utf-8"))
        self.assertEqual(rejected["nxextract_version"], "1.2.20")
        self.assertNotIn("payload_content_seal", rejected)

        self.run_cli(
            extra=[
                "--reuse-only",
                "--expected-content-seal",
                expected_content_seal,
            ]
        )

        migrated = json.loads(marker_path.read_text(encoding="utf-8"))
        self.assertEqual(migrated["nxextract_version"], NX.NXEXTRACT_VERSION)
        self.assertEqual(migrated["marker_migrated_from"], "1.2.20")
        self.assertEqual(migrated["transaction_id"], transaction_id)
        self.assertRegex(migrated["payload_content_seal"], r"^[0-9a-f]{64}$")
        self.assert_payload()
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("no extraction", log)

    def test_exact_current_marker_keeps_metadata_fast_path(self):
        self.write_recipe()
        make_zip(self.data / "sealed.apk", self.merged_entries())
        self.run_cli()
        recipe = NX.Recipe(str(self.recipe_path))
        marker_path = self.game / ".synthetic-data.json"
        logger = NX.Logger(None, verbose=False)
        with mock.patch.object(
            NX,
            "payload_content_seal",
            side_effect=AssertionError("content path must stay cold"),
        ):
            self.assertIsNotNone(
                NX.marker_fast_valid(
                    str(marker_path), recipe, str(self.game), logger
                )
            )

    def test_mutable_path_survives_guest_save_and_stays_sealed_elsewhere(self):
        # P12 (caso Tightrope): o jogo grava o save DENTRO do payload selado.
        # Com o caminho declarado em "mutable", o marker continua valido apos
        # o save mudar/criar; qualquer OUTRO arquivo mudando ainda reprova.
        recipe = base_recipe(self.lib, self.assets)
        recipe["mutable"] = ["assets/mySave.sol"]
        self.write_recipe(recipe)
        source = self.data / "sealed.apk"
        make_zip(source, self.merged_entries())
        self.run_cli()
        recipe_obj = NX.Recipe(str(self.recipe_path))
        marker_path = self.game / ".synthetic-data.json"
        logger = NX.Logger(None, verbose=False)

        self.assertIsNotNone(
            NX.marker_fast_valid(
                str(marker_path), recipe_obj, str(self.game), logger))
        # o guest cria e depois altera o save: marker permanece valido
        save = self.game / "assets/mySave.sol"
        save.write_bytes(b"save-1")
        self.assertIsNotNone(
            NX.marker_fast_valid(
                str(marker_path), recipe_obj, str(self.game), logger))
        save.write_bytes(b"save-2-diferente")
        self.assertIsNotNone(
            NX.marker_fast_valid(
                str(marker_path), recipe_obj, str(self.game), logger))
        # o resto do payload segue selado
        other = self.game / "assets/readme.dat"
        original = other.read_bytes()
        other.write_bytes(b"X" * len(original))
        self.assertIsNone(
            NX.marker_fast_valid(
                str(marker_path), recipe_obj, str(self.game), logger))
        other.write_bytes(original)

    def test_mutable_path_recipe_validation_is_strict(self):
        # fora de commit, com {abi}, duplicado e lista gigante: todos reprovam
        for bad in (["outside.sol"], ["lib/{abi}/x.so"],
                    ["assets/a", "assets/a"], ["assets/%d" % i for i in range(33)]):
            recipe = base_recipe(self.lib, self.assets)
            recipe["mutable"] = bad
            self.write_recipe(recipe)
            with self.assertRaises(NX.RecipeError):
                NX.Recipe(str(self.recipe_path))
        # recipe sem o membro segue identico (regressao)
        self.write_recipe()
        self.assertEqual(NX.Recipe(str(self.recipe_path)).mutable_paths, ())

    def test_force_source_reinstalls_valid_payload_transactionally(self):
        self.write_recipe()
        source = self.data / "first.apk"
        make_zip(source, self.merged_entries())
        self.run_cli()
        marker_path = self.game / ".synthetic-data.json"
        first_marker = json.loads(marker_path.read_text(encoding="utf-8"))

        self.run_cli(extra=["--force-source"])

        second_marker = json.loads(marker_path.read_text(encoding="utf-8"))
        self.assertNotEqual(
            first_marker["transaction_id"],
            second_marker["transaction_id"],
        )
        self.assert_payload()
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn(
            "force-source requested; bypassing the installed marker",
            log,
        )

    @unittest.skipIf(os.geteuid() == 0, "root ignores the read-only directory")
    def test_undeletable_source_cache_does_not_fail_a_committed_install(self):
        # A FUSE-backed share (exFAT on Knulli, NFS, SMB) can refuse to drop the
        # scratch cache. The payload is already committed at that point, so the
        # run must still succeed. A read-only directory reproduces the refusal.
        self.write_recipe()
        make_zip(self.data / "merged.apk", self.merged_entries())
        cache = self.game / ".nxextract/synthetic-port/source-cache"
        trap = cache / "undeletable"
        trap.mkdir(parents=True)
        (trap / "pinned").write_bytes(b"pinned")
        trap.chmod(0o500)
        try:
            self.run_cli()
            self.assert_payload()
            log = (self.game / "test-extract.log").read_text(encoding="utf-8")
            self.assertIn("warning: kept source cache for the next run", log)
        finally:
            trap.chmod(0o700)

    def _assert_published_cleanup_failure_is_nonfatal(self, cleanup_name):
        self.write_recipe()
        make_zip(self.data / "cleanup.apk", self.merged_entries())
        workspace = self.game / ".nxextract/synthetic-port"
        marker = self.game / ".synthetic-data.json"
        target = workspace / cleanup_name
        real_remove_path = NX.remove_path
        real_rmtree = NX.shutil.rmtree
        real_unlink = NX.os.unlink
        target_path = os.path.abspath(str(target))

        def is_published_target(path):
            return marker.exists() and os.path.abspath(os.fspath(path)) == target_path

        def refuse_remove(path):
            if is_published_target(path):
                raise OSError(errno.ENOTEMPTY, "synthetic cleanup refusal", str(path))
            return real_remove_path(path)

        def refuse_rmtree(path, *args, **kwargs):
            if is_published_target(path):
                # Simulate a FUSE/SMB path that remains present after the
                # best-effort retry. The retained journal must trigger a later
                # recovery attempt.
                return None
            return real_rmtree(path, *args, **kwargs)

        def refuse_unlink(path, *args, **kwargs):
            if is_published_target(path):
                raise OSError(errno.EBUSY, "synthetic cleanup refusal", str(path))
            return real_unlink(path, *args, **kwargs)

        argv = [
            "install",
            "--recipe",
            str(self.recipe_path),
            "--game-dir",
            str(self.game),
            "--quiet",
            "--ui",
            "none",
        ]
        with mock.patch.object(NX, "remove_path", side_effect=refuse_remove), \
                mock.patch.object(NX.shutil, "rmtree", side_effect=refuse_rmtree), \
                mock.patch.object(NX.os, "unlink", side_effect=refuse_unlink):
            status = NX.main(argv)

        self.assertEqual(status, 0)
        self.assert_payload()
        self.assertTrue(marker.exists())
        self.assertTrue(target.exists())
        self.assertTrue((workspace / "transaction.json").exists())
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("published payload remains valid", log)

        # Once the filesystem accepts removal again, normal recovery consumes
        # the retained journal before accepting the fast marker.
        self.run_cli()
        self.assertFalse(target.exists())
        self.assertFalse((workspace / "transaction.json").exists())

    def test_backup_cleanup_failure_after_marker_is_nonfatal(self):
        self._assert_published_cleanup_failure_is_nonfatal("backup")

    def test_stage_cleanup_failure_after_marker_is_nonfatal(self):
        self._assert_published_cleanup_failure_is_nonfatal("stage")

    def test_journal_cleanup_failure_after_marker_is_nonfatal(self):
        self._assert_published_cleanup_failure_is_nonfatal("transaction.json")

    def test_existing_data_rejection_logs_validation_reason(self):
        self.write_recipe()
        installed = self.game / "lib/arm64-v8a/libgame.so"
        installed.parent.mkdir(parents=True)
        installed.write_bytes(self.lib + b"-changed")
        self.run_cli(expect=1)
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn(
            "existing data not adoptable for ABI arm64-v8a:",
            log,
        )
        self.assertIn(
            "native (lib/arm64-v8a/libgame.so) has unexpected size",
            log,
        )

    def test_rejected_candidates_are_reported_as_different_build(self):
        self.write_recipe()
        entries = self.merged_entries()
        entries["lib/arm64-v8a/libgame.so"] = self.lib + b"-other-build"
        make_zip(self.data / "other-build.apk", entries)
        self.run_cli(expect=1)
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn(
            "required payload native was not found: 1 candidate(s) matched "
            "the source pattern but failed validation",
            log,
        )
        self.assertIn("probably a different build of the game", log)
        self.assertIn("lib/arm64-v8a/libgame.so", log)

    def test_valid_staged_file_is_resumed_without_rewrite(self):
        self.write_recipe()
        source = self.data / "resume.apk"
        make_zip(source, self.merged_entries())
        staged = (
            self.game
            / ".nxextract/synthetic-port/stage/lib/arm64-v8a/libgame.so"
        )
        staged.parent.mkdir(parents=True)
        staged.write_bytes(self.lib)
        old_time = time.time() - 3600
        os.utime(staged, (old_time, old_time))
        self.run_cli()
        self.assert_payload()
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("resuming", log)

    def test_stage_failing_whole_set_validation_is_discarded(self):
        # Cada item extraído passa sozinho; o CONJUNTO reprova porque a receita
        # exige um payload companheiro que nenhuma regra produz. Antes da 1.2.2
        # o stage ruim ficava no disco e o resume o revalidava e reprovava para
        # sempre. Agora ele é descartado e a execução seguinte extrai do zero.
        recipe = base_recipe(self.lib, self.assets)
        recipe["validate"].append(
            {"path": "assets/companion.dat", "type": "file", "min_size": 1}
        )
        self.write_recipe(recipe)
        source = self.data / "whole-set.apk"
        make_zip(source, self.merged_entries())
        stage = self.game / ".nxextract/synthetic-port/stage"

        self.run_cli(expect=1)
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("discarding staged data that failed validation", log)
        self.assertFalse(stage.exists())
        self.assertFalse(
            (self.game / ".nxextract/synthetic-port/state.json").exists()
        )
        # A fonte legal do usuário nunca é apagada, e nada foi publicado.
        self.assertTrue(source.exists())
        self.assertFalse((self.game / "lib/arm64-v8a/libgame.so").exists())

        # Segunda tentativa: reprova de novo (a receita continua exigindo o
        # companheiro), mas EXTRAINDO do zero — sem retomar o stage reprovado.
        (self.game / "test-extract.log").unlink()
        self.run_cli(expect=1)
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertNotIn("resuming", log)

    def test_failed_hook_preserves_previous_live_payload_and_source(self):
        recipe = base_recipe(
            self.lib,
            self.assets,
            hooks=[{"id": "intentional-failure", "argv": ["/bin/false"]}],
        )
        self.write_recipe(recipe)
        old = self.game / "assets/readme.dat"
        old.parent.mkdir(parents=True)
        old.write_bytes(b"old-live-data")
        source = self.data / "hook.apk"
        make_zip(source, self.merged_entries())
        self.run_cli(expect=1)
        self.assertEqual(old.read_bytes(), b"old-live-data")
        self.assertFalse((self.game / "lib/arm64-v8a/libgame.so").exists())
        self.assertTrue(source.exists())
        self.assertTrue(
            (
                self.game
                / ".nxextract/synthetic-port/stage/lib/arm64-v8a/libgame.so"
            ).exists()
        )

    def test_hook_checkpoint_expands_selected_abi(self):
        recipe = base_recipe(
            self.lib,
            self.assets,
            hooks=[
                {
                    "id": "abi-checkpoint",
                    "argv": ["/bin/true"],
                    "checkpoint": [
                        {
                            "path": "lib/{abi}/libgame.so",
                            "type": "file",
                            "sha256": sha256(self.lib),
                            "elf_machine": "arm64-v8a",
                        }
                    ],
                }
            ],
        )
        self.write_recipe(recipe)
        make_zip(self.data / "checkpoint.apk", self.merged_entries())
        self.run_cli()
        self.assert_payload()

    def test_recovery_rolls_back_an_interrupted_publish(self):
        self.write_recipe()
        recipe = NX.Recipe(str(self.recipe_path))
        workspace = self.game / ".nxextract/synthetic-port"
        live = self.game / "assets/readme.dat"
        backup = workspace / "backup/assets/readme.dat"
        cache = workspace / "source-cache/bundle/base.apk"
        live.parent.mkdir(parents=True)
        backup.parent.mkdir(parents=True)
        cache.parent.mkdir(parents=True)
        live.write_bytes(b"new-unpublished-data")
        backup.write_bytes(b"old-live-data")
        cache.write_bytes(b"cached-source")
        NX.atomic_write_json(
            workspace / "transaction.json",
            {
                "format": NX.FORMAT_VERSION,
                "transaction_format": NX.TRANSACTION_FORMAT_VERSION,
                "transaction_id": "1" * 32,
                "recipe_id": recipe.identifier,
                "recipe_digest": recipe.digest,
                "abi": "arm64-v8a",
                "published": False,
                "paths": [
                    {
                        "path": "lib/arm64-v8a/libgame.so",
                        "had_live": False,
                        "phase": "pending",
                    },
                    {
                        "path": "assets",
                        "had_live": True,
                        "phase": "installed",
                    }
                ],
            },
        )

        logger = NX.Logger(None, verbose=False)
        NX.recover_transaction(
            recipe,
            str(self.game),
            str(workspace),
            str(self.game / ".synthetic-data.json"),
            logger,
        )

        self.assertEqual(live.read_bytes(), b"old-live-data")
        self.assertEqual(
            (workspace / "stage/assets/readme.dat").read_bytes(),
            b"new-unpublished-data",
        )
        self.assertEqual(cache.read_bytes(), b"cached-source")
        self.assertFalse((workspace / "backup").exists())
        self.assertFalse((workspace / "transaction.json").exists())

    def test_recovery_finishes_a_marker_published_transaction(self):
        self.write_recipe()
        make_zip(self.data / "published.apk", self.merged_entries())
        self.run_cli()
        recipe = NX.Recipe(str(self.recipe_path))
        workspace = self.game / ".nxextract/synthetic-port"
        marker = self.game / ".synthetic-data.json"
        marker_data = json.loads(marker.read_text(encoding="utf-8"))
        for relative in ("stage/payload", "backup/payload", "source-cache/base.apk"):
            path = workspace / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"temporary")
        transaction_id = marker_data["transaction_id"]
        NX.atomic_write_json(
            workspace / "transaction.json",
            {
                "format": NX.FORMAT_VERSION,
                "transaction_format": NX.TRANSACTION_FORMAT_VERSION,
                "transaction_id": transaction_id,
                "recipe_id": recipe.identifier,
                "recipe_digest": recipe.digest,
                "abi": "arm64-v8a",
                "published": False,
                "paths": [
                    {
                        "path": "lib/arm64-v8a/libgame.so",
                        "had_live": False,
                        "phase": "installed",
                    },
                    {
                        "path": "assets",
                        "had_live": False,
                        "phase": "installed",
                    },
                ],
            },
        )

        logger = NX.Logger(None, verbose=False)
        NX.recover_transaction(
            recipe, str(self.game), str(workspace), str(marker), logger
        )

        self.assertFalse((workspace / "stage").exists())
        self.assertFalse((workspace / "backup").exists())
        self.assertFalse((workspace / "source-cache").exists())
        self.assertFalse((workspace / "transaction.json").exists())
        self.assertTrue(marker.exists())

    def test_every_transaction_boundary_recovers_after_power_loss(self):
        transitions = [
            "journal-created",
            "backup-skipped-0",
            "install-intent-0",
            "install-renamed-0",
            "install-recorded-0",
            "backup-intent-1",
            "backup-renamed-1",
            "backup-recorded-1",
            "install-intent-1",
            "install-renamed-1",
            "install-recorded-1",
            "payload-validated",
            "marker-published",
            "journal-published",
        ]
        published = {"marker-published", "journal-published"}
        for number, transition in enumerate(transitions):
            with self.subTest(transition=transition):
                game = Path(self.temporary.name) / ("power-loss-%02d" % number)
                data = game / "gamedata"
                data.mkdir(parents=True)
                recipe_path = game / "extractor.json"
                recipe_path.write_text(
                    json.dumps(
                        base_recipe(self.lib, self.assets),
                        sort_keys=True,
                        indent=2,
                    ),
                    encoding="utf-8",
                )
                source = data / "payload.apk"
                make_zip(source, self.merged_entries())

                # Path 0 (lib) has no previous live object; path 1 (assets)
                # does. Together they exercise backup-skipped and backed-up.
                old_assets = game / "assets"
                old_assets.mkdir()
                (old_assets / "legacy.dat").write_bytes(b"old-live-payload")

                observed = []

                def interrupt(name, _journal):
                    observed.append(name)
                    if name == transition:
                        raise SimulatedPowerLoss(transition)

                argv = [
                    "install",
                    "--recipe",
                    str(recipe_path),
                    "--game-dir",
                    str(game),
                    "--quiet",
                    "--ui",
                    "none",
                    "--force-source",
                ]
                with mock.patch.object(
                    NX, "_transaction_transition", side_effect=interrupt
                ):
                    with self.assertRaises(SimulatedPowerLoss):
                        NX.main(argv)
                self.assertIn(transition, observed)

                recipe = NX.Recipe(str(recipe_path))
                workspace = game / ".nxextract/synthetic-port"
                logger = NX.Logger(None, verbose=False)
                NX.recover_transaction(
                    recipe,
                    str(game),
                    str(workspace),
                    str(game / ".synthetic-data.json"),
                    logger,
                )

                self.assertFalse((workspace / "transaction.json").exists())
                self.assertFalse((workspace / "backup").exists())
                if transition in published:
                    self.assertEqual(
                        (game / "lib/arm64-v8a/libgame.so").read_bytes(),
                        self.lib,
                    )
                    for relative, payload in self.assets.items():
                        self.assertEqual(
                            (game / "assets" / relative).read_bytes(),
                            payload,
                        )
                    self.assertFalse((game / "assets/legacy.dat").exists())
                    marker = json.loads(
                        (game / ".synthetic-data.json").read_text(encoding="utf-8")
                    )
                    self.assertTrue(NX.marker_matches_recipe(marker, recipe))
                else:
                    self.assertFalse(
                        (game / "lib/arm64-v8a/libgame.so").exists()
                    )
                    self.assertEqual(
                        (game / "assets/legacy.dat").read_bytes(),
                        b"old-live-payload",
                    )
                    self.assertFalse((game / "assets/readme.dat").exists())
                    self.assertFalse((game / ".synthetic-data.json").exists())

    def test_recovery_rolls_back_when_matching_marker_payload_is_corrupt(self):
        self.write_recipe()
        make_zip(self.data / "corrupt-after-marker.apk", self.merged_entries())
        old_assets = self.game / "assets"
        old_assets.mkdir()
        (old_assets / "legacy.dat").write_bytes(b"old-live-payload")

        def interrupt(name, _journal):
            if name == "marker-published":
                raise SimulatedPowerLoss(name)

        argv = [
            "install",
            "--recipe",
            str(self.recipe_path),
            "--game-dir",
            str(self.game),
            "--quiet",
            "--ui",
            "none",
            "--force-source",
        ]
        with mock.patch.object(
            NX, "_transaction_transition", side_effect=interrupt
        ):
            with self.assertRaises(SimulatedPowerLoss):
                NX.main(argv)

        live_library = self.game / "lib/arm64-v8a/libgame.so"
        damaged = bytearray(live_library.read_bytes())
        damaged[-1] ^= 0xFF
        live_library.write_bytes(damaged)

        recipe = NX.Recipe(str(self.recipe_path))
        workspace = self.game / ".nxextract/synthetic-port"
        NX.recover_transaction(
            recipe,
            str(self.game),
            str(workspace),
            str(self.game / ".synthetic-data.json"),
            NX.Logger(None, verbose=False),
        )

        self.assertFalse(live_library.exists())
        self.assertEqual(
            (self.game / "assets/legacy.dat").read_bytes(),
            b"old-live-payload",
        )
        self.assertFalse((workspace / "transaction.json").exists())

    def test_zip_slip_is_rejected_before_writing_payload(self):
        self.write_recipe()
        entries = self.merged_entries()
        entries["assets/../../escaped"] = b"bad"
        make_zip(self.data / "unsafe.apk", entries)
        self.run_cli(expect=1)
        self.assertFalse((self.game / "escaped").exists())
        self.assertFalse((Path(self.temporary.name) / "escaped").exists())

    def test_casefold_destination_collision_is_rejected(self):
        collision_assets = {
            "Foo.bin": b"first",
            "foo.bin": b"second",
        }
        self.assets = collision_assets
        self.write_recipe(base_recipe(self.lib, collision_assets))
        make_zip(self.data / "collision.apk", self.merged_entries())
        self.run_cli(expect=1)
        self.assertFalse((self.game / "assets").exists())

    def test_workspace_symlink_is_rejected(self):
        self.write_recipe()
        outside = Path(self.temporary.name) / "outside-workspace"
        outside.mkdir()
        (self.game / ".nxextract").symlink_to(outside, target_is_directory=True)
        make_zip(self.data / "payload.apk", self.merged_entries())
        self.run_cli(expect=1)
        self.assertEqual(list(outside.iterdir()), [])
        self.assertFalse((self.game / "assets").exists())

    def test_log_symlink_and_hardlink_never_touch_their_target(self):
        self.write_recipe()
        outside = Path(self.temporary.name) / "outside-log"
        outside.write_bytes(b"sentinel-log")
        log = self.game / "test-extract.log"

        log.symlink_to(outside)
        self.run_cli(expect=1)
        self.assertEqual(outside.read_bytes(), b"sentinel-log")
        log.unlink()

        os.link(outside, log)
        self.run_cli(expect=1)
        self.assertEqual(outside.read_bytes(), b"sentinel-log")
        log.unlink()

        result = self.game / "nxextract-result.json"
        result.symlink_to(outside)
        self.run_cli(expect=1)
        self.assertEqual(outside.read_bytes(), b"sentinel-log")
        result.unlink()

        os.link(outside, result)
        self.run_cli(expect=1)
        self.assertEqual(outside.read_bytes(), b"sentinel-log")

    def test_lock_symlink_and_hardlink_never_touch_their_target(self):
        self.write_recipe()
        make_zip(self.data / "payload.apk", self.merged_entries())
        workspace = self.game / ".nxextract/synthetic-port"
        workspace.mkdir(parents=True)
        lock = workspace / "install.lock"
        outside = Path(self.temporary.name) / "outside-lock"
        outside.write_bytes(b"sentinel-lock")

        lock.symlink_to(outside)
        self.run_cli(expect=1)
        self.assertEqual(outside.read_bytes(), b"sentinel-lock")
        lock.unlink()

        os.link(outside, lock)
        self.run_cli(expect=1)
        self.assertEqual(outside.read_bytes(), b"sentinel-lock")

    def test_internal_source_cache_symlink_is_rejected(self):
        self.write_recipe()
        make_zip(self.data / "payload.apk", self.merged_entries())
        workspace = self.game / ".nxextract/synthetic-port"
        workspace.mkdir(parents=True)
        outside = Path(self.temporary.name) / "outside-cache"
        outside.mkdir()
        (workspace / "source-cache").symlink_to(
            outside, target_is_directory=True
        )

        self.run_cli(expect=1)

        self.assertEqual(list(outside.iterdir()), [])
        self.assertFalse((self.game / "assets").exists())

    def test_internal_stage_symlink_is_rejected(self):
        self.write_recipe()
        make_zip(self.data / "payload.apk", self.merged_entries())
        workspace = self.game / ".nxextract/synthetic-port"
        workspace.mkdir(parents=True)
        outside = Path(self.temporary.name) / "outside-stage"
        outside.mkdir()
        (workspace / "stage").symlink_to(outside, target_is_directory=True)

        self.run_cli(expect=1)

        self.assertEqual(list(outside.iterdir()), [])
        self.assertFalse((self.game / "assets").exists())

    def test_nested_stage_symlink_is_rejected_before_preflight_or_copy(self):
        self.write_recipe()
        make_zip(self.data / "payload.apk", self.merged_entries())
        workspace = self.game / ".nxextract/synthetic-port"
        stage = workspace / "stage"
        stage.mkdir(parents=True)
        outside = Path(self.temporary.name) / "outside-nested-stage"
        outside.mkdir()
        (stage / "lib").symlink_to(outside, target_is_directory=True)

        self.run_cli(expect=1)

        self.assertEqual(list(outside.iterdir()), [])
        self.assertFalse((self.game / "assets").exists())

    def test_hardlinked_existing_payload_is_not_adopted(self):
        self.write_recipe()
        outside = Path(self.temporary.name) / "outside-hardlink-payload"
        outside.write_bytes(self.lib)
        destination = self.game / "lib/arm64-v8a/libgame.so"
        destination.parent.mkdir(parents=True)
        os.link(outside, destination)

        self.run_cli(expect=1)

        self.assertEqual(outside.read_bytes(), self.lib)
        self.assertFalse((self.game / ".synthetic-data.json").exists())

    def test_internal_hook_checkpoint_symlink_is_rejected(self):
        recipe = base_recipe(
            self.lib,
            self.assets,
            hooks=[{"id": "safe-noop", "argv": ["/bin/true"]}],
        )
        self.write_recipe(recipe)
        make_zip(self.data / "payload.apk", self.merged_entries())
        workspace = self.game / ".nxextract/synthetic-port"
        workspace.mkdir(parents=True)
        outside = Path(self.temporary.name) / "outside-hooks"
        outside.mkdir()
        (workspace / "hooks").symlink_to(outside, target_is_directory=True)

        self.run_cli(expect=1)

        self.assertEqual(list(outside.iterdir()), [])
        self.assertFalse((self.game / "assets").exists())

    def test_progress_file_must_remain_inside_private_workspace(self):
        self.write_recipe()
        outside = Path(self.temporary.name) / "outside-progress"
        outside.write_bytes(b"sentinel-progress")

        self.run_cli(
            expect=1,
            extra=["--progress-file", str(outside)],
        )

        self.assertEqual(outside.read_bytes(), b"sentinel-progress")

    def test_recipe_and_explicit_input_symlinks_are_rejected(self):
        outside_recipe = Path(self.temporary.name) / "outside-recipe.json"
        outside_recipe.write_text(
            json.dumps(base_recipe(self.lib, self.assets)),
            encoding="utf-8",
        )
        self.recipe_path.symlink_to(outside_recipe)
        self.run_cli(expect=1)
        self.assertFalse((self.game / "assets").exists())

        self.recipe_path.unlink()
        os.link(outside_recipe, self.recipe_path)
        self.run_cli(expect=1)
        self.assertFalse((self.game / "assets").exists())

        self.recipe_path.unlink()
        self.write_recipe()
        outside_apk = Path(self.temporary.name) / "outside.apk"
        make_zip(outside_apk, self.merged_entries())
        linked_apk = self.data / "linked.apk"
        linked_apk.symlink_to(outside_apk)
        self.run_cli(inputs=[linked_apk], expect=1)
        self.assertTrue(outside_apk.exists())
        self.assertFalse((self.game / "assets").exists())

    def test_recipe_inside_symlinked_parent_is_rejected_before_reading_it(self):
        outside = Path(self.temporary.name) / "outside-recipe-parent"
        outside.mkdir()
        (outside / "extractor.json").write_text("not-json\n", encoding="utf-8")
        (self.game / "linked-config").symlink_to(
            outside, target_is_directory=True
        )
        self.recipe_path = self.game / "linked-config/extractor.json"

        result = self.run_cli(expect=1)

        self.assertIn("unsafe non-directory parent", result.stderr)
        self.assertFalse((self.game / "test-extract.log").exists())
        self.assertFalse((self.game / ".nxextract").exists())

    def test_linked_transaction_journal_is_rejected_without_reading_target(self):
        self.write_recipe()
        workspace = self.game / ".nxextract/synthetic-port"
        workspace.mkdir(parents=True)
        journal = workspace / "transaction.json"
        outside = Path(self.temporary.name) / "outside-journal"
        outside.write_text('{"hostile":true}\n', encoding="utf-8")

        journal.symlink_to(outside)
        self.run_cli(expect=1)
        self.assertEqual(
            outside.read_text(encoding="utf-8"),
            '{"hostile":true}\n',
        )
        journal.unlink()

        os.link(outside, journal)
        self.run_cli(expect=1)
        self.assertEqual(
            outside.read_text(encoding="utf-8"),
            '{"hostile":true}\n',
        )

    def test_zip_symbolic_link_member_is_rejected(self):
        assets = dict(self.assets, link=b"outside")
        self.write_recipe(base_recipe(self.lib, assets))
        source = self.data / "symlink-member.apk"
        source.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(
            source, "w", compression=zipfile.ZIP_DEFLATED
        ) as archive:
            for name, payload in self.merged_entries().items():
                archive.writestr(name, payload)
            link = zipfile.ZipInfo("assets/link")
            link.create_system = 3
            link.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(link, b"outside")

        self.run_cli(expect=1)

        self.assertFalse((self.game / "assets").exists())

    def test_unselected_special_zip_member_is_rejected(self):
        self.write_recipe()
        source = self.data / "unselected-special.apk"
        with zipfile.ZipFile(
            source, "w", compression=zipfile.ZIP_DEFLATED
        ) as archive:
            for name, payload in self.merged_entries().items():
                archive.writestr(name, payload)
            link = zipfile.ZipInfo("not-selected-by-recipe")
            link.create_system = 3
            link.external_attr = (stat.S_IFLNK | 0o777) << 16
            archive.writestr(link, b"outside")

        self.run_cli(expect=1)

        self.assertFalse((self.game / "assets").exists())

    def test_unselected_zip_case_collision_is_accepted(self):
        """A collision outside the selected members must not reject the APK.

        Real APKs carry obfuscated resources differing only in case; refusing
        the archive rejected legitimate builds whose colliding members are
        never written to the card.
        """
        self.write_recipe()
        entries = self.merged_entries()
        entries["Assets/unselected.bin"] = b"collision"
        make_zip(self.data / "directory-collision.apk", entries)

        self.run_cli()

        self.assertTrue((self.game / "assets").is_dir())

    def test_selected_zip_case_collision_is_rejected(self):
        """Two SELECTED members collapsing to one portable path still fail."""
        self.write_recipe()
        entries = self.merged_entries()
        entries["assets/Collide.bin"] = b"one"
        entries["assets/collide.bin"] = b"two"
        make_zip(self.data / "selected-collision.apk", entries)

        self.run_cli(expect=1)

        self.assertFalse((self.game / "assets").exists())

    def test_unicode_normalization_destination_collision_is_rejected(self):
        collision_assets = {
            "\u00e9.bin": b"first",
            "e\u0301.bin": b"second",
        }
        self.assets = collision_assets
        self.write_recipe(base_recipe(self.lib, collision_assets))
        make_zip(self.data / "unicode-collision.apk", self.merged_entries())

        self.run_cli(expect=1)

        self.assertFalse((self.game / "assets").exists())

    def test_recipe_rejects_log_path_escape(self):
        recipe = base_recipe(self.lib, self.assets)
        recipe["log"] = "../escaped.log"
        self.write_recipe(recipe)
        make_zip(self.data / "payload.apk", self.merged_entries())
        self.run_cli(expect=1)
        self.assertFalse((Path(self.temporary.name) / "escaped.log").exists())

    def test_different_packages_are_never_merged(self):
        self.write_recipe()
        base_entries = {
            "AndroidManifest.xml": plain_manifest("org.nextos.one"),
            **{"assets/" + key: value for key, value in self.assets.items()},
        }
        make_zip(self.data / "one.apk", base_entries)
        make_zip(
            self.data / "two.apk",
            {
                "AndroidManifest.xml": plain_manifest(
                    "org.nextos.two", "config.arm64_v8a"
                ),
                "lib/arm64-v8a/libgame.so": self.lib,
            },
        )
        self.run_cli(expect=1)
        self.assertFalse((self.game / "assets").exists())

    def test_two_different_matching_bundles_are_ambiguous(self):
        recipe = base_recipe(self.lib, self.assets)
        recipe["extract"][0]["validate"].pop("sha256")
        recipe["extract"][0]["validate"].pop("size")
        recipe["validate"] = []
        self.write_recipe(recipe)
        bundles = []
        for number in (1, 2):
            changed_lib = self.lib + bytes((number,))
            inner = zip_bytes(
                {
                    "AndroidManifest.xml": plain_manifest(
                        "org.nextos.synthetic%d" % number
                    ),
                    "lib/arm64-v8a/libgame.so": changed_lib,
                    **{
                        "assets/" + key: value
                        for key, value in self.assets.items()
                    },
                }
            )
            bundle = self.data / ("bundle%d.apkm" % number)
            bundles.append(bundle)
            make_zip(
                bundle,
                {"payload%d.apk" % number: inner},
            )
        self.run_cli(expect=1)

        # An explicit legal input resolves the ambiguity without relying on
        # external filenames or deleting either user package.
        self.run_cli(inputs=[bundles[0]])
        self.assertEqual(
            (self.game / "lib/arm64-v8a/libgame.so").read_bytes(),
            self.lib + b"\x01",
        )
        self.assertTrue(all(path.exists() for path in bundles))

    def test_progress_protocol_is_atomic_and_parseable(self):
        target = Path(self.temporary.name) / "progress.txt"
        result = subprocess.run(
            [
                sys.executable,
                str(NXEXTRACT),
                "progress",
                "--file",
                str(target),
                "--phase",
                "5",
                "--overall",
                "700",
                "--phase-progress",
                "321",
                "--done-bytes",
                "123",
                "--total-bytes",
                "456",
                "--message",
                "BAKING TEXTURES",
                "--detail",
                "texture 7 / 20",
            ],
            check=False,
        )
        self.assertEqual(result.returncode, 0)
        lines = target.read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines[0], "1 700 1000")
        self.assertEqual(lines[1], "BAKING TEXTURES")
        self.assertEqual(lines[2], "NXEXTRACT_V1 5 700 321 123 456")
        self.assertEqual(lines[3], "texture 7 / 20")

    def test_terminal_result_covers_install_fast_path_and_adoption(self):
        self.write_recipe()
        source = self.data / "private-distributor-secret.apk"
        make_zip(source, self.merged_entries())

        self.run_cli()
        result_path = self.game / "nxextract-result.json"
        result = json.loads(result_path.read_text(encoding="utf-8"))
        schema = json.loads(
            (ROOT / "docs/terminal-result-schema-v1.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertFalse(schema["additionalProperties"])
        self.assertEqual(schema["properties"]["schema_version"]["const"], 1)
        self.assertEqual(set(result), set(schema["required"]))
        self.assertEqual(result["schema"], NX.TERMINAL_RESULT_SCHEMA)
        self.assertEqual(result["schema_version"], 1)
        self.assertEqual(result["nxextract_version"], "1.3.0")
        self.assertEqual(result["outcome"], "success")
        self.assertEqual(result["code"], "NXE0000")
        self.assertEqual(result["final_phase"]["id"], "ready")
        self.assertEqual(result["recipe"]["id"], "synthetic-port")
        self.assertEqual(result["recipe"]["version"], "test-1")
        self.assertEqual(result["package_id"], "org.nextos.synthetic")
        self.assertEqual(result["abi"], "arm64-v8a")
        self.assertEqual(result["container"]["kind"], "apk-set")
        self.assertRegex(result["container"]["identity"], r"^[0-9a-f]{64}$")
        self.assertEqual(result["validated"]["items"], 3)
        self.assertEqual(
            result["validated"]["bytes"],
            len(self.lib) + sum(len(value) for value in self.assets.values()),
        )
        self.assertEqual(
            [item["id"] for item in result["validated"]["critical_payloads"]],
            ["native", "assets"],
        )
        self.assertEqual(result["logs"]["summary"], "test-extract.log")
        self.assertEqual(result["logs"]["detail"], "nxextract-detail.log")
        serialized = json.dumps(result, sort_keys=True)
        self.assertNotIn(source.name, serialized)
        self.assertNotIn(str(source), serialized)
        self.assertFalse(list(self.game.glob(".nxextract-result.json.tmp.*")))
        compact_log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        detail_log = (self.game / "nxextract-detail.log").read_text(encoding="utf-8")
        self.assertNotIn("extracted lib/arm64-v8a/libgame.so", compact_log)
        self.assertIn("extracted lib/arm64-v8a/libgame.so", detail_log)

        source.unlink()
        self.run_cli()
        fast = json.loads(result_path.read_text(encoding="utf-8"))
        self.assertEqual(fast["code"], "NXE0001")
        self.assertEqual(fast["validated"], result["validated"])
        self.assertEqual(fast["container"], result["container"])

        (self.game / ".synthetic-data.json").unlink()
        self.run_cli()
        adopted = json.loads(result_path.read_text(encoding="utf-8"))
        self.assertEqual(adopted["code"], "NXE0002")
        self.assertEqual(adopted["container"]["kind"], "existing")
        self.assertRegex(adopted["container"]["identity"], r"^[0-9a-f]{64}$")
        self.assertEqual(adopted["validated"]["items"], 3)

    def test_terminal_error_is_stable_compact_and_sanitized(self):
        self.write_recipe()
        hidden_name = "private-distributor-release.apks"
        make_zip(
            self.data / hidden_name,
            {
                "payload.apk": zip_bytes(
                    {
                        "AndroidManifest.xml": plain_manifest("org.wrong.game"),
                        "unrelated.bin": b"wrong",
                    }
                )
            },
        )
        self.run_cli(expect=1)

        result = json.loads(
            (self.game / "nxextract-result.json").read_text(encoding="utf-8")
        )
        self.assertEqual(result["outcome"], "error")
        self.assertIn(result["code"], {"NXE2001", "NXE3001"})
        self.assertIn(result["final_phase"]["id"], NX.PHASE_IDS)
        self.assertEqual(result["validated"]["items"], 0)
        self.assertEqual(result["validated"]["bytes"], 0)
        self.assertIsInstance(result["error"]["class"], str)
        self.assertTrue(result["error"]["message"])
        serialized = json.dumps(result, sort_keys=True)
        self.assertNotIn(hidden_name, serialized)
        self.assertNotIn(str(self.game), serialized)
        compact = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertRegex(compact, r"TERMINAL ERROR NXE[0-9]{4}:")

    def test_os_error_code_separates_the_real_field_causes(self):
        """P13: NXE6001 deixa de esconder cartao cheio, permissao e midia.

        A TELA nao muda: o codigo ocupa o mesmo lugar, mantem o formato
        NXE#### que o launcher publicado ja valida, e nenhuma categoria nova
        de resultado aparece. O que muda e o suporte distinguir a causa sem
        pedir log.
        """
        cases = {
            errno.ENOSPC: "NXE6002",
            errno.EDQUOT: "NXE6002",
            errno.EFBIG: "NXE6002",
            errno.EACCES: "NXE6003",
            errno.EPERM: "NXE6003",
            errno.EROFS: "NXE6003",
            errno.ENOENT: "NXE6004",
            errno.ENOTDIR: "NXE6004",
            errno.ENAMETOOLONG: "NXE6004",
            errno.EIO: "NXE6005",
            errno.ENODEV: "NXE6005",
            errno.ENXIO: "NXE6005",
            errno.ESTALE: "NXE6005",
        }
        for number, expected in cases.items():
            self.assertEqual(
                NX.stable_error_code(OSError(number, os.strerror(number))),
                expected,
                "errno %d" % number,
            )
        # Sem errno conhecido, o codigo historico continua valendo.
        self.assertEqual(
            NX.stable_error_code(OSError(errno.EBUSY, "busy")), "NXE6001")
        self.assertEqual(NX.stable_error_code(OSError("no errno")), "NXE6001")
        # As outras familias nao se mexem.
        self.assertEqual(NX.stable_error_code(NX.RecipeError("x")), "NXE1001")
        self.assertEqual(NX.stable_error_code(NX.PlanError("x")), "NXE3001")
        self.assertEqual(NX.stable_error_code(NX.ValidationError("x")),
                         "NXE4001")
        self.assertEqual(NX.stable_error_code(NX.NXError("x")), "NXE7001")
        self.assertEqual(NX.stable_error_code(ValueError("x")), "NXE9001")
        # Todo codigo continua no formato que o launcher publicado valida.
        for code in {value for value in cases.values()} | {"NXE6001"}:
            self.assertRegex(code, r"^NXE[0-9]{4}$")

    def test_terminal_result_atomic_failure_preserves_previous_document(self):
        target = Path(self.temporary.name) / "result.json"
        previous = {"schema": "previous", "complete": True}
        target.write_text(json.dumps(previous) + "\n", encoding="utf-8")
        replacement = {"schema": NX.TERMINAL_RESULT_SCHEMA, "complete": True}
        with mock.patch.object(
            NX.os, "replace", side_effect=OSError(errno.EIO, "simulated rename loss")
        ):
            with self.assertRaises(OSError):
                NX.publish_terminal_result(str(target), replacement)
        self.assertEqual(json.loads(target.read_text(encoding="utf-8")), previous)
        self.assertFalse(list(target.parent.glob(".result.json.tmp.*")))

    def test_compact_log_suppresses_file_chatter_but_never_terminal_cause(self):
        compact = Path(self.temporary.name) / "compact.log"
        detail = Path(self.temporary.name) / "detail.log"
        started = time.monotonic()
        logger = NX.Logger(
            str(compact),
            detail_path=str(detail),
            verbose=False,
        )
        for index in range(2000):
            logger.detail("extracted private-file-%04d -> payload" % index)
        for index in range(8):
            logger.miss("candidate-selection", "candidate miss %d" % index)
        logger.terminal("TERMINAL ERROR NXE3001: no compatible payload")
        logger.close()
        elapsed = time.monotonic() - started

        compact_text = compact.read_text(encoding="utf-8")
        detail_text = detail.read_text(encoding="utf-8")
        self.assertNotIn("private-file-0000", compact_text)
        self.assertIn("private-file-0000", detail_text)
        self.assertIn("private-file-1999", detail_text)
        self.assertEqual(compact_text.count("candidate miss"), 1)
        self.assertIn("suppressed 7 repeated candidate-selection miss(es)", compact_text)
        self.assertIn("TERMINAL ERROR NXE3001", compact_text)
        self.assertGreater(detail.stat().st_size, compact.stat().st_size * 100)
        self.assertLess(elapsed, 2.0)

        verbose_compact = Path(self.temporary.name) / "verbose-compact.log"
        verbose_detail = Path(self.temporary.name) / "verbose-detail.log"
        logger = NX.Logger(
            str(verbose_compact),
            detail_path=str(verbose_detail),
            verbose=False,
            verbose_detail=True,
        )
        logger.detail("per-file verbose opt-in")
        logger.close()
        self.assertIn(
            "per-file verbose opt-in",
            verbose_compact.read_text(encoding="utf-8"),
        )

    def test_terminal_message_removes_container_origin_and_host_path(self):
        message = (
            "bad /home/user/private-source.apk from "
            "https://downloads.invalid/private-source.apk"
        )
        sanitized = NX.sanitize_terminal_message(message)
        self.assertNotIn("private-source.apk", sanitized)
        self.assertNotIn("/home/user", sanitized)
        self.assertNotIn("downloads.invalid", sanitized)
        self.assertIn("<container>", sanitized)

    def test_every_terminal_error_sink_is_sanitized(self):
        # 23/08/2026: a excecao CRUA ia para a tela de setup (progress.fail) e
        # para a linha TERMINAL ERROR do log que o jogador publica -- so' o
        # JSON passava pela redacao. Os DOIS sinks tem de usar o mesmo filtro.
        source = (Path(NX.__file__)).read_text(encoding="utf-8")
        raw_fail = source.count("progress.fail(str(error)")
        self.assertEqual(
            raw_fail, 0,
            "progress.fail must sanitize the exception before display")
        raw_terminal = source.count('"TERMINAL ERROR %s: %s" % (code, error)')
        self.assertEqual(
            raw_terminal, 0,
            "logger.terminal must sanitize the exception before logging")
        self.assertGreaterEqual(
            source.count("sanitize_terminal_message(error)"), 4,
            "both except blocks must route fail+terminal through the filter")

    def test_missing_game_container_message_names_gamedata_and_extensions(self):
        # Erro numero 1 do usuario BYO: a mensagem antiga nao dizia ONDE por o
        # arquivo nem o que e' aceito.
        source = (Path(NX.__file__)).read_text(encoding="utf-8")
        self.assertIn("gamedata/ folder", source)
        self.assertIn(".apk .apkm .apks .xapk .zip .obb", source)

    def test_required_ui_accepts_private_sdl_readiness_proof(self):
        self.write_recipe()
        make_zip(self.data / "required-ui.apk", self.merged_entries())
        ui = Path(self.temporary.name) / "visible-ui"
        write_channel_ui(ui, "publish()\nwait_stop()\n")

        self.run_cli(extra=["--require-ui", "--ui", str(ui)])

        self.assert_payload()
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("mandatory setup UI graphical renderer confirmed: sdl", log)
        result = json.loads(
            (self.game / "nxextract-result.json").read_text(encoding="utf-8")
        )
        self.assertEqual(
            result["ui"],
            {"mode": "visible", "renderer": "sdl", "fallback_reason": None},
        )

    def test_required_ui_accepts_private_fbdev_readiness_proof(self):
        self.write_recipe()
        make_zip(self.data / "required-fbdev-ui.apk", self.merged_entries())
        ui = Path(self.temporary.name) / "visible-fbdev-ui"
        write_channel_ui(ui, "publish(b'visible=fbdev\\n')\nwait_stop()\n")

        self.run_cli(extra=["--require-ui", "--ui", str(ui)])

        self.assert_payload()
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("mandatory setup UI graphical renderer confirmed: fbdev", log)
        result = json.loads(
            (self.game / "nxextract-result.json").read_text(encoding="utf-8")
        )
        self.assertEqual(
            result["ui"],
            {"mode": "visible", "renderer": "fbdev", "fallback_reason": None},
        )

    def test_required_ui_channel_survives_chmod_insensitive_game_tree(self):
        # P1: em /roms FAT/exFAT nao ha dono nem chmod. O canal da sessao vive
        # em descritores herdados, entao nenhum ui.ready/ui.stop pode existir
        # em disco — nem no jogo, nem no XDG, nem em /tmp.
        self.write_recipe()
        source = self.data / "fat-mode-ui.apk"
        make_zip(source, self.merged_entries())
        workspace = self.game / ".nxextract/synthetic-port"
        workspace.mkdir(parents=True)
        for path in (self.game, self.data, self.recipe_path, source, workspace.parent, workspace):
            path.chmod(0o777)

        runtime_base = Path(self.temporary.name) / "private-runtime"
        runtime_base.mkdir(mode=0o700)
        trace = Path(self.temporary.name) / "ui-runtime-trace.json"
        ui = Path(self.temporary.name) / "fat-mode-ui"
        write_channel_ui(
            ui,
            "import json, stat\n"
            "publish()\n"
            "wait_stop()\n"
            "record = {\n"
            "    'cwd': os.getcwd(),\n"
            "    'stop': sys.argv[2],\n"
            "    'ready': sys.argv[3],\n"
            "    'stop_is_fifo': stat.S_ISFIFO(os.fstat(stop_fd).st_mode),\n"
            "}\n"
            "with open(%r, 'w', encoding='utf-8') as stream:\n"
            "    json.dump(record, stream)\n" % str(trace),
        )

        self.run_cli(
            extra=["--require-ui", "--ui", str(ui)],
            env={
                "XDG_RUNTIME_DIR": str(self.game),
                "TMPDIR": str(runtime_base),
            },
        )

        self.assert_payload()
        record = json.loads(trace.read_text(encoding="utf-8"))
        self.assertTrue(record["stop"].startswith("fd:"))
        self.assertTrue(record["ready"].startswith("fd:"))
        self.assertTrue(record["stop_is_fifo"])
        self.assertEqual(Path(record["cwd"]), workspace)
        self.assertEqual(list(runtime_base.iterdir()), [])
        self.assertFalse((workspace / "ui.ready").exists())
        self.assertFalse((workspace / "ui.stop").exists())
        self.assertFalse((self.game / "ui.ready").exists())
        self.assertFalse((self.game / "ui.stop").exists())
        self.assertTrue((workspace / "ui.log").is_file())

    def test_required_ui_ignores_adversarial_runtime_bases(self):
        # P1: XDG symlink para diretorio 0777 e TMPDIR 0777 sem sticky eram
        # bases recusadas; agora sao simplesmente irrelevantes — o handshake
        # nao cria pathname algum e a extracao completa visivel.
        self.write_recipe()
        make_zip(self.data / "unsafe-runtime-ui.apk", self.merged_entries())
        unsafe_runtime = Path(self.temporary.name) / "unsafe-runtime"
        unsafe_runtime.mkdir(mode=0o777)
        unsafe_runtime.chmod(0o777)
        linked_runtime = Path(self.temporary.name) / "linked-runtime"
        linked_runtime.symlink_to(unsafe_runtime, target_is_directory=True)
        ui = Path(self.temporary.name) / "unsafe-runtime-ui"
        write_channel_ui(ui, "publish()\nwait_stop()\n")

        self.run_cli(
            extra=["--require-ui", "--ui", str(ui)],
            env={
                "XDG_RUNTIME_DIR": str(linked_runtime),
                "TMPDIR": str(unsafe_runtime),
            },
        )

        self.assert_payload()
        result = json.loads(
            (self.game / "nxextract-result.json").read_text(encoding="utf-8")
        )
        self.assertEqual(result["ui"]["mode"], "visible")
        self.assertEqual(list(unsafe_runtime.iterdir()), [])

    def test_private_ui_session_channels_fail_closed(self):
        # P1 item 3: dono, tipo, modo e identidade dev/ino validados por
        # fstat; qualquer divergencia e' falha fechada.
        descriptors, identities = NX.create_private_ui_session_channels()
        try:
            for name, descriptor in descriptors.items():
                self.assertEqual(
                    NX._assert_private_session_descriptor(
                        descriptor, identities[name]
                    ),
                    identities[name],
                )

            regular = os.open(
                str(Path(self.temporary.name) / "not-a-pipe"),
                os.O_CREAT | os.O_RDWR,
                0o600,
            )
            try:
                with self.assertRaisesRegex(
                    NX.NXError, "not a private pipe"
                ):
                    NX._assert_private_session_descriptor(regular)
            finally:
                os.close(regular)

            foreign_read, foreign_write = os.pipe()
            try:
                with self.assertRaisesRegex(NX.NXError, "was replaced"):
                    NX._assert_private_session_descriptor(
                        foreign_read, identities["ready_read"]
                    )
            finally:
                os.close(foreign_read)
                os.close(foreign_write)

            genuine = os.fstat(descriptors["ready_read"])
            hostile_owner = os.stat_result(
                (
                    genuine.st_mode,
                    genuine.st_ino,
                    genuine.st_dev,
                    genuine.st_nlink,
                    genuine.st_uid + 1,
                    genuine.st_gid,
                    genuine.st_size,
                    genuine.st_atime,
                    genuine.st_mtime,
                    genuine.st_ctime,
                )
            )
            with mock.patch.object(NX.os, "fstat", return_value=hostile_owner):
                with self.assertRaisesRegex(
                    NX.NXError, "not owned by this user"
                ):
                    NX._assert_private_session_descriptor(
                        descriptors["ready_read"]
                    )
            hostile_mode = os.stat_result(
                (
                    genuine.st_mode | 0o066,
                    genuine.st_ino,
                    genuine.st_dev,
                    genuine.st_nlink,
                    genuine.st_uid,
                    genuine.st_gid,
                    genuine.st_size,
                    genuine.st_atime,
                    genuine.st_mtime,
                    genuine.st_ctime,
                )
            )
            with mock.patch.object(NX.os, "fstat", return_value=hostile_mode):
                with self.assertRaisesRegex(NX.NXError, "mode is not private"):
                    NX._assert_private_session_descriptor(
                        descriptors["ready_read"]
                    )
        finally:
            for descriptor in descriptors.values():
                try:
                    os.close(descriptor)
                except OSError:
                    pass

    def test_session_channel_rejects_substituted_descriptor(self):
        # P1 item 6 (negativo): dup2 de outro objeto sobre o mesmo numero de
        # descritor muda a identidade dev/ino e e' recusado pelo guard.
        recipe = mock.Mock(title="Fixture", version="1")
        workspace = Path(self.temporary.name) / "swap-workspace"
        workspace.mkdir()
        session = NX.UISession(
            "none",
            True,
            str(ROOT),
            str(workspace),
            str(Path(self.temporary.name) / "swap-progress"),
            recipe,
            mock.Mock(),
        )
        session._prepare_session_channels()
        try:
            session._assert_session_channels()
            foreign_read, foreign_write = os.pipe()
            try:
                os.dup2(foreign_read, session.channel["ready_read"])
                with self.assertRaisesRegex(NX.NXError, "was replaced"):
                    session._assert_session_channels()
            finally:
                os.close(foreign_read)
                os.close(foreign_write)
        finally:
            session._cleanup_session_channels()
            session._cleanup_session_channels()  # idempotente (item 5)
            self.assertIsNone(session.channel)

    def test_long_extraction_survives_runtime_directory_recycling(self):
        # P1 itens 1/4/6: a fixture longa apaga o XDG_RUNTIME_DIR no meio da
        # copia — exatamente o Linger=no do dArkOSRE — e a instalacao deve
        # terminar NXE0000 com a UI reapada e zero residuo. Antes do canal por
        # descritores este cenario morria com NXE7001 (reproduzido em campo
        # no Brotato e em fixture nesta worktree).
        self.assets = {
            "levels/pack%02d.bin" % index: (b"%04d" % index) * (256 * 1024 // 4)
            for index in range(24)
        }
        self.write_recipe()
        entries = self.merged_entries()
        make_zip(self.data / "long-copy.apk", entries)

        xdg = Path(self.temporary.name) / "run-user-1000"
        xdg.mkdir(mode=0o700)
        trace = Path(self.temporary.name) / "recycle-trace.json"
        ui = Path(self.temporary.name) / "recycling-ui"
        write_channel_ui(
            ui,
            "import json, shutil\n"
            "publish()\n"
            "session = %r\n"
            "recycled = False\n"
            "deadline = time.monotonic() + 60\n"
            "stopped = False\n"
            "while time.monotonic() < deadline:\n"
            "    if not recycled:\n"
            "        try:\n"
            "            with open(progress_path, 'r') as stream:\n"
            "                text = stream.read()\n"
            "        except OSError:\n"
            "            text = ''\n"
            "        if 'EXTRACTING GAME DATA' in text:\n"
            "            shutil.rmtree(session, ignore_errors=True)\n"
            "            recycled = True\n"
            "    readable, _, _ = select.select([stop_fd], [], [], 0.01)\n"
            "    if readable:\n"
            "        stopped = True\n"
            "        break\n"
            "with open(%r, 'w', encoding='utf-8') as stream:\n"
            "    json.dump({'recycled': recycled, 'stopped': stopped}, stream)\n"
            % (str(xdg), str(trace)),
        )

        self.run_cli(
            extra=["--require-ui", "--ui", str(ui)],
            env={"XDG_RUNTIME_DIR": str(xdg)},
        )

        self.assert_payload()
        record = json.loads(trace.read_text(encoding="utf-8"))
        self.assertTrue(record["recycled"], "a fixture precisa apagar a sessao")
        self.assertTrue(record["stopped"], "a UI deve receber o stop e ser reapada")
        result = json.loads(
            (self.game / "nxextract-result.json").read_text(encoding="utf-8")
        )
        self.assertEqual(result["code"], "NXE0000")
        self.assertEqual(result["ui"]["mode"], "visible")
        self.assertFalse(xdg.exists())
        workspace = self.game / ".nxextract/synthetic-port"
        self.assertFalse((workspace / "ui.ready").exists())
        self.assertFalse((workspace / "ui.stop").exists())
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertNotIn("identity was lost", log)

    def test_required_ui_rejects_tty_proof_before_extraction(self):
        # P11.7: uma prova nao-aprovada (tty) segue REJEITADA como renderer,
        # mas a instalacao do usuario agora cai para HEADLESS e completa; o
        # gate de QA restaura o fail-closed com NXEXTRACT_REQUIRE_VISIBLE_UI=1.
        self.write_recipe()
        make_zip(self.data / "tty-ui.apk", self.merged_entries())
        ui = Path(self.temporary.name) / "tty-ui"
        write_channel_ui(ui, "publish(b'visible=tty\\n')\nwait_stop()\n")

        self.run_cli(
            expect=1,
            extra=["--require-ui", "--ui", str(ui)],
            env={"NXEXTRACT_REQUIRE_VISIBLE_UI": "1"},
        )
        self.assertFalse((self.game / "lib/arm64-v8a/libgame.so").exists())
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("did not attest an approved graphical renderer", log)
        self.assertIn("visible=tty", log)

        self.run_cli(extra=["--require-ui", "--ui", str(ui)])
        self.assertTrue((self.game / "lib/arm64-v8a/libgame.so").exists())
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("did not attest an approved graphical renderer", log)
        self.assertIn("continuing HEADLESS", log)
        result = json.loads(
            (self.game / "nxextract-result.json").read_text(encoding="utf-8")
        )
        self.assertEqual(result["ui"]["mode"], "headless-fallback")
        self.assertIn("did not attest", result["ui"]["fallback_reason"])

    def test_required_ui_fails_before_extraction_without_readiness(self):
        self.write_recipe()
        make_zip(self.data / "headless-ui.apk", self.merged_entries())
        ui = Path(self.temporary.name) / "headless-ui"
        ui.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        ui.chmod(0o700)

        # Gate de QA (fail-closed preservado sob o knob).
        self.run_cli(
            expect=1,
            extra=["--require-ui", "--ui", str(ui)],
            env={"NXEXTRACT_REQUIRE_VISIBLE_UI": "1"},
        )
        self.assertFalse((self.game / "lib/arm64-v8a/libgame.so").exists())
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("exited before opening a visible renderer", log)

        # Default do usuario: instala HEADLESS e registra o motivo (P11.7).
        self.run_cli(extra=["--require-ui", "--ui", str(ui)])
        self.assertTrue((self.game / "lib/arm64-v8a/libgame.so").exists())
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("continuing HEADLESS", log)
        result = json.loads(
            (self.game / "nxextract-result.json").read_text(encoding="utf-8")
        )
        self.assertEqual(result["ui"]["mode"], "headless-fallback")
        self.assertIn("exited before opening", result["ui"]["fallback_reason"])

    def test_required_ui_rejects_readiness_from_exited_renderer(self):
        self.write_recipe()
        make_zip(self.data / "dead-ready-ui.apk", self.merged_entries())
        ui = Path(self.temporary.name) / "dead-ready-ui"
        write_channel_ui(ui, "publish()\n")

        self.run_cli(
            expect=1,
            extra=["--require-ui", "--ui", str(ui)],
            env={"NXEXTRACT_REQUIRE_VISIBLE_UI": "1"},
        )
        self.assertFalse((self.game / "lib/arm64-v8a/libgame.so").exists())
        log = (self.game / "test-extract.log").read_text(encoding="utf-8")
        self.assertIn("exited", log)

        # Default: a prova de um renderer ja morto continua NAO valendo como
        # visivel; a extracao completa headless com o motivo registrado.
        self.run_cli(extra=["--require-ui", "--ui", str(ui)])
        self.assertTrue((self.game / "lib/arm64-v8a/libgame.so").exists())
        result = json.loads(
            (self.game / "nxextract-result.json").read_text(encoding="utf-8")
        )
        self.assertEqual(result["ui"]["mode"], "headless-fallback")

    def test_required_ui_uses_40_second_fail_closed_boundary(self):
        # A fronteira continua 40s no contrato; aqui ela e' encolhida por
        # patch para provar timeout -> headless (default) e timeout -> erro
        # (gate de QA), com uma UI real que nunca publica prova.
        self.write_recipe()
        make_zip(self.data / "boundary-ui.apk", self.merged_entries())
        silent = Path(self.temporary.name) / "silent-ui"
        write_channel_ui(silent, "wait_stop()\n")
        late = Path(self.temporary.name) / "late-ui"
        write_channel_ui(late, "time.sleep(0.5)\npublish()\nwait_stop()\n")

        with mock.patch.object(NX.UISession, "READY_TIMEOUT_SECONDS", 2.0):
            # A espera cobre a negociacao lenta: prova aos 0.5s e' aceita.
            recipe = mock.Mock(title="Boundary Fixture", version="1")
            workspace = Path(self.temporary.name) / "late-workspace"
            workspace.mkdir()
            session = NX.UISession(
                str(late),
                True,
                str(ROOT),
                str(workspace),
                str(Path(self.temporary.name) / "late-progress"),
                recipe,
                NX.Logger(str(Path(self.temporary.name) / "late-log")),
            )
            try:
                self.assertTrue(session.start())
                self.assertEqual(session.renderer, "sdl")
            finally:
                session.stop()

        with mock.patch.object(NX.UISession, "READY_TIMEOUT_SECONDS", 0.4):
            recipe = mock.Mock(title="Boundary Fixture", version="1")
            workspace = Path(self.temporary.name) / "absent-workspace"
            workspace.mkdir()
            session = NX.UISession(
                str(silent),
                True,
                str(ROOT),
                str(workspace),
                str(Path(self.temporary.name) / "absent-progress"),
                recipe,
                NX.Logger(str(Path(self.temporary.name) / "absent-log")),
            )
            try:
                # P11.7: o estouro da fronteira cai para HEADLESS (a
                # instalacao segue) com o motivo registrado...
                self.assertFalse(session.start())
                self.assertIn(
                    "did not confirm a visible renderer",
                    session.headless_reason,
                )
            finally:
                session.stop()

            # ...e o gate de QA preserva o fail-closed historico sob o knob.
            workspace = Path(self.temporary.name) / "strict-workspace"
            workspace.mkdir()
            session = NX.UISession(
                str(silent),
                True,
                str(ROOT),
                str(workspace),
                str(Path(self.temporary.name) / "strict-progress"),
                recipe,
                NX.Logger(str(Path(self.temporary.name) / "strict-log")),
            )
            try:
                with mock.patch.dict(
                    NX.os.environ, {"NXEXTRACT_REQUIRE_VISIBLE_UI": "1"}
                ):
                    with self.assertRaisesRegex(
                        NX.NXError,
                        "did not confirm a visible renderer",
                    ):
                        session.start()
            finally:
                session.stop()
        self.assertEqual(NX.UISession.READY_TIMEOUT_SECONDS, 40.0)

    def test_log_budgets_are_enforced_not_just_declared(self):
        """The budgets in nx-budget-check were a number nothing applied.

        NXExtract runs at install time on the same card the payloads land on,
        so an unbounded log competes for space with the extraction itself.
        """
        # The ceilings are pinned to the published budget: drift here would
        # make the enforcement quietly disagree with the document.
        self.assertEqual(NX.COMPACT_LOG_CEILING, 2 * 1024 * 1024)
        self.assertEqual(NX.DETAIL_LOG_CEILING, 8 * 1024 * 1024)
        base = pathlib.Path(self.game) / "budget"
        base.mkdir(parents=True, exist_ok=True)
        compact = base / "nxextract.log"
        detail = base / "nxextract-detail.log"
        old = (NX.COMPACT_LOG_CEILING, NX.DETAIL_LOG_CEILING)
        NX.COMPACT_LOG_CEILING, NX.DETAIL_LOG_CEILING = 4096, 8192
        try:
            logger = NX.Logger(path=str(compact), detail_path=str(detail),
                               verbose=False)
            try:
                logger.log("FIRST-LINE-MARKER")
                for index in range(400):
                    logger.log("pad %04d %s" % (index, "y" * 80))
                logger.log("LAST-LINE-MARKER")
            finally:
                logger.close()
            compact_text = compact.read_text(encoding="utf-8")
            detail_text = detail.read_text(encoding="utf-8")
            prev = detail.with_name(detail.name + ".prev")
        finally:
            NX.COMPACT_LOG_CEILING, NX.DETAIL_LOG_CEILING = old
        # Compact: capped, and it SAYS it was capped -- otherwise a cap reads
        # exactly like a run that died.
        self.assertLessEqual(len(compact_text.encode("utf-8")), 4096 + 512)
        self.assertIn("FIRST-LINE-MARKER", compact_text)
        self.assertIn("reached its 4096 byte budget", compact_text)
        self.assertNotIn("LAST-LINE-MARKER", compact_text)
        # Detail: rotates once, so the most recent window survives whole and
        # exactly one previous generation is kept.
        self.assertTrue(prev.is_file(), "the detail log never rotated")
        self.assertLessEqual(len(detail_text.encode("utf-8")), 8192 + 512)
        self.assertIn("LAST-LINE-MARKER", detail_text)
        self.assertFalse(detail.with_name(detail.name + ".prev.prev").exists())
        for line in detail_text.splitlines() + prev.read_text(
                encoding="utf-8").splitlines():
            self.assertRegex(line, r"^\[\d{4}-\d{2}-\d{2} ")
        # Writer against reader: the support bundle parses this exact file.
        # The cap notice is a new line shape, and a reader that chokes on it
        # would break diagnosis on precisely the long installs that hit the
        # cap.
        support_path = (pathlib.Path(__file__).resolve().parents[3]
                        / "framework/nxobs/nx-support-bundle.py")
        if support_path.is_file():
            spec = importlib.util.spec_from_file_location(
                "nxobs_support_from_nxextract", support_path)
            support = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(support)
            events = []
            support.parse_extractor(compact_text.splitlines(), events, "r1")
            # The notice must not be mistaken for a failure: nothing failed.
            self.assertTrue(
                all(event["reason_code"] != 1499 for event in events),
                "the compact-log cap notice was read as an extractor failure",
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
