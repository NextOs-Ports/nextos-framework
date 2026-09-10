#!/usr/bin/env python3
"""Small, proprietary-data-free tests for FP2's NXExtract preparation hook."""

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest


NXEXTRACT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(NXEXTRACT / "vendor" / "python"))
sys.path.insert(0, str(NXEXTRACT))
os.environ["PF2_LZ4_LIBRARY"] = str(
    NXEXTRACT / "lib" / "aarch64" / "liblz4.so.1"
)

import exact_gles2 as exact  # noqa: E402
import prepare_fp2_data as prep  # noqa: E402


def digest(payload):
    return hashlib.sha256(payload).hexdigest()


class ApprovedSkipForTest(RuntimeError):
    pass


class PreparationPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.policy_path = NXEXTRACT / "fp2-preparation-policy.json"
        cls.policy = prep.load_policy(str(cls.policy_path))

    def test_policy_is_semantic_and_complete(self):
        policy = self.policy
        self.assertEqual(policy["package"], "com.GalaxyTrail.FP2")
        self.assertEqual(policy["format"], 2)
        self.assertNotIn("game_version", policy)
        self.assertNotIn("input", policy)
        self.assertEqual(len(policy["logical_outputs"]), 31)
        decisions = policy["task_decisions"]
        self.assertEqual(decisions["contract"], prep.TASK_ORDER_CONTRACT)
        self.assertEqual(decisions["repeat"], prep.TASK_REPEAT_CONTRACT)
        self.assertEqual(decisions["translate_token"], "T")
        self.assertEqual(decisions["omit_token"], "O")
        sequence = decisions["sequence"]
        self.assertEqual(len(sequence), 702)
        self.assertEqual(sequence.count("T"), 411)
        self.assertEqual(sequence.count("O"), 291)

        output_paths = [item["path"] for item in policy["logical_outputs"]]
        self.assertEqual(output_paths, sorted(output_paths))
        for item in policy["logical_outputs"]:
            self.assertEqual(set(item), {"path"})

        serialized = self.policy_path.read_text(encoding="utf-8")
        self.assertIsNone(re.search(r"\b[0-9a-f]{64}\b", serialized))
        for forbidden in ("/home/", "/mnt/", "apk_sha256", "container_sha256",
                          "input_sha256", "output_sha256", "game_version",
                          "APKPure", "APKMirror", "APKVision", "5play"):
            self.assertNotIn(forbidden, serialized)

    def test_package_and_recipe_contracts_cover_every_runtime_path(self):
        port_root = NXEXTRACT.parent
        recipe = json.loads(
            (port_root / "extractor.json").read_text(encoding="utf-8")
        )
        self.assertEqual(recipe["commit"], ["assets", "lib"])
        for rule in recipe["extract"]:
            destination = rule["destination"].replace("{abi}", "arm64-v8a")
            self.assertTrue(
                any(
                    destination == root or destination.startswith(root + "/")
                    for root in recipe["commit"]
                ),
                "%s is outside the transactional roots" % destination,
            )

        extraction = json.loads(
            (port_root / "RELEASE-METADATA.json").read_text(encoding="utf-8")
        )["extraction"]
        self.assertEqual(extraction["expected_payload_files"], 1150)
        self.assertEqual(extraction["expected_payload_bytes"], 974956342)
        self.assertEqual(
            extraction["final_content_manifest_sha256"],
            "c36c7a891eca75a69a312efc1f42c05ffa37b35a386a8004660cc6e2025f47c5",
        )

        project_nxport = json.loads(
            (port_root / "nxproject.json").read_text(encoding="utf-8")
        )["nxport"]
        standalone_nxport = json.loads(
            (port_root / "nxport.json").read_text(encoding="utf-8")
        )
        for manifest in (project_nxport, standalone_nxport):
            self.assertNotIn("execution_roles", manifest)
            self.assertIn(
                "nxextract/bin/fp2-smolv-cross-nextos",
                manifest["required_files"],
            )

    def test_nxextract_authenticated_paths_are_still_safety_checked(self):
        with tempfile.TemporaryDirectory() as root:
            stage = Path(root) / "stage"
            data = stage / "assets" / "bin" / "Data"
            data.mkdir(parents=True)
            (data / "globalgamemanagers").write_bytes(b"authenticated")
            libraries = stage / "lib"
            libraries.mkdir()
            for name in ("libil2cpp.so", "libmain.so", "libunity.so"):
                (libraries / name).write_bytes(b"authenticated")
            policy = {
                "logical_outputs": [
                    {"path": "assets/bin/Data/globalgamemanagers"},
                ],
            }
            prep.validate_owner_input(str(stage), policy)
            (libraries / "unexpected").write_bytes(b"unsafe extra entry")
            with self.assertRaises(prep.PreparationError):
                prep.validate_owner_input(str(stage), policy)

    def test_tree_and_stage_paths_reject_symlinks_and_escape(self):
        with tempfile.TemporaryDirectory() as root:
            assets = Path(root) / "assets"
            assets.mkdir()
            outside = Path(root) / "outside"
            outside.write_bytes(b"outside")
            (assets / "unsafe").symlink_to(outside)
            with self.assertRaises(prep.PreparationError):
                prep.validate_logical_asset(str(assets / "unsafe"))
            with self.assertRaises(prep.PreparationError):
                prep.stage_path(root, "../outside")

    def test_task_decision_order_is_unique_repeatable_and_fail_closed(self):
        accepted_source = b"accepted smolv bytes"
        skipped_source = b"skipped smolv bytes"
        metadata = "shader\tvertex\tfragment"
        with tempfile.TemporaryDirectory() as work:
            translator = prep.PolicyTranslator(
                "/not-invoked", work, {"sequence": "TO"},
                ApprovedSkipForTest,
            )
            accepted = translator.key(accepted_source, metadata)
            self.assertEqual(accepted, "task-0001")
            self.assertEqual(translator.key(accepted_source, metadata), accepted)
            skipped_key = translator.key(skipped_source, metadata)
            self.assertEqual(skipped_key, "task-0002")
            with self.assertRaises(ApprovedSkipForTest):
                translator.translate({"key": skipped_key})
            self.assertEqual(os.listdir(work), [])
            with self.assertRaises(prep.PreparationError):
                translator.key(b"unknown", metadata)

    def test_stencil_correction_is_name_and_structure_scoped(self):
        fragment = """#version 100
void main()
{
    vec4 sample = texture2D(_MainTex, vs_TEXCOORD1);
    sample.x = texture2D(_AlphaTex, vs_TEXCOORD1).x;
    vec4 color = sample.yzwx * vs_TEXCOORD0;
    gl_FragData[0] = color;
}
"""
        corrected = exact.correct_backend_fragment(
            "Sprites/StencilDraw", fragment
        )
        self.assertEqual(corrected, exact.STENCIL_SPRITE_FRAGMENT_GLES2)
        self.assertEqual(
            exact.correct_backend_fragment("Other/Shader", fragment), fragment
        )
        with self.assertRaises(ValueError):
            exact.correct_backend_fragment(
                "Sprites/StencilInvert", fragment.replace(".yzwx", ".xyzw")
            )

        injector = exact.ExactInjector(None)
        injector.validate_correction(
            "Sprites/StencilDraw",
            "#version 100\nvoid main() {}\n",
            corrected,
        )
        with self.assertRaises(ValueError):
            injector.validate_correction(
                "Sprites/StencilDraw", "void main() {}\n", corrected
            )

    def test_tool_paths_modes_and_port_boundary_are_enforced(self):
        with tempfile.TemporaryDirectory() as root:
            game = Path(root) / "game"
            translator = game / prep.TRANSLATOR_RELATIVE
            lz4 = game / prep.LZ4_RELATIVE
            translator.parent.mkdir(parents=True)
            lz4.parent.mkdir(parents=True)
            translator.write_bytes(b"test translator")
            lz4.write_bytes(b"test lz4")
            translator.chmod(stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
            policy = {
                "translator": {"path": prep.TRANSLATOR_RELATIVE},
                "lz4": {"path": prep.LZ4_RELATIVE},
            }
            resolved = prep.validate_tools(str(game), policy)
            self.assertEqual(resolved[0], str(translator.resolve()))
            self.assertEqual(resolved[1], str(lz4.resolve()))

            # Bytes are pinned by the generation manifest, not duplicated here.
            translator.write_bytes(b"mutated translator")
            prep.validate_tools(str(game), policy)
            translator.chmod(stat.S_IRUSR | stat.S_IWUSR)
            with self.assertRaises(prep.PreparationError):
                prep.validate_tools(str(game), policy)

            policy["translator"]["path"] = "nxextract/bin/other"
            with self.assertRaises(prep.PreparationError):
                prep.validate_tools(str(game), policy)

    def test_stale_python_bytecode_fails_closed(self):
        with tempfile.TemporaryDirectory() as root:
            game = Path(root) / "game"
            nxextract = game / "nxextract"
            nxextract.mkdir(parents=True)
            prep.reject_stale_python_cache(str(game))

            cache = nxextract / "__pycache__"
            cache.mkdir()
            with self.assertRaises(prep.PreparationError):
                prep.reject_stale_python_cache(str(game))
            cache.rmdir()

            bytecode = nxextract / "stale.pyc"
            bytecode.write_bytes(b"stale")
            with self.assertRaises(prep.PreparationError):
                prep.reject_stale_python_cache(str(game))

    def test_runtime_has_no_on_device_build_or_download_path(self):
        runtime = "\n".join(
            (NXEXTRACT / name).read_text(encoding="utf-8")
            for name in ("prepare_fp2_data.py", "exact_gles2.py",
                         "shader_core.py", "serialized_patch.py")
        )
        for forbidden in ("glslang", "pip install", "os.system(",
                          "subprocess.Popen("):
            self.assertNotIn(forbidden, runtime)
        for forbidden_option in ("--policy", "--translator", "--translator-sha256",
                                 "--lz4-library", "--lz4-sha256"):
            self.assertNotIn("add_argument(\"%s\"" % forbidden_option, runtime)

    def test_provenance_pins_the_shipped_native_artifacts(self):
        provenance = json.loads(
            (NXEXTRACT / "specs" / "toolchain-provenance.json").read_text(
                encoding="utf-8"
            )
        )
        for section in ("translator", "lz4"):
            artifact = provenance[section]["artifact"]
            relative = artifact["path"].split("nxextract/", 1)[1]
            path = NXEXTRACT / relative
            self.assertTrue(path.is_file())
            self.assertFalse(path.is_symlink())
            self.assertEqual(path.stat().st_size, artifact["size"])
            self.assertEqual(digest(path.read_bytes()), artifact["sha256"])
            self.assertEqual("%04o" % stat.S_IMODE(path.stat().st_mode),
                             artifact["mode"])

        readelf = (shutil.which("aarch64-linux-gnu-readelf") or
                   shutil.which("readelf"))
        if readelf is None:
            self.skipTest("readelf is unavailable")
        for section in ("translator", "lz4"):
            artifact = provenance[section]["artifact"]
            relative = artifact["path"].split("nxextract/", 1)[1]
            output = subprocess.run(
                [readelf, "-d", str(NXEXTRACT / relative)],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            ).stdout
            needed = re.findall(
                r"\(NEEDED\).*Shared library: \[([^]]+)\]", output
            )
            self.assertEqual(needed, artifact["needed"])

        source_inputs = provenance["translator"]["source_inputs"]
        self.assertEqual(
            source_inputs["spirv_cross"]["revision"],
            "eb32b288ea553e938005fcfd819a2290b1c8032d",
        )
        self.assertEqual(
            source_inputs["spirv_tools"]["revision"],
            "9a49b0883b9b635689a85b5647dbfcb223268151",
        )
        self.assertEqual(
            source_inputs["spirv_headers"]["revision"],
            "29981f65241605e08b0ede4cfeb999fe3b723c6a",
        )


if __name__ == "__main__":
    unittest.main()
