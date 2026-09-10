from __future__ import annotations

import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


PORT_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(PORT_DIR / "tools"))

import sallyface_oversize_fix as oversize  # noqa: E402
import sallyface_stage as stage  # noqa: E402
import shader_gles2_patch as shader_patch  # noqa: E402
sys.path.insert(0, str(PORT_DIR / "nxextract"))
import sf_prepare  # noqa: E402
import nxextract as nxextract_engine  # noqa: E402


class OversizeToolTests(unittest.TestCase):
    def test_contract_is_exactly_the_two_mali_limit_textures(self) -> None:
        self.assertEqual(
            {
                item.name: (
                    item.source_width,
                    item.source_height,
                    item.output_width,
                    item.output_height,
                )
                for item in oversize.TEXTURES
            },
            {
                "os3_BG2": (6018, 833, 3009, 416),
                "os3_BG": (6252, 1607, 3126, 803),
            },
        )
        self.assertEqual(oversize.OUTPUT_ASSETS_SIZE, 15_188_296)
        self.assertEqual(
            oversize.OUTPUT_ASSETS_SHA256,
            "22145289e107499203e3bbd4ea8b1de2787acb2733f45bfbe25b8ca7b2c5de90",
        )

    def test_canonical_json_is_stable(self) -> None:
        first = oversize.canonical_json({"z": 1, "a": [2, 3]})
        second = oversize.canonical_json({"a": [2, 3], "z": 1})
        self.assertEqual(first, second)
        self.assertEqual(json.loads(first), {"a": [2, 3], "z": 1})

    def test_atomic_output_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "result.bin"
            self.assertTrue(oversize._atomic_write_if_changed(output, b"fixed"))
            self.assertFalse(oversize._atomic_write_if_changed(output, b"fixed"))
            self.assertEqual(output.read_bytes(), b"fixed")

    def test_wrong_source_fails_before_unity_parse(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets = root / oversize.SOURCE_ASSETS_NAME
            ress = root / oversize.SOURCE_RESS_NAME
            assets.write_bytes(b"wrong")
            ress.write_bytes(b"wrong")
            with self.assertRaises(oversize.ContractError):
                oversize.inspect_source(assets, ress)

    def test_source_and_output_must_be_distinct(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "same"
            source.write_bytes(b"x")
            with self.assertRaises(oversize.ContractError):
                oversize._require_distinct(source, source)


class StageToolTests(unittest.TestCase):
    def test_bundle_entry_path_gate(self) -> None:
        self.assertEqual(stage._safe_entry_name("sharedassets1.assets"), Path("sharedassets1.assets"))
        for unsafe in ("../escape", "/absolute", "./alias", "a\\b"):
            with self.subTest(unsafe=unsafe):
                with self.assertRaises(stage.StageError):
                    stage._safe_entry_name(unsafe)

    def test_manifest_write_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            value = {"schema": stage.SCHEMA, "counts": {"textures": 1002}}
            stage._atomic_manifest(path, value)
            first = path.read_bytes()
            stage._atomic_manifest(path, value)
            self.assertEqual(path.read_bytes(), first)

    def test_closed_stage_counts(self) -> None:
        self.assertEqual(stage.EXPECTED_BUNDLE_ENTRIES, 613)
        self.assertEqual(stage.EXPECTED_SERIALIZED_FILES, 227)
        self.assertEqual(stage.EXPECTED_SCENES, 227)
        self.assertEqual(stage.EXPECTED_TEXTURES, 1002)
        self.assertEqual(stage.EXPECTED_SCENE_SHADERS, 19)
        self.assertEqual(len(stage.SCENE_SHADER_CONTRACT), 9)
        self.assertEqual(
            stage.SCENE_SHADER_CONTRACT["sharedassets4.assets"],
            ((14, "Legacy Shaders/Particles/Alpha Blended"),),
        )
        self.assertEqual(stage.EXPECTED_FORMATS, {3: 112, 4: 311, 45: 76, 47: 503})
        self.assertEqual(stage.PATCHED_DATA_BYTES, 23_887_846)
        self.assertEqual(
            stage.PATCHED_DATA_SHA256,
            "154199377b342d29a2f0587eea7bc60437494d7a4d8e72912cdf6d626c359664",
        )

    def test_shader_source_to_output_rejects_same_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "data.unity3d"
            source.write_bytes(b"UnityFS")
            with self.assertRaises(ValueError):
                shader_patch.patch_source_to_output(source, source)

    def test_scene_shader_source_to_output_rejects_same_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "sharedassets4.assets"
            source.write_bytes(b"not-a-serialized-file")
            with self.assertRaises(ValueError):
                shader_patch.patch_serialized_source_to_output(source, source)

    def test_shader_blob_program_type_is_changed_without_resizing(self) -> None:
        stamp = shader_patch.VERSION_STAMPS[0]
        source = b"prefix" + struct.pack("<ii", stamp, shader_patch.TYPE_GLES3) + b"suffix"
        result, hits = shader_patch.patch_segment(source)
        self.assertEqual(hits, 1)
        self.assertEqual(len(result), len(source))
        self.assertEqual(
            result,
            b"prefix" + struct.pack("<ii", stamp, shader_patch.TYPE_GLES) + b"suffix",
        )

    def test_global_unity_settings_are_converted_to_gles2(self) -> None:
        class FakeType:
            def __init__(self, name: str) -> None:
                self.name = name

        class FakeObject:
            def __init__(self, name: str, tree: dict) -> None:
                self.type = FakeType(name)
                self.tree = tree
                self.saved = None

            def read_typetree(self) -> dict:
                return dict(self.tree)

            def save_typetree(self, tree: dict) -> None:
                self.saved = tree

        counters = shader_patch.new_counters()
        player = FakeObject("PlayerSettings", {"playerMinOpenGLESVersion": 2})
        build = FakeObject("BuildSettings", {"m_GraphicsAPIs": [21, 11]})

        self.assertTrue(shader_patch.patch_object(player, counters, "fixture"))
        self.assertTrue(shader_patch.patch_object(build, counters, "fixture"))
        self.assertEqual(player.saved["playerMinOpenGLESVersion"], 1)
        self.assertEqual(build.saved["m_GraphicsAPIs"], [8])
        self.assertEqual(counters["minimum_gles"], 1)
        self.assertEqual(counters["graphics_apis"], 1)


class RuntimePolicyTests(unittest.TestCase):
    def test_nxextract_uses_internal_profiles_not_outer_apk_identity(self) -> None:
        recipe = json.loads(
            (PORT_DIR / "extractor" / "sallyface-nextos.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(recipe["version"], "arm64-4")
        self.assertEqual(
            recipe["compatibility"]["package_families"],
            ["com.portablemoose.sallyface"],
        )
        selectors = {
            item["member"]
            for item in recipe["compatibility"]["required_members"]
            if isinstance(item, dict) and item.get("role") == "patch_selector"
        }
        self.assertEqual(
            selectors,
            {
                "assets/bin/Data/data.unity3d",
                "assets/bin/Data/datapack.unity3d",
            },
        )
        self.assertEqual(len(recipe["patch_profiles"]), 4)
        self.assertEqual(
            {item["fallback"] for item in recipe["patch_profiles"]},
            {"generic-pass-through"},
        )
        outer = recipe["reference_build"]["container_sha256"]
        decision = json.dumps(
            {
                "input": recipe["input"],
                "compatibility": recipe["compatibility"],
                "patch_profiles": recipe["patch_profiles"],
                "extract": recipe["extract"],
            },
            sort_keys=True,
        )
        self.assertNotIn(outer, decision)
        self.assertNotIn("filename", decision.casefold())
        self.assertNotIn("versioncode", decision.casefold())

    def test_arm64_4_receipt_forces_old_payload_reprocessing(self) -> None:
        recipe = json.loads(
            (PORT_DIR / "extractor" / "sallyface-nextos.json").read_text(
                encoding="utf-8"
            )
        )
        receipt = "assets/.sallyface-prepare-arm64-4.json"
        self.assertIn(
            ".sallyface-prepare-arm64-4.json",
            recipe["extract"][3]["output_validate"]["required_paths"],
        )
        checks = {item["path"]: item for item in recipe["validate"]}
        self.assertEqual(len(checks[receipt]["sha256"]), 3)
        self.assertEqual(
            recipe["hooks"][0]["checkpoint"][0]["path"], receipt
        )
        self.assertEqual(
            recipe["hooks"][0]["checkpoint"][0]["sha256"],
            checks[receipt]["sha256"],
        )

        with tempfile.TemporaryDirectory() as directory:
            stage_root = Path(directory)
            (stage_root / "assets").mkdir()
            with self.assertRaises(nxextract_engine.ValidationError):
                nxextract_engine.validate_output_path(
                    stage_root / receipt,
                    checks[receipt],
                    full=True,
                )
            for profile in ("sf-assets-a", "sf-assets-b", None):
                path = Path(sf_prepare.write_prepare_receipt(
                    str(stage_root), profile
                ))
                self.assertEqual(path.name, sf_prepare.PREPARE_RECEIPT)
                value = json.loads(path.read_text(encoding="ascii"))
                self.assertEqual(value["recipe_version"], "arm64-4")
                self.assertEqual(
                    value["profile"], profile or "generic-pass-through"
                )
                self.assertIn(
                    hashlib.sha256(path.read_bytes()).hexdigest(),
                    checks[receipt]["sha256"],
                )
                nxextract_engine.validate_output_path(
                    path,
                    checks[receipt],
                    full=True,
                )

    def test_retag_profiles_are_output_verified_and_not_blind(self) -> None:
        spec = json.loads(
            (PORT_DIR / "nxextract" / "sf_retag_spec.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(spec["schema_version"], 2)
        self.assertEqual(
            [item["id"] for item in spec["profiles"]],
            ["sf-assets-a", "sf-assets-b"],
        )
        serialized = json.dumps(spec, sort_keys=True)
        self.assertNotIn("sha_in", serialized)
        self.assertNotIn("blob_sha_in", serialized)
        for profile in spec["profiles"]:
            self.assertEqual(len(profile["loose_assets"]), 9)
            self.assertTrue(profile["data_bundle"]["bundle_sha_out"])
            nodes = {
                entry["name"]: entry
                for entry in profile["data_bundle"]["nodes"]
            }
            self.assertEqual(
                set(nodes),
                {"globalgamemanagers", "Resources/unity_builtin_extra"},
            )
            self.assertEqual(len(nodes["globalgamemanagers"]["ops"]), 5)
            self.assertEqual(len(nodes["globalgamemanagers"]["sha_out"]), 64)
            for entry in profile["loose_assets"]:
                self.assertTrue(entry["ops"])
                self.assertEqual(len(entry["sha_out"]), 64)

    def test_generation_v2_declares_complete_nxextract_closure(self) -> None:
        project = json.loads(
            (PORT_DIR / "nxproject.json").read_text(encoding="utf-8")
        )
        nxport = project["nxport"]
        self.assertEqual(nxport["schema_version"], 3)
        self.assertEqual(project["runtime_root"], ".")
        runtime = nxport["generation_runtime"]
        declared = {item["path"] for item in runtime}
        expected = {
            "extractor.json",
            "nxextract/nxextract.py",
            "nxextract/run-extractor.sh",
            "nxextract/nxextract-runtime-env.sh",
            "nxextract/nxextract-ui",
            "nxextract/sf_prepare.py",
            "nxextract/sf_retag.py",
            "nxextract/sf_unbundle.py",
            "nxextract/sf_retag_spec.json",
        }
        actual = {
            "extractor.json",
            *{
                path.relative_to(PORT_DIR).as_posix()
                for path in (PORT_DIR / "nxextract").iterdir()
                if path.is_file() and path.name != "__pycache__"
            },
        }
        self.assertEqual(actual, expected)
        self.assertTrue(expected <= declared)
        self.assertIn("sallyface-nextos", declared)
        self.assertIn("port-env.sh", declared)
        self.assertIn("nxsplash-nextos", declared)

    def test_default_settings_declares_all_quality_profiles(self) -> None:
        settings = (PORT_DIR / "defaults" / "NEXTOSSETTINGS.txt").read_text(
            encoding="utf-8"
        )
        self.assertIn("quality: auto|low|medium|high", settings)
        self.assertIn("quality=auto", settings)

        build = (PORT_DIR / "package" / "build-package.sh").read_text(
            encoding="utf-8"
        )
        finalizer = PORT_DIR / "package" / "finalize-owner-settings.py"
        self.assertIn("finalize-owner-settings.py", build)
        self.assertTrue(finalizer.is_file())
        self.assertIn("receipt[\"artifacts\"] = after", finalizer.read_text(
            encoding="utf-8"
        ))

    def test_fullscreen_contract_defaults_to_fill_with_native_rollback(self) -> None:
        # O launcher e' gerado pelo nxbootstrap e carrega port-env.sh; a
        # politica de aspecto do port mora la'.
        launcher = (PORT_DIR / "port-env.sh").read_text(encoding="utf-8")
        self.assertIn('${SF_ASPECT:-fill}', launcher)
        self.assertIn('fill) SF_ASPECT=fill', launcher)
        self.assertIn('native) SF_ASPECT=native', launcher)

        egl = (PORT_DIR / "src" / "egl_sdl.c").read_text(encoding="utf-8")
        measured = egl.index("SDL_GL_GetDrawableSize")
        published = egl.index("sf_window_configure", measured)
        receipt = egl.index('DISPLAY-RECEIPT', published)
        self.assertLess(measured, published)
        self.assertLess(published, receipt)
        self.assertIn("surface->width = is_window ? screen_width : 16;", egl)
        self.assertIn("surface->height = is_window ? screen_height : 16;", egl)
        self.assertNotIn("glViewport", egl)

    def test_fullscreen_contract_reaches_android_and_jni_channels(self) -> None:
        android = (PORT_DIR / "src" / "android.c").read_text(encoding="utf-8")
        self.assertIn("drawable_contract_locked", android)
        self.assertIn("ANativeWindow_setBuffersGeometry requested=", android)

        jni = (PORT_DIR / "src" / "jni.c").read_text(encoding="utf-8")
        for channel in (
            '"getResources"',
            '"getDisplayMetrics"',
            '"getWindowManager"',
            '"getDefaultDisplay"',
            '"widthPixels"',
            '"heightPixels"',
        ):
            self.assertIn(channel, jni)
        self.assertIn("sf_window_get_size", jni)

    def test_es_mapping_honors_inverted_right_stick_axis(self) -> None:
        fixture = """\
<inputConfig type="joystick" deviceName="Xbox Position Pad" deviceGUID="03000000000000000000000000000000">
  <input name="b" type="button" id="0" value="1" />
  <input name="start" type="button" id="11" value="1" />
  <input name="leftanalogleft" type="axis" id="0" value="-1" />
  <input name="leftanalogup" type="axis" id="1" value="-1" />
  <input name="rightanalogleft" type="axis" id="2" value="1" />
  <input name="rightanalogup" type="axis" id="3" value="-1" />
</inputConfig>
"""
        result = subprocess.run(
            ["awk", "-f", str(PORT_DIR / "es2sdl.awk")],
            input=fixture,
            text=True,
            check=True,
            capture_output=True,
        ).stdout.strip()
        self.assertIn(",a:b0,", result)
        self.assertIn(",leftx:a0,", result)
        self.assertIn(",lefty:a1,", result)
        self.assertIn(",rightx:a2~,", result)
        self.assertIn(",righty:a3,", result)

    def test_launcher_leaves_quality_to_canonical_adapter(self) -> None:
        launcher = (PORT_DIR / "port-env.sh").read_text(encoding="utf-8")
        self.assertNotIn("export SF_TEX_HALF_MIN=", launcher)
        self.assertNotIn("SF_SWAP_CEILING_MB", launcher)
        self.assertNotIn("SF_AVAIL_FLOOR_MB", launcher)
        self.assertIn("export SF_ETC1_CACHE_ABI=$sf_abi", launcher)

    def test_kmsdrm_provider_recovery_uses_a_coherent_pair(self) -> None:
        launcher = (PORT_DIR / "port-env.sh").read_text(encoding="utf-8")
        self.assertIn('[ "${CFW_NAME:-}" = dArkOSRE ]', launcher)
        self.assertIn('[ -z "${SDL_VIDEO_EGL_DRIVER+x}" ]', launcher)
        self.assertIn('[ -z "${SDL_VIDEO_GL_DRIVER+x}" ]', launcher)
        self.assertIn("export SDL_VIDEO_EGL_DRIVER=libEGL.so", launcher)
        self.assertIn("export SDL_VIDEO_GL_DRIVER=libGLESv2.so", launcher)
        self.assertIn("/usr/lib/aarch64-linux-gnu", launcher)

    def test_native_default_is_no_runtime_half_resolution(self) -> None:
        egl = (PORT_DIR / "src" / "egl.c").read_text(encoding="utf-8")
        self.assertIn("tex_half_min = v && *v ? atoi(v) : 0;", egl)
        self.assertIn('getenv("SF_ETC1_CACHE_ABI")', egl)
        self.assertIn('profile_half ? "perfil-low" : "clamp-hw"', egl)

    def test_memory_guard_is_observer_only(self) -> None:
        guard = (PORT_DIR / "src" / "mem_guard.c").read_text(encoding="utf-8")
        self.assertNotIn("_exit(70)", guard)
        self.assertNotIn("sf_input_request_exit()", guard)
        self.assertIn("somente telemetria", guard)

    def test_shader_platform_table_crash_patch_stays_forbidden(self) -> None:
        for name in ("src/main.c", "src/egl.c", "port-env.sh"):
            text = (PORT_DIR / name).read_text(encoding="utf-8")
            self.assertNotIn("platform_table_5_to_9", text, name)

    def test_xbox_pause_uses_start_without_digital_r2(self) -> None:
        source = (PORT_DIR / "src" / "input.c").read_text(encoding="utf-8")
        self.assertIn("[SDL_CONTROLLER_BUTTON_START] = 105", source)
        self.assertNotIn("[SDL_CONTROLLER_BUTTON_START] = 108", source)
        self.assertNotIn("const int trig_key", source)

    def test_documentation_does_not_claim_rgba4444(self) -> None:
        documents = sorted(PORT_DIR.glob("*.md"))
        self.assertTrue(documents, "no markdown documentation found")
        for document in documents:
            text = document.read_text(encoding="utf-8")
            self.assertNotIn("RGBA4444", text, document.name)


if __name__ == "__main__":
    unittest.main()
